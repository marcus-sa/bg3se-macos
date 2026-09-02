# ImmutableDataHeadmaster Manager Lookup — build 4.1.1.7398727 (arm64)

Why `Ext.StaticData.Get(guid, "Progression")` returned nil, and why the fix is a
vtable check rather than a new offset.

Related: [STATICDATA.md](STATICDATA.md), [STATICDATA_MANAGERS.md](STATICDATA_MANAGERS.md).

## The lookup

`src/staticdata/staticdata_registry.c` resolves a GuidResource bank by keying the
headmaster's manager `HashMap` with a per-type index the game keeps in a global:

```
ls::TypeId<eoc::ProgressionManager, ls::ImmutableDataHeadmaster>::m_TypeIndex
```

`tools/generate_staticdata_registry.py` pairs each Windows resource type name with
that global's image-relative offset. The offsets are correct — verified against
the binary:

```
$ nm -arch arm64 bg3_arm64 | grep m_TypeIndexE | c++filt | grep ProgressionManager
000000010893ddb8 D ls::TypeId<eoc::ProgressionManager, ls::ImmutableDataHeadmaster>::m_TypeIndex
000000010893ddc0 D guard variable for ls::TypeId<eoc::ProgressionManager, …>::m_TypeIndex
```

`0x10893ddb8 - 0x100000000 = 0x0893ddb8`, which is exactly what the generated
table carries.

## The defect: m_TypeIndex is a guarded static that ships as 0

Each `m_TypeIndex` is a **function-local static** — the `__ZGV…` guard variable at
`m_TypeIndex + 8` proves it. Both slots live in `__DATA,__data` and both are zero
in the file image:

```
Progression:       fileoff=0x893ddb8  idx=00000000  guard=0000000000000000
SpellList:         fileoff=0x8927ba0  idx=00000000  guard=0000000000000000
ClassDescription:  fileoff=0x8941368  idx=00000000  guard=0000000000000000
PassiveList:       fileoff=0x8927b80  idx=00000000  guard=0000000000000000
EquipmentList:     fileoff=0x8927b70  idx=00000000  guard=0000000000000000
```

Checked for **all 105** types in the generated table: every one starts at 0 with a
zero guard.

So until the game itself first resolves a type, reading its `m_TypeIndex` yields
`0` — and `0` is a perfectly valid index belonging to whichever type registered
first. The old code only rejected `type_index < 0`, so an unresolved type walked
the map with someone else's key and got back **a different type's bank**. The
subsequent `GetObjectByKey` on that foreign bank simply missed, so the symptom
was a silent nil rather than a visible mis-decode — but the same code path would
happily have handed a caller a foreign object had the GUIDs collided.

Evidence from a live session (`bg3se_2026-09-02_02-12-21.log`):

| line | time | event |
|---|---|---|
| 27962 | 02:13:35.509 | `EXP_Lib.lua:40: attempt to index a nil value (local 'resource')` — `Get(…, "Progression")` |
| 27968 | 02:13:35.523 | `static data layout for SpellList checks out` — `Get(…, "SpellList")` succeeded 14 ms later |
| 34135 | 02:13:44.466 | `Get<T>` hook captures the real `ClassDescriptions` |
| 38932 | 02:14:27.891 | `Get<T>` hook captures the real `ProgressionManager` |

`SpellList` worked because the game had already resolved that type (its guard had
run); `Progression` had not been touched yet, so its index global was still 0.
Across 281 session logs the `static data layout for <type>` self-test line has
only ever printed `SpellList` — no `Progression`, `ClassDescription`,
`PassiveList` or `EquipmentList` resource has ever been pushed successfully.

## The fix: identify the bank by its vtable

`__ZTV<class>` exists for every one of the 105 engine classes in the registry, so
a bank can be identified without trusting any index:

```
$ nm -arch arm64 bg3_arm64 | grep ' __ZTV' | c++filt
00000001086dd9b8 s vtable for eoc::ProgressionManager
00000001086ef340 s vtable for eoc::SpellListManager
…
```

An object's vptr is the symbol **+ 0x10**, not the symbol itself. Dumping the head
of each vtable out of `__DATA_CONST,__const` shows the Itanium layout with no RTTI:

```
eoc::SpellListManager  (0x1086ef340)      eoc::ProgressionManager (0x1086dd9b8)
  +0x00 0000000000000000  offset-to-top     +0x00 0000000000000000
  +0x08 0000000000000000  typeinfo (none)   +0x08 0000000000000000
  +0x10 0000000101f42fbc  slot 0  ~D1       +0x10 0000000101c23bf0
  +0x18 0000000101f42fc0  slot 1  ~D0       +0x18 0000000101c23bf4
  +0x20 0000000101f4304c  slot 2            +0x20 0000000101c23c80
  +0x28 0000000101f437ec  slot 3            +0x28 0000000101c24420
  +0x30 0000000101f438f4  slot 4            +0x30 0000000101c24528
  …
  +0x40 0000000101f43e84  slot 6  GetObjectByKey
```

`offset-to-top == 0` and `typeinfo == 0` for **all 105** vtables (there is no
`__ZTI`/`__ZTS` symbol for any of them — the binary is built `-fno-rtti`), so the
address point is uniformly `+0x10`. This also independently confirms the existing
`BANK_VT_GET_OBJECT_BY_KEY = 0x30` slot-6 assumption: `vptr + 0x30` is file
`+0x40`, the seventh entry, past the two-slot destructor pair.

`staticdata_registry_get_manager_ex` now:

1. reads the guard byte at `m_TypeIndex + 8` and only uses the index if it is
   non-zero **and** the index is `>= 0`;
2. still validates that the bank the index found carries this type's vptr;
3. falls back to a linear sweep of the map's `Values` array (bounded by
   `Keys.size` at `+0x2C`) for a bank whose vptr matches — which reaches a type
   whose index global has never been initialised;
4. reports *why* it failed (`no headmaster` / `m_TypeIndex never initialised` /
   `no bank of this type` / `map unreadable`) so `Ext.StaticData.Get` can log a
   line naming the type and GUID instead of returning a bare nil.

## Offsets used

| Symbol / field | Image-relative | Source |
|---|---|---|
| `ls::ImmutableDataHeadmaster::m_ptr` | `0x08ac13c8` | pre-existing, `staticdata_registry.c` |
| `HashMap.HashKeys` / `HashSize` | `+0x00` / `+0x08` | disassembly of `Get<ls::TagManager>` @ `0x10118616c` |
| `HashMap.NextIds` / `Keys` / `Keys.size` / `Values` | `+0x10` / `+0x20` / `+0x2C` / `+0x30` | same |
| `TypeId<T,H>::m_TypeIndex` guard | `m_TypeIndex + 8` | `__ZGV…` symbol address, all 105 types |
| vtable address point | `__ZTV<class> + 0x10` | zero offset-to-top + zero typeinfo, all 105 |
| `GetObjectByKey` | `vptr + 0x30` (slot 6) | vtable dump above |

Per-type vtable offsets are generated into
`src/staticdata/generated_staticdata_vtables.c`; the file header carries the
reproduction command. `tests/tier0/test_staticdata_registry.c` fails if a type
loses its vtable row, if two types share one, or if an index offset collides with
a vtable offset.

## What is still unknown

Whether `eoc::ProgressionManager` (and `ClassDescriptions`, `PassiveListManager`,
`EquipmentListManager`) is *present in the headmaster map at all* when
`StatsLoaded` fires. The vtable sweep can only find a bank that has been
registered. The logs show the game resolving `ProgressionManager` ~52 s after
`StatsLoaded`, which says nothing about whether the bank existed earlier. If the
sweep still misses, the new log line says so explicitly:

```
[StaticData] 'Progression' (eoc::ProgressionManager) cannot be resolved on this
build: m_TypeIndex never initialised. Get('aafbbc41-…', 'Progression') and every
other lookup of this type returns nil.
```
