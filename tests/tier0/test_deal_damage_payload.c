/*
 * Tier 0: member layouts of the objects the DealDamage / DealtDamage /
 * BeforeDealDamage payload decodes, and the enum label tables it decodes
 * through.
 *
 * test_deal_damage_abi.c already pins the hook's ARGUMENT layout — where each
 * object POINTER comes from. This file pins the separate question of what
 * lives INSIDE those objects, which is what src/lua/lua_events.c reads.
 *
 * Two failure modes are covered, and the second is the one that hides:
 *
 *  1. An offset constant changes. The asserts against the literals below catch
 *     that directly. Every literal is cited in src/stats/deal_damage_layout.h
 *     and derived in ghidra/offsets/DEALDAMAGE_PAYLOAD_LAYOUTS.md.
 *
 *  2. A field WIDTH changes in the mirror struct. Widening HitDesc.EffectFlags
 *     from uint32 to uint64, say, shifts every member behind it without moving
 *     a single *_OFF_* constant, so an offsets-only test still passes while the
 *     mirror silently stops describing the game object. Pinning offsetof()
 *     against the constants, and sizeof() against the measured struct size,
 *     makes that fail here.
 */

#include "test_harness.h"
#include <stddef.h>
#include "deal_damage_layout.h"
#include "enum_registry.h"

void enum_register_definitions(void);

/* ---------------------------------------------------------------- functor */

/*
 * StatsFunctorDealDamage::Parse @0x101fb0d14 stores StringToDamageType()'s
 * result with `strb w0, [x19, #0x47]`. One byte: widening it here would push
 * the three sibling enums Clone() copies alongside it out of place.
 */
TEST(deal_damage_functor_damage_type_offset) {
    ASSERT_EQ(DEAL_DAMAGE_FUNCTOR_OFF_DAMAGE_TYPE, 0x47);
    ASSERT_EQ(DEAL_DAMAGE_FUNCTOR_SIZE, 0x68);
}

/* ---------------------------------------------------------------- SpellId */

TEST(spell_id_offsets_match_mirror) {
    ASSERT_EQ(offsetof(SpellIdLayout, OriginatorPrototype),
              (size_t)SPELL_ID_OFF_ORIGINATOR_PROTOTYPE);
    ASSERT_EQ(offsetof(SpellIdLayout, SourceType),
              (size_t)SPELL_ID_OFF_SOURCE_TYPE);
    ASSERT_EQ(offsetof(SpellIdLayout, Source),
              (size_t)SPELL_ID_OFF_SOURCE);
    ASSERT_EQ(offsetof(SpellIdLayout, ProgressionSource),
              (size_t)SPELL_ID_OFF_PROGRESSION_SOURCE);
    ASSERT_EQ(offsetof(SpellIdLayout, Prototype),
              (size_t)SPELL_ID_OFF_PROTOTYPE);
    ASSERT_EQ(sizeof(SpellIdLayout), (size_t)SPELL_ID_SIZE);
}

TEST(spell_id_field_widths) {
    SpellIdLayout s;
    ASSERT_EQ(sizeof s.OriginatorPrototype, (size_t)4);   /* FixedString */
    ASSERT_EQ(sizeof s.SourceType,          (size_t)1);
    ASSERT_EQ(sizeof s.Source,              (size_t)16);  /* Guid */
    ASSERT_EQ(sizeof s.ProgressionSource,   (size_t)16);  /* Guid */
    ASSERT_EQ(sizeof s.Prototype,           (size_t)4);   /* FixedString */
}

/*
 * SpellInfo (Windows SpellIdWithPrototype) must keep SpellId at offset 0:
 * Execute @0x1057736b8 hands its SpellInfo& straight to a
 * `eoc::spell::MetaId const&` parameter with no adjustment, and the decoder
 * reads Prototype through the SpellId offsets on the SpellInfo pointer.
 */
TEST(spell_info_embeds_spell_id_at_zero) {
    ASSERT_EQ(offsetof(SpellInfoLayout, Id), (size_t)0);
    ASSERT_EQ(offsetof(SpellInfoLayout, SpellProto),
              (size_t)SPELL_INFO_OFF_SPELL_PROTO);
    ASSERT_EQ(SPELL_INFO_OFF_SPELL_PROTO, SPELL_ID_SIZE);
    ASSERT_EQ(sizeof(SpellInfoLayout), (size_t)SPELL_INFO_SIZE);
}

/* ------------------------------------------------------- SpellPrototype */

TEST(spell_prototype_head_offsets) {
    ASSERT_EQ(SPELL_PROTOTYPE_OFF_STATS_OBJECT_INDEX, 0x00);
    ASSERT_EQ(SPELL_PROTOTYPE_OFF_SPELL_TYPE_ID,      0x04);
    ASSERT_EQ(SPELL_PROTOTYPE_OFF_SPELL_ID,           0x08);
    ASSERT_EQ(SPELL_PROTOTYPE_OFF_SPELL_FLAGS,        0x10);
}

/* ---------------------------------------------------------------- HitDesc */

TEST(hit_desc_offsets_match_mirror) {
    ASSERT_EQ(offsetof(HitDescLayout, TotalDamageDone), (size_t)HIT_DESC_OFF_TOTAL_DAMAGE_DONE);
    ASSERT_EQ(offsetof(HitDescLayout, DeathType),       (size_t)HIT_DESC_OFF_DEATH_TYPE);
    ASSERT_EQ(offsetof(HitDescLayout, MainDamageType),  (size_t)HIT_DESC_OFF_MAIN_DAMAGE_TYPE);
    ASSERT_EQ(offsetof(HitDescLayout, CauseType),       (size_t)HIT_DESC_OFF_CAUSE_TYPE);
    ASSERT_EQ(offsetof(HitDescLayout, ImpactPosition),  (size_t)HIT_DESC_OFF_IMPACT_POSITION);
    ASSERT_EQ(offsetof(HitDescLayout, ImpactDirection), (size_t)HIT_DESC_OFF_IMPACT_DIRECTION);
    ASSERT_EQ(offsetof(HitDescLayout, ImpactForce),     (size_t)HIT_DESC_OFF_IMPACT_FORCE);
    ASSERT_EQ(offsetof(HitDescLayout, ArmorAbsorption), (size_t)HIT_DESC_OFF_ARMOR_ABSORPTION);
    ASSERT_EQ(offsetof(HitDescLayout, LifeSteal),       (size_t)HIT_DESC_OFF_LIFE_STEAL);
    ASSERT_EQ(offsetof(HitDescLayout, EffectFlags),     (size_t)HIT_DESC_OFF_EFFECT_FLAGS);
    ASSERT_EQ(offsetof(HitDescLayout, Inflicter),       (size_t)HIT_DESC_OFF_INFLICTER);
    ASSERT_EQ(offsetof(HitDescLayout, InflicterOwner),  (size_t)HIT_DESC_OFF_INFLICTER_OWNER);
    ASSERT_EQ(offsetof(HitDescLayout, ThrownObject),    (size_t)HIT_DESC_OFF_THROWN_OBJECT);
    ASSERT_EQ(offsetof(HitDescLayout, HitWith),         (size_t)HIT_DESC_OFF_HIT_WITH);
    ASSERT_EQ(offsetof(HitDescLayout, AttackAbility),   (size_t)HIT_DESC_OFF_ATTACK_ABILITY);
    ASSERT_EQ(offsetof(HitDescLayout, SaveAbility),     (size_t)HIT_DESC_OFF_SAVE_ABILITY);
    ASSERT_EQ(offsetof(HitDescLayout, SpellAttackType), (size_t)HIT_DESC_OFF_SPELL_ATTACK_TYPE);
    ASSERT_EQ(offsetof(HitDescLayout, SpellId),         (size_t)HIT_DESC_OFF_SPELL_ID);
    ASSERT_EQ(offsetof(HitDescLayout, SpellSchool),     (size_t)HIT_DESC_OFF_SPELL_SCHOOL);
    ASSERT_EQ(offsetof(HitDescLayout, Flags),           (size_t)HIT_DESC_OFF_FLAGS);
    ASSERT_EQ(offsetof(HitDescLayout, SpellLevel),      (size_t)HIT_DESC_OFF_SPELL_LEVEL);
    ASSERT_EQ(offsetof(HitDescLayout, SpellPowerLevel), (size_t)HIT_DESC_OFF_SPELL_POWER_LEVEL);
    ASSERT_EQ(offsetof(HitDescLayout, TotalHealDone),   (size_t)HIT_DESC_OFF_TOTAL_HEAL_DONE);
    ASSERT_EQ(offsetof(HitDescLayout, DamageListElements),
              (size_t)HIT_DESC_OFF_DAMAGE_LIST_ELEMENTS);
    ASSERT_EQ(offsetof(HitDescLayout, DamageListCapacity),
              (size_t)HIT_DESC_OFF_DAMAGE_LIST_CAPACITY);
    ASSERT_EQ(offsetof(HitDescLayout, DamageListSize),
              (size_t)HIT_DESC_OFF_DAMAGE_LIST_SIZE);
    ASSERT_EQ(sizeof(HitDescLayout), (size_t)HIT_DESC_SIZE);
}

/*
 * Widths as the deserializer @0x1019c24d0 reads them: literal byte counts of
 * 4 for the int32s and the FixedString, 1 for each enum byte, 12 for the two
 * Vector3fs, 8 for each of the three EntityHandles.
 */
TEST(hit_desc_field_widths_match_deserializer) {
    HitDescLayout h;
    ASSERT_EQ(sizeof h.TotalDamageDone, (size_t)4);
    ASSERT_EQ(sizeof h.DeathType,       (size_t)1);
    ASSERT_EQ(sizeof h.MainDamageType,  (size_t)1);
    ASSERT_EQ(sizeof h.CauseType,       (size_t)1);
    ASSERT_EQ(sizeof h.ImpactPosition,  (size_t)12);
    ASSERT_EQ(sizeof h.ImpactDirection, (size_t)12);
    ASSERT_EQ(sizeof h.ImpactForce,     (size_t)4);
    ASSERT_EQ(sizeof h.ArmorAbsorption, (size_t)4);
    ASSERT_EQ(sizeof h.LifeSteal,       (size_t)4);
    ASSERT_EQ(sizeof h.EffectFlags,     (size_t)4);
    ASSERT_EQ(sizeof h.Inflicter,       (size_t)8);
    ASSERT_EQ(sizeof h.InflicterOwner,  (size_t)8);
    ASSERT_EQ(sizeof h.ThrownObject,    (size_t)8);
    ASSERT_EQ(sizeof h.HitWith,         (size_t)1);
    ASSERT_EQ(sizeof h.AttackAbility,   (size_t)1);
    ASSERT_EQ(sizeof h.SaveAbility,     (size_t)1);
    ASSERT_EQ(sizeof h.SpellAttackType, (size_t)1);
    ASSERT_EQ(sizeof h.SpellId,         (size_t)4);
    ASSERT_EQ(sizeof h.SpellSchool,     (size_t)1);
    ASSERT_EQ(sizeof h.Flags,           (size_t)1);
    ASSERT_EQ(sizeof h.SpellLevel,      (size_t)4);
    ASSERT_EQ(sizeof h.SpellPowerLevel, (size_t)4);
    ASSERT_EQ(sizeof h.TotalHealDone,   (size_t)4);
}

/*
 * The three EntityHandles are declared AFTER the three one-byte enums but live
 * BEFORE them in memory. Taking the mangled member list as an offset order
 * would put Inflicter at 0x4f and HitWith at 0x30 — plausible, and wrong.
 */
TEST(hit_desc_handles_precede_the_enum_bytes) {
    ASSERT_TRUE(HIT_DESC_OFF_INFLICTER < HIT_DESC_OFF_HIT_WITH);
    ASSERT_EQ(HIT_DESC_OFF_INFLICTER_OWNER, HIT_DESC_OFF_INFLICTER + 8);
    ASSERT_EQ(HIT_DESC_OFF_THROWN_OBJECT, HIT_DESC_OFF_INFLICTER_OWNER + 8);
}

/* ------------------------------------------------------------- AttackDesc */

TEST(attack_desc_offsets_match_mirror) {
    ASSERT_EQ(offsetof(AttackDescLayout, TotalDamageDone),
              (size_t)ATTACK_DESC_OFF_TOTAL_DAMAGE_DONE);
    ASSERT_EQ(offsetof(AttackDescLayout, TotalHealDone),
              (size_t)ATTACK_DESC_OFF_TOTAL_HEAL_DONE);
    ASSERT_EQ(offsetof(AttackDescLayout, DamageListElements),
              (size_t)ATTACK_DESC_OFF_DAMAGE_LIST_ELEMENTS);
    ASSERT_EQ(offsetof(AttackDescLayout, DamageListCapacity),
              (size_t)ATTACK_DESC_OFF_DAMAGE_LIST_CAPACITY);
    ASSERT_EQ(offsetof(AttackDescLayout, DamageListSize),
              (size_t)ATTACK_DESC_OFF_DAMAGE_LIST_SIZE);
    ASSERT_EQ(sizeof(AttackDescLayout), (size_t)ATTACK_DESC_SIZE);
}

/*
 * HitResult is HitDesc followed immediately by AttackDesc: the Execute
 * prologue does `add x27, x0, #0x1a8` right after the HitDesc copy ctor and
 * `add x20, x19, #0x1c8` after the AttackDesc one. The DealtDamage decoder
 * reaches Result.Attack by adding HIT_DESC_SIZE, so a shrunk HitDesc would
 * decode the wrong bytes as an AttackDesc.
 */
TEST(hit_result_halves_are_adjacent) {
    ASSERT_EQ(HIT_DESC_SIZE, 0x1a8);
    ASSERT_EQ(HIT_DESC_SIZE + ATTACK_DESC_SIZE, 0x1c8);
}

/* ------------------------------------------------------------- DamagePair */

TEST(damage_pair_layout) {
    ASSERT_EQ(offsetof(DamagePairLayout, Amount), (size_t)DAMAGE_PAIR_OFF_AMOUNT);
    ASSERT_EQ(offsetof(DamagePairLayout, DamageType),
              (size_t)DAMAGE_PAIR_OFF_DAMAGE_TYPE);
    ASSERT_EQ(sizeof(DamagePairLayout), (size_t)DAMAGE_PAIR_SIZE);
    /* AddDamage @0x1011787e8 walks the list with `ldr x10, [x21], #0x8`. */
    ASSERT_EQ(DAMAGE_PAIR_SIZE, 8);
}

/* ----------------------------------------------------------- enum tables */

/*
 * DealDamage's headline field. The decoder turns the byte at functor+0x47 into
 * one of these strings; mods compare it with == against literals, so a renamed
 * or renumbered label is a silent behaviour change for every such mod.
 * Values are from Noesis::TypeEnumCreator<EDamageType>::Fill @0x10250eb68 and
 * corroborated by _Enum_DamageType NAMES/VALUES on 4.1.1.7398727.
 */
TEST(damage_type_labels_match_game_tables) {
    EnumTypeInfo *info = enum_registry_find_by_name("DamageType");
    ASSERT_NOT_NULL(info);
    int idx = info->registry_index;
    ASSERT_STR_EQ(enum_find_label(idx, 0),  "None");
    ASSERT_STR_EQ(enum_find_label(idx, 1),  "Slashing");
    ASSERT_STR_EQ(enum_find_label(idx, 2),  "Piercing");
    ASSERT_STR_EQ(enum_find_label(idx, 3),  "Bludgeoning");
    ASSERT_STR_EQ(enum_find_label(idx, 4),  "Acid");
    ASSERT_STR_EQ(enum_find_label(idx, 5),  "Thunder");
    ASSERT_STR_EQ(enum_find_label(idx, 6),  "Necrotic");
    ASSERT_STR_EQ(enum_find_label(idx, 7),  "Fire");
    ASSERT_STR_EQ(enum_find_label(idx, 8),  "Lightning");
    ASSERT_STR_EQ(enum_find_label(idx, 9),  "Cold");
    ASSERT_STR_EQ(enum_find_label(idx, 10), "Psychic");
    ASSERT_STR_EQ(enum_find_label(idx, 11), "Poison");
    ASSERT_STR_EQ(enum_find_label(idx, 12), "Radiant");
    ASSERT_STR_EQ(enum_find_label(idx, 13), "Force");
    /* 14 is Windows' Sentinel; this build's tables stop at 13. */
    ASSERT_NULL(enum_find_label(idx, 14));
}

/*
 * SpellFlags is a bitmask, and the SpellProto.SpellFlags array is built one
 * label per set bit via enum_find_label. enum_find_label returns the FIRST
 * registered entry for a value, so the game's own spelling must stay ahead of
 * the Windows alias or the array silently changes dialect.
 */
TEST(spell_flags_prefers_windows_spelling_over_game_alias) {
    EnumTypeInfo *info = enum_registry_find_by_name("SpellFlags");
    ASSERT_NOT_NULL(info);
    ASSERT_TRUE(info->is_bitfield);
    int idx = info->registry_index;

    /* Emitted labels are the Windows spellings: mods compare these as string
     * literals, e.g. Expansion's EXP_IsSpell does `if flag == "IsSpell"`, and
     * this build calls that bit "Spell". The Windows name is the API contract
     * here, so it must win value -> label. */
    ASSERT_STR_EQ(enum_find_label(idx, 0x400), "IsSpell");
    ASSERT_STR_EQ(enum_find_label(idx, 0x1),   "HasVerbalComponent");
    ASSERT_STR_EQ(enum_find_label(idx, 0x40),  "IsConcentration");
    ASSERT_STR_EQ(enum_find_label(idx, 0x2000000ULL), "IsHarmful");

    /* A bit Windows does not name still emits this build's own spelling. */
    ASSERT_STR_EQ(enum_find_label(idx, 0x200000000000000ULL), "ChasmRecovery");

    /* Both dialects still resolve name -> value, so Ext.Enums.SpellFlags
     * accepts either. */
    ASSERT_EQ(enum_find_value(idx, "IsSpell"), (int64_t)0x400);
    ASSERT_EQ(enum_find_value(idx, "Spell"),   (int64_t)0x400);
    ASSERT_EQ(enum_find_value(idx, "HasVerbalComponent"), (int64_t)0x1);
    ASSERT_EQ(enum_find_value(idx, "Verbal"),  (int64_t)0x1);
    ASSERT_EQ(enum_find_value(idx, "Wildshape"), (int64_t)0x2000000000ULL);

    /* Bit 43 has no entry in this build's table under either spelling. */
    ASSERT_NULL(enum_find_label(idx, 0x80000000000ULL));
    ASSERT_EQ(enum_find_value(idx, "RangeIgnoreBlindness"), (int64_t)-1);
    ASSERT_EQ(info->allowed_flags & 0x80000000000ULL, 0ULL);
}

/*
 * The decoder omits a field entirely when its enum type is not registered,
 * rather than pushing the raw ordinal under a name the API documents as a
 * string. If someone later derives these label sets on this build and
 * registers them, this test fails and forces the decoder to start exposing
 * HitDesc.DeathType / CauseType / HitWith / SpellAttackType — instead of the
 * registry quietly gaining names nothing reads.
 */
TEST(unverified_hit_enums_stay_unregistered) {
    ASSERT_NULL(enum_registry_find_by_name("DeathType"));
    ASSERT_NULL(enum_registry_find_by_name("CauseType"));
    ASSERT_NULL(enum_registry_find_by_name("HitWith"));
    ASSERT_NULL(enum_registry_find_by_name("SpellAttackType"));
}

void register_deal_damage_payload_tests(void) {
    printf("DealDamage payload layouts:\n");
    RUN_TEST(deal_damage_functor_damage_type_offset);
    RUN_TEST(spell_id_offsets_match_mirror);
    RUN_TEST(spell_id_field_widths);
    RUN_TEST(spell_info_embeds_spell_id_at_zero);
    RUN_TEST(spell_prototype_head_offsets);
    RUN_TEST(hit_desc_offsets_match_mirror);
    RUN_TEST(hit_desc_field_widths_match_deserializer);
    RUN_TEST(hit_desc_handles_precede_the_enum_bytes);
    RUN_TEST(attack_desc_offsets_match_mirror);
    RUN_TEST(hit_result_halves_are_adjacent);
    RUN_TEST(damage_pair_layout);

    enum_registry_init();
    enum_register_definitions();
    RUN_TEST(damage_type_labels_match_game_tables);
    RUN_TEST(spell_flags_prefers_windows_spelling_over_game_alias);
    RUN_TEST(unverified_hit_enums_stay_unregistered);
}
