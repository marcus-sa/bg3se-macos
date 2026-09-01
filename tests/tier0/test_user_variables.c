/*
 * Tier 0: Ext.Vars table identity.
 *
 * Table-valued variables are persisted as JSON. Deserializing on every read
 * hands the mod a NEW table each time, so a nested write goes into a throwaway
 * object and is silently lost. Real mods rely on the Windows BG3SE semantics —
 * same table object every read — and break in ways that look nothing like a
 * variable bug. "Sit This One Out 2" is the reference case:
 *
 *     Vars.SittingOut[combat] = Vars.SittingOut[combat] or {}   -- SitOut.lua:593
 *     if Vars.SittingOut[combat][uuid] then                     -- SitOut.lua:596
 *
 * Without identity, :596 raises "attempt to index a nil value", DoSitOut aborts
 * before Osi.ApplyStatus(SITOUT_ONCOMBATSTART_DOONCE_TECHNICAL), and companions
 * never sit out. These tests pin the semantics, not the implementation.
 */

#include "test_harness.h"
#include "user_variables.h"

#include <lauxlib.h>
#include <lualib.h>

static lua_State *vars_state(void) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    uvar_init();
    return L;
}

/* Register one persistent mod variable and seed it with an empty table. */
static void seed_mod_table(lua_State *L, const char *uuid, const char *key) {
    mvar_get_or_create_mod(uuid);
    mvar_register_prototype(uuid, key, UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_PERSISTENT);
    lua_newtable(L);
    mvar_set(L, uuid, key, lua_gettop(L));
    lua_pop(L, 1);
}

TEST(mod_table_reads_are_identity_stable) {
    lua_State *L = vars_state();
    seed_mod_table(L, "uuid-A", "SittingOut");

    mvar_get(L, "uuid-A", "SittingOut");   /* [t1] */
    mvar_get(L, "uuid-A", "SittingOut");   /* [t1, t2] */
    ASSERT_TRUE(lua_istable(L, -1));
    ASSERT_TRUE(lua_rawequal(L, -1, -2));

    lua_settop(L, 0);
    uvar_shutdown();
    lua_close(L);
}

TEST(nested_write_survives_a_reread) {
    lua_State *L = vars_state();
    seed_mod_table(L, "uuid-B", "SittingOut");

    /* Vars.SittingOut["combat-1"] = {} */
    mvar_get(L, "uuid-B", "SittingOut");
    lua_newtable(L);
    lua_setfield(L, -2, "combat-1");
    lua_pop(L, 1);

    /* Vars.SittingOut["combat-1"]["char-1"] = true — the line that used to die */
    mvar_get(L, "uuid-B", "SittingOut");
    lua_getfield(L, -1, "combat-1");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "char-1");
    lua_pop(L, 2);

    mvar_get(L, "uuid-B", "SittingOut");
    lua_getfield(L, -1, "combat-1");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "char-1");
    ASSERT_TRUE(lua_toboolean(L, -1));

    lua_settop(L, 0);
    uvar_shutdown();
    lua_close(L);
}

/* LoadVars() does `Vars.X = Vars.X or {}` on every call; the round-trip must
 * not swap the object out from under state the mod already stored in it. */
TEST(set_then_get_returns_the_same_object) {
    lua_State *L = vars_state();
    mvar_get_or_create_mod("uuid-C");
    mvar_register_prototype("uuid-C", "Enemies", UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_PERSISTENT);

    lua_newtable(L);
    lua_pushinteger(L, 7);
    lua_setfield(L, -2, "marker");
    mvar_set(L, "uuid-C", "Enemies", lua_gettop(L));   /* [written] */

    mvar_get(L, "uuid-C", "Enemies");                  /* [written, read] */
    ASSERT_TRUE(lua_rawequal(L, -1, -2));
    lua_getfield(L, -1, "marker");
    ASSERT_EQ(lua_tointeger(L, -1), 7);

    lua_settop(L, 0);
    uvar_shutdown();
    lua_close(L);
}

/* Overwriting with a scalar must drop the cached table, not keep serving it. */
TEST(scalar_overwrite_clears_the_cached_table) {
    lua_State *L = vars_state();
    seed_mod_table(L, "uuid-D", "SittingOut");

    lua_pushinteger(L, 42);
    mvar_set(L, "uuid-D", "SittingOut", lua_gettop(L));
    lua_pop(L, 1);

    mvar_get(L, "uuid-D", "SittingOut");
    ASSERT_FALSE(lua_istable(L, -1));
    ASSERT_EQ(lua_tointeger(L, -1), 42);

    lua_settop(L, 0);
    uvar_shutdown();
    lua_close(L);
}

/* Entity variables (entity.Vars.Foo.bar = 1) share the same storage path. */
TEST(entity_table_reads_are_identity_stable) {
    lua_State *L = vars_state();
    uvar_register_prototype("MyMod_State", UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_PERSISTENT);
    uvar_get_or_create_entity("guid-1", 0);

    lua_newtable(L);
    uvar_set(L, "guid-1", 0, "MyMod_State", lua_gettop(L));
    lua_pop(L, 1);

    uvar_get(L, "guid-1", "MyMod_State");
    lua_pushinteger(L, 3);
    lua_setfield(L, -2, "hp");
    lua_pop(L, 1);

    uvar_get(L, "guid-1", "MyMod_State");
    lua_getfield(L, -1, "hp");
    ASSERT_EQ(lua_tointeger(L, -1), 3);

    lua_settop(L, 0);
    uvar_shutdown();
    lua_close(L);
}

/* Each Lua state owns its cache: a table cached in one registry must never be
 * handed to another state, whose registry indices mean something else. */
TEST(cache_does_not_leak_across_states) {
    lua_State *sv = vars_state();
    seed_mod_table(sv, "uuid-E", "SittingOut");

    mvar_get(sv, "uuid-E", "SittingOut");
    lua_pushboolean(sv, 1);
    lua_setfield(sv, -2, "server-only");
    lua_pop(sv, 1);

    lua_State *cl = luaL_newstate();
    luaL_openlibs(cl);
    mvar_get(cl, "uuid-E", "SittingOut");
    ASSERT_TRUE(lua_istable(cl, -1));
    /* Same state, twice: still stable. */
    mvar_get(cl, "uuid-E", "SittingOut");
    ASSERT_TRUE(lua_rawequal(cl, -1, -2));

    lua_settop(sv, 0);
    lua_settop(cl, 0);
    uvar_shutdown();
    lua_close(cl);
    lua_close(sv);
}

void register_user_variables_tests(void) {
    printf("User variables (Ext.Vars):\n");
    RUN_TEST(mod_table_reads_are_identity_stable);
    RUN_TEST(nested_write_survives_a_reread);
    RUN_TEST(set_then_get_returns_the_same_object);
    RUN_TEST(scalar_overwrite_clears_the_cached_table);
    RUN_TEST(entity_table_reads_are_identity_stable);
    RUN_TEST(cache_does_not_leak_across_states);
    printf("\n");
}
