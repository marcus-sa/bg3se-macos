/*
 * Tier 0: per-context mod environments (src/lua/lua_modenv.c).
 *
 * Reproduces the SubclassCompatibilityFramework failure the shadow env exists
 * to fix: the client bootstrap builds Globals/Queue through Ext.Require, the
 * server bootstrap then re-declares `Globals = {}` at file scope while its own
 * requires are cache hits, and the client's StatsLoaded closure dies on
 * `#Globals.ValidationErrors`.
 */

#include "test_harness.h"
#include "lua_modenv.h"

#include <lauxlib.h>
#include <lualib.h>

/* Run `src` with _ENV set to the table at the top of the stack (which is
 * popped). Returns 1 on success. */
static int run_with_env(lua_State *L, const char *src) {
    if (luaL_loadstring(L, src) != LUA_OK) {
        lua_pop(L, 2);
        return 0;
    }
    lua_insert(L, -2);                       /* [chunk, env] */
    if (lua_setupvalue(L, -2, 1) == NULL) {  /* _ENV is upvalue 1 */
        lua_pop(L, 2);
        return 0;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

/* Push a fresh published Mods.<name> table shaped like mod_env_set builds it. */
static void push_published_env(lua_State *L) {
    lua_newtable(L);
    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_G");
}

#define CTX_SERVER 1
#define CTX_CLIENT 2

TEST(modenv_first_context_owns_published_table) {
    lua_State *L = luaL_newstate();
    ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    lua_modenv_reset(L);

    ASSERT_TRUE(lua_modenv_claim_owner(L, "SCF", CTX_CLIENT));
    /* Same context re-entering (a second bootstrap pass) keeps the table. */
    ASSERT_TRUE(lua_modenv_claim_owner(L, "SCF", CTX_CLIENT));
    /* The other half must not get it. */
    ASSERT_FALSE(lua_modenv_claim_owner(L, "SCF", CTX_SERVER));
    /* Ownership is per mod, not global. */
    ASSERT_TRUE(lua_modenv_claim_owner(L, "OtherMod", CTX_SERVER));

    ASSERT_EQ(lua_gettop(L), 0);
    lua_close(L);
}

TEST(modenv_shadow_is_memoized_per_context) {
    lua_State *L = luaL_newstate();
    ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    lua_modenv_reset(L);

    push_published_env(L);                           /* [E] */
    lua_modenv_push_shadow(L, -1, CTX_SERVER, "SCF");/* [E, S1] */
    lua_modenv_push_shadow(L, -2, CTX_SERVER, "SCF");/* [E, S1, S2] */
    ASSERT_TRUE(lua_rawequal(L, -1, -2));

    /* A different context gets a different shadow. */
    lua_modenv_push_shadow(L, 1, CTX_CLIENT, "SCF"); /* [E, S1, S2, S3] */
    ASSERT_FALSE(lua_rawequal(L, -1, -2));

    lua_settop(L, 0);
    lua_close(L);
}

TEST(modenv_shadow_reads_fall_through_to_publisher_and_globals) {
    lua_State *L = luaL_newstate();
    ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    lua_modenv_reset(L);

    lua_pushinteger(L, 7);
    lua_setglobal(L, "RealGlobal");

    push_published_env(L);                            /* [E] */
    lua_pushstring(L, "from-client");
    lua_setfield(L, -2, "Marker");

    lua_modenv_push_shadow(L, -1, CTX_SERVER, "SCF"); /* [E, S] */
    ASSERT_TRUE(run_with_env(L,
        "SeenMarker = Marker\n"
        "SeenGlobal = RealGlobal\n"
        "SeenSelf = (_G == _ENV)\n"));                /* [E] */

    /* Those three writes are new keys, so they land on the published table --
     * that is the pre-split behaviour and mods rely on it. */
    lua_getfield(L, -1, "SeenMarker");
    ASSERT_STR_EQ(lua_tostring(L, -1), "from-client");
    lua_pop(L, 1);
    lua_getfield(L, -1, "SeenGlobal");
    ASSERT_EQ((int)lua_tointeger(L, -1), 7);
    lua_pop(L, 1);
    lua_getfield(L, -1, "SeenSelf");
    ASSERT_TRUE(lua_toboolean(L, -1));
    lua_pop(L, 1);

    lua_settop(L, 0);
    lua_close(L);
}

TEST(modenv_shadow_cannot_clobber_publisher_keys) {
    lua_State *L = luaL_newstate();
    ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    lua_modenv_reset(L);

    /* --- client half: builds Globals through its Ext.Require chain --- */
    push_published_env(L);                            /* [E] */
    lua_pushvalue(L, -1);                             /* [E, E] */
    ASSERT_TRUE(run_with_env(L,
        "Globals = {}\n"
        "Queue = {}\n"
        "Globals.ValidationErrors = {}\n"
        "Queue.Commit = function() return #Globals.ValidationErrors end\n"));

    /* --- server half: shadow env, re-declares the same names --- */
    lua_modenv_push_shadow(L, -1, CTX_SERVER, "SCF"); /* [E, S] */
    lua_pushvalue(L, -1);                             /* [E, S, S] */
    ASSERT_TRUE(run_with_env(L,
        "Api = {}\n"        /* new key -> writes through to the published table */
        "Globals = {}\n")); /* owned key -> captured privately */

    /* The client's closure still sees its own populated Globals. */
    lua_getfield(L, 1, "Queue");
    lua_getfield(L, -1, "Commit");
    ASSERT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK);
    ASSERT_EQ((int)lua_tointeger(L, -1), 0);   /* 0, not an error on nil */
    lua_pop(L, 2);

    /* The published Globals is untouched... */
    lua_getfield(L, 1, "Globals");
    lua_getfield(L, -1, "ValidationErrors");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_pop(L, 2);

    /* ...while the server half sees its own fresh one. */
    lua_getfield(L, 2, "Globals");
    lua_getfield(L, -1, "ValidationErrors");
    ASSERT_TRUE(lua_isnil(L, -1));
    lua_pop(L, 2);

    /* A key only the server declared is shared, as before the split. */
    lua_getfield(L, 1, "Api");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_pop(L, 1);

    lua_settop(L, 0);
    lua_close(L);
}

TEST(modenv_shadow_updates_its_own_key_after_first_shadow_write) {
    lua_State *L = luaL_newstate();
    ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    lua_modenv_reset(L);

    push_published_env(L);                            /* [E] */
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "Counter");

    lua_modenv_push_shadow(L, -1, CTX_SERVER, "SCF"); /* [E, S] */
    lua_pushvalue(L, -1);
    ASSERT_TRUE(run_with_env(L, "Counter = 2\n"));
    lua_pushvalue(L, -1);
    ASSERT_TRUE(run_with_env(L, "Counter = Counter + 1\n"));

    lua_getfield(L, -1, "Counter");
    ASSERT_EQ((int)lua_tointeger(L, -1), 3);
    lua_pop(L, 1);
    lua_getfield(L, 1, "Counter");
    ASSERT_EQ((int)lua_tointeger(L, -1), 1);   /* publisher never moved */
    lua_pop(L, 1);

    lua_settop(L, 0);
    lua_close(L);
}

void register_mod_env_tests(void) {
    RUN_TEST(modenv_first_context_owns_published_table);
    RUN_TEST(modenv_shadow_is_memoized_per_context);
    RUN_TEST(modenv_shadow_reads_fall_through_to_publisher_and_globals);
    RUN_TEST(modenv_shadow_cannot_clobber_publisher_keys);
    RUN_TEST(modenv_shadow_updates_its_own_key_after_first_shadow_write);
}
