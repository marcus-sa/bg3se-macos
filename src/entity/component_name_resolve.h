/**
 * component_name_resolve.h - BG3SE component name -> engine class name
 *
 * Splits the name half of component lookup away from the index half.
 *
 * Mods address components by the short name Windows BG3SE exposes
 * ("CombatantJoinEvent"), and resolving that to the engine class name
 * ("esv::combat::JoinEventOneFrameComponent") does not require the ECS to have
 * assigned the class a ComponentTypeIndex yet. Those two questions used to be
 * fused: a subscription could only be accepted if the index was already
 * discovered, so a perfectly valid name raised "Unknown component type" purely
 * because the mod's bootstrap ran before the ECS registered the type. See
 * entity_events.c for the deferral that fusion prevented.
 *
 * The registry is reached through a view rather than linked directly so the
 * resolution order can be tested against a synthetic registry — including the
 * not-yet-discovered state, which is exactly the state that is impossible to
 * stage against the live game.
 */

#ifndef BG3SE_COMPONENT_NAME_RESOLVE_H
#define BG3SE_COMPONENT_NAME_RESOLVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read-only window onto whatever holds component metadata.
 *
 * The two probes are deliberately separate: `has_index` answers "can a
 * subscription bind right now", `is_registered` answers "is this a real
 * component on this build". A name that answers no to the first and yes to the
 * second is the deferral case.
 */
typedef struct {
    /** true when engine_name has a discovered ComponentTypeIndex; writes it. */
    bool (*has_index)(const char *engine_name, uint16_t *out_index, void *userdata);
    /** true when engine_name names a component this build knows about at all. */
    bool (*is_registered)(const char *engine_name, void *userdata);
    void *userdata;
} ComponentNameRegistryView;

typedef enum {
    /* No alias row, no registry row, no probe hit — a typo or a component this
     * build does not have. Callers must treat this as an error: silently
     * accepting it would turn a misspelling into a subscription that can never
     * fire. */
    COMPONENT_NAME_UNRESOLVED = 0,
    /* Engine class known and its index is discovered — bind immediately. */
    COMPONENT_NAME_RESOLVED,
    /* Engine class known, index still COMPONENT_INDEX_UNDEFINED — defer. */
    COMPONENT_NAME_PENDING
} ComponentNameResolution;

/**
 * Resolve a component name written by a mod to an engine class name.
 *
 * Candidates are enumerated in a fixed order (exact name, alias table, outer
 * namespace probes, inner-namespace initialism probes) and the enumeration is
 * run twice: once accepting only candidates with a discovered index, then once
 * accepting any registered candidate. The two passes exist so that adding
 * deferral cannot change which component an already-working name binds to: a
 * candidate that is merely registered never outranks one that is live.
 *
 * @param name        Name as written by the mod (short or full engine name).
 * @param view        Registry window; both probes must be non-NULL.
 * @param out_engine  Receives the engine class name on a RESOLVED/PENDING result.
 * @param out_len     Size of out_engine; must be >= COMPONENT_MAX_NAME_LEN.
 * @param out_index   Receives the index on RESOLVED; untouched otherwise.
 */
ComponentNameResolution component_name_resolve(
    const char *name,
    const ComponentNameRegistryView *view,
    char *out_engine, size_t out_len,
    uint16_t *out_index);

#ifdef __cplusplus
}
#endif

#endif /* BG3SE_COMPONENT_NAME_RESOLVE_H */
