"""Common utilities for plugin management scripts."""

import re
import sys
from pathlib import Path


def validate_plugin_name(name: str) -> bool:
    """Validate plugin name.

    Plugin names must:
    - Contain only letters, digits, and underscores
    - Not exceed 8 characters
    """
    if not re.match(r'^[A-Za-z0-9_]+$', name):
        print("Plugin names may only contain letters, digits, and underscores", file=sys.stderr)
        return False
    if len(name) > 8:
        print("Plugin names may not exceed 8 characters", file=sys.stderr)
        return False
    return True


def patch_workflow(workflow_file: Path, output_file: Path, plugin: str) -> None:
    """Patch a workflow file for the plugin.

    Adjusts paths and checkout steps for out-of-tree plugin builds.
    """
    content = workflow_file.read_text()

    # Replace the "Clone recursively" checkout step to clone mimir/mimir
    content = re.sub(
        r'(      - name: Clone (?:mimir )?recursively\n        uses: actions/checkout@v4)\n        with:\n          submodules: recursive',
        r'\1\n        with:\n          repository: mimir/mimir\n          path: mimir\n          submodules: recursive',
        content
    )

    # Add the plugin clone step after the mimir clone
    content = re.sub(
        r'(      - name: Clone (?:mimir )?recursively\n        uses: actions/checkout@v4\n        with:\n          repository: mimir/mimir\n          path: mimir\n          submodules: recursive)',
        lambda m: m.group(1) + f'\n\n      - name: Clone {plugin} plugin\n        uses: actions/checkout@v4\n        with:\n          path: mimir/extra/{plugin}',
        content
    )

    # Fix working directories and cmake paths
    content = content.replace('${{github.workspace}}/build', 'mimir/build')
    content = content.replace('${{github.workspace}}', 'mimir')
    content = re.sub(r'-B mimir(?!/)', '-B build', content)
    content = content.replace('cmake -B build/build', 'cmake -B build')

    output_file.parent.mkdir(parents=True, exist_ok=True)
    output_file.write_text(content)


def rewrite_sources_for_out_of_tree(cmake_file: Path) -> None:
    """Rewrite CMakeLists.txt source paths for out-of-tree builds.

    Adds 'src/' prefix to source file paths that don't already have it.
    """
    lines = cmake_file.read_text().splitlines(keepends=True)
    in_sources = False
    result = []

    for line in lines:
        stripped = line.strip()

        if in_sources and (stripped == ')' or re.match(r'^[A-Z_][A-Z0-9_]*$', stripped)):
            in_sources = False

        if stripped == 'SOURCES':
            in_sources = True
            result.append(line)
            continue

        if (in_sources and
            re.match(r'^[^#][^\s]*\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$', stripped) and
            not stripped.startswith('src/')):
            indent = len(line) - len(line.lstrip())
            result.append(' ' * indent + 'src/' + stripped + '\n')
            continue

        result.append(line)

    cmake_file.write_text(''.join(result))
