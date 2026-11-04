# SPDX-License-Identifier: GPL-3.0-or-later
"""P1 test cases: the runtime's lifted test programs (runtime/tests/lift), prepared for one PS5 title that runs them all.

The Windows test build (runtime/tests/lift/CMakeLists.txt) makes one executable per program, each loading its program at
0x7100000000. A title is one process, so here every program is linked at a base of its own (0x70_0000_0000 + n * 64 MiB,
a range no runtime region and no Unicorn harness region uses), lifted with its own module name and index, and listed in a
table the title walks. Per case this does what the CMake build does:

  guest ELF     recomp/oracle/e2e/build_prog.compile_cmd (clang --target=aarch64), EXPECT-INSN checked with llvm-objdump
  expected      Unicorn on the ELF (tools/unicorn_expect.py) for the S1 programs; for the exception programs, the program
                compiled for this host with native/rt2.h and run (its C++ exceptions are the reference)
  sites         tools/make_sites.py (entry hooks on the guest runtime's stubs)
  lifted C++    python -m nxrecomp lift --kind static_elf --name tNN --index NN --base <its base>

Runs on Linux (the archlinux WSL distro): the upstream tools call `<LLVM_BIN>/clang.exe` and the like, so LLVM_BIN is pointed
at a folder of links with those names to the distro's LLVM.

    make_cases.py <out-dir>          writes <out-dir>/{elf,expected,gen,sites}/ and <out-dir>/cases.inc
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
RECOMP = os.path.join(ROOT, 'recomp')
TESTS = os.path.join(ROOT, 'runtime', 'tests')
LIFT = os.path.join(TESTS, 'lift')
TOOLS = os.path.join(TESTS, 'tools')
S1 = os.path.join(RECOMP, 'oracle', 'e2e', 'progs')
PR = os.path.join(LIFT, 'progs')

BASE0 = 0x7000000000
STEP = 0x4000000      # 64 MiB: 21 programs end below 0x7060000000, clear of 0x7100000000 and the harness addresses above it

# (name, source, expected-from, options) in the order the CMake build lists them. Left out: 24_eh_term_uncaught and
# 25_eh_term_noexcept end the process in std::terminate (the Windows build runs them in child processes), and the
# ABI-mismatch and child-exit variants, which need a process per run.
CASES = [(p.rsplit('.', 1)[0], os.path.join(S1, p), 'unicorn', {}) for p in (
    '00_smoke.c', '01_int.c', '02_switch.c', '03_fp.c', '04_neon_int.c', '05_neon_fp_mem.c', '06_crypto_crc.c', '07_lse.c',
    '08_excl.c', '10_virtual.cpp', '11_tailcall.c', '12_eh_basic.cpp', '13_eh_classes.cpp', '14_eh_calls.cpp')]
CASES.append(('09_tls', os.path.join(S1, '09_tls.c'), 'unicorn', {'mask': [100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 113, 285]}))
for p in ('19_eh_probe', '20_eh_hier', '21_eh_ptr', '22_eh_rethrow_nested', '23_eh_catchall_misc'):
    CASES.append((p, os.path.join(PR, p + '.cpp'), 'native', {'sites': True}))
CASES.append(('26_eh_threads', os.path.join(PR, '26_eh_threads.cpp'), 'native', {'sites': True, 'threads': 8}))


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        raise SystemExit('FAILED: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


def llvm_links(out):
    """A folder where clang.exe, clang++.exe, llvm-objdump.exe and llvm-nm.exe are this system's tools."""
    d = os.path.join(out, 'llvm-bin')
    os.makedirs(d, exist_ok=True)
    for name in ('clang', 'clang++', 'llvm-objdump', 'llvm-nm'):
        target = shutil.which(name)
        if not target:
            raise SystemExit('%s not found on PATH' % name)
        link = os.path.join(d, name + '.exe')
        if os.path.lexists(link):
            os.remove(link)
        os.symlink(target, link)
    return d


def main():
    out = os.path.abspath(sys.argv[1])
    for sub in ('elf', 'expected', 'gen', 'sites', 'native'):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    os.environ['LLVM_BIN'] = llvm_links(out)
    sys.path.insert(0, os.path.join(RECOMP, 'oracle', 'e2e'))
    sys.path.insert(0, TOOLS)
    import build_prog   # noqa: E402  the S1 oracle's compile command, with its BASE set per case
    build_prog.LLVM = os.environ['LLVM_BIN']
    import guest_build  # noqa: E402

    rows = []
    for i, (name, src, expect, opt) in enumerate(CASES):
        base = BASE0 + i * STEP
        assert base + STEP <= 0x7100000000, 'case bases must stay below 0x7100000000'
        elf = os.path.join(out, 'elf', name + '.elf')
        expected = os.path.join(out, 'expected', name + '.expected')
        build_prog.BASE = base
        ok, msg = guest_build.build(src, elf, '-O2')
        if not ok:
            raise SystemExit('%s: %s' % (name, msg))
        span = max(int(l.split()[0], 16) for l in run(['llvm-nm', '--defined-only', elf]).splitlines() if len(l.split()) == 3) - base
        if span >= STEP:
            raise SystemExit('%s: the image spans %#x bytes, more than its %#x slot' % (name, span, STEP))

        if expect == 'unicorn':
            run([sys.executable, os.path.join(TOOLS, 'unicorn_expect.py'), elf, expected])
        else:
            exe = os.path.join(out, 'native', name)
            run(['clang++', '-std=c++17', '-O2', '-w', '-I', os.path.join(LIFT, 'native'), '-I', S1, src,
                 os.path.join(LIFT, 'native', 'native_main.cpp'), '-o', exe])
            run([exe, expected])

        lift = [sys.executable, '-m', 'nxrecomp', 'lift', elf, os.path.join(out, 'gen', name), '--kind', 'static_elf',
                '--name', 't%02d' % i, '--index', str(i), '--base', hex(base), '--shards', '2', '--jobs', '2']
        if opt.get('sites'):
            sites = os.path.join(out, 'sites', name + '.sites')
            run([sys.executable, os.path.join(TOOLS, 'make_sites.py'), elf, sites])
            lift += ['--sites', sites]
        run(lift, cwd=RECOMP)

        mask = opt.get('mask', [])
        rows.append('    {"%s", "%s.elf", "%s.expected", &nxmod_t%02d, %d, {%s}, %d},' % (
            name, name, name, i, opt.get('threads', 0), ', '.join(str(m) for m in mask) or '-1', len(mask)))
        print('%-24s base %x  expected from %s' % (name, base, expect), flush=True)

    with open(os.path.join(out, 'cases.inc'), 'w') as f:
        f.write('// generated by ps5/p1/make_cases.py: {name, elf, expected, module, threads, mask[], mask count}\n')
        for i in range(len(CASES)):
            f.write('extern "C" const NxModule nxmod_t%02d;\n' % i)
        f.write('const Case kCases[] = {\n%s\n};\n' % '\n'.join(rows))
    print('%d cases written to %s' % (len(CASES), out))


if __name__ == '__main__':
    main()
