# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the launcher's PS5 toolchain folder (tools/ps5 in the release): what a player's Windows PC needs to build and sign the
PS5 title (PPSA90006) from their own dump, with no WSL. Run by ps5/tools/bundle-windows.sh, on Linux / WSL, after
ps5/game/build.sh has built the title (that build tree is the source of the prebuilt libraries and of the recipe).

    python3 ps5/tools/bundle_windows.py --out <dir> --llvm <clang+llvm-23.1.1-x86_64-pc-windows-msvc dir> --zig <zig.exe>

The folder holds no game code and nothing made from the game:
  llvm/          clang.exe, ld.lld.exe, llvm-ar.exe and clang's own headers, from LLVM's official Windows release (the same LLVM
                 version as the Linux build, so the lifted code compiles to the same objects)
  sdk/           the PS5 payload SDK's sysroot (target/: headers, stub libraries, libc, libc++, the platform layer) and ldscripts/
  prebuilt/      the title's own code, built by ps5/game/build.sh: the runtime (libnxrt*.a), the graphics shell, the title objects,
                 SDL3, libvpx, RADV, the AGC and CommonDialog link stubs, clang's builtins, the link script and version script
  title/         param.json, icon0.png, libc.prx, the fallback font: the title folder around eboot.bin
  bin/           ps5-native-tool.exe (PS5_Vulkan's converter and signer, built here for Windows with Zig)
  recipe.json    the build's exact clang -cc1 line for a lifted file and its exact ld.lld line, with the paths as {TOKENS}
                 (game-build/release_b/ps5_build.py fills them in on the player's PC)
"""
import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PS5 = os.path.dirname(HERE)
ROOT = os.path.dirname(PS5)
FORMAT = 1          # recipe.json layout; ps5_build.py checks it

# ps5/game/build.sh's ps5-native-tool calls. check_build_sh() fails the bundle if build.sh no longer has these flags.
TOOL_LINK = ['--module-sdk', '0x02000009', '--companion-sdk', '0x08050001', '--file-name', 'eboot.elf']
TOOL_SIGN = ['--magic', '0x1D3D154F']
STUBS = ['libSceAgc.so', 'libSceAgcDriver.so', 'libSceCommonDialog.so']   # the --stub files, in build.sh's order


def fail(msg):
    sys.exit('error: ' + msg)


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        fail('%s failed:\n%s' % (' '.join(cmd[:3]), r.stdout[-4000:]))
    return r.stdout


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def check_build_sh():
    text = open(os.path.join(PS5, 'game', 'build.sh')).read()
    for flag in [' '.join(TOOL_LINK[i:i + 2]) for i in range(0, len(TOOL_LINK), 2)] + [' '.join(TOOL_SIGN)]:
        if flag not in text:
            fail('ps5/game/build.sh no longer passes "%s" to ps5-native-tool: update TOOL_LINK / TOOL_SIGN here' % flag)


def hash_lines(cmd, cwd):
    """The commands clang's driver runs for `cmd` (its -### output), each as an argument list."""
    out = run(['bash', '-c', cmd + ' -###'], cwd=cwd)
    return [shlex.split(l) for l in out.splitlines() if l.startswith(' "')]


class Paths:
    """Rewrites the Linux build's absolute paths into the recipe's tokens."""

    def __init__(self, sdk, res, lift, inc):
        self.prefixes = [(os.path.realpath(sdk), '{SDK}'), (sdk.rstrip('/'), '{SDK}'), (res, '{RES}'),
                         (os.path.realpath(inc), '{INC}'), (lift, '{LIFT}')]

    def sub(self, s):
        if s[:2] in ('-L', '-I') and len(s) > 2:
            return s[:2] + self.sub(s[2:])
        for p, tok in self.prefixes:
            s = s.replace(p + '/toolchain/..', p)
        for p, tok in self.prefixes:
            if s == p or s.startswith(p + '/'):
                return tok + s[len(p):]
            if '=' in s and s.split('=', 1)[1].startswith(p):
                k, v = s.split('=', 1)
                return k + '=' + tok + v[len(p):]
        return s


def compile_recipe(cmds, build, paths):
    line = next((l for l in cmds.splitlines() if '/main/c_000.cpp' in l and ' -c ' in l), None)
    if not line:
        fail('no compile line for a lifted file of main in the build (ps5/game/build.sh, then run this again)')
    cc1 = [a for a in hash_lines(line, build) if '-cc1' in a]
    if len(cc1) != 1:
        fail('unexpected driver output for the lifted compile')
    args, out = cc1[0][1:], []
    skip = {'-dependency-file', '-MT'}
    i = 0
    while i < len(args):
        a = args[i]
        if a in skip:
            i += 2
            continue
        if a == '-sys-header-deps':
            i += 1
            continue
        if a == '-main-file-name':
            out += [a, '{NAME}']
            i += 2
            continue
        if a == '-o':
            out += ['-o', '{OUT}']
            i += 2
            continue
        if a.startswith(('-fdebug-compilation-dir=', '-fcoverage-compilation-dir=')):
            out.append(a.split('=')[0] + '=.')       # no -g on lifted code: the folder is not in the object (checked)
            i += 1
            continue
        if a.endswith('/main/c_000.cpp'):
            out.append('{SRC}')
            i += 1
            continue
        if a.endswith('/main') and out and out[-1] == '-I':
            out.append('{MODDIR}')
            i += 1
            continue
        out.append(paths.sub(a))
        i += 1
    return out


def link_recipe(cmds, build, paths, sdk, pre, copies):
    line = cmds.splitlines()[-1]
    line = re.sub(r'^: && ', '', line)
    line = re.sub(r' && :$', '', line)
    if ' -o pokopia_ps5 ' not in line:
        fail('the last build command is not the title link')
    lld = [a for a in hash_lines(line, build) if a and a[0].endswith('prospero-lld')]
    if len(lld) != 1:
        fail('unexpected driver output for the title link')
    # what the SDK's bin/prospero-lld wrapper adds around the driver's arguments (its -T elf_x86_64.x is dropped: -T is given)
    wrapper = open(os.path.join(sdk, 'bin', 'prospero-lld')).read()
    for want in ('-m elf_x86_64 $PIE $LDSCRIPT --eh-frame-hdr', '-z max-page-size=0x4000 -mllvm -emulated-tls', '"$@" --hash-style=gnu'):
        if want not in wrapper:
            fail('the SDK\'s prospero-lld wrapper changed (%s): update link_recipe' % want)
    args = lld[0][1:]
    if '-T' not in args and '--script' not in args:
        fail('the title link has no -T: the wrapper\'s default script would apply')
    pie = [] if any(a in ('--shared', '-r', '--static') for a in args) else ['-pie']
    out = ['-m', 'elf_x86_64'] + pie + ['--eh-frame-hdr', '-z', 'max-page-size=0x4000', '-mllvm', '-emulated-tls']
    i = 0
    while i < len(args):
        a = args[i]
        if a == '-rpath':                  # the build tree's stub folders: not used on the console (the eboot is the same without it)
            i += 2
            continue
        if a == '-o':
            out += ['-o', '{OUT}']
            i += 2
            continue
        if a == '-L.':
            out.append('-L{PRE}')
            i += 1
            continue
        if a in ('-m', '-z', '--default-script', '-mllvm'):   # options with a separate value that is not a path of the build
            out += [a, args[i + 1]]
            i += 2
            continue
        if a == '-T':
            out.append(a)
            i += 1
            continue
        m = re.match(r'^libpkp_(\w+)\.a$', a)
        if m:
            out.append('{OBJ}/libpkp_%s.a' % m.group(1))
            i += 1
            continue
        s = paths.sub(a)
        if s != a:
            out.append(s)
            i += 1
            continue
        # a file of the build: copied into prebuilt/
        val, prefix = a, ''
        if a.startswith('--version-script='):
            prefix, val = '--version-script=', a.split('=', 1)[1]
        f = val if os.path.isabs(val) else os.path.join(build, val)
        if not a.startswith('-') or prefix:
            if not os.path.isfile(f):
                fail('link input %s is not a file' % f)
            name = os.path.basename(f)
            if name in copies and os.path.realpath(copies[name]) != os.path.realpath(f):
                fail('two link inputs are named %s' % name)
            copies[name] = f
            out.append(prefix + '{PRE}/' + name)
            i += 1
            continue
        out.append(a)
        i += 1
    out.append('--hash-style=gnu')
    for a in out:
        if a.startswith('/') or '/home/' in a or '/mnt/' in a:
            fail('the link recipe still names a path of this machine: %s' % a)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--llvm', required=True, help='the extracted clang+llvm-<v>-x86_64-pc-windows-msvc folder')
    ap.add_argument('--zig', required=True, help='zig.exe (builds ps5-native-tool.exe)')
    a = ap.parse_args()
    env = os.environ
    sdk, vulkan, work = env['PS5_SDK'], env['PS5_VULKAN_DIR'], env['PKP_BUILD']
    build = os.path.join(work, 'game')
    lift = env.get('PKP_LIFT') or os.path.join(work, 'game-lift')
    agc = env['PKP_AGC_STUBS']
    out = os.path.abspath(a.out)
    check_build_sh()
    if not os.path.isfile(os.path.join(build, 'pokopia_ps5')):
        fail('no title build in %s: run ps5/game/build.sh first' % build)

    # the Linux clang and the Windows LLVM must be the same release, or the lifted code would compile differently
    res = run([os.path.join(sdk, 'bin', 'prospero-clang'), '--print-resource-dir']).strip()
    lin_ver = run([os.path.join(sdk, 'bin', 'prospero-clang'), '--version']).split('\n')[0]
    llvm_bin = os.path.join(a.llvm, 'bin')
    win_ver = run([os.path.join(llvm_bin, 'clang.exe'), '--version']).split('\n')[0]
    ver = lambda s: re.search(r'clang version (\d+\.\d+\.\d+)', s).group(1)
    if ver(lin_ver) != ver(win_ver):
        fail('the build used clang %s but --llvm is %s' % (ver(lin_ver), ver(win_ver)))

    if os.path.isdir(out):
        shutil.rmtree(out)
    for d in ('llvm/bin', 'sdk', 'prebuilt', 'title', 'bin', 'licenses'):
        os.makedirs(os.path.join(out, d), exist_ok=True)

    print('==> recipe')
    cmds = run(['ninja', '-t', 'commands', 'pokopia_ps5'], cwd=build)
    paths = Paths(sdk, res, lift, os.path.join(ROOT, 'recomp', 'include'))
    copies = {}
    cc1 = compile_recipe(cmds, build, paths)
    lld = link_recipe(cmds, build, paths, sdk, os.path.join(out, 'prebuilt'), copies)
    for t in cc1:
        if t.startswith('/') or '/home/' in t or '/mnt/' in t:
            fail('the compile recipe still names a path of this machine: %s' % t)
    # the lifted modules as the CMake build archives them: one archive per module, its files in name order (file(GLOB) sorts)
    modules = [m.group(1) for m in re.finditer(r'\{OBJ\}/libpkp_(\w+)\.a', ' '.join(lld))]
    resver = os.path.basename(res)

    print('==> LLVM %s (Windows)' % ver(win_ver))
    for exe in ('clang.exe', 'ld.lld.exe', 'llvm-ar.exe'):
        shutil.copy2(os.path.join(llvm_bin, exe), os.path.join(out, 'llvm', 'bin', exe))
    shutil.copytree(os.path.join(a.llvm, 'lib', 'clang', resver, 'include'), os.path.join(out, 'llvm', 'lib', 'clang', resver, 'include'))
    # files of the Linux resource folder the link names ({RES}/lib/linux/libclang_rt.builtins-x86_64.a: compiler-rt, the same
    # LLVM release; the Windows release has no Linux runtimes)
    for t in lld:
        if t.startswith('{RES}/'):
            dst = os.path.join(out, 'llvm', 'lib', 'clang', resver, t[len('{RES}/'):])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(res, t[len('{RES}/'):]), dst)
    shutil.copy2(os.path.join(a.llvm, 'LICENSE.TXT') if os.path.isfile(os.path.join(a.llvm, 'LICENSE.TXT'))
                 else os.path.join(a.llvm, 'include', 'llvm', 'Support', 'LICENSE.TXT'), os.path.join(out, 'licenses', 'LICENSE.LLVM.txt'))

    print('==> PS5 payload SDK sysroot')
    # symlinks resolved: the player's PC is Windows
    shutil.copytree(os.path.join(sdk, 'target'), os.path.join(out, 'sdk', 'target'), symlinks=False)
    shutil.copytree(os.path.join(sdk, 'ldscripts'), os.path.join(out, 'sdk', 'ldscripts'), symlinks=False)
    for lic in ('LICENSE', 'COPYING'):
        for base in (sdk, os.path.dirname(sdk)):
            if os.path.isfile(os.path.join(base, lic)):
                shutil.copy2(os.path.join(base, lic), os.path.join(out, 'licenses', 'LICENSE.ps5-payload-sdk.txt'))

    print('==> prebuilt title code (%d files)' % len(copies))
    for name, f in sorted(copies.items()):
        shutil.copy2(f, os.path.join(out, 'prebuilt', name))
    # the stubs ps5-native-tool reads (--stub), next to the link's own copies
    for s in STUBS:
        src = os.path.join(agc if s.startswith('libSceAgc') else os.path.join(work, 'stubs'), s)
        shutil.copy2(src, os.path.join(out, 'prebuilt', s))

    print('==> title folder files')
    shutil.copy2(os.path.join(PS5, 'game', 'param.json'), os.path.join(out, 'title', 'param.json'))
    for icon in (os.path.join(sdk, 'samples/install_app/FAKE02932/icon0.png'), os.path.expanduser('~/ps5-sdk/samples/install_app/FAKE02932/icon0.png')):
        if os.path.isfile(icon):
            shutil.copy2(icon, os.path.join(out, 'title', 'icon0.png'))
            break
    shutil.copy2(os.path.join(vulkan, 'runtime', 'libc.prx'), os.path.join(out, 'title', 'libc.prx'))
    shutil.copy2(os.path.join(ROOT, 'runtime', 'assets', 'fonts', 'DejaVuSans.ttf'), os.path.join(out, 'title', 'DejaVuSans.ttf'))
    shutil.copy2(os.path.join(vulkan, 'LICENSE'), os.path.join(out, 'licenses', 'LICENSE.PS5_Vulkan.GPL-3.0.txt'))

    print('==> ps5-native-tool.exe')
    native = os.path.join(vulkan, 'tooling', 'native')
    zlib = os.path.join(vulkan, '.deps', 'native', 'zlib')
    zsrc = next((os.path.join(zlib, d) for d in sorted(os.listdir(zlib)) if d.startswith('zlib-') and os.path.isdir(os.path.join(zlib, d))), None)
    if not zsrc:
        fail('no zlib source in %s (PS5_Vulkan\'s tools/setup-native-dependencies.sh fetches it)' % zlib)
    win = lambda p: run(['wslpath', '-w', p]).strip()
    tmp = os.path.join(out, 'bin', 'obj')
    os.makedirs(tmp)
    objs = []
    for c in ('adler32', 'crc32', 'deflate', 'inflate', 'inftrees', 'inffast', 'trees', 'zutil', 'compress', 'uncompr'):
        o = os.path.join(tmp, c + '.o')
        run([a.zig, 'cc', '-target', 'x86_64-windows-gnu', '-O2', '-c', win(os.path.join(zsrc, c + '.c')), '-o', win(o)])
        objs.append(win(o))
    run([a.zig, 'c++', '-target', 'x86_64-windows-gnu', '-std=c++20', '-O2', '-I', win(zsrc)]
        + [win(os.path.join(native, f)) for f in ('native_app_builder.cpp', 'self_container.cpp', 'elf_object.cpp', 'sce_module_writer.cpp')]
        + objs + ['-o', win(os.path.join(out, 'bin', 'ps5-native-tool.exe'))])
    shutil.rmtree(tmp)
    shutil.copy2(os.path.join(zsrc, 'LICENSE'), os.path.join(out, 'licenses', 'LICENSE.zlib.txt'))

    revs = {}
    for name, d in (('pokopia', ROOT), ('ps5_vulkan', vulkan), ('ps5_sdk', sdk)):
        r = subprocess.run(['git', '-C', d, 'rev-parse', 'HEAD'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        revs[name] = r.stdout.strip() or 'unknown'
    recipe = {
        'format': FORMAT,
        'title_id': 'PPSA90006',
        'llvm': ver(win_ver),
        'resource_dir': 'llvm/lib/clang/' + resver,
        'modules': modules,
        'cc1': cc1,
        'ld': lld,
        'tool_link': ['--stub-dir', '{SDK}/target/lib'] + sum([['--stub', '{PRE}/' + s] for s in STUBS], []) + TOOL_LINK,
        'tool_sign': TOOL_SIGN,
        'title_files': {'sce_sys/param.json': 'title/param.json', 'sce_sys/icon0.png': 'title/icon0.png',
                        'sce_module/libc.prx': 'title/libc.prx', 'fonts/DejaVuSans.ttf': 'title/DejaVuSans.ttf'},
        'revisions': revs,
        'reference_eboot_sha256': sha256(os.path.join(ROOT, 'dist', 'PPSA90006', 'eboot.bin'))
        if os.path.isfile(os.path.join(ROOT, 'dist', 'PPSA90006', 'eboot.bin')) else '',
    }
    if not os.path.isfile(os.path.join(out, 'title', 'icon0.png')):
        del recipe['title_files']['sce_sys/icon0.png']
    with open(os.path.join(out, 'recipe.json'), 'w', newline='\n') as f:
        json.dump(recipe, f, indent=1)
    size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(out) for f in fs)
    print('==> %s: %.0f MB' % (out, size / 1e6))


if __name__ == '__main__':
    main()
