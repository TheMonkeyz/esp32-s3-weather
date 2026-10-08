#!/usr/bin/env python3
"""espforge's components at the tag the firmware uses, for builds outside idf.py (host tests, the emulator, CI jobs,
the cloud recipe). The tag is the one in main/idf_component.yml (forge_core's `version:`); the clone goes to
.espforge/ (git-ignored), or to --dest. Prints the components folder: pass it as FORGE= to the Makefiles.

    python tools/fetch_forge.py                  # .espforge/components at the pinned tag
    make -C tests/host FORGE=$(python tools/fetch_forge.py)

An idf.py build puts the same components in managed_components/, which the Makefiles use first.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = 'https://github.com/TheMonkeyz/espforge.git'


def pinned_tag():
    """forge_core's version in main/idf_component.yml (all five espforge entries share it)."""
    text = open(os.path.join(ROOT, 'main', 'idf_component.yml'), encoding='utf-8').read()
    m = re.search(r'^\s*forge_core:\s*\n(?:\s+\w+:.*\n)*?\s+version:\s*"?([^"\s#]+)', text, re.M)
    if not m:
        sys.exit('no forge_core version in main/idf_component.yml')
    return m.group(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dest', default=os.path.join(ROOT, '.espforge'), help='where to clone (default .espforge)')
    ap.add_argument('--tag', help='another tag or branch (default: the pinned one)')
    opts = ap.parse_args()
    tag = opts.tag or pinned_tag()
    stamp = os.path.join(opts.dest, '.forge_tag')
    have = open(stamp).read().strip() if os.path.exists(stamp) else None
    if have != tag:
        if os.path.exists(opts.dest):
            subprocess.run(['git', '-C', opts.dest, 'fetch', '-q', '--depth', '1', 'origin', 'tag', tag], check=True)
            subprocess.run(['git', '-C', opts.dest, 'checkout', '-q', tag], check=True)
        else:
            subprocess.run(['git', '-c', 'advice.detachedHead=false', 'clone', '-q', '--depth', '1', '--branch', tag, REPO, opts.dest], check=True)
        open(stamp, 'w').write(tag)
        print(f'espforge {tag} in {opts.dest}', file=sys.stderr)
    print(os.path.join(opts.dest, 'components').replace('\\', '/'))


if __name__ == '__main__':
    main()
