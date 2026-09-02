/**
 * BG3SE-macOS - Ext.Vars savegame persistence
 *
 * What the store is
 * -----------------
 * A SNAPSHOT of the mod/entity variable set as of the moment the game wrote a
 * savegame, one JSON file per savegame under
 * Application Support/BG3SE/SaveVars/<savegame folder>.json.
 *
 * "As of the save" is the whole contract, not an implementation detail. On
 * Windows these variables live inside the .lsv, so they are captured when the
 * save is taken and rewind with it by construction. A sidecar only behaves like
 * a savegame if it is written at the same instant the savegame is.
 *
 * This used to be a 5-second timer, and that was a real bug, not a rounding
 * error: the file ended up holding whatever was live at the last tick, so
 * loading a save restored post-save state and could not rewind. Observed with
 * Sit This One Out 2, which holds Vars.LeftCombat[uuid][combat] while a
 * character sits out and deletes LeftCombat[uuid] on CombatEnded (SitOut.lua:694):
 * the store carried three combats the mod had already cleaned up, and DoSitOut
 * (SitOut.lua:596) skips its ApplyStatus for anyone with a non-empty
 * LeftCombat[uuid] — so the mod refused to work, and reloading (the player's fix)
 * restored the very state they were clearing.
 *
 * Therefore: NOTHING is written between saves. Do not add a timer, a flush on
 * quit, a flush on shutdown, or an Ext.Vars API that writes on demand. Changes a
 * mod makes after the last save are deliberately lost on crash or on quitting
 * without saving, exactly as every other piece of savegame state is; a store
 * that outlived its save would be worse than no store at all.
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
 * Detecting a save without a hook
 * -------------------------------
 * The tick watches the game's own save directory for a savegame PAYLOAD that
 * was written after we last accounted for one, scoring each save by the newer
 * of the save folder's mtime and its .lsv's mtime:
 *
 *   - a new save folder bumps the folder's mtime AND creates the .lsv;
 *   - overwriting an existing slot can leave the folder untouched entirely.
 *     Real instance in this profile: QuickSave_69's .lsv has mtime 13:27:21
 *     while its folder still reads 12:29:01, an hour earlier — the payload was
 *     rewritten in place. Keying adoption on folder CREATION, as this file used
 *     to, misses that save completely and keeps writing to the previous key.
 *
 * Reads never move mtime, so loading a save can never be mistaken for writing
 * one — which is what keeps the load path (below) and this one apart.
 *
 * Known limitation: anything else that creates or rewrites a save folder while
 * the game runs — a cloud sync restoring another machine's saves, a file copy —
 * looks like a save from here, and the next snapshot would go to that save's
 * store. It is the same exposure the previous folder-creation heuristic had. A
 * savegame hook would close it, and is the reason to revisit one; see below for
 * why there is no hook today.
 *
 * No hook is installed for this, on any build. A code hook would have to be
 * gated on the exact build or it resolves to a different live function, and it
 * could only tell us that *a* save is happening — never which folder it lands
 * in, which is the part the store actually needs. Filesystem observation is
 * build-independent, so there is no address to guess and nothing to fail open:
 * if no save can be identified, no file is written and the store keeps the
 * contents of the last save that was.
 *
 * FSEvents/kqueue were rejected: kqueue reports directory-level changes only
 * (it would miss the in-place rewrite above), and FSEvents needs a CFRunLoop
 * thread whose callbacks could not touch Lua anyway — it would have to hand the
 * work back to this tick, which is where it already happens.
 *
 * Identifying the savegame on load
 * --------------------------------
 * A single global file lets one playthrough overwrite another's variables (the
 * profile here holds saves for five different characters), so the store is
 * keyed by savegame folder name. Without the hook there is no callback that
 * names the save being loaded, so the name is recovered from the game's own
 * save directory: the save whose .lsv was most recently READ (max of
 * atime/mtime), considering only files touched after this process started. The
 * game reads the .lsv to load it, and it does so after the load menu has
 * finished enumerating, so the loaded save is last.
 *
 * Limitation, stated plainly: atime is a heuristic. If it cannot single out a
 * save the store stays detached and nothing is written — the failure mode is
 * "no variables", never another savegame's variables. The next save the player
 * makes is unambiguous (it is a payload write, not a read) and attaches us.
 * BG3SE_VARS_STORE_KEY overrides the resolver entirely.
 *
 * Threading: every entry point runs on the Osiris event thread while main.c
 * holds the Lua gate. Serialization reads Lua state (uvar_store_build goes
 * through mvar_get/uvar_get), so it must not happen anywhere else. There is no
 * lock here on purpose.
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
#define VARS_STORE_MAX_BYTES (16 * 1024 * 1024)

/* How often the tick may re-stat the save directory. This throttles DETECTION,
 * never writing: a scan that finds nothing writes nothing, and the interval is
 * only the worst-case delay between the game finishing a save and the snapshot
 * being taken. A profile with 289 saves costs ~600 stats per scan, so 1 Hz is
 * about 1 ms/s of one core. */
#define VARS_SAVE_SCAN_INTERVAL_MS 1000

// ============================================================================
// State
// ============================================================================

static char s_store_dir[PATH_MAX];
static char s_key[VARS_KEY_MAX];        // "" when detached
static uint64_t s_save_watermark_ns;    // newest save payload write already handled
static uint64_t s_process_start_ns;
static bool s_session_active;
static uint64_t s_last_scan_ms;
static char *s_last_written;            // last payload written, for change detection

// ============================================================================
// Small helpers
// ============================================================================

static uint64_t monotonic_ms(void) {
    static mach_timebase_info_data_t tb = {0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (mach_absolute_time() * tb.numer) / (tb.denom * 1000000ULL);
}

/* Wall clock, in the same units as the file timestamps it is compared against. */
static uint64_t wall_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t timespec_ns(struct timespec ts) {
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* When this file's contents were last written. mtime only, deliberately: ctime
 * also moves for metadata-only changes (an xattr from a backup or indexing
 * tool), and a spurious "this save was just written" is the one failure this
 * file must not have — it would attach the live campaign's variables to some
 * other campaign's save. A save the game writes always carries a fresh mtime. */
static uint64_t stat_written_ns(const struct stat *st) {
    return timespec_ns(st->st_mtimespec);
}

static uint64_t stat_read_ns(const struct stat *st) {
    uint64_t a = timespec_ns(st->st_atimespec);
    uint64_t m = timespec_ns(st->st_mtimespec);
    return a > m ? a : m;
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
    PICK_MOST_RECENTLY_WRITTEN   /* the save the game just wrote */
} SavegamePick;

static bool scan_story_dir(const char *story_dir, SavegamePick pick,
                           char *out, size_t out_n, uint64_t *best_ns) {
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

        struct stat lsv_st;
        bool have_lsv = stat_savegame_payload(save_dir, e->d_name, &lsv_st);

        uint64_t score;
        if (pick == PICK_MOST_RECENTLY_WRITTEN) {
            // Both halves are needed. The folder alone misses a slot whose .lsv
            // was rewritten in place; the .lsv alone misses the instant between
            // the folder appearing and its payload landing.
            score = stat_written_ns(&dir_st);
            if (have_lsv) {
                uint64_t payload = stat_written_ns(&lsv_st);
                if (payload > score) score = payload;
            }
        } else {
            if (!have_lsv) continue;
            score = stat_read_ns(&lsv_st);
        }

        // *best_ns starts at the caller's cutoff and carries across profiles, so
        // comparing against it is both the "newer than we've handled" test and
        // the "beats every other candidate" test. Strictly greater: a save we
        // have already snapshotted must not be snapshotted again.
        if (score <= *best_ns) continue;

        *best_ns = score;
        snprintf(out, out_n, "%s", e->d_name);
        found = true;
    }

    closedir(d);
    return found;
}

static bool resolve_savegame(SavegamePick pick, uint64_t after_ns,
                             char *out, size_t out_n, uint64_t *out_score_ns) {
    const char *home = getenv("HOME");
    if (!home) return false;

    char profiles[PATH_MAX];
    if (snprintf(profiles, sizeof(profiles),
                 "%s/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles", home)
        >= (int)sizeof(profiles)) return false;

    DIR *d = opendir(profiles);
    if (!d) return false;

    bool found = false;
    uint64_t best = after_ns;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char story[PATH_MAX];
        if (snprintf(story, sizeof(story), "%s/%s/Savegames/Story", profiles, e->d_name)
            >= (int)sizeof(story)) continue;
        if (scan_story_dir(story, pick, out, out_n, &best)) {
            found = true;
        }
    }

    closedir(d);
    if (found && out_score_ns) *out_score_ns = best;
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

/* Serialize the live variable set into the attached save's store. Reads Lua
 * state, so this only ever runs on the game thread under the Lua gate. */
static void snapshot_locked(lua_State *L) {
    if (!L || s_key[0] == '\0') return;

    char path[PATH_MAX];
    if (!store_path(s_key, path, sizeof(path))) return;

    const char *json = NULL;
    size_t len = build_payload(L, &json);
    if (!json) { lua_pop(L, 1); return; }

    // A long save writes its .lsv over several seconds, so one save can be
    // observed by more than one scan. Comparing the rendered payload keeps the
    // repeat snapshots free instead of rewriting an identical file.
    if (s_last_written && strcmp(s_last_written, json) == 0) {
        lua_pop(L, 1);
        return;
    }

    if (write_atomic(path, json, len)) {
        remember_payload(json, len);
        LOG_PERSIST_INFO("Ext.Vars: snapshot of %zu bytes for savegame '%s'", len, s_key);
    }
    lua_pop(L, 1);
}

/* Move a store we could not parse aside instead of letting the next snapshot
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
    // Only save payloads written from here on count as "the game just saved".
    // Anchoring on the attached save's own timestamp instead would make loading
    // an older save immediately adopt whatever newer save happens to sit in the
    // profile, and write this campaign's variables into it.
    s_save_watermark_ns = wall_now_ns();
    restore_locked(L);
}

// ============================================================================
// Public API
// ============================================================================

void vars_persist_init(void) {
    s_process_start_ns = wall_now_ns();
    s_key[0] = '\0';
    s_save_watermark_ns = s_process_start_ns;
    s_session_active = false;
    s_last_scan_ms = 0;
    free(s_last_written);
    s_last_written = NULL;
}

void vars_persist_on_session_begin(lua_State *L) {
    // COsiris::Load rebuilds the story exactly once per session, so this is the
    // one point that reliably separates two campaigns. Detaching here is what
    // stops a save made by the next campaign from being written under the
    // outgoing one's key; clearing is what stops "New Game" straight after a
    // session from inheriting its values.
    s_key[0] = '\0';
    s_save_watermark_ns = wall_now_ns();
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
    if (resolve_savegame(PICK_MOST_RECENTLY_READ, s_process_start_ns,
                         name, sizeof(name), NULL)) {
        attach(L, name);
        return;
    }

    // Nothing readable was touched since launch, so we cannot say which save
    // this is. Persisting under a guess would hand the next campaign this one's
    // variables, so stay detached and let the first save of the session (which
    // IS unambiguous — it is a payload write, not a read) attach us instead.
    s_key[0] = '\0';
    s_save_watermark_ns = wall_now_ns();
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
    if (s_last_scan_ms != 0 && now_ms - s_last_scan_ms < VARS_SAVE_SCAN_INTERVAL_MS) return;
    s_last_scan_ms = now_ms;

    char name[VARS_KEY_MAX];
    uint64_t written_ns = 0;
    if (!resolve_savegame(PICK_MOST_RECENTLY_WRITTEN, s_save_watermark_ns,
                          name, sizeof(name), &written_ns)) {
        return;   // no save since the last one we handled: write nothing
    }

    // Advance before snapshotting: a failed write must not leave this save
    // pending forever, re-firing on every scan for the rest of the session.
    s_save_watermark_ns = written_ns;
    vars_persist_on_save_written(L, name);
}

void vars_persist_on_save_written(lua_State *L, const char *key) {
    if (!L) return;
    s_session_active = true;

    if (key && key[0] && strcmp(key, s_key) != 0) {
        snprintf(s_key, sizeof(s_key), "%s", key);
        free(s_last_written);
        s_last_written = NULL;   /* force a write under the new key */
        LOG_PERSIST_INFO("Ext.Vars: now persisting to savegame '%s'", s_key);
    }

    // Detached: the save could not be named, so there is nowhere this snapshot
    // could go that would not be a guess at another savegame's store.
    if (s_key[0] == '\0') return;

    snapshot_locked(L);
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
    s_save_watermark_ns = wall_now_ns();
    free(s_last_written);
    s_last_written = NULL;
}

void vars_persist_reset_scan_throttle(void) {
    s_last_scan_ms = 0;
}
