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
 *
 * They also pin the timing, which is half the contract. The store is a SNAPSHOT
 * of the moment the game wrote a save, so a reload rewinds to it: what a mod
 * changes afterwards is not in the store, and must not come back. It used to be
 * a 5-second timer, and Sit This One Out 2 broke on exactly that — the store
 * held LeftCombat entries the mod had since cleaned up, so reloading (the
 * player's fix) restored the state they were trying to clear.
 */

#include "test_harness.h"
#include "user_variables.h"
#include "vars_persist.h"

#include <lauxlib.h>
#include <lualib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

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

/* The game wrote the attached savegame. This is the ONLY thing that writes the
 * store, so every "and then it was saved" below goes through here. */
static void game_saved(lua_State *L) {
    vars_persist_on_save_written(L, NULL);
}

// ---------------------------------------------------------------------------
// Fake savegame directory — drives the real save-detection scan.
// ---------------------------------------------------------------------------

static char s_home[512];
static char s_home_prev[512];
static bool s_home_overridden;

static void mkdir_p(const char *path) {
    char tmp[900];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        (void)mkdir(tmp, 0755);
        *p = '/';
    }
    (void)mkdir(tmp, 0755);
}

static void story_dir(char *out, size_t n) {
    snprintf(out, n,
             "%s/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles"
             "/Public/Savegames/Story", s_home);
}

/* Point the savegame resolver at a directory this test owns. HOME is the only
 * input it has; the store directory is overridden separately, so nothing here
 * can reach the developer's real profile. */
static void fake_home_init(const char *name) {
    const char *prev = getenv("HOME");
    snprintf(s_home_prev, sizeof(s_home_prev), "%s", prev ? prev : "");
    snprintf(s_home, sizeof(s_home), "/tmp/bg3se_vars_home_%s_%d", name, (int)getpid());

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_home);
    (void)system(cmd);

    char story[900];
    story_dir(story, sizeof(story));
    mkdir_p(story);

    setenv("HOME", s_home, 1);
    s_home_overridden = true;
}

static void fake_home_cleanup(void) {
    if (!s_home_overridden) return;
    if (s_home_prev[0]) setenv("HOME", s_home_prev, 1);
    else                unsetenv("HOME");
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_home);
    (void)system(cmd);
    s_home_overridden = false;
}

/* "<prefix>__<stem>" holds "<stem>.lsv", exactly as BG3 lays a save out. The
 * timestamp is explicit so a test never depends on filesystem granularity. */
static void write_fake_save(const char *folder, const char *payload, time_t when) {
    char story[900];
    story_dir(story, sizeof(story));

    char dir[900];
    snprintf(dir, sizeof(dir), "%s/%s", story, folder);
    mkdir_p(dir);

    const char *stem = folder;
    for (const char *p = folder; *p; p++) {
        if (p[0] == '_' && p[1] == '_') stem = p + 2;
    }

    char path[900];
    snprintf(path, sizeof(path), "%s/%s.lsv", dir, stem);
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fputs(payload, f);
    fclose(f);

    struct timespec times[2];
    times[0].tv_sec = when;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    ASSERT_EQ(utimensat(AT_FDCWD, path, times, 0), 0);
}

static time_t fake_save_dir_mtime(const char *folder) {
    char story[900], dir[900];
    story_dir(story, sizeof(story));
    snprintf(dir, sizeof(dir), "%s/%s", story, folder);
    struct stat st;
    ASSERT_EQ(stat(dir, &st), 0);
    return st.st_mtime;
}

/* One scan, now: the tick throttles detection to once a second so that a
 * profile with hundreds of saves is not re-stat'ed on every Osiris event. */
static void scan_now(lua_State *L) {
    vars_persist_reset_scan_throttle();
    vars_persist_tick(L);
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
    fake_home_cleanup();
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

    game_saved(L);
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
    game_saved(L);

    /* Vars.SpellOwners["spell-b"] = "char-9" — no setter involved. */
    mvar_get(L, MOD_UUID, "SpellOwners");
    lua_pushstring(L, "char-9");
    lua_setfield(L, -2, "spell-b");
    lua_pop(L, 1);

    game_saved(L);
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
    game_saved(L);

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
    game_saved(L);

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
    game_saved(L);
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

    game_saved(L);
    ASSERT_TRUE(store_exists("NeverSeen"));

    teardown(L);
}

/* Nothing may be written while no savegame is attached: a snapshot taken with
 * no key would otherwise stamp an empty variable set onto whichever save the
 * resolver last named. */
TEST(detached_save_writes_nothing) {
    lua_State *L = fresh_state("detached");
    register_spell_owners();
    vars_persist_detach();

    lua_newtable(L);
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);

    game_saved(L);
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

    game_saved(L);
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
    game_saved(L);

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

// ---------------------------------------------------------------------------
// Snapshot timing. The store must correspond to the save it is named after.
// ---------------------------------------------------------------------------

/* Sit This One Out 2 keeps Vars.LeftCombat[uuid][combat] while a character sits
 * out a fight and drops LeftCombat[uuid] on CombatEnded. DoSitOut refuses to
 * apply its status while that table is non-empty, so a store holding entries
 * the save did not have permanently disables the mod — and reloading, the
 * player's way out, is what restored them. */
static void register_left_combat(void) {
    mvar_register_prototype(MOD_UUID, "LeftCombat",
                            UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_WRITEABLE_SERVER |
                            UVAR_FLAG_PERSISTENT);
}

/* LeftCombat = { ["char"] = { [combat] = true } }, or an empty table. */
static void set_left_combat(lua_State *L, const char *combat) {
    lua_newtable(L);
    if (combat) {
        lua_newtable(L);
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, combat);
        lua_setfield(L, -2, "char-1");
    }
    mvar_set(L, MOD_UUID, "LeftCombat", lua_gettop(L));
    lua_pop(L, 1);
}

static bool left_combat_has(lua_State *L, const char *combat) {
    mvar_get(L, MOD_UUID, "LeftCombat");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
    lua_getfield(L, -1, "char-1");
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return false; }
    lua_getfield(L, -1, combat);
    bool has = lua_toboolean(L, -1);
    lua_pop(L, 3);
    return has;
}

TEST(changes_made_after_the_save_are_not_in_the_snapshot) {
    lua_State *L = fresh_state("aftersave");
    register_left_combat();
    vars_persist_attach_key(L, "SaveOne");

    /* The state the player saved: nobody is sitting out. */
    set_left_combat(L, NULL);
    game_saved(L);

    /* Play on. Three combats accumulate, and none of them is in the save. */
    set_left_combat(L, "combat-1");
    ASSERT_TRUE(left_combat_has(L, "combat-1"));

    /* Reload. This is a rewind, so the post-save entries must be gone. */
    vars_persist_attach_key(L, "SaveOne");
    ASSERT_FALSE(left_combat_has(L, "combat-1"));

    teardown(L);
}

TEST(the_snapshot_keeps_what_the_save_held_even_after_the_mod_clears_it) {
    lua_State *L = fresh_state("atsavetime");
    register_left_combat();
    vars_persist_attach_key(L, "SaveOne");

    /* Saved mid-combat, with the character sitting out. */
    set_left_combat(L, "combat-1");
    game_saved(L);

    /* CombatEnded later drops the whole table — after the save, so it does not
     * belong to it. The old timer wrote this out and the save lost its state. */
    set_left_combat(L, NULL);

    vars_persist_attach_key(L, "SaveOne");
    ASSERT_TRUE(left_combat_has(L, "combat-1"));

    teardown(L);
}

// ---------------------------------------------------------------------------
// Save detection. These drive the real scan against a savegame directory laid
// out the way BG3 lays one out.
// ---------------------------------------------------------------------------

#define SAVE_ONE "Illidan A-11112620812__QuickSave_1"
#define SAVE_TWO "Illidan A-11112620813__QuickSave_2"

static void set_generation(lua_State *L, int generation) {
    lua_newtable(L);
    lua_pushinteger(L, generation);
    lua_setfield(L, -2, "generation");
    mvar_set(L, MOD_UUID, "SpellOwners", lua_gettop(L));
    lua_pop(L, 1);
}

static lua_Integer generation(lua_State *L) {
    mvar_get(L, MOD_UUID, "SpellOwners");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return -1; }
    lua_getfield(L, -1, "generation");
    lua_Integer g = lua_tointeger(L, -1);
    lua_pop(L, 2);
    return g;
}

/* A savegame appearing on disk is the trigger; a tick on its own is not. */
TEST(the_snapshot_is_taken_when_the_game_writes_a_save) {
    lua_State *L = fresh_state("savewatch");
    fake_home_init("savewatch");
    register_spell_owners();
    vars_persist_on_level_started(L);
    time_t base = time(NULL);

    set_generation(L, 1);
    scan_now(L);
    ASSERT_STR_EQ(vars_persist_current_key(), "");
    ASSERT_FALSE(store_exists(SAVE_ONE));

    write_fake_save(SAVE_ONE, "lsv", base + 10);
    scan_now(L);
    ASSERT_STR_EQ(vars_persist_current_key(), SAVE_ONE);
    ASSERT_TRUE(store_exists(SAVE_ONE));

    /* Keep playing. No save, so no scan may write — losing this is correct:
     * it matches the savegame, which does not have it either. */
    set_generation(L, 2);
    scan_now(L);
    scan_now(L);

    vars_persist_attach_key(L, SAVE_ONE);
    ASSERT_EQ(generation(L), 1);

    teardown(L);
}

/* Overwriting a slot creates no folder: BG3 can rewrite the .lsv in place and
 * leave the folder's own timestamps untouched (observed on a real profile —
 * QuickSave_69's payload is an hour newer than its folder). Adoption keyed on
 * folder creation misses that save entirely. */
TEST(overwriting_an_existing_save_slot_still_snapshots) {
    lua_State *L = fresh_state("overwrite");
    fake_home_init("overwrite");
    register_spell_owners();
    vars_persist_on_level_started(L);
    time_t base = time(NULL);

    set_generation(L, 1);
    write_fake_save(SAVE_ONE, "lsv", base + 10);
    scan_now(L);
    ASSERT_EQ(generation(L), 1);

    set_generation(L, 2);
    time_t folder_before = fake_save_dir_mtime(SAVE_ONE);
    write_fake_save(SAVE_ONE, "lsv-overwritten", base + 20);
    ASSERT_EQ(fake_save_dir_mtime(SAVE_ONE), folder_before);

    scan_now(L);
    vars_persist_attach_key(L, SAVE_ONE);
    ASSERT_EQ(generation(L), 2);

    teardown(L);
}

/* Saving into a new slot moves the store with it, and leaves the save the
 * player started from exactly as they left it. */
TEST(saving_to_a_new_slot_moves_the_store_and_leaves_the_old_one) {
    lua_State *L = fresh_state("newslot");
    fake_home_init("newslot");
    register_spell_owners();
    vars_persist_on_level_started(L);
    time_t base = time(NULL);

    set_generation(L, 1);
    write_fake_save(SAVE_ONE, "lsv", base + 10);
    scan_now(L);
    ASSERT_STR_EQ(vars_persist_current_key(), SAVE_ONE);

    set_generation(L, 2);
    write_fake_save(SAVE_TWO, "lsv", base + 20);
    scan_now(L);
    ASSERT_STR_EQ(vars_persist_current_key(), SAVE_TWO);

    vars_persist_attach_key(L, SAVE_TWO);
    ASSERT_EQ(generation(L), 2);
    vars_persist_attach_key(L, SAVE_ONE);
    ASSERT_EQ(generation(L), 1);

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
    RUN_TEST(detached_save_writes_nothing);
    RUN_TEST(non_persistent_variables_are_not_stored);
    RUN_TEST(new_campaign_does_not_inherit_the_previous_one);
    RUN_TEST(changes_made_after_the_save_are_not_in_the_snapshot);
    RUN_TEST(the_snapshot_keeps_what_the_save_held_even_after_the_mod_clears_it);
    RUN_TEST(the_snapshot_is_taken_when_the_game_writes_a_save);
    RUN_TEST(overwriting_an_existing_save_slot_still_snapshots);
    RUN_TEST(saving_to_a_new_slot_moves_the_store_and_leaves_the_old_one);
    printf("\n");
}
