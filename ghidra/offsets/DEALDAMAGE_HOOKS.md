# DealDamage event hook targets — 4.1.1.7398727

**Game version:** Baldur's Gate 3 v4.1.1.7398727 (`CFBundleVersion` of the
installed app bundle)

**Platform:** macOS ARM64

**Derived:** 2026-09-02

**Status:** nm-verified local symbols; full register/stack ABI verified by
disassembly of both the callee prologue and a complete call site

## Why this document exists

`DealDamage` / `BeforeDealDamage` used to be sourced from
`(anonymous namespace)::ProcessDealDamageFunctors`. That function does not
receive the payload Windows BG3SE publishes, so the Lua event was mostly nil and
any mod written against the documented API failed on its first field read
(`attempt to index a nil value (field 'Functor')`). Windows sources the three
damage events from two different functions:

| Windows event | Windows hook |
|---|---|
| `DealDamage`, `DealtDamage` | `stats::DealDamageFunctor::ApplyDamage` |
| `BeforeDealDamage` | `esv::StatsSystem::ThrowDamageEvent` |

Neither Windows name exists as a symbol in the macOS binary. Both are Larian
functions that Windows BG3SE names itself (it resolves them by byte signature,
not by symbol). This document establishes the macOS counterparts.

## Target 1 — `DealDamage` / `DealtDamage`

```text
0x105773558  t  esv::functor::StatsFunctorDealDamage::Execute(
                  eoc::StatsFunctorDealDamage const&,
                  ecs::EntityRef const&,
                  ecs::EntityRef const&,
                  Vector3f const&,
                  bool,
                  eoc::spell::SpellInfo const&,
                  ls::TypeWrap<unsigned int, eoc::TStoryActionIDClassname, true>,
                  eoc::ActionOriginator const&,
                  eoc::ClassDescriptions const&,
                  eoc::HitDesc const&,
                  eoc::AttackDesc const&,
                  ls::ID<ecs::EntityHandleTraits> const&,
                  eoc::EHitWith,
                  int,
                  bool,
                  ls::ID<ecs::EntityHandleTraits> const&,
                  eoc::spell::SpellId const&)
```

Offset in the table: `0x05773558`.

`esv::functor::StatsFunctorDealDamage` is a **namespace**, not a class — these
are free functions that take the functor explicitly, so there is no `this`.

### Identification

The seventeen explicit parameters match
`DealDamageFunctor__ApplyDamageProc` in
`BG3Extender/GameHooks/EngineHooksFwdDecl.h:63` one-for-one and in order.
The two parameters Windows could only type structurally resolve here:

| # | Windows | macOS |
|--:|---|---|
| 1 | `DealDamageFunctor* functor` | `eoc::StatsFunctorDealDamage const&` |
| 2 | `ecs::EntityRef* casterHandle` | `ecs::EntityRef const&` |
| 3 | `ecs::EntityRef* targetHandle` | `ecs::EntityRef const&` |
| 4 | `glm::vec3* position` | `Vector3f const&` |
| 5 | `bool isFromItem` | `bool` |
| 6 | `SpellIdWithPrototype* spellId` | `eoc::spell::SpellInfo const&` |
| 7 | `int storyActionId` | `ls::TypeWrap<unsigned int, TStoryActionIDClassname, true>` |
| 8 | `ActionOriginator* originator` | `eoc::ActionOriginator const&` |
| 9 | `GuidResourceBankBase* classResourceMgr` | `eoc::ClassDescriptions const&` |
| 10 | `HitDesc* hit` | `eoc::HitDesc const&` |
| 11 | `AttackDesc* attack` | `eoc::AttackDesc const&` |
| 12 | `EntityHandle* sourceHandle2` | `ls::ID<ecs::EntityHandleTraits> const&` |
| 13 | `HitWith hitWith` | `eoc::EHitWith` |
| 14 | `int conditionRollIndex` | `int` |
| 15 | `bool entityDamagedEventParam` | `bool` |
| 16 | `__int64 a17` | `ls::ID<ecs::EntityHandleTraits> const&` |
| 17 | `SpellId* spellId2` | `eoc::spell::SpellId const&` |

**Not** the sibling overload at `0x105777aa0`
(`..., Vector3f const&, float, bool, ...`, twelve parameters) — that is the
position/radius form with no target `EntityRef`.

### Machine ABI

The demangled name proves the source-level parameters, not the machine
contract. Both were read off this binary.

**Callee prologue @ `0x105773558`:**

```asm
105773580:  str  x7, [sp, #0xd8]
105773584:  mov  x28, x6
105773588:  mov  x25, x5
10577358c:  mov  x22, x4
105773590:  mov  x21, x3
105773594:  str  x2, [sp, #0xe8]
105773598:  mov  x26, x1
10577359c:  mov  x19, x0
1057735a0:  ldp  x23, x20, [x29, #0x20]
...
1057735b4:  mov  x1, x23
1057735b8:  bl   eoc::HitDesc::HitDesc(eoc::HitDesc const&)   ; x0 = destination
1057735bc:  add  x27, x0, #0x1a8
1057735c8:  bl   eoc::AttackDesc::AttackDesc(eoc::AttackDesc const&)
1057735cc:  add  x20, x19, #0x1c8
1057735d0:  str  wzr, [x19, #0x208]
```

`x0` is copy-constructed into as the returned `HitResult`, and `x8` is used as
scratch two instructions later, so **the indirect return object is passed in
`x0`, not `x8`** on this build — the same convention already documented for the
nine `ExecuteStatsFunctors` overloads in
`docs/bugs/wave2-functor-crash-analysis.md`. The two arguments loaded from
`[x29+0x20]` and `[x29+0x28]` are handed straight to the `HitDesc` and
`AttackDesc` copy constructors, which pins parameters 10 and 11 to stack slots
2 and 3.

**Complete call site @ `0x1049e5890`** (inside
`esv::FallingUnitTestHelper::DealDamage`, chosen because it sets every argument
from a literal displacement):

```asm
1049e5890:  ldp  x1, x3, [sp, #0x48]
1049e5908:  str  x23, [sp]            ; +0x00  p8  originator
1049e5904:  stp  x24, x25, [sp, #0x8] ; +0x08  p9  classDescriptions
                                      ; +0x10  p10 hit
1049e5900:  stp  x26, x8, [sp, #0x18] ; +0x18  p11 attack
                                      ; +0x20  p12 sourceHandle2
1049e58fc:  strb w9,  [sp, #0x28]     ; +0x28  p13 hitWith      (ONE byte)
1049e58f4:  str  wzr, [sp, #0x2c]     ; +0x2c  p14 conditionRollIndex
1049e58f0:  strb w9,  [sp, #0x30]     ; +0x30  p15 entityDamagedEvent
1049e58e8:  stp  x8, x9, [sp, #0x38]  ; +0x38  p16 sourceHandle3
                                      ; +0x40  p17 spellId2
1049e590c:  add  x4, sp, #0x90        ; p4 position
1049e5910:  add  x6, sp, #0xa8        ; p6 spellInfo
1049e5914:  mov  x0, x22              ; hidden HitResult out
1049e5918:  mov  x2, x20              ; p2 casterRef
1049e591c:  mov  x7, #0x0             ; p7 storyActionId
                                      ; x8 is NEVER set for this call
1049e5920:  bl   esv::functor::StatsFunctorDealDamage::Execute(...)
```

Resulting contract:

| Location | Parameter |
|---|---|
| `x0` | hidden `HitResult` output object |
| `x1` | p1 functor |
| `x2` | p2 casterRef |
| `x3` | p3 targetRef |
| `x4` | p4 position |
| `w5` | p5 isFromItem |
| `x6` | p6 spellId (`SpellInfo`) |
| `w7` | p7 storyActionId |
| stack `+0x00` | p8 originator |
| stack `+0x08` | p9 classDescriptions |
| stack `+0x10` | p10 hit |
| stack `+0x18` | p11 attack |
| stack `+0x20` | p12 sourceHandle2 |
| stack `+0x28` | p13 hitWith (`uint8`) |
| stack `+0x2c` | p14 conditionRollIndex (`int32`) |
| stack `+0x30` | p15 entityDamagedEvent (`bool`) |
| stack `+0x38` | p16 sourceHandle3 |
| stack `+0x40` | p17 spellId2 |

Two hazards, both silent:

1. **`result_out` in `x0`.** Omitting it shifts every explicit parameter down a
   register — the exact 2026-07-29 SIGSEGV. Accept it first, forward it
   unchanged, never dereference it, and return what the original returned
   (AAPCS leaves the result address in `x0`).
2. **Stack packing.** Apple's arm64 ABI packs stack arguments to natural size
   instead of 8-byte slots. `hitWith` really is one byte at `+0x28`
   (`strb`, not `str`); widening it to `int` pushes `conditionRollIndex` to
   `+0x30` and drags the last four arguments — two of them pointers — out of
   place. `DealDamageStackArgsLayout` in `src/stats/functor_types.h` mirrors
   this tail and `tests/tier0/test_deal_damage_abi.c` pins every offset.

### Bonus: `HitResult` layout

The prologue above, and independently `ExecuteStatsFunctor @ 0x10577e650`
(`add x28, x0, #0x1a8`, `add x22, x26, #0x1c8`, `str w8, [x26, #0x208]`), give:

| Offset | Field |
|---:|---|
| `0x000` | `HitDesc Hit` (size `0x1a8`) |
| `0x1a8` | `AttackDesc Attack` (size `0x20`) |
| `0x1c8` | results block, `0x40` bytes, zero-initialized |
| `0x208` | `uint32 NumConditionRolls` |

Total `0x20c`, comfortably under `FUNCTOR_RESULT_BUFSZ` (`0x400`). Note the
`HitResult` comment block in `functor_types.h` still says `0x1B0`/`0x1D0`/`0x1D8`
from the older vintage; the struct is only ever used as an opaque size bound, so
it is left alone, but the values above are the measured ones for 7398727.

## Target 2 — `BeforeDealDamage`

```text
0x1057c3aa0  t  esv::StatsSystem::ApplyDamage(
                  ecs::EntityRefView<eoc::ResistancesComponent const,
                                     eoc::TagComponent const,
                                     eoc::BoostsContainerComponent,
                                     eoc::DataComponent,
                                     eoc::HealthComponent> const&,
                  eoc::HitDesc&,
                  eoc::AttackDesc&,
                  EAbility,
                  bool)
```

Offset in the table: `0x057c3aa0`.

### Identification

Windows resolves `esv::StatsSystem::ThrowDamageEvent` by a byte signature whose
comment reads *"Sig: call from DealDamageFunctor::ApplyDamage, near ref to
PassiveSystemID"* (`BG3Extender/GameHooks/BinaryMappings.xml:431`). Both halves
of that description reproduce here:

```asm
; inside StatsFunctorDealDamage::Execute
105774bfc:  ldrb w5, [x29, #0x40]     ; p15 entityDamagedEvent (stack +0x30)
105774c00:  add  x1, sp, #0xb00       ; EntityRefView
105774c04:  add  x2, sp, #0x140       ; HitDesc
105774c08:  ldr  x0, [sp, #0x90]      ; StatsSystem*
105774c0c:  mov  x3, x28              ; AttackDesc
105774c10:  ldr  w4, [sp, #0x7c]      ; EAbility
105774c14:  bl   esv::StatsSystem::ApplyDamage(...)
105774c18:  ldrb w8, [sp, #0x146]     ; HitDesc+6 (CauseType)
105774c1c:  cmp  w8, #0xa
...
105774c3c:  ldr  x9, ... ls::TypeId<esv::PassiveSystem, ecs::SystemsContext>::m_TypeIndex
```

* It is called from inside the `DealDamage` target established above.
* The instruction stream immediately after the call loads
  `ls::TypeId<esv::PassiveSystem, ...>::m_TypeIndex` — Windows' "near ref to
  PassiveSystemID" anchor.
* Windows' own signature bytes for this site include
  `cmp byte ptr [rbp+…var_B10+6], 0Ah`; the macOS site does the identical
  `ldrb`/`cmp #0xa` against `HitDesc+6`.
* The bool Windows passes on the stack (`mov byte ptr [rsp+…], al`) is p15
  `entityDamagedEvent` of the enclosing `ApplyDamage`, loaded here from
  `[x29+0x40]` = stack `+0x30`, matching the layout table above.

The argument shape matches Windows'
`(StatsSystem*, void* temp5, HitDesc*, AttackDesc*, bool, bool)` — the first of
Windows' two trailing bools is really `EAbility`, a one-byte enum.

### Machine ABI

All six arguments are register-passed, so there is no stack-packing hazard.
Confirmed by the prologue:

```asm
1057c3aec:  mov  x21, x3    ; AttackDesc&
1057c3af0:  mov  x22, x2    ; HitDesc&
1057c3af4:  mov  x23, x1    ; EntityRefView const&
1057c3af8:  mov  x24, x0    ; StatsSystem* this
1057c3afc:  stp  w4, w5, [sp, #0x10]   ; EAbility, bool
...
1057c3b40:  ldr  w9, [x22]  ; HitDesc+0 = TotalDamageDone
1057c3b44:  subs w25, w8, w9 ; subtracted from HealthComponent current HP
```

`x0..x3` pointers, `w4` `EAbility`, `w5` `bool`. There is no hidden result
object: the function returns `void` and `x0` is `this`.

## Version gating

Both offsets exist only in the `4.1.1.7398727` row of
`src/core/offset_table.c`. The `6995620` and `7209685` rows leave them at `0`
**deliberately** — neither address was derived on its own binary, and a borrowed
address does not fail safely: it resolves to a different live function and Dobby
patches it. With `0`, `offset_table_game_fn()` returns `NULL`, the hook is not
installed, and the error is logged. On top of that, `functor_hooks_init()` is
only reached at all when the detected version equals
`FUNCTOR_ADDRS_VERIFIED_BUILD` (`src/injector/main.c`).

## Reproducing

```sh
BIN="$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"

# Symbols (LOCAL symbols — plain nm, no -g)
nm -arch arm64 -n "$BIN" | c++filt | grep 'StatsFunctorDealDamage::Execute'
nm -arch arm64 -n "$BIN" | c++filt | grep 'StatsSystem::ApplyDamage'

# Independent re-derivation through the manifest resolver
python3 tools/port_offsets.py resolve --emit | grep -E 'DEAL_DAMAGE_APPLY|THROW_DAMAGE'
python3 tools/port_offsets.py verify
```

`tools/offset_manifest.json` carries both exact demangled symbols, so
`port_offsets.py` re-derives `0x05773558` and `0x057c3aa0` from `nm` alone —
an independent confirmation of the hand-derived addresses above.

Disassembly for the ABI evidence was produced with
`objdump -d --no-show-raw-insn "$BIN"` (llvm-objdump; the `--macho`
`--start-address` path silently ignores the range and dumps from the start).
