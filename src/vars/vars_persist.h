/**
 * BG3SE-macOS - Ext.Vars savegame persistence
 *
 * Mod variables and entity user variables belong to a savegame. Windows BG3SE
 * writes them into the savegame itself (esv::OsirisVariableHelper::SavegameVisit
 * -> ScriptExtenderSave region), so they are snapshotted when the save is taken
 * and rewind with it. This port writes a sidecar store per savegame under
 * ~/Library/Application Support/BG3SE/SaveVars/ and reproduces that property by
 * writing the store ONLY when the game writes a save. See vars_persist.c for why
 * a sidecar, how a save is detected, and what is deliberately not persisted.
 */

#ifndef BG3SE_VARS_PERSIST_H
#define BG3SE_VARS_PERSIST_H

#include <lua.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Record the process start time. Must run before any savegame can be loaded:
 * the savegame resolver only trusts save files touched after this point.
 */
void vars_persist_init(void);

/**
 * The Osiris story was (re)loaded — one per session, new game or savegame load
 * alike. Detaches and empties the variable set so the outgoing campaign's
 * values can neither be read by the incoming one nor written into its store.
 */
void vars_persist_on_session_begin(lua_State *L);

/**
 * A savegame finished loading (Osiris SavegameLoaded). Identifies which save
 * was read and replaces the whole variable set with that save's snapshot.
 */
void vars_persist_on_savegame_loaded(lua_State *L);

/**
 * Gameplay started (Osiris LevelGameplayStarted). Marks the session live so a
 * save the player makes can be noticed. Never restores and never clears: it
 * also fires on level transitions inside a session, where wiping the live
 * variables would destroy the very state mods are mid-way through using.
 */
void vars_persist_on_level_started(lua_State *L);

/**
 * Called from the Osiris tick, on the game thread, under the Lua gate. Looks
 * for a savegame the game has just written to disk and, when it finds one,
 * snapshots the variable set into that save's store.
 *
 * Between saves this writes NOTHING. That is the contract: the store has to
 * hold the variables as of the save it is named after, or a reload restores
 * post-save state instead of rewinding to it.
 */
void vars_persist_tick(lua_State *L);

/**
 * The game wrote savegame `key` (NULL/"" keeps the attached one): adopt it and
 * snapshot the variable set into its store, on the caller's thread. Only ever
 * call this where a save has actually been written — an out-of-band call is
 * what puts post-save state into a save's store.
 */
void vars_persist_on_save_written(lua_State *L, const char *key);

/** Attached savegame name, or "" when nothing is attached. */
const char *vars_persist_current_key(void);

// ---------------------------------------------------------------------------
// Test seams — used by tier0 to drive the store without a game.
// ---------------------------------------------------------------------------

/** Override the store directory (created if missing). Pass NULL to reset. */
void vars_persist_set_store_dir(const char *dir);

/** Attach to `key` and restore its store, exactly as a savegame load would. */
void vars_persist_attach_key(lua_State *L, const char *key);

/** Forget the attached savegame without touching the variables. */
void vars_persist_detach(void);

/**
 * Drop the tick's scan throttle so the next vars_persist_tick() re-reads the
 * savegame directory immediately, instead of waiting out the scan interval.
 */
void vars_persist_reset_scan_throttle(void);

#ifdef __cplusplus
}
#endif

#endif /* BG3SE_VARS_PERSIST_H */
