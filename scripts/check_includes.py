#!/usr/bin/env python3
"""Check (and optionally fix) the include style of MimIR sources.

Use `#include "..."` for headers that belong to the artifact you are currently building and `#include <...>` for everything you link against.
The artifact a file belongs to - and hence the prefix of its own headers - is derived from its path:

    src/mim/... include/mim/...                 libmim                  "mim/..."
    src/automaton/... include/automaton/...     libautomaton            "automaton/..."
    src/mim/plug/X/... include/mim/plug/X/...   plugin X                "mim/plug/X/..."
    extra/X/...                                 out-of-tree plugin X    "mim/plug/X/..."
    src/mim/cli/...                             the mim CLI             -
    gtest/...                                   the unit tests          -
    py/bindings/...                             the Python bindings     -

The last three link against `libmim` but do not ship headers of their own, so they use `<...>` throughout.
For a plugin `X`, this means:

    #include <absl/container/flat_hash_map.h>   // external dependency
    #include <mim/world.h>                      // libmim
    #include <mim/plug/mem/mem.h>               // another plugin
    #include "mim/plug/X/autogen.h"             // the plugin itself

Consequently, an in-tree plugin and an out-of-tree plugin spell their includes exactly the same way.

Only path-qualified includes are checked; a plain `#include "foo.h"` next to the including file is always fine.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROOTS = ['src', 'include', 'extra', 'gtest', 'py/bindings']
EXTS = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'}

INCLUDE = re.compile(r'^(?P<lead>\s*#\s*include\s*)(?P<open>["<])(?P<path>[^">]*)(?P<close>[">])')


def relative(path: Path) -> str:
    """Return `path` relative to the repository root - or as is if it lives outside of it."""
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def artifact_of(path: Path) -> tuple[str, str | None] | None:
    """Return the artifact `path` belongs to as a `(name, own)` pair.

    `own` is the include prefix of the artifact's own headers or `None` if the artifact does not ship any.
    Return `None` if `path` is not part of any artifact.
    """
    try:
        parts = path.resolve().relative_to(ROOT).parts
    except ValueError:
        return None

    match parts:
        case ('src' | 'include', 'mim', 'plug', plugin, *rest) if rest:
            return f'plugin "{plugin}"', f'mim/plug/{plugin}/'
        case ('extra', plugin, *rest) if rest:
            return f'plugin "{plugin}"', f'mim/plug/{plugin}/'
        case ('src', 'mim', 'cli', *rest) if rest:
            return 'the mim CLI', None
        case ('gtest', *rest) if rest:
            return 'the unit tests', None
        case ('py', 'bindings', *rest) if rest:
            return 'the Python bindings', None
        case ('src' | 'include', 'mim', *rest) if rest:
            return 'libmim', 'mim/'
        case ('src' | 'include', 'automaton', *rest) if rest:
            return 'libautomaton', 'automaton/'
        case _:
            return None


def check(path: Path, fix: bool) -> list[str]:
    """Check `path` and return all diagnostics; rewrite `path` in place if `fix` is set."""
    if not (artifact := artifact_of(path)):
        return []

    name, own = artifact
    diagnostics = []
    lines = path.read_text(encoding='utf-8').splitlines(keepends=True)
    dirty = False

    for i, line in enumerate(lines):
        if not (m := INCLUDE.match(line)):
            continue

        included = m.group('path')
        quoted   = m.group('open') == '"'
        mine     = own is not None and included.startswith(own)

        if quoted and '/' in included and not mine:
            old, new, why = f'"{included}"', f'<{included}>', f'is not part of {name}'
        elif not quoted and mine:
            old, new, why = f'<{included}>', f'"{included}"', f'is part of {name}'
        else:
            continue

        diagnostics.append(f'{relative(path)}:{i + 1}: {old} {why}; use {new}')

        if fix:
            lines[i] = line.replace(old, new, 1)
            dirty = True

    if dirty:
        path.write_text(''.join(lines), encoding='utf-8')

    return diagnostics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('files', nargs='*', type=Path, help='files to check; defaults to all sources')
    parser.add_argument('--fix', action='store_true', help='rewrite offending includes in place')
    args = parser.parse_args()

    files = args.files or sorted(f for r in ROOTS for f in ROOT.glob(f'{r}/**/*') if f.suffix in EXTS)
    diagnostics = [d for f in files if f.is_file() for d in check(f, args.fix)]

    for d in diagnostics:
        print(d, file=sys.stderr)

    if diagnostics and args.fix:
        print(f'fixed {len(diagnostics)} include(s); rerun clang-format to regroup them', file=sys.stderr)
        return 1
    if diagnostics:
        print(f'{len(diagnostics)} include(s) violate the include style; rerun with --fix', file=sys.stderr)
        return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
