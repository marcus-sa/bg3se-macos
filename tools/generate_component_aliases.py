#!/usr/bin/env python3
"""Generate src/entity/generated_component_aliases.h.

Pairs a BG3SE short component name with the engine class it names.  The two
columns have two different authorities and neither may be swapped for the
other:

  short name  - Windows BG3SE's own DEFINE_COMPONENT(<short>, "<engine class>")
                declarations.  This is extender API surface, not engine data; a
                macOS-only spelling would break every mod written against
                Windows BG3SE, which is all of them.

  engine name - the name column of src/entity/generated_component_registry.c,
                which tools/extract_typeids.py extracts from the shipped arm64
                slice.  A pair whose engine class this build does not register
                is dropped rather than emitted dead.

Usage:
    python3 tools/generate_component_aliases.py \\
        --windows /path/to/bg3se-windows/BG3Extender/GameDefinitions \\
        --out src/entity/generated_component_aliases.h

Rows are emitted sorted by short name under plain byte order, because
component_alias_lookup() binary-searches them with strcmp(); an out-of-order
row is silently unreachable and looks identical to "not an alias".
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REGISTRY = REPO_ROOT / "src/entity/generated_component_registry.c"
DEFAULT_OUT = REPO_ROOT / "src/entity/generated_component_aliases.h"

DEFINE_COMPONENT = re.compile(
    r'DEFINE_COMPONENT\(\s*([A-Za-z0-9_]+)\s*,\s*"([^"]+)"'
)
REGISTRY_ROW = re.compile(r'^\s*\{\s*"([^"]+)"', re.M)

HEADER = '''/*
 * generated_component_aliases.h - BG3SE short component name -> engine class name
 *
 * Generated; do not edit by hand. Regenerate with
 * tools/generate_component_aliases.py (see ghidra/offsets/COMPONENT_NAME_ALIASES.md).
 *
 * Build identity: {build_id}
 *
 * Two independent sources, one per column:
 *
 *   Short name  - the identifier a mod types into Ext.Entity.OnCreate /
 *                 OnDestroy. This is BG3SE API surface, not engine data, so
 *                 the authority is the Windows extender's own
 *                 DEFINE_COMPONENT(<short>, "<engine class>") declarations
 *                 under BG3Extender/GameDefinitions/. A macOS-only spelling
 *                 would break every mod written against Windows BG3SE.
 *
 *   Engine name - must name a real component on THIS build. Every row below
 *                 was filtered against the name column of
 *                 generated_component_registry.c, which is extracted from the
 *                 shipped arm64 slice's ls::TypeId<T, ecs::ComponentTypeIdContext>
 *                 ::m_TypeIndex symbols. Windows pairs whose engine class does
 *                 not exist here are dropped, not carried over.
 *
 * Why this table exists at all: resolve_component_type() in entity_events.c
 * probes <prefix><name><suffix> combinations, which can only ever reach
 * components whose engine name is the short name plus an outer namespace. It
 * cannot reach esv::combat::JoinEventOneFrameComponent from "CombatantJoinEvent"
 * — the inner namespace and the OneFrame infix are not recoverable by
 * probing. Expansion's BootstrapServer.lua subscribes to that component at
 * file scope, so the raised "Unknown component type" aborted the chunk and
 * every registration after that line silently never happened.
 *
 * Rows are sorted by short name so the lookup can binary-search.
 */

#ifndef BG3SE_GENERATED_COMPONENT_ALIASES_H
#define BG3SE_GENERATED_COMPONENT_ALIASES_H

#include <stddef.h>

typedef struct {{
    const char *shortName;
    const char *engineName;
}} ComponentAliasEntry;

static const ComponentAliasEntry g_ComponentAliases[] = {{
'''

FOOTER = '''}};

#define GENERATED_COMPONENT_ALIAS_COUNT \\
    (sizeof(g_ComponentAliases) / sizeof(g_ComponentAliases[0]))

#endif /* BG3SE_GENERATED_COMPONENT_ALIASES_H */
'''


def read_windows_pairs(gamedefs: Path) -> dict[str, str]:
    pairs: dict[str, str] = {}
    for root, _, files in os.walk(gamedefs):
        for name in sorted(files):
            path = Path(root) / name
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in DEFINE_COMPONENT.finditer(text):
                pairs[match.group(1)] = match.group(2)
    return pairs


def read_registry_names(registry: Path) -> set[str]:
    return set(REGISTRY_ROW.findall(registry.read_text(encoding="utf-8")))


def read_build_id(registry: Path) -> str:
    match = re.search(
        r"^ \* Build identity: (\S+)$", registry.read_text(encoding="utf-8"), re.M
    )
    if not match:
        raise SystemExit(f"no build identity in {registry}")
    return match.group(1)


def render(pairs: dict[str, str], names: set[str], build_id: str) -> str:
    rows = sorted((s, e) for s, e in pairs.items() if e in names)
    body = "".join(f'    {{ "{s}", "{e}" }},\n' for s, e in rows)
    return HEADER.format(build_id=build_id) + body + FOOTER.format()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--windows",
        required=True,
        type=Path,
        help="path to bg3se-windows/BG3Extender/GameDefinitions",
    )
    parser.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)

    if not args.windows.is_dir():
        parser.error(f"not a directory: {args.windows}")
    if not args.registry.is_file():
        parser.error(f"not a file: {args.registry}")

    pairs = read_windows_pairs(args.windows)
    names = read_registry_names(args.registry)
    kept = {s: e for s, e in pairs.items() if e in names}
    dropped = sorted(s for s in pairs if s not in kept)

    args.out.write_text(
        render(pairs, names, read_build_id(args.registry)), encoding="utf-8"
    )
    print(
        f"{len(pairs)} Windows pairs, {len(kept)} kept, {len(dropped)} dropped",
        file=sys.stderr,
    )
    for short in dropped:
        print(f"  dropped {short} -> {pairs[short]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
