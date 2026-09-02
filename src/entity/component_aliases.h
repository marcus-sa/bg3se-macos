/**
 * component_aliases.h - BG3SE short component name -> engine class name
 *
 * Mods address components by the short name Windows BG3SE exposes
 * ("CombatantJoinEvent"), not by the engine class name the ECS registers
 * ("esv::combat::JoinEventOneFrameComponent"). The table lives in
 * generated_component_aliases.h; this is the lookup over it.
 */

#ifndef BG3SE_COMPONENT_ALIASES_H
#define BG3SE_COMPONENT_ALIASES_H

#include <stddef.h>

/**
 * Translate a BG3SE short component name to the engine class name.
 * Returns NULL when the name is not an alias — callers must then fall back to
 * whatever resolution they used before, because a name may legitimately be a
 * full engine name already, or reachable by prefix probing.
 *
 * The returned pointer is a static string literal and outlives any caller.
 */
const char *component_alias_lookup(const char *shortName);

/** Number of rows in the alias table (for tests and diagnostics). */
size_t component_alias_count(void);

/** Row accessor for tests and diagnostics; NULL when index is out of range. */
const char *component_alias_short_at(size_t index);
const char *component_alias_engine_at(size_t index);

#endif /* BG3SE_COMPONENT_ALIASES_H */
