# Pokopia Launcher

Tauri 2 launcher for the Pokopia PC port (`pkp.exe`). It ships no game files and never changes yours. It reads the game
files you extracted from your own copy, writes `pkp.ini` next to `pkp.exe` and builds the shader cache from your romfs.

## First run

1. **Game files**: pick the dump folder (`<dump>\base\{exefs,romfs}` and/or `<dump>\updates\<version>\{exefs,romfs}`).
   The newest complete update is used, otherwise the base. "Use these game files" writes `[fs] exefs` and `[fs] romfs`.
   Instead of a folder, the player can pick NSP / XCI files plus their keys (`rawdump.rs`, `tools\extract\nx_extract.exe`), or
   one `.nup` package ([a55uka/nup](https://github.com/a55uka/nup) format 1, `src-tauri/src/nup.rs`, ported from the
   fefw-recomp launcher). A .nup is already decrypted and holds the base game and the update, so it needs no keys. Set up
   (and the PS5 install) first unpacks either into `dump\` next to the launcher (about 7 GB), then carries on as with a dump
   folder. The .nup unpack checks the title ID, writes each file as `.part` and renames it when complete, so Pause keeps the
   finished files; `cargo run --release --example nup_unpack -- <file.nup> <title id> [<out dir>]` does the same from a terminal.
2. **Set up**: writes the `pkp.ini` paths (`[fs] exefs`, `[fs] romfs`, `[fs] save_dir`, `[nvn] shader_cache =
   shader_cache/spirv`) and builds `shader_cache\spirv\` next to `pkp.exe`:
   - Extract: `tools\shaders\extract_corpus.py` finds every shader in the romfs and in `exefs\main` and writes the
     distinct blocks to `shader_cache\corpus\` (about 10 GB, deleted when setup finishes).
   - Translate: `tools\spirv_harness\spirv_harness.exe --batch` in chunks of 1000 into `shader_cache\spirv\`
     (`<hash>.spv` + `results.tsv`, about 26 GB for the 99,012 blocks of 2.0.1).
   A progress bar shows each step. Pause stops the tools and Continue setup picks up where they stopped: a block listed
   in `results.tsv` is never translated twice, and extraction is skipped if it already ran for the same romfs. The tools
   run at below-normal priority.
   State: `shader_cache\setup.json`.
3. **Play** turns on once setup has finished. The launcher starts pkp.exe with no paths on the command line. A `pkp.ini`
   next to it makes its folder the working directory, and every path comes from the ini (relative paths start there).

`release\Play.bat` starts the game the same way without the launcher (after setup).

## Release model (b): pkp.exe built on the player's PC

When `tools\build\pkp_build.py` is present (the package from the main project's release script), the release holds no
`pkp.exe`. Set up first builds it from the chosen dump (`src-tauri/src/gamebuild.rs`): verify the ExeFS by hash, lift,
compile with the bundled Zig, link against `pkp_rt.dll`; then the shader steps below run. Play stays off until both are
done. Pause works in every step and Continue setup resumes. Headless check of the whole first run:
`cargo run --release --example first_run -- <game dir> <exefs> <romfs> <tools dir> [shader limit] [jobs]`.

## PS5 mode

The **PS5** page puts the game on the player's own PS5 as the homebrew title `PPSA90006` (`/data/homebrew/PPSA90006`), with no
WSL and no developer tools: pick the dump folder or a `.nup` (unpacked into `dump\` first, on the first step's bar), type the console's IP address (its FTP server on port 2121 must be running),
press **Install on PS5**. Four steps, each with its own bar; Pause stops the running one and Continue picks up where it stopped
(`src-tauri/src/ps5.rs`):

1. **Build the PS5 title**: `tools\build\ps5_build.py` checks the dump (the same check as the PC build), lifts it with the PC
   build's own lifter, compiles the lifted code for the console with the bundled clang, links it against the prebuilt runtime,
   RADV, SDL3 and libvpx, then converts and signs it with `ps5-native-tool` into `<game>\ps5\PPSA90006`. About 7 minutes on a
   16-thread PC. The same dump gives the same `eboot.bin` as `ps5/game/build.sh` on Linux: the bundle carries that build's exact
   `clang -cc1` and `ld.lld` lines and the same LLVM version, and `<game>\ps5\build\done.json` says whether the result matches
   the release's reference build (`matches_reference`).
2. **Make the shader cache**: the PC's own shader setup (`shaders.rs`) into `<game>\shader_cache\spirv`. A cache the PC setup
   already finished is used as it is.
3. **Upload the game data**: the dump's `exefs\` and `romfs\` (6.75 GB).
4. **Upload the shaders and the title**: the shader cache (about 40 GB, `results.tsv` last), then the title folder
   (`eboot.bin` last). It checks the size of `eboot.bin` on the console afterwards: a running title's `eboot.bin` cannot be
   replaced, so close the game on the PS5 first.

Your own home screen pictures: put `icon0.png` (512x512 PNG) and/or the backdrops `pic0.dds` and `pic1.dds` (3840x2160 BC7 DDS,
from PS5_Vulkan's `tools/prepare-assets.sh --background <picture>`) in `<game>\ps5\art`; the build puts
them in the title folder (`sce_sys`). The package ships none.

Uploads use four FTP connections and skip files the console already has with the same size, so a stopped upload continues.
`<game>\ps5\install.json` records finished uploads per console address. After an upload that changed anything, the console's
`pkp-prepared.txt` is deleted, so the title's **Prepare game** checks the files again. The player then starts `PPSA90006` (the
first time, installed with their homebrew installer like any fake-signed title), picks **Prepare game** once, then **Play**.

Headless check of the whole install: `cargo run --release --example ps5_install -- <game dir> <exefs> <romfs> <tools dir> <ip>`
(add `check` to only connect).

### The PS5 toolchain (`tools\ps5`)

Made on the developer's Linux / WSL machine after `ps5/game/build.sh`, by:

```bash
ps5/tools/bundle-windows.sh <out>/tools/ps5 <clang+llvm-23.1.1-x86_64-pc-windows-msvc folder> <zig.exe>
```

The LLVM folder is LLVM's official Windows release, extracted; it must be the same version as the Linux clang (the script checks).
`zig.exe` (the release's own, `tools\build\zig`) builds `ps5-native-tool.exe` from PS5_Vulkan's sources. The folder (about 590 MB,
about 150 MB zipped) holds `llvm\` (clang, ld.lld, llvm-ar, headers, compiler-rt builtins), `sdk\` (the payload SDK's sysroot),
`prebuilt\` (the title's own code built by `build.sh`, RADV, SDL3, libvpx, link stubs), `title\` (param.json, icon0.png,
libc.prx, font), `bin\ps5-native-tool.exe` and `recipe.json` (the compile and link lines with `{SDK}`, `{RES}`, `{PRE}`, `{OBJ}`
... for paths). Nothing in it comes from the game. Licences: `BUNDLED_TOOLS.txt` and `tools\ps5\licenses`.

The release also needs `tools\build\ps5_build.py` next to `game_build.py` (from `game-build/release_b`).
`game-build/release_b/make_release.py` makes such a release from a published package zip: it puts in the launcher, both build
scripts, the lifter and `tools\ps5`, keeps the rest (the runtime DLL among it; the lifter's ABI must match it), and writes
`<package>-<version>-win64.zip`.

## Release folder

The launcher looks for its tools in `tools\` next to its own exe. A release folder must contain:

```
pkp.exe                     the game (game-build, -Release)
PokopiaLauncher.exe         this launcher (npm run tauri build)
Play.bat                    release\Play.bat (optional)
tools\
  spirv_harness\            self-contained publish of nx tools/shaders/harness:
                              dotnet publish tools\shaders\harness\SpirvHarness.csproj -c Release -r win-x64
                                --self-contained true -o <release>\tools\spirv_harness     (~84 MB)
                            (stage the Ryujinx sources first: tools\shaders\scripts\setup_ryujinx.ps1)
  shaders\extract_corpus.py nx tools/shaders/ampere/extract_corpus.py
  python\python.exe         optional: Python 3 (for example the embeddable package) with the lz4 package installed.
                            Without it the first python.exe on PATH that can `import lz4.block` is used.
```

If a file is missing, setup stops and names it. `pkp.exe` can live in another folder (Settings, game folder), but
`tools\` always sits next to the launcher.

## Development

- `cargo check` in `src-tauri`; `npm run tauri build` for the exe.
- `cargo run --example shader_setup -- <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]` runs the setup's
  shader steps without the window (same code, `src-tauri/src/shaders.rs`); `limit` translates only that many blocks.
