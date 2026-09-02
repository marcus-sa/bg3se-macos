/**
 * BG3SE-macOS - Per-context mod environments
 *
 * Windows BG3SE runs a mod's client and server halves in two SEPARATE Lua
 * states, so BootstrapClient.lua and BootstrapServer.lua each get their own
 * copy of the mod's globals. This port has one state, and its Ext.Require
 * cache is keyed by file path, so a file pulled in by BOTH bootstraps executes
 * exactly once.
 *
 * That combination destroys state. SubclassCompatibilityFramework's
 * BootstrapClient runs first (at the main menu) and builds a fully populated
 * `Globals` plus `Queue.Commit` through its Ext.Require chain. Its
 * BootstrapServer then runs at story load and opens with a bare
 *     Api = {} ; Utils = {} ; Globals = {}
 * followed by Ext.Require("Globals/_init.lua") -- a cache hit, so nothing
 * repopulates. The client's own StatsLoaded handler then died on
 * `#Globals.ValidationErrors` (nil), every session.
 *
 * Re-running the require chain per context is NOT the fix: 4 of the 8
 * dual-bootstrap mods in a normal load order re-register side effects at file
 * scope if their shared files execute twice (MCM alone would call
 * Ext.Net.CreateChannel 17 more times and double-subscribe SessionLoaded;
 * Expansion would run its whole StatsLoaded progression pass twice).
 *
 * So the SECOND context to bootstrap a given mod runs in a shadow table
 * layered over the published Mods.<ModTable>:
 *   - reads fall through to the published table (and on to _G), so nothing the
 *     first context established becomes invisible;
 *   - a write to a key the first context already owns is captured in the
 *     shadow, so it cannot clobber the live value the first context's closures
 *     read;
 *   - a write to a key nobody owns yet lands in the published table, exactly
 *     as before, so second-context definitions stay visible to the first.
 * First writer of a key wins; the second context can add, but not destroy.
 */

#ifndef BG3SE_LUA_MODENV_H
#define BG3SE_LUA_MODENV_H

#include <lua.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Record which context owns the published Mods.<mod_table> table.
 *
 * @return 1 if `ctx` owns it (it claimed it now, or claimed it earlier),
 *         0 if a different context already owns it -- meaning `ctx` must
 *         bootstrap in a shadow env.
 */
int lua_modenv_claim_owner(lua_State *L, const char *mod_table, int ctx);

/**
 * Push the shadow environment for (ctx, mod_table), layered over the published
 * table at `published_index`. Shadows are memoized per (ctx, mod_table), so a
 * context that bootstraps the same mod twice keeps one environment.
 *
 * Always pushes exactly one value.
 */
void lua_modenv_push_shadow(lua_State *L, int published_index, int ctx,
                            const char *mod_table);

/**
 * Drop all ownership records and shadow environments for this state.
 * Only used by tests; the runtime never un-bootstraps a mod.
 */
void lua_modenv_reset(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* BG3SE_LUA_MODENV_H */
