/*
 * Tier 0: static data registry tables (src/staticdata/generated_*.c).
 *
 * Both tables are generated from the 4.1.1.7398727 arm64 binary and are the
 * only thing standing between Ext.StaticData.Get and a wrong answer: the
 * m_TypeIndex global a type is looked up by ships as 0 with an uninitialised
 * guard, and 0 is a valid index belonging to some other type, so the vtable
 * offset is what proves a resolved bank is the right one. A type that loses its
 * vtable row silently falls back to trusting the index again.
 */

#include "test_harness.h"
#include "staticdata_registry.h"

#include <string.h>

TEST(staticdata_every_type_has_a_vtable_offset) {
    int types = 0;
    for (int i = 0; g_staticdata_types[i].name; i++) {
        types++;
        const StaticDataTypeEntry *e = &g_staticdata_types[i];
        int found = 0;
        for (int j = 0; g_staticdata_vtables[j].engine_class; j++) {
            if (strcmp(g_staticdata_vtables[j].engine_class, e->engine_class) == 0) {
                ASSERT_NE(g_staticdata_vtables[j].vtable_offset, 0ULL);
                found = 1;
                break;
            }
        }
        ASSERT_TRUE(found);
    }
    ASSERT_TRUE(types > 100);
}

TEST(staticdata_vtable_offsets_are_distinct) {
    /* Two types sharing a vtable would make bank_matches_type accept the wrong
     * bank for one of them -- exactly the failure the check exists to stop. */
    for (int i = 0; g_staticdata_vtables[i].engine_class; i++) {
        for (int j = i + 1; g_staticdata_vtables[j].engine_class; j++) {
            ASSERT_NE(g_staticdata_vtables[i].vtable_offset,
                      g_staticdata_vtables[j].vtable_offset);
        }
    }
}

TEST(staticdata_index_and_vtable_offsets_never_collide) {
    /* m_TypeIndex lives in __DATA,__data and the vtables in
     * __DATA_CONST,__const. An overlap would mean one of the two generators
     * matched the wrong symbol. */
    for (int i = 0; g_staticdata_types[i].name; i++) {
        for (int j = 0; g_staticdata_vtables[j].engine_class; j++) {
            ASSERT_NE(g_staticdata_types[i].index_offset,
                      g_staticdata_vtables[j].vtable_offset);
        }
    }
}

TEST(staticdata_type_names_are_unique_and_nonempty) {
    for (int i = 0; g_staticdata_types[i].name; i++) {
        ASSERT_TRUE(g_staticdata_types[i].name[0] != '\0');
        ASSERT_TRUE(g_staticdata_types[i].engine_class[0] != '\0');
        ASSERT_NE(g_staticdata_types[i].index_offset, 0ULL);
        for (int j = i + 1; g_staticdata_types[j].name; j++) {
            ASSERT_TRUE(strcmp(g_staticdata_types[i].name,
                               g_staticdata_types[j].name) != 0);
        }
    }
}

TEST(staticdata_types_expansion_needs_are_registered) {
    /* The four defTypes the Expansion mod asks for on every StatsLoaded. */
    static const char *needed[] = {
        "Progression", "ClassDescription", "PassiveList", "EquipmentList"
    };
    for (size_t k = 0; k < sizeof(needed) / sizeof(needed[0]); k++) {
        int found = 0;
        for (int i = 0; g_staticdata_types[i].name; i++) {
            if (strcmp(g_staticdata_types[i].name, needed[k]) == 0) { found = 1; break; }
        }
        ASSERT_TRUE(found);
    }
}

void register_staticdata_registry_tests(void) {
    printf("staticdata registry:\n");
    RUN_TEST(staticdata_every_type_has_a_vtable_offset);
    RUN_TEST(staticdata_vtable_offsets_are_distinct);
    RUN_TEST(staticdata_index_and_vtable_offsets_never_collide);
    RUN_TEST(staticdata_type_names_are_unique_and_nonempty);
    RUN_TEST(staticdata_types_expansion_needs_are_registered);
}
