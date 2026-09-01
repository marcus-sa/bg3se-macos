/*
 * spell_meta_layout.h - eoc::spell::SpellMeta layout for BG3 4.1.1.7398727 (arm64)
 *
 * Every offset here is read out of the shipped arm64 slice. The Windows BG3SE
 * header (GameDefinitions/Components/Spell.h:104) is used for field ORDER only:
 * its natural packing is 0x60, while component_offsets.h previously declared an
 * element size of 80, and an 80-byte stride puts Spells[2] 32 bytes inside
 * Spells[1] — every element after the first decodes as garbage.
 *
 * Evidence (disassembly transcribed in ghidra/offsets/SPELL_META_LAYOUT.md):
 *
 *   sizeof / array stride = 0x60
 *     ls::DynamicArray<eoc::spell::SpellMeta, ls::TaggedAllocator<int>>
 *       ::Reallocate(int) @ 0x1019d9858
 *         mov  w10, #0x60           ; =96
 *         smaddl x8, w8, w10, x9    ; buf + count * 0x60
 *     and the same function's destroy loop advances `add x22, x22, #0x60`,
 *     releasing a FixedString at +0x00 and one at +0x54 per element.
 *
 *   every field offset
 *     ecs::sync::Serialize<eoc::spell::SpellMeta, ls::FieldMeta<...>>(...)
 *       @ 0x1019d7a4c. The mangled symbol carries the member list in
 *       declaration order — m_Id, m_BoostSourceHandle, m_LearningStrategy,
 *       m_PreparationStrategy, m_CastActionResource, m_CastAbility,
 *       m_CooldownType, m_ContainerId, m_IsContainer — and the body writes each
 *       from a literal displacement off the object with a literal byte count:
 *         ldr w0,[x20]        -> FixedString  @0x00
 *         add x8,x20,#0x8  n=1 -> u8          @0x08
 *         add x8,x20,#0x20 n=16 -> Guid       @0x20
 *         add x8,x20,#0x10 n=16 -> Guid       @0x10
 *         ldr x8,[x20,#0x30]  -> EntityHandle @0x30
 *         add x8,x20,#0x38 n=1 -> u8          @0x38
 *         add x8,x20,#0x39 n=1 -> u8          @0x39
 *         add x8,x20,#0x40 n=16 -> Guid       @0x40
 *         add x8,x20,#0x50 n=1 -> u8          @0x50
 *         add x8,x20,#0x51 n=1 -> u8          @0x51
 *         ldr w0,[x20,#0x54]  -> FixedString  @0x54
 *         add x8,x20,#0x58 n=1 -> bool        @0x58
 *
 *   which Guid is Source and which is ProgressionSource
 *     The serializer emits +0x20 before +0x10, so it cannot settle the pairing
 *     on its own. eoc::spell::LEGACY_Visit(ls::ObjectVisitor*,
 *     eoc::spell::MetaId&) @ 0x101f7dc50 does: it visits +0x08, +0x20, +0x10,
 *     +0x00 keyed by the file-static FixedStrings strSourceType (0x1089f36f0),
 *     strProgressionSource (0x1089f36e0), strSourceId (0x1089f36e8) and
 *     strOriginatorPrototype (0x1089f36f8). So +0x10 is Source (the game calls
 *     it SourceId) and +0x20 is ProgressionSource — the opposite of the order
 *     the serializer happens to write them in.
 */

#ifndef BG3SE_SPELL_META_LAYOUT_H
#define BG3SE_SPELL_META_LAYOUT_H

#include <stdint.h>

/* Array element stride. component_offsets.h must declare this as elemSize. */
#define SPELL_META_SIZE 0x60

/* eoc::spell::MetaId, embedded at offset 0 of SpellMeta. */
#define SPELL_META_OFF_ORIGINATOR_PROTOTYPE 0x00  /* FixedString */
#define SPELL_META_OFF_SOURCE_TYPE          0x08  /* eoc::spell::ESourceType (u8) */
#define SPELL_META_OFF_SOURCE               0x10  /* Guid (strSourceId) */
#define SPELL_META_OFF_PROGRESSION_SOURCE   0x20  /* Guid */

#define SPELL_META_OFF_BOOST_HANDLE         0x30  /* EntityHandle */
#define SPELL_META_OFF_LEARNING_STRATEGY    0x38  /* eoc::spell::ELearningStrategy (u8) */
#define SPELL_META_OFF_PREPARE_TYPE         0x39  /* eoc::spell::EPreparationStrategy (u8) */
#define SPELL_META_OFF_CASTING_RESOURCE     0x40  /* Guid */
#define SPELL_META_OFF_CASTING_ABILITY      0x50  /* EAbility (u8) */
#define SPELL_META_OFF_COOLDOWN_TYPE        0x51  /* eoc::ECooldownType (u8) */
#define SPELL_META_OFF_CONTAINER_SPELL      0x54  /* FixedString */
#define SPELL_META_OFF_LINKED_CONTAINER     0x58  /* bool */

/*
 * Mirror of the game struct. Nothing reads foreign memory through this type —
 * the decoder uses safe_memory_read* at the SPELL_META_OFF_* displacements — it
 * exists so tier0 can pin those constants against a layout the C compiler
 * computes independently, and so a future edit that moves one field without
 * moving the other trips a test instead of silently shifting a decode.
 */
typedef struct {
    uint32_t OriginatorPrototype;     /* 0x00 */
    uint8_t  _pad04[4];               /* 0x04 */
    uint8_t  SourceType;              /* 0x08 */
    uint8_t  _pad09[7];               /* 0x09 */
    uint8_t  Source[16];              /* 0x10 */
    uint8_t  ProgressionSource[16];   /* 0x20 */
    uint64_t BoostHandle;             /* 0x30 */
    uint8_t  LearningStrategy;        /* 0x38 */
    uint8_t  PrepareType;             /* 0x39 */
    uint8_t  _pad3a[6];               /* 0x3a */
    uint8_t  PreferredCastingResource[16]; /* 0x40 */
    uint8_t  SpellCastingAbility;     /* 0x50 */
    uint8_t  CooldownType;            /* 0x51 */
    uint8_t  _pad52[2];               /* 0x52 */
    uint32_t ContainerSpell;          /* 0x54 */
    uint8_t  LinkedSpellContainer;    /* 0x58 */
    uint8_t  _pad59[7];               /* 0x59 */
} SpellMetaLayout;

#endif /* BG3SE_SPELL_META_LAYOUT_H */
