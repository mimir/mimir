#!/usr/bin/env python3
"""Automate cutting a MimIR release: tag it, publish a GitHub release, bump to the next -dev version, and verify docs."""

import argparse
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE_FILE = ROOT / 'CMakeLists.txt'
PYPROJECT_FILE = ROOT / 'py' / 'pyproject.toml'
README_FILE = ROOT / 'docs' / 'README.md'
AUR_DIR = Path(os.environ.get('MIM_AUR_DIR', ROOT.parent / 'aur'))
AUR_PKGBUILD = AUR_DIR / 'PKGBUILD'
DOCS_SITE_VERSIONS_URL = 'https://mimir.github.io/versions.json'
DOXYGEN_WORKFLOW = 'doxygen.yml'
RUN_POLL_SECONDS = 10
RUN_POLL_ATTEMPTS = 30

VERSION_RE = re.compile(r'project\(MimIR VERSION (\d+)\.(\d+)\)')
SUFFIX_RE = re.compile(r'(set\(MIM_VER_SUFFIX ")([^"]*)("\s+CACHE)')
PYPROJECT_VERSION_RE = re.compile(r'(version = ")([^"]+)(")')
BADGE_RE = re.compile(r'(badge/Docs-)(.+?)(-blue)')
PKGVER_RE = re.compile(r'^pkgver=(.*)$', re.MULTILINE)
PKGREL_RE = re.compile(r'^pkgrel=.*$', re.MULTILINE)


def print_help():
    """Print help message."""
    help_text = """Usage: ./scripts/release.py <command> [options]

Automates the MimIR release process:
  status   Show the current version, suffix, and repo state.
  cut      Drop the -dev suffix, commit, tag, push, and publish a GitHub release.
  bump     Bump to the next minor version with a -dev suffix, commit, and push.
  verify   Poll the doxygen CI run and check that mimir.github.io/versions.json
           lists master and all released versions.
  aur      Wait for the release's source tarball, then bump the AUR package in
           $MIM_AUR_DIR (default: ../aur) to it, commit, and push.
  all      Run cut, bump, aur, then verify, pausing for confirmation in between.

Options:
  -y, --yes       Assume "yes" to all confirmation prompts.
  -n, --dry-run   Print the actions that would be taken without changing anything.
  -h, --help      Show this help text and exit.
"""
    print(help_text, file=sys.stderr)


def run(cmd, dry_run=False, cwd=ROOT, **kwargs):
    """Print and run a command; skip execution (but still print it) in dry-run mode."""
    print('+ ' + ' '.join(cmd))
    if dry_run:
        return None
    return subprocess.run(cmd, cwd=cwd, check=True, **kwargs)


def capture(cmd, cwd=ROOT):
    """Run a read-only command and return its stripped stdout."""
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


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


def read_pkgver():
    """Read pkgver out of the AUR PKGBUILD."""
    match = PKGVER_RE.search(AUR_PKGBUILD.read_text())
    if not match:
        sys.exit(f'error: could not find pkgver in {AUR_PKGBUILD}')
    return match.group(1)


def write_pkgbuild_version(version, dry_run=False):
    """Rewrite pkgver and reset pkgrel in the AUR PKGBUILD."""
    text = AUR_PKGBUILD.read_text()
    text = PKGVER_RE.sub(f'pkgver={version}', text, count=1)
    text = PKGREL_RE.sub('pkgrel=1', text, count=1)
    print(f'+ write {AUR_PKGBUILD}: pkgver={version} pkgrel=1')
    if not dry_run:
        AUR_PKGBUILD.write_text(text)


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


def poll(probe, what):
    """Return the first truthy result of `probe()`, retrying for a while, or None if there is none."""
    for attempt in range(1, RUN_POLL_ATTEMPTS + 1):
        if result := probe():
            return result
        print(f'waiting for {what} ({attempt}/{RUN_POLL_ATTEMPTS})...')
        time.sleep(RUN_POLL_SECONDS)
    return None


def find_doxygen_run(sha, ref_name):
    """Return the id of the doxygen run for `sha` whose head ref is `ref_name`, or '' if there is none yet."""
    jq = f'[.[] | select(.headBranch == "{ref_name}")] | first | .databaseId // empty'
    return capture(['gh', 'run', 'list', '--workflow', DOXYGEN_WORKFLOW, '--commit', sha, '--json', 'databaseId,headBranch', '--jq', jq])


def watch_doxygen_run(sha, ref_name, dry_run=False):
    """Wait for the doxygen run of `ref_name` to appear, watch it to completion, and report whether it succeeded."""
    if dry_run:
        print(f'+ watch {DOXYGEN_WORKFLOW} run for {ref_name} ({sha[:12]})')
        return True

    run_id = poll(lambda: find_doxygen_run(sha, ref_name), f'the doxygen run for {ref_name} to appear')
    if not run_id:
        print(f'warning: no doxygen run found for {ref_name} ({sha[:12]})')
        return False

    print(f'+ gh run watch {run_id} --exit-status')
    if subprocess.run(['gh', 'run', 'watch', run_id, '--exit-status'], cwd=ROOT, check=False).returncode != 0:
        print(f'warning: doxygen run {run_id} for {ref_name} did not succeed; inspect it with `gh run view {run_id}` and re-run it with `gh run rerun {run_id}`')
        return False
    return True


def released_version():
    """Return the latest release as "major.minor": the current version, or the previous minor while at -dev."""
    major, minor, suffix = read_version()
    return f'{major}.{minor - 1}' if suffix == '-dev' else f'{major}.{minor}'


def wait_for_release_asset(tag, name, dry_run=False):
    """Wait until the release workflow has uploaded `name` to the GitHub release `tag`."""
    if dry_run:
        print(f'+ wait for release asset {name} of {tag}')
        return True

    def uploaded():
        return name in capture(['gh', 'release', 'view', tag, '--json', 'assets', '--jq', '.assets[].name']).splitlines()

    if poll(uploaded, f'the release workflow to upload {name}'):
        return True
    print(f'warning: {name} is still missing from release {tag}; check the release workflow with `gh run list --workflow release.yml`')
    return False


def require_clean(cwd=ROOT):
    """Exit unless the working tree of `cwd` is clean."""
    if capture(['git', 'status', '--porcelain'], cwd=cwd):
        sys.exit(f'error: working tree of {cwd} is not clean; commit or stash changes first')


def preflight(require_dev):
    """Sanity-check repo state before mutating anything."""
    require_clean()

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

    # All deploying doxygen runs share one concurrency group and GitHub keeps at most one of them queued,
    # so pushing again before this one has finished would silently evict the docs snapshot for the tag.
    print(f'Waiting for the doxygen deploy of {tag} before anything else may push to master...')
    sha = capture(['git', 'rev-parse', 'HEAD'])
    if not watch_doxygen_run(sha, tag, args.dry_run):
        print(f'warning: the {tag} docs snapshot is probably missing from https://mimir.github.io; re-run the doxygen workflow for {tag} before bumping')

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
    """Poll the doxygen CI runs for master and the latest tag, then check the deployed versions manifest."""
    tag = f'v{released_version()}'

    sha = capture(['git', 'rev-parse', 'HEAD'])
    print(f'Watching doxygen workflow run(s) for {sha[:12]}...')
    run(['gh', 'run', 'list', '--workflow', DOXYGEN_WORKFLOW, '--commit', sha], args.dry_run)
    watch_doxygen_run(sha, 'master', args.dry_run)

    tag_sha = capture(['git', 'rev-list', '-n', '1', tag]) if capture(['git', 'tag', '-l', tag]) else ''
    if not tag_sha:
        print(f'warning: no local tag {tag}; skipping its doxygen run')
    else:
        watch_doxygen_run(tag_sha, tag, args.dry_run)

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
    if tag not in labels:
        print(f'warning: "{tag}" missing from deployed versions.json (CI may still be running)')


def cmd_aur(args):
    """Bump the AUR package to the latest release, commit, and push."""
    if not AUR_PKGBUILD.is_file():
        sys.exit(f'error: no PKGBUILD in {AUR_DIR}; clone ssh://aur@aur.archlinux.org/mim.git there or set MIM_AUR_DIR')
    require_clean(AUR_DIR)

    version = released_version()
    pkgver = read_pkgver()
    if pkgver == version:
        sys.exit(f'error: the AUR package is already at {version}')

    if not wait_for_release_asset(f'v{version}', f'mimir-{version}.tar.gz', args.dry_run):
        sys.exit('aborted; run `aur` again once the tarball is uploaded')

    print(f'Bumping the AUR package {pkgver} -> {version}...')
    write_pkgbuild_version(version, args.dry_run)
    run(['updpkgsums'], args.dry_run, cwd=AUR_DIR)
    print('+ makepkg --printsrcinfo > .SRCINFO')
    if not args.dry_run:
        (AUR_DIR / '.SRCINFO').write_text(capture(['makepkg', '--printsrcinfo'], cwd=AUR_DIR) + '\n')

    run(['git', 'add', 'PKGBUILD', '.SRCINFO'], args.dry_run, cwd=AUR_DIR)
    run(['git', 'diff', '--cached'], args.dry_run, cwd=AUR_DIR)

    if not confirm(f'Commit "mim {version}" in {AUR_DIR}?', args.yes):
        sys.exit('aborted')
    run(['git', 'commit', '-m', f'mim {version}'], args.dry_run, cwd=AUR_DIR)

    if not confirm('Push to the AUR?', args.yes):
        sys.exit('aborted before push; local commit is in place, push manually when ready')
    run(['git', 'push', 'origin', 'master'], args.dry_run, cwd=AUR_DIR)


def cmd_all(args):
    """Run cut, bump, aur, then verify, pausing for confirmation in between."""
    cmd_cut(args)
    if not confirm('Continue with bumping to the next -dev version?', args.yes):
        sys.exit('stopped after cut; run `bump`, `aur`, and `verify` manually when ready')
    cmd_bump(args)
    # The release workflow uploads the tarball for the AUR while bump runs.
    if confirm('Bump the AUR package to the new release?', args.yes):
        cmd_aur(args)
    cmd_verify(args)


COMMANDS = {
    'status': cmd_status,
    'cut': cmd_cut,
    'bump': cmd_bump,
    'verify': cmd_verify,
    'aur': cmd_aur,
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
