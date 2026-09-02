/**
 * BG3SE-macOS - Ext.Vars savegame persistence
 *
 * Why a sidecar and not the savegame
 * ----------------------------------
 * Windows BG3SE hooks esv::OsirisVariableHelper::SavegameVisit and appends a
 * ScriptExtenderSave region to the savegame's own LSF payload. That seam does
 * exist in this build: the symbol sits at preferred VA 0x104b51a9c and its
 * first 16 bytes still match savegame_hook.c's prologue gate byte-for-byte on
 * 4.1.1.7398727 (re-derived with nm + a raw slice read; see
 * ghidra/offsets/SAVEGAME_HOOK_SURFACE.md). Two things still rule it out here:
 *
 *   1. It is only statically verified. The gate in savegame_hook.c wants a
 *      repeatable runtime observation of both directions before it arms, and
 *      that observation has not been made on this build.
 *   2. It runs on the save worker thread (esv::SaveSystem::DoSaveFlow is a WT
 *      job). Serializing variables there means calling into the server
 *      lua_State while the game thread may be inside it — the exact race the
 *      Lua gate exists to prevent. The sidecar path does all of its Lua work on
 *      the Osiris event thread under that gate instead.
 *
 * Identifying the savegame
 * ------------------------
 * A single global file lets one playthrough overwrite another's variables (the
 * profile here holds saves for five different characters), so the store is
 * keyed by savegame folder name. Without the hook there is no callback that
 * names the save, so the name is recovered from the game's own save directory:
 *
 *   restore  the save whose .lsv was most recently READ (max of atime/mtime),
 *            considering only files touched after this process started. The
 *            game reads the .lsv to load it, and it does so after the load
 *            menu has finished enumerating, so the loaded save is last.
 *   persist  the save directory most recently CREATED after we attached. That
 *            is the player saving, and the live variables must carry forward
 *            into the new file.
 *
 * Limitation, stated plainly: atime is a heuristic. If it cannot single out a
 * save the store stays detached and nothing is written — the failure mode is
 * "no variables", never another savegame's variables. BG3SE_VARS_STORE_KEY
 * overrides the resolver entirely.
 *
 * Threading: every entry point runs on the Osiris event thread while main.c
 * holds the Lua gate. There is no lock here on purpose.
 */

#include "vars_persist.h"
#include "user_variables.h"
#include "../lua/lua_json.h"
#include "../core/logging.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lauxlib.h>

// ============================================================================
// Constants
// ============================================================================

#define VARS_KEY_MAX 192
#define VARS_FLUSH_INTERVAL_MS 5000
#define VARS_STORE_MAX_BYTES (16 * 1024 * 1024)

// ============================================================================
// State
// ============================================================================

static char s_store_dir[PATH_MAX];
static char s_key[VARS_KEY_MAX];        // "" when detached
static time_t s_watch_since;            // adopt only save folders created at/after this
static time_t s_process_start;
static bool s_session_active;
static uint64_t s_last_flush_ms;
static char *s_last_written;            // last payload written, for change detection

// ============================================================================
// Small helpers
// ============================================================================

static uint64_t monotonic_ms(void) {
    static mach_timebase_info_data_t tb = {0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (mach_absolute_time() * tb.numer) / (tb.denom * 1000000ULL);
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    (void)mkdir(path, 0755);
}

static const char *store_dir(void) {
    if (s_store_dir[0] == '\0') {
        const char *home = getenv("HOME");
        if (!home) return NULL;
        char support[PATH_MAX];
        snprintf(support, sizeof(support), "%s/Library/Application Support/BG3SE", home);
        ensure_dir(support);
        snprintf(s_store_dir, sizeof(s_store_dir), "%s/SaveVars", support);
    }
    ensure_dir(s_store_dir);
    return s_store_dir;
}

/* Savegame names carry spaces and are otherwise path-safe, but they come from
 * the filesystem and end up in a path we build, so anything that could escape
 * the store directory is folded away. */
static void sanitize_key(const char *in, char *out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-' || c == ' ';
        out[j++] = ok ? (char)c : '_';
    }
    out[j] = '\0';
}

static bool store_path(const char *key, char *out, size_t n) {
    const char *dir = store_dir();
    if (!dir || !key || key[0] == '\0') return false;
    char safe[VARS_KEY_MAX];
    sanitize_key(key, safe, sizeof(safe));
    if (safe[0] == '\0') return false;
    return snprintf(out, n, "%s/%s.json", dir, safe) < (int)n;
}

// ============================================================================
// Savegame discovery
// ============================================================================

/* A save folder holds exactly "<stem>.lsv" and "<stem>.WebP", where <stem> is
 * the folder name after the last "__" ("Illidan A-55112...__QuickSave_20" ->
 * "QuickSave_20.lsv") or the whole folder name for a named manual save. Building
 * the path costs one stat instead of an opendir per save, which matters when a
 * profile holds hundreds of them; readdir is the fallback if the guess misses. */
static bool stat_savegame_payload(const char *save_dir, const char *save_name,
                                  struct stat *out) {
    const char *stem = save_name;
    for (const char *p = save_name; *p; p++) {
        if (p[0] == '_' && p[1] == '_') stem = p + 2;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s.lsv", save_dir, stem) < (int)sizeof(path) &&
        stat(path, out) == 0) {
        return true;
    }

    DIR *d = opendir(save_dir);
    if (!d) return false;
    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".lsv") != 0) continue;
        if (snprintf(path, sizeof(path), "%s/%s", save_dir, e->d_name) < (int)sizeof(path) &&
            stat(path, out) == 0) {
            found = true;
        }
        break;
    }
    closedir(d);
    return found;
}

typedef enum {
    PICK_MOST_RECENTLY_READ,     /* the save that was just loaded */
    PICK_MOST_RECENTLY_CREATED   /* the save the player just made */
} SavegamePick;

static bool scan_story_dir(const char *story_dir, SavegamePick pick,
                           time_t not_before, char *out, size_t out_n,
                           time_t *best_score, time_t *out_created) {
    DIR *d = opendir(story_dir);
    if (!d) return false;

    bool found = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        char save_dir[PATH_MAX];
        if (snprintf(save_dir, sizeof(save_dir), "%s/%s", story_dir, e->d_name)
            >= (int)sizeof(save_dir)) continue;

        struct stat dir_st;
        if (stat(save_dir, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) continue;

        time_t score;
        if (pick == PICK_MOST_RECENTLY_CREATED) {
            score = dir_st.st_mtime;
        } else {
            struct stat lsv_st;
            if (!stat_savegame_payload(save_dir, e->d_name, &lsv_st)) continue;
            score = lsv_st.st_atime > lsv_st.st_mtime ? lsv_st.st_atime : lsv_st.st_mtime;
        }

        // *best_score carries across profiles, so compare against it rather
        // than a per-directory "first hit" — otherwise the first candidate in
        // the second profile would displace a better one from the first.
        if (score < not_before) continue;
        if (score <= *best_score) continue;

        *best_score = score;
        *out_created = dir_st.st_mtime;
        snprintf(out, out_n, "%s", e->d_name);
        found = true;
    }

    closedir(d);
    return found;
}

static bool resolve_savegame(SavegamePick pick, time_t not_before,
                             char *out, size_t out_n, time_t *out_created) {
    const char *home = getenv("HOME");
    if (!home) return false;
    if (not_before < 1) not_before = 1;   /* keeps "best so far = 0" meaningful */

    char profiles[PATH_MAX];
    if (snprintf(profiles, sizeof(profiles),
                 "%s/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles", home)
        >= (int)sizeof(profiles)) return false;

    DIR *d = opendir(profiles);
    if (!d) return false;

    bool found = false;
    time_t best = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char story[PATH_MAX];
        if (snprintf(story, sizeof(story), "%s/%s/Savegames/Story", profiles, e->d_name)
            >= (int)sizeof(story)) continue;
        if (scan_story_dir(story, pick, not_before, out, out_n, &best, out_created)) {
            found = true;
        }
    }

    closedir(d);
    return found;
}

// ============================================================================
// Store I/O
// ============================================================================

/* Build the payload and push it as a JSON string. Returns the string length. */
static size_t build_payload(lua_State *L, const char **out_json) {
    uvar_store_build(L);
    int root = lua_gettop(L);

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    json_stringify_value(L, root, &b);
    luaL_pushresult(&b);

    size_t len = 0;
    const char *json = lua_tolstring(L, -1, &len);
    lua_remove(L, root);   /* leave only the string on the stack */
    *out_json = json;
    return len;
}

static bool write_atomic(const char *path, const char *data, size_t len) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;

    FILE *f = fopen(tmp, "w");
    if (!f) {
        LOG_PERSIST_ERROR("Ext.Vars: cannot open %s: %s", tmp, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) {
        LOG_PERSIST_ERROR("Ext.Vars: cannot write %s: %s", path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

static void remember_payload(const char *json, size_t len) {
    free(s_last_written);
    s_last_written = malloc(len + 1);
    if (s_last_written) {
        memcpy(s_last_written, json, len);
        s_last_written[len] = '\0';
    }
}

static void flush_locked(lua_State *L) {
    if (!L || s_key[0] == '\0') return;

    char path[PATH_MAX];
    if (!store_path(s_key, path, sizeof(path))) return;

    const char *json = NULL;
    size_t len = build_payload(L, &json);
    if (!json) { lua_pop(L, 1); return; }

    // Dirty flags only catch mvar_set/uvar_set; a mod that mutates a cached
    // table in place (Vars.Foo[k] = v) never sets one. Comparing the rendered
    // payload is what makes that case persist, and it is also what keeps the
    // tick from rewriting an unchanged file every 5 seconds.
    if (s_last_written && strcmp(s_last_written, json) == 0) {
        lua_pop(L, 1);
        return;
    }

    if (write_atomic(path, json, len)) {
        remember_payload(json, len);
        LOG_PERSIST_INFO("Ext.Vars: wrote %zu bytes for savegame '%s'", len, s_key);
    }
    lua_pop(L, 1);
}

/* Move a store we could not parse aside instead of letting the next flush
 * overwrite it. A malformed store may still be the only copy of a player's
 * mod state, and "degrade to no variables" must not also mean "destroy". */
static void quarantine_store(const char *path) {
    char bad[PATH_MAX];
    if (snprintf(bad, sizeof(bad), "%s.bad", path) >= (int)sizeof(bad)) return;
    if (rename(path, bad) == 0) {
        LOG_PERSIST_WARN("Ext.Vars: moved unreadable store aside as %s", bad);
    }
}

/* Replace the variable set with the store for `s_key`. Any failure leaves the
 * set EMPTY rather than carrying the previous savegame's values forward. */
static void restore_locked(lua_State *L) {
    char path[PATH_MAX];
    if (!store_path(s_key, path, sizeof(path))) {
        uvar_store_clear(L);
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        uvar_store_clear(L);
        free(s_last_written);
        s_last_written = NULL;
        LOG_PERSIST_INFO("Ext.Vars: no store yet for savegame '%s'", s_key);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > VARS_STORE_MAX_BYTES) {
        fclose(f);
        uvar_store_clear(L);
        free(s_last_written);
        s_last_written = NULL;
        LOG_PERSIST_WARN("Ext.Vars: store for '%s' has implausible size %ld; ignored",
                         s_key, size);
        quarantine_store(path);
        return;
    }

    char *json = malloc((size_t)size + 1);
    if (!json) {
        fclose(f);
        uvar_store_clear(L);
        return;
    }
    size_t got = fread(json, 1, (size_t)size, f);
    fclose(f);
    json[got] = '\0';

    int base = lua_gettop(L);
    const char *end = json_parse_value(L, json);
    bool ok = end != NULL && lua_gettop(L) > base && lua_istable(L, -1);

    if (ok) {
        lua_getfield(L, -1, "version");
        lua_Integer version = lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (version != UVAR_STORE_VERSION) {
            LOG_PERSIST_WARN("Ext.Vars: store for '%s' is version %lld, expected %d; ignored",
                             s_key, (long long)version, UVAR_STORE_VERSION);
            ok = false;
        }
    }

    if (ok) {
        uvar_store_apply(L, lua_gettop(L));
        remember_payload(json, got);
        LOG_PERSIST_INFO("Ext.Vars: restored %zu bytes for savegame '%s'", got, s_key);
    } else {
        uvar_store_clear(L);
        free(s_last_written);
        s_last_written = NULL;
        LOG_PERSIST_WARN("Ext.Vars: store for '%s' is malformed; starting empty", s_key);
        quarantine_store(path);
    }

    lua_settop(L, base);
    free(json);
}

static void attach(lua_State *L, const char *key) {
    snprintf(s_key, sizeof(s_key), "%s", key);
    // Only save folders that appear from here on are "the player just saved".
    // Anchoring on the attached save's own mtime instead would make loading an
    // older save immediately adopt whatever newer save happens to sit in the
    // profile, and write this campaign's variables into it.
    s_watch_since = time(NULL);
    restore_locked(L);
}

// ============================================================================
// Public API
// ============================================================================

void vars_persist_init(void) {
    s_process_start = time(NULL);
    s_key[0] = '\0';
    s_watch_since = s_process_start;
    s_session_active = false;
    s_last_flush_ms = 0;
}

void vars_persist_on_session_begin(lua_State *L) {
    // COsiris::Load rebuilds the story exactly once per session, so this is the
    // one point that reliably separates two campaigns. Detaching here is what
    // stops the tick from writing the outgoing campaign's variables into the
    // savegame it was attached to while the next one loads; clearing is what
    // stops "New Game" straight after a session from inheriting its values.
    s_key[0] = '\0';
    s_watch_since = time(NULL);
    s_session_active = false;
    free(s_last_written);
    s_last_written = NULL;
    if (L) uvar_store_clear(L);
}

void vars_persist_on_savegame_loaded(lua_State *L) {
    if (!L) return;
    s_session_active = true;

    const char *override = getenv("BG3SE_VARS_STORE_KEY");
    if (override && override[0]) {
        attach(L, override);
        LOG_PERSIST_INFO("Ext.Vars: BG3SE_VARS_STORE_KEY pins the store to '%s'", s_key);
        return;
    }

    char name[VARS_KEY_MAX];
    time_t created = 0;
    if (resolve_savegame(PICK_MOST_RECENTLY_READ, s_process_start,
                         name, sizeof(name), &created)) {
        attach(L, name);
        return;
    }

    // Nothing readable was touched since launch, so we cannot say which save
    // this is. Persisting under a guess would hand the next campaign this one's
    // variables, so stay detached and let the first save of the session (which
    // IS unambiguous — it did not exist a moment ago) attach us instead.
    s_key[0] = '\0';
    s_watch_since = time(NULL);
    uvar_store_clear(L);
    free(s_last_written);
    s_last_written = NULL;
    LOG_PERSIST_WARN("Ext.Vars: could not identify the loaded savegame; "
                     "variables will not persist until the next save");
}

void vars_persist_on_level_started(lua_State *L) {
    (void)L;
    // Only marks the session live. This also fires on level transitions inside
    // a session, so it must never clear: vars_persist_on_session_begin already
    // did that at the one point where it is safe.
    s_session_active = true;
}

void vars_persist_tick(lua_State *L) {
    if (!L || !s_session_active) return;

    uint64_t now_ms = monotonic_ms();
    if (s_last_flush_ms != 0 && now_ms - s_last_flush_ms < VARS_FLUSH_INTERVAL_MS) return;
    s_last_flush_ms = now_ms;

    // Adopt a save folder that appeared after we attached: that is the player
    // having saved, and the live variables have to carry forward into the new
    // file. Deliberately keyed on CREATION, not on the read heuristic — a save
    // being loaded is older than s_watch_since, so this can never take the key
    // away from vars_persist_on_savegame_loaded before it has restored.
    char name[VARS_KEY_MAX];
    time_t created = 0;
    if (resolve_savegame(PICK_MOST_RECENTLY_CREATED, s_watch_since,
                         name, sizeof(name), &created) &&
        strcmp(name, s_key) != 0) {
        snprintf(s_key, sizeof(s_key), "%s", name);
        time_t now = time(NULL);
        s_watch_since = (created + 1 > now) ? created + 1 : now;
        free(s_last_written);
        s_last_written = NULL;   /* force a write under the new key */
        LOG_PERSIST_INFO("Ext.Vars: now persisting to savegame '%s'", s_key);
    }

    flush_locked(L);
}

void vars_persist_flush_now(lua_State *L) {
    if (!L || s_key[0] == '\0') return;
    flush_locked(L);
}

const char *vars_persist_current_key(void) {
    return s_key;
}

void vars_persist_set_store_dir(const char *dir) {
    if (dir && dir[0]) {
        snprintf(s_store_dir, sizeof(s_store_dir), "%s", dir);
        ensure_dir(s_store_dir);
    } else {
        s_store_dir[0] = '\0';
    }
}

void vars_persist_attach_key(lua_State *L, const char *key) {
    if (!L || !key || !key[0]) return;
    s_session_active = true;
    attach(L, key);
}

void vars_persist_detach(void) {
    s_key[0] = '\0';
    s_watch_since = time(NULL);
    free(s_last_written);
    s_last_written = NULL;
}
