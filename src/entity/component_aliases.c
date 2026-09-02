/**
 * component_aliases.c - lookup over the generated short-name alias table.
 */

#include "component_aliases.h"
#include "generated_component_aliases.h"

#include <string.h>

const char *component_alias_lookup(const char *shortName) {
    if (!shortName || !*shortName) return NULL;

    /*
     * The generator emits rows sorted by shortName under the C locale, which is
     * plain byte order — the same order strcmp() compares in. Keep them sorted:
     * an out-of-order row makes this binary search silently miss it, and a
     * missed alias is indistinguishable from "Unknown component type". The
     * ordering is asserted in tests/tier0/test_component_aliases.c.
     */
    size_t lo = 0, hi = GENERATED_COMPONENT_ALIAS_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(shortName, g_ComponentAliases[mid].shortName);
        if (cmp == 0) return g_ComponentAliases[mid].engineName;
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

size_t component_alias_count(void) {
    return GENERATED_COMPONENT_ALIAS_COUNT;
}

const char *component_alias_short_at(size_t index) {
    if (index >= GENERATED_COMPONENT_ALIAS_COUNT) return NULL;
    return g_ComponentAliases[index].shortName;
}

const char *component_alias_engine_at(size_t index) {
    if (index >= GENERATED_COMPONENT_ALIAS_COUNT) return NULL;
    return g_ComponentAliases[index].engineName;
}
