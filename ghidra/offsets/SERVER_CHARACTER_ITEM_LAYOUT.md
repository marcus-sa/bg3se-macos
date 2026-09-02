# esv::Character / esv::Item member layout — build 4.1.1.7398727 (arm64)

Why `Ext.Entity.Get(uuid).ServerCharacter` used to be nil, why the layout needs
one extra dereference, and where every offset in
`src/entity/component_offsets.h` came from.

Related: [COMPONENT_NAME_ALIASES.md](COMPONENT_NAME_ALIASES.md),
[STATICDATA_HEADMASTER_LOOKUP.md](STATICDATA_HEADMASTER_LOOKUP.md),
[EXTRACTION_METHODOLOGY.md](EXTRACTION_METHODOLOGY.md).

## Setup

Everything below is read out of the arm64 slice. The shipped binary is fat and
reading file offsets without thinning silently yields garbage:

```bash
BIN="$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"
lipo -thin arm64 "$BIN" -output /tmp/bg3_arm64
dis() { objdump -d --no-show-raw-insn --start-address=$1 --stop-address=$2 /tmp/bg3_arm64; }
```

Note: pass `objdump` the plain form. With `--macho` this Apple objdump ignores
`--start-address` and dumps from the top of `__text`.

## Both are proxy components: the ECS slot holds a pointer

This is the part that has to be right before any offset means anything. The slot
in the component page is 8 bytes and holds an `esv::Character*`; the object
itself is a separate heap allocation.

```
$ dis 0x1052dbc20 0x1052dbc90
; ecs::legacy::ImmediateWorldCache::AddComponent<esv::Character,
;                                   eoc::CharacterTemplate const*&>
1052dbc2c   mov  w0, #0x1a8               ; = 424, sizeof(esv::Character)
1052dbc30   bl   <operator new>
1052dbc44   bl   0x10516e10c              ; esv::Character::Character(ls::GameObjectTemplate const*)
1052dbc74   mov  w1, #0x8                 ; component slot size
1052dbc78   bl   ecs::ComponentFrameStorageAllocRaw
1052dbc7c   str  x25, [x0]                ; slot <- Character*
```

The destructor confirms the indirection from the other side:

```
$ dis 0x103bdeb6c 0x103bdeba8
; ecs::_private::GetComponentDestructor<esv::Character>::__invoke
103bdeb7c   ldr  x19, [x0]                ; load the object out of the slot
103bdeb84   add  x0, x19, #0x38
103bdeb88   bl   esv::CharacterColdData::~CharacterColdData
```

`esv::Item` is identical in shape:

```
$ dis 0x1052d8830 0x1052d8880   ; AddComponent<esv::Item, eoc::ItemTemplate* const&>
1052d8844   mov  w0, #0xb0                ; = 176, sizeof(esv::Item)
1052d885c   bl   0x105466bb4              ; esv::Item::Item(ls::GameObjectTemplate const*)

$ dis 0x103bd304c 0x103bd3088   ; GetComponentDestructor<esv::Item>
103bd305c   ldr  x19, [x0]
103bd3064   add  x0, x19, #0x38
103bd3068   bl   esv::ItemColdData::~ItemColdData
```

Independent corroboration: Windows BG3SE declares both as
`struct Character : public BaseProxyComponent` / `struct Item : public
BaseProxyComponent`.

Consequence in this port: `component_lookup_by_index()` already had an `isProxy`
parameter, but the `entity.<ShortName>` path in `entity_system.c` hardcoded
`false`, so it returned the slot address. `ComponentLayoutDef` therefore gained
an `isProxy` flag and that call site now passes `layout->isProxy`. Without it
`ServerCharacter.Template` would read 0xC8 bytes into the storage page: a
plausible-looking pointer that is not a template.

The four client object components have the same shape and are flagged the same
way, even though they carry no properties yet:

| type | GetComponentDestructor | first instruction |
|---|---|---|
| `ecl::Character` | `0x100eaa7d0` | `ldr x19, [x0]` |
| `ecl::Item` | `0x100e41804` | `ldr x19, [x0]` |
| `ecl::Projectile` | `0x100e3fd04` | `ldr x8, [x0]` |
| `ecl::Scenery` | `0x100e3e514` | `ldr x19, [x0]` |

`esv::Projectile` (`0x103bceca8`) is the same shape, but no field offsets were
derived for it, so its layout deliberately keeps modelling the 8-byte slot and
stays `isProxy = false`; its single `ProjectilePtr` property is the pointer the
slot holds.

## esv::Character — sizeof 0x1a8

`Template` is the field with a known consumer (AppearanceEditEnhanced reads
`Ext.Entity.Get(uuid).ServerCharacter.Template.Icon`). It is nailed down by six
independent sites; the first is the whole function:

```
$ dis 0x10516929c 0x1051692a8
000000010516929c <esv::Character::GetTemplate() const>:
10516929c   ldr  x0, [x0, #0xc8]
1051692a0   ret
```

| # | site | instruction |
|---|---|---|
| 1 | `esv::Character::GetTemplate() const` @`0x10516929c` | `ldr x0, [x0, #0xc8]; ret` |
| 2 | `esv::Character::SetTemplate(ls::GameObjectTemplate const*)` @`0x105169254` | `ldr x0,[x0,#0xc8]` … `str x19,[x20,#0xc8]`, bracketed by `esv::DecreaseTemplateRefCount` / `IncreaseTemplateRefCount` |
| 3 | `esv::Character::Character(ls::GameObjectTemplate const*)` @`0x10516e1f4` | `str x20, [x19, #0xc8]` with x20 = the ctor's template argument |
| 4 | `esv::Character::GetCharacterTemplate(bool) const` @`0x1051718bc` | `ldr x0, [x19, #0xc8]` (fallback return) |
| 5 | `esv::CharacterColdData::~CharacterColdData` @`0x103bdf1fc` | `ldr x0,[x19,#0x90]` → `DecreaseTemplateRefCount` → `str xzr,[x19,#0x90]`; `~Character` enters ColdData with `add x0, x0, #0x38`, so `0x38 + 0x90 = 0xC8` |
| 6 | `esv::Character::SavegameVisit` @`0x10516e96c` | `ldp x23, x0, [x20, #0xc8]` (Template + OriginalTemplate as a pair) |

The pointee type is `eoc::CharacterTemplate*`, not merely
`ls::GameObjectTemplate*`: the only caller of the ctor is
`AddComponent<esv::Character, eoc::CharacterTemplate const*&>`.

Fields committed to `component_offsets.h`, each with the site the offset was
read from:

| offset | width | field | evidence |
|---|---|---|---|
| `0x10` | 8 | `field_10` (own EntityHandle) | `GetEntityObjectHandle() const` @`0x105169304` = `ldr x0,[x0,#0x10]; ret`; `AddComponent` @`0x1052dbc84` `str x8,[x25,#0x10]` |
| `0x18` | 8 | `Flags` | `IsFlag(unsigned long long)` @`0x105169960` `ldr x8,[x0,#0x18]; tst x8,x1`; `RaiseFlag` @`0x105169314` `orr`/`str`; `ClearFlag` @`0x105169664` `bic`/`str` |
| `0x20` | 8 | `MyHandle` (leading handle of `ecs::EntityRef`) | `GetEntityRef() const` @`0x10516930c` = `add x0,x0,#0x20; ret`; `GetUUID() const` @`0x105169230` `ldp x1, x8, [x0, #0x20]` passes `{handle, world*}` |
| `0x30` | 4 | `Level` (FixedString) | `GetCurrentLevel() const` @`0x10516a23c` = `add x0,x0,#0x30; ret`; `SetGlobal` @`0x1051692cc` `add x2, x0, #0x30` as the `FixedString const&` argument of `LEGACY_SetGlobal` |
| `0x34` | 4 | `VisualResource` (FixedString) | ctor @`0x10516e150` `str d0,[x0,#0x30]` with `d0 = -1` initialises two FixedStrings; `~Character` @`0x105169000` releases `[x19,#0x34]` then `[x19,#0x30]` through `ls::gst::Map::Release`. **Offset and width proven; the name is only the FixedString Windows declares after `Level`.** |
| `0xC8` | 8 | `Template` | six sites above |
| `0xD0` | 8 | `OriginalTemplate` | `SetOriginalTemplate` @`0x10516ac48`/`0x10516ac70` with template refcounting; `~CharacterColdData` @`0x103bdf1ec` `ldr x0,[x19,#0x98]` (`0x38+0x98`); ctor @`0x10516e214` |
| `0xD8` | 8 | `TemplateUsedForSpells` | `LEGACY_CacheTemplatesIfNeeded` @`0x10516cc44` releases `[x19,#0xd8]` then @`0x10516cc68` stores the value loaded from `[x19,#0xd0]`; `~CharacterColdData` @`0x103bdf1dc` (`0x38+0xa0`) |
| `0x130` | 8 | `StatusManager` | typed by callee: `SavegameVisit` @`0x10516f10c` → `esv::StatusMachine::SavegameVisit`; `Update` @`0x105173a5c` → `esv::StatusMachine::RemoveStatuses` |
| `0x150` | 8 | `PlayerData` | `CreatePlayerData` @`0x1051718d8` guards on it, `malloc(0x98)`, `bl esv::PlayerData::PlayerData()`, `str x20,[x19,#0x150]` @`0x10517197c` |
| `0x158` | 8 | `OwnerCharacter` | `SetOwnerCharacter` @`0x105176680` `str x23,[x19,#0x158]`; `GetOwnerUserID(bool)` @`0x1051757c8` walks the same field through `GetComponent<esv::Character const>` |
| `0x160` | 8 | `FollowCharacter` | `SetFollowCharacter` @`0x10517161c` `str x19,[x20,#0x160]` (x19 = argument); `SavegameVisit` @`0x10516f174` |
| `0x170` | 8 | `EnemyCharacter` | `SetEnemyCharacter` reads the old value @`0x1051768d0` and stores the argument @`0x10517694c` |
| `0x184` | 4 | `UserID` | `GetOwnerUserID(bool)` @`0x105175820` returns `ldr w0,[x19,#0x184]`; ctor @`0x10516e1b8` writes `0xFFFF0000` to `0x184` and `0x188` |
| `0x188` | 4 | `UserID2` | `GetReservedForUserID(bool)` @`0x1051726dc` returns `ldr w0,[x19,#0x188]`; `SetReservedForUserID` @`0x105175870` compares against the `net::UserID const&` argument |
| `0x18C` | 4 | `GeneralSpeedMultiplier` | ctor @`0x10516e1c0` stores `0x3f800000` (1.0f); `esv::msmoveto_helpers::GetModifiedMovementSpeed` @`0x104b21988` `ldr s0,[x20,#0x18c]; fmul s0, s8, s0`; `CharacterProtocol::SyncPeerCharacters` @`0x1048af510` delta-syncs the same float |

### Left out of esv::Character, and why

* **`Inventory`.** Not found. The ctor writes four null handles at
  `0x158/0x160/0x168/0x170` (`stp q0, q0, [x8]` @`0x10516e1a0`, x8 = `this`+0x158),
  and `0x168` is the one Windows' declaration order would call `Inventory` — but
  `SyncPeerCharacters` @`0x1048af53c` resolves it through
  `ecs::EntityWorld::GetComponent<esv::Character>`, i.e. it holds a *character*.
  No call site anywhere hands it to an inventory API. The macOS handle order
  already differs from the Windows declaration order, so assuming `0x168` is
  `Inventory` would be exactly the mistake that mapped `Level` to the wrong
  component.
* **The `0x38..0xC8` array block.** `~CharacterColdData` destroys exactly nine
  `Array<T>` slots at ColdData `+0x00,0x10,…,0x80`, i.e. Character
  `0x38,0x48,…,0xB8`. Only slot 0 is independently typed (`SyncPeerCharacters`
  @`0x1048ac520` strides it by 4 bytes = `PeerId`, so it is `UpdatePeerIds`).
  Windows names `Summons`/`CreatedTemplateItems`/`Treasures`/… in a declaration
  order this build is already known to reorder, so the rest are unnamed.
* **`0xE0`–`0x148`.** The AI/dialog/controller pointer block. Two of them
  (`0x108`, `0x120`) are `esv::TaskController*` by `SavegameVisit`, and `0xF8`
  feeds `esv::OsirisTaskFactory::CreateTask`, but Windows declares five
  same-typed pointers in this span so none can be individually named.
* **`VariableManager` @`0x138`.** One site only (`SavegameVisit` @`0x10516f25c`
  → `ls::VariableManager::Visit`). Kept out for want of a second.

No `ecs::sync::Serialize<esv::Character, ls::FieldMeta<…>>` exists in this
binary — the component is not net-synced through that path — so the
named-member-list technique was unavailable here. `SavegameVisit`'s field-name
`FixedString`s are runtime-initialised to −1 by
`__GLOBAL__sub_I_EoCFixedStrings.cpp` @`0x106638578`, so they could not be read
out of the image either.

## esv::Item — sizeof 0xb0

```
$ dis 0x105461c5c 0x105461c68
0000000105461c5c <esv::Item::GetTemplate() const>:
105461c5c   ldr  x0, [x0, #0x48]
105461c60   ret
```

`Template` @`0x48`, from six sites: `GetTemplate` above; `SetTemplate`
@`0x105461c14` (`ldr x0,[x0,#0x48]` … `str x19,[x20,#0x48]` between the refcount
calls); the ctor @`0x105466c60` storing its argument; `esv::ItemColdData::~ItemColdData`
@`0x10546418c` (`ldr x0,[x19,#0x10]` → `DecreaseTemplateRefCount`, ColdData is at
Item+0x38 so `0x38+0x10 = 0x48`); `SavegameVisit` @`0x1054672b0` and again
@`0x1054677f0` loading Template and OriginalTemplate as a pair. `GetTemplate` is
also vtable slot 6 of `vtable for esv::Item` @`0x10883e030`.

Pointee type is `eoc::ItemTemplate*`: the only caller of the ctor is
`AddComponent<esv::Item, eoc::ItemTemplate* const&>` @`0x1052d868c`.

| offset | width | field | evidence |
|---|---|---|---|
| `0x10` | 8 | `field_10` | `GetEntityObjectHandle() const` @`0x105461cc4` = `ldr x0,[x0,#0x10]; ret`; `AddComponent` @`0x1052d8c54` |
| `0x18` | 8 | `Flags` | `IsFlag` @`0x1054625f0` `ldr x8,[x0,#0x18]; tst x8,x1`; `ClearFlag` @`0x1054623a0`; `RaiseFlag` @`0x105461d0c` |
| `0x20` | 8 | `MyHandle` | `GetEntityRef` @`0x105461ccc` = `add x0,x0,#0x20; ret`; `GetUUID` @`0x105461bf0` `ldp x1, x8, [x0, #0x20]` |
| `0x30` | 4 | `Level` | `GetCurrentLevel` @`0x105463058` = `add x0,x0,#0x30; ret`; `SetGlobal` @`0x105461c8c` |
| `0x34` | 4 | `ItemType` | ctor @`0x105466c24` `stur w22,[x21,#-0x4]` after `ls::gst::Acquire`; dtor releases it. **Offset/width proven, name from the Windows declaration order.** |
| `0x48` | 8 | `Template` | six sites above |
| `0x50` | 8 | `OriginalTemplate` | `SetOriginalTemplate` @`0x105463f60`/`f78`/`f88` with refcounting; ctor @`0x105466c80`; ColdData dtor `+0x18` |
| `0x58` | 8 | `ItemMachine` | `ldr x21,[x19,#0x58]` @`0x105462af4` → `bl esv::ItemMachine::CreateState` @`0x105462b00` |
| `0x60` | 8 | `PlanManager` | `ldr x0,[x19,#0x60]` @`0x105463788` in the `esv::PlanManager::WriteInterruptParam` / `OnSuspend` sequence |
| `0x68` | 8 | `VariableManager` | `ldr x0,[x19,#0x68]` @`0x105467cc0` → `bl ls::VariableManager::Visit` |
| `0x70` | 8 | `StatusManager` | `ldr x0,[x19,#0x70]` @`0x105467dc0` → `bl esv::StatusMachine::SavegameVisit`; `SetOwner` @`0x105469178` → `StatusMachine::UpdateTickingOwnership` |
| `0x78` | 8 | `StatsObject` | `SetStatsId` @`0x10546986c` `str x0,[x19,#0x78]`; `SavegameVisit` @`0x1054678fc` stores the object looked up by the FixedString at `0x9c` |
| `0x9C` | 4 | `Stats` (FixedString) | `SetStatsId` @`0x1054697f0` reads, @`0x105469838` writes, with `gst::Acquire` / `gst::Map::Release`; ColdData dtor `+0x64` |

Unlike `esv::Character`, the derived offsets here line up with the Windows
declaration order all the way to `0xa0`, which is independent corroboration for
the middle of the struct.

### Left out of esv::Item, and why

* **`Amount`, `Vitality`.** No such fields. `esv::Item::SetAmount(int)`
  @`0x10546883c` writes nothing to `this`; it appends to an
  `ls::DynamicArray<esv::inventory::StackSystem::DirectRequest>`
  (`0x105468880`–`0x1054688e4`). `esv::Item::SetCurrentHP(int)` @`0x10546cc20`
  takes an `EntityHandle` in x0, not a `this` pointer.
* **The tail from `0xa4`.** Windows declares `int32 TreasureLevel; int32 Amount;
  ItemFlags2 Flags2;` here; this build has one int32 at `0xa4` and four separate
  byte-sized savegame fields at `0xa8..0xab`, all visited through the visitor's
  bool slot. The order stops holding, so nothing at or past `0xa4` is named.
  `IsGlobal() const` @`0x105461c98` does read bit 1 of the byte at `0xac`, but a
  single-bit accessor does not name the byte.
* **`InventoryParent` / `OwnerCharacter`.** No such members.
  `esv::Item::SetOwner(esv::Character const*)` @`0x105468c78` writes no Item
  field — it goes through `GetComponent<eoc::inventory::OwnerComponent>`. The
  EntityHandle at `0x90` is managed by `SetInUseByCharacter` @`0x10546cb78`,
  which is "in use by", not "owner", so it is not exposed under that name.
* **`0x80`, `0x88`, `0x98`, `0xa0`.** Read and written but never passed to a
  type-revealing callee. `0x98` and `0xa0` are FixedStrings (ColdData dtor
  `+0x60`, `+0x68`); Windows calls them `field_70` and `PreviousLevel`, but the
  adjacent tail already diverges, so the names are not carried.

## What resolves now, and what still does not

`Ext.Entity.Get(uuid).ServerCharacter` returns a property proxy, and
`.Template` returns the `eoc::CharacterTemplate*` as an address.
`.Template.Icon` still does not work: there is no `CharacterTemplate` proxy in
the property system, and `Ext.Template`'s own Lua surface exposes only `Guid`
(the rest of `push_template_to_lua` in `src/lua/lua_template.c` is behind
`#if 0`). Making `.Template.Icon` resolve needs a `CharacterTemplate` layout and
a nested-object field type, which is a separate change.

All pointer-valued properties are read-only. `SetTemplate` brackets its store
with `esv::IncreaseTemplateRefCount` / `DecreaseTemplateRefCount`, so a write
through the property system would desync the template refcount.
