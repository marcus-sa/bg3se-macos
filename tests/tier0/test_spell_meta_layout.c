/*
 * Tier 0: eoc::spell::SpellMeta layout and the enum labels its fields decode to.
 *
 * SpellContainer.Spells is a DynamicArray<SpellMeta>, so the element size is a
 * stride: get it wrong and element N lands inside element N-1, which does not
 * fault — it hands mods plausible-looking garbage for every entry after the
 * first. component_offsets.h declared 80 until the 0x60 stride was read off
 * DynamicArray<eoc::spell::SpellMeta>::Reallocate @0x1019d9858 on 4.1.1.7398727
 * (`mov w10, #0x60` feeding `smaddl x8, w8, w10, x9`).
 *
 * The SPELL_META_OFF_* constants below are what the decoder actually reads
 * through; SpellMetaLayout is a mirror the compiler lays out independently.
 * Pinning one against the other means a future edit that moves a field without
 * moving its constant fails here instead of silently shifting a decode.
 * Provenance for each offset is cited in src/entity/spell_meta_layout.h.
 */

#include "test_harness.h"
#include <stddef.h>
#include "spell_meta_layout.h"
#include "enum_registry.h"

TEST(spell_meta_stride_is_0x60) {
    ASSERT_EQ(sizeof(SpellMetaLayout), (size_t)SPELL_META_SIZE);
    ASSERT_EQ(SPELL_META_SIZE, 96);
}

TEST(spell_meta_id_offsets_match_mirror) {
    ASSERT_EQ(offsetof(SpellMetaLayout, OriginatorPrototype),
              (size_t)SPELL_META_OFF_ORIGINATOR_PROTOTYPE);
    ASSERT_EQ(offsetof(SpellMetaLayout, SourceType),
              (size_t)SPELL_META_OFF_SOURCE_TYPE);
    ASSERT_EQ(offsetof(SpellMetaLayout, Source),
              (size_t)SPELL_META_OFF_SOURCE);
    ASSERT_EQ(offsetof(SpellMetaLayout, ProgressionSource),
              (size_t)SPELL_META_OFF_PROGRESSION_SOURCE);
}

TEST(spell_meta_offsets_match_mirror) {
    ASSERT_EQ(offsetof(SpellMetaLayout, BoostHandle),
              (size_t)SPELL_META_OFF_BOOST_HANDLE);
    ASSERT_EQ(offsetof(SpellMetaLayout, LearningStrategy),
              (size_t)SPELL_META_OFF_LEARNING_STRATEGY);
    ASSERT_EQ(offsetof(SpellMetaLayout, PrepareType),
              (size_t)SPELL_META_OFF_PREPARE_TYPE);
    ASSERT_EQ(offsetof(SpellMetaLayout, PreferredCastingResource),
              (size_t)SPELL_META_OFF_CASTING_RESOURCE);
    ASSERT_EQ(offsetof(SpellMetaLayout, SpellCastingAbility),
              (size_t)SPELL_META_OFF_CASTING_ABILITY);
    ASSERT_EQ(offsetof(SpellMetaLayout, CooldownType),
              (size_t)SPELL_META_OFF_COOLDOWN_TYPE);
    ASSERT_EQ(offsetof(SpellMetaLayout, ContainerSpell),
              (size_t)SPELL_META_OFF_CONTAINER_SPELL);
    ASSERT_EQ(offsetof(SpellMetaLayout, LinkedSpellContainer),
              (size_t)SPELL_META_OFF_LINKED_CONTAINER);
}

/*
 * Field widths, not just their starts. The serializer at 0x1019d7a4c writes
 * each of these with a literal byte count: 1 for the four enums and the bool,
 * 16 for the three Guids, 8 for the EntityHandle, 4 for the two FixedStrings.
 * Widening one here would move everything behind it without moving any
 * SPELL_META_OFF_* constant, so the offset asserts alone would not catch it.
 */
TEST(spell_meta_field_widths_match_serializer) {
    SpellMetaLayout m;
    ASSERT_EQ(sizeof m.OriginatorPrototype,      (size_t)4);
    ASSERT_EQ(sizeof m.SourceType,               (size_t)1);
    ASSERT_EQ(sizeof m.Source,                   (size_t)16);
    ASSERT_EQ(sizeof m.ProgressionSource,        (size_t)16);
    ASSERT_EQ(sizeof m.BoostHandle,              (size_t)8);
    ASSERT_EQ(sizeof m.LearningStrategy,         (size_t)1);
    ASSERT_EQ(sizeof m.PrepareType,              (size_t)1);
    ASSERT_EQ(sizeof m.PreferredCastingResource, (size_t)16);
    ASSERT_EQ(sizeof m.SpellCastingAbility,      (size_t)1);
    ASSERT_EQ(sizeof m.CooldownType,             (size_t)1);
    ASSERT_EQ(sizeof m.ContainerSpell,           (size_t)4);
    ASSERT_EQ(sizeof m.LinkedSpellContainer,     (size_t)1);
}

/*
 * The decoder turns SpellCastingAbility into a label through the enum registry,
 * so the registry mapping is part of the decode, not decoration: a mod's
 * `spell.SpellCastingAbility == "Intelligence"` is a string compare against
 * whatever AbilityId[4] resolves to. Verified against 7398727's own
 * _Enum_Ability::NAMES / ::VALUES tables (0x10878f9a0 / 0x107887564).
 */
TEST(ability_id_labels_match_game_tables) {
    EnumTypeInfo *info = enum_registry_find_by_name("AbilityId");
    ASSERT_NOT_NULL(info);

    ASSERT_STR_EQ(enum_find_label(info->registry_index, 0), "None");
    ASSERT_STR_EQ(enum_find_label(info->registry_index, 1), "Strength");
    ASSERT_STR_EQ(enum_find_label(info->registry_index, 2), "Dexterity");
    ASSERT_STR_EQ(enum_find_label(info->registry_index, 3), "Constitution");
    ASSERT_STR_EQ(enum_find_label(info->registry_index, 4), "Intelligence");
    ASSERT_STR_EQ(enum_find_label(info->registry_index, 5), "Wisdom");
    ASSERT_STR_EQ(enum_find_label(info->registry_index, 6), "Charisma");
    // 7 is off the end of the game's table; an out-of-range byte must not
    // silently borrow a neighbouring label.
    ASSERT_NULL(enum_find_label(info->registry_index, 7));
}

/*
 * Values read off eoc::StringToCooldownType @0x101f7d05c. Value 6 is the one
 * worth pinning: the game accepts UntilRestPerItem and OncePerRestPerItem for
 * it, while Windows BG3SE calls it UntilPerRestPerItem — a name that appears
 * nowhere in the 7398727 binary. All three must resolve by name so mods written
 * against either spelling keep working.
 */
TEST(spell_cooldown_type_labels_match_game_parser) {
    EnumTypeInfo *info = enum_registry_find_by_name("SpellCooldownType");
    ASSERT_NOT_NULL(info);
    int idx = info->registry_index;

    ASSERT_STR_EQ(enum_find_label(idx, 0), "Default");
    ASSERT_STR_EQ(enum_find_label(idx, 1), "OncePerTurn");
    ASSERT_STR_EQ(enum_find_label(idx, 2), "OncePerCombat");
    ASSERT_STR_EQ(enum_find_label(idx, 3), "UntilRest");
    ASSERT_STR_EQ(enum_find_label(idx, 4), "OncePerTurnNoRealtime");
    ASSERT_STR_EQ(enum_find_label(idx, 5), "UntilShortRest");
    ASSERT_STR_EQ(enum_find_label(idx, 6), "UntilRestPerItem");
    ASSERT_STR_EQ(enum_find_label(idx, 7), "OncePerShortRestPerItem");

    ASSERT_EQ(enum_find_value(idx, "OncePerRestPerItem"), (int64_t)6);
    ASSERT_EQ(enum_find_value(idx, "UntilPerRestPerItem"), (int64_t)6);
    ASSERT_EQ(enum_find_value(idx, "UntilShortRestPerItem"), (int64_t)7);
}

/* eoc::spell::StringToPreparationStrategy @0x101f7da90; unmatched input -> 2. */
TEST(spell_prepare_type_labels_match_game_parser) {
    EnumTypeInfo *info = enum_registry_find_by_name("SpellPrepareType");
    ASSERT_NOT_NULL(info);
    int idx = info->registry_index;

    ASSERT_STR_EQ(enum_find_label(idx, 0), "AlwaysPrepared");
    ASSERT_STR_EQ(enum_find_label(idx, 1), "RequiresPreparation");
    ASSERT_STR_EQ(enum_find_label(idx, 2), "Unknown");
}

/*
 * ESourceType and ELearningStrategy have verified offsets but no label set that
 * can be justified against 7398727, so the decoder deliberately pushes their
 * raw ordinal. If someone later registers those enum types, this test fails and
 * forces the decoder to be updated to pass the type name through — rather than
 * the registry quietly gaining names the decoder never uses.
 */
TEST(unverified_spell_enums_stay_unregistered) {
    ASSERT_NULL(enum_registry_find_by_name("SpellSourceType"));
    ASSERT_NULL(enum_registry_find_by_name("SpellLearningStrategy"));
}

void register_spell_meta_layout_tests(void) {
    printf("SpellMeta layout:\n");
    RUN_TEST(spell_meta_stride_is_0x60);
    RUN_TEST(spell_meta_id_offsets_match_mirror);
    RUN_TEST(spell_meta_offsets_match_mirror);
    RUN_TEST(spell_meta_field_widths_match_serializer);

    enum_registry_init();
    enum_register_definitions();
    RUN_TEST(ability_id_labels_match_game_tables);
    RUN_TEST(spell_cooldown_type_labels_match_game_parser);
    RUN_TEST(spell_prepare_type_labels_match_game_parser);
    RUN_TEST(unverified_spell_enums_stay_unregistered);
}
