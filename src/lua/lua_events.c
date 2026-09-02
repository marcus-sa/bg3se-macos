/**
 * BG3SE-macOS - Event System Implementation
 *
 * Advanced event subscription system with:
 * - Priority-based handler ordering (lower priority = called first)
 * - Once flag for auto-unsubscription after first call
 * - Handler ID return for explicit unsubscription
 * - Deferred modifications during dispatch to prevent iterator corruption
 * - Protected calls to prevent cascade failures
 */

#include "lua_events.h"
#include "../stats/functor_hooks.h"
#include "../stats/deal_damage_layout.h"
#include "lua_gate.h"
#include "lua_runtime.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../mod/mod_loader.h"
#include "../entity/component_registry.h"
#include "../entity/component_lookup.h"
#include "../entity/entity_system.h"
#include "../entity/guid_lookup.h"
#include "../enum/enum_registry.h"
#include "../lifetime/lifetime.h"
#include "../strings/fixed_string.h"

#include <lauxlib.h>

#include <stdatomic.h>
#include <string.h>
#include <mach/mach_time.h>

// ============================================================================
// Constants
// ============================================================================

// Per EVENT TYPE, not overall: a common event like Tick or GameStateChanged
// collects one handler per interested mod. 45 SE mods is already most of the
// way to 64. Not observed overflowing yet, but it is the same shape of cap as
// MAX_MODS/MAX_MOD_UUIDS/UVAR_MAX_MODS, and the whole table only grows from
// 0.25 MB to 1.0 MB.
#define MAX_EVENT_HANDLERS 256
#define MAX_DEFERRED_OPERATIONS 256
#define DEFAULT_PRIORITY 100

// ============================================================================
// Data Structures
// ============================================================================

typedef struct {
    int callback_ref;       // Lua registry reference
    int priority;           // Lower = first (default 100)
    int once;               // Auto-unsubscribe after first call
    uint64_t handler_id;    // Unique ID for unsubscription
    char mod_name[64];      // Mod that registered this handler (for crash attribution)
} EventHandler;

typedef struct {
    BG3SEEventType event;
    uint64_t handler_id;
} DeferredUnsubscribe;

// ============================================================================
// Static State
// ============================================================================


/*
 * Every event dispatch needs a lifetime scope, not just a mod context.
 *
 * Objects handed to Lua (stats entries, entities) are stamped with the scope
 * that was current when they were created, and are refused once it ends. With
 * no scope open at all the current handle is the null handle, which never
 * validates -- so an object fetched inside an Ext.Events handler was expired
 * the moment it was returned, and even
 *
 *     Ext.Stats.Get("Target_AnimateDead").ContainerSpells
 *
 * failed within a single expression. Only the Osiris callback path opened a
 * scope, so every Ext.Events subscriber was affected.
 *
 * The mod context already brackets each dispatch exactly, including the early
 * exits, so the scope rides along with it.
 */
static void event_scope_begin(lua_State *L, const char *mod_name) {
    mod_context_push(mod_name);
    lifetime_lua_begin_scope(L);
}

static void event_scope_end(lua_State *L) {
    lifetime_lua_end_scope(L);
    mod_context_pop();
}

static EventHandler g_handlers[EVENT_MAX][MAX_EVENT_HANDLERS];
static int g_handler_counts[EVENT_MAX] = {0};
static uint64_t g_next_handler_id = 1;  // Global counter, never reuse
static int g_dispatch_depth[EVENT_MAX] = {0};  // Reentrancy tracking
static int g_initialized = 0;
static bool g_trace_enabled = false;  // Event tracing for debugging
static int g_current_game_state = 0;  // Cached from GameStateChanged (for Ext.Utils.GetGameState)

// Per-mod health tracking (for crash attribution and !mod_diag)
// One entry per mod that ever registers a handler; must cover the SE mod count.
#define MAX_MOD_HEALTH 1024

typedef struct {
    char mod_name[64];
    uint32_t handlers_registered;
    uint32_t errors_logged;
    uint32_t events_handled;
    uint64_t last_error_time;
    char last_error[256];
    bool soft_disabled;
} ModHealthEntry;

static ModHealthEntry g_mod_health[MAX_MOD_HEALTH];
static int g_mod_health_count = 0;

// Deferred unsubscriptions (processed after dispatch completes)
static DeferredUnsubscribe g_deferred_unsubs[MAX_DEFERRED_OPERATIONS];
static int g_deferred_unsub_count = 0;

// Event names for logging and Lua
static const char *g_event_names[EVENT_MAX] = {
    "SessionLoading",
    "SessionLoaded",
    "ResetCompleted",
    "Tick",
    "StatsLoaded",
    "ModuleLoadStarted",
    "GameStateChanged",
    "KeyInput",
    "DoConsoleCommand",
    "LuaConsoleInput",
    "Log",                 // Log message interception (Windows parity)
    // Engine events (Issue #51)
    "TurnStarted",
    "TurnEnded",
    "CombatStarted",
    "CombatEnded",
    "StatusApplied",
    "StatusRemoved",
    "EquipmentChanged",
    "LevelUp",
    // Additional engine events (Issue #51 expansion)
    "Died",
    "Downed",
    "Resurrected",
    "SpellCast",
    "SpellCastFinished",
    "HitNotification",
    "ShortRestStarted",
    "ApprovalChanged",
    // Lifecycle events (Issue #51 expansion)
    "StatsStructureLoaded",
    "ModuleResume",
    "Shutdown",
    // Functor events (Issue #53)
    "ExecuteFunctor",
    "AfterExecuteFunctor",
    "DealDamage",
    "DealtDamage",
    "BeforeDealDamage",
    "NetModMessage",          // Network mod message (Issue #6)
    "NetMessage",             // Legacy network message (no module, Issue #6)
    // Spell cast phase events (Issue #51 expansion)
    "SpellCastCountered",
    "SpellCastJumpStart",
    "ConcentrationCleared",
    "SpellCastLogicExecutionStart",
    "SpellCastLogicExecutionEnd",
    "SpellCastPrepareStart",
    "SpellCastPrepareEnd",
    "SpellCastPreviewEnd",
    // Client input events (needed by MCM's SubscribedEvents)
    "ControllerButtonInput",
    "MouseButtonInput",
};

// ============================================================================
// Internal: Mod Name Extraction from Lua Callstack
// ============================================================================

/**
 * Extract mod name from the Lua callstack.
 * Walks the stack looking for source paths matching "Mods/<ModName>/ScriptExtender/".
 * Falls back to mod_get_current_name() for bootstrap-time registrations.
 * If nothing found, uses "unknown".
 */
static void extract_mod_name_from_lua(lua_State *L, char *out, size_t out_size) {
    out[0] = '\0';

    // First try mod_get_current_name() — active during bootstrap
    const char *current = mod_get_current_name();
    if (current && current[0]) {
        strncpy(out, current, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    // Walk the Lua callstack looking for mod source paths
    lua_Debug ar;
    for (int level = 1; level < 10; level++) {
        if (!lua_getstack(L, level, &ar)) break;
        lua_getinfo(L, "S", &ar);

        if (!ar.source || ar.source[0] == '=') continue;

        // Look for "Mods/<ModName>/ScriptExtender/" pattern
        const char *mods_prefix = strstr(ar.source, "Mods/");
        if (!mods_prefix) mods_prefix = strstr(ar.source, "Mods\\");
        if (mods_prefix) {
            const char *name_start = mods_prefix + 5;  // skip "Mods/"
            const char *name_end = strchr(name_start, '/');
            if (!name_end) name_end = strchr(name_start, '\\');
            if (name_end && (size_t)(name_end - name_start) < out_size) {
                size_t len = (size_t)(name_end - name_start);
                if (len >= out_size) len = out_size - 1;
                memcpy(out, name_start, len);
                out[len] = '\0';
                return;
            }
        }
    }

    // Check if this is from the console (string input)
    if (lua_getstack(L, 1, &ar)) {
        lua_getinfo(L, "S", &ar);
        if (ar.source && (ar.source[0] == '=' || strstr(ar.source, "string"))) {
            strncpy(out, "console", out_size - 1);
            out[out_size - 1] = '\0';
            return;
        }
    }

    strncpy(out, "unknown", out_size - 1);
    out[out_size - 1] = '\0';
}

// ============================================================================
// Internal: Per-Mod Health Tracking
// ============================================================================

/**
 * Find or create a health entry for a mod.
 */
static ModHealthEntry *mod_health_get_or_create(const char *mod_name) {
    if (!mod_name || !mod_name[0]) mod_name = "unknown";

    for (int i = 0; i < g_mod_health_count; i++) {
        if (strcmp(g_mod_health[i].mod_name, mod_name) == 0) {
            return &g_mod_health[i];
        }
    }

    if (g_mod_health_count >= MAX_MOD_HEALTH) return NULL;

    ModHealthEntry *entry = &g_mod_health[g_mod_health_count++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->mod_name, mod_name, sizeof(entry->mod_name) - 1);
    return entry;
}

/**
 * Record a successful event dispatch for a mod.
 */
static void mod_health_record_success(const char *mod_name) {
    ModHealthEntry *entry = mod_health_get_or_create(mod_name);
    if (entry) entry->events_handled++;
}

/**
 * Record an error for a mod.
 */
static void mod_health_record_error(const char *mod_name, const char *error_msg) {
    ModHealthEntry *entry = mod_health_get_or_create(mod_name);
    if (!entry) return;
    entry->errors_logged++;
    entry->last_error_time = (uint64_t)mach_absolute_time();
    if (error_msg) {
        strncpy(entry->last_error, error_msg, sizeof(entry->last_error) - 1);
        entry->last_error[sizeof(entry->last_error) - 1] = '\0';
    }
}

// ============================================================================
// Internal: Priority Sort
// ============================================================================

/**
 * Sort handlers by priority (lower first) using insertion sort.
 * Stable sort preserves registration order for equal priorities.
 */
static void sort_handlers_by_priority(BG3SEEventType event) {
    int count = g_handler_counts[event];
    if (count <= 1) return;

    for (int i = 1; i < count; i++) {
        EventHandler key = g_handlers[event][i];
        int j = i - 1;

        while (j >= 0 && g_handlers[event][j].priority > key.priority) {
            g_handlers[event][j + 1] = g_handlers[event][j];
            j--;
        }
        g_handlers[event][j + 1] = key;
    }
}

// ============================================================================
// Internal: Remove Handler
// ============================================================================

static int remove_handler_by_id(lua_State *L, BG3SEEventType event, uint64_t handler_id) {
    for (int i = 0; i < g_handler_counts[event]; i++) {
        if (g_handlers[event][i].handler_id == handler_id) {
            // Release callback reference
            if (g_handlers[event][i].callback_ref != LUA_NOREF &&
                g_handlers[event][i].callback_ref != LUA_REFNIL) {
                luaL_unref(L, LUA_REGISTRYINDEX, g_handlers[event][i].callback_ref);
            }

            // Shift remaining handlers down using memmove (ARM64 SIMD-optimized)
            int remaining = g_handler_counts[event] - i - 1;
            if (remaining > 0) {
                memmove(&g_handlers[event][i],
                        &g_handlers[event][i + 1],
                        remaining * sizeof(EventHandler));
            }
            g_handler_counts[event]--;

            return 1;  // Found and removed
        }
    }
    return 0;  // Not found
}

// ============================================================================
// Internal: Process Deferred Unsubscriptions
// ============================================================================

static void process_deferred_unsubscribes(lua_State *L, BG3SEEventType event) {
    // Process all deferred unsubscriptions for this event
    // Using swap-and-pop for O(1) removal instead of O(n) shift
    int i = 0;
    while (i < g_deferred_unsub_count) {
        if (g_deferred_unsubs[i].event == event) {
            remove_handler_by_id(L, event, g_deferred_unsubs[i].handler_id);

            // Swap with last element and pop (O(1) removal)
            g_deferred_unsubs[i] = g_deferred_unsubs[--g_deferred_unsub_count];
            // Don't increment i - check same index again (now contains swapped element)
        } else {
            i++;
        }
    }
}

// Forward declaration: poll cache init is defined later in this file with the
// polling infrastructure. Called from events_init() which comes first.
static void poll_cache_init(void);

// ============================================================================
// Public API: Initialize
// ============================================================================

void events_init(void) {
    if (g_initialized) return;

    // Initialize handler counts and dispatch depth
    // Note: g_handlers array doesn't need initialization - only slots up to
    // g_handler_counts[e] are ever accessed, and they're properly filled on Subscribe
    memset(g_handler_counts, 0, sizeof(g_handler_counts));
    memset(g_dispatch_depth, 0, sizeof(g_dispatch_depth));

    g_next_handler_id = 1;
    g_deferred_unsub_count = 0;
    g_initialized = 1;

    // Initialise the one-frame poll cache (sets component_name pointers)
    poll_cache_init();

    LOG_EVENTS_INFO("Event system initialized");
}

// ============================================================================
// Public API: Fire Event
// ============================================================================

void events_fire(lua_State *L, BG3SEEventType event) {
    if (!L || event >= EVENT_MAX) return;

    int count = g_handler_counts[event];
    if (count == 0) return;

    // Log for non-Tick events (Tick is too frequent)
    if (event != EVENT_TICK) {
        LOG_EVENTS_DEBUG("Firing %s (%d handlers)", g_event_names[event], count);
    }

    g_dispatch_depth[event]++;

    for (int i = 0; i < g_handler_counts[event]; i++) {
        EventHandler *h = &g_handlers[event][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        // Skip soft-disabled mods
        ModHealthEntry *health = mod_health_get_or_create(h->mod_name);
        if (health && health->soft_disabled) continue;

        // Open a lifetime scope and set mod context for crash attribution
        event_scope_begin(L, h->mod_name);

        // Get callback from registry
        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table (empty for basic events)
        lua_newtable(L);

        // Protected call to prevent cascade failures
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("Error in %s handler (id=%llu, mod=%s): %s",
                       g_event_names[event], h->handler_id,
                       h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        // End the lifetime scope and clear mod context
        event_scope_end(L);

        // Handle Once flag - queue for deferred removal
        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){event, h->handler_id};
            }
        }
    }

    g_dispatch_depth[event]--;

    // Process deferred unsubscriptions when dispatch completes
    if (g_dispatch_depth[event] == 0) {
        process_deferred_unsubscribes(L, event);
    }
}

// ============================================================================
// Public API: Fire Tick Event (with DeltaTime)
// ============================================================================

void events_fire_tick(lua_State *L, float delta_time) {
    if (!L) return;

    int count = g_handler_counts[EVENT_TICK];
    if (count == 0) return;

    g_dispatch_depth[EVENT_TICK]++;

    for (int i = 0; i < g_handler_counts[EVENT_TICK]; i++) {
        EventHandler *h = &g_handlers[EVENT_TICK][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        // Skip soft-disabled mods
        ModHealthEntry *health = mod_health_get_or_create(h->mod_name);
        if (health && health->soft_disabled) continue;

        // Open a lifetime scope and set mod context for crash attribution
        event_scope_begin(L, h->mod_name);

        // Get callback from registry
        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table with DeltaTime
        lua_newtable(L);
        lua_pushnumber(L, delta_time);
        lua_setfield(L, -2, "DeltaTime");

        // Protected call
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("Tick handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        // Handle Once flag
        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_TICK, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_TICK]--;

    if (g_dispatch_depth[EVENT_TICK] == 0) {
        process_deferred_unsubscribes(L, EVENT_TICK);
    }
}

// ============================================================================
// Public API: Fire GameStateChanged Event (with FromState and ToState)
// ============================================================================

// The port infers a generic ServerGameState (game_state.c), but mods subscribe
// in client context and compare e.ToState against Ext.Enums.ClientGameState
// (ecl::GameState). Those enums use DIFFERENT numeric values (e.g. Running is
// 13 internally but 18 in ecl, and the main menu is Idle=3 internally but
// Menu=7 in ecl). Map to ecl values so MCM's `e.ToState == ClientGameState.X`
// checks match — otherwise its client init (which builds the settings window)
// never fires. Unmapped states pass through unchanged.
static int server_state_to_ecl(int s) {
    switch (s) {
        case 2:  return 1;   // Init          -> Init
        case 3:  return 7;   // Idle          -> Menu (main menu)
        case 4:  return 8;   // Exit          -> Exit
        case 5:  return 10;  // LoadLevel     -> LoadLevel
        case 6:  return 11;  // LoadModule    -> LoadModule
        case 7:  return 12;  // LoadSession   -> LoadSession
        case 8:  return 13;  // UnloadLevel   -> UnloadLevel
        case 9:  return 14;  // UnloadModule  -> UnloadModule
        case 10: return 15;  // UnloadSession -> UnloadSession
        case 12: return 16;  // Paused        -> Paused
        case 13: return 18;  // Running       -> Running
        case 14: return 21;  // Save          -> Save
        case 15: return 19;  // Disconnect    -> Disconnect
        default: return s;   // Unknown(0) and any others pass through
    }
}

// ecl::GameState label for an internal ServerGameState (NULL if none).
static const char *server_state_to_ecl_label(int s) {
    switch (s) {
        case 0:  return "Unknown";
        case 2:  return "Init";
        case 3:  return "Menu";          // main menu
        case 4:  return "Exit";
        case 5:  return "LoadLevel";
        case 6:  return "LoadModule";
        case 7:  return "LoadSession";
        case 8:  return "UnloadLevel";
        case 9:  return "UnloadModule";
        case 10: return "UnloadSession";
        case 12: return "Paused";
        case 13: return "Running";
        case 14: return "Save";
        case 15: return "Disconnect";
        default: return NULL;
    }
}

// Push the matching Ext.Enums.ClientGameState EnumValue *userdata* for an internal
// state. Mods compare e.ToState with `==` against Ext.Enums.ClientGameState.X,
// and Lua only invokes __eq between two userdata of the same type — an integer
// never equals the EnumValue. So we must hand back the same userdata. Falls back
// to the ecl integer if the enum isn't available.
void events_push_client_gamestate(lua_State *L, int internal_state) {
    // Every lua_getfield target must be type-checked: this runs during event
    // construction BEFORE the handler's lua_pcall, so an "attempt to index
    // nil" here would hit the panic handler and abort the process.
    const char *label = server_state_to_ecl_label(internal_state);
    if (label) {
        int base = lua_gettop(L);
        lua_getglobal(L, "Ext");                     // Ext
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "Enums");            // Ext, Enums
            if (lua_istable(L, -1) || lua_isuserdata(L, -1)) {
                lua_getfield(L, -1, "ClientGameState");  // Ext, Enums, CGS
                if (lua_istable(L, -1) || lua_isuserdata(L, -1)) {
                    lua_getfield(L, -1, label);      // Ext, Enums, CGS, val
                    if (!lua_isnil(L, -1)) {
                        lua_insert(L, base + 1);     // val, Ext, Enums, CGS
                        lua_settop(L, base + 1);     // val
                        return;
                    }
                }
            }
        }
        lua_settop(L, base);
    }
    lua_pushinteger(L, server_state_to_ecl(internal_state));
}

void events_fire_game_state_changed(lua_State *L, int fromState, int toState) {
    // Cache the current game state for Ext.Utils.GetGameState()
    g_current_game_state = toState;

    if (!L) return;

    int count = g_handler_counts[EVENT_GAME_STATE_CHANGED];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing GameStateChanged (from=%d, to=%d, %d handlers)",
                fromState, toState, count);

    g_dispatch_depth[EVENT_GAME_STATE_CHANGED]++;

    for (int i = 0; i < g_handler_counts[EVENT_GAME_STATE_CHANGED]; i++) {
        EventHandler *h = &g_handlers[EVENT_GAME_STATE_CHANGED][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        // Skip soft-disabled mods
        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        // Get callback from registry
        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table with FromState and ToState as ClientGameState
        // EnumValue userdata (so `e.ToState == Ext.Enums.ClientGameState.X` works).
        lua_newtable(L);
        events_push_client_gamestate(L,fromState);
        lua_setfield(L, -2, "FromState");
        events_push_client_gamestate(L,toState);
        lua_setfield(L, -2, "ToState");

        // Protected call
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("GameStateChanged handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        // Handle Once flag
        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_GAME_STATE_CHANGED, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_GAME_STATE_CHANGED]--;

    if (g_dispatch_depth[EVENT_GAME_STATE_CHANGED] == 0) {
        process_deferred_unsubscribes(L, EVENT_GAME_STATE_CHANGED);
    }
}

int events_get_current_game_state(void) {
    return g_current_game_state;
}

// Map a macOS virtual keycode (kVK_*) to the SDL scancode NAME string that
// Windows BG3SE puts in KeyInputEvent.Key (see GameDefinitions/Enumerations/
// IMGUI.inl SDLScanCode enum). Mods (MCM keybindings, e.Key == "ESCAPE", etc.)
// compare against these names, so the port must deliver names, not raw keycodes.
static const char *macos_keycode_to_sdl_name(int kc) {
    switch (kc) {
        // Letters
        case 0x00: return "A"; case 0x0B: return "B"; case 0x08: return "C";
        case 0x02: return "D"; case 0x0E: return "E"; case 0x03: return "F";
        case 0x05: return "G"; case 0x04: return "H"; case 0x22: return "I";
        case 0x26: return "J"; case 0x28: return "K"; case 0x25: return "L";
        case 0x2E: return "M"; case 0x2D: return "N"; case 0x1F: return "O";
        case 0x23: return "P"; case 0x0C: return "Q"; case 0x0F: return "R";
        case 0x01: return "S"; case 0x11: return "T"; case 0x20: return "U";
        case 0x09: return "V"; case 0x0D: return "W"; case 0x07: return "X";
        case 0x10: return "Y"; case 0x06: return "Z";
        // Number row
        case 0x12: return "NUM_1"; case 0x13: return "NUM_2"; case 0x14: return "NUM_3";
        case 0x15: return "NUM_4"; case 0x17: return "NUM_5"; case 0x16: return "NUM_6";
        case 0x1A: return "NUM_7"; case 0x1C: return "NUM_8"; case 0x19: return "NUM_9";
        case 0x1D: return "NUM_0";
        // Punctuation / symbols
        case 0x32: return "GRAVE"; case 0x1B: return "MINUS"; case 0x18: return "EQUALS";
        case 0x21: return "LEFTBRACKET"; case 0x1E: return "RIGHTBRACKET";
        case 0x2A: return "BACKSLASH"; case 0x29: return "SEMICOLON";
        case 0x27: return "APOSTROPHE"; case 0x2B: return "COMMA";
        case 0x2F: return "PERIOD"; case 0x2C: return "SLASH";
        // Editing / whitespace
        case 0x24: return "RETURN"; case 0x30: return "TAB"; case 0x31: return "SPACE";
        case 0x33: return "BACKSPACE"; case 0x35: return "ESCAPE"; case 0x39: return "CAPSLOCK";
        // Navigation cluster
        case 0x73: return "HOME"; case 0x77: return "END"; case 0x74: return "PAGEUP";
        case 0x79: return "PAGEDOWN"; case 0x75: return "DEL";
        // kVK_Help occupies the Insert position on PC keyboards. MCM's default
        // "Toggle MCM window" bind is INSERT, and without this it reported
        // UNKNOWN and could never match.
        case 0x72: return "INSERT";
        case 0x7B: return "LEFT"; case 0x7C: return "RIGHT"; case 0x7D: return "DOWN"; case 0x7E: return "UP";
        // Function keys
        case 0x7A: return "F1"; case 0x78: return "F2"; case 0x63: return "F3";
        case 0x76: return "F4"; case 0x60: return "F5"; case 0x61: return "F6";
        case 0x62: return "F7"; case 0x64: return "F8"; case 0x65: return "F9";
        case 0x6D: return "F10"; case 0x67: return "F11"; case 0x6F: return "F12";
        default: return "UNKNOWN";
    }
}

void events_fire_key_input(lua_State *L, int keyCode, bool pressed, int modifiers, const char *character) {
    if (!L) return;

    int count = g_handler_counts[EVENT_KEY_INPUT];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing KeyInput (key=%d, pressed=%d, mods=0x%x, %d handlers)",
                keyCode, pressed, modifiers, count);

    g_dispatch_depth[EVENT_KEY_INPUT]++;

    for (int i = 0; i < g_handler_counts[EVENT_KEY_INPUT]; i++) {
        EventHandler *h = &g_handlers[EVENT_KEY_INPUT][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (lua_isfunction(L, -1)) {
            // Create event data table matching Windows BG3SE KeyInputEvent:
            // { Event="KeyDown"/"KeyUp", Key=<SDL scancode name>, Modifiers,
            //   Pressed, Repeat }. Mods (MCM keybindings, e.Key=="ESCAPE", the
            //   e.Event=="KeyDown" guard) rely on this exact shape.
            lua_newtable(L);
            lua_pushstring(L, pressed ? "KeyDown" : "KeyUp");
            lua_setfield(L, -2, "Event");
            lua_pushstring(L, macos_keycode_to_sdl_name(keyCode));
            lua_setfield(L, -2, "Key");
            lua_pushboolean(L, pressed);
            lua_setfield(L, -2, "Pressed");
            lua_pushboolean(L, 0);  // Repeat: not tracked from CGEventTap; treat as non-repeat
            lua_setfield(L, -2, "Repeat");
            // Modifiers: an ARRAY of SDL-style modifier NAME strings (MCM's
            // ExtractActiveModifiers does ipairs over it, so it MUST be a table,
            // not an integer — an integer errors and breaks keybinding dispatch).
            // Empty when no modifier is held (the common case, e.g. the toggle key).
            // Bits are CGEventFlags; we map to the left-hand variants.
            lua_newtable(L);
            int mi = 0;
            if (modifiers & 0x20000)  { lua_pushstring(L, "LSHIFT"); lua_rawseti(L, -2, ++mi); }
            if (modifiers & 0x40000)  { lua_pushstring(L, "LCTRL");  lua_rawseti(L, -2, ++mi); }
            if (modifiers & 0x80000)  { lua_pushstring(L, "LALT");   lua_rawseti(L, -2, ++mi); }
            lua_setfield(L, -2, "Modifiers");
            // Keep the raw keycode + character as extras (harmless; some code reads them).
            lua_pushinteger(L, keyCode);
            lua_setfield(L, -2, "KeyCode");
            if (character && character[0]) {
                lua_pushstring(L, character);
            } else {
                lua_pushnil(L);
            }
            lua_setfield(L, -2, "Character");

            // Key delivery is the whole keybinding path (MCM's window toggle
            // among them), and silence here is indistinguishable from "no key
            // was pressed".
            //
            // Budget the logging PER KEYCODE, not globally: a global cap is
            // spent by whatever the player happens to type first, and the one
            // keypress being investigated then never appears. Presses only -
            // releases double the volume and say nothing extra.
            if (pressed && keyCode >= 0 && keyCode < 256) {
                static uint8_t s_keySeen[256];
                if (s_keySeen[keyCode] < 3) {
                    s_keySeen[keyCode]++;
                    LOG_EVENTS_DEBUG("KeyInput -> %s (key=%s code=%d) handler=%llu",
                                    h->mod_name, macos_keycode_to_sdl_name(keyCode),
                                    keyCode, (unsigned long long)h->handler_id);
                }
            }

            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                const char *err = lua_tostring(L, -1);
                LOG_EVENTS_ERROR("KeyInput handler %llu (mod=%s) error: %s",
                           (unsigned long long)h->handler_id, h->mod_name, err ? err : "unknown");
                mod_health_record_error(h->mod_name, err);
                lua_pop(L, 1);
            } else {
                mod_health_record_success(h->mod_name);
            }

            if (h->once) {
                if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                    g_deferred_unsubs[g_deferred_unsub_count++] =
                        (DeferredUnsubscribe){EVENT_KEY_INPUT, h->handler_id};
                }
            }
        } else {
            lua_pop(L, 1);
        }

        event_scope_end(L);
    }

    g_dispatch_depth[EVENT_KEY_INPUT]--;

    if (g_dispatch_depth[EVENT_KEY_INPUT] == 0) {
        process_deferred_unsubscribes(L, EVENT_KEY_INPUT);
    }
}

// ============================================================================
// Public API: Fire DoConsoleCommand Event
// ============================================================================

bool events_fire_do_console_command(lua_State *L, const char *command) {
    if (!L) return false;

    int count = g_handler_counts[EVENT_DO_CONSOLE_COMMAND];
    if (count == 0) return false;

    LOG_EVENTS_DEBUG("Firing DoConsoleCommand (command=%s, %d handlers)", command, count);

    bool prevented = false;
    g_dispatch_depth[EVENT_DO_CONSOLE_COMMAND]++;

    for (int i = 0; i < g_handler_counts[EVENT_DO_CONSOLE_COMMAND]; i++) {
        EventHandler *h = &g_handlers[EVENT_DO_CONSOLE_COMMAND][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        // Skip soft-disabled mods
        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table with Command and Prevent fields
        lua_newtable(L);
        lua_pushstring(L, command);
        lua_setfield(L, -2, "Command");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "Prevent");

        // Keep reference to event table to check Prevent after call
        lua_pushvalue(L, -1);
        int event_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("DoConsoleCommand handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
            // Check if handler set Prevent = true
            lua_rawgeti(L, LUA_REGISTRYINDEX, event_ref);
            lua_getfield(L, -1, "Prevent");
            if (lua_toboolean(L, -1)) {
                prevented = true;
            }
            lua_pop(L, 2);  // Prevent value and event table
        }
        luaL_unref(L, LUA_REGISTRYINDEX, event_ref);
        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_DO_CONSOLE_COMMAND, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_DO_CONSOLE_COMMAND]--;

    if (g_dispatch_depth[EVENT_DO_CONSOLE_COMMAND] == 0) {
        process_deferred_unsubscribes(L, EVENT_DO_CONSOLE_COMMAND);
    }

    return prevented;
}

// ============================================================================
// Public API: Fire LuaConsoleInput Event
// ============================================================================

bool events_fire_lua_console_input(lua_State *L, const char *input) {
    if (!L) return false;

    int count = g_handler_counts[EVENT_LUA_CONSOLE_INPUT];
    if (count == 0) return false;

    LOG_EVENTS_DEBUG("Firing LuaConsoleInput (%d handlers)", count);

    bool prevented = false;
    g_dispatch_depth[EVENT_LUA_CONSOLE_INPUT]++;

    for (int i = 0; i < g_handler_counts[EVENT_LUA_CONSOLE_INPUT]; i++) {
        EventHandler *h = &g_handlers[EVENT_LUA_CONSOLE_INPUT][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table with Input and Prevent fields
        lua_newtable(L);
        lua_pushstring(L, input);
        lua_setfield(L, -2, "Input");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "Prevent");

        // Keep reference to event table to check Prevent after call
        lua_pushvalue(L, -1);
        int event_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("LuaConsoleInput handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
            // Check if handler set Prevent = true
            lua_rawgeti(L, LUA_REGISTRYINDEX, event_ref);
            lua_getfield(L, -1, "Prevent");
            if (lua_toboolean(L, -1)) {
                prevented = true;
            }
            lua_pop(L, 2);  // Prevent value and event table
        }
        luaL_unref(L, LUA_REGISTRYINDEX, event_ref);

        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_LUA_CONSOLE_INPUT, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_LUA_CONSOLE_INPUT]--;

    if (g_dispatch_depth[EVENT_LUA_CONSOLE_INPUT] == 0) {
        process_deferred_unsubscribes(L, EVENT_LUA_CONSOLE_INPUT);
    }

    return prevented;
}

// ============================================================================
// Public API: Get Handler Count
// ============================================================================

int events_get_handler_count(BG3SEEventType event) {
    if (event < 0 || event >= EVENT_MAX) return 0;
    return g_handler_counts[event];
}

// ============================================================================
// Public API: Get Event Name
// ============================================================================

const char *events_get_name(BG3SEEventType event) {
    if (event < 0 || event >= EVENT_MAX) return "Unknown";
    return g_event_names[event];
}

// ============================================================================
// Public API: Event Tracing
// ============================================================================

void events_set_trace_enabled(bool enabled) {
    g_trace_enabled = enabled;
    LOG_EVENTS_INFO("Event tracing %s", enabled ? "ENABLED" : "DISABLED");
}

bool events_get_trace_enabled(void) {
    return g_trace_enabled;
}

// ============================================================================
// Public API: Mod Health (for crash reports and !mod_diag)
// ============================================================================

int events_get_mod_health_count(void) {
    return g_mod_health_count;
}

const char *events_get_mod_health_name(int index) {
    if (index < 0 || index >= g_mod_health_count) return NULL;
    return g_mod_health[index].mod_name;
}

void events_get_mod_health_stats(int index, uint32_t *handlers, uint32_t *errors,
                                  uint32_t *handled, bool *disabled) {
    if (index < 0 || index >= g_mod_health_count) return;
    if (handlers) *handlers = g_mod_health[index].handlers_registered;
    if (errors) *errors = g_mod_health[index].errors_logged;
    if (handled) *handled = g_mod_health[index].events_handled;
    if (disabled) *disabled = g_mod_health[index].soft_disabled;
}

const char *events_get_mod_last_error(int index) {
    if (index < 0 || index >= g_mod_health_count) return NULL;
    if (g_mod_health[index].last_error[0] == '\0') return NULL;
    return g_mod_health[index].last_error;
}

bool events_set_mod_disabled(const char *mod_name, bool disabled) {
    for (int i = 0; i < g_mod_health_count; i++) {
        if (strcmp(g_mod_health[i].mod_name, mod_name) == 0) {
            g_mod_health[i].soft_disabled = disabled;
            LOG_EVENTS_INFO("Mod '%s' %s", mod_name,
                       disabled ? "DISABLED (soft)" : "ENABLED");
            return true;
        }
    }
    return false;
}

// ============================================================================
// Lua API: Subscribe
// ============================================================================

/**
 * Event:Subscribe(callback, [options])
 *
 * Options table:
 *   Priority: number (default 100, lower = called first)
 *   Once: boolean (default false, auto-unsubscribe after first call)
 *
 * Returns: handler ID (integer) for use with Unsubscribe
 */
static int lua_event_subscribe(lua_State *L) {
    // Event type from closure upvalue
    int event = (int)lua_tointeger(L, lua_upvalueindex(1));
    if (event < 0 || event >= EVENT_MAX) {
        return luaL_error(L, "Invalid event type");
    }

    // Callback is arg 2 (arg 1 is self due to colon syntax)
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // Parse options (arg 3, optional table)
    int priority = DEFAULT_PRIORITY;
    int once = 0;

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "Priority");
        if (lua_isnumber(L, -1)) {
            priority = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "Once");
        if (lua_isboolean(L, -1)) {
            once = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }

    // Check handler limit
    if (g_handler_counts[event] >= MAX_EVENT_HANDLERS) {
        return luaL_error(L, "Too many handlers for event %s (max %d)",
                         g_event_names[event], MAX_EVENT_HANDLERS);
    }

    // Store callback in registry
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Allocate handler
    uint64_t handler_id = g_next_handler_id++;
    int idx = g_handler_counts[event]++;

    g_handlers[event][idx].callback_ref = ref;
    g_handlers[event][idx].priority = priority;
    g_handlers[event][idx].once = once;
    g_handlers[event][idx].handler_id = handler_id;

    // Capture mod name from Lua callstack for crash attribution
    extract_mod_name_from_lua(L, g_handlers[event][idx].mod_name,
                              sizeof(g_handlers[event][idx].mod_name));

    // Track per-mod handler registration
    ModHealthEntry *health = mod_health_get_or_create(g_handlers[event][idx].mod_name);
    if (health) health->handlers_registered++;

    // Re-sort by priority
    sort_handlers_by_priority(event);

    // Log subscription (not for Tick - too noisy)
    if (event != EVENT_TICK) {
        LOG_EVENTS_DEBUG("Subscribed to %s (id=%llu, priority=%d, once=%d, mod=%s)",
                   g_event_names[event], handler_id, priority, once,
                   g_handlers[event][idx].mod_name);
    }

    // Return handler ID
    lua_pushinteger(L, (lua_Integer)handler_id);
    return 1;
}

// ============================================================================
// Lua API: Unsubscribe
// ============================================================================

/**
 * Event:Unsubscribe(handlerId)
 *
 * Returns: boolean (true if handler was found and removed)
 */
static int lua_event_unsubscribe(lua_State *L) {
    // Event type from closure upvalue
    int event = (int)lua_tointeger(L, lua_upvalueindex(1));
    if (event < 0 || event >= EVENT_MAX) {
        return luaL_error(L, "Invalid event type");
    }

    // Handler ID is arg 2 (arg 1 is self)
    uint64_t handler_id = (uint64_t)luaL_checkinteger(L, 2);

    // If currently dispatching, defer the removal
    if (g_dispatch_depth[event] > 0) {
        if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
            g_deferred_unsubs[g_deferred_unsub_count++] =
                (DeferredUnsubscribe){event, handler_id};
            lua_pushboolean(L, 1);  // Will be removed
        } else {
            LOG_EVENTS_WARN("Deferred unsubscribe queue full");
            lua_pushboolean(L, 0);
        }
        return 1;
    }

    // Immediate removal
    int found = remove_handler_by_id(L, event, handler_id);

    if (found && event != EVENT_TICK) {
        LOG_EVENTS_DEBUG("Unsubscribed from %s (id=%llu)",
                   g_event_names[event], handler_id);
    }

    lua_pushboolean(L, found);
    return 1;
}

// ============================================================================
// Lua API: OnNextTick
// ============================================================================

/**
 * Ext.OnNextTick(callback)
 *
 * Convenience function to subscribe to Tick with Once=true.
 * Returns: handler ID (can be used to cancel before it fires)
 */
static int lua_on_next_tick(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);

    // Check handler limit
    if (g_handler_counts[EVENT_TICK] >= MAX_EVENT_HANDLERS) {
        return luaL_error(L, "Too many Tick handlers");
    }

    // Store callback in registry
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Allocate handler with Once=true
    uint64_t handler_id = g_next_handler_id++;
    int idx = g_handler_counts[EVENT_TICK]++;

    g_handlers[EVENT_TICK][idx].callback_ref = ref;
    g_handlers[EVENT_TICK][idx].priority = DEFAULT_PRIORITY;
    g_handlers[EVENT_TICK][idx].once = 1;  // Auto-unsubscribe
    g_handlers[EVENT_TICK][idx].handler_id = handler_id;

    // Re-sort by priority
    sort_handlers_by_priority(EVENT_TICK);

    // Return handler ID
    lua_pushinteger(L, (lua_Integer)handler_id);
    return 1;
}

// ============================================================================
// Internal: Create Event Object
// ============================================================================

static void create_event_object(lua_State *L, BG3SEEventType event) {
    lua_newtable(L);

    // Subscribe method with event type as upvalue
    lua_pushinteger(L, event);
    lua_pushcclosure(L, lua_event_subscribe, 1);
    lua_setfield(L, -2, "Subscribe");

    // Unsubscribe method with event type as upvalue
    lua_pushinteger(L, event);
    lua_pushcclosure(L, lua_event_unsubscribe, 1);
    lua_setfield(L, -2, "Unsubscribe");
}

// ============================================================================
// Public API: Register Lua API
// ============================================================================

void lua_events_register(lua_State *L, int ext_table_index) {
    // Initialize event system
    events_init();

    // Convert negative index to absolute
    if (ext_table_index < 0) {
        ext_table_index = lua_gettop(L) + ext_table_index + 1;
    }

    // Create Ext.Events table
    lua_newtable(L);

    // Register all events
    for (int i = 0; i < EVENT_MAX; i++) {
        create_event_object(L, i);
        lua_setfield(L, -2, g_event_names[i]);
    }

    lua_setfield(L, ext_table_index, "Events");

    // Register Ext.OnNextTick
    lua_pushcfunction(L, lua_on_next_tick);
    lua_setfield(L, ext_table_index, "OnNextTick");

    LOG_EVENTS_INFO("Ext.Events namespace registered with %d event types", EVENT_MAX);

    // Initialize Log event callback with the logging system
    events_init_log_callback();
}

// ============================================================================
// Engine Events - Fire Functions (Issue #51)
// ============================================================================

void events_fire_turn_started(lua_State *L, uint64_t entity, int round) {
    if (!L) return;

    int count = g_handler_counts[EVENT_TURN_STARTED];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing TurnStarted (entity=0x%llx, round=%d, %d handlers)",
                (unsigned long long)entity, round, count);

    g_dispatch_depth[EVENT_TURN_STARTED]++;

    for (int i = 0; i < g_handler_counts[EVENT_TURN_STARTED]; i++) {
        EventHandler *h = &g_handlers[EVENT_TURN_STARTED][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)entity);
        lua_setfield(L, -2, "Entity");
        lua_pushinteger(L, round);
        lua_setfield(L, -2, "Round");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("TurnStarted handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_TURN_STARTED, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_TURN_STARTED]--;

    if (g_dispatch_depth[EVENT_TURN_STARTED] == 0) {
        process_deferred_unsubscribes(L, EVENT_TURN_STARTED);
    }
}

// ============================================================================
// Osiris Bridge Events (Issue #51 - TurnStarted/TurnEnded from Osiris)
// ============================================================================

void events_fire_turn_started_from_osiris(lua_State *L, const char *characterGuid) {
    if (!L) return;

    int count = g_handler_counts[EVENT_TURN_STARTED];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing TurnStarted from Osiris (guid=%s, %d handlers)",
                characterGuid ? characterGuid : "nil", count);

    g_dispatch_depth[EVENT_TURN_STARTED]++;

    for (int i = 0; i < g_handler_counts[EVENT_TURN_STARTED]; i++) {
        EventHandler *h = &g_handlers[EVENT_TURN_STARTED][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table with CharacterGuid
        lua_newtable(L);
        if (characterGuid) {
            lua_pushstring(L, characterGuid);
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "CharacterGuid");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("TurnStarted (Osiris) handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_TURN_STARTED, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_TURN_STARTED]--;

    if (g_dispatch_depth[EVENT_TURN_STARTED] == 0) {
        process_deferred_unsubscribes(L, EVENT_TURN_STARTED);
    }
}

void events_fire_turn_ended_from_osiris(lua_State *L, const char *characterGuid) {
    if (!L) return;

    int count = g_handler_counts[EVENT_TURN_ENDED];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing TurnEnded from Osiris (guid=%s, %d handlers)",
                characterGuid ? characterGuid : "nil", count);

    g_dispatch_depth[EVENT_TURN_ENDED]++;

    for (int i = 0; i < g_handler_counts[EVENT_TURN_ENDED]; i++) {
        EventHandler *h = &g_handlers[EVENT_TURN_ENDED][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table with CharacterGuid
        lua_newtable(L);
        if (characterGuid) {
            lua_pushstring(L, characterGuid);
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "CharacterGuid");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("TurnEnded (Osiris) handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_TURN_ENDED, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_TURN_ENDED]--;

    if (g_dispatch_depth[EVENT_TURN_ENDED] == 0) {
        process_deferred_unsubscribes(L, EVENT_TURN_ENDED);
    }
}

void events_fire_status_applied(lua_State *L, uint64_t entity, const char *statusId, uint64_t source) {
    if (!L) return;

    int count = g_handler_counts[EVENT_STATUS_APPLIED];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing StatusApplied (entity=0x%llx, status=%s, source=0x%llx, %d handlers)",
                (unsigned long long)entity, statusId ? statusId : "nil",
                (unsigned long long)source, count);

    g_dispatch_depth[EVENT_STATUS_APPLIED]++;

    for (int i = 0; i < g_handler_counts[EVENT_STATUS_APPLIED]; i++) {
        EventHandler *h = &g_handlers[EVENT_STATUS_APPLIED][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)entity);
        lua_setfield(L, -2, "Entity");
        lua_pushstring(L, statusId ? statusId : "");
        lua_setfield(L, -2, "StatusId");
        lua_pushinteger(L, (lua_Integer)source);
        lua_setfield(L, -2, "Source");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("StatusApplied handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_STATUS_APPLIED, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_STATUS_APPLIED]--;

    if (g_dispatch_depth[EVENT_STATUS_APPLIED] == 0) {
        process_deferred_unsubscribes(L, EVENT_STATUS_APPLIED);
    }
}

// ============================================================================
// Functor Events (Issue #53)
// ============================================================================

/*
 * Lifetime-scoped handle to the engine-owned StatsFunctorList a functor event
 * is dispatching. Valid only inside that dispatch (functor_dispatch_valid);
 * Ext.Stats.ExecuteFunctors refuses stale ones. Pushed ALONGSIDE the legacy
 * FunctorListPtr integer, which stays for compatibility.
 */
typedef struct {
    const void *list;
    const void *engine_ctx;   /* the LIVE context this dispatch is running with */
    int ctx_type;
    uint64_t seq;
} LuaFunctorListUD;

#define FUNCTOR_LIST_MT "bg3se.FunctorList"

static void push_functor_list_ud(lua_State *L, const void *functors,
                                 const void *engine_ctx, int ctxType,
                                 uint64_t seq) {
    LuaFunctorListUD *ud =
        (LuaFunctorListUD *)lua_newuserdatauv(L, sizeof *ud, 0);
    ud->list = functors;
    ud->engine_ctx = engine_ctx;
    ud->ctx_type = ctxType;
    ud->seq = seq;
    if (luaL_newmetatable(L, FUNCTOR_LIST_MT)) {
        /* metatable is a name tag only; no methods */
    }
    lua_setmetatable(L, -2);
}

void events_fire_execute_functor(lua_State *L, int ctxType, void *functors, void *context) {
    if (!L) return;

    int count = g_handler_counts[EVENT_EXECUTE_FUNCTOR];
    if (count == 0) return;

    uint64_t __dispatch_seq = functor_dispatch_begin();

    LOG_EVENTS_DEBUG("Firing ExecuteFunctor (ctx=%d, functors=%p, context=%p, %d handlers)",
                ctxType, functors, context, count);

    g_dispatch_depth[EVENT_EXECUTE_FUNCTOR]++;

    for (int i = 0; i < g_handler_counts[EVENT_EXECUTE_FUNCTOR]; i++) {
        EventHandler *h = &g_handlers[EVENT_EXECUTE_FUNCTOR][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table
        lua_newtable(L);
        lua_pushinteger(L, ctxType);
        lua_setfield(L, -2, "ContextType");
        lua_pushinteger(L, (lua_Integer)(uintptr_t)functors);
        lua_setfield(L, -2, "FunctorListPtr");
        push_functor_list_ud(L, functors, context, ctxType, __dispatch_seq);
        lua_setfield(L, -2, "FunctorList");
        lua_pushinteger(L, (lua_Integer)(uintptr_t)context);
        lua_setfield(L, -2, "ContextPtr");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("ExecuteFunctor handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_EXECUTE_FUNCTOR, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_EXECUTE_FUNCTOR]--;

    if (g_dispatch_depth[EVENT_EXECUTE_FUNCTOR] == 0) {
        process_deferred_unsubscribes(L, EVENT_EXECUTE_FUNCTOR);
    }

    functor_dispatch_end();
}

void events_fire_after_execute_functor(lua_State *L, int ctxType, void *functors, void *context) {
    if (!L) return;

    int count = g_handler_counts[EVENT_AFTER_EXECUTE_FUNCTOR];
    if (count == 0) return;

    uint64_t __dispatch_seq = functor_dispatch_begin();

    LOG_EVENTS_DEBUG("Firing AfterExecuteFunctor (ctx=%d, %d handlers)", ctxType, count);

    g_dispatch_depth[EVENT_AFTER_EXECUTE_FUNCTOR]++;

    for (int i = 0; i < g_handler_counts[EVENT_AFTER_EXECUTE_FUNCTOR]; i++) {
        EventHandler *h = &g_handlers[EVENT_AFTER_EXECUTE_FUNCTOR][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table
        lua_newtable(L);
        lua_pushinteger(L, ctxType);
        lua_setfield(L, -2, "ContextType");
        lua_pushinteger(L, (lua_Integer)(uintptr_t)functors);
        lua_setfield(L, -2, "FunctorListPtr");
        push_functor_list_ud(L, functors, context, ctxType, __dispatch_seq);
        lua_setfield(L, -2, "FunctorList");
        lua_pushinteger(L, (lua_Integer)(uintptr_t)context);
        lua_setfield(L, -2, "ContextPtr");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("AfterExecuteFunctor handler error (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_AFTER_EXECUTE_FUNCTOR, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_AFTER_EXECUTE_FUNCTOR]--;

    if (g_dispatch_depth[EVENT_AFTER_EXECUTE_FUNCTOR] == 0) {
        process_deferred_unsubscribes(L, EVENT_AFTER_EXECUTE_FUNCTOR);
    }

    functor_dispatch_end();
}

static void set_pointer_field(lua_State *L, const char *field, const void *ptr) {
    if (ptr) {
        lua_pushinteger(L, (lua_Integer)(uintptr_t)ptr);
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, field);
}

static void set_nil_field(lua_State *L, const char *field) {
    lua_pushnil(L);
    lua_setfield(L, -2, field);
}

/*
 * Windows hands Lua a bound userdata for each of these engine objects. We have
 * the verified address (it is a hook argument) but no independently verified
 * member layout on this build, so the value is a table carrying only `Ptr`.
 * That keeps `event.Functor` / `event.Hit` indexable — a mod written against
 * the documented API indexes them on its first line, which is precisely what
 * used to raise "attempt to index a nil value (field 'Functor')" — while any
 * member read returns nil instead of a number decoded from a guessed offset.
 */
static void set_opaque_object_field(lua_State *L, const char *field, const void *ptr) {
    if (!ptr) {
        lua_pushnil(L);
        lua_setfield(L, -2, field);
        return;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(uintptr_t)ptr);
    lua_setfield(L, -2, "Ptr");
    lua_setfield(L, -2, field);
}

static void set_position_field(lua_State *L, const char *field, const void *position) {
    float xyz[3];
    if (!position ||
        !safe_memory_read((mach_vm_address_t)(uintptr_t)position, xyz, sizeof xyz)) {
        lua_pushnil(L);
        lua_setfield(L, -2, field);
        return;
    }
    lua_newtable(L);
    static const char *const kAxis[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; i++) {
        lua_pushnumber(L, (lua_Number)xyz[i]);
        lua_setfield(L, -2, kAxis[i]);
        lua_pushnumber(L, (lua_Number)xyz[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, field);
}

/* ---------------------------------------------------------------------------
 * Decoders for the DealDamage payload's object-valued members.
 *
 * Every offset used below comes from src/stats/deal_damage_layout.h, which
 * derives them from the 4.1.1.7398727 arm64 slice; nothing here is taken from
 * the Windows headers. Members whose offset or label table could not be
 * derived on that binary are left absent rather than filled in from a
 * plausible guess — a nil field is recoverable, a wrong decode silently feeds
 * mods a number that looks real.
 *
 * Each engine object is snapshotted with ONE safe_memory_read and then decoded
 * out of the local copy. Never touch game memory field by field here:
 * safe_memory_read is a mach_vm_read_overwrite syscall, DealDamage fires once
 * per damage instance per target, and this runs on the game thread inside a
 * Dobby trampoline. Per-field reads would turn one AoE into a few hundred
 * syscalls. Reading through the snapshot also means every field of an object
 * comes from the same instant.
 * ------------------------------------------------------------------------- */

/* Unaligned-safe scalar loads out of a snapshot buffer. */
static uint8_t buf_u8(const uint8_t *b, uint32_t off) { return b[off]; }

static int32_t buf_i32(const uint8_t *b, uint32_t off) {
    int32_t v; memcpy(&v, b + off, sizeof v); return v;
}

static uint32_t buf_u32(const uint8_t *b, uint32_t off) {
    uint32_t v; memcpy(&v, b + off, sizeof v); return v;
}

static uint64_t buf_u64(const uint8_t *b, uint32_t off) {
    uint64_t v; memcpy(&v, b + off, sizeof v); return v;
}

static float buf_f32(const uint8_t *b, uint32_t off) {
    float v; memcpy(&v, b + off, sizeof v); return v;
}

/* Look up the label a value carries in one of this build's own enum tables.
 * Returns NULL when the type is not registered or the value has no entry —
 * callers then leave the field absent rather than pushing a raw number under
 * a name the documented API says is a string. */
static const char *event_enum_label(const char *type_name, uint64_t value) {
    EnumTypeInfo *info = enum_registry_find_by_name(type_name);
    if (!info) return NULL;
    return enum_find_label(info->registry_index, value);
}

static void set_enum_byte_at(lua_State *L, const char *field,
                             const uint8_t *b, uint32_t off,
                             const char *enum_type) {
    const char *label = event_enum_label(enum_type, buf_u8(b, off));
    if (!label) return;
    lua_pushstring(L, label);
    lua_setfield(L, -2, field);
}

static void set_i32_at(lua_State *L, const char *field,
                       const uint8_t *b, uint32_t off) {
    lua_pushinteger(L, (lua_Integer)buf_i32(b, off));
    lua_setfield(L, -2, field);
}

static void set_float_at(lua_State *L, const char *field,
                         const uint8_t *b, uint32_t off) {
    lua_pushnumber(L, (lua_Number)buf_f32(b, off));
    lua_setfield(L, -2, field);
}

/* FixedString members are a 4-byte global-string-table index. 0xFFFFFFFF is
 * the null index; fixed_string_resolve also returns NULL before the GST is
 * available, in which case the field stays absent rather than becoming "". */
static void set_fixedstring_at(lua_State *L, const char *field,
                               const uint8_t *b, uint32_t off) {
    uint32_t idx = buf_u32(b, off);
    if (!fixed_string_is_valid(idx)) return;
    const char *s = fixed_string_resolve(idx);
    if (!s) return;
    lua_pushstring(L, s);
    lua_setfield(L, -2, field);
}

/* ls::Guid is the same 16 raw bytes the entity system reads out of
 * ls::uuid::Component, so guid_to_string produces the spelling Osiris and
 * Ext.Entity.HandleToUuid use. */
static void set_guid_at(lua_State *L, const char *field,
                        const uint8_t *b, uint32_t off) {
    Guid g;
    memcpy(&g, b + off, sizeof g);
    char str[40];
    guid_to_string(&g, str);
    lua_pushstring(L, str);
    lua_setfield(L, -2, field);
}

static void set_vec3_at(lua_State *L, const char *field,
                        const uint8_t *b, uint32_t off) {
    lua_newtable(L);
    static const char *const kAxis[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; i++) {
        float v = buf_f32(b, off + (uint32_t)(i * 4));
        lua_pushnumber(L, (lua_Number)v);
        lua_setfield(L, -2, kAxis[i]);
        lua_pushnumber(L, (lua_Number)v);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, field);
}

/*
 * Push the same entity object Ext.Entity.Get returns, so `caster.Uuid.EntityUuid`
 * resolves through the ordinary component path instead of a second, divergent
 * entity shape invented here. EntityUserdata + the "BG3Entity" metatable is the
 * whole contract (src/entity/entity_system.h); the lifetime handle must be the
 * scope that is current now, which event_scope_begin() opened for this handler.
 *
 * Returns false (pushing nothing) rather than wrapping a handle that will not
 * resolve — see the guard below.
 */
static bool push_entity_object(lua_State *L, uint64_t handle) {
    if (!handle || !entity_handle_is_valid((EntityHandle)handle)) return false;
    /*
     * entity_handle_is_valid() only rejects all-ones. The engine's own "unset"
     * handle on 7398727 is 0xFFC0000000000000 and it guards every use of a
     * handle with two tests, both visible in the hooked function itself
     * (StatsFunctorDealDamage::Execute @0x1057736d4):
     *     lsr x9, x8, #54 ; cmp w9, #0x40 ; b.hi   <- type index out of range
     *     cmp x8, #-0x40000000000000 ; b.eq        <- the unset sentinel
     * (0xFFC0000000000000 >> 54 is 0x3ff, so the range test covers both.)
     * Wrapping an unset handle would not fail here, it would fail two field
     * reads later as `attempt to index a nil value (field 'Uuid')` inside the
     * mod. A nil Caster is what the documented `if caster ~= nil` guard is for.
     */
    if ((handle >> 54) > 0x40) return false;
    if (!entity_system_ready()) return false;

    EntityUserdata *ud = (EntityUserdata *)lua_newuserdata(L, sizeof(EntityUserdata));
    ud->handle = (EntityHandle)handle;
    ud->lifetime = lifetime_lua_get_current(L);
    luaL_getmetatable(L, "BG3Entity");
    if (!lua_istable(L, -1)) {
        /* No metatable registered in this VM: a bare userdata raises on its
         * first index, which is strictly worse for a mod than nil. */
        lua_pop(L, 2);
        return false;
    }
    lua_setmetatable(L, -2);
    return true;
}

/* Sets `field` to the entity object and `handle_field` to the raw 64-bit
 * handle, so the plain integer this payload used to carry under `field` stays
 * reachable. */
static void set_entity_object_from_handle(lua_State *L, const char *field,
                                          const char *handle_field,
                                          uint64_t handle) {
    lua_pushinteger(L, (lua_Integer)handle);
    lua_setfield(L, -2, handle_field);
    if (!push_entity_object(L, handle)) lua_pushnil(L);
    lua_setfield(L, -2, field);
}

/*
 * `ref` is either an ecs::EntityRef ({EntityHandle Handle; EntityWorld* World;})
 * or a bare ls::ID<EntityHandleTraits>; the handle is at offset 0 in both. The
 * +0/+8 EntityRef layout is documented in functor_types.h and re-confirmed on
 * 7398727 inside the hooked function itself, which does `ldp x1, x0, [ref]` to
 * feed EntityWorld::GetComponent(world, handle).
 */
static void set_entity_object_field(lua_State *L, const char *field,
                                    const char *handle_field, const void *ref) {
    uint64_t handle = 0;
    if (!ref || !safe_memory_read_u64((mach_vm_address_t)(uintptr_t)ref, &handle)) {
        lua_pushnil(L);
        lua_setfield(L, -2, field);
        return;
    }
    set_entity_object_from_handle(L, field, handle_field, handle);
}

/*
 * A DynamicArray<TDamagePair> whose element pointer and count have already been
 * decoded out of the owning object's snapshot. The count comes from live
 * memory, so it is clamped: a torn or stale read must cost a bounded number of
 * reads, not a stall on the game thread inside a Lua callback.
 */
static void set_damage_list_field(lua_State *L, const char *field,
                                  const void *elements, int32_t count) {
    if (count < 0) count = 0;
    if (count > DAMAGE_LIST_MAX_ELEMENTS) count = DAMAGE_LIST_MAX_ELEMENTS;

    uint8_t pairs[DAMAGE_LIST_MAX_ELEMENTS * DAMAGE_PAIR_SIZE];
    if (!elements || count == 0 ||
        !safe_memory_read((mach_vm_address_t)(uintptr_t)elements, pairs,
                          (size_t)count * DAMAGE_PAIR_SIZE)) {
        count = 0;
    }

    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        const uint8_t *e = pairs + (size_t)i * DAMAGE_PAIR_SIZE;
        lua_newtable(L);
        set_i32_at(L, "Amount", e, DAMAGE_PAIR_OFF_AMOUNT);
        set_enum_byte_at(L, "DamageType", e, DAMAGE_PAIR_OFF_DAMAGE_TYPE,
                         "DamageType");
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, field);
}

/*
 * eoc::SpellPrototype head. Only the four members read off this build are
 * exposed; SpellTypeId stays a raw number because no label table for it was
 * derived here, and it is named ...Id rather than ...Type to say so.
 */
static void push_spell_prototype(lua_State *L, uintptr_t proto) {
    uint8_t b[SPELL_PROTOTYPE_OFF_SPELL_FLAGS + 8];
    if (!safe_memory_read((mach_vm_address_t)proto, b, sizeof b)) {
        lua_pushnil(L);
        return;
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)proto);
    lua_setfield(L, -2, "Ptr");

    set_i32_at(L, "StatsObjectIndex", b, SPELL_PROTOTYPE_OFF_STATS_OBJECT_INDEX);
    set_i32_at(L, "SpellTypeId", b, SPELL_PROTOTYPE_OFF_SPELL_TYPE_ID);
    /* The prototype's own stats name. Windows calls the member SpellId; it is
     * also published as Name because it is the same string SpellId.Prototype
     * carries. */
    set_fixedstring_at(L, "SpellId", b, SPELL_PROTOTYPE_OFF_SPELL_ID);
    set_fixedstring_at(L, "Name", b, SPELL_PROTOTYPE_OFF_SPELL_ID);

    uint64_t flags = buf_u64(b, SPELL_PROTOTYPE_OFF_SPELL_FLAGS);
    lua_pushinteger(L, (lua_Integer)flags);
    lua_setfield(L, -2, "SpellFlagsValue");

    /* An array of flag names, which is the shape mods iterate:
     *   for _, flag in ipairs(proto.SpellFlags) do ... end
     * One label per set bit from this build's own SpellFlags table
     * (enum_find_label returns the game's spelling, not the Windows alias). A
     * bit the table does not name contributes nothing rather than a
     * fabricated label. */
    EnumTypeInfo *info = enum_registry_find_by_name("SpellFlags");
    lua_newtable(L);
    if (info) {
        int n = 0;
        for (int bit = 0; bit < 64; bit++) {
            uint64_t mask = 1ULL << bit;
            if (!(flags & mask)) continue;
            const char *label = enum_find_label(info->registry_index, mask);
            if (!label) continue;
            lua_pushstring(L, label);
            lua_rawseti(L, -2, ++n);
        }
    }
    lua_setfield(L, -2, "SpellFlags");
}

/*
 * eoc::spell::SpellId, and its SpellInfo subclass (Windows
 * SpellIdWithPrototype) when `with_prototype` is set. Member names are the
 * game's own, taken from the file-static FixedStrings its savegame visitor
 * keys each field with.
 *
 * SourceType is deliberately absent: the byte at +0x08 is verified, but this
 * build ships no name table for eoc::spell::ESourceType that was derived here.
 */
static void set_spell_id_field(lua_State *L, const char *field,
                               const void *ptr, bool with_prototype) {
    uint8_t b[SPELL_INFO_SIZE];
    size_t want = with_prototype ? (size_t)SPELL_INFO_SIZE : (size_t)SPELL_ID_SIZE;
    if (!ptr || !safe_memory_read((mach_vm_address_t)(uintptr_t)ptr, b, want)) {
        lua_pushnil(L);
        lua_setfield(L, -2, field);
        return;
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(uintptr_t)ptr);
    lua_setfield(L, -2, "Ptr");

    set_fixedstring_at(L, "OriginatorPrototype", b,
                       SPELL_ID_OFF_ORIGINATOR_PROTOTYPE);
    set_guid_at(L, "Source", b, SPELL_ID_OFF_SOURCE);
    set_guid_at(L, "ProgressionSource", b, SPELL_ID_OFF_PROGRESSION_SOURCE);
    set_fixedstring_at(L, "Prototype", b, SPELL_ID_OFF_PROTOTYPE);

    if (with_prototype) {
        uintptr_t proto = (uintptr_t)buf_u64(b, SPELL_INFO_OFF_SPELL_PROTO);
        if (proto) {
            push_spell_prototype(L, proto);
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "SpellProto");
    }

    lua_setfield(L, -2, field);
}

/*
 * eoc::HitDesc. The members whose offsets came out of
 * ecs::sync::Deserialize<eoc::HitDesc, ...> are exposed; DeathType, CauseType,
 * HitWith, SpellAttackType and the two flag words are NOT, because this build
 * ships no derived label table for those enums (only the upper-cased khonsu
 * scripting spellings, which no mod compares against) and the documented API
 * says they are strings. `Ptr` stays so existing introspection keeps working
 * and a raw read from Lua is still possible.
 */
static void set_hit_desc_field(lua_State *L, const char *field, const void *ptr) {
    uint8_t b[HIT_DESC_SIZE];
    if (!ptr || !safe_memory_read((mach_vm_address_t)(uintptr_t)ptr, b, sizeof b)) {
        lua_pushnil(L);
        lua_setfield(L, -2, field);
        return;
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(uintptr_t)ptr);
    lua_setfield(L, -2, "Ptr");

    set_i32_at(L, "TotalDamageDone", b, HIT_DESC_OFF_TOTAL_DAMAGE_DONE);
    set_i32_at(L, "TotalHealDone", b, HIT_DESC_OFF_TOTAL_HEAL_DONE);
    set_i32_at(L, "ArmorAbsorption", b, HIT_DESC_OFF_ARMOR_ABSORPTION);
    set_i32_at(L, "LifeSteal", b, HIT_DESC_OFF_LIFE_STEAL);
    set_i32_at(L, "SpellLevel", b, HIT_DESC_OFF_SPELL_LEVEL);
    set_i32_at(L, "SpellPowerLevel", b, HIT_DESC_OFF_SPELL_POWER_LEVEL);
    set_float_at(L, "ImpactForce", b, HIT_DESC_OFF_IMPACT_FORCE);

    set_enum_byte_at(L, "DamageType", b, HIT_DESC_OFF_MAIN_DAMAGE_TYPE,
                     "DamageType");
    set_enum_byte_at(L, "AttackRollAbility", b, HIT_DESC_OFF_ATTACK_ABILITY,
                     "AbilityId");
    set_enum_byte_at(L, "SaveAbility", b, HIT_DESC_OFF_SAVE_ABILITY,
                     "AbilityId");

    set_fixedstring_at(L, "SpellId", b, HIT_DESC_OFF_SPELL_ID);

    set_vec3_at(L, "ImpactPosition", b, HIT_DESC_OFF_IMPACT_POSITION);
    set_vec3_at(L, "ImpactDirection", b, HIT_DESC_OFF_IMPACT_DIRECTION);

    set_entity_object_from_handle(L, "Inflicter", "InflicterHandle",
                                  buf_u64(b, HIT_DESC_OFF_INFLICTER));
    set_entity_object_from_handle(L, "InflicterOwner", "InflicterOwnerHandle",
                                  buf_u64(b, HIT_DESC_OFF_INFLICTER_OWNER));
    set_entity_object_from_handle(L, "Throwing", "ThrowingHandle",
                                  buf_u64(b, HIT_DESC_OFF_THROWN_OBJECT));

    set_damage_list_field(L, "DamageList",
                          (const void *)(uintptr_t)buf_u64(
                              b, HIT_DESC_OFF_DAMAGE_LIST_ELEMENTS),
                          buf_i32(b, HIT_DESC_OFF_DAMAGE_LIST_SIZE));

    lua_setfield(L, -2, field);
}

/* eoc::AttackDesc — two totals and the accumulated per-type damage list. */
static void set_attack_desc_field(lua_State *L, const char *field, const void *ptr) {
    uint8_t b[ATTACK_DESC_SIZE];
    if (!ptr || !safe_memory_read((mach_vm_address_t)(uintptr_t)ptr, b, sizeof b)) {
        lua_pushnil(L);
        lua_setfield(L, -2, field);
        return;
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(uintptr_t)ptr);
    lua_setfield(L, -2, "Ptr");

    set_i32_at(L, "TotalDamageDone", b, ATTACK_DESC_OFF_TOTAL_DAMAGE_DONE);
    set_i32_at(L, "TotalHealDone", b, ATTACK_DESC_OFF_TOTAL_HEAL_DONE);
    set_damage_list_field(L, "DamageList",
                          (const void *)(uintptr_t)buf_u64(
                              b, ATTACK_DESC_OFF_DAMAGE_LIST_ELEMENTS),
                          buf_i32(b, ATTACK_DESC_OFF_DAMAGE_LIST_SIZE));

    lua_setfield(L, -2, field);
}
static void push_deal_damage_payload(lua_State *L, BG3SEEventType event,
                                     const DealDamageEventData *d) {
    lua_newtable(L);

    /* The hook target is StatsFunctorDealDamage::Execute, so the functor is a
     * DealDamage functor by construction. TypeId therefore comes from the hook
     * site, not from a read through StatsFunctorBase+0x3C. */
    set_opaque_object_field(L, "Functor", d->functor);
    lua_getfield(L, -1, "Functor");
    if (lua_istable(L, -1)) {
        lua_pushinteger(L, FUNCTOR_ID_DEAL_DAMAGE);
        lua_setfield(L, -2, "TypeId");
        lua_pushstring(L, "DealDamage");
        lua_setfield(L, -2, "Type");
        /*
         * Read-only. Windows lets a handler assign Functor.DamageType, but the
         * functor here is the one parsed once out of the stats entry and shared
         * by every future execution of that spell, so a write would not be
         * scoped to this hit — it would repaint the stat permanently. Assigning
         * to this key therefore only updates the Lua table.
         */
        uint8_t dt = 0;
        if (safe_memory_read_u8((mach_vm_address_t)((uintptr_t)d->functor +
                                    DEAL_DAMAGE_FUNCTOR_OFF_DAMAGE_TYPE), &dt)) {
            set_enum_byte_at(L, "DamageType", &dt, 0, "DamageType");
        }
    }
    lua_pop(L, 1);

    set_entity_object_field(L, "Caster", "CasterHandle", d->casterRef);
    set_entity_object_field(L, "Target", "TargetHandle", d->targetRef);
    /* Windows' Caster2 — the separate source handle argument, not a second
     * read of Caster. */
    set_entity_object_field(L, "Caster2", "Caster2Handle", d->sourceHandle2);
    set_position_field(L, "Position", d->position);

    lua_pushboolean(L, d->isFromItem);
    lua_setfield(L, -2, "IsFromItem");
    lua_pushinteger(L, (lua_Integer)d->storyActionId);
    lua_setfield(L, -2, "StoryActionId");
    lua_pushinteger(L, (lua_Integer)d->hitWith);
    lua_setfield(L, -2, "HitWith");
    lua_pushinteger(L, (lua_Integer)d->conditionRollIndex);
    lua_setfield(L, -2, "ConditionRollIndex");

    /* p6 is eoc::spell::SpellInfo (Windows SpellIdWithPrototype), p17 is a
     * plain eoc::spell::SpellId — only the former carries SpellProto. */
    set_spell_id_field(L, "SpellId", d->spellId, true);
    set_spell_id_field(L, "SpellId2", d->spellId2, false);
    set_opaque_object_field(L, "Originator", d->originator);
    set_hit_desc_field(L, "Hit", d->hit);
    set_attack_desc_field(L, "Attack", d->attack);

    /* Windows carries Result on DealtDamage only; before the original runs the
     * output object is uninitialized, so exposing it would be a lie. */
    if (event == EVENT_DEALT_DAMAGE) {
        /* HitResult is HitDesc at +0 followed by AttackDesc at +0x1a8
         * (ghidra/offsets/DEALDAMAGE_HOOKS.md), so the same two decoders apply
         * to the halves of the output object. */
        set_opaque_object_field(L, "Result", d->result);
        lua_getfield(L, -1, "Result");
        if (lua_istable(L, -1) && d->result) {
            set_hit_desc_field(L, "Hit", d->result);
            set_attack_desc_field(L, "Attack",
                                  (const void *)((uintptr_t)d->result + HIT_DESC_SIZE));
        }
        lua_pop(L, 1);
    } else {
        set_nil_field(L, "Result");
    }
}

void events_fire_deal_damage(lua_State *L, BG3SEEventType event,
                             const DealDamageEventData *d) {
    if (!L || !d || (event != EVENT_DEAL_DAMAGE && event != EVENT_DEALT_DAMAGE)) {
        return;
    }

    int count = g_handler_counts[event];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing %s (functor=%p, %d handlers)",
                     g_event_names[event], d->functor, count);

    g_dispatch_depth[event]++;

    for (int i = 0; i < g_handler_counts[event]; i++) {
        EventHandler *h = &g_handlers[event][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);
        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        push_deal_damage_payload(L, event, d);

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("%s handler error (id=%llu, mod=%s): %s",
                             g_event_names[event], h->handler_id, h->mod_name,
                             err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);
        if (h->once && g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
            g_deferred_unsubs[g_deferred_unsub_count++] =
                (DeferredUnsubscribe){event, h->handler_id};
        }
    }

    g_dispatch_depth[event]--;
    if (g_dispatch_depth[event] == 0) {
        process_deferred_unsubscribes(L, event);
    }
}

void events_fire_before_deal_damage(lua_State *L, void *statsSystem,
                                    const void *hit, const void *attack) {
    if (!L) return;

    int count = g_handler_counts[EVENT_BEFORE_DEAL_DAMAGE];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing BeforeDealDamage (hit=%p, attack=%p, %d handlers)",
                     hit, attack, count);

    g_dispatch_depth[EVENT_BEFORE_DEAL_DAMAGE]++;

    for (int i = 0; i < g_handler_counts[EVENT_BEFORE_DEAL_DAMAGE]; i++) {
        EventHandler *h = &g_handlers[EVENT_BEFORE_DEAL_DAMAGE][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);
        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        lua_newtable(L);
        set_hit_desc_field(L, "Hit", hit);
        set_attack_desc_field(L, "Attack", attack);
        /* Not part of the Windows payload; exposed as a plain address for
         * diagnostics, matching the *Ptr convention used elsewhere here. */
        set_pointer_field(L, "StatsSystemPtr", statsSystem);

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("BeforeDealDamage handler error (id=%llu, mod=%s): %s",
                             h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);
        if (h->once && g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
            g_deferred_unsubs[g_deferred_unsub_count++] =
                (DeferredUnsubscribe){EVENT_BEFORE_DEAL_DAMAGE, h->handler_id};
        }
    }

    g_dispatch_depth[EVENT_BEFORE_DEAL_DAMAGE]--;
    if (g_dispatch_depth[EVENT_BEFORE_DEAL_DAMAGE] == 0) {
        process_deferred_unsubscribes(L, EVENT_BEFORE_DEAL_DAMAGE);
    }
}

// ============================================================================
// NetModMessage Event (Issue #6)
// ============================================================================

void events_fire_net_mod_message(lua_State *L, const char *channel, const char *payload,
                                  const char *module, int userId, uint64_t requestId,
                                  uint64_t replyId, bool binary) {
    if (!L) return;

    // Fire per-channel listeners (Ext.RegisterNetListener)
    events_fire_net_listeners(L, channel, payload, userId);

    // Legacy compatibility (Issue #6): If no module and no requestId,
    // fire the legacy NetMessage event. Most existing mods use this.
    if ((!module || module[0] == '\0') && requestId == 0 && replyId == 0) {
        events_fire_net_message(L, channel, payload, userId);
    }

    int count = g_handler_counts[EVENT_NET_MOD_MESSAGE];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing NetModMessage (%d handlers), channel=%s", count, channel);

    g_dispatch_depth[EVENT_NET_MOD_MESSAGE]++;

    for (int i = 0; i < g_handler_counts[EVENT_NET_MOD_MESSAGE]; i++) {
        EventHandler *h = &g_handlers[EVENT_NET_MOD_MESSAGE][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table
        lua_newtable(L);

        lua_pushstring(L, channel ? channel : "");
        lua_setfield(L, -2, "Channel");

        lua_pushstring(L, payload ? payload : "{}");
        lua_setfield(L, -2, "Payload");

        lua_pushstring(L, module ? module : "");
        lua_setfield(L, -2, "Module");

        lua_pushinteger(L, userId);
        lua_setfield(L, -2, "UserID");

        lua_pushinteger(L, (lua_Integer)requestId);
        lua_setfield(L, -2, "RequestId");

        lua_pushinteger(L, (lua_Integer)replyId);
        lua_setfield(L, -2, "ReplyId");

        lua_pushboolean(L, binary);
        lua_setfield(L, -2, "Binary");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("Error in NetModMessage handler (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_NET_MOD_MESSAGE, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_NET_MOD_MESSAGE]--;

    if (g_dispatch_depth[EVENT_NET_MOD_MESSAGE] == 0) {
        process_deferred_unsubscribes(L, EVENT_NET_MOD_MESSAGE);
    }
}

// ============================================================================
// Legacy NetMessage Event (Issue #6 - Windows BG3SE Parity)
//
// In Windows BG3SE, messages without a module UUID fire Ext.Events.NetMessage
// instead of Ext.Events.NetModMessage. Most existing mods use this legacy API.
// ============================================================================

void events_fire_net_message(lua_State *L, const char *channel, const char *payload,
                              int userId) {
    if (!L) return;

    // Note: per-channel listeners are fired from events_fire_net_mod_message
    // (which calls this function for legacy messages). Don't fire them again here
    // to avoid double-firing.

    int count = g_handler_counts[EVENT_NET_MESSAGE];
    if (count == 0) return;

    LOG_EVENTS_DEBUG("Firing NetMessage (legacy, %d handlers), channel=%s", count, channel);

    g_dispatch_depth[EVENT_NET_MESSAGE]++;

    for (int i = 0; i < g_handler_counts[EVENT_NET_MESSAGE]; i++) {
        EventHandler *h = &g_handlers[EVENT_NET_MESSAGE][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table (legacy format: Channel, Payload, UserID)
        lua_newtable(L);

        lua_pushstring(L, channel ? channel : "");
        lua_setfield(L, -2, "Channel");

        lua_pushstring(L, payload ? payload : "{}");
        lua_setfield(L, -2, "Payload");

        lua_pushinteger(L, userId);
        lua_setfield(L, -2, "UserID");

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_EVENTS_ERROR("Error in NetMessage handler (id=%llu, mod=%s): %s",
                       h->handler_id, h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_NET_MESSAGE, h->handler_id};
            }
        }
    }

    g_dispatch_depth[EVENT_NET_MESSAGE]--;

    if (g_dispatch_depth[EVENT_NET_MESSAGE] == 0) {
        process_deferred_unsubscribes(L, EVENT_NET_MESSAGE);
    }
}

// ============================================================================
// Per-Channel Net Listener Registry (Ext.RegisterNetListener)
// ============================================================================

// Registry key for the per-channel listener table
// Layout: g_net_listener_registry_ref -> { [channel] = { callback1, callback2, ... } }
static int g_net_listener_registry_ref = LUA_NOREF;

void events_push_net_listener_registry(lua_State *L) {
    if (g_net_listener_registry_ref == LUA_NOREF) {
        lua_newtable(L);
        lua_pushvalue(L, -1);
        g_net_listener_registry_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_net_listener_registry_ref);
}

void events_register_net_listener(lua_State *L, const char *channel, int callback_ref) {
    if (!L || !channel) return;

    events_push_net_listener_registry(L);

    // Get or create the channel's listener array
    lua_getfield(L, -1, channel);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);  // pop nil
        lua_newtable(L);
        lua_pushvalue(L, -1);  // duplicate for setfield
        lua_setfield(L, -3, channel);
    }

    // Append the callback ref to the channel's array
    int len = (int)lua_rawlen(L, -1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
    lua_rawseti(L, -2, len + 1);

    lua_pop(L, 2);  // pop channel table + registry table

    LOG_EVENTS_DEBUG("Registered net listener for channel '%s' (ref=%d)", channel, callback_ref);
}

void events_fire_net_listeners(lua_State *L, const char *channel, const char *payload, int userId) {
    if (!L || !channel || g_net_listener_registry_ref == LUA_NOREF) return;

    // Get the registry table
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_net_listener_registry_ref);
    lua_getfield(L, -1, channel);

    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    int len = (int)lua_rawlen(L, -1);
    for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_isfunction(L, -1)) {
            // Call with (channel, payload, userId)
            lua_pushstring(L, channel);
            lua_pushstring(L, payload ? payload : "{}");
            lua_pushinteger(L, userId);

            if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
                const char *err = lua_tostring(L, -1);
                LOG_EVENTS_ERROR("RegisterNetListener handler error (channel=%s): %s",
                           channel, err ? err : "unknown");
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
    }

    lua_pop(L, 2);  // pop channel table + registry table
}

// ============================================================================
// Log Event (Windows BG3SE Parity)
// ============================================================================

static int g_log_callback_id = -1;
static bool g_log_event_dispatching = false;  // Prevent recursion

// Dispatch barrier (replaces the old g_log_callback_L null check, E2.0).
// log_unregister_callback can return while a logging thread still holds a
// snapshot of the callback; that late invocation must find the barrier down
// rather than resolve the still-alive runtime. Cleared first in
// events_shutdown_log_callback(); checked before and after the gate.
static _Atomic(bool) g_log_dispatch_enabled = false;

/**
 * Fire the Log event with message data.
 * Returns true if any handler set e.Prevent = true.
 */
bool events_fire_log(lua_State *L, const char *level, const char *module, const char *message) {
    if (!L) return false;

    // Prevent infinite recursion (logging from within log handler)
    if (g_log_event_dispatching) return false;

    int count = g_handler_counts[EVENT_LOG];
    if (count == 0) return false;

    g_log_event_dispatching = true;
    g_dispatch_depth[EVENT_LOG]++;

    bool prevented = false;

    for (int i = 0; i < g_handler_counts[EVENT_LOG]; i++) {
        EventHandler *h = &g_handlers[EVENT_LOG][i];
        if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) {
            continue;
        }

        ModHealthEntry *mh = mod_health_get_or_create(h->mod_name);
        if (mh && mh->soft_disabled) continue;

        event_scope_begin(L, h->mod_name);

        lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            event_scope_end(L);
            continue;
        }

        // Create event data table
        lua_newtable(L);

        lua_pushstring(L, level ? level : "INFO");
        lua_setfield(L, -2, "Level");

        lua_pushstring(L, module ? module : "Core");
        lua_setfield(L, -2, "Module");

        lua_pushstring(L, message ? message : "");
        lua_setfield(L, -2, "Message");

        // Add Prevent field (initialized to false)
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "Prevent");

        // Keep a reference to check Prevent after call
        lua_pushvalue(L, -1);
        int event_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            // Don't use LOG_EVENTS_ERROR here - would cause recursion!
            fprintf(stderr, "[BG3SE] Log event handler error (mod=%s): %s\n",
                    h->mod_name, err ? err : "unknown");
            mod_health_record_error(h->mod_name, err);
            lua_pop(L, 1);
        } else {
            mod_health_record_success(h->mod_name);
        }

        event_scope_end(L);

        // Check if Prevent was set
        lua_rawgeti(L, LUA_REGISTRYINDEX, event_ref);
        lua_getfield(L, -1, "Prevent");
        if (lua_toboolean(L, -1)) {
            prevented = true;
        }
        lua_pop(L, 2);  // Pop Prevent and event table
        luaL_unref(L, LUA_REGISTRYINDEX, event_ref);

        if (h->once) {
            if (g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) {
                g_deferred_unsubs[g_deferred_unsub_count++] =
                    (DeferredUnsubscribe){EVENT_LOG, h->handler_id};
            }
        }

        if (prevented) break;  // Stop early if prevented
    }

    g_dispatch_depth[EVENT_LOG]--;

    if (g_dispatch_depth[EVENT_LOG] == 0) {
        process_deferred_unsubscribes(L, EVENT_LOG);
    }

    g_log_event_dispatching = false;
    return prevented;
}

/**
 * Log callback for the C logging system.
 * Forwards log messages to Lua handlers.
 */
static void log_event_callback(LogLevel level, LogModule module,
                               const char *message, void *userdata) {
    (void)userdata;

    // Log handlers live in the server VM until per-state handler tagging
    // lands (E2.0 audit 1.8) — resolve through the runtime registry.
    if (!atomic_load_explicit(&g_log_dispatch_enabled, memory_order_acquire)) return;
    if (!lua_runtime_state_for(LUA_CONTEXT_SERVER)) return;
    if (g_log_event_dispatching) return;  // Prevent recursion

    // Convert level and module to strings
    const char *level_str = log_level_name(level);
    const char *module_str = log_module_name(module);

    /*
     * TRYLOCK, never lock. The old comment argued a blocking lua_gate_lock()
     * was deadlock-free because the LOG mutex is released before callbacks --
     * true, and insufficient: a logging thread may already hold OTHER locks.
     * Observed 2026-08-28 (~8 min hang, diagnosed from a `sample` of the
     * wedged process):
     *
     *   render thread: holds the ImGui object mutex (imgui_metal_render_frame)
     *                  -> logs -> HERE -> waits on the Lua gate
     *   console eval:  holds the Lua gate (Tier 2 test creating an ImGui
     *                  window) -> waits on the ImGui object mutex
     *
     * Classic two-lock cycle; every Lua consumer then starves ("Lua service
     * tick starved: 300 consecutive gate misses"). Forwarding log lines to
     * Lua handlers is best-effort by nature -- dropping one under contention
     * is invisible; blocking the renderer is fatal. So: trylock, and on
     * contention this event is simply not delivered to Lua (the file/console
     * sinks already received it).
     */
    if (!lua_gate_trylock()) return;
    lua_State *L = lua_runtime_state_for(LUA_CONTEXT_SERVER);
    if (L && atomic_load_explicit(&g_log_dispatch_enabled, memory_order_acquire)) {
        events_fire_log(L, level_str, module_str, message);
    }
    lua_gate_unlock();
}

/**
 * Initialize the Log event callback with the logging system.
 */
void events_init_log_callback(void) {
    if (g_log_callback_id >= 0) {
        // Already registered
        return;
    }

    // Barrier up before registration so no callback invocation can race the
    // enable.
    atomic_store_explicit(&g_log_dispatch_enabled, true, memory_order_release);

    // Register callback with the logging system
    // Only forward messages that pass the current level filter
    g_log_callback_id = log_register_callback(log_event_callback, NULL,
                                              LOG_LEVEL_DEBUG, 0);

    if (g_log_callback_id >= 0) {
        // Enable callback output flag so callbacks actually get invoked
        uint32_t flags = log_get_output_flags();
        log_set_output_flags(flags | LOG_OUTPUT_CALLBACK);
        LOG_LUA_INFO("Log event callback registered (id=%d)", g_log_callback_id);
    }
}

/**
 * Tear down the Log event callback before the Lua state closes.
 * Must be called while holding the Lua gate (shutdown_lua does), so no
 * logging thread can be mid-dispatch into the dying state.
 */
void events_shutdown_log_callback(void) {
    // Barrier down FIRST: a logging thread may already hold a snapshot of the
    // callback and invoke it after log_unregister_callback returns.
    atomic_store_explicit(&g_log_dispatch_enabled, false, memory_order_release);
    if (g_log_callback_id >= 0) {
        log_unregister_callback(g_log_callback_id);
        g_log_callback_id = -1;
    }
}

// ============================================================================
// One-Frame Component Polling (Issue #51)
// ============================================================================

// ---------------------------------------------------------------------------
// TypeIndex cache: resolved once at first subscription, reused every tick.
// Avoids the Lua global table walk (Ext → Entity → GetAllEntitiesWithComponent
// → registry name scan) on every polling tick.
// ---------------------------------------------------------------------------

typedef struct {
    const char              *component_name;  // static string — never freed
    const ComponentInfo     *info;            // NULL until resolved; INVALID_PTR sentinel
    uint8_t                  resolved;        // 1 = resolved (info may still be NULL/invalid)
} OneFramePollEntry;

// Sentinel: component name registered in the system but TypeId unresolved
#define POLL_INFO_UNRESOLVABLE ((const ComponentInfo *)1)

// Static handle buffer: reused every tick, protected by single-threaded Lua
#define POLL_MAX_ENTITIES 4096
static uint64_t g_poll_handle_buf[POLL_MAX_ENTITIES];

// ---------------------------------------------------------------------------
// Fast direct-C poll: resolve ComponentInfo once, then call
// component_lookup_get_all_with_component directly — no Lua table traversal.
// Falls back to the Lua-path helper if the registry isn't warm yet.
// ---------------------------------------------------------------------------

static void poll_oneframe_direct(lua_State *L, OneFramePollEntry *entry,
                                 void (*handler)(lua_State*, uint64_t)) {
    // Lazy resolution: look up once and cache the ComponentInfo pointer
    if (!entry->resolved) {
        entry->resolved = 1;
        if (component_lookup_ready()) {
            const ComponentInfo *info = component_registry_lookup(entry->component_name);
            if (info && info->index != COMPONENT_INDEX_UNDEFINED) {
                entry->info = info;
            } else {
                entry->info = POLL_INFO_UNRESOLVABLE;
                LOG_EVENTS_DEBUG("Poll cache: '%s' unresolvable (not in registry or TypeId=65535)",
                                 entry->component_name);
            }
        }
        // If lookup_ready() returns false, leave resolved=1 but info=NULL so
        // we fall through to the Lua path this tick.
    }

    if (entry->info == NULL) {
        // Registry not warm yet — use the Lua global path
        goto lua_fallback;
    }
    if (entry->info == POLL_INFO_UNRESOLVABLE) {
        return;  // Component doesn't exist in this binary
    }

    {
        // Fast path: direct C call
        int count = component_lookup_get_all_with_component(
            entry->info->index, g_poll_handle_buf, POLL_MAX_ENTITIES);

        if (g_trace_enabled && count > 0) {
            LOG_EVENTS_INFO("[TRACE] Fast poll '%s': %d entities (typeIndex=%u)",
                            entry->component_name, count, entry->info->index);
        }

        for (int i = 0; i < count; i++) {
            handler(L, g_poll_handle_buf[i]);
        }
        return;
    }

lua_fallback:
    // Lua-path: used only until the registry becomes warm (first few ticks)
    if (g_trace_enabled) {
        LOG_EVENTS_INFO("[TRACE] Lua-path poll: %s (registry not ready)", entry->component_name);
    }
    lua_getglobal(L, "Ext");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "Entity");
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return; }
    lua_getfield(L, -1, "GetAllEntitiesWithComponent");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 3); return; }
    lua_pushstring(L, entry->component_name);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isinteger(L, -1)) {
                handler(L, (uint64_t)lua_tointeger(L, -1));
            }
            lua_pop(L, 1);
        }
    } else if (g_trace_enabled) {
        const char *err = lua_tostring(L, -1);
        if (err) {
            LOG_EVENTS_DEBUG("[TRACE] Lua-path poll failed for %s: %s",
                            entry->component_name, err);
        }
    }
    lua_pop(L, 3);
    return;
}

// Legacy helper kept for callers that pass a literal string without a cache entry
static void poll_oneframe_component(lua_State *L, const char *componentName,
                                    void (*handler)(lua_State*, uint64_t)) {
    if (g_trace_enabled) {
        LOG_EVENTS_INFO("[TRACE] Polling component: %s", componentName);
    }

    lua_getglobal(L, "Ext");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    lua_getfield(L, -1, "Entity");
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return; }

    lua_getfield(L, -1, "GetAllEntitiesWithComponent");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 3); return; }

    lua_pushstring(L, componentName);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1)) {
        int entity_count = 0;
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isinteger(L, -1)) {
                uint64_t entity = (uint64_t)lua_tointeger(L, -1);
                entity_count++;
                if (g_trace_enabled) {
                    LOG_EVENTS_INFO("[TRACE] Found entity 0x%llx with %s",
                                    (unsigned long long)entity, componentName);
                }
                handler(L, entity);
            }
            lua_pop(L, 1);  // Pop value, keep key
        }
        if (g_trace_enabled && entity_count > 0) {
            LOG_EVENTS_INFO("[TRACE] %s: %d entities processed", componentName, entity_count);
        }
    } else if (g_trace_enabled) {
        // Log if the component wasn't found (typeIndex=65535 case)
        const char *err = lua_tostring(L, -1);
        if (err) {
            LOG_EVENTS_DEBUG("[TRACE] GetAllEntitiesWithComponent failed for %s: %s",
                            componentName, err);
        }
    }
    lua_pop(L, 3);  // Pop result + Entity + Ext
}

// Individual event handlers for each one-frame component type
static void handle_turn_started(lua_State *L, uint64_t entity) {
    events_fire_turn_started(L, entity, 0);  // Round extracted from component if needed
}

/**
 * Helper macro for oneframe event handler dispatch with mod attribution.
 * All oneframe handlers share the same structure: dispatch to each handler
 * with soft-disable, mod context, and health tracking.
 */
#define ONEFRAME_DISPATCH(EVENT_TYPE, FIELD_NAME, ENTITY_VAR) \
    do { \
        if (g_handler_counts[EVENT_TYPE] == 0) return; \
        g_dispatch_depth[EVENT_TYPE]++; \
        for (int i = 0; i < g_handler_counts[EVENT_TYPE]; i++) { \
            EventHandler *h = &g_handlers[EVENT_TYPE][i]; \
            if (h->callback_ref == LUA_NOREF || h->callback_ref == LUA_REFNIL) continue; \
            ModHealthEntry *mh = mod_health_get_or_create(h->mod_name); \
            if (mh && mh->soft_disabled) continue; \
            event_scope_begin(L, h->mod_name); \
            lua_rawgeti(L, LUA_REGISTRYINDEX, h->callback_ref); \
            if (lua_isfunction(L, -1)) { \
                lua_newtable(L); \
                lua_pushinteger(L, (lua_Integer)(ENTITY_VAR)); \
                lua_setfield(L, -2, FIELD_NAME); \
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) { \
                    mod_health_record_error(h->mod_name, lua_tostring(L, -1)); \
                    lua_pop(L, 1); \
                } else { \
                    mod_health_record_success(h->mod_name); \
                } \
            } else { \
                lua_pop(L, 1); \
            } \
            event_scope_end(L); \
            if (h->once && g_deferred_unsub_count < MAX_DEFERRED_OPERATIONS) { \
                g_deferred_unsubs[g_deferred_unsub_count++] = (DeferredUnsubscribe){EVENT_TYPE, h->handler_id}; \
            } \
        } \
        g_dispatch_depth[EVENT_TYPE]--; \
        if (g_dispatch_depth[EVENT_TYPE] == 0) process_deferred_unsubscribes(L, EVENT_TYPE); \
    } while (0)

static void handle_turn_ended(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_TURN_ENDED, "Entity", entity);
}

static void handle_combat_started(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_COMBAT_STARTED, "CombatId", entity);
}

static void handle_combat_left(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_COMBAT_ENDED, "Entity", entity);
}

static void handle_status_applied(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_STATUS_APPLIED, "Entity", entity);
}

static void handle_status_removed(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_STATUS_REMOVED, "Entity", entity);
}

static void handle_equipment_changed(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_EQUIPMENT_CHANGED, "Entity", entity);
}

static void handle_level_up(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_LEVEL_UP, "Entity", entity);
}

// ============================================================================
// Additional One-Frame Handlers (Issue #51 expansion)
// ============================================================================

static void handle_died(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_DIED, "Entity", entity);
}

static void handle_downed(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_DOWNED, "Entity", entity);
}

static void handle_resurrected(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_RESURRECTED, "Entity", entity);
}

static void handle_spell_cast(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SPELL_CAST, "Entity", entity);
}

static void handle_spell_cast_finished(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SPELL_CAST_FINISHED, "Entity", entity);
}

static void handle_hit_notification(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_HIT_NOTIFICATION, "Entity", entity);
}

static void handle_short_rest_started(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SHORT_REST_STARTED, "Entity", entity);
}

static void handle_approval_changed(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_APPROVAL_CHANGED, "Entity", entity);
}

// ============================================================================
// Spell Cast Phase Handlers (Issue #51 expansion)
// ============================================================================

static void handle_spell_cast_countered(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SPELL_CAST_COUNTERED, "Entity", entity);
}

static void handle_spell_cast_jump_start(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SPELL_CAST_JUMP_START, "Entity", entity);
}

static void handle_concentration_cleared(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_CONCENTRATION_CLEARED, "Entity", entity);
}

static void handle_spell_cast_logic_execution_start(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SPELL_CAST_LOGIC_EXECUTION_START, "Entity", entity);
}

static void handle_spell_cast_logic_execution_end(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SPELL_CAST_LOGIC_EXECUTION_END, "Entity", entity);
}

static void handle_spell_cast_prepare_start(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SPELL_CAST_PREPARE_START, "Entity", entity);
}

static void handle_spell_cast_prepare_end(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SPELL_CAST_PREPARE_END, "Entity", entity);
}

static void handle_spell_cast_preview_end(lua_State *L, uint64_t entity) {
    ONEFRAME_DISPATCH(EVENT_SPELL_CAST_PREVIEW_END, "Entity", entity);
}

// ---------------------------------------------------------------------------
// Static poll cache: one entry per engine event, indexed by BG3SEEventType.
// Entries for non-polled events (NetMessage, etc.) are left as {NULL, NULL, 0}
// and are never accessed because the poll loop only iterates engine events.
// ---------------------------------------------------------------------------

// Declare entries for every engine event between EVENT_TURN_STARTED and
// EVENT_SPELL_CAST_PREVIEW_END. Events that need two components (e.g.
// EquipmentChanged) get a second entry; the first entry's handler is called
// for both. We keep a flat list of (event, component_name, handler) triples
// rather than one-per-event to handle the dual-component case cleanly.

typedef struct {
    BG3SEEventType          event;
    const char             *component_name;
    void (*handler)(lua_State*, uint64_t);
    OneFramePollEntry       cache;
} PolledEvent;

// The static table. All cache fields initialise to zero (= unresolved).
static PolledEvent g_polled_events[] = {
    { EVENT_TURN_STARTED,    "esv::TurnStartedEventOneFrameComponent",                               handle_turn_started,                       {NULL, NULL, 0} },
    { EVENT_TURN_ENDED,      "esv::TurnEndedEventOneFrameComponent",                                 handle_turn_ended,                         {NULL, NULL, 0} },
    { EVENT_COMBAT_STARTED,  "esv::combat::JoinEventOneFrameComponent",                              handle_combat_started,                     {NULL, NULL, 0} },
    { EVENT_COMBAT_ENDED,    "esv::combat::FleeSuccessOneFrameComponent",                            handle_combat_left,                        {NULL, NULL, 0} },
    { EVENT_EQUIPMENT_CHANGED, "esv::item::EquippedEventOneFrameComponent",                          handle_equipment_changed,                  {NULL, NULL, 0} },
    { EVENT_EQUIPMENT_CHANGED, "esv::item::UnequippedEventOneFrameComponent",                        handle_equipment_changed,                  {NULL, NULL, 0} },
    { EVENT_STATUS_APPLIED,  "esv::status::ActivationEventOneFrameComponent",                        handle_status_applied,                     {NULL, NULL, 0} },
    { EVENT_STATUS_REMOVED,  "esv::status::DeactivationEventOneFrameComponent",                      handle_status_removed,                     {NULL, NULL, 0} },
    { EVENT_LEVEL_UP,        "esv::stats::LevelChangedOneFrameComponent",                            handle_level_up,                           {NULL, NULL, 0} },
    { EVENT_DIED,            "esv::death::ExecuteDieLogicEventOneFrameComponent",                    handle_died,                               {NULL, NULL, 0} },
    { EVENT_DOWNED,          "esv::death::DownedEventOneFrameComponent",                             handle_downed,                             {NULL, NULL, 0} },
    { EVENT_RESURRECTED,     "esv::death::ResurrectedEventOneFrameComponent",                        handle_resurrected,                        {NULL, NULL, 0} },
    { EVENT_SPELL_CAST,      "eoc::spell_cast::CastEventOneFrameComponent",                          handle_spell_cast,                         {NULL, NULL, 0} },
    { EVENT_SPELL_CAST_FINISHED, "eoc::spell_cast::FinishedEventOneFrameComponent",                  handle_spell_cast_finished,                {NULL, NULL, 0} },
    { EVENT_HIT_NOTIFICATION, "esv::hit::HitNotificationEventOneFrameComponent",                     handle_hit_notification,                   {NULL, NULL, 0} },
    { EVENT_SHORT_REST_STARTED, "esv::rest::ShortRestResultEventOneFrameComponent",                   handle_short_rest_started,                 {NULL, NULL, 0} },
    { EVENT_APPROVAL_CHANGED, "esv::approval::RatingsChangedOneFrameComponent",                      handle_approval_changed,                   {NULL, NULL, 0} },
    // Spell cast phase events (Issue #51 expansion)
    { EVENT_SPELL_CAST_COUNTERED,             "eoc::spell_cast::CounteredEventOneFrameComponent",             handle_spell_cast_countered,             {NULL, NULL, 0} },
    { EVENT_SPELL_CAST_JUMP_START,            "eoc::spell_cast::JumpStartEventOneFrameComponent",             handle_spell_cast_jump_start,            {NULL, NULL, 0} },
    { EVENT_CONCENTRATION_CLEARED,            "esv::concentration::OnConcentrationClearedEventOneFrameComponent", handle_concentration_cleared,        {NULL, NULL, 0} },
    { EVENT_SPELL_CAST_LOGIC_EXECUTION_START, "eoc::spell_cast::LogicExecutionStartEventOneFrameComponent",   handle_spell_cast_logic_execution_start, {NULL, NULL, 0} },
    { EVENT_SPELL_CAST_LOGIC_EXECUTION_END,   "eoc::spell_cast::LogicExecutionEndEventOneFrameComponent",     handle_spell_cast_logic_execution_end,   {NULL, NULL, 0} },
    { EVENT_SPELL_CAST_PREPARE_START,         "eoc::spell_cast::PrepareStartEventOneFrameComponent",          handle_spell_cast_prepare_start,         {NULL, NULL, 0} },
    { EVENT_SPELL_CAST_PREPARE_END,           "eoc::spell_cast::PrepareEndEventOneFrameComponent",            handle_spell_cast_prepare_end,           {NULL, NULL, 0} },
    { EVENT_SPELL_CAST_PREVIEW_END,           "eoc::spell_cast::PreviewEndEventOneFrameComponent",            handle_spell_cast_preview_end,           {NULL, NULL, 0} },
};

#define POLLED_EVENTS_COUNT ((int)(sizeof(g_polled_events) / sizeof(g_polled_events[0])))

// Initialise the component_name pointer in each cache entry once at startup.
// Called from events_init() so the cache is self-consistent from the first tick.
static void poll_cache_init(void) {
    for (int i = 0; i < POLLED_EVENTS_COUNT; i++) {
        g_polled_events[i].cache.component_name = g_polled_events[i].component_name;
        g_polled_events[i].cache.info = NULL;
        g_polled_events[i].cache.resolved = 0;
    }
}

void events_poll_oneframe_components(lua_State *L) {
    if (!L) return;

    // Only poll if we have subscribers to any engine events
    int total_handlers = 0;
    for (int i = EVENT_TURN_STARTED; i <= EVENT_SPELL_CAST_PREVIEW_END; i++) {
        total_handlers += g_handler_counts[i];
    }
    if (total_handlers == 0) return;

    // Iterate the flat poll table. Each entry is guarded by its event's
    // handler count so unsubscribed events cost only one comparison.
    for (int i = 0; i < POLLED_EVENTS_COUNT; i++) {
        PolledEvent *pe = &g_polled_events[i];
        if (g_handler_counts[pe->event] == 0) continue;
        poll_oneframe_direct(L, &pe->cache, pe->handler);
    }
}
