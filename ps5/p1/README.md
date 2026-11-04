# P1 — the runtime's lifted test programs on the PS5

The first console milestone of [the PS5 plan](../PLAN.md): recompiled AArch64 code runs on the PS5 through the real
runtime (nxrt) and the [compat layer](../compat/). No game files are involved.

The title `PPSA90002` runs the runtime's own integration tests (`runtime/tests/lift`) in one process:

| Programs | What they exercise | Expected results from |
|---|---|---|
| 00–08, 10, 11 | integer, switch tables, FP, NEON, crypto/CRC, LSE atomics, exclusives, virtual calls, tail calls | Unicorn running the original AArch64 code |
| 09_tls | TLS variables and thread pointers (13 address-dependent words masked, as upstream) | Unicorn |
| 12–14 | C++ exceptions through the S1 host services | Unicorn |
| 19–23 | C++ exceptions through the runtime's unwinder (libc++abi-shaped guest runtime) | the same program on the host, with the host's exceptions |
| 26_eh_threads | exceptions on 8 guest threads at once | host |
| ABI check | a module lifted against another ABI version is refused | — |

Each program also has to return with the callee-saved registers and SP it was entered with. Left out because they end
the process (upstream runs them in child processes): `24_eh_term_uncaught`, `25_eh_term_noexcept`.

## Build

In the archlinux WSL distro, once:

```bash
python3 -m venv /home/kai/pkp-venv
/home/kai/pkp-venv/bin/pip install unicorn==2.1.4 capstone==5.0.9 lz4 pyelftools
```

Then:

```bash
bash /mnt/c/kb/Pokopia-PS5/ps5/p1/build.sh
```

1. `make_cases.py` compiles each program for AArch64 at a base of its own (`0x70_0000_0000 + n × 64 MiB`), computes its
   expected results, and lifts it with nxrecomp as module `tNN`.
2. CMake (`ps5/CMakeLists.txt`) builds the runtime against `ps5/compat`, the 21 lifted modules and `p1_main.cpp`, and
   links them as a title (`ps5/title`: the CRT, link layout and wraps of KongaTime's Darkness and Monado titles).
3. `ps5-native-tool` converts and signs it. `dist/PPSA90002/` gets `eboot.bin`, `sce_sys/`, `sce_module/libc.prx` and
   `tests/` (the ELFs and expected results).

## Run

```bash
bash /mnt/c/kb/Pokopia-PS5/ps5/p1/build.sh --deploy <console-ip>
```

This uploads the title to `/data/homebrew/PPSA90002` over the console's FTP (port 2121). Launch it from the home screen.
It has no picture: it posts "Pokopia P1 starting", runs for a few seconds, then posts "Pokopia P1: n/22 passed" and
exits. Read the results back:

```bash
curl -s ftp://<console-ip>:2121/data/pokopia-p1/results.txt
curl -s ftp://<console-ip>:2121/data/pokopia-p1/nxrt.log
```

`results.txt` has one line per step, written before the step runs (`STAGE`, `RUN`, then `PASS`/`FAIL` with the first
differing words), so a file that stops says where the title died. A pass is `RESULT 22/22 passed`.
