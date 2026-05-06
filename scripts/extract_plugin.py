#!/usr/bin/env python3
"""Extract an existing in-tree plugin into extra/<plugin_name>."""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from plugin_utils import validate_plugin_name, patch_workflow, rewrite_sources_for_out_of_tree


def print_help():
    """Print help message."""
    help_text = """Usage: ./scripts/extract_plugin.py <plugin_name>

Extract an existing in-tree plugin into extra/<plugin_name>.

The script:
  - moves src/mim/plug/<plugin_name>/ into extra/<plugin_name>/
  - moves include/mim/plug/<plugin_name>/ into extra/<plugin_name>/include/...
  - moves lit/<plugin_name>/ into extra/<plugin_name>/lit/
  - removes <plugin_name> from src/mim/plug/CMakeLists.txt
  - creates GitHub workflow files for CI/CD
  - rewrites the extracted CMakeLists.txt for standalone find_package(mim) use

Arguments:
  <plugin_name>  Name of an existing in-tree plugin.

Options:
  -h, --help     Show this help text and exit.
"""
    print(help_text, file=sys.stderr)


def extract_plugin(root: Path, plugin: str) -> None:
    """Extract an in-tree plugin to extra/."""
    src_plugin_root = root / 'src/mim/plug' / plugin
    include_plugin_root = root / 'include/mim/plug' / plugin
    lit_plugin_root = root / 'lit' / plugin
    extra_plugin_root = root / 'extra' / plugin
    plug_cmake = root / 'src/mim/plug/CMakeLists.txt'

    # Validation
    if not src_plugin_root.is_dir():
        print(f"In-tree plugin source directory does not exist: {src_plugin_root}", file=sys.stderr)
        sys.exit(1)

    if not (src_plugin_root / 'CMakeLists.txt').is_file():
        print(f"In-tree plugin CMakeLists.txt does not exist: {src_plugin_root / 'CMakeLists.txt'}", file=sys.stderr)
        sys.exit(1)

    if not (src_plugin_root / f'{plugin}.mim').is_file():
        print(f"In-tree plugin description does not exist: {src_plugin_root / f'{plugin}.mim'}", file=sys.stderr)
        sys.exit(1)

    if extra_plugin_root.exists():
        print(f"Target directory already exists: {extra_plugin_root}", file=sys.stderr)
        sys.exit(1)

    # Check if plugin is in plug_cmake
    plug_cmake_content = plug_cmake.read_text()
    if f'  {plugin}\n' not in plug_cmake_content and f'\n  {plugin}\n' not in plug_cmake_content:
        # More lenient check - just look for the plugin name on its own line with whitespace
        import re
        if not re.search(rf'^\s+{plugin}\s*$', plug_cmake_content, re.MULTILINE):
            print(f"Plugin '{plugin}' is not listed in {plug_cmake}", file=sys.stderr)
            sys.exit(1)

    # Create directory structure
    (extra_plugin_root / 'src').mkdir(parents=True, exist_ok=True)
    (extra_plugin_root / 'include/mim/plug').mkdir(parents=True, exist_ok=True)

    # Move CMakeLists.txt and .mim file
    shutil.move(str(src_plugin_root / 'CMakeLists.txt'), str(extra_plugin_root / 'CMakeLists.txt'))
    shutil.move(str(src_plugin_root / f'{plugin}.mim'), str(extra_plugin_root / f'{plugin}.mim'))

    # Move all files from src_plugin_root to extra_plugin_root/src
    for path in src_plugin_root.iterdir():
        shutil.move(str(path), str(extra_plugin_root / 'src' / path.name))
    src_plugin_root.rmdir()

    # Move include directory if it exists
    if include_plugin_root.is_dir():
        shutil.move(str(include_plugin_root), str(extra_plugin_root / 'include/mim/plug' / plugin))

    # Move lit directory if it exists
    if lit_plugin_root.is_dir():
        (extra_plugin_root / 'lit').mkdir(parents=True, exist_ok=True)
        for path in lit_plugin_root.iterdir():
            shutil.move(str(path), str(extra_plugin_root / 'lit' / path.name))
        lit_plugin_root.rmdir()

    # Prepend find_package header to CMakeLists.txt
    cmake_path = extra_plugin_root / 'CMakeLists.txt'
    original_content = cmake_path.read_text()
    new_content = f"""cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
project({plugin})

if(NOT COMMAND add_mim_plugin)
    find_package(mim REQUIRED)
endif()

"""
    cmake_path.write_text(new_content + original_content)

    # Rewrite source paths
    rewrite_sources_for_out_of_tree(cmake_path)

    # Create GitHub workflows
    for workflow_name in ['linux.yml', 'macos.yml', 'windows.yml']:
        source_workflow = root / '.github/workflows' / workflow_name
        if source_workflow.exists():
            output_workflow = extra_plugin_root / '.github/workflows' / workflow_name
            patch_workflow(source_workflow, output_workflow, plugin)

    # Remove plugin from src/mim/plug/CMakeLists.txt
    import re
    plug_cmake_content = plug_cmake.read_text()
    plug_cmake_content = re.sub(rf'^\s+{plugin}\s*$', '', plug_cmake_content, flags=re.MULTILINE)
    plug_cmake.write_text(plug_cmake_content)

    # Git staging
    try:
        subprocess.run(['git', '-C', str(root), 'add', '-f', str(extra_plugin_root)], check=True, capture_output=True)
        subprocess.run(['git', '-C', str(root), 'add', str(plug_cmake)], check=True, capture_output=True)
        subprocess.run(['git', '-C', str(root), 'add', '-u', str(root / 'src/mim/plug'), str(root / 'include/mim/plug'), str(root / 'lit')], check=True, capture_output=True)
    except subprocess.CalledProcessError:
        pass

    print(f"Extracted in-tree plugin '{plugin}' to '{extra_plugin_root}'")


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description='Extract an existing in-tree plugin into extra/<plugin_name>.',
        add_help=False
    )
    parser.add_argument('plugin_name', nargs='?', default=None, help='Plugin name')
    parser.add_argument('-h', '--help', action='store_true', help='Show help message')

    args = parser.parse_args()

    if args.help or not args.plugin_name:
        print_help()
        sys.exit(0 if args.help else 1)

    if not validate_plugin_name(args.plugin_name):
        print_help()
        sys.exit(1)

    root = Path(__file__).parent.parent.resolve()
    import os
    os.chdir(root)
    extract_plugin(root, args.plugin_name)


if __name__ == '__main__':
    main()
