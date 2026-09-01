# `eoc::spell::SpellMeta` layout — 4.1.1.7398727

**Game version:** Baldur's Gate 3 v4.1.1.7398727 (`CFBundleVersion` of the
installed app bundle)

**Platform:** macOS ARM64

**Derived:** 2026-09-02

**Status:** every offset, the struct size, and the field names read directly out
of the shipped arm64 slice by `nm` + `objdump` disassembly. No offset here is
inferred from the Windows headers.

## Why this document exists

`Ext.Entity.Get(char).SpellContainer.Spells[i]` returned a stub table carrying
only `__ptr` / `__index` / `__size`: `ELEM_TYPE_SPELL_META` shared a `case` with
the generic fallback in `src/entity/component_property.c`. Mods reading
`spell.SpellId.OriginatorPrototype` therefore failed with
`attempt to index a nil value (field 'SpellId')` on every
`EnteredForceTurnBased`.

`component_offsets.h` also declared the element size as **80**, which is wrong.
Element size is the array *stride*, so an 80-byte stride puts `Spells[1]`
32 bytes inside `Spells[0]` — it does not fault, it just hands mods
plausible-looking garbage for every element after the first.

## Struct size / array stride — 0x60

```text
0x1019d9858  t  ls::DynamicArray<eoc::spell::SpellMeta,
                                 ls::TaggedAllocator<int>>::Reallocate(int)
```

```asm
1019d9924: ldr     x9, [x19]
1019d9928: mov     w10, #0x60              ; =96
1019d992c: smaddl  x8, w8, w10, x9         ; buf + count * 0x60
```

The same function's per-element destroy loop advances by the same stride and
releases exactly two `FixedString`s per element, at `+0x00` and `+0x54`:

```asm
1019d98b4: add     x22, x22, #0x60
1019d98c0: ldr     x25, [x19]
1019d98c4: add     x8, x25, x22
1019d98c8: ldr     w8, [x8, #0x54]         ; ContainerId  -> gst::Map::Release
1019d98e8: ldr     w8, [x25, x22]          ; OriginatorPrototype -> Release
```

That independently corroborates the two FixedString positions.

## Field offsets — `ecs::sync::Serialize<eoc::spell::SpellMeta, ...>`

```text
0x1019d7a4c  t  ecs::sync::Serialize<eoc::spell::SpellMeta,
                  ls::FieldMeta<eoc::spell::MetaId eoc::spell::SpellMeta::*,
                                &SpellMeta::m_Id>,
                  ls::FieldMeta<... &SpellMeta::m_BoostSourceHandle>,
                  ls::FieldMeta<... &SpellMeta::m_LearningStrategy>,
                  ls::FieldMeta<... &SpellMeta::m_PreparationStrategy>,
                  ls::FieldMeta<... &SpellMeta::m_CastActionResource>,
                  ls::FieldMeta<... &SpellMeta::m_CastAbility>,
                  ls::FieldMeta<... &SpellMeta::m_CooldownType>,
                  ls::FieldMeta<... &SpellMeta::m_ContainerId>,
                  ls::FieldMeta<... &SpellMeta::m_IsContainer>>
                (ecs::sync::NetworkWriter&, SpellMeta const&, ls::TypeList<...>)
```

The mangled symbol carries the complete member list, in declaration order, with
each member's real name and type. The body then writes each member into the
scratch buffer from a literal displacement off the object pointer (`x20`) with a
literal byte count:

| Disassembly (at `0x1019d7a4c`+) | Offset | Bytes | Member |
|---|---|---|---|
| `ldr w0, [x20]` → `ls::gst::Get(u32)` | `0x00` | 4 | `m_Id.OriginatorPrototype` (FixedString) |
| `add x8, x20, #0x8` ; len `#0x1` | `0x08` | 1 | `m_Id.SourceType` (`eoc::spell::ESourceType`) |
| `add x8, x20, #0x20` ; len `#0x10` | `0x20` | 16 | `m_Id` Guid |
| `add x8, x20, #0x10` ; len `#0x10` | `0x10` | 16 | `m_Id` Guid |
| `ldr x8, [x20, #0x30]` | `0x30` | 8 | `m_BoostSourceHandle` (EntityHandle) |
| `add x8, x20, #0x38` ; len `#0x1` | `0x38` | 1 | `m_LearningStrategy` |
| `add x8, x20, #0x39` ; len `#0x1` | `0x39` | 1 | `m_PreparationStrategy` |
| `add x8, x20, #0x40` ; len `#0x10` | `0x40` | 16 | `m_CastActionResource` (Guid) |
| `add x8, x20, #0x50` ; len `#0x1` | `0x50` | 1 | `m_CastAbility` (`EAbility`) |
| `add x8, x20, #0x51` ; len `#0x1` | `0x51` | 1 | `m_CooldownType` (`eoc::ECooldownType`) |
| `ldr w0, [x20, #0x54]` → `ls::gst::Get(u32)` | `0x54` | 4 | `m_ContainerId` (FixedString) |
| `add x8, x20, #0x58` ; len `#0x1` | `0x58` | 1 | `m_IsContainer` (bool) |

Sum with natural alignment: `0x58 + 1` rounded to the struct's 8-byte alignment
= `0x60`, matching the stride above.

## Which Guid is `Source` and which is `ProgressionSource`

The serializer emits `+0x20` **before** `+0x10`, so it cannot settle the pairing
on its own. The savegame visitor does:

```text
0x101f7dc50  t  eoc::spell::LEGACY_Visit(ls::ObjectVisitor*,
                                         eoc::spell::MetaId&)
```

It visits `+0x08`, `+0x20`, `+0x10`, `+0x00` in that order, keying each one with
a file-static `FixedString` whose BSS symbol names the field:

| Key global | Address | Visited offset |
|---|---|---|
| `(anonymous namespace)::strSourceType` | `0x1089f36f0` | `+0x08` (u8) |
| `(anonymous namespace)::strProgressionSource` | `0x1089f36e0` | `+0x20` (Guid) |
| `(anonymous namespace)::strSourceId` | `0x1089f36e8` | `+0x10` (Guid) |
| `(anonymous namespace)::strOriginatorPrototype` | `0x1089f36f8` | `+0x00` (FixedString) |

So `+0x10` is `Source` (the game's own name is `SourceId`) and `+0x20` is
`ProgressionSource` — the **opposite** of the order the serializer writes them
in. Taking the pairing from the serializer alone would have swapped them.

## Resulting layout

| Offset | Type | Game name | Windows BG3SE name |
|---|---|---|---|
| `0x00` | FixedString | `m_Id.OriginatorPrototype` | `SpellId.OriginatorPrototype` |
| `0x04` | — | padding | — |
| `0x08` | u8 enum | `m_Id.SourceType` | `SpellId.SourceType` |
| `0x09` | — | padding | — |
| `0x10` | Guid | `m_Id.SourceId` | `SpellId.Source` |
| `0x20` | Guid | `m_Id.ProgressionSource` | `SpellId.ProgressionSource` |
| `0x30` | EntityHandle | `m_BoostSourceHandle` | `BoostHandle` |
| `0x38` | u8 enum | `m_LearningStrategy` | `LearningStrategy` |
| `0x39` | u8 enum | `m_PreparationStrategy` | `PrepareType` |
| `0x3a` | — | padding | — |
| `0x40` | Guid | `m_CastActionResource` | `PreferredCastingResource` |
| `0x50` | u8 enum | `m_CastAbility` | `SpellCastingAbility` |
| `0x51` | u8 enum | `m_CooldownType` | `CooldownType` |
| `0x52` | — | padding | — |
| `0x54` | FixedString | `m_ContainerId` | `ContainerSpell` |
| `0x58` | bool | `m_IsContainer` | `LinkedSpellContainer` |
| `0x59` | — | tail padding | — |
| **`0x60`** | | **sizeof** | |

Mirrored in `src/entity/spell_meta_layout.h`; pinned by
`tests/tier0/test_spell_meta_layout.c`.

## Enum value tables

### `EAbility` — verified, exposed as strings

7398727 ships parallel name/value tables for the khonsu scripting export:

```text
0x10878f9a0  s  ls::anubis::game::_khonsu::_enum::_Enum_Ability::NAMES
0x107887564  s  ls::anubis::game::_khonsu::_enum::_Enum_Ability::VALUES
```

`NAMES` is seven `{const char*, size_t}` string views; `VALUES` is seven
`int32`. Read out:

| Value | Name |
|---|---|
| 0 | `None` |
| 1 | `Strength` |
| 2 | `Dexterity` |
| 3 | `Constitution` |
| 4 | `Intelligence` |
| 5 | `Wisdom` |
| 6 | `Charisma` |

`ls::thoth::shared::_khonsu::_enum::_Enum_Ability::NAMES` (`0x1086f3390`)
carries the identical seven entries. This matches the `AbilityId` table already
registered in `src/enum/enum_definitions.c`.

### `eoc::ECooldownType` — verified, exposed as strings

```text
0x101f7d05c  t  eoc::StringToCooldownType(ls::_StringView<char>)
```

A length-switch followed by inline `memcmp`s; each accepted literal falls
through to a `mov w0, #<value>`, and any unmatched string returns 0.

| Value | Accepted literals |
|---|---|
| 0 | *(fallthrough — `Default`)* |
| 1 | `OncePerTurn` |
| 2 | `OncePerCombat` |
| 3 | `UntilRest` |
| 4 | `OncePerTurnNoRealtime` |
| 5 | `UntilShortRest` |
| 6 | `UntilRestPerItem`, `OncePerRestPerItem` |
| 7 | `UntilShortRestPerItem`, `OncePerShortRestPerItem` |

Note the divergence from Windows BG3SE, which names value 6
`UntilPerRestPerItem` — a spelling that appears nowhere in the 7398727 binary.
`SpellCooldownType` in `enum_definitions.c` registers the game's own names first
(so `enum_find_label` returns those) plus the Windows spelling as an alias, so
name→value lookups from either dialect resolve.

### `eoc::spell::EPreparationStrategy` — verified, exposed as strings

```text
0x101f7da90  t  eoc::spell::StringToPreparationStrategy(ls::_StringView<char>)
```

| Value | Literal |
|---|---|
| 0 | `AlwaysPrepared` |
| 1 | `RequiresPreparation` |
| 2 | *(fallthrough — `Unknown`)* |

Matches the Windows `SpellPrepareType` table.

### `eoc::spell::ESourceType` and `eoc::spell::ELearningStrategy` — NOT verified

Their offsets are verified (`+0x08` and `+0x38` above), but 7398727 ships no
name/value table for either: there is no `_Enum_SourceType` /
`_Enum_LearningStrategy` in the khonsu registry, no `StringTo…` parser, and the
`ls::model` registration for `game.spell.v0.ESourceType` /
`game.spell.v0.ELearningStrategy` carries no literal names. The Windows label
sets could not be justified against this build, so
`src/entity/component_property.c` exposes both as **raw ordinals** rather than a
guessed name, and `enum_definitions.c` deliberately does not register them —
`tests/tier0/test_spell_meta_layout.c` asserts they stay unregistered so a later
registration cannot silently start feeding unverified names to mods.

## Reproducing

```bash
BIN="$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"
lipo -thin arm64 "$BIN" -output /tmp/bg3_arm64
nm -arch arm64 "$BIN" | grep 'N3eoc5spell9SpellMetaE'
objdump -d --start-address=0x1019d9858 --stop-address=0x1019d9960 /tmp/bg3_arm64
objdump -d --start-address=0x1019d7a4c --stop-address=0x1019d7cc0 /tmp/bg3_arm64
objdump -d --start-address=0x101f7dc50 --stop-address=0x101f7de18 /tmp/bg3_arm64
objdump -d --start-address=0x101f7d05c --stop-address=0x101f7d2a0 /tmp/bg3_arm64
objdump -d --start-address=0x101f7da90 --stop-address=0x101f7db10 /tmp/bg3_arm64
```

Addresses are the binary's own preferred load addresses (no slide applied).
