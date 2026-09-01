/*
 * Tier 0 tests for Osiris database column-type validation.
 *
 * Tests osi_type_decode_class / osi_type_matches_declared from
 * src/osiris/osiris_types.h — the rule Osi.DB_*:Get() uses to decide whether a
 * stored fact really matches the database's declared signature. Pure logic,
 * zero deps.
 */

#include "test_harness.h"
#include "osiris_types.h"

/* ── Decode classes ──────────────────────────────────────────────── */

TEST(decode_class_scalars_are_distinct) {
    ASSERT_EQ(osi_type_decode_class(OSI_TYPE_INTEGER), 1);
    ASSERT_EQ(osi_type_decode_class(OSI_TYPE_INTEGER64), 2);
    ASSERT_EQ(osi_type_decode_class(OSI_TYPE_REAL), 3);
}

TEST(decode_class_none_is_zero) {
    ASSERT_EQ(osi_type_decode_class(OSI_TYPE_NONE), 0);
}

TEST(decode_class_string_family_collapses) {
    /* STRING, GUIDSTRING and every aliased GUID subtype all store a handle. */
    ASSERT_EQ(osi_type_decode_class(OSI_TYPE_STRING), 4);
    ASSERT_EQ(osi_type_decode_class(OSI_TYPE_GUIDSTRING), 4);
    ASSERT_EQ(osi_type_decode_class(21), 4);      /* e.g. CHARACTERGUID */
    ASSERT_EQ(osi_type_decode_class(OSI_TYPE_MAX_PLAUSIBLE), 4);
}

TEST(decode_class_rejects_out_of_range) {
    ASSERT_EQ(osi_type_decode_class(OSI_TYPE_MAX_PLAUSIBLE + 1), 0);
    ASSERT_EQ(osi_type_decode_class(0xFFFF), 0);
}

/* ── Declared-type matching ──────────────────────────────────────── */

TEST(matches_declared_exact) {
    ASSERT_TRUE(osi_type_matches_declared(OSI_TYPE_INTEGER, OSI_TYPE_INTEGER));
    ASSERT_TRUE(osi_type_matches_declared(21, 21));
}

TEST(matches_declared_guid_aliases) {
    /* Osiris aliases GUID subtypes freely; both read as a string handle. */
    ASSERT_TRUE(osi_type_matches_declared(OSI_TYPE_GUIDSTRING, 21));
    ASSERT_TRUE(osi_type_matches_declared(21, OSI_TYPE_GUIDSTRING));
    ASSERT_TRUE(osi_type_matches_declared(22, 21));
}

TEST(matches_declared_rejects_the_db_avatars_corruption) {
    /* The bug: a CHARACTERGUID column whose value claims to be an INTEGER
     * because the "type word" was really the tuple's Size field. */
    ASSERT_FALSE(osi_type_matches_declared(OSI_TYPE_INTEGER, 21));
    ASSERT_FALSE(osi_type_matches_declared(OSI_TYPE_INTEGER64, 21));
    ASSERT_FALSE(osi_type_matches_declared(OSI_TYPE_INTEGER, OSI_TYPE_GUIDSTRING));
}

TEST(matches_declared_rejects_integer_width_mismatch) {
    ASSERT_FALSE(osi_type_matches_declared(OSI_TYPE_INTEGER, OSI_TYPE_INTEGER64));
    ASSERT_FALSE(osi_type_matches_declared(OSI_TYPE_INTEGER64, OSI_TYPE_INTEGER));
    ASSERT_FALSE(osi_type_matches_declared(OSI_TYPE_REAL, OSI_TYPE_INTEGER));
}

TEST(matches_declared_rejects_none_and_garbage) {
    ASSERT_FALSE(osi_type_matches_declared(OSI_TYPE_NONE, OSI_TYPE_NONE));
    ASSERT_FALSE(osi_type_matches_declared(OSI_TYPE_NONE, OSI_TYPE_INTEGER));
    ASSERT_FALSE(osi_type_matches_declared(OSI_TYPE_INTEGER, OSI_TYPE_NONE));
    ASSERT_FALSE(osi_type_matches_declared(0xC7D3, 21));
    ASSERT_FALSE(osi_type_matches_declared(21, 0xC7D3));
}

/* ── Registration ────────────────────────────────────────────────── */

void register_osiris_db_type_tests(void) {
    printf("[osiris_db_types]\n");
    RUN_TEST(decode_class_scalars_are_distinct);
    RUN_TEST(decode_class_none_is_zero);
    RUN_TEST(decode_class_string_family_collapses);
    RUN_TEST(decode_class_rejects_out_of_range);
    RUN_TEST(matches_declared_exact);
    RUN_TEST(matches_declared_guid_aliases);
    RUN_TEST(matches_declared_rejects_the_db_avatars_corruption);
    RUN_TEST(matches_declared_rejects_integer_width_mismatch);
    RUN_TEST(matches_declared_rejects_none_and_garbage);
}
