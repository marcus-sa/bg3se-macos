# BG3SE short component names → engine class names — 4.1.1.7398727

**Game version:** Baldur's Gate 3 v4.1.1.7398727

**Platform:** macOS ARM64

**Derived:** 2026-09-02

**Artifact:** `src/entity/generated_component_aliases.h` (629 rows)

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
were read from Windows, 14 dropped (below), 629 kept.

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

## The 14 Windows pairs that are not in the table

These are dropped because their engine class is absent from
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

Most of them **do** exist on this build — the TypeId globals are exported:

```bash
nm -arch arm64 /tmp/bg3_arm64 | \
  grep -E '__ZN2ls6TypeIdIN3esv9CharacterEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE$'
# 0000000108... D  __ZN2ls6TypeIdIN3esv9CharacterEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE
```

They are missing because the TypeId extractor that produced
`generated_component_registry.c` only keeps classes whose name ends in
`Component`; `esv::Character`, `ecl::Scenery`, `eoc::rest::LongRestState` and
friends do not. (`ls::DefaultCameraBehavior` and `ls::EffectCameraBehavior` have
no `ComponentTypeIdContext` symbol at all and may be Windows-only or
differently spelled here.)

Fixing that means widening the extractor in `tools/`, which is out of scope for
this change; the aliases for those names are deliberately absent rather than
present-but-dead, so nothing claims to work that does not.

## Regenerating

```python
# Run from the repo root with the Windows reference checked out.
import re, os
WIN  = '/path/to/bg3se-windows/BG3Extender/GameDefinitions'
PORT = 'src/entity'

pairs = {}
for root, _, files in os.walk(WIN):
    for f in sorted(files):
        txt = open(os.path.join(root, f), encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'DEFINE_COMPONENT\(\s*([A-Za-z0-9_]+)\s*,\s*"([^"]+)"', txt):
            pairs[m.group(1)] = m.group(2)

reg   = open(os.path.join(PORT, 'generated_component_registry.c'), encoding='utf-8').read()
names = set(re.findall(r'^\s*\{\s*"([^"]+)"', reg, re.M))

for short, engine in sorted((s, e) for s, e in pairs.items() if e in names):
    print('    { "%s", "%s" },' % (short, engine))
```

Rows must stay sorted by short name — `component_alias_lookup()` binary-searches
and `tests/tier0/test_component_aliases.c` asserts the ordering, because an
out-of-order row is silently unreachable and looks identical to "not an alias".
