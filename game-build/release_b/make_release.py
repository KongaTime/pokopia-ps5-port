# SPDX-License-Identifier: GPL-3.0-or-later
"""A release package with PS5 mode, made from a published model (b) package (the zip of a release) and this repository.

    python game-build/release_b/make_release.py --base PokopiaRecomp-0.2.0-win64.zip --version 0.3.0
        --launcher launcher/src-tauri/target/release/recomp-launcher.exe --ps5 <tools/ps5 folder> --out <dir>

What changes against the base package, and nothing else (its pkp_rt.dll, sdk/, Zig, Python, shader tools, assets and licences
stay as they are, so this works only while the runtime and its API, ctx.h's NXRECOMP_ABI_VERSION, are the base package's):
  PokopiaLauncher.exe          the launcher built from this repository (PS5 page)
  tools/build/game_build.py    from game-build/release_b
  tools/build/ps5_build.py     from game-build/release_b
  tools/build/recomp/          LICENSE, games/, include/, nxrecomp/ from recomp/ (the lifter)
  tools/ps5/                   the PS5 toolchain (ps5/tools/bundle-windows.sh)
  licenses/BUNDLED_TOOLS.txt   from game-build/release_b
  README.txt                   the base's, with the version and a PS5 section
Writes <out>/<package>-<version>/ and <out>/<package>-<version>-win64.zip. The package holds no game code or data.
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

PS5_README = '''
PS5 (optional): the launcher's PS5 page puts the game on your own PS5 as the homebrew title PPSA90006. You need a PS5 that runs
homebrew and its FTP server (port 2121). Pick the same dump folder, type the console's IP address and press Install on PS5: the
launcher builds and signs the title from your dump on this PC, makes the shader cache and uploads everything (about 50 GB, so the
first install takes hours; you can pause and continue). Then start PPSA90006 on the console (install it once with your homebrew
installer), pick Prepare game, then Play.
'''


def fail(msg):
    sys.exit('error: ' + msg)


def abi(path):
    m = re.search(r'#define NXRECOMP_ABI_VERSION (\d+)', open(path, encoding='utf-8').read())
    return int(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', required=True, help='the published package zip this one builds on')
    ap.add_argument('--version', required=True)
    ap.add_argument('--launcher', required=True, help='the launcher exe built from this repository (npm run tauri build)')
    ap.add_argument('--ps5', required=True, help='the tools/ps5 folder (ps5/tools/bundle-windows.sh)')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    z = zipfile.ZipFile(a.base)
    top = z.namelist()[0].split('/')[0]                       # PokopiaRecomp-0.2.0
    m = re.match(r'^(.*)-([0-9][^-]*)$', top)
    if not m:
        fail('the base zip\'s folder %s is not <package>-<version>' % top)
    package, base_version = m.groups()
    name = '%s-%s' % (package, a.version)
    dst = os.path.join(os.path.abspath(a.out), name)
    if os.path.exists(dst):
        shutil.rmtree(dst)
    os.makedirs(a.out, exist_ok=True)
    print('==> %s from %s' % (name, top))
    tmp = os.path.join(os.path.abspath(a.out), '.extract')
    shutil.rmtree(tmp, ignore_errors=True)
    z.extractall(tmp)
    os.replace(os.path.join(tmp, top), dst)
    shutil.rmtree(tmp)

    for need in ('pkp_rt.dll', 'tools/build/game_build.py', 'tools/build/recomp/include/nxrecomp/ctx.h', 'README.txt'):
        if not os.path.exists(os.path.join(dst, need)):
            fail('the base package has no %s: not a model (b) release' % need)
    base_abi, our_abi = abi(os.path.join(dst, 'tools/build/recomp/include/nxrecomp/ctx.h')), abi(os.path.join(ROOT, 'recomp/include/nxrecomp/ctx.h'))
    if base_abi != our_abi:
        fail('the base package\'s runtime is ABI %s, this repository\'s lifter ABI %s: build a new runtime instead' % (base_abi, our_abi))
    if not os.path.isfile(os.path.join(a.ps5, 'recipe.json')):
        fail('%s is not a tools/ps5 folder (no recipe.json)' % a.ps5)

    print('==> launcher, build scripts, lifter')
    shutil.copy2(a.launcher, os.path.join(dst, 'PokopiaLauncher.exe'))
    for f in ('game_build.py', 'ps5_build.py'):
        shutil.copy2(os.path.join(HERE, f), os.path.join(dst, 'tools', 'build', f))
    rc = os.path.join(dst, 'tools', 'build', 'recomp')
    shutil.rmtree(rc)
    os.makedirs(rc)
    shutil.copy2(os.path.join(ROOT, 'recomp', 'LICENSE'), rc)
    ignore = shutil.ignore_patterns('__pycache__', '*.pyc')
    for d in ('games', 'include', 'nxrecomp'):
        shutil.copytree(os.path.join(ROOT, 'recomp', d), os.path.join(rc, d), ignore=ignore)
    shutil.copy2(os.path.join(HERE, 'BUNDLED_TOOLS.txt'), os.path.join(dst, 'licenses', 'BUNDLED_TOOLS.txt'))

    print('==> tools/ps5')
    shutil.copytree(a.ps5, os.path.join(dst, 'tools', 'ps5'), ignore=ignore)

    readme = open(os.path.join(dst, 'README.txt'), encoding='utf-8').read()
    readme = readme.replace(base_version, a.version, 1)
    if 'PPSA90006' not in readme:
        readme = readme.replace('\nLicences:', PS5_README + '\nLicences:', 1)
    with open(os.path.join(dst, 'README.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(readme.replace('\r\n', '\n'))

    print('==> zip')
    out_zip = os.path.join(os.path.abspath(a.out), name + '-win64.zip')
    if os.path.exists(out_zip):
        os.remove(out_zip)
    n = 0
    with zipfile.ZipFile(out_zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zo:
        for dp, dns, fns in os.walk(dst):
            dns.sort()
            rel = os.path.relpath(dp, os.path.dirname(dst)).replace('\\', '/')
            zo.write(dp, rel + '/')
            for f in sorted(fns):
                zo.write(os.path.join(dp, f), rel + '/' + f)
                n += 1
    h = hashlib.sha256(open(out_zip, 'rb').read()).hexdigest()
    with open(out_zip + '.sha256', 'w', newline='\n') as f:
        f.write('%s  %s\n' % (h, os.path.basename(out_zip)))
    print('    %s: %d files, %.0f MB, sha256 %s' % (out_zip, n, os.path.getsize(out_zip) / 1e6, h))


if __name__ == '__main__':
    main()
