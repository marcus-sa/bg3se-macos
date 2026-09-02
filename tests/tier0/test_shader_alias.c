/*
 * Tier 0 — shader_alias: base-shader name candidates for a GetShader miss.
 *
 * The concrete paths below are real: taken from a BG3SE session log (the
 * Immolation Aura crash) and checked against the shipped pak file tables.
 */

#include "test_harness.h"
#include "shader_alias.h"

#define MOD_ROOT  "/Users/x/Baldur's Gate 3.app/Contents/Data/Public/" \
                  "DemonHunter_28d085ef-620b-a5bd-2602-d40a5b6435a7/"
#define BASE_ROOT "/Users/x/Baldur's Gate 3.app/Contents/Data/Public/Shared/"
#define DECAL     "Assets/Materials/Effects/Decal/" \
                  "VFX_Decal_Deferred_AlphaBlend_Unlit_Emissive_" \
                  "PolarUV_UVDistortion_01_ST_DEF_Metal.bshd"

TEST(strip_uuid_removes_material_clone_segment) {
    char out[PATH_MAX];
    ASSERT_TRUE(shader_alias_strip_uuid(
        "CHAR_Hair_28d085ef-620b-a5bd-2602-d40a5b6435a7_STI_DEF",
        out, sizeof(out)));
    ASSERT_STR_EQ(out, "CHAR_Hair_STI_DEF");
}

TEST(strip_uuid_leaves_mod_folder_alone) {
    /* Followed by `/`, so it names a directory: stripping it would produce
     * Public/DemonHunter/... which does not exist in any pak. */
    char out[PATH_MAX];
    ASSERT_FALSE(shader_alias_strip_uuid(MOD_ROOT DECAL, out, sizeof(out)));
}

TEST(strip_uuid_fails_closed_on_small_buffer) {
    char out[8];
    ASSERT_FALSE(shader_alias_strip_uuid(
        "CHAR_Hair_28d085ef-620b-a5bd-2602-d40a5b6435a7_STI_DEF",
        out, sizeof(out)));
}

TEST(base_root_rewrites_mod_namespace_to_shared) {
    char out[PATH_MAX];
    ASSERT_TRUE(shader_alias_base_root(MOD_ROOT DECAL, out, sizeof(out)));
    ASSERT_STR_EQ(out, BASE_ROOT DECAL);
}

TEST(base_root_handles_relative_virtual_path) {
    char out[PATH_MAX];
    ASSERT_TRUE(shader_alias_base_root("Public/DemonHunter_x/Assets/A.bshd",
                                       out, sizeof(out)));
    ASSERT_STR_EQ(out, "Public/Shared/Assets/A.bshd");
}

TEST(base_root_declines_when_already_shared) {
    char out[PATH_MAX];
    ASSERT_FALSE(shader_alias_base_root(BASE_ROOT DECAL, out, sizeof(out)));
}

TEST(base_root_declines_without_public_component) {
    char out[PATH_MAX];
    ASSERT_FALSE(shader_alias_base_root(
        "Shaders/Metal/VelocityBufferStaticInstanced.bshd",
        out, sizeof(out)));
}

TEST(base_root_ignores_public_as_a_name_fragment) {
    char out[PATH_MAX];
    ASSERT_FALSE(shader_alias_base_root("Mods/NotPublic/Assets/A.bshd",
                                        out, sizeof(out)));
}

TEST(candidates_for_mod_shipped_stock_material) {
    char c[SHADER_ALIAS_MAX_CANDIDATES][PATH_MAX];
    ASSERT_EQ(shader_alias_candidates(MOD_ROOT DECAL, c), 1);
    ASSERT_STR_EQ(c[0], BASE_ROOT DECAL);
}

TEST(candidates_for_clone_inside_mod_namespace) {
    /* Both shapes at once: UUID in the material name AND a mod namespace. */
    const char *name = MOD_ROOT "Assets/Materials/Effects/Decal/"
                       "M_01_b99d7a0e-5dd8-042f-37ca-5e5f092d3648_ST_DEF.bshd";
    char c[SHADER_ALIAS_MAX_CANDIDATES][PATH_MAX];
    int n = shader_alias_candidates(name, c);
    ASSERT_EQ(n, 3);
    /* uuid stripped, mod namespace kept */
    ASSERT_STR_EQ(c[0], MOD_ROOT "Assets/Materials/Effects/Decal/"
                                 "M_01_ST_DEF.bshd");
    /* namespace rewritten, uuid kept */
    ASSERT_STR_EQ(c[1], BASE_ROOT "Assets/Materials/Effects/Decal/"
                                  "M_01_b99d7a0e-5dd8-042f-37ca-5e5f092d3648"
                                  "_ST_DEF.bshd");
    /* both */
    ASSERT_STR_EQ(c[2], BASE_ROOT "Assets/Materials/Effects/Decal/"
                                  "M_01_ST_DEF.bshd");
}

TEST(candidates_empty_for_engine_shader_with_no_alias) {
    /* VelocityBufferStaticInstanced has no macOS counterpart at all — only
     * VelocityBufferStatic and VelocityBufferCamera ship. Guessing one would
     * bind a shader with a different vertex layout, so we produce none. */
    char c[SHADER_ALIAS_MAX_CANDIDATES][PATH_MAX];
    ASSERT_EQ(shader_alias_candidates(
        "Shaders/Metal/VelocityBufferStaticInstanced.bshd", c), 0);
}

TEST(candidates_never_returns_the_original_name) {
    char c[SHADER_ALIAS_MAX_CANDIDATES][PATH_MAX];
    int n = shader_alias_candidates(BASE_ROOT DECAL, c);
    for (int i = 0; i < n; i++) {
        ASSERT_TRUE(strcmp(c[i], BASE_ROOT DECAL) != 0);
    }
}

TEST(candidates_tolerates_null) {
    char c[SHADER_ALIAS_MAX_CANDIDATES][PATH_MAX];
    ASSERT_EQ(shader_alias_candidates(NULL, c), 0);
}

void register_shader_alias_tests(void);
void register_shader_alias_tests(void) {
    printf("shader_alias:\n");
    RUN_TEST(strip_uuid_removes_material_clone_segment);
    RUN_TEST(strip_uuid_leaves_mod_folder_alone);
    RUN_TEST(strip_uuid_fails_closed_on_small_buffer);
    RUN_TEST(base_root_rewrites_mod_namespace_to_shared);
    RUN_TEST(base_root_handles_relative_virtual_path);
    RUN_TEST(base_root_declines_when_already_shared);
    RUN_TEST(base_root_declines_without_public_component);
    RUN_TEST(base_root_ignores_public_as_a_name_fragment);
    RUN_TEST(candidates_for_mod_shipped_stock_material);
    RUN_TEST(candidates_for_clone_inside_mod_namespace);
    RUN_TEST(candidates_empty_for_engine_shader_with_no_alias);
    RUN_TEST(candidates_never_returns_the_original_name);
    RUN_TEST(candidates_tolerates_null);
}
