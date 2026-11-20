# SPDX-License-Identifier: GPL-3.0-or-later
"""First-run build of the PS5 title (PPSA90006) on the player's Windows PC, from their own dump: the launcher's PS5 mode.

    python -I tools/build/ps5_build.py --exefs <dump>/updates/<v>/exefs --game <install folder> [--jobs N] [--keep]

The same steps as ps5/game/build.sh, with the toolchain in tools/ps5 (ps5/tools/bundle_windows.py makes it; its recipe.json holds the
Linux build's exact compile and link lines) instead of WSL:
  1. verify the ExeFS against the game manifest (game_build.verify: the supported 2.0.1a dump, by sha256),
  2. lift the four NSOs with the bundled lifter (game_build.lift, the PC build's own step) into <game>/ps5/build/lift,
  3. compile the lifted C++ for the console with the bundled clang (the recipe's -cc1 line) into <game>/ps5/build/obj, in parallel,
  4. archive each module (llvm-ar, files in name order, as CMake does) and link the title with ld.lld (the recipe's line) against the
     prebuilt runtime, RADV, SDL3 and libvpx,
  5. convert and sign it with ps5-native-tool (link, self --sign) and lay out the title folder <game>/ps5/PPSA90006:
     eboot.bin, sce_sys/param.json, sce_sys/icon0.png, sce_module/libc.prx, fonts/DejaVuSans.ttf,
  6. delete the generated sources and objects (unless --keep) and record the result in <game>/ps5/build/done.json.

Everything made here comes from the player's dump and stays on their PC (and, uploaded by the launcher, on their console).
The same dump and the same toolchain give the same eboot.bin as ps5/game/build.sh: done.json says whether it matches the release's
reference build (recipe.json reference_eboot_sha256).
Resumable and cancellable exactly as game_build.py (the file <game>/ps5/build/cancel stops it). Output lines: @progress / @error.
Exit codes: 0 done, 1 error, 2 cancelled.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time
from concurrent.futures import ThreadPoolExecutor, FIRST_COMPLETED, wait

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import game_build as gb                                   # noqa: E402  (verify, lift, Runner, progress: shared with the PC build)
from game_build import Fail, Cancelled, progress, say     # noqa: E402

TOOLS = os.path.dirname(HERE)                             # tools/
PS5 = os.path.join(TOOLS, 'ps5')
FORMAT = 1          # done.json format; the launcher's ps5.rs BUILD_FORMAT moves with it
RECIPE_FORMAT = 1   # bundle_windows.py FORMAT


def sha256(path):
    return gb.sha256(path)


def lifter_hash():
    """One hash over the bundled lifter (its Python sources, headers and game files): another lifter starts the build over."""
    import hashlib
    h = hashlib.sha256()
    for root in (os.path.join(gb.RECOMP, 'nxrecomp'), os.path.join(gb.RECOMP, 'include'), os.path.join(gb.RECOMP, 'games')):
        for dp, dns, fns in os.walk(root):
            dns[:] = sorted(d for d in dns if d != '__pycache__')
            for f in sorted(fns):
                if f.endswith(('.py', '.h', '.sites', '.toml', '.sig')):
                    p = os.path.join(dp, f)
                    h.update(os.path.relpath(p, gb.RECOMP).replace('\\', '/').encode())
                    h.update(open(p, 'rb').read())
    return h.hexdigest()


def load_recipe():
    p = os.path.join(PS5, 'recipe.json')
    if not os.path.isfile(p):
        raise Fail('The PS5 toolchain is missing (%s). Extract the release zip again so tools/ps5 is complete.' % p)
    r = json.load(open(p))
    if r.get('format') != RECIPE_FORMAT:
        raise Fail('tools/ps5/recipe.json has format %s; this build script reads %d. Extract the release zip again.' % (r.get('format'), RECIPE_FORMAT))
    missing = [x for x in ('llvm/bin/clang.exe', 'llvm/bin/ld.lld.exe', 'llvm/bin/llvm-ar.exe', 'bin/ps5-native-tool.exe', 'sdk/target/lib')
               if not os.path.exists(os.path.join(PS5, x))]
    if missing:
        raise Fail('The PS5 toolchain is incomplete, missing: %s. Extract the release zip again.' % ', '.join('tools/ps5/' + m for m in missing))
    return r


def fill(args, tokens):
    out = []
    for a in args:
        for k, v in tokens.items():
            a = a.replace('{%s}' % k, v)
        out.append(a)
    return out


def fwd(p):
    return os.path.abspath(p).replace('\\', '/')


def compile_all(runner, recipe, gen, obj, jobs):
    clang = os.path.join(PS5, 'llvm', 'bin', 'clang.exe')
    base = {'SDK': fwd(os.path.join(PS5, 'sdk')), 'RES': fwd(os.path.join(PS5, recipe['resource_dir'])), 'INC': fwd(os.path.join(gb.RECOMP, 'include'))}
    units = []
    for name in recipe['modules']:
        d = os.path.join(gen, name)
        for f in sorted(os.listdir(d)):
            if f.endswith('.cpp'):
                units.append((name, os.path.join(d, f)))
    total = len(units)
    todo = []
    for name, src in units:
        o = os.path.join(obj, name, os.path.basename(src) + '.o')
        if not os.path.isfile(o):
            todo.append((name, src, o))
    done = total - len(todo)
    progress('compile', done, total, 'Compiling for the PS5: %d of %d' % (done, total))
    for name in recipe['modules']:
        os.makedirs(os.path.join(obj, name), exist_ok=True)
    todo.sort(key=lambda t: -os.path.getsize(t[1]))     # biggest first

    def one(t):
        name, src, o = t
        tmp = o + '.tmp'
        tokens = dict(base, NAME=os.path.basename(src), MODDIR=fwd(os.path.dirname(src)), SRC=fwd(src), OUT=fwd(tmp))
        rc, tail = runner.run([clang] + fill(recipe['cc1'], tokens))
        if rc != 0:
            raise Fail('Compiling %s/%s for the PS5 failed (exit %d):\n%s' % (name, os.path.basename(src), rc, tail))
        os.replace(tmp, o)

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        pending = {ex.submit(one, t) for t in todo}
        try:
            while pending:
                fin, pending = wait(pending, return_when=FIRST_COMPLETED)
                for f in fin:
                    f.result()
                    done += 1
                    progress('compile', done, total, 'Compiling for the PS5: %d of %d' % (done, total))
        except BaseException:
            for f in pending:
                f.cancel()
            raise


def link(runner, recipe, obj, out_elf):
    progress('link', 0, 3, 'Linking the PS5 title')
    ar = os.path.join(PS5, 'llvm', 'bin', 'llvm-ar.exe')
    for name in recipe['modules']:
        a = os.path.join(obj, 'libpkp_%s.a' % name)
        if os.path.isfile(a):
            os.remove(a)
        objs = sorted(f for f in os.listdir(os.path.join(obj, name)) if f.endswith('.o'))
        rsp = os.path.join(obj, '%s.rsp' % name)
        with open(rsp, 'w') as f:
            f.write('\n'.join('"%s"' % fwd(os.path.join(obj, name, o)) for o in objs))
        rc, tail = runner.run([ar, 'qc', a, '@' + rsp])
        if rc != 0:
            raise Fail('Archiving %s failed (exit %d):\n%s' % (name, rc, tail))
    tokens = {'SDK': fwd(os.path.join(PS5, 'sdk')), 'RES': fwd(os.path.join(PS5, recipe['resource_dir'])), 'PRE': fwd(os.path.join(PS5, 'prebuilt')),
              'OBJ': fwd(obj), 'OUT': fwd(out_elf)}
    rsp = os.path.join(obj, 'link.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join('"%s"' % a for a in fill(recipe['ld'], tokens)))
    rc, tail = runner.run([os.path.join(PS5, 'llvm', 'bin', 'ld.lld.exe'), '@' + rsp], cwd=obj)
    if rc != 0:
        raise Fail('Linking the PS5 title failed (exit %d):\n%s' % (rc, tail))
    progress('link', 1, 3, 'Linked')


def sign(runner, recipe, elf, obj, title):
    tool = os.path.join(PS5, 'bin', 'ps5-native-tool.exe')
    tokens = {'SDK': fwd(os.path.join(PS5, 'sdk')), 'PRE': fwd(os.path.join(PS5, 'prebuilt'))}
    linked = os.path.join(obj, 'eboot.elf')
    rc, tail = runner.run([tool, 'link', '--in', elf, '--out', linked] + fill(recipe['tool_link'], tokens))
    if rc != 0:
        raise Fail('Converting the PS5 title failed (exit %d):\n%s' % (rc, tail))
    progress('link', 2, 3, 'Signing the PS5 title')
    tmp = title + '.tmp'
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    rc, tail = runner.run([tool, 'self', '--sign', '--in', linked, '--out', os.path.join(tmp, 'eboot.bin')] + recipe['tool_sign'])
    if rc != 0:
        raise Fail('Signing the PS5 title failed (exit %d):\n%s' % (rc, tail))
    for dst, src in recipe['title_files'].items():
        d = os.path.join(tmp, *dst.split('/'))
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(os.path.join(PS5, *src.split('/')), d)
    # the player's own pictures, if they put any in <game>/ps5/art (never in the package: see LEGAL.md): the home screen icon
    # (icon0.png, 512x512 PNG) and backdrops (pic0.dds when the title is selected, pic1.dds while it starts: 3840x2160 BC7
    # DDS, as PS5_Vulkan's tools/prepare-assets.sh writes them)
    art = os.path.join(os.path.dirname(title), 'art')
    for name in ('icon0.png', 'pic0.dds', 'pic1.dds'):
        if os.path.isfile(os.path.join(art, name)):
            shutil.copyfile(os.path.join(art, name), os.path.join(tmp, 'sce_sys', name))
    shutil.rmtree(title, ignore_errors=True)
    os.replace(tmp, title)
    progress('link', 3, 3, 'PS5 title signed')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--keep', action='store_true', help='keep the generated sources and objects')
    ap.add_argument('--manifest', default=os.path.join(TOOLS, '..', 'game.toml'), help='the game manifest (default: game.toml in the package root)')
    a = ap.parse_args()
    gb.load_manifest(a.manifest)
    root = os.path.join(os.path.abspath(a.game), 'ps5')
    build = os.path.join(root, 'build')
    os.makedirs(build, exist_ok=True)
    cancel_file = os.path.join(build, 'cancel')
    if os.path.exists(cancel_file):
        os.remove(cancel_file)
    t0 = time.time()
    times = {}
    try:
        recipe = load_recipe()
        title = os.path.join(root, recipe['title_id'])
        want = gb.verify(a.exefs)
        stamp = {'format': FORMAT, 'main_build_id': want['main_build_id'], 'sha256': want['sha256'],
                 'recipe': sha256(os.path.join(PS5, 'recipe.json')), 'lifter': lifter_hash()}
        state_p = os.path.join(build, 'state.json')
        old = json.load(open(state_p)) if os.path.isfile(state_p) else None
        if old != stamp:                 # another dump or another toolchain: start over
            for d in ('lift', 'obj'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
            if os.path.isfile(os.path.join(build, 'done.json')):
                os.remove(os.path.join(build, 'done.json'))
            json.dump(stamp, open(state_p, 'w'), indent=1)
        times['verify'] = round(time.time() - t0, 1)
        runner = gb.Runner(cancel_file)
        gen, obj = os.path.join(build, 'lift'), os.path.join(build, 'obj')
        t = time.time()
        gb.lift(runner, a.exefs, gen, a.jobs)
        times['lift'] = round(time.time() - t, 1)
        t = time.time()
        compile_all(runner, recipe, gen, obj, a.jobs)
        times['compile'] = round(time.time() - t, 1)
        t = time.time()
        elf = os.path.join(obj, 'pokopia_ps5')
        link(runner, recipe, obj, elf)
        sign(runner, recipe, elf, obj, title)
        times['link'] = round(time.time() - t, 1)
        # the console's file system reports a signed file's size as that of the ELF inside it: the launcher compares with this
        elf_size = os.path.getsize(os.path.join(obj, 'eboot.elf'))
        if not a.keep:
            progress('finish', 0, 1, 'Removing the build files')
            for d in ('lift', 'obj'):
                shutil.rmtree(os.path.join(build, d), ignore_errors=True)
        eboot = sha256(os.path.join(title, 'eboot.bin'))
        ref = recipe.get('reference_eboot_sha256', '')
        done = dict(stamp, eboot_sha256=eboot, eboot_elf_size=elf_size, matches_reference=(eboot == ref) if ref else None, seconds=times,
                    finished=time.strftime('%Y-%m-%d %H:%M:%S'))
        json.dump(done, open(os.path.join(build, 'done.json'), 'w'), indent=1)
        progress('finish', 1, 1, 'The PS5 title is ready (%d s)%s' % (time.time() - t0, ', identical to the reference build' if done['matches_reference'] else ''))
        return 0
    except Cancelled:
        say('cancelled', 'Build paused. Continue picks up where it stopped.')
        return 2
    except Fail as e:
        say('error', str(e).replace('\n', '\\n'))
        return 1
    except Exception as e:
        say('error', '%s: %s' % (type(e).__name__, str(e).replace('\n', '\\n')))
        return 1


if __name__ == '__main__':
    sys.exit(main())
