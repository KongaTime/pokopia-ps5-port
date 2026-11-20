# Pokopia Recomp — PS5

Pokémon Pokopia 2.0.1a, recompiled to run as a native homebrew title on a PS5 (title ID `PPSA90006`), on top of the
Windows recompilation in this repository. Runs at 1080p, about 60 fps, with sound, the DualSense and the console's own
on-screen keyboard.

You need your own copy of the game, version 2.0.1a, and a PS5 that runs homebrew. Nothing from the game is in this
repository or in anything built from it without your dump: you lift and build the game yourself, on your PC, from your
dump (see [LEGAL.md](../LEGAL.md)). Not affiliated with Nintendo, The Pokémon Company, Koei Tecmo or Sony.

## Install with the launcher (Windows)

The simplest way, with no WSL and no developer tools: open the launcher of a release that has PS5 mode, go to **PS5**, pick your
2.0.1a dump folder, type your PS5's IP address, start the console's FTP server (port 2121) and press **Install on PS5**. The
launcher builds and signs the title on your PC from your dump, makes the shader cache and uploads everything to
`/data/homebrew/PPSA90006`. Each step has its own progress bar; you can pause and continue later. The first install takes hours,
mostly the upload of about 50 GB. Then start `PPSA90006` on the console and pick **Prepare game**, then **Play** (step 4 below).
How it works: [launcher/README.md](../launcher/README.md#ps5-mode).

The rest of this page is the Linux / WSL route.

## What you need

On the console:

- a PS5 that runs homebrew titles from `/data/homebrew` (the same set-up as other fake-signed homebrew ports)
- an FTP server on port 2121 (to upload the game; the console's homebrew tools usually include one)
- about 50 GB free: the shader cache (40.5 GB, 99,251 files), the game data (6.75 GB) and the title (300 MB)

On the PC (Linux, or Windows with WSL; an Arch or Ubuntu WSL distro works):

- `cmake`, `ninja`, `curl`, `unzip`, Python 3.11+ with `recomp/requirements.txt`
- the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) (`prospero-clang`)
- [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), built: `ps5-native-tool` (link and sign), `libc.prx` and its
  RADV archive (the console has no system Vulkan; the title carries Mesa's RADV driver)
- the AGC link stubs `libSceAgc.so` / `libSceAgcDriver.so` (any PS5 homebrew build that links RADV has them)
- `nasm`, for the movie decoder; SDL3 and libvpx are built by `setup.sh` (`ps5/tools/build-sdl3.sh`, `build-libvpx.sh`)
- for the shader cache: a Windows PC that can run this project's shader tools (see below)

## Set-up

1. Copy `ps5/local.env.example` to `ps5/local.env` and fill in the paths above and your console's address.
2. Make the shader cache on Windows from the same dump: the Windows launcher's Set up does it (1 to 2 hours), or
   `spirv_harness --batch` from [tools/shaders](../tools/shaders/README.md). You need its `spirv` folder
   (`<hash>.spv` files and `results.tsv`).
3. Start the FTP server on the console, then run:

   ```bash
   ps5/setup.sh /path/to/your/dump --shaders /path/to/spirv
   ```

   It checks the tools and that the dump is 2.0.1a, fetches the graphics sources, lifts the game (about 20 minutes),
   builds and signs the title, and uploads the game data, the shaders and the title to `/data/homebrew/PPSA90006`.
   Each step that is already done is skipped, so after a failure run it again; a shader upload that stopped goes on
   where it was. The first upload of the shaders (47 GB with the game data, over FTP) takes hours: leave it running.
4. Start `PPSA90006` from the home screen. The first time, pick **Prepare game**: it checks the game files and the
   shaders (about 15 seconds) and says what is missing, if anything. Then **Play** starts the game, and later starts go
   straight to Play.

Later builds: `ps5/game/build.sh --deploy` rebuilds and uploads only the title. Close the game first: the console
does not let a running title's `eboot.bin` be replaced, and the upload fails silently.

## Controls

| DualSense | Switch |
|---|---|
| Cross / Circle / Square / Triangle | A / B / X / Y (by position: Cross confirms) |
| L1 / R1, L2 / R2 | L / R, ZL / ZR |
| Sticks, stick clicks, D-pad | the same |
| Options | + |
| Touchpad click | − (the DualSense has no Select) |

Naming your character opens the console's keyboard. If it cannot open, the name is `Player`; you can set another one
with `NXRT_APPLET_SWKBD_TEXT=<name>` in `pkp-env.txt` (below).

## Settings and logs

All in `/data/homebrew/PPSA90006` on the console (the title's `/app0`):

| File | What it is |
|---|---|
| `pkp-env.txt` | optional, `NAME=VALUE` lines: any runtime setting as its `NXRT_*` variable ([runtime/docs/CONFIG.md](../runtime/docs/CONFIG.md)); these replace the console defaults |
| `pkp.ini` | optional: the runtime's ini, for the keys `pkp-env.txt` does not set |
| `controls.ini` | button mapping and display options (`[play] controls`) |
| `title.png`, `logo.png` | optional, your own pictures for the start screen: the background (filled to the screen) and the name (top left). With a `title.png` and no `logo.png` the picture's own name is used |
| `sce_sys/icon0.png`, `sce_sys/pic0.png` | the home screen icon (512x512) and backdrop (1920x1080). Builds use the payload SDK's sample icon and no backdrop; your own pictures come from `PKP_ART` (`ps5/local.env`) or, with the launcher, `<game folder>\ps5\art`. They are never part of the repository or a release. After changing them, install the title again with your homebrew installer so the home screen reads them |
| `pkp-prepared.txt` | written when Prepare game has passed; delete it to prepare again |
| `save/` | your save data |
| `pipeline_cache.bin`, `pipeline_keys.bin` | compiled graphics pipelines, kept between sessions so the second start stutters less; delete both to start over |
| `pkp-results/pkp.log` | the log of the last session (`pkp.log.prev`: the one before) |
| `eboot_crash.txt`, `eboot_freeze.txt` | written when the game crashes or hangs: attach them to a report |

Useful settings: `NXRT_AUDIO_OUT_LATENCY_MS` (default 80 on the console: higher means fewer gaps in the sound while loading,
with more delay), `NXRT_BOOT_MAX_SECONDS` (end the session after that many seconds; 0, the default, never).

## Known issues

- Short gaps in the sound while an area loads.
- The first time an area is seen, a few frames can stutter while its pipelines compile; they are kept for later sessions.
- Without the shader cache the game runs, but everything whose shader is missing is not drawn.
- The console logs one `SIGSYS` when the game exits; it is the exit itself and harmless.

## Reporting problems

Open an issue with `pkp-results/pkp.log` (and `eboot_crash.txt` or `eboot_freeze.txt` when there is one), your console's
firmware version and what you were doing.

## For developers

- Everything for the console is under `ps5/`: `compat/` (the Win32 subset the runtime uses, on the console's kernel),
  `runtime/` (PS5 back ends: audio, the keyboard, services), `gfx/`, `title/` (the title's libc and link), `game/` (the
  title's `main`, the pad, the build), `stubs/` (link stubs the SDK lacks). [PLAN.md](PLAN.md) has the port's history.
- `-DPKP_PS5_BRINGUP=ON` links the main-loop traces used during bring-up; `PKP_FSTRACE=1`, `PKP_WATCH`, `PKP_TRAP` and
  `PKP_SAMPLE` in `pkp-env.txt` turn on the other bring-up traces (see `game/pkp_ps5_main.cpp`). A file named `pkp-debug`
  next to the game turns on the debug log.

## Credits

The Windows recompilation this is built on: [pokopia-recomp](https://github.com/drakolordx7/pokopia-recomp) and
[nxrecomp](https://github.com/drakolordx7/nxrecomp), with the credits in the [main README](../README.md). On the console:
Mesa's RADV through mihawk-99's [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan); KongaTime's PS5 ports
(Monado-PS5 and the KH 358/2 Days port: the title build, the PS5 SDL3, the pad and audio set-up); the AGC link stubs from
[The-Darkness-Recomp](https://github.com/portingpete/The-Darkness-Recomp); the
[PS5 payload SDK](https://github.com/ps5-payload-dev/sdk).

## License

GPL-3.0, as the rest of the project. Third-party code keeps its own licence.
