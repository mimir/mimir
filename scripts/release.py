#!/usr/bin/env python3
"""Automate cutting a MimIR release: tag it, publish a GitHub release, bump to the next -dev version, and verify docs."""

import argparse
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE_FILE = ROOT / 'CMakeLists.txt'
PYPROJECT_FILE = ROOT / 'py' / 'pyproject.toml'
README_FILE = ROOT / 'docs' / 'README.md'
DOCS_SITE_VERSIONS_URL = 'https://mimir.github.io/versions.json'

VERSION_RE = re.compile(r'project\(MimIR VERSION (\d+)\.(\d+)\)')
SUFFIX_RE = re.compile(r'(set\(MIM_VER_SUFFIX ")([^"]*)("\s+CACHE)')
PYPROJECT_VERSION_RE = re.compile(r'(version = ")([^"]+)(")')
BADGE_RE = re.compile(r'(badge/Docs-)(.+?)(-blue)')


def print_help():
    """Print help message."""
    help_text = """Usage: ./scripts/release.py <command> [options]

Automates the MimIR release process:
  status   Show the current version, suffix, and repo state.
  cut      Drop the -dev suffix, commit, tag, push, and publish a GitHub release.
  bump     Bump to the next minor version with a -dev suffix, commit, and push.
  verify   Poll the doxygen CI run and check that docs.github.io/versions.json
           lists master and all released versions.
  all      Run cut, then bump, then verify, pausing for confirmation in between.

Options:
  -y, --yes       Assume "yes" to all confirmation prompts.
  -n, --dry-run   Print the actions that would be taken without changing anything.
  -h, --help      Show this help text and exit.
"""
    print(help_text, file=sys.stderr)


def run(cmd, dry_run=False, **kwargs):
    """Print and run a command; skip execution (but still print it) in dry-run mode."""
    print('+ ' + ' '.join(cmd))
    if dry_run:
        return None
    return subprocess.run(cmd, cwd=ROOT, check=True, **kwargs)


def capture(cmd):
    """Run a read-only command and return its stripped stdout."""
    return subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()


def confirm(prompt, assume_yes):
    """Ask the user to confirm an action, unless --yes was passed."""
    if assume_yes:
        return True
    reply = input(f'{prompt} [y/N] ').strip().lower()
    return reply == 'y'


def read_version():
    """Read (major, minor, suffix) out of the top-level CMakeLists.txt."""
    text = CMAKE_FILE.read_text()
    version_match = VERSION_RE.search(text)
    suffix_match = SUFFIX_RE.search(text)
    if not version_match or not suffix_match:
        sys.exit(f'error: could not find MimIR version/suffix in {CMAKE_FILE}')
    return int(version_match.group(1)), int(version_match.group(2)), suffix_match.group(2)


def write_version(major, minor, suffix, dry_run=False):
    """Rewrite the project version and MIM_VER_SUFFIX default in CMakeLists.txt."""
    text = CMAKE_FILE.read_text()
    text = VERSION_RE.sub(f'project(MimIR VERSION {major}.{minor})', text)
    text = SUFFIX_RE.sub(lambda m: f'{m.group(1)}{suffix}{m.group(3)}', text)
    print(f'+ write {CMAKE_FILE.relative_to(ROOT)}: version={major}.{minor} suffix="{suffix}"')
    if not dry_run:
        CMAKE_FILE.write_text(text)


def write_pyproject_version(version_str, dry_run=False):
    """Rewrite the version field in py/pyproject.toml."""
    text = PYPROJECT_FILE.read_text()
    text = PYPROJECT_VERSION_RE.sub(lambda m: f'{m.group(1)}{version_str}{m.group(3)}', text, count=1)
    print(f'+ write {PYPROJECT_FILE.relative_to(ROOT)}: version="{version_str}"')
    if not dry_run:
        PYPROJECT_FILE.write_text(text)


def update_docs_badge(new_label, dry_run=False):
    """Insert a new version label right after 'master' in the docs Docs-badge."""
    text = README_FILE.read_text()

    def repl(m):
        parts = m.group(2).split('/')
        if new_label not in parts:
            insert_at = 1 if parts and parts[0] == 'master' else 0
            parts.insert(insert_at, new_label)
        return m.group(1) + '/'.join(parts) + m.group(3)

    new_text, count = BADGE_RE.subn(repl, text, count=1)
    if count == 0:
        sys.exit(f'error: could not find Docs badge in {README_FILE}')
    print(f'+ write {README_FILE.relative_to(ROOT)}: Docs badge -> includes {new_label}')
    if not dry_run:
        README_FILE.write_text(new_text)


def preflight(require_dev):
    """Sanity-check repo state before mutating anything."""
    status = capture(['git', 'status', '--porcelain'])
    if status:
        sys.exit('error: working tree is not clean; commit or stash changes first')

    branch = capture(['git', 'branch', '--show-current'])
    if branch != 'master':
        sys.exit(f'error: expected to be on "master", currently on "{branch}"')

    run(['git', 'fetch', 'origin', 'master'])
    local_sha = capture(['git', 'rev-parse', 'HEAD'])
    remote_sha = capture(['git', 'rev-parse', 'origin/master'])
    if local_sha != remote_sha:
        sys.exit('error: local master is not in sync with origin/master')

    major, minor, suffix = read_version()
    if require_dev and suffix != '-dev':
        sys.exit(f'error: expected MIM_VER_SUFFIX to be "-dev", found "{suffix}"')
    return major, minor, suffix


def cmd_status(args):
    """Print the current version and repo state."""
    major, minor, suffix = read_version()
    branch = capture(['git', 'branch', '--show-current'])
    status = capture(['git', 'status', '--porcelain'])
    tags = capture(['git', 'tag', '-l', 'v*'])
    print(f'version:      {major}.{minor}{suffix}')
    print(f'branch:       {branch}')
    print(f'tree clean:   {"yes" if not status else "no"}')
    print(f'local tags:   {tags or "(none)"}')


def cmd_cut(args):
    """Drop the -dev suffix, commit, tag, push, and publish a GitHub release."""
    major, minor, suffix = preflight(require_dev=True)
    tag = f'v{major}.{minor}'

    if capture(['git', 'tag', '-l', tag]):
        sys.exit(f'error: tag {tag} already exists locally')
    if capture(['git', 'ls-remote', '--tags', 'origin', tag]):
        sys.exit(f'error: tag {tag} already exists on origin')

    print(f'Cutting release {tag} (dropping "-dev" suffix)...')
    write_version(major, minor, '', args.dry_run)
    write_pyproject_version(f'{major}.{minor}.0', args.dry_run)
    update_docs_badge(tag, args.dry_run)

    run(['git', 'add', str(CMAKE_FILE), str(PYPROJECT_FILE), str(README_FILE)], args.dry_run)
    run(['git', 'diff', '--cached'], args.dry_run)

    if not confirm(f'Commit and tag as "{tag}"?', args.yes):
        sys.exit('aborted')

    run(['git', 'commit', '-m', f'version {major}.{minor}'], args.dry_run)
    run(['git', 'tag', tag], args.dry_run)

    if not confirm(f'Push master + tag {tag} to origin?', args.yes):
        sys.exit(f'aborted before push; local commit/tag for {tag} are in place, push manually when ready')

    run(['git', 'push', '--atomic', 'origin', 'master', tag], args.dry_run)

    if not confirm(f'Publish a GitHub release for {tag} (via gh release create --generate-notes)?', args.yes):
        print(f'skipped GitHub release; run: gh release create {tag} --title "MimIR {major}.{minor}" --generate-notes')
        return

    run(['gh', 'release', 'create', tag, '--title', f'MimIR {major}.{minor}', '--generate-notes'], args.dry_run)
    print(f'Release {tag} published. Review/edit the auto-generated notes: gh release edit {tag} --notes-file -')


def cmd_bump(args):
    """Bump to the next minor version with a -dev suffix, commit, and push."""
    major, minor, suffix = preflight(require_dev=False)
    if suffix == '-dev':
        sys.exit(f'error: already at a -dev version ({major}.{minor}-dev); nothing to bump')

    next_minor = minor + 1
    print(f'Bumping {major}.{minor} -> {major}.{next_minor}-dev...')
    write_version(major, next_minor, '-dev', args.dry_run)
    write_pyproject_version(f'{major}.{next_minor}.0.dev0', args.dry_run)

    run(['git', 'add', str(CMAKE_FILE), str(PYPROJECT_FILE)], args.dry_run)
    run(['git', 'diff', '--cached'], args.dry_run)

    if not confirm(f'Commit "bump to {major}.{next_minor}-dev"?', args.yes):
        sys.exit('aborted')
    run(['git', 'commit', '-m', f'bump to {major}.{next_minor}-dev'], args.dry_run)

    if not confirm('Push master to origin?', args.yes):
        sys.exit('aborted before push; local commit is in place, push manually when ready')
    run(['git', 'push', 'origin', 'master'], args.dry_run)


def cmd_verify(args):
    """Poll the doxygen CI run for the current HEAD and check the deployed versions manifest."""
    sha = capture(['git', 'rev-parse', 'HEAD'])
    print(f'Watching doxygen workflow run(s) for {sha[:12]}...')
    run(['gh', 'run', 'list', '--workflow', 'doxygen.yml', '--commit', sha], args.dry_run)

    run_id = capture(['gh', 'run', 'list', '--workflow', 'doxygen.yml', '--commit', sha, '--json', 'databaseId', '--jq', '.[0].databaseId'])
    if not run_id:
        print('warning: no doxygen run found yet for this commit; it may not have started. Re-run `verify` shortly.')
    else:
        run(['gh', 'run', 'watch', run_id, '--exit-status'], args.dry_run)

    if args.dry_run:
        print(f'+ fetch {DOCS_SITE_VERSIONS_URL}')
        return

    with urllib.request.urlopen(DOCS_SITE_VERSIONS_URL) as response:
        import json
        versions = json.load(response)
    labels = [entry.get('label') for entry in versions]
    print(f'live docs versions: {labels}')
    if 'master' not in labels:
        print('warning: "master" missing from deployed versions.json')

    major, minor, _ = read_version()
    tag = f'v{major}.{minor}' if _ != '-dev' else f'v{major}.{minor - 1}'
    if tag not in labels:
        print(f'warning: "{tag}" missing from deployed versions.json (CI may still be running)')


def cmd_all(args):
    """Run cut, then bump, then verify, pausing for confirmation in between."""
    cmd_cut(args)
    if not confirm('Continue with bumping to the next -dev version?', args.yes):
        sys.exit('stopped after cut; run `bump` and `verify` manually when ready')
    cmd_bump(args)
    cmd_verify(args)


COMMANDS = {
    'status': cmd_status,
    'cut': cmd_cut,
    'bump': cmd_bump,
    'verify': cmd_verify,
    'all': cmd_all,
}


def main():
    """Program entry point."""
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('command', choices=COMMANDS.keys())
    parser.add_argument('-y', '--yes', action='store_true')
    parser.add_argument('-n', '--dry-run', action='store_true')
    parser.add_argument('-h', '--help', action='store_true')
    args = parser.parse_args()

    if args.help:
        print_help()
        return 0

    COMMANDS[args.command](args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
