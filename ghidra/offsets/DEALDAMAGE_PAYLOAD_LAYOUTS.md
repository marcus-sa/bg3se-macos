# DealDamage payload member layouts — 4.1.1.7398727

**Game version:** Baldur's Gate 3 v4.1.1.7398727 (`CFBundleVersion` of the
installed app bundle)

**Platform:** macOS ARM64

**Derived:** 2026-09-02

**Status:** every offset, width and enum label below was read out of the
shipped arm64 slice with `nm` + `objdump`. Nothing here is taken from the
Windows headers; where a Windows name is quoted it is only to say which
documented API field the member corresponds to.

## Why this document exists

[`DEALDAMAGE_HOOKS.md`](DEALDAMAGE_HOOKS.md) established *where the hook
arguments come from* — the two hook addresses and their complete arm64 ABI.
It deliberately stopped there: the argument **addresses** were verified, but no
member layout was, so `src/lua/lua_events.c` published each object-valued field
as a table carrying only `Ptr`.

That left the events unusable for their actual consumer. MetamagicExtended's
`HandleDealDamage` reads `e.Functor.DamageType` on its second line and returns
early when it is nil, so the mod's entire transmuted-damage feature was a
silent no-op. This document is the missing half: what lives *inside* those
objects.

Consumed by `src/stats/deal_damage_layout.h` (offset constants + mirror
structs) and pinned by `tests/tier0/test_deal_damage_payload.c`.

## Method note

Members are read out of functions that key an offset to something independently
identifying — a named symbol, a named `FixedString`, a typed callee parameter —
never out of a struct definition. Two traps this build actually sets:

* **Declaration order is not offset order.** `eoc::HitDesc`'s three
  `EntityHandle`s are declared *after* three one-byte enums but live *before*
  them (0x30/0x38/0x40 versus 0x4c/0x4d/0x4e). Reading the serializer's member
  list as a layout puts all six in the wrong place, and every one of them still
  decodes to a plausible-looking value.
* **Serializers do not cover everything.** `HitDesc`'s `DamageList` is not
  network-serialized, so it is absent from the `ecs::sync::Deserialize` member
  list entirely. It is recovered from a different function that copies it.

## 1. `eoc::StatsFunctorDealDamage` — hook parameter 1

### Size

```text
0x101fb14b4  t  eoc::StatsFunctorDealDamage::Clone() const
```

```asm
101fb1518:  mov  w0, #0x68           ; =104  -> operator new
101fb1550:  bl   eoc::StatsFunctorBase::StatsFunctorBase(eoc::StatsFunctorBase const&)
101fb1560:  ldur w8, [x19, #0x47]    ; four bytes at 0x47 — the four one-byte enums
101fb1564:  stur w8, [x20, #0x47]
101fb1568:  ldr  x8, [x19, #0x50]    ; StatsExpressionRef, refcounted below
101fb1570:  cbz  x8, ...
101fb1574:  add  x8, x8, #0x20
101fb157c:  ldadd w9, w8, [x8]
101fb1580:  ldr  x8, [x19, #0x58]    ; CoinMultiplier + four bools
101fb1588:  ldrh w8, [x19, #0x60]    ; two more bools
```

`sizeof == 0x68`. The derived part of the object is `0x47..0x61`.

### `DamageType` — `+0x47`, one byte, `EDamageType`

```text
0x101fb0d14  t  eoc::StatsFunctorDealDamage::Parse(
                  ls::Span<ls::_StringView<char>>&, ls::STDString&,
                  ls::EnumFlags<eoc::EStatsFunctorContext>)
```

```asm
101fb0f88:  bl   StringToDamageType(ls::_StringView<char> const&)
101fb0f8c:  strb w0, [x19, #0x47]
```

The store is the instruction immediately after the call, so the byte at `+0x47`
is by construction whatever `StringToDamageType` returned.

Corroborated by a second, unrelated function:

```text
0x105771fb4  t  (anonymous namespace)::GetDamageRollAndDamageTypeInternal(
                  eoc::StatsFunctorDealDamage const&, ..., EDamageType&, bool&)
```

```asm
105772d68:  ldrb w0, [x19, #0x47]    ; x19 = the functor (parameter 1)
105772d74:  bl   (anonymous namespace)::MergeDamage(EDamageType, ...)
```

`w0` is `MergeDamage`'s leading `EDamageType` parameter.

### The three siblings at `+0x48..+0x4a` — read, not exposed

`Parse` also does `strb w28, [x19, #0x48]`, `strb w0, [x19, #0x49]` (after
`eoc::StringToDealDamageDamageRollValue`) and `strb w0, [x19, #0x4a]` (after
`eoc::StringToDealDamageDamageTypeValue`). The offsets are certain; the label
sets for those two enums were not derived on this build, and publishing a raw
ordinal under a field name the API documents as a string reads like a decode.
Left unexposed.

## 2. `EDamageType` label table

Two independent tables on this build agree on all fourteen values.

```text
0x10250eb68  t  Noesis::TypeEnumCreator<EDamageType>::Fill(Noesis::Type*)
```

A straight run of `SymbolManager::AddStaticString(<literal>)` →
`Noesis::TypeEnum::AddValue(Symbol, uint64 <ordinal>)`:

| Value | Name | Value | Name |
|---|---|---|---|
| 0 | `None` | 7 | `Fire` |
| 1 | `Slashing` | 8 | `Lightning` |
| 2 | `Piercing` | 9 | `Cold` |
| 3 | `Bludgeoning` | 10 | `Psychic` |
| 4 | `Acid` | 11 | `Poison` |
| 5 | `Thunder` | 12 | `Radiant` |
| 6 | `Necrotic` | 13 | `Force` |

The same fill continues with `15 Weapon`, `16 Spell`, `17 HealAmount` — stats
expression pseudo-types, not damage types a hit can carry. Not registered.
Value 14 (Windows: `Sentinel`) has no entry.

```text
0x108793fb8  s  ls::anubis::game::_khonsu::_enum::_Enum_DamageType::NAMES
0x107889a58  s  ls::anubis::game::_khonsu::_enum::_Enum_DamageType::VALUES
0x107889a3d  s  ls::anubis::game::_khonsu::_enum::_Enum_DamageType::FQDN
```

`NAMES` is fourteen `{const char*, size_t}` string views
(`0x108793fb8..0x108794098`), `VALUES` fourteen `int32`
(`0x107889a58..0x107889a90`), `FQDN` reads `ls.anubis.game.DamageType`. Same
fourteen entries, same 0..13, upper-cased (`NONE`, `SLASHING`, …) because that
table is the scripting export.

The mixed-case Noesis spelling is what `enum_definitions.c` registers: it is a
name this build ships *and* the one mods compare against
(`damageType == "Fire"`).

## 3. `eoc::spell::SpellId` — hook parameter 17

### It is `eoc::spell::MetaId` plus one `FixedString`

```text
0x101f7e248  t  eoc::spell::LEGACY_Visit(ls::SavegameVisitor*,
                                         eoc::spell::SpellId&)
```

The visitor keys each visited offset with a file-static `FixedString` whose BSS
symbol names the field:

| Key global | Address | Visited offset | Type |
|---|---|---:|---|
| `(anonymous namespace)::strSourceType` | `0x1089f36f0` | `+0x08` | u8 |
| `(anonymous namespace)::strProgressionSource` | `0x1089f36e0` | `+0x20` | Guid |
| `(anonymous namespace)::strSourceId` | `0x1089f36e8` | `+0x10` | Guid |
| `(anonymous namespace)::strOriginatorPrototype` | `0x1089f36f8` | `+0x00` | FixedString |
| `(anonymous namespace)::strPrototype` | `0x1089f3700` | `+0x30` | FixedString |

```asm
101f7e3e4:  adrp x8, 0x1089f3000
101f7e3e8:  ldr  x21, [x8, #0x700]   ; (anonymous namespace)::strPrototype
101f7e3ec:  add  x20, x20, #0x30     ; &SpellId + 0x30
101f7e404:  ldr  w8, [x20]           ; 4-byte FixedString
```

The first four rows are the `eoc::spell::MetaId` layout already documented in
[`SPELL_META_LAYOUT.md`](SPELL_META_LAYOUT.md) and derived there from a
completely different function (`ecs::sync::Serialize<eoc::spell::SpellMeta,…>`),
which is a useful cross-check. `SpellId` adds `Prototype` at `+0x30`;
`sizeof == 0x38` with tail padding.

## 4. `eoc::spell::SpellInfo` — hook parameter 6 (Windows `SpellIdWithPrototype`)

### `SpellId` is embedded at offset 0

```text
0x105773558  t  esv::functor::StatsFunctorDealDamage::Execute(...)
```

```asm
1057736b8:  ldr  x1, [sp, #0xc0]     ; the SpellInfo& argument, unadjusted
1057736bc:  bl   eoc::spell::GetSpellBoostHandle(
                     eoc::spell::ContainerComponent const*,
                     eoc::spell::MetaId const&)
```

Passing a `SpellInfo*` verbatim where the callee's declared parameter type is
`eoc::spell::MetaId const&` is only correct if the derivation offset is zero.

The same function also validates `Prototype` through the offset above:

```asm
105773694:  ldr  w8, [x28, #0x30]    ; x28 = the SpellInfo argument
105773698:  cmn  w8, #0x1            ; == FixedString null index?
1057736a4:  ldrb w8, [x28, #0x8]     ; SourceType
1057736ac:  cmp  w8, #0x17
```

### `SpellProto` — `+0x38`, `eoc::SpellPrototype*`

```text
0x101f331ec  t  eoc::spell::GetSpellRanges(..., eoc::spell::SpellInfo const&, ...)
```

This function returns a struct, so its explicit parameters start at `x1` and
the `SpellInfo const&` is `x4`:

```asm
101f33230:  ldr  x8, [x4, #0x38]     ; SpellInfo + 0x38
101f33238:  stp  x8, x22, [sp, #0x20]; -> eoc::spell::RangeContext + 0x00
101f33494:  bl   eoc::spell::GetSpellRanges(eoc::spell::RangeContext const&)
```

and the value comes straight back out as a `SpellPrototype`:

```text
0x101f328b0  t  eoc::spell::GetSpellRanges(eoc::spell::RangeContext const&)
```

```asm
101f328e8:  ldr  x21, [x20]          ; RangeContext + 0x00
101f32928:  mov  x0, x21
101f3292c:  bl   eoc::SpellPrototype::GetTargetRadius(...)   ; x0 = the object
```

### `+0x40..+0x57` — bounded, not decoded

```text
0x105782434  t  eoc::spell::SpellInfo::operator=(eoc::spell::SpellInfo const&)
```

copies `0x00` (FixedString, gst-refcounted), `0x08..0x27` (two `q` moves),
`0x28`, `0x30` (FixedString, gst-refcounted), then `0x38..0x47` and
`0x48..0x57` as two more `q` moves, and touches nothing beyond — so
`sizeof == 0x58`. Windows documents the remainder as
`std::optional<Guid> SpellCastSource`; 32 bytes minus the 8-byte prototype
pointer is consistent with that, but nothing here says which half is the Guid
and which the engaged flag. Not exposed.

## 5. `eoc::SpellPrototype` — head only

```text
0x101f2fd2c  t  eoc::spell::GetModifiedSpellFlags(
                  eoc::SpellPrototype const&,
                  eoc::spell::ModificationContainerComponent const*)
```

```asm
101f2fd4c:  ldr  w0, [x19, #0x8]     ; FixedString SpellId -> hash
101f2fd50:  bl   ls::gst hash
101f2fd60:  ldr  x9, [x20]           ; compared against the container's keys
...
101f2fdb4:  ldr  x2, [x19, #0x10]    ; base flags, passed to the modifier applier
101f2fdcc:  ldr  x0, [x19, #0x10]    ; no container -> return the flags verbatim
101f2fdd8:  ret
```

and from `GetSpellRanges(RangeContext const&)`:

```asm
101f328ec:  ldr  w19, [x21, #0x4]    ; 1..11 jump table over spell kinds
101f32960:  ldr  w9, [x24]           ; StatsObjectIndex
101f32964:  tbnz w9, #0x1f, ...      ; negative -> no object
101f32968:  ldr  w10, [x8, #0x118]   ; RPGStats object count
101f32974:  ldr  x10, [x8, #0xc8]    ; RPGStats object array
101f32978:  ldr  x25, [x10, x9, lsl #3]
```

| Offset | Type | Field |
|---:|---|---|
| `0x00` | int32 | `StatsObjectIndex` |
| `0x04` | uint32 | `SpellTypeId` (no derived label table — exposed as a number) |
| `0x08` | FixedString | `SpellId` (the prototype's stats name) |
| `0x10` | uint64 | `SpellFlags` (`ls::EnumFlags<eoc::ESpellFlags>`) |

`0x0c` is where Windows puts `uint8 SpellSchool`; not derived here, not exposed.
Nothing past `0x18` was looked at — this is a head, not a layout.

## 6. `eoc::ESpellFlags` label table

```text
0x1086f4a60  s  ls::thoth::shared::_khonsu::_enum::_Enum_SpellFlags::NAMES
0x10787d870  s  ls::thoth::shared::_khonsu::_enum::_Enum_SpellFlags::VALUES
0x10787d852  s  ls::thoth::shared::_khonsu::_enum::_Enum_SpellFlags::FQDN
```

`NAMES` spans `0x1086f4a60..0x1086f4e00` = 58 `{const char*, size_t}` views;
`VALUES` spans `0x10787d870..0x10787da40` = 58 **uint64** (a flag enum, so the
values are 8 bytes, not 4 — the span/count arithmetic is the check).
`FQDN` reads `ls.thoth.shared.SpellFlags`.

Registered verbatim in `enum_definitions.c`. Fourteen names differ from the
Windows BG3SE spelling; the game's own name is registered first (so
`enum_find_label` returns it) with the Windows spelling as an alias:

| Value | Game name | Windows name |
|---|---|---|
| `0x1` | `Verbal` | `HasVerbalComponent` |
| `0x2` | `Somatic` | `HasSomaticComponent` |
| `0x4` | `Jump` | `IsJump` |
| `0x8` | `Attack` | `IsAttack` |
| `0x10` | `Melee` | `IsMelee` |
| `0x20` | `HighGroundRangeExtension` | `HasHighGroundRangeExtension` |
| `0x40` | `Concentration` | `IsConcentration` |
| `0x80` | `FallDamage` | `AddFallDamageOnLand` |
| `0x400` | `Spell` | `IsSpell` |
| `0x800` | `UNUSED_A` | `CombatLogSetSingleLineRoll` |
| `0x1000` | `EnemySpell` | `IsEnemySpell` |
| `0x4000` | `CannotTargetItem` | `CannotTargetItems` |
| `0x2000000` | `Harmful` | `IsHarmful` |
| `0x4000000` | `Trap` | `IsTrap` |
| `0x8000000` | `DefaultWeaponAction` | `IsDefaultWeaponAction` |
| `0x10000000` | `UNUSED_B` | `CallAlliesSpell` |
| `0x2000000000` | `WildShape` | `Wildshape` |
| `0x10000000000` | `UNUSED_F` | `TrajectoryRules` |

Bit 43 (`0x80000000000`, Windows `RangeIgnoreBlindness`) has **no entry** in
this build's table and is registered under neither spelling; a hit that sets it
contributes no label rather than a fabricated one.

## 7. `eoc::HitDesc` — hook parameter 10 / `BeforeDealDamage` parameter 2

```text
0x1019c24d0  t  void ecs::sync::Deserialize<eoc::HitDesc,
                  ls::FieldMeta<int eoc::HitDesc::*, &eoc::HitDesc::m_TotalDamageDone>,
                  ls::FieldMeta<EDeathType …::m_DeathType>,
                  ls::FieldMeta<EDamageType …::m_MainDamageType>,
                  ls::FieldMeta<TCauseType …::m_CauseType>,
                  ls::FieldMeta<Vector3f …::m_ImpactPosition>,
                  ls::FieldMeta<Vector3f …::m_ImpactDirection>,
                  ls::FieldMeta<float …::m_ImpactForce>,
                  ls::FieldMeta<int …::m_ArmorAbsorption>,
                  ls::FieldMeta<int …::m_LifeSteal>,
                  ls::FieldMeta<ls::EnumFlags<eoc::EDamageEffectFlag> …::m_EffectFlags>,
                  ls::FieldMeta<eoc::EHitWith …::m_HitWith>,
                  ls::FieldMeta<EAbility …::m_AttackAbility>,
                  ls::FieldMeta<EAbility …::m_SaveAbility>,
                  ls::FieldMeta<ls::ID<ecs::EntityHandleTraits> …::m_InflicterObject>,
                  ls::FieldMeta<… …::m_InflicterOwnerObject>,
                  ls::FieldMeta<… …::m_ThrownObject>,
                  ls::FieldMeta<ESpellAttackType …::m_LastSpellAttackType>,
                  ls::FieldMeta<ls::FixedString …::m_SpellId>,
                  ls::FieldMeta<ESpellSchool …::m_SpellSchool>,
                  ls::FieldMeta<int …::m_SpellLevel>,
                  ls::FieldMeta<int …::m_SpellPowerLevel>,
                  ls::FieldMeta<int …::m_TotalHealDone>,
                  ls::FieldMeta<ls::EnumFlags<eoc::EHitDescFlag> …::m_Flags>>
                (ecs::sync::NetworkReader&, eoc::HitDesc&, ls::TypeList<…>)
```

The mangled name gives all 23 members in declaration order with their real
names and types. The body then reads each one into a literal displacement off
the object (`x19`) with a literal byte count — `add x0, x19, #<off>` followed by
`mov w2, #<len>` and the reader call, except the three `EntityHandle`s, which
go through an 8-byte stack temp and a gst remap before landing in a
`str x8, [x19, #<off>]`.

| Member (declaration order) | Offset | Bytes |
|---|---:|---:|
| `m_TotalDamageDone` | `0x000` | 4 |
| `m_DeathType` | `0x004` | 1 |
| `m_MainDamageType` | `0x005` | 1 |
| `m_CauseType` | `0x006` | 1 |
| `m_ImpactPosition` | `0x008` | 12 |
| `m_ImpactDirection` | `0x014` | 12 |
| `m_ImpactForce` | `0x020` | 4 |
| `m_ArmorAbsorption` | `0x024` | 4 |
| `m_LifeSteal` | `0x028` | 4 |
| `m_EffectFlags` | `0x02c` | 4 |
| `m_HitWith` | `0x04c` | 1 |
| `m_AttackAbility` | `0x04d` | 1 |
| `m_SaveAbility` | `0x04e` | 1 |
| `m_InflicterObject` | `0x030` | 8 |
| `m_InflicterOwnerObject` | `0x038` | 8 |
| `m_ThrownObject` | `0x040` | 8 |
| `m_LastSpellAttackType` | `0x04f` | 1 |
| `m_SpellId` | `0x148` | 4 |
| `m_SpellSchool` | `0x159` | 1 |
| `m_SpellLevel` | `0x15c` | 4 |
| `m_SpellPowerLevel` | `0x160` | 4 |
| `m_TotalHealDone` | `0x164` | 4 |
| `m_Flags` | `0x15b` | 1 |

Note rows 11–16 and the last row: **declaration order is not offset order.**
The handles at `0x30/0x38/0x40` are declared after the enums at `0x4c..0x4e`,
and `m_Flags` at `0x15b` is declared after `m_TotalHealDone` at `0x164`.

Key instructions for the three handles and the FixedString:

```asm
1019c2870:  str  x8, [x19, #0x30]    ; m_InflicterObject
1019c2914:  str  x8, [x19, #0x38]    ; m_InflicterOwnerObject
1019c29b8:  str  x8, [x19, #0x40]    ; m_ThrownObject
1019c29f4:  add  x1, x19, #0x148
1019c29fc:  bl   ecs::sync::Deserialize(ecs::sync::NetworkReader&, ls::FixedString&)
```

### The `DamageList` at `0x198`, and `sizeof == 0x1a8`

Not serialized, so it is absent from the member list above. Recovered from:

```text
0x10117860c  t  eoc::AttackDesc::AttackDesc(eoc::HitDesc const&, ...)
```

```asm
101178634:  ldr  w8, [x1]            ; HitDesc + 0x000 = TotalDamageDone
101178638:  ldr  w9, [x1, #0x164]    ; HitDesc + 0x164 = TotalHealDone
10117863c:  stp  w8, w9, [x0]        ; -> AttackDesc + 0x00 / + 0x04
101178644:  ldr  x22, [x1, #0x198]   ; HitDesc DamageList elements
10117864c:  ldr  w8, [x1, #0x1a4]    ; HitDesc DamageList size
```

The `0x164` read independently confirms `m_TotalHealDone`'s offset from a
different function. Capacity sits at `0x1a0` (the array is
elements/capacity/size like every other `ls::DynamicArray` here), which closes
the struct at `0x1a8` — matching the size already measured in
`DEALDAMAGE_HOOKS.md` from the `HitResult` layout
(`add x27, x0, #0x1a8` after the copy constructor).

### What is exposed to Lua, and what is not

Exposed: the six `int32`s, `ImpactForce`, both `Vector3f`s, the three entity
handles (as entity objects), `m_SpellId`, `m_MainDamageType` as a `DamageType`
string, `m_AttackAbility` / `m_SaveAbility` as `AbilityId` strings, and the
`DamageList`.

**Not** exposed: `m_DeathType`, `m_CauseType`, `m_HitWith`,
`m_LastSpellAttackType`, `m_EffectFlags`, `m_Flags`. Their offsets are certain;
their labels are not. This build ships only the upper-cased khonsu scripting
spellings for those enums —
`_Enum_DeathType::NAMES` (`0x1087942c8`) reads `NONE`, `ACID`, `CHASM`, `DOT`,
… and `_Enum_CauseType::NAMES` (`0x108794128`) reads `NONE`, `SURFACE_MOVE`,
… — which is not what any mod compares against, and there is no
`Noesis::TypeEnumCreator` fill for them to supply the mixed-case names the way
there is for `EDamageType`, `ESpellSchool` and `EAbility`. Publishing the
Windows spelling would be exactly the failure mode this repo has shipped three
times. `tests/tier0/test_deal_damage_payload.c` asserts these enum types stay
unregistered, so deriving them later forces the decoder to be updated rather
than letting the registry gain names nothing reads.

## 8. `eoc::AttackDesc` — hook parameter 11 / `BeforeDealDamage` parameter 3

```text
0x101178738  t  eoc::AttackDesc::operator=(eoc::AttackDesc const&)
```

```asm
10117874c:  ldr  d0, [x1]            ; 0x00..0x07  (two int32s)
101178750:  str  d0, [x0]
101178754:  ldrh w8, [x1, #0x8]      ; 0x08..0x09  (two bytes)
101178758:  strh w8, [x0, #0x8]
101178760:  ldr  x21, [x1, #0x10]    ; DamageList elements
101178764:  ldr  w8, [x1, #0x1c]     ; DamageList size
1011787b4:  ldp  w9, w8, [x19, #0x18]; capacity then size
```

| Offset | Type | Field |
|---:|---|---|
| `0x00` | int32 | `TotalDamageDone` |
| `0x04` | int32 | `TotalHealDone` |
| `0x08` | uint8 | Windows: `InitialHPPercentage` — not derived, not exposed |
| `0x09` | uint8 | not derived, not exposed |
| `0x10` | `TDamagePair*` | `DamageList` elements |
| `0x18` | int32 | `DamageList` capacity |
| `0x1c` | int32 | `DamageList` size |

`sizeof == 0x20`, matching `DEALDAMAGE_HOOKS.md`'s independent measurement of
the `HitResult` layout (`AttackDesc` spans `0x1a8..0x1c8`).

## 9. `TDamagePair`

```text
0x1011787e8  t  eoc::AttackDesc::AddDamage(DamageList const&)
```

```asm
101178824:  ldr  x10, [x21], #0x8    ; stride 8
101178894:  ldr  w12, [x10]          ; +0x00 int32 amount
1011788a4:  cmp  w12, #0x0           ; zero-amount entries are dropped
1011788b0:  add  x11, x10, #0x4
1011788cc:  ldrb w13, [x11]          ; +0x04 one-byte discriminator
1011788d4:  ldrb w15, [x15, #0x4]    ; ... compared against another element's
1011788d8:  cmp  w13, w15
1011788ec:  ldr  w15, [x10]          ; equal -> the int32 amounts are summed
1011788f0:  add  w14, w15, w14
```

| Offset | Type | Field |
|---:|---|---|
| `0x00` | int32 | `Amount` |
| `0x04` | uint8 | `DamageType` (`EDamageType`) |

`sizeof == 8`.

The decoder clamps the element count it reads out of live memory to
`DAMAGE_LIST_MAX_ELEMENTS` (64). The count comes out of an engine array while
the game thread is inside the hook; a torn or stale read must cost a bounded
number of `safe_memory_read` calls, not a stall on the game thread inside a Lua
callback.

## Reproducing

```sh
BIN="$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"

# The shipped binary is a fat Mach-O; thin it first or every file offset below
# is wrong by the arm64 slice base (0xf5c4000 on this build).
lipo -thin arm64 "$BIN" -output /tmp/bg3.arm64

nm -arch arm64 -n "$BIN" | c++filt > /tmp/nm.txt
grep -E 'StatsFunctorDealDamage::(Parse|Clone)' /tmp/nm.txt
grep -E 'ecs::sync::Deserialize<eoc::HitDesc' /tmp/nm.txt
grep -E 'eoc::AttackDesc::(AttackDesc|operator=|AddDamage)' /tmp/nm.txt
grep -E 'spell::LEGACY_Visit|spell::SpellInfo::operator=|GetModifiedSpellFlags' /tmp/nm.txt
grep -E '_Enum_(DamageType|SpellFlags)::(NAMES|VALUES|FQDN)' /tmp/nm.txt
grep -E 'TypeEnumCreator<EDamageType>::Fill' /tmp/nm.txt

# Disassembly (llvm-objdump; the --macho --start-address path silently ignores
# the range and dumps from the start)
objdump -d --no-show-raw-insn --start-address=0x101fb0d14 --stop-address=0x101fb14b4 /tmp/bg3.arm64
objdump -d --no-show-raw-insn --start-address=0x1019c24d0 --stop-address=0x1019c2b4c /tmp/bg3.arm64
objdump -d --no-show-raw-insn --start-address=0x10117860c --stop-address=0x101178950 /tmp/bg3.arm64
objdump -d --no-show-raw-insn --start-address=0x101f7e248 --stop-address=0x101f7e440 /tmp/bg3.arm64
objdump -d --no-show-raw-insn --start-address=0x101f2fd2c --stop-address=0x101f2fde4 /tmp/bg3.arm64
objdump -d --no-show-raw-insn --start-address=0x10250eb68 --stop-address=0x10250ed5c /tmp/bg3.arm64
```

The `_Enum_*` `NAMES`/`VALUES` tables live in `__DATA_CONST,__const` and
`__TEXT,__const`; convert a VA to a file offset with the section's
`addr`/`offset` pair from `otool -l`, then read `NAMES` as
`{const char*, size_t}` pairs and `VALUES` as `int32` (plain enums) or `uint64`
(flag enums — the entry size follows from the table span divided by the name
count).

## Consumers

* `src/stats/deal_damage_layout.h` — the offset constants and the mirror
  structs the tests pin them against.
* `src/lua/lua_events.c` — `push_deal_damage_payload`,
  `events_fire_before_deal_damage`, and the `set_*_field` decoders.
* `src/enum/enum_definitions.c` — `DamageType` (verification comment) and
  `SpellFlags` (new).
* `tests/tier0/test_deal_damage_payload.c` — offsets, field widths, struct
  sizes and enum labels.
