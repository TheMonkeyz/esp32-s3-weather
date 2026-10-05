#!/usr/bin/env python3
"""Build the display against an espforge checkout that isn't released yet, to test an espforge change with this app
before tagging espforge. Nothing is committed: for the length of the build, main/idf_component.yml's four espforge
entries become `override_path:` entries pointing at the checkout (the only override ESP-IDF's component manager honours
over a git dependency: -DEXTRA_COMPONENT_DIRS lost to the managed copy, October 5), then the file is put back exactly
as it was, even if the build fails. The build goes to its own folder (build/forge) with its own sdkconfig. Label the
build (version.txt) as a test, e.g. v1.14.0-forge.N.

    python tools/forge_local.py                          # ../espforge, then flash build/forge with the harness:
    python tools/harness/harness.py --flash build/forge/weather_amoled.bin
    python tools/forge_local.py --espforge D:/src/espforge --idf C:/Espressif/esp-idf

Run it from a shell where ESP-IDF is set up (export.ps1 / export.sh), or pass --idf. When the espforge change is
good: tag espforge (rc), bump the four tags in main/idf_component.yml, and build normally (managed_components then
holds the tagged copies again).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, 'main', 'idf_component.yml')
COMPONENTS = ('forge_core', 'forge_net', 'forge_ota', 'dns_server')


def local_manifest(text, espforge):
    """The manifest with each espforge entry (git / path / version lines) replaced by override_path."""
    for c in COMPONENTS:
        block = re.compile(r'^(  ' + c + r':\n)((?:    \w+:.*\n)+)', re.M)
        path = os.path.join(espforge, 'components', c).replace('\\', '/')
        text, n = block.subn(lambda m: m.group(1) + f'    override_path: "{path}"   # tools/forge_local.py (temporary)\n',
                             text)
        if n != 1:
            sys.exit(f'{c} not found in main/idf_component.yml')
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--espforge', default=os.path.join(os.path.dirname(ROOT), 'espforge'),
                    help='the espforge checkout (default: next to this project)')
    ap.add_argument('--build', default='build/forge', help='build folder (default build/forge)')
    ap.add_argument('--idf', help='ESP-IDF folder, when idf.py is not on the PATH')
    ap.add_argument('target', nargs='*', default=['build'], help='idf.py targets (default: build)')
    opts = ap.parse_args()
    espforge = os.path.abspath(opts.espforge)
    for c in COMPONENTS:
        if not os.path.exists(os.path.join(espforge, 'components', c, 'CMakeLists.txt')):
            sys.exit(f'not an espforge checkout: {espforge}/components/{c} has no CMakeLists.txt')
    head = subprocess.run(['git', '-C', espforge, 'describe', '--tags', '--always', '--dirty'],
                          capture_output=True, text=True).stdout.strip()
    print(f'building against espforge {head or "?"} at {espforge}', flush=True)
    idf_path = opts.idf or os.environ.get('IDF_PATH')    # export.ps1 / export.sh set it
    idf = os.path.join(idf_path, 'tools', 'idf.py') if idf_path else shutil.which('idf.py')
    if not idf:
        sys.exit('idf.py not found: run export.ps1 / export.sh first, or pass --idf')
    cmd = [sys.executable, idf] if idf.endswith('.py') else [idf]
    cmd += ['-B', opts.build, '-D', 'SDKCONFIG=' + os.path.join(opts.build, 'sdkconfig')] + opts.target

    backup = MANIFEST + '.forge_local'
    if os.path.exists(backup):                     # a run that was killed (Ctrl-C twice, a pipe closed early)
        shutil.move(backup, MANIFEST)
        print('main/idf_component.yml restored from the last run that was cut short', flush=True)
    original = open(MANIFEST, encoding='utf-8', newline='').read()
    open(backup, 'w', encoding='utf-8', newline='').write(original)   # if this script is killed: copy it back
    try:
        open(MANIFEST, 'w', encoding='utf-8', newline='').write(local_manifest(original, espforge))
        code = subprocess.run(cmd, cwd=ROOT).returncode
    finally:
        open(MANIFEST, 'w', encoding='utf-8', newline='').write(original)
        os.remove(backup)
    sys.exit(code)


if __name__ == '__main__':
    main()
