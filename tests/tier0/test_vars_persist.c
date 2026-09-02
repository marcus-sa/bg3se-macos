/*
 * Tier 0: Ext.Vars savegame persistence.
 *
 * Mod and entity variables never survived a reload: nothing called the save
 * paths, so ~/Library/Application Support/BG3SE/modvars.json was never even
 * created. Every mod that keeps state in Ext.Vars saw nil after every load —
 * AppearanceEditEnhanced's Cleanup.lua:56 indexes Vars.SpellOwners from a
 * CharacterJoinedParty handler and died with "attempt to index a nil value"
 * every time a companion joined.
 *
 * These tests pin the store contract, not the file layout: write, reload,
 * same values (nested tables included); one savegame's variables never appear
 * in another's; a malformed store degrades to "no variables" instead of
 * crashing or half-restoring; and a restored value is never shadowed by the
 * identity cache's copy of the outgoing savegame's table.
 */

#include "test_harness.h"
#include "user_variables.h"
#include "vars_persist.h"

#include <lauxlib.h>
#include <lualib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

#define MOD_UUID "aee-uuid"

static char s_dir[512];

static void store_dir_init(const char *name) {
    snprintf(s_dir, sizeof(s_dir), "/tmp/bg3se_vars_test_%s_%d", name, (int)getpid());
    /* Start from a clean directory so a previous run cannot answer for us. */
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_dir);
    (void)system(cmd);
    mkdir(s_dir, 0755);
    vars_persist_set_store_dir(s_dir);
}

static void store_dir_cleanup(void) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_dir);
    (void)system(cmd);
    vars_persist_set_store_dir(NULL);
}

static void write_raw_store(const char *key, const char *contents) {
    char path[700];
    snprintf(path, sizeof(path), "%s/%s.json", s_dir, key);
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fputs(contents, f);
    fclose(f);
}

static bool store_exists(const char *key) {
    char path[700];
    snprintf(path, sizeof(path), "%s/%s.json", s_dir, key);
    struct stat st;
    return stat(path, &st) == 0;
}

static lua_State *fresh_state(const char *name) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    uvar_init();
    vars_persist_init();
    store_dir_init(name);
    return L;
}

static void teardown(lua_State *L) {
    lua_settop(L, 0);
    vars_persist_detach();
    uvar_shutdown();
    store_dir_cleanup();
    lua_close(L);
}

/* SpellOwners as AppearanceEditEnhanced registers it. */
static void register_spell_owners(void) {
    mvar_register_prototype(MOD_UUID, "SpellOwners",
                            UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_WRITEABLE_SERVER |
                            UVAR_FLAG_PERSISTENT);
}

TEST(round_trip_restores_scalars_and_nested_tables) {
    lua_State *L = fresh_state("roundtrip");
    register_spell_owners();
    mvar_register_prototype(MOD_UUID, "Version",
                            UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_PERSISTENT);
    uvar_register_prototype("MyMod_State",
                            UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_PERSISTENT);

    vars_persist_attach_key(L, "SaveOne");

    /* SpellOwners = { ["spell-a"] = { owner = "char-1", level = 3 } } */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, "char-1");
    lua_setfield(L, -2, "owner");
    lua_pushinteger(L, 3);
    lua_setfield(L, -2, "level");
    lua_setfield(L, -2, "spell-a");
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);

    lua_pushinteger(L, 42);
    mvar_set(L, MOD_UUID, "Version", lua_gettop(L));
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushinteger(L, 17);
    lua_setfield(L, -2, "hp");
    uvar_set(L, "guid-1", 0, "MyMod_State", lua_gettop(L));
    lua_pop(L, 1);

    vars_persist_flush_now(L);
    ASSERT_TRUE(store_exists("SaveOne"));

    /* Reload the same savegame. */
    vars_persist_attach_key(L, "SaveOne");

    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "spell-a");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "owner");
    ASSERT_STR_EQ(lua_tostring(L, -1), "char-1");
    lua_pop(L, 1);
    lua_getfield(L, -1, "level");
    ASSERT_EQ(lua_tointeger(L, -1), 3);
    lua_pop(L, 3);

    mvar_get(L, MOD_UUID, "Version");
    ASSERT_EQ(lua_tointeger(L, -1), 42);
    lua_pop(L, 1);

    uvar_get(L, "guid-1", "MyMod_State");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "hp");
    ASSERT_EQ(lua_tointeger(L, -1), 17);
    lua_pop(L, 2);

    teardown(L);
}

/* The dirty flags only fire on mvar_set/uvar_set. A mod that writes into the
 * cached table (Vars.SpellOwners[id] = {...}) sets none of them, and gating the
 * write on them would drop exactly the mutation pattern real mods use. */
TEST(in_place_mutation_persists_without_a_dirty_flag) {
    lua_State *L = fresh_state("inplace");
    register_spell_owners();
    vars_persist_attach_key(L, "SaveOne");

    lua_newtable(L);
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);
    vars_persist_flush_now(L);

    /* Vars.SpellOwners["spell-b"] = "char-9" — no setter involved. */
    mvar_get(L, MOD_UUID, "SpellOwners");
    lua_pushstring(L, "char-9");
    lua_setfield(L, -2, "spell-b");
    lua_pop(L, 1);

    vars_persist_flush_now(L);
    vars_persist_attach_key(L, "SaveOne");

    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "spell-b");
    ASSERT_STR_EQ(lua_tostring(L, -1), "char-9");
    lua_pop(L, 2);

    teardown(L);
}

/* The whole point of keying by savegame: two playthroughs must not see each
 * other's variables, and switching back must not have lost anything. */
TEST(savegames_do_not_share_variables) {
    lua_State *L = fresh_state("isolation");
    register_spell_owners();

    vars_persist_attach_key(L, "SaveOne");
    lua_newtable(L);
    lua_pushstring(L, "char-1");
    lua_setfield(L, -2, "spell-a");
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);
    vars_persist_flush_now(L);

    vars_persist_attach_key(L, "SaveTwo");
    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_isnil(L, -1));
    lua_pop(L, 1);

    vars_persist_attach_key(L, "SaveOne");
    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "spell-a");
    ASSERT_STR_EQ(lua_tostring(L, -1), "char-1");
    lua_pop(L, 2);

    teardown(L);
}

/* mvar_get answers from the identity cache before it looks at the stored JSON,
 * so a restore that left the cache alone would hand the mod the table it built
 * for the savegame it just left. */
TEST(restore_is_not_shadowed_by_the_cached_table) {
    lua_State *L = fresh_state("shadow");
    register_spell_owners();
    vars_persist_attach_key(L, "SaveOne");

    lua_newtable(L);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "generation");
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);
    vars_persist_flush_now(L);

    /* Keep playing: the live table now says generation = 2 and is cached. */
    mvar_get(L, MOD_UUID, "SpellOwners");
    lua_pushinteger(L, 2);
    lua_setfield(L, -2, "generation");
    lua_pop(L, 1);

    /* Reload the savegame as it was written. */
    vars_persist_attach_key(L, "SaveOne");

    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "generation");
    ASSERT_EQ(lua_tointeger(L, -1), 1);
    lua_pop(L, 2);

    teardown(L);
}

/* Identity stability must survive the round trip: a restored table has to stay
 * the same object across reads, or every nested write after a load is lost. */
TEST(restored_tables_stay_identity_stable) {
    lua_State *L = fresh_state("identity");
    register_spell_owners();
    vars_persist_attach_key(L, "SaveOne");

    lua_newtable(L);
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);
    vars_persist_flush_now(L);
    vars_persist_attach_key(L, "SaveOne");

    mvar_get(L, MOD_UUID, "SpellOwners");
    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_rawequal(L, -1, -2));

    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "nested");
    lua_pop(L, 2);

    mvar_get(L, MOD_UUID, "SpellOwners");
    lua_getfield(L, -1, "nested");
    ASSERT_TRUE(lua_toboolean(L, -1));
    lua_pop(L, 2);

    teardown(L);
}

/* A truncated or hand-edited store must read as "no variables", never as a
 * partial restore that looks like real data, and never as a crash. */
TEST(malformed_store_degrades_to_no_variables) {
    lua_State *L = fresh_state("malformed");
    register_spell_owners();

    /* Seed live state so a failed restore that forgot to clear would show. */
    vars_persist_attach_key(L, "SaveOne");
    lua_newtable(L);
    lua_pushstring(L, "stale");
    lua_setfield(L, -2, "spell-a");
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);

    write_raw_store("Broken", "{\"version\":1,\"mods\":{\"aee-uuid\":{\"vars\":{\"Spell");
    vars_persist_attach_key(L, "Broken");

    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_isnil(L, -1));
    lua_pop(L, 1);

    /* And the unreadable file is set aside rather than silently overwritten. */
    char bad[700];
    snprintf(bad, sizeof(bad), "%s/Broken.json.bad", s_dir);
    struct stat st;
    ASSERT_EQ(stat(bad, &st), 0);

    teardown(L);
}

/* A store from a future (or corrupted) schema version must not be applied
 * field-by-field on a best-effort basis. */
TEST(unknown_store_version_is_rejected) {
    lua_State *L = fresh_state("version");
    register_spell_owners();

    write_raw_store("Future",
                    "{\"version\":999,\"mods\":{\"aee-uuid\":"
                    "{\"flags\":{\"SpellOwners\":513},"
                    "\"vars\":{\"SpellOwners\":{\"spell-a\":\"char-1\"}}}}}");
    vars_persist_attach_key(L, "Future");

    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_isnil(L, -1));
    lua_pop(L, 1);

    teardown(L);
}

/* First load of a savegame nobody has played with SE installed. */
TEST(missing_store_starts_empty_and_still_persists) {
    lua_State *L = fresh_state("missing");
    register_spell_owners();

    vars_persist_attach_key(L, "NeverSeen");
    ASSERT_FALSE(store_exists("NeverSeen"));

    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_isnil(L, -1));
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "count");
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);

    vars_persist_flush_now(L);
    ASSERT_TRUE(store_exists("NeverSeen"));

    teardown(L);
}

/* Nothing may be written before a savegame is attached: a flush at the main
 * menu would otherwise stamp an empty variable set onto whichever save the
 * resolver last named. */
TEST(detached_flush_writes_nothing) {
    lua_State *L = fresh_state("detached");
    register_spell_owners();
    vars_persist_detach();

    lua_newtable(L);
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);

    vars_persist_flush_now(L);
    ASSERT_STR_EQ(vars_persist_current_key(), "");
    ASSERT_FALSE(store_exists("SaveOne"));

    teardown(L);
}

/* Non-persistent variables are session state; persisting them would resurrect
 * a mod's scratch data on load. */
TEST(non_persistent_variables_are_not_stored) {
    lua_State *L = fresh_state("nonpersistent");
    mvar_register_prototype(MOD_UUID, "Scratch",
                            UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_WRITEABLE_SERVER);
    vars_persist_attach_key(L, "SaveOne");

    lua_pushstring(L, "transient");
    mvar_set(L, MOD_UUID, "Scratch", lua_gettop(L));
    lua_pop(L, 1);

    vars_persist_flush_now(L);
    vars_persist_attach_key(L, "SaveOne");

    mvar_get(L, MOD_UUID, "Scratch");
    ASSERT_TRUE(lua_isnil(L, -1));
    lua_pop(L, 1);

    teardown(L);
}

/* Starting a brand-new campaign in the same process must not inherit the
 * campaign the player just left: nothing is attached yet, so whatever is still
 * in memory belongs to the previous savegame and would land in the new
 * campaign's first save. */
TEST(new_campaign_does_not_inherit_the_previous_one) {
    lua_State *L = fresh_state("newcampaign");
    register_spell_owners();

    vars_persist_attach_key(L, "SaveOne");
    lua_newtable(L);
    lua_pushstring(L, "char-1");
    lua_setfield(L, -2, "spell-a");
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);
    vars_persist_flush_now(L);

    /* Back to the menu, then "New Game": the story reloads, then gameplay
     * starts with no savegame to attach to. */
    vars_persist_on_session_begin(L);
    vars_persist_on_level_started(L);

    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_isnil(L, -1));
    lua_pop(L, 1);

    /* The savegame we left is untouched. */
    vars_persist_attach_key(L, "SaveOne");
    mvar_get(L, MOD_UUID, "SpellOwners");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "spell-a");
    ASSERT_STR_EQ(lua_tostring(L, -1), "char-1");
    lua_pop(L, 2);

    teardown(L);
}

void register_vars_persist_tests(void) {
    printf("Ext.Vars savegame persistence:\n");
    RUN_TEST(round_trip_restores_scalars_and_nested_tables);
    RUN_TEST(in_place_mutation_persists_without_a_dirty_flag);
    RUN_TEST(savegames_do_not_share_variables);
    RUN_TEST(restore_is_not_shadowed_by_the_cached_table);
    RUN_TEST(restored_tables_stay_identity_stable);
    RUN_TEST(malformed_store_degrades_to_no_variables);
    RUN_TEST(unknown_store_version_is_rejected);
    RUN_TEST(missing_store_starts_empty_and_still_persists);
    RUN_TEST(detached_flush_writes_nothing);
    RUN_TEST(non_persistent_variables_are_not_stored);
    RUN_TEST(new_campaign_does_not_inherit_the_previous_one);
    printf("\n");
}
