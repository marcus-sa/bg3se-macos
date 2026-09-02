/**
 * component_name_resolve.c - candidate enumeration for component names.
 *
 * The enumeration order below is load-bearing and was lifted verbatim from
 * resolve_component_type() in entity_events.c; each step's comment records the
 * concrete mod failure that put it there.
 */

#include "component_name_resolve.h"
#include "component_aliases.h"
#include "component_registry.h"

#include <stdio.h>
#include <string.h>

/* Outer namespaces, tried in this order. eoc:: first matches the order mods
 * were written against on Windows. */
static const char *const kPrefixes[] = { "eoc::", "esv::", "ecl::", "ls::", NULL };
/* "" second so an explicit ...Component spelling is not shadowed by a
 * doubled suffix. */
static const char *const kSuffixes[] = { "Component", "", NULL };

/* Nested-namespace abbreviations. BG3SE contracts a component's inner
 * namespace into an initialism, so eoc::character_creation::StateComponent is
 * written "CCState" by mods. Probing only the outer prefixes can never reach
 * those, which is why AppearanceEditEnhanced ("CCState") and CustomCompanions
 * ("CCLevelUpDefinition") both failed to load with "Unknown component type"
 * while the components were registered all along.
 * Several rows may share an abbreviation: BG3SE collapses deeper nesting into
 * the same initialism, so CCRespec is
 * eoc::character_creation::definition::RespecComponent while CCState is
 * eoc::character_creation::StateComponent. Rows are tried in order. */
static const struct { const char *abbrev; const char *ns; } kNested[] = {
    { "CC",     "character_creation::" },
    { "CC",     "character_creation::definition::" },
    { "Hotbar", "hotbar::" },
    { NULL, NULL }
};

/* One accept step. Returns true when the candidate is taken. */
typedef bool (*AcceptFn)(const char *candidate,
                         const ComponentNameRegistryView *view,
                         uint16_t *out_index);

static bool accept_with_index(const char *candidate,
                              const ComponentNameRegistryView *view,
                              uint16_t *out_index) {
    return view->has_index(candidate, out_index, view->userdata);
}

static bool accept_registered(const char *candidate,
                              const ComponentNameRegistryView *view,
                              uint16_t *out_index) {
    (void)out_index;
    return view->is_registered(candidate, view->userdata);
}

/**
 * Run the full candidate enumeration once under `accept`.
 * Returns true and writes the winning engine name into out_engine.
 */
static bool enumerate(const char *name,
                      const ComponentNameRegistryView *view,
                      AcceptFn accept,
                      char *out_engine, size_t out_len,
                      uint16_t *out_index) {
    /* Exact match: the name is already an engine class name. */
    if (accept(name, view, out_index)) {
        snprintf(out_engine, out_len, "%s", name);
        return true;
    }

    /* Explicit short-name table before any probing. The probe below can only
     * reach components whose engine name is <outer namespace> + <short name>,
     * so it can never reach esv::combat::JoinEventOneFrameComponent from
     * "CombatantJoinEvent": neither the inner namespace nor the OneFrame infix
     * is recoverable from the short name.
     * The table is also the authority where the probe would answer but answer
     * wrongly: Windows binds "Level" to ls::LevelComponent (eoc::LevelComponent
     * is "EocLevel") while the probe reaches eoc:: first. */
    const char *aliased = component_alias_lookup(name);
    if (aliased) {
        if (accept(aliased, view, out_index)) {
            snprintf(out_engine, out_len, "%s", aliased);
            return true;
        }
        /* Fall through rather than fail: an alias whose target this build does
         * not register must not be worse than having no alias at all. */
    }

    /* Guard against names too long for probing. Longest expansion is
     * "eoc::" (5) + "character_creation::definition::" (32) + "Component" (9)
     * = 46.
     * snprintf would truncate rather than overflow, and a truncated name simply
     * fails to match, but keep the guard honest about the real bound. */
    if (strlen(name) > COMPONENT_MAX_NAME_LEN - 47) {
        return false;
    }

    char probe[COMPONENT_MAX_NAME_LEN];

    for (int p = 0; kPrefixes[p]; p++) {
        for (int s = 0; kSuffixes[s]; s++) {
            snprintf(probe, sizeof(probe), "%s%s%s", kPrefixes[p], name, kSuffixes[s]);
            if (accept(probe, view, out_index)) {
                snprintf(out_engine, out_len, "%s", probe);
                return true;
            }
        }
    }

    for (int n = 0; kNested[n].abbrev; n++) {
        size_t alen = strlen(kNested[n].abbrev);
        if (strncmp(name, kNested[n].abbrev, alen) != 0 || !name[alen]) continue;

        for (int p = 0; kPrefixes[p]; p++) {
            for (int s = 0; kSuffixes[s]; s++) {
                snprintf(probe, sizeof(probe), "%s%s%s%s",
                         kPrefixes[p], kNested[n].ns, name + alen, kSuffixes[s]);
                if (accept(probe, view, out_index)) {
                    snprintf(out_engine, out_len, "%s", probe);
                    return true;
                }
            }
        }
    }

    return false;
}

ComponentNameResolution component_name_resolve(
    const char *name,
    const ComponentNameRegistryView *view,
    char *out_engine, size_t out_len,
    uint16_t *out_index) {

    if (!name || !*name || !view || !view->has_index || !view->is_registered ||
        !out_engine || out_len < COMPONENT_MAX_NAME_LEN || !out_index) {
        return COMPONENT_NAME_UNRESOLVED;
    }

    /* Pass 1 accepts only live components, so a name that already bound to a
     * particular component keeps binding to it. Demoting that to "first
     * registered candidate wins" would silently re-point existing
     * subscriptions at a sibling in another namespace. */
    if (enumerate(name, view, accept_with_index, out_engine, out_len, out_index)) {
        return COMPONENT_NAME_RESOLVED;
    }

    if (enumerate(name, view, accept_registered, out_engine, out_len, out_index)) {
        return COMPONENT_NAME_PENDING;
    }

    return COMPONENT_NAME_UNRESOLVED;
}
