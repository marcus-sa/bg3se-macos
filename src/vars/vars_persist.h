/**
 * BG3SE-macOS - Ext.Vars savegame persistence
 *
 * Mod variables and entity user variables belong to a savegame. Windows BG3SE
 * writes them into the savegame itself (esv::OsirisVariableHelper::SavegameVisit
 * -> ScriptExtenderSave region); this port writes a sidecar store per savegame
 * under ~/Library/Application Support/BG3SE/SaveVars/ instead. See vars_persist.c
 * for why, and for how the savegame is identified.
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
 * was read, replaces the whole variable set with that save's store, and starts
 * persisting under it.
 */
void vars_persist_on_savegame_loaded(lua_State *L);

/**
 * Gameplay started (Osiris LevelGameplayStarted). Marks the session live so
 * the tick may adopt the first save the player makes. Never restores and never
 * clears: it also fires on level transitions inside a session, where wiping the
 * live variables would destroy the very state mods are mid-way through using.
 */
void vars_persist_on_level_started(lua_State *L);

/**
 * Called from the Osiris tick. Adopts a savegame the player just created and
 * writes the store when its contents changed. Rate-limited internally.
 */
void vars_persist_tick(lua_State *L);

/**
 * Write the store immediately, ignoring the rate limit. No-op when no savegame
 * is attached.
 */
void vars_persist_flush_now(lua_State *L);

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

#ifdef __cplusplus
}
#endif

#endif /* BG3SE_VARS_PERSIST_H */
