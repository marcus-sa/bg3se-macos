/*
 * deal_damage_layout.h - member layouts of the objects the DealDamage /
 * DealtDamage / BeforeDealDamage hooks receive as arguments.
 *
 * Game build: Baldur's Gate 3 4.1.1.7398727, macOS arm64.
 *
 * The hook argument ADDRESSES are established in functor_types.h and
 * ghidra/offsets/DEALDAMAGE_HOOKS.md. This header is the separate question of
 * what lives INSIDE those objects, and every offset below was read off this
 * build's own arm64 slice, never taken from the Windows headers: MSVC and
 * Apple arm64 order and pack differently, and this repo has already shipped
 * three bugs from layouts copied out of documents.
 *
 * Full derivation with instruction citations:
 *   ghidra/offsets/DEALDAMAGE_PAYLOAD_LAYOUTS.md
 *
 * Each *_OFF_* constant is what src/lua/lua_events.c actually reads through.
 * The mirror structs below are laid out independently by the compiler, and
 * tests/tier0/test_deal_damage_payload.c pins one against the other — so an
 * edit that widens a field without moving its constant fails a test instead of
 * silently shifting every decode behind it.
 */

#ifndef DEAL_DAMAGE_LAYOUT_H
#define DEAL_DAMAGE_LAYOUT_H

#include <stdint.h>

/* =========================================================================
 * eoc::StatsFunctorDealDamage  (hook parameter 1)
 *
 * sizeof == 0x68, from `mov w0, #0x68` feeding operator new in
 * eoc::StatsFunctorDealDamage::Clone() @0x101fb14b4.
 *
 * Only DamageType is decoded. Clone copies four consecutive bytes at 0x47
 * (`ldur w8, [x19, #0x47]`), so 0x48..0x4a are three more one-byte enums, but
 * the label tables for eoc::EDealDamageWeaponType / DamageRollValue /
 * DamageTypeValue were not derived on this build and a raw integer under a
 * documented enum field name reads like a decode. They stay unexposed.
 * ========================================================================= */

/*
 * Parse @0x101fb0d14 calls StringToDamageType(ls::_StringView<char> const&)
 * and stores the result with `strb w0, [x19, #0x47]` on the very next
 * instruction. Corroborated in GetDamageRollAndDamageTypeInternal
 * @0x105772d68, which loads `ldrb w0, [x19, #0x47]` straight into the leading
 * `EDamageType` parameter of MergeDamage().
 */
#define DEAL_DAMAGE_FUNCTOR_OFF_DAMAGE_TYPE   0x47
#define DEAL_DAMAGE_FUNCTOR_SIZE              0x68

/* =========================================================================
 * eoc::spell::SpellId   (hook parameter 17 — Windows "SpellId2")
 * eoc::spell::SpellInfo (hook parameter 6  — Windows "SpellIdWithPrototype")
 *
 * SpellInfo derives from SpellId at offset 0, which derives from
 * eoc::spell::MetaId at offset 0. Proof that the derivation offset is really
 * zero: StatsFunctorDealDamage::Execute @0x1057736b8 passes its SpellInfo&
 * argument unadjusted as the `eoc::spell::MetaId const&` parameter of
 * GetSpellBoostHandle(). The MetaId half is therefore the layout already
 * documented in ghidra/offsets/SPELL_META_LAYOUT.md and mirrored in
 * src/entity/spell_meta_layout.h; it is repeated here rather than included so
 * this header stays inside src/stats/.
 *
 * Field names are the game's own: eoc::spell::LEGACY_Visit(ls::SavegameVisitor*,
 * eoc::spell::SpellId&) @0x101f7e248 keys each visited offset with a file-static
 * FixedString whose BSS symbol names the field.
 * ========================================================================= */

#define SPELL_ID_OFF_ORIGINATOR_PROTOTYPE   0x00  /* strOriginatorPrototype */
#define SPELL_ID_OFF_SOURCE_TYPE            0x08  /* strSourceType          */
#define SPELL_ID_OFF_SOURCE                 0x10  /* strSourceId            */
#define SPELL_ID_OFF_PROGRESSION_SOURCE     0x20  /* strProgressionSource   */
#define SPELL_ID_OFF_PROTOTYPE              0x30  /* strPrototype           */
#define SPELL_ID_SIZE                       0x38

/*
 * SpellInfo adds the resolved prototype pointer. GetSpellRanges @0x101f33230
 * does `ldr x8, [x4, #0x38]` on its SpellInfo& argument, parks the value in
 * eoc::spell::RangeContext+0x00, and GetSpellRanges(RangeContext const&)
 * @0x101f328b0 then loads it back and passes it as the object pointer of
 * eoc::SpellPrototype::GetTargetRadius(...).
 *
 * 0x40..0x57 is the std::optional<Guid> SpellCastSource that Windows documents;
 * SpellInfo::operator= @0x105782500 copies exactly those 32 bytes as two 16-byte
 * moves, which bounds the region but does not say which half is the value and
 * which the engaged flag. Not exposed.
 */
#define SPELL_INFO_OFF_SPELL_PROTO          0x38
#define SPELL_INFO_SIZE                     0x58  /* SpellInfo::operator=    */
                                                  /* @0x105782434 touches    */
                                                  /* 0x00..0x57 and no more  */

/* =========================================================================
 * eoc::SpellPrototype — head only.
 *
 * Everything past 0x18 is undecoded; this is not a full mirror and has no
 * size constant, deliberately, because nothing here allocates one.
 * ========================================================================= */

/*
 * +0x00 int32 StatsObjectIndex: GetSpellRanges(RangeContext const&)
 *       @0x101f32960 loads it, sign-checks it (`tbnz w9, #0x1f`), bounds-checks
 *       it against the RPGStats object count at manager+0x118 and indexes the
 *       object array at manager+0xc8 with it.
 * +0x04 uint32 SpellTypeId: @0x101f328ec `ldr w19, [x21, #0x4]` drives a
 *       1..11 jump table over the spell kinds. No verified label table on this
 *       build, so it is not exposed as a string.
 * +0x08 FixedString SpellId (the prototype's stats name):
 *       GetModifiedSpellFlags @0x101f2fd4c reads it and uses it as the hash key
 *       into the spell-modification container.
 * +0x10 uint64 SpellFlags (ls::EnumFlags<eoc::ESpellFlags>):
 *       GetModifiedSpellFlags @0x101f2fdcc returns *(uint64*)(proto+0x10)
 *       verbatim when there is no modification container to apply.
 *
 * 0x0c is where Windows puts uint8 SpellSchool. Not derived here, not exposed.
 */
#define SPELL_PROTOTYPE_OFF_STATS_OBJECT_INDEX  0x00
#define SPELL_PROTOTYPE_OFF_SPELL_TYPE_ID       0x04
#define SPELL_PROTOTYPE_OFF_SPELL_ID            0x08
#define SPELL_PROTOTYPE_OFF_SPELL_FLAGS         0x10

/* =========================================================================
 * eoc::HitDesc  (hook parameter 10, and BeforeDealDamage parameter 2)
 *
 * Offsets and widths come from
 *   ecs::sync::Deserialize<eoc::HitDesc, ls::FieldMeta<...>> @0x1019c24d0,
 * whose mangled name carries all 23 members in declaration order with their
 * real names and types, and whose body reads each one into a literal
 * displacement off the object with a literal byte count.
 *
 * Note the members are NOT in offset order: the three EntityHandles at
 * 0x30/0x38/0x40 are declared after the three one-byte enums at 0x4c..0x4e.
 * Reading the declaration order as an offset order would misplace all six.
 *
 * sizeof == 0x1a8, from StatsFunctorDealDamage::Execute @0x1057735bc
 * (`add x27, x0, #0x1a8` right after the HitDesc copy constructor returns).
 * ========================================================================= */

#define HIT_DESC_OFF_TOTAL_DAMAGE_DONE     0x000  /* int32                   */
#define HIT_DESC_OFF_DEATH_TYPE            0x004  /* uint8  EDeathType       */
#define HIT_DESC_OFF_MAIN_DAMAGE_TYPE      0x005  /* uint8  EDamageType      */
#define HIT_DESC_OFF_CAUSE_TYPE            0x006  /* uint8  TCauseType       */
#define HIT_DESC_OFF_IMPACT_POSITION       0x008  /* Vector3f                */
#define HIT_DESC_OFF_IMPACT_DIRECTION      0x014  /* Vector3f                */
#define HIT_DESC_OFF_IMPACT_FORCE          0x020  /* float                   */
#define HIT_DESC_OFF_ARMOR_ABSORPTION      0x024  /* int32                   */
#define HIT_DESC_OFF_LIFE_STEAL            0x028  /* int32                   */
#define HIT_DESC_OFF_EFFECT_FLAGS          0x02c  /* uint32 EDamageEffectFlag*/
#define HIT_DESC_OFF_INFLICTER             0x030  /* EntityHandle            */
#define HIT_DESC_OFF_INFLICTER_OWNER       0x038  /* EntityHandle            */
#define HIT_DESC_OFF_THROWN_OBJECT         0x040  /* EntityHandle            */
#define HIT_DESC_OFF_HIT_WITH              0x04c  /* uint8  EHitWith         */
#define HIT_DESC_OFF_ATTACK_ABILITY        0x04d  /* uint8  EAbility         */
#define HIT_DESC_OFF_SAVE_ABILITY          0x04e  /* uint8  EAbility         */
#define HIT_DESC_OFF_SPELL_ATTACK_TYPE     0x04f  /* uint8  ESpellAttackType */
#define HIT_DESC_OFF_SPELL_ID              0x148  /* FixedString             */
#define HIT_DESC_OFF_SPELL_SCHOOL          0x159  /* uint8  ESpellSchool     */
#define HIT_DESC_OFF_FLAGS                 0x15b  /* uint8  EHitDescFlag     */
#define HIT_DESC_OFF_SPELL_LEVEL           0x15c  /* int32                   */
#define HIT_DESC_OFF_SPELL_POWER_LEVEL     0x160  /* int32                   */
#define HIT_DESC_OFF_TOTAL_HEAL_DONE       0x164  /* int32                   */
/*
 * The DamageList is not network-serialized, so it is absent from the
 * Deserialize member list. eoc::AttackDesc::AttackDesc(eoc::HitDesc const&, …)
 * @0x101178644 reads its element pointer from HitDesc+0x198 and its element
 * count from HitDesc+0x1a4, then copies `count` 8-byte elements.
 */
#define HIT_DESC_OFF_DAMAGE_LIST_ELEMENTS  0x198  /* TDamagePair*            */
#define HIT_DESC_OFF_DAMAGE_LIST_CAPACITY  0x1a0  /* int32                   */
#define HIT_DESC_OFF_DAMAGE_LIST_SIZE      0x1a4  /* int32                   */
#define HIT_DESC_SIZE                      0x1a8

/* =========================================================================
 * eoc::AttackDesc (hook parameter 11, and BeforeDealDamage parameter 3)
 *
 * From eoc::AttackDesc::AttackDesc(eoc::HitDesc const&, …) @0x10117860c and
 * eoc::AttackDesc::operator= @0x101178738: `stp w8, w9, [x0]` writes the two
 * int32s at 0x00/0x04, `ldrh w8, [x1, #0x8]` copies two bytes at 0x08, and the
 * DynamicArray occupies 0x10 (elements) / 0x18 (capacity) / 0x1c (size) —
 * `ldp w9, w8, [x19, #0x18]` loads capacity then size as one pair.
 *
 * sizeof == 0x20, independently measured in DEALDAMAGE_HOOKS.md from the
 * HitResult layout (AttackDesc spans 0x1a8..0x1c8).
 * ========================================================================= */

#define ATTACK_DESC_OFF_TOTAL_DAMAGE_DONE     0x00  /* int32 */
#define ATTACK_DESC_OFF_TOTAL_HEAL_DONE       0x04  /* int32 */
#define ATTACK_DESC_OFF_DAMAGE_LIST_ELEMENTS  0x10  /* TDamagePair* */
#define ATTACK_DESC_OFF_DAMAGE_LIST_CAPACITY  0x18  /* int32 */
#define ATTACK_DESC_OFF_DAMAGE_LIST_SIZE      0x1c  /* int32 */
#define ATTACK_DESC_SIZE                      0x20

/* =========================================================================
 * TDamagePair — the element type of both damage lists.
 *
 * eoc::AttackDesc::AddDamage(DamageList const&) @0x1011787e8 copies elements
 * with `ldr x10, [x21], #0x8` (stride 8), drops entries whose `ldr w12, [x10]`
 * int32 is zero, and merges entries whose `ldrb w13, [elem + 0x4]` one-byte
 * discriminators are equal by summing the int32s at +0x00.
 * ========================================================================= */

#define DAMAGE_PAIR_OFF_AMOUNT       0x00  /* int32              */
#define DAMAGE_PAIR_OFF_DAMAGE_TYPE  0x04  /* uint8  EDamageType */
#define DAMAGE_PAIR_SIZE             0x08

/*
 * Guard against a runaway/garbage element count. Both damage lists are engine
 * DynamicArrays whose size we read out of live memory while the game thread is
 * inside the hook; a torn or stale read must cost a bounded number of
 * safe_memory_read calls, not a multi-second stall inside a Lua callback.
 * BG3's own damage lists hold one entry per damage type present on a hit.
 */
#define DAMAGE_LIST_MAX_ELEMENTS     64

/* =========================================================================
 * Mirror structs — laid out by the compiler, pinned against the constants
 * above by tests/tier0/test_deal_damage_payload.c. Nothing reads game memory
 * through these; they exist so that a widened field fails a test.
 * ========================================================================= */

typedef struct {
    uint32_t OriginatorPrototype;   /* 0x00 FixedString */
    uint8_t  _pad04[4];
    uint8_t  SourceType;            /* 0x08 */
    uint8_t  _pad09[7];
    uint8_t  Source[16];            /* 0x10 Guid */
    uint8_t  ProgressionSource[16]; /* 0x20 Guid */
    uint32_t Prototype;             /* 0x30 FixedString */
    uint8_t  _pad34[4];
} SpellIdLayout;                    /* 0x38 */

typedef struct {
    SpellIdLayout Id;               /* 0x00 */
    void         *SpellProto;       /* 0x38 */
    uint8_t       _pad40[0x18];     /* 0x40 std::optional<Guid>, undecoded */
} SpellInfoLayout;                  /* 0x58 */

typedef struct {
    int32_t  TotalDamageDone;       /* 0x000 */
    uint8_t  DeathType;             /* 0x004 */
    uint8_t  MainDamageType;        /* 0x005 */
    uint8_t  CauseType;             /* 0x006 */
    uint8_t  _pad007;
    float    ImpactPosition[3];     /* 0x008 */
    float    ImpactDirection[3];    /* 0x014 */
    float    ImpactForce;           /* 0x020 */
    int32_t  ArmorAbsorption;       /* 0x024 */
    int32_t  LifeSteal;             /* 0x028 */
    uint32_t EffectFlags;           /* 0x02c */
    uint64_t Inflicter;             /* 0x030 */
    uint64_t InflicterOwner;        /* 0x038 */
    uint64_t ThrownObject;          /* 0x040 */
    uint8_t  _pad048[4];            /* 0x048 not serialized; Windows: StoryActionId */
    uint8_t  HitWith;               /* 0x04c */
    uint8_t  AttackAbility;         /* 0x04d */
    uint8_t  SaveAbility;           /* 0x04e */
    uint8_t  SpellAttackType;       /* 0x04f */
    uint8_t  _pad050[0xf8];         /* 0x050 undecoded */
    uint32_t SpellId;               /* 0x148 FixedString */
    uint8_t  _pad14c[0xd];          /* 0x14c undecoded */
    uint8_t  SpellSchool;           /* 0x159 */
    uint8_t  _pad15a;
    uint8_t  Flags;                 /* 0x15b */
    int32_t  SpellLevel;            /* 0x15c */
    int32_t  SpellPowerLevel;       /* 0x160 */
    int32_t  TotalHealDone;         /* 0x164 */
    uint8_t  _pad168[0x30];         /* 0x168 undecoded */
    void    *DamageListElements;    /* 0x198 */
    int32_t  DamageListCapacity;    /* 0x1a0 */
    int32_t  DamageListSize;        /* 0x1a4 */
} HitDescLayout;                    /* 0x1a8 */

typedef struct {
    int32_t  TotalDamageDone;       /* 0x00 */
    int32_t  TotalHealDone;         /* 0x04 */
    uint8_t  field_8;               /* 0x08 Windows: InitialHPPercentage */
    uint8_t  field_9;               /* 0x09 */
    uint8_t  _pad0a[6];
    void    *DamageListElements;    /* 0x10 */
    int32_t  DamageListCapacity;    /* 0x18 */
    int32_t  DamageListSize;        /* 0x1c */
} AttackDescLayout;                 /* 0x20 */

typedef struct {
    int32_t Amount;                 /* 0x00 */
    uint8_t DamageType;             /* 0x04 */
    uint8_t _pad05[3];
} DamagePairLayout;                 /* 0x08 */

#endif /* DEAL_DAMAGE_LAYOUT_H */
