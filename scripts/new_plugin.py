#!/usr/bin/env python3
"""Create a new MimIR plugin from the demo plugin template."""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


def print_help():
    """Print help message."""
    help_text = """Usage: ./scripts/new_plugin.py <plugin_name> [--extra]

Create a new plugin derived from the demo plugin.

Arguments:
  <plugin_name>  Plugin name using only letters, digits, and underscores.
                 The name must not exceed 8 characters.

Options:
  --extra        Create a standalone third-party plugin repository in extra/.
  -h, --help     Show this help text and exit.
"""
    print(help_text, file=sys.stderr)


def validate_plugin_name(name: str) -> bool:
    """Validate plugin name."""
    if not re.match(r'^[A-Za-z0-9_]+$', name):
        print("Plugin names may only contain letters, digits, and underscores", file=sys.stderr)
        return False
    if len(name) > 8:
        print("Plugin names may not exceed 8 characters", file=sys.stderr)
        return False
    return True


def patch_workflow(workflow_file: Path, output_file: Path, plugin: str) -> None:
    """Patch a workflow file for the plugin."""
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


def create_extra_plugin(root: Path, plugin: str) -> None:
    """Create an out-of-tree plugin in extra/."""
    plugin_root = root / 'extra' / plugin
    demo_src = root / 'src/mim/plug/demo'
    demo_inc = root / 'include/mim/plug/demo'
    demo_lit = root / 'lit/demo'

    # Create directory structure
    (plugin_root / 'src').mkdir(parents=True, exist_ok=True)
    (plugin_root / 'include/mim/plug' / plugin).mkdir(parents=True, exist_ok=True)
    (plugin_root / 'lit').mkdir(parents=True, exist_ok=True)

    # Copy files
    shutil.copy(demo_src / 'demo.mim', plugin_root / f'{plugin}.mim')
    shutil.copy(demo_src / 'CMakeLists.txt', plugin_root / 'CMakeLists.txt')
    shutil.copy(demo_src / 'demo.cpp', plugin_root / 'src' / f'{plugin}.cpp')
    shutil.copy(demo_src / 'normalizers.cpp', plugin_root / 'src' / 'normalizers.cpp')
    shutil.copy(demo_inc / 'demo.h', plugin_root / 'include/mim/plug' / plugin / f'{plugin}.h')
    shutil.copy(demo_lit / 'const.mim', plugin_root / 'lit' / 'const.mim')

    # Replace demo -> plugin in all files
    for file_path in plugin_root.rglob('*'):
        if file_path.is_file():
            content = file_path.read_text()
            content = content.replace('demo', plugin)
            file_path.write_text(content)

    # Fix CMakeLists.txt structure
    cmake_file = plugin_root / 'CMakeLists.txt'
    content = cmake_file.read_text()

    # Add header at the beginning
    if not content.startswith('cmake_minimum_required'):
        content = f'cmake_minimum_required(VERSION 3.25 FATAL_ERROR)\nproject({plugin})\n\nif(NOT COMMAND add_mim_plugin)\n    find_package(mim REQUIRED)\nendif()\n\n' + content

    # Fix source paths
    content = re.sub(rf'^(\s*){plugin}\.cpp$', rf'\1src/{plugin}.cpp', content, flags=re.MULTILINE)
    content = re.sub(r'^(\s*)normalizers\.cpp$', r'\1src/normalizers.cpp', content, flags=re.MULTILINE)

    cmake_file.write_text(content)

    # Create workflow files
    for workflow_name in ['linux.yml', 'macos.yml', 'windows.yml']:
        source_workflow = root / '.github/workflows' / workflow_name
        if source_workflow.exists():
            output_workflow = plugin_root / '.github/workflows' / workflow_name
            patch_workflow(source_workflow, output_workflow, plugin)

    # Initialize git repo
    subprocess.run(['git', 'init'], cwd=plugin_root, check=True, capture_output=True)
    subprocess.run(['git', 'add', '.'], cwd=plugin_root, check=True, capture_output=True)
    subprocess.run(
        ['git', 'commit', '-m', f'Initial commit: {plugin} plugin'],
        cwd=plugin_root,
        check=True,
        capture_output=True
    )

    print(f"Created out-of-tree plugin repo in: {plugin_root}")


def create_intree_plugin(root: Path, plugin: str) -> None:
    """Create an in-tree plugin."""
    demo_src = root / 'src/mim/plug/demo'
    demo_inc = root / 'include/mim/plug/demo'
    demo_lit = root / 'lit/demo'

    # Create directory structure
    (root / 'include/mim/plug' / plugin).mkdir(parents=True, exist_ok=True)
    (root / 'src/mim/plug' / plugin).mkdir(parents=True, exist_ok=True)
    (root / 'lit' / plugin).mkdir(parents=True, exist_ok=True)

    # Copy and process files
    file_mappings = [
        (demo_src / 'demo.mim', root / 'src/mim/plug' / plugin / f'{plugin}.mim'),
        (demo_src / 'CMakeLists.txt', root / 'src/mim/plug' / plugin / 'CMakeLists.txt'),
        (demo_src / 'demo.cpp', root / 'src/mim/plug' / plugin / f'{plugin}.cpp'),
        (demo_src / 'normalizers.cpp', root / 'src/mim/plug' / plugin / 'normalizers.cpp'),
        (demo_inc / 'demo.h', root / 'include/mim/plug' / plugin / f'{plugin}.h'),
        (demo_lit / 'const.mim', root / 'lit' / plugin / 'const.mim'),
    ]

    for source, target in file_mappings:
        content = source.read_text()
        content = content.replace('demo', plugin)
        target.write_text(content)
        subprocess.run(['git', 'add', str(target)], cwd=root, check=True, capture_output=True)

    # Update main CMakeLists.txt
    cmake_file = root / 'src/mim/plug/CMakeLists.txt'
    content = cmake_file.read_text()

    # Extract plugins, add new one, sort
    match = re.search(r'set\(MIM_PLUGINS(.*?)\)', content, re.DOTALL)
    if match:
        plugin_block = match.group(1)
        plugins = [p.strip() for p in plugin_block.split('\n') if p.strip()]
        plugins.append(plugin)
        plugins = sorted(set(plugins))

        new_block = 'set(MIM_PLUGINS\n'
        for p in plugins:
            new_block += f'    {p}\n'
        new_block += ')'

        content = re.sub(r'set\(MIM_PLUGINS.*?\)', new_block, content, flags=re.DOTALL)
        cmake_file.write_text(content)
        subprocess.run(['git', 'add', str(cmake_file)], cwd=root, check=True, capture_output=True)


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description='Create a new MimIR plugin from the demo plugin template.',
        add_help=False
    )
    parser.add_argument('plugin_name', nargs='?', default=None, help='Plugin name')
    parser.add_argument('--extra', action='store_true', help='Create a standalone third-party plugin')
    parser.add_argument('-h', '--help', action='store_true', help='Show help message')

    args = parser.parse_args()

    if args.help or not args.plugin_name:
        print_help()
        sys.exit(1 if not args.help else 0)

    if not validate_plugin_name(args.plugin_name):
        print_help()
        sys.exit(1)

    root = Path(__file__).parent.parent.resolve()
    import os
    os.chdir(root)

    if args.extra:
        create_extra_plugin(root, args.plugin_name)
    else:
        create_intree_plugin(root, args.plugin_name)


if __name__ == '__main__':
    main()
