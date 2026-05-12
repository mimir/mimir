#!/usr/bin/env python3
"""Deploy generated documentation into the website repository."""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


VERSION_RE = re.compile(r'v[0-9][A-Za-z0-9.+-]*')


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    workspace = Path(os.environ.get('GITHUB_WORKSPACE', '.')).resolve()
    parser = argparse.ArgumentParser(description='Deploy generated docs to the site checkout.')
    parser.add_argument('--build-dir', type=Path, default=workspace / 'build' / 'html', help='Generated HTML docs directory')
    parser.add_argument('--site-dir', type=Path, default=workspace / 'site', help='Checked out site repository')
    parser.add_argument('--git-ref', default=os.environ.get('GITHUB_REF'), help='Git ref being deployed')
    parser.add_argument('--ref-name', default=os.environ.get('GITHUB_REF_NAME'), help='Short git ref name being deployed')
    return parser.parse_args()


def remove_path(path: Path) -> None:
    """Remove a file or directory."""
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()


def copy_children(source_dir: Path, target_dir: Path) -> None:
    """Copy all children from source_dir into target_dir."""
    for child in source_dir.iterdir():
        destination = target_dir / child.name
        if child.is_dir():
            shutil.copytree(child, destination)
        else:
            shutil.copy2(child, destination)


def deploy_root(build_dir: Path, site_dir: Path) -> None:
    """Deploy docs to the site root while preserving versioned subdirectories."""
    preserved_names = {'.git'}
    preserved_names.update(path.name for path in site_dir.iterdir() if path.is_dir() and VERSION_RE.fullmatch(path.name))

    for child in site_dir.iterdir():
        if child.name not in preserved_names:
            remove_path(child)

    copy_children(build_dir, site_dir)


def deploy_version(build_dir: Path, site_dir: Path, version: str) -> None:
    """Deploy docs to a versioned subdirectory."""
    target_dir = site_dir / version
    if target_dir.exists():
        remove_path(target_dir)
    target_dir.mkdir(parents=True, exist_ok=True)
    copy_children(build_dir, target_dir)


def natural_key(name: str) -> list[object]:
    """Split version names into comparable text and numeric chunks."""
    return [int(part) if part.isdigit() else part for part in re.findall(r'\d+|[^\d]+', name)]


def update_versions(site_dir: Path) -> None:
    """Update versions.json in the root docs and each versioned snapshot."""
    version_dirs = [
        path for path in site_dir.iterdir()
        if path.is_dir() and VERSION_RE.fullmatch(path.name) and (path / 'index.html').exists()
    ]

    versions: list[dict[str, str]] = []
    if (site_dir / 'index.html').exists():
        versions.append({'label': 'master', 'href': '/'})

    for path in sorted(version_dirs, key=lambda entry: natural_key(entry.name), reverse=True):
        versions.append({'label': path.name, 'href': f'/{path.name}/'})

    payload = json.dumps(versions, indent=2) + '\n'
    for output_dir in [site_dir, *version_dirs]:
        (output_dir / 'versions.json').write_text(payload)


def git(site_dir: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    """Run a git command in the site repository."""
    return subprocess.run(
        ['git', *args],
        cwd=site_dir,
        check=check,
        capture_output=True,
        text=True,
    )


def commit_and_push(site_dir: Path, label: str) -> None:
    """Commit site changes and push with a small rebase retry loop."""
    git(site_dir, 'add', '-A')
    if git(site_dir, 'diff', '--cached', '--quiet', check=False).returncode == 0:
        return

    git(site_dir, 'config', 'user.name', 'github-actions[bot]')
    git(site_dir, 'config', 'user.email', '41898282+github-actions[bot]@users.noreply.github.com')
    git(site_dir, 'commit', '-m', f'Deploy docs for {label}')

    for attempt in range(3):
        if git(site_dir, 'push', 'origin', 'master', check=False).returncode == 0:
            return
        git(site_dir, 'pull', '--rebase', 'origin', 'master')
        if attempt < 2:
            continue

    raise RuntimeError('failed to push deployed docs after 3 attempts')


def deploy(build_dir: Path, site_dir: Path, git_ref: str, ref_name: str) -> None:
    """Deploy docs to the correct location based on the ref."""
    if not build_dir.is_dir():
        raise FileNotFoundError(f'Build directory does not exist: {build_dir}')
    if not site_dir.is_dir():
        raise FileNotFoundError(f'Site directory does not exist: {site_dir}')
    if not git_ref:
        raise ValueError('Missing git ref')
    if not ref_name:
        raise ValueError('Missing ref name')

    if git_ref == 'refs/heads/master':
        deploy_root(build_dir, site_dir)
        label = 'master'
    else:
        deploy_version(build_dir, site_dir, ref_name)
        label = ref_name

    update_versions(site_dir)
    (site_dir / '.nojekyll').touch()
    commit_and_push(site_dir, label)


def main() -> int:
    """Program entry point."""
    args = parse_args()
    deploy(args.build_dir.resolve(), args.site_dir.resolve(), args.git_ref, args.ref_name)
    return 0


if __name__ == '__main__':
    sys.exit(main())
