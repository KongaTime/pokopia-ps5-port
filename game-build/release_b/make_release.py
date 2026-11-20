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
  README.txt                   written here (README_TEXT), headed --title and the version
  ps5/art/                     --art: the home screen icon0.png / pic0.dds / pic1.dds the PS5 build puts in the title, with
                               licenses/NOTICE.artwork.txt (the pictures are the rights holders'; never in the repository)
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

README_TEXT = '''@TITLE@ @VERSION@

Pokemon Pokopia (version 2.0.1a), recompiled to run natively as a homebrew title on your PS5 (PPSA90006), and on this PC.
You need your own copy of the game: a decrypted dump folder (base/ and updates/<version>/, each with exefs/ and romfs/), or
your NSP / XCI files with your own keys. Nothing from the game is in this download: it is built from your dump, on your PC.

PS5
1. Your PS5 must run homebrew, with its FTP server started (port 2121).
2. Run PokopiaLauncher.exe and open the PS5 page.
3. Pick your dump folder, type your PS5's IP address (Settings, Network, Connection status) and press Install on PS5.
   The launcher builds and signs the title on this PC (10 to 30 minutes), makes the shader cache (1 to 2 hours) and uploads
   everything to the console (about 50 GB). You can pause and continue later; nothing finished is done twice.
4. Install PPSA90006 with your homebrew installer, start it, pick Prepare game once (it checks the files), then Play.

PC
1. Run PokopiaLauncher.exe, pick your dump folder on the Game files page and press Set up (it builds pkp.exe and the
   shader cache on this PC; you can pause and continue later).
2. Press Play.

Licences: LICENSE.txt and licenses/. Not affiliated with or endorsed by Nintendo, The Pokemon Company or Koei Tecmo.
'''

ART = ('icon0.png', 'pic0.dds', 'pic1.dds')
ART_NOTICE = '''The home screen pictures in ps5/art (icon0.png, pic0.dds, pic1.dds) are made from official artwork of Pokemon Pokopia.
That artwork belongs to its rights holders (Nintendo, Creatures Inc., GAME FREAK inc., The Pokemon Company); it is not
covered by this project's licence. Delete ps5/art before installing to use the plain icon instead. Rights holders: see
LEGAL.md of the project ("Rights holder contact").
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
    ap.add_argument('--package', help="the package name (default: the base package's)")
    ap.add_argument('--title', default='Pokopia PS5 Port', help='the heading of README.txt')
    ap.add_argument('--art', help='a folder with the home screen pictures to ship (icon0.png, pic0.dds, pic1.dds)')
    a = ap.parse_args()

    z = zipfile.ZipFile(a.base)
    top = z.namelist()[0].split('/')[0]                       # PokopiaRecomp-0.2.0
    m = re.match(r'^(.*)-([0-9][^-]*)$', top)
    if not m:
        fail('the base zip\'s folder %s is not <package>-<version>' % top)
    package, base_version = m.groups()
    package = a.package or package
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

    with open(os.path.join(dst, 'README.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(README_TEXT.replace('@TITLE@', a.title).replace('@VERSION@', a.version))

    if a.art:
        print('==> ps5/art')
        art = os.path.join(dst, 'ps5', 'art')
        os.makedirs(art)
        n = 0
        for f in ART:
            if os.path.isfile(os.path.join(a.art, f)):
                shutil.copy2(os.path.join(a.art, f), os.path.join(art, f))
                n += 1
        if not n:
            fail('%s has none of %s' % (a.art, ', '.join(ART)))
        with open(os.path.join(dst, 'licenses', 'NOTICE.artwork.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
            f.write(ART_NOTICE)

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
