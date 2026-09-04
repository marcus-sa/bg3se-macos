/**
 * BG3SE-macOS - User Variables System Implementation
 *
 * Entity-attached custom variables with persistence.
 * Storage lives in vars_persist.c: one JSON store per savegame under
 * ~/Library/Application Support/BG3SE/SaveVars/. This file owns the in-memory
 * variable set and the payload it serializes to.
 */

#include "user_variables.h"
#include "../lua/lua_json.h"
#include "../core/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include <lauxlib.h>
#include <lualib.h>

// ============================================================================
// Static State
// ============================================================================

// User variables (entity-attached)
static UserVariablePrototype g_Prototypes[UVAR_MAX_PROTOTYPES];
static int g_PrototypeCount = 0;

static EntityVariables g_Entities[UVAR_MAX_ENTITIES];
static int g_EntityCount = 0;

// Mod variables (global per-mod)
static ModVariables g_Mods[UVAR_MAX_MODS];
static int g_ModCount = 0;
static bool g_ModsDirty = false;

static bool g_Initialized = false;
static bool g_Dirty = false;

// ============================================================================
// Helper: Free a UserVariable's allocated memory
// ============================================================================

static void free_variable(UserVariable *var) {
    if (var && (var->type == UVAR_TYPE_STRING || var->type == UVAR_TYPE_TABLE)) {
        if (var->value.string) {
            free(var->value.string);
            var->value.string = NULL;
        }
    }
    if (var) {
        var->type = UVAR_TYPE_NULL;
        var->dirty = false;
    }
}

// ============================================================================
// Helper: Identity-stable cache for table-valued variables
// ============================================================================
//
// Table variables are stored as JSON. Without a cache, every read re-parsed
// that JSON into a BRAND NEW Lua table, so the mod-facing semantics silently
// diverged from Windows BG3SE: a nested write went into a throwaway table and
// vanished.
//
//     Vars.SittingOut[combat] = Vars.SittingOut[combat] or {}   -- lost
//     if Vars.SittingOut[combat][uuid] then                     -- indexes nil
//
// That is exactly how "Sit This One Out 2" fails on macOS: DoSitOut() aborts
// at SitOut.lua:596 before it reaches Osi.ApplyStatus(..., SITOUT_ONCOMBATSTART
// _DOONCE_TECHNICAL), so companions never actually sit out. FindEnemies (:503)
// and OnLeftCombat (:668) die the same way.
//
// Windows BG3SE hands back the same deserialized table every read and only
// reserializes when it syncs/persists. Mirror that with a strong-valued cache
// table in the Lua registry.
//
// The cache is PER LUA STATE (registry-scoped): server and client run separate
// states with separate registries, so a table cached in one is meaningless in
// the other. Cross-state coherence still goes through the JSON, exactly as
// before — this changes nothing about sync, only about object identity within
// one state.
//
// The stored JSON goes stale the moment a mod mutates the cached table in
// place. That is fine because every reader of value.string for a table
// (uvar_get / mvar_get, and therefore the save paths, which serialize what
// those return) consults the cache first.

#define VARCACHE_REGISTRY_KEY "BG3SE_VarTableCache"

/* "<scope>|<id>|<key>" — scope 'e' for entity vars, 'm' for mod vars. */
static void varcache_key(char *out, size_t n, char scope,
                         const char *id, const char *key) {
    snprintf(out, n, "%c|%s|%s", scope, id ? id : "", key ? key : "");
}

/* Push the cache table for this state, creating it on first use. */
static void varcache_push_table(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, VARCACHE_REGISTRY_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, VARCACHE_REGISTRY_KEY);
    }
}

/* Push the cached table for `ck` and return 1, or push nothing and return 0. */
static int varcache_push(lua_State *L, const char *ck) {
    varcache_push_table(L);
    lua_getfield(L, -1, ck);
    if (lua_istable(L, -1)) {
        lua_remove(L, -2);      /* drop the cache table, keep the value */
        return 1;
    }
    lua_pop(L, 2);
    return 0;
}

/* Cache the value at `value_index` under `ck` (tables only; anything else
 * clears the entry so a later read falls through to the stored scalar). */
static void varcache_store(lua_State *L, const char *ck, int value_index) {
    if (value_index < 0) value_index = lua_gettop(L) + value_index + 1;
    varcache_push_table(L);
    if (lua_type(L, value_index) == LUA_TTABLE) {
        lua_pushvalue(L, value_index);
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, ck);
    lua_pop(L, 1);
}

static void varcache_remove(lua_State *L, const char *ck) {
    varcache_push_table(L);
    lua_pushnil(L);
    lua_setfield(L, -2, ck);
    lua_pop(L, 1);
}

/* Drop every cached table for this state.
 *
 * Loading a savegame replaces the whole variable set. mvar_get/uvar_get answer
 * from this cache BEFORE they ever look at value.string, so a table cached
 * while the previous savegame was loaded would shadow the restored value and
 * the mod would keep reading (and mutating) the outgoing campaign's data. */
static void varcache_reset(lua_State *L) {
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, VARCACHE_REGISTRY_KEY);
}

void uvar_cache_invalidate(lua_State *L) {
    if (L) varcache_reset(L);
}

// ============================================================================
// Helper: Find entity by GUID
// ============================================================================

static int find_entity_index(const char *guid) {
    for (int i = 0; i < g_EntityCount; i++) {
        if (strcmp(g_Entities[i].guid, guid) == 0) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Helper: Find mod by UUID
// ============================================================================

static int find_mod_index(const char *uuid) {
    for (int i = 0; i < g_ModCount; i++) {
        if (strcmp(g_Mods[i].uuid, uuid) == 0) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Helper: Find mod prototype by key
// ============================================================================

static int find_mod_prototype_index(ModVariables *mod, const char *key) {
    if (!mod || !mod->prototypes) return -1;
    for (int i = 0; i < mod->prototype_count; i++) {
        if (strcmp(mod->prototypes[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Initialization
// ============================================================================

void uvar_init(void) {
    if (g_Initialized) return;

    memset(g_Prototypes, 0, sizeof(g_Prototypes));
    memset(g_Entities, 0, sizeof(g_Entities));
    memset(g_Mods, 0, sizeof(g_Mods));
    g_PrototypeCount = 0;
    g_EntityCount = 0;
    g_ModCount = 0;
    g_Dirty = false;
    g_ModsDirty = false;
    g_Initialized = true;

    LOG_LUA_INFO("User variables system initialized");
}

void uvar_shutdown(void) {
    if (!g_Initialized) return;

    // Every count is reset alongside the pointer it describes. A count left
    // standing over a freed array outlives this call in the static storage and
    // reads as "that many live slots" to whoever allocates the array next.
    for (int i = 0; i < g_EntityCount; i++) {
        if (g_Entities[i].vars) {
            for (int j = 0; j < g_Entities[i].var_count; j++) {
                free_variable(&g_Entities[i].vars[j]);
            }
            free(g_Entities[i].vars);
            g_Entities[i].vars = NULL;
        }
        g_Entities[i].var_count = 0;
    }

    // Free all mod variable storage
    for (int i = 0; i < g_ModCount; i++) {
        if (g_Mods[i].vars) {
            for (int j = 0; j < g_Mods[i].var_count; j++) {
                free_variable(&g_Mods[i].vars[j]);
            }
            free(g_Mods[i].vars);
            g_Mods[i].vars = NULL;
        }
        g_Mods[i].var_count = 0;
        if (g_Mods[i].prototypes) {
            free(g_Mods[i].prototypes);
            g_Mods[i].prototypes = NULL;
        }
        g_Mods[i].prototype_count = 0;
    }

    g_PrototypeCount = 0;
    g_EntityCount = 0;
    g_ModCount = 0;
    g_Initialized = false;

    LOG_LUA_INFO("User variables system shutdown");
}

// ============================================================================
// Prototype Registration
// ============================================================================

int uvar_register_prototype(const char *key, uint32_t flags) {
    if (!g_Initialized) uvar_init();

    // Check if already registered
    for (int i = 0; i < g_PrototypeCount; i++) {
        if (strcmp(g_Prototypes[i].key, key) == 0) {
            // Update flags
            g_Prototypes[i].flags = flags;
            LOG_LUA_INFO("Updated user variable prototype: %s (flags=0x%x)", key, flags);
            return i;
        }
    }

    // Register new prototype
    if (g_PrototypeCount >= UVAR_MAX_PROTOTYPES) {
        LOG_LUA_ERROR("Max prototypes reached (%d)", UVAR_MAX_PROTOTYPES);
        return -1;
    }

    int idx = g_PrototypeCount++;
    strncpy(g_Prototypes[idx].key, key, UVAR_MAX_KEY_LENGTH - 1);
    g_Prototypes[idx].key[UVAR_MAX_KEY_LENGTH - 1] = '\0';
    g_Prototypes[idx].flags = flags;
    g_Prototypes[idx].registered = true;

    LOG_LUA_INFO("Registered user variable: %s (flags=0x%x)", key, flags);
    return idx;
}

const UserVariablePrototype* uvar_get_prototype(const char *key) {
    for (int i = 0; i < g_PrototypeCount; i++) {
        if (strcmp(g_Prototypes[i].key, key) == 0) {
            return &g_Prototypes[i];
        }
    }
    return NULL;
}

int uvar_get_prototype_index(const char *key) {
    for (int i = 0; i < g_PrototypeCount; i++) {
        if (strcmp(g_Prototypes[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Entity Variable Storage
// ============================================================================

EntityVariables* uvar_get_or_create_entity(const char *guid, uint64_t handle) {
    if (!g_Initialized) uvar_init();

    // Find existing
    int idx = find_entity_index(guid);
    if (idx >= 0) {
        // Update handle if changed
        g_Entities[idx].entity_handle = handle;
        return &g_Entities[idx];
    }

    // Create new
    if (g_EntityCount >= UVAR_MAX_ENTITIES) {
        LOG_LUA_ERROR("Max entities reached (%d)", UVAR_MAX_ENTITIES);
        return NULL;
    }

    idx = g_EntityCount++;
    strncpy(g_Entities[idx].guid, guid, UVAR_GUID_LENGTH - 1);
    g_Entities[idx].guid[UVAR_GUID_LENGTH - 1] = '\0';
    g_Entities[idx].entity_handle = handle;
    g_Entities[idx].vars = NULL;
    g_Entities[idx].var_count = 0;

    return &g_Entities[idx];
}

EntityVariables* uvar_get_entity(const char *guid) {
    int idx = find_entity_index(guid);
    return (idx >= 0) ? &g_Entities[idx] : NULL;
}

// ============================================================================
// Variable Get/Set
// ============================================================================

void uvar_set(lua_State *L, const char *guid, uint64_t handle,
              const char *key, int value_index) {
    // Get prototype
    int proto_idx = uvar_get_prototype_index(key);
    if (proto_idx < 0) {
        // Auto-register with default flags if not registered
        proto_idx = uvar_register_prototype(key,
            UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_WRITEABLE_SERVER |
            UVAR_FLAG_PERSISTENT | UVAR_FLAG_SYNC_ON_TICK);
        if (proto_idx < 0) {
            luaL_error(L, "Failed to register variable '%s'", key);
            return;
        }
    }

    // Get or create entity storage
    EntityVariables *ent = uvar_get_or_create_entity(guid, handle);
    if (!ent) {
        luaL_error(L, "Failed to create entity storage for %s", guid);
        return;
    }

    // Ensure vars array is large enough.
    //
    // `old_count` is what the buffer actually carries over, which is nothing at
    // all when vars is NULL - realloc(NULL, n) returns UNINITIALISED memory. A
    // stale var_count standing over a NULL vars would otherwise start the init
    // loop past the end of the fresh buffer (initialising nothing) and shrink
    // the count on the way out, leaving free_variable() below to free whatever
    // garbage happened to sit in the slot. Growing only, and initialising every
    // slot the old buffer did not carry, makes both halves safe independently.
    if (ent->vars == NULL || ent->var_count <= proto_idx) {
        int old_count = ent->vars && ent->var_count > 0 ? ent->var_count : 0;
        int new_count = proto_idx + 1 > old_count ? proto_idx + 1 : old_count;
        UserVariable *new_vars = realloc(ent->vars, (size_t)new_count * sizeof(UserVariable));
        if (!new_vars) {
            luaL_error(L, "Out of memory");
            return;
        }
        // Initialize new slots
        for (int i = old_count; i < new_count; i++) {
            new_vars[i].type = UVAR_TYPE_NULL;
            new_vars[i].dirty = false;
            new_vars[i].value.string = NULL;
        }
        ent->vars = new_vars;
        ent->var_count = new_count;
    }

    // Free old value
    free_variable(&ent->vars[proto_idx]);

    // Convert Lua value to UserVariable
    UserVariable *var = &ent->vars[proto_idx];
    int vtype = lua_type(L, value_index);

    switch (vtype) {
        case LUA_TNIL:
            var->type = UVAR_TYPE_NULL;
            break;

        case LUA_TBOOLEAN:
            var->type = UVAR_TYPE_BOOLEAN;
            var->value.boolean = lua_toboolean(L, value_index);
            break;

        case LUA_TNUMBER:
            if (lua_isinteger(L, value_index)) {
                var->type = UVAR_TYPE_INTEGER;
                var->value.integer = lua_tointeger(L, value_index);
            } else {
                var->type = UVAR_TYPE_NUMBER;
                var->value.number = lua_tonumber(L, value_index);
            }
            break;

        case LUA_TSTRING: {
            var->type = UVAR_TYPE_STRING;
            size_t len;
            const char *str = lua_tolstring(L, value_index, &len);
            var->value.string = malloc(len + 1);
            if (var->value.string) {
                memcpy(var->value.string, str, len);
                var->value.string[len] = '\0';
            }
            break;
        }

        case LUA_TTABLE: {
            // Serialize table to JSON
            var->type = UVAR_TYPE_TABLE;
            luaL_Buffer b;
            luaL_buffinit(L, &b);

            // Push value to top of stack for json_stringify_value
            lua_pushvalue(L, value_index);
            json_stringify_value(L, lua_gettop(L), &b);
            lua_pop(L, 1);  // Pop the pushed value

            luaL_pushresult(&b);
            size_t json_len;
            const char *json = lua_tolstring(L, -1, &json_len);
            var->value.string = malloc(json_len + 1);
            if (var->value.string) {
                memcpy(var->value.string, json, json_len);
                var->value.string[json_len] = '\0';
            }
            lua_pop(L, 1);  // Pop JSON string
            break;
        }

        default:
            LOG_LUA_WARN("Unsupported type for user variable: %s", lua_typename(L, vtype));
            var->type = UVAR_TYPE_NULL;
            break;
    }

    // Keep the caller's own table as the cached object for this state, so a
    // later read returns the SAME table and in-place nested writes survive.
    {
        char ck[UVAR_GUID_LENGTH + UVAR_MAX_KEY_LENGTH + 8];
        varcache_key(ck, sizeof(ck), 'e', guid, key);
        if (vtype == LUA_TTABLE) varcache_store(L, ck, value_index);
        else                     varcache_remove(L, ck);
    }

    var->dirty = true;
    g_Dirty = true;

    LOG_LUA_DEBUG("Set %s.Vars.%s (type=%d)", guid, key, var->type);
}

int uvar_get(lua_State *L, const char *guid, const char *key) {
    // Get prototype
    int proto_idx = uvar_get_prototype_index(key);
    if (proto_idx < 0) {
        lua_pushnil(L);
        return 1;
    }

    // Get entity
    EntityVariables *ent = uvar_get_entity(guid);
    if (!ent || !ent->vars || ent->var_count <= proto_idx) {
        lua_pushnil(L);
        return 1;
    }

    // Get variable
    UserVariable *var = &ent->vars[proto_idx];

    switch (var->type) {
        case UVAR_TYPE_NULL:
            lua_pushnil(L);
            break;

        case UVAR_TYPE_BOOLEAN:
            lua_pushboolean(L, var->value.boolean);
            break;

        case UVAR_TYPE_INTEGER:
            lua_pushinteger(L, var->value.integer);
            break;

        case UVAR_TYPE_NUMBER:
            lua_pushnumber(L, var->value.number);
            break;

        case UVAR_TYPE_STRING:
            if (var->value.string) {
                lua_pushstring(L, var->value.string);
            } else {
                lua_pushnil(L);
            }
            break;

        case UVAR_TYPE_TABLE: {
            // Identity-stable: hand back the same table every read, so mods can
            // mutate it in place (entity.Vars.Foo.bar = 1). Only deserialize on
            // a cache miss (first read of this state, or after a reload).
            char ck[UVAR_GUID_LENGTH + UVAR_MAX_KEY_LENGTH + 8];
            varcache_key(ck, sizeof(ck), 'e', guid, key);
            if (varcache_push(L, ck)) break;

            if (var->value.string) {
                // Parse JSON back to table
                const char *end = json_parse_value(L, var->value.string);
                if (!end) {
                    LOG_LUA_ERROR("Failed to parse stored JSON for %s.%s", guid, key);
                    lua_pushnil(L);
                } else {
                    varcache_store(L, ck, -1);
                }
            } else {
                lua_pushnil(L);
            }
            break;
        }

        default:
            lua_pushnil(L);
            break;
    }

    return 1;
}

void uvar_mark_dirty(const char *guid, const char *key) {
    int proto_idx = uvar_get_prototype_index(key);
    if (proto_idx < 0) return;

    EntityVariables *ent = uvar_get_entity(guid);
    if (!ent || !ent->vars || ent->var_count <= proto_idx) return;

    ent->vars[proto_idx].dirty = true;
    g_Dirty = true;
}

int uvar_get_entities_with_variable(lua_State *L, const char *key) {
    int proto_idx = uvar_get_prototype_index(key);
    if (proto_idx < 0) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);
    int table_idx = 1;

    for (int i = 0; i < g_EntityCount; i++) {
        EntityVariables *ent = &g_Entities[i];
        if (ent->vars && ent->var_count > proto_idx &&
            ent->vars[proto_idx].type != UVAR_TYPE_NULL) {
            lua_pushstring(L, ent->guid);
            lua_rawseti(L, -2, table_idx++);
        }
    }

    return 1;
}

// ============================================================================
// Savegame Store — in-memory half
// ============================================================================
//
// Everything below builds or replaces the whole persistent variable set.
// vars_persist.c decides WHICH savegame the set belongs to and WHEN it is
// written; this file only knows how to turn the set into a Lua table and back.

int uvar_store_build(lua_State *L) {
    if (!g_Initialized) uvar_init();

    lua_newtable(L);
    int root = lua_gettop(L);

    lua_pushinteger(L, UVAR_STORE_VERSION);
    lua_setfield(L, root, "version");

    // Entity variable prototypes are global (one flag set per key), so they
    // ride along and are re-registered on restore. Without them a value read
    // back before its mod registers the key would land on auto-registered
    // default flags and quietly lose, say, its client-side visibility.
    lua_newtable(L);
    for (int i = 0; i < g_PrototypeCount; i++) {
        if ((g_Prototypes[i].flags & UVAR_FLAG_PERSISTENT) == 0) continue;
        lua_pushinteger(L, (lua_Integer)g_Prototypes[i].flags);
        lua_setfield(L, -2, g_Prototypes[i].key);
    }
    lua_setfield(L, root, "prototypes");

    lua_newtable(L);
    int entities = lua_gettop(L);
    for (int i = 0; i < g_EntityCount; i++) {
        EntityVariables *ent = &g_Entities[i];
        if (!ent->vars) continue;

        lua_newtable(L);
        int ent_t = lua_gettop(L);
        bool any = false;

        for (int j = 0; j < ent->var_count && j < g_PrototypeCount; j++) {
            if ((g_Prototypes[j].flags & UVAR_FLAG_PERSISTENT) == 0) continue;
            if (ent->vars[j].type == UVAR_TYPE_NULL) continue;

            // Read through uvar_get on purpose: for a table it returns the
            // CACHED object, so nested writes the mod made in place (which
            // never set var->dirty) are what gets serialized.
            uvar_get(L, ent->guid, g_Prototypes[j].key);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; }
            lua_setfield(L, ent_t, g_Prototypes[j].key);
            any = true;
        }

        if (any) lua_setfield(L, entities, ent->guid);
        else     lua_pop(L, 1);
    }
    lua_setfield(L, root, "entities");

    lua_newtable(L);
    int mods = lua_gettop(L);
    for (int i = 0; i < g_ModCount; i++) {
        ModVariables *mod = &g_Mods[i];
        if (!mod->vars || !mod->prototypes) continue;

        lua_newtable(L);
        int vars_t = lua_gettop(L);
        lua_newtable(L);
        int flags_t = lua_gettop(L);
        bool any = false;

        for (int j = 0; j < mod->var_count && j < mod->prototype_count; j++) {
            if ((mod->prototypes[j].flags & UVAR_FLAG_PERSISTENT) == 0) continue;
            if (mod->vars[j].type == UVAR_TYPE_NULL) continue;

            mvar_get(L, mod->uuid, mod->prototypes[j].key);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; }
            lua_setfield(L, vars_t, mod->prototypes[j].key);

            lua_pushinteger(L, (lua_Integer)mod->prototypes[j].flags);
            lua_setfield(L, flags_t, mod->prototypes[j].key);
            any = true;
        }

        if (any) {
            lua_newtable(L);
            lua_pushvalue(L, flags_t);
            lua_setfield(L, -2, "flags");
            lua_pushvalue(L, vars_t);
            lua_setfield(L, -2, "vars");
            lua_setfield(L, mods, mod->uuid);
        }
        lua_pop(L, 2);  // flags_t, vars_t
    }
    lua_setfield(L, root, "mods");

    return 1;
}

void uvar_store_clear(lua_State *L) {
    if (!g_Initialized) uvar_init();

    for (int i = 0; i < g_EntityCount; i++) {
        EntityVariables *ent = &g_Entities[i];
        if (!ent->vars) continue;
        for (int j = 0; j < ent->var_count; j++) {
            free_variable(&ent->vars[j]);
        }
        free(ent->vars);
        ent->vars = NULL;
        ent->var_count = 0;
    }
    // Entity slots are per-savegame; reusing them across loads would burn
    // through UVAR_MAX_ENTITIES with GUIDs the new savegame never mentions.
    memset(g_Entities, 0, sizeof(g_Entities));
    g_EntityCount = 0;

    // Mod slots are NOT reset: prototypes are registered once at bootstrap by
    // RegisterModVariable, and dropping them here would leave every later
    // mvar_set auto-registering with default flags instead.
    for (int i = 0; i < g_ModCount; i++) {
        ModVariables *mod = &g_Mods[i];
        // Reset the count even when there is no array to free. Skipping the
        // whole body on a NULL vars is what preserves a stale var_count, and
        // that is precisely the state mvar_set below cannot grow out of safely.
        if (mod->vars) {
            for (int j = 0; j < mod->var_count; j++) {
                free_variable(&mod->vars[j]);
            }
            free(mod->vars);
            mod->vars = NULL;
        }
        mod->var_count = 0;
        mod->dirty = false;
    }

    g_Dirty = false;
    g_ModsDirty = false;

    varcache_reset(L);
}

/* lua_next requires the key it hands back to stay a string; lua_tostring on a
 * numeric key would rewrite that slot in place and corrupt the traversal. */
static bool store_copy_key(lua_State *L, int key_index, char *out, size_t n) {
    if (lua_type(L, key_index) != LUA_TSTRING) return false;
    size_t len = 0;
    const char *k = lua_tolstring(L, key_index, &len);
    if (!k || len == 0 || len >= n) return false;
    memcpy(out, k, len);
    out[len] = '\0';
    return true;
}

void uvar_store_apply(lua_State *L, int root_idx) {
    if (!g_Initialized) uvar_init();
    if (root_idx < 0) root_idx = lua_gettop(L) + root_idx + 1;

    uvar_store_clear(L);

    lua_getfield(L, root_idx, "prototypes");
    if (lua_istable(L, -1)) {
        int t = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, t) != 0) {
            char key[UVAR_MAX_KEY_LENGTH];
            if (store_copy_key(L, -2, key, sizeof(key)) && lua_isinteger(L, -1)) {
                uvar_register_prototype(key, (uint32_t)lua_tointeger(L, -1));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, root_idx, "entities");
    if (lua_istable(L, -1)) {
        int ents = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, ents) != 0) {
            char guid[UVAR_GUID_LENGTH];
            if (store_copy_key(L, -2, guid, sizeof(guid)) && lua_istable(L, -1)) {
                int vars_t = lua_gettop(L);
                lua_pushnil(L);
                while (lua_next(L, vars_t) != 0) {
                    char key[UVAR_MAX_KEY_LENGTH];
                    if (store_copy_key(L, -2, key, sizeof(key))) {
                        uvar_set(L, guid, 0, key, lua_gettop(L));
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, root_idx, "mods");
    if (lua_istable(L, -1)) {
        int mods = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, mods) != 0) {
            char uuid[UVAR_GUID_LENGTH];
            if (store_copy_key(L, -2, uuid, sizeof(uuid)) && lua_istable(L, -1)) {
                int entry = lua_gettop(L);

                // Flags first: mvar_set auto-registers a missing prototype with
                // defaults, and a default-registered key would then be stuck
                // with those flags for the rest of the session.
                lua_getfield(L, entry, "flags");
                if (lua_istable(L, -1)) {
                    int flags_t = lua_gettop(L);
                    lua_pushnil(L);
                    while (lua_next(L, flags_t) != 0) {
                        char key[UVAR_MAX_KEY_LENGTH];
                        if (store_copy_key(L, -2, key, sizeof(key)) && lua_isinteger(L, -1)) {
                            mvar_register_prototype(uuid, key,
                                                    (uint32_t)lua_tointeger(L, -1));
                        }
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);

                lua_getfield(L, entry, "vars");
                if (lua_istable(L, -1)) {
                    int vars_t = lua_gettop(L);
                    lua_pushnil(L);
                    while (lua_next(L, vars_t) != 0) {
                        char key[UVAR_MAX_KEY_LENGTH];
                        if (store_copy_key(L, -2, key, sizeof(key))) {
                            mvar_set(L, uuid, key, lua_gettop(L));
                        }
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // A restore is not a mod write: leaving the dirty flags set would make the
    // very next flush rewrite the store it just read.
    g_Dirty = false;
    g_ModsDirty = false;
    for (int i = 0; i < g_ModCount; i++) g_Mods[i].dirty = false;
}

// ============================================================================
// Entity Vars Proxy Metatable
// ============================================================================

// Userdata for entity vars proxy
typedef struct {
    char guid[UVAR_GUID_LENGTH];
    uint64_t handle;
} EntityVarsProxy;

static int entity_vars_proxy_index(lua_State *L) {
    EntityVarsProxy *proxy = (EntityVarsProxy*)luaL_checkudata(L, 1, "BG3EntityVars");
    const char *key = luaL_checkstring(L, 2);

    return uvar_get(L, proxy->guid, key);
}

static int entity_vars_proxy_newindex(lua_State *L) {
    EntityVarsProxy *proxy = (EntityVarsProxy*)luaL_checkudata(L, 1, "BG3EntityVars");
    const char *key = luaL_checkstring(L, 2);
    // value is at index 3

    uvar_set(L, proxy->guid, proxy->handle, key, 3);
    return 0;
}

static int entity_vars_proxy_pairs(lua_State *L) {
    EntityVarsProxy *proxy = (EntityVarsProxy*)luaL_checkudata(L, 1, "BG3EntityVars");

    // Return all variables for this entity as an iterator
    lua_newtable(L);

    EntityVariables *ent = uvar_get_entity(proxy->guid);
    if (ent && ent->vars) {
        for (int i = 0; i < ent->var_count && i < g_PrototypeCount; i++) {
            if (ent->vars[i].type != UVAR_TYPE_NULL) {
                uvar_get(L, proxy->guid, g_Prototypes[i].key);
                lua_setfield(L, -2, g_Prototypes[i].key);
            }
        }
    }

    // Use pairs on the table we just built
    lua_getglobal(L, "pairs");
    lua_pushvalue(L, -2);
    lua_call(L, 1, 3);

    return 3;
}

static int entity_vars_proxy_tostring(lua_State *L) {
    EntityVarsProxy *proxy = (EntityVarsProxy*)luaL_checkudata(L, 1, "BG3EntityVars");
    lua_pushfstring(L, "EntityVars(%s)", proxy->guid);
    return 1;
}

void uvar_push_entity_proxy(lua_State *L, const char *guid, uint64_t handle) {
    // Create userdata
    EntityVarsProxy *proxy = (EntityVarsProxy*)lua_newuserdata(L, sizeof(EntityVarsProxy));
    strncpy(proxy->guid, guid, UVAR_GUID_LENGTH - 1);
    proxy->guid[UVAR_GUID_LENGTH - 1] = '\0';
    proxy->handle = handle;

    // Set metatable
    luaL_getmetatable(L, "BG3EntityVars");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        // Create metatable if not exists
        luaL_newmetatable(L, "BG3EntityVars");

        lua_pushcfunction(L, entity_vars_proxy_index);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, entity_vars_proxy_newindex);
        lua_setfield(L, -2, "__newindex");

        lua_pushcfunction(L, entity_vars_proxy_pairs);
        lua_setfield(L, -2, "__pairs");

        lua_pushcfunction(L, entity_vars_proxy_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_setmetatable(L, -2);
}

// ============================================================================
// Lua API: Ext.Vars.RegisterUserVariable(name, opts)
// ============================================================================

static int lua_register_user_variable(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    uint32_t flags = 0;

    // Parse options
    lua_getfield(L, 2, "Server");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_IS_ON_SERVER;
        flags |= UVAR_FLAG_WRITEABLE_SERVER;  // Default writeable if on server
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "Client");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_IS_ON_CLIENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "WriteableOnServer");
    if (lua_isboolean(L, -1)) {
        if (lua_toboolean(L, -1)) {
            flags |= UVAR_FLAG_WRITEABLE_SERVER;
        } else {
            flags &= ~UVAR_FLAG_WRITEABLE_SERVER;
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "WriteableOnClient");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_WRITEABLE_CLIENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "Persistent");
    if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) {
        // Default true
        flags |= UVAR_FLAG_PERSISTENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "SyncToClient");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_SYNC_TO_CLIENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "SyncToServer");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_SYNC_TO_SERVER;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "SyncOnWrite");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_SYNC_ON_WRITE;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "SyncOnTick");
    if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) {
        // Default true
        flags |= UVAR_FLAG_SYNC_ON_TICK;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "DontCache");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_DONT_CACHE;
    }
    lua_pop(L, 1);

    int idx = uvar_register_prototype(key, flags);
    lua_pushboolean(L, idx >= 0);
    return 1;
}

// ============================================================================
// Lua API: Ext.Vars.GetEntitiesWithVariable(varName)
// ============================================================================

static int lua_get_entities_with_variable(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    return uvar_get_entities_with_variable(L, key);
}

// ============================================================================
// Lua API: Ext.Vars.SyncUserVariables()
// ============================================================================

static int lua_sync_user_variables(lua_State *L) {
    (void)L;
    // Windows uses this to push variables to peers, not to write the savegame.
    // It must not write the store either: the store is a snapshot of the last
    // save, and a mod calling this mid-session would stamp post-save state onto
    // that save, which is exactly the divergence this port had before.
    return 0;
}

// ============================================================================
// Lua API: Ext.Vars.DirtyUserVariables([entityGuid], [varName])
// ============================================================================

static int lua_dirty_user_variables(lua_State *L) {
    const char *guid = lua_isstring(L, 1) ? lua_tostring(L, 1) : NULL;
    const char *key = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;

    if (!guid) {
        // Mark all dirty
        for (int i = 0; i < g_EntityCount; i++) {
            EntityVariables *ent = &g_Entities[i];
            if (ent->vars) {
                for (int j = 0; j < ent->var_count; j++) {
                    ent->vars[j].dirty = true;
                }
            }
        }
        g_Dirty = true;
    } else if (!key) {
        // Mark all vars for entity dirty
        EntityVariables *ent = uvar_get_entity(guid);
        if (ent && ent->vars) {
            for (int j = 0; j < ent->var_count; j++) {
                ent->vars[j].dirty = true;
            }
            g_Dirty = true;
        }
    } else {
        // Mark specific var dirty
        uvar_mark_dirty(guid, key);
    }

    return 0;
}

// ============================================================================
// Mod Variables Implementation
// ============================================================================

ModVariables* mvar_get_or_create_mod(const char *mod_uuid) {
    if (!g_Initialized) uvar_init();

    // Find existing
    int idx = find_mod_index(mod_uuid);
    if (idx >= 0) {
        return &g_Mods[idx];
    }

    // Create new
    if (g_ModCount >= UVAR_MAX_MODS) {
        // Fires once per attempt, which in a large profile means hundreds of
        // identical lines that say nothing about the consequence.
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_LUA_ERROR("mod variable storage full at %d mods; '%s' and any "
                          "later mod cannot register Ext.Vars and may fail to "
                          "initialise.", UVAR_MAX_MODS, mod_uuid);
        }
        return NULL;
    }

    idx = g_ModCount++;
    strncpy(g_Mods[idx].uuid, mod_uuid, UVAR_GUID_LENGTH - 1);
    g_Mods[idx].uuid[UVAR_GUID_LENGTH - 1] = '\0';
    g_Mods[idx].prototypes = NULL;
    g_Mods[idx].prototype_count = 0;
    g_Mods[idx].vars = NULL;
    g_Mods[idx].var_count = 0;
    g_Mods[idx].dirty = false;

    return &g_Mods[idx];
}

ModVariables* mvar_get_mod(const char *mod_uuid) {
    int idx = find_mod_index(mod_uuid);
    return (idx >= 0) ? &g_Mods[idx] : NULL;
}

int mvar_register_prototype(const char *mod_uuid, const char *key, uint32_t flags) {
    ModVariables *mod = mvar_get_or_create_mod(mod_uuid);
    if (!mod) return -1;

    // Check if already registered
    int existing = find_mod_prototype_index(mod, key);
    if (existing >= 0) {
        mod->prototypes[existing].flags = flags;
        LOG_LUA_INFO("Updated mod variable prototype: %s.%s (flags=0x%x)", mod_uuid, key, flags);
        return existing;
    }

    // Register new prototype
    int new_count = mod->prototype_count + 1;
    UserVariablePrototype *new_protos = realloc(mod->prototypes,
                                                 new_count * sizeof(UserVariablePrototype));
    if (!new_protos) {
        LOG_LUA_ERROR("Out of memory for mod prototype");
        return -1;
    }

    mod->prototypes = new_protos;
    int idx = mod->prototype_count++;
    strncpy(mod->prototypes[idx].key, key, UVAR_MAX_KEY_LENGTH - 1);
    mod->prototypes[idx].key[UVAR_MAX_KEY_LENGTH - 1] = '\0';
    mod->prototypes[idx].flags = flags;
    mod->prototypes[idx].registered = true;

    LOG_LUA_INFO("Registered mod variable: %s.%s (flags=0x%x)", mod_uuid, key, flags);
    return idx;
}

void mvar_set(lua_State *L, const char *mod_uuid, const char *key, int value_index) {
    ModVariables *mod = mvar_get_or_create_mod(mod_uuid);
    if (!mod) {
        luaL_error(L, "Failed to get/create mod storage for %s", mod_uuid);
        return;
    }

    // Get or create prototype
    int proto_idx = find_mod_prototype_index(mod, key);
    if (proto_idx < 0) {
        // Auto-register with default flags
        proto_idx = mvar_register_prototype(mod_uuid, key,
            UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_WRITEABLE_SERVER |
            UVAR_FLAG_PERSISTENT | UVAR_FLAG_SYNC_ON_TICK);
        if (proto_idx < 0) {
            luaL_error(L, "Failed to register mod variable '%s'", key);
            return;
        }
    }

    // Ensure vars array is large enough.
    //
    // `old_count` is what the buffer actually carries over, which is nothing at
    // all when vars is NULL - realloc(NULL, n) returns UNINITIALISED memory. A
    // stale var_count standing over a NULL vars would otherwise start the init
    // loop past the end of the fresh buffer (initialising nothing) and shrink
    // the count on the way out, leaving free_variable() below to free whatever
    // garbage happened to sit in the slot. Growing only, and initialising every
    // slot the old buffer did not carry, makes both halves safe independently.
    if (mod->vars == NULL || mod->var_count <= proto_idx) {
        int old_count = mod->vars && mod->var_count > 0 ? mod->var_count : 0;
        int new_count = proto_idx + 1 > old_count ? proto_idx + 1 : old_count;
        UserVariable *new_vars = realloc(mod->vars, (size_t)new_count * sizeof(UserVariable));
        if (!new_vars) {
            luaL_error(L, "Out of memory");
            return;
        }
        // Initialize new slots
        for (int i = old_count; i < new_count; i++) {
            new_vars[i].type = UVAR_TYPE_NULL;
            new_vars[i].dirty = false;
            new_vars[i].value.string = NULL;
        }
        mod->vars = new_vars;
        mod->var_count = new_count;
    }

    // Free old value
    free_variable(&mod->vars[proto_idx]);

    // Convert Lua value to UserVariable
    UserVariable *var = &mod->vars[proto_idx];
    int vtype = lua_type(L, value_index);

    switch (vtype) {
        case LUA_TNIL:
            var->type = UVAR_TYPE_NULL;
            break;

        case LUA_TBOOLEAN:
            var->type = UVAR_TYPE_BOOLEAN;
            var->value.boolean = lua_toboolean(L, value_index);
            break;

        case LUA_TNUMBER:
            if (lua_isinteger(L, value_index)) {
                var->type = UVAR_TYPE_INTEGER;
                var->value.integer = lua_tointeger(L, value_index);
            } else {
                var->type = UVAR_TYPE_NUMBER;
                var->value.number = lua_tonumber(L, value_index);
            }
            break;

        case LUA_TSTRING: {
            var->type = UVAR_TYPE_STRING;
            size_t len;
            const char *str = lua_tolstring(L, value_index, &len);
            var->value.string = malloc(len + 1);
            if (var->value.string) {
                memcpy(var->value.string, str, len);
                var->value.string[len] = '\0';
            }
            break;
        }

        case LUA_TTABLE: {
            // Serialize table to JSON
            var->type = UVAR_TYPE_TABLE;
            luaL_Buffer b;
            luaL_buffinit(L, &b);

            lua_pushvalue(L, value_index);
            json_stringify_value(L, lua_gettop(L), &b);
            lua_pop(L, 1);

            luaL_pushresult(&b);
            size_t json_len;
            const char *json = lua_tolstring(L, -1, &json_len);
            var->value.string = malloc(json_len + 1);
            if (var->value.string) {
                memcpy(var->value.string, json, json_len);
                var->value.string[json_len] = '\0';
            }
            lua_pop(L, 1);
            break;
        }

        default:
            LOG_LUA_WARN("Unsupported type for mod variable: %s", lua_typename(L, vtype));
            var->type = UVAR_TYPE_NULL;
            break;
    }

    // Keep the caller's own table as the cached object for this state, so a
    // later read returns the SAME table and in-place nested writes survive
    // (Vars.SittingOut[combat] = {} then Vars.SittingOut[combat][uuid] = true).
    {
        char ck[UVAR_GUID_LENGTH + UVAR_MAX_KEY_LENGTH + 8];
        varcache_key(ck, sizeof(ck), 'm', mod_uuid, key);
        if (vtype == LUA_TTABLE) varcache_store(L, ck, value_index);
        else                     varcache_remove(L, ck);
    }

    var->dirty = true;
    mod->dirty = true;
    g_ModsDirty = true;

    LOG_LUA_DEBUG("Set mod %s.%s (type=%d)", mod_uuid, key, var->type);
}

int mvar_get(lua_State *L, const char *mod_uuid, const char *key) {
    ModVariables *mod = mvar_get_mod(mod_uuid);
    if (!mod) {
        lua_pushnil(L);
        return 1;
    }

    int proto_idx = find_mod_prototype_index(mod, key);
    if (proto_idx < 0 || !mod->vars || mod->var_count <= proto_idx) {
        lua_pushnil(L);
        return 1;
    }

    UserVariable *var = &mod->vars[proto_idx];

    switch (var->type) {
        case UVAR_TYPE_NULL:
            lua_pushnil(L);
            break;

        case UVAR_TYPE_BOOLEAN:
            lua_pushboolean(L, var->value.boolean);
            break;

        case UVAR_TYPE_INTEGER:
            lua_pushinteger(L, var->value.integer);
            break;

        case UVAR_TYPE_NUMBER:
            lua_pushnumber(L, var->value.number);
            break;

        case UVAR_TYPE_STRING:
            if (var->value.string) {
                lua_pushstring(L, var->value.string);
            } else {
                lua_pushnil(L);
            }
            break;

        case UVAR_TYPE_TABLE: {
            // Identity-stable: hand back the same table every read. Without
            // this, Vars.X[k] = v is written into a throwaway parse result and
            // silently lost (see the cache helpers above).
            char ck[UVAR_GUID_LENGTH + UVAR_MAX_KEY_LENGTH + 8];
            varcache_key(ck, sizeof(ck), 'm', mod_uuid, key);
            if (varcache_push(L, ck)) break;

            if (var->value.string) {
                const char *end = json_parse_value(L, var->value.string);
                if (!end) {
                    LOG_LUA_ERROR("Failed to parse stored JSON for mod %s.%s", mod_uuid, key);
                    lua_pushnil(L);
                } else {
                    varcache_store(L, ck, -1);
                }
            } else {
                lua_pushnil(L);
            }
            break;
        }

        default:
            lua_pushnil(L);
            break;
    }

    return 1;
}

void mvar_mark_dirty(const char *mod_uuid, const char *key) {
    ModVariables *mod = mvar_get_mod(mod_uuid);
    if (!mod) return;

    if (key) {
        int proto_idx = find_mod_prototype_index(mod, key);
        if (proto_idx >= 0 && mod->vars && mod->var_count > proto_idx) {
            mod->vars[proto_idx].dirty = true;
        }
    }
    mod->dirty = true;
    g_ModsDirty = true;
}

// ============================================================================
// Mod Variables Proxy Metatable
// ============================================================================

typedef struct {
    char uuid[UVAR_GUID_LENGTH];
} ModVarsProxy;

static int mod_vars_proxy_index(lua_State *L) {
    ModVarsProxy *proxy = (ModVarsProxy*)luaL_checkudata(L, 1, "BG3ModVars");
    const char *key = luaL_checkstring(L, 2);

    return mvar_get(L, proxy->uuid, key);
}

static int mod_vars_proxy_newindex(lua_State *L) {
    ModVarsProxy *proxy = (ModVarsProxy*)luaL_checkudata(L, 1, "BG3ModVars");
    const char *key = luaL_checkstring(L, 2);

    mvar_set(L, proxy->uuid, key, 3);
    return 0;
}

static int mod_vars_proxy_pairs(lua_State *L) {
    ModVarsProxy *proxy = (ModVarsProxy*)luaL_checkudata(L, 1, "BG3ModVars");

    lua_newtable(L);

    ModVariables *mod = mvar_get_mod(proxy->uuid);
    if (mod && mod->vars && mod->prototypes) {
        for (int i = 0; i < mod->var_count && i < mod->prototype_count; i++) {
            if (mod->vars[i].type != UVAR_TYPE_NULL) {
                mvar_get(L, proxy->uuid, mod->prototypes[i].key);
                lua_setfield(L, -2, mod->prototypes[i].key);
            }
        }
    }

    lua_getglobal(L, "pairs");
    lua_pushvalue(L, -2);
    lua_call(L, 1, 3);

    return 3;
}

static int mod_vars_proxy_tostring(lua_State *L) {
    ModVarsProxy *proxy = (ModVarsProxy*)luaL_checkudata(L, 1, "BG3ModVars");
    lua_pushfstring(L, "ModVars(%s)", proxy->uuid);
    return 1;
}

void mvar_push_mod_proxy(lua_State *L, const char *mod_uuid) {
    ModVarsProxy *proxy = (ModVarsProxy*)lua_newuserdata(L, sizeof(ModVarsProxy));
    strncpy(proxy->uuid, mod_uuid, UVAR_GUID_LENGTH - 1);
    proxy->uuid[UVAR_GUID_LENGTH - 1] = '\0';

    luaL_getmetatable(L, "BG3ModVars");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_newmetatable(L, "BG3ModVars");

        lua_pushcfunction(L, mod_vars_proxy_index);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, mod_vars_proxy_newindex);
        lua_setfield(L, -2, "__newindex");

        lua_pushcfunction(L, mod_vars_proxy_pairs);
        lua_setfield(L, -2, "__pairs");

        lua_pushcfunction(L, mod_vars_proxy_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_setmetatable(L, -2);
}

// ============================================================================
// Lua API: Ext.Vars.RegisterModVariable(modUuid, name, opts)
// ============================================================================

static int lua_register_mod_variable(lua_State *L) {
    const char *mod_uuid = luaL_checkstring(L, 1);
    const char *key = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);

    uint32_t flags = 0;

    // Parse options (same as user variables)
    lua_getfield(L, 3, "Server");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_IS_ON_SERVER;
        flags |= UVAR_FLAG_WRITEABLE_SERVER;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "Client");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_IS_ON_CLIENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "Persistent");
    if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_PERSISTENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "SyncOnTick");
    if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_SYNC_ON_TICK;
    }
    lua_pop(L, 1);

    int idx = mvar_register_prototype(mod_uuid, key, flags);
    lua_pushboolean(L, idx >= 0);
    return 1;
}

// ============================================================================
// Lua API: Ext.Vars.GetModVariables(modUuid)
// ============================================================================

static int lua_get_mod_variables(lua_State *L) {
    const char *mod_uuid = luaL_checkstring(L, 1);

    // Ensure mod exists
    mvar_get_or_create_mod(mod_uuid);

    // Return proxy
    mvar_push_mod_proxy(L, mod_uuid);
    return 1;
}

// ============================================================================
// Lua API: Ext.Vars.SyncModVariables()
// ============================================================================

static int lua_sync_mod_variables(lua_State *L) {
    (void)L;
    // See lua_sync_user_variables: peer sync, never a store write.
    return 0;
}

// ============================================================================
// Lua API: Ext.Vars.DirtyModVariables([modUuid], [varName])
// ============================================================================

static int lua_dirty_mod_variables(lua_State *L) {
    const char *mod_uuid = lua_isstring(L, 1) ? lua_tostring(L, 1) : NULL;
    const char *key = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;

    if (!mod_uuid) {
        // Mark all mods dirty
        for (int i = 0; i < g_ModCount; i++) {
            g_Mods[i].dirty = true;
            if (g_Mods[i].vars) {
                for (int j = 0; j < g_Mods[i].var_count; j++) {
                    g_Mods[i].vars[j].dirty = true;
                }
            }
        }
        g_ModsDirty = true;
    } else {
        mvar_mark_dirty(mod_uuid, key);
    }

    return 0;
}

// ============================================================================
// Registration
// ============================================================================

void uvar_register_lua(lua_State *L, int ext_vars_index) {
    // Convert negative index to absolute
    if (ext_vars_index < 0) {
        ext_vars_index = lua_gettop(L) + ext_vars_index + 1;
    }

    // Add functions to Ext.Vars table
    lua_pushcfunction(L, lua_register_user_variable);
    lua_setfield(L, ext_vars_index, "RegisterUserVariable");

    lua_pushcfunction(L, lua_get_entities_with_variable);
    lua_setfield(L, ext_vars_index, "GetEntitiesWithVariable");

    lua_pushcfunction(L, lua_sync_user_variables);
    lua_setfield(L, ext_vars_index, "SyncUserVariables");

    lua_pushcfunction(L, lua_dirty_user_variables);
    lua_setfield(L, ext_vars_index, "DirtyUserVariables");

    // Add mod variable functions
    lua_pushcfunction(L, lua_register_mod_variable);
    lua_setfield(L, ext_vars_index, "RegisterModVariable");

    lua_pushcfunction(L, lua_get_mod_variables);
    lua_setfield(L, ext_vars_index, "GetModVariables");

    lua_pushcfunction(L, lua_sync_mod_variables);
    lua_setfield(L, ext_vars_index, "SyncModVariables");

    lua_pushcfunction(L, lua_dirty_mod_variables);
    lua_setfield(L, ext_vars_index, "DirtyModVariables");

    // Create BG3EntityVars metatable
    luaL_newmetatable(L, "BG3EntityVars");

    lua_pushcfunction(L, entity_vars_proxy_index);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, entity_vars_proxy_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, entity_vars_proxy_pairs);
    lua_setfield(L, -2, "__pairs");

    lua_pushcfunction(L, entity_vars_proxy_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pop(L, 1);  // Pop BG3EntityVars metatable

    // Create BG3ModVars metatable
    luaL_newmetatable(L, "BG3ModVars");

    lua_pushcfunction(L, mod_vars_proxy_index);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, mod_vars_proxy_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, mod_vars_proxy_pairs);
    lua_setfield(L, -2, "__pairs");

    lua_pushcfunction(L, mod_vars_proxy_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pop(L, 1);  // Pop BG3ModVars metatable

    // Initialize system
    uvar_init();

    LOG_LUA_INFO("User and mod variables API registered");
}
