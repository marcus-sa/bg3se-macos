# BG3SE short component names → engine class names — 4.1.1.7398727

**Game version:** Baldur's Gate 3 v4.1.1.7398727

**Platform:** macOS ARM64

**Derived:** 2026-09-02

**Artifact:** `src/entity/generated_component_aliases.h` (643 rows)

## Why this table exists

`Ext.Entity.OnCreate(name, fn)` resolves `name` through
`resolve_component_type()` in `src/entity/entity_events.c`. Before this table,
resolution was: exact registry match, then a probe over
`{eoc::, esv::, ecl::, ls::} × name × {Component, ""}`, plus three hardcoded
inner-namespace initialisms (`CC`, `Hotbar`).

That probe can only ever reach a component whose engine name is
*outer namespace + short name (+ "Component")*. It cannot reach

```
CombatantJoinEvent  ->  esv::combat::JoinEventOneFrameComponent
```

because neither the inner `combat::` namespace nor the `OneFrame` infix is
recoverable from the short name.

`Ext.Entity.OnCreate` **raises** on an unresolvable name. Mods call it at file
scope, so the raise aborts the chunk: Expansion's `BootstrapServer.lua`
subscribes to `CombatantJoinEvent` at line 2421 and every registration in the
remaining ~2000 lines silently never ran. One missing name disabled most of the
mod, and the only symptom was a single `PAK load error`.

## Where each column comes from

| Column | Authority | Why |
|---|---|---|
| short name | Windows BG3SE `DEFINE_COMPONENT(<short>, "<engine class>")` under `BG3Extender/GameDefinitions/` | This is extender API surface, not engine data. A macOS-only spelling would break every mod written against Windows BG3SE, which is all of them. |
| engine class | the name column of `src/entity/generated_component_registry.c` | That file is extracted from this build's `ls::TypeId<T, ecs::ComponentTypeIdContext>::m_TypeIndex` symbols, i.e. from the shipped arm64 slice. |

Only pairs whose engine class exists on **this** build are emitted. 643 pairs
were read from Windows, 643 kept — nothing is dropped on this build any more.
The 14 that used to be dropped are covered below.

## Why the mapping cannot fabricate a component

`resolve_component_type()` consults the table *after* an exact registry match
and *before* the probe, and treats a miss as "keep going":

```c
const char *aliased = component_alias_lookup(name);
if (aliased) {
    info = component_registry_lookup(aliased);
    if (info && info->index != COMPONENT_INDEX_UNDEFINED) return info->index;
    /* fall through to the probe */
}
```

An alias whose target this build does not register is inert, not harmful. The
only way the table can be wrong is if Windows itself pairs a short name with the
wrong engine class — the table adds no independent guesses.

## Two names the probe was answering incorrectly

The probe silently disagreed with Windows on two names, which the table now
settles:

| Short name | Probe reached | Windows / this table |
|---|---|---|
| `Level` | `eoc::LevelComponent` | `ls::LevelComponent` (`eoc::LevelComponent` is `EocLevel`, also in the table) |
| `Constellation` | `esv::ConstellationComponent` | `ls::constellation::Component` |

Both are now reachable under the names Windows uses. Note
`src/entity/component_offsets.h` already bound the *layout* shortName `"Level"`
to `ls::LevelComponent`, so the probe was the odd one out.

## Also wired into property access

`component_property_get_layout_by_short_name()` falls back through the same
table when the direct shortName match fails. The generated layouts carry a
shortName derived from the engine class name — `eoc::spell::AddedSpellsComponent`
becomes `"AddedSpellsComponent"` — while a mod writes `entity.AddedSpells`.
The fallback runs only after the direct match fails, so no existing name
changes meaning.

## The 14 pairs that used to be missing — now all present

These were absent because their engine class was absent from
`generated_component_registry.c`:

```
DefaultCameraBehavior  -> ls::DefaultCameraBehavior
EffectCameraBehavior   -> ls::EffectCameraBehavior
GameCameraBehavior     -> ecl::GameCameraBehavior
GlobalCombatRequests   -> esv::combat::GlobalCombatRequests
LongRestState          -> eoc::rest::LongRestState
LongRestTimeline       -> eoc::rest::LongRestTimeline
LongRestTimers         -> eoc::rest::LongRestTimers
LongRestUsers          -> eoc::rest::LongRestUsers
RestingEntities        -> eoc::rest::RestingEntities
Scenery                -> ecl::Scenery
ServerCharacter        -> esv::Character
ServerItem             -> esv::Item
ServerProjectile       -> esv::Projectile
TLPreviewDummy         -> ecl::TLPreviewDummy
```

They were missing from the registry because `component_surface()` in
`tools/extract_typeids.py` kept only classes whose name contained `Component`.
That is a naming convention, not a property of the ECS: every one of these
carries a real `ls::TypeId<T, ecs::ComponentTypeIdContext>::m_TypeIndex`. The
filter is gone, so the surface went from 2004 to 2092 rows and all 14 resolve.

All 14 confirmed on this build (addresses are the ones now in
`generated_component_registry.c`):

| short name | engine class | m_TypeIndex | guard |
|---|---|---|---|
| `DefaultCameraBehavior` | `ls::DefaultCameraBehavior` | `0x10896e468` | `0x10896e470` |
| `EffectCameraBehavior` | `ls::EffectCameraBehavior` | `0x10896e488` | `0x10896e490` |
| `GameCameraBehavior` | `ecl::GameCameraBehavior` | `0x1088e1658` | `0x1088e1660` |
| `GlobalCombatRequests` | `esv::combat::GlobalCombatRequests` | `0x1089295d0` | `0x1089295d8` |
| `LongRestState` | `eoc::rest::LongRestState` | `0x10891b490` | `0x10891b498` |
| `LongRestTimeline` | `eoc::rest::LongRestTimeline` | `0x108943f58` | `0x108943f60` |
| `LongRestTimers` | `eoc::rest::LongRestTimers` | `0x10891ac80` | `0x10891ac88` |
| `LongRestUsers` | `eoc::rest::LongRestUsers` | `0x108943f68` | `0x108943f70` |
| `RestingEntities` | `eoc::rest::RestingEntities` | `0x10893fb48` | `0x10893fb50` |
| `Scenery` | `ecl::Scenery` | `0x1088df3a8` | `0x1088df3b0` |
| `ServerCharacter` | `esv::Character` | `0x10894d9c8` | `0x10894d9d0` |
| `ServerItem` | `esv::Item` | `0x10894a510` | `0x10894a518` |
| `ServerProjectile` | `esv::Projectile` | `0x10893fce8` | `0x10893fcf0` |
| `TLPreviewDummy` | `ecl::TLPreviewDummy` | `0x1088e1bf0` | `0x1088e1bf8` |

```bash
# reproduce any row
lipo -thin arm64 "$BG3" -output /tmp/bg3_arm64
nm -arch arm64 -gU /tmp/bg3_arm64 | c++filt | \
  grep -E 'ls::TypeId<esv::Character, ecs::ComponentTypeIdContext>::m_TypeIndex'
```

The earlier note that `ls::DefaultCameraBehavior` and `ls::EffectCameraBehavior`
"have no `ComponentTypeIdContext` symbol at all" was wrong — they were simply
being filtered out with the other 86.

`ServerCharacter` and `ServerItem` additionally have derived property layouts;
see [SERVER_CHARACTER_ITEM_LAYOUT.md](SERVER_CHARACTER_ITEM_LAYOUT.md). The
other 12 resolve as component types (so `Ext.Entity.OnCreate` accepts them) but
carry no field offsets.

## Left unfixed

Nothing from the Windows `DEFINE_COMPONENT` set. Two things that widening did
*not* fix, recorded so they are not mistaken for regressions:

* **`.Template.Icon` still fails.** `ServerCharacter.Template` resolves to the
  `eoc::CharacterTemplate*` as an address; there is no template proxy to index
  into. See the last section of SERVER_CHARACTER_ITEM_LAYOUT.md.
* **Fields that could not be derived** are listed per component in that same
  document — `esv::Character.Inventory` most notably. They are omitted rather
  than guessed.

## Two side effects of widening the surface

Widening added 88 registry rows, and `resolve_component_type()` probes the
registry after consulting this table. Two consequences:

1. **70 short names became probe-reachable that previously raised.** They are
   the trigger and camera-behaviour classes (`RegionTrigger`, `PortalTrigger`,
   `ArcBallCameraBehavior`, `ls::Scene`, …). Previously `Ext.Entity.OnCreate`
   raised "Unknown component type" on all of them; now they resolve. Purely
   additive — none of them resolved to anything before.

2. **Bare `Character` and `Item` changed meaning in the probe.** The probe order
   is `eoc::`, `esv::`, `ecl::`, so `"Character"` used to fall through to
   `ecl::Character` and now stops at `esv::Character` (same for `Item`). Neither
   is a BG3SE name: Windows has `ClientCharacter` for `ecl::Character` and
   `ServerCharacter` for `esv::Character`, both of which are in this table and
   are consulted before the probe, so no mod written against Windows BG3SE is
   affected. The bare spellings were a port-only accident in both directions.

## Regenerating

```bash
# 1. the engine-class column (also emits the guard table)
BIN="$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"
python3 tools/extract_typeids.py "$BIN" --build-id 4.1.1.7398727 \
    --header-out src/entity/generated_typeids.h \
    --registry-out src/entity/generated_component_registry.c

# 2. the short-name column, filtered against what step 1 produced
python3 tools/generate_component_aliases.py \
    --windows /path/to/bg3se-windows/BG3Extender/GameDefinitions
```

`generate_component_aliases.py` prints `<n> Windows pairs, <n> kept, <n>
dropped` on stderr and names every dropped pair, so a build where some engine
class disappears is visible without diffing the header.

Rows must stay sorted by short name — `component_alias_lookup()` binary-searches
and `tests/tier0/test_component_aliases.c` asserts the ordering, because an
out-of-order row is silently unreachable and looks identical to "not an alias".
