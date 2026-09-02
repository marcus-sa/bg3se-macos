/**
 * BG3SE-macOS - Per-context mod environments
 *
 * See lua_modenv.h for why the second context to bootstrap a mod needs its own
 * environment layered over the published Mods.<ModTable>.
 */

#include "lua_modenv.h"

#include <lauxlib.h>
#include <stdio.h>

#define MODENV_OWNER_KEY   "BG3SE_ModEnvOwner"    /* mod_table    -> context int */
#define MODENV_SHADOW_KEY  "BG3SE_ModEnvShadow"   /* "ctx:modtbl" -> shadow table */

/* Push registry[key], creating it as a fresh table on first use. */
static void push_named_registry_table(lua_State *L, const char *key) {
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }
}

int lua_modenv_claim_owner(lua_State *L, const char *mod_table, int ctx) {
    if (!mod_table || !mod_table[0]) return 1;

    push_named_registry_table(L, MODENV_OWNER_KEY);   /* [owners] */
    lua_getfield(L, -1, mod_table);                   /* [owners, owner?] */

    int owns;
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushinteger(L, ctx);
        lua_setfield(L, -2, mod_table);
        owns = 1;
    } else {
        owns = ((int)lua_tointeger(L, -1) == ctx);
        lua_pop(L, 1);
    }

    lua_pop(L, 1);                                    /* [] */
    return owns;
}

/*
 * __newindex for a shadow env. Upvalue 1 is the published Mods.<ModTable>.
 *
 * The rawget decides ownership: a key the published table already carries
 * belongs to the first context, so the write is captured privately in the
 * shadow instead of overwriting it. Everything else writes straight through,
 * preserving the pre-split behaviour where a second-context definition is
 * visible to the first context's closures.
 */
static int modenv_shadow_newindex(lua_State *L) {
    lua_settop(L, 3);                        /* [shadow, k, v] */
    lua_pushvalue(L, lua_upvalueindex(1));   /* [shadow, k, v, E] */
    lua_pushvalue(L, 2);                     /* [shadow, k, v, E, k] */
    lua_rawget(L, -2);                       /* [shadow, k, v, E, E[k]] */
    int owned_by_publisher = !lua_isnil(L, -1);
    lua_pop(L, 1);                           /* [shadow, k, v, E] */

    if (owned_by_publisher) {
        lua_pop(L, 1);                       /* [shadow, k, v] */
        lua_rawset(L, 1);                    /* shadow[k] = v */
    } else {
        lua_pushvalue(L, 2);
        lua_pushvalue(L, 3);
        lua_rawset(L, -3);                   /* E[k] = v */
    }
    return 0;
}

void lua_modenv_push_shadow(lua_State *L, int published_index, int ctx,
                            const char *mod_table) {
    published_index = lua_absindex(L, published_index);

    char key[320];
    snprintf(key, sizeof(key), "%d:%s", ctx, mod_table ? mod_table : "");

    push_named_registry_table(L, MODENV_SHADOW_KEY);  /* [shadows] */
    lua_getfield(L, -1, key);                         /* [shadows, S?] */
    if (lua_istable(L, -1)) {
        lua_remove(L, -2);                            /* [S] */
        return;
    }
    lua_pop(L, 1);                                    /* [shadows] */

    lua_newtable(L);                                  /* [shadows, S] */

    /* _G inside the shadow must be the shadow, for the same reason the
     * published env sets E._G = E: mods write bare globals and read them back
     * as _G[name] (CommunityLibrary's Import() does exactly that). */
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_G");

    lua_newtable(L);                                  /* [shadows, S, mt] */
    lua_pushvalue(L, published_index);
    lua_setfield(L, -2, "__index");                   /* reads fall through to E */
    lua_pushvalue(L, published_index);
    lua_pushcclosure(L, modenv_shadow_newindex, 1);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);                          /* [shadows, S] */

    lua_pushvalue(L, -1);
    lua_setfield(L, -3, key);                         /* shadows[key] = S */
    lua_remove(L, -2);                                /* [S] */
}

void lua_modenv_reset(lua_State *L) {
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, MODENV_OWNER_KEY);
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, MODENV_SHADOW_KEY);
}
