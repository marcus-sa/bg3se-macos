# `eoc::spell::AddedSpellsComponent` element type — 4.1.1.7398727

**Game version:** Baldur's Gate 3 v4.1.1.7398727

**Platform:** macOS ARM64

**Derived:** 2026-09-02

**Status:** element type, array offset and struct size read out of the shipped
arm64 slice. The element *layout* is reused from
[SPELL_META_LAYOUT.md](SPELL_META_LAYOUT.md), which was derived the same way.

## Why this document exists

`src/entity/component_offsets.h` declared

```c
{ "Spells", 0x00, FIELD_TYPE_DYNAMIC_ARRAY, 0, true },
```

`elemType 0` is `ELEM_TYPE_UNKNOWN`, which routes to the generic stub branch in
`component_property.c`: every element comes back as a table carrying only
`__ptr` / `__index` / `__size`, so `AddedSpells.Spells[i].SpellId` is nil.

Windows BG3SE declares the member as `Array<SpellMeta>` — the same type as
`SpellContainerComponent.Spells`. That is a *plausible* answer, and this repo
has already shipped two bugs from plausible-but-unverified layouts, so the
element type was confirmed against this build before wiring it up.

## The array is `DynamicArray<eoc::spell::SpellMeta>` at offset 0

The ECS keeps a per-component-type destructor thunk. For this component it is
one instruction:

```text
0x101f6f9d8  T  ecs::_private::GetComponentDestructor<eoc::spell::AddedSpellsComponent>()
                  ::'lambda'(void*)::__invoke(void*)
```

```asm
101f6f9d8: 17e9a563    b  0x1019d8f64
                       ; ls::DynamicArray<eoc::spell::SpellMeta,
                       ;   ls::TaggedAllocator<int>>::~DynamicArray()
```

Three facts fall out of that single branch:

1. **Element type** — the callee is the `SpellMeta` instantiation of
   `~DynamicArray`, named in its own mangled symbol. Nothing is inferred.
2. **Array offset is `+0x00`** — `x0` is passed through unadjusted. A member at
   a non-zero displacement would need an `add x0, x0, #N` first (see the
   contrast case below).
3. **The array is the only member that owns memory** — the thunk tail-calls;
   there is no second destructor call and no epilogue.

## Contrast cases — the test discriminates, it does not match everything

`eoc::spell::ContainerComponent`, whose `Array<SpellMeta>` decode already
shipped and is exercised by `tests/tier0/test_spell_meta_layout.c`, has the
identical thunk:

```text
0x1019d8f60  T  ecs::_private::GetComponentDestructor<eoc::spell::ContainerComponent>()...
1019d8f60: 14000001    b  0x1019d8f64   ; ~DynamicArray<eoc::spell::SpellMeta>
```

`eoc::spell::BookComponent` does not:

```text
0x1019dd728  T  ecs::_private::GetComponentDestructor<eoc::spell::BookComponent>()...
1019dd728: 91002000    add  x0, x0, #0x8
1019dd72c: 14000001    b    0x1019dd730
                            ; ~DynamicArray<eoc::spell::SpellData>
```

so the same read reports a *different* offset (`+0x08`) and a *different*
element type (`SpellData`) where those differ. That is why this thunk is
usable as evidence rather than as a coincidence.

## Struct size — 0x10

```text
0x101f6ffc0  t  ecs::legacy::ImmediateWorldCache::AddComponent<
                  eoc::spell::AddedSpellsComponent>(ls::ID<ecs::EntityHandleTraits>)
```

```asm
101f70128: 52800201    mov  w1, #0x10          ; =16
101f7012c: 950fb889    bl   0x10635e350
                            ; ecs::ComponentFrameStorageAllocRaw(
                            ;   ComponentFrameStorage&, int,
                            ;   ComponentFrameStorageIndex&)
```

`w1` is the byte count. 0x10 is exactly the `DynamicArray` header, which agrees
with point 3 above: the component is that array and nothing else.

## `DynamicArray` header offsets

Read off the same destructor at `0x1019d8f64`:

```asm
1019d8f84: ldr    w8, [x0, #0x8]     ; capacity_
1019d8f90: ldr    w8, [x19, #0xc]    ; size_
1019d8fc4: ldr    x25, [x19]         ; buf_
1019d8fb8: add    x20, x20, #0x60    ; per-element stride
```

`buf_ +0x00 / capacity_ +0x08 / size_ +0x0C`, matching what
`component_property.c` already reads for `SpellContainer.Spells`, and the
0x60 stride documented in [SPELL_META_LAYOUT.md](SPELL_META_LAYOUT.md).

The destroy loop releases a `FixedString` at element `+0x00` and one at `+0x54`
— `m_Id.OriginatorPrototype` and `m_ContainerId` — which is the same pair the
`SpellMeta` document derives independently from the serializer.

## Resulting declaration

```c
{ "Spells", 0x00, FIELD_TYPE_DYNAMIC_ARRAY, 0, true,
  ELEM_TYPE_SPELL_META, SPELL_META_SIZE },
```

Pinned by `tests/tier0/test_component_offsets_spells.c`.

## What is still not decoded

Windows also exposes `eoc::spell::LearnedSpellsComponent`. Its destructor
(`0x1019f4ba0`) is a real function body rather than a tail call to a
`DynamicArray` thunk, so its member layout is not settled by this technique and
it is deliberately left alone.

## Reproducing

```bash
BIN="$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"
lipo -thin arm64 "$BIN" -output /tmp/bg3_arm64
nm -arch arm64 /tmp/bg3_arm64 | grep 'GetComponentDestructorIN3eoc5spell'
objdump -d --start-address=0x101f6f9d8 --stop-address=0x101f6f9dc /tmp/bg3_arm64
objdump -d --start-address=0x1019d8f60 --stop-address=0x1019d8f64 /tmp/bg3_arm64
objdump -d --start-address=0x1019dd728 --stop-address=0x1019dd730 /tmp/bg3_arm64
objdump -d --start-address=0x1019d8f64 --stop-address=0x1019d9010 /tmp/bg3_arm64
objdump -d --start-address=0x101f70120 --stop-address=0x101f70130 /tmp/bg3_arm64
```

Addresses are the binary's own preferred load addresses (no slide applied).
