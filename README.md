# Pokopia PS5 Port

Pokémon Pokopia (Switch 2, version 2.0.1a) recompiled to run natively on the PS5 as the homebrew title PPSA90006, and on
Windows. Built on the Windows recompilation [pokopia-recomp](https://github.com/drakolordx7/pokopia-recomp).

[Download the latest release](../../releases/latest)

You need your own copy of the game, version 2.0.1a: an extracted folder, a `.nup` file, or NSP/XCI files plus your keys. Nothing from the
game is in this repository or in a release: the game is built from your dump, on your PC. Not affiliated with Nintendo, The
Pokémon Company, Koei Tecmo or Sony.

## PS5

You need a PS5 that runs homebrew, with its FTP server (port 2121), and a Windows PC on the same network.

1. Unzip the release and run `PokopiaLauncher.exe`.
2. On the **PS5** page, pick your dump folder or your `.nup` file (**Pick a .nup**). **Find my PS5** fills in the console's
   address, or type it. Press **Install on PS5**: it builds and signs the title on your PC, makes the shader cache and uploads
   everything to the console. The first time takes a while (the build about 15 minutes, the shaders under an hour on a fast
   PC, then about 18 GB to send); you can pause and continue.
3. Install PPSA90006 with your homebrew installer, start it, pick **Prepare game** once, then **Play**.

With a `.nup`, the console gets that one file (in `PPSA90006/dump/`) and the game reads its data straight from it: nothing is
unpacked on the PS5. The shader cache goes over as one compressed file, `spirv.pack` (11 GB instead of 40 GB in about 99,000
files).

Details, settings, logs and the Linux / WSL build: [ps5/README.md](ps5/README.md). How the launcher does it:
[launcher/README.md](launcher/README.md#ps5-mode).

## PC

### Requirements

- Windows 10 or 11, 64-bit
- GPU with Vulkan 1.3
- 16 GB of RAM

### Usage

1. Unzip the release and run `PokopiaLauncher.exe`.
2. On the **Game files** page, pick your dump folder (or your `.nup` file, or your NSP/XCI files and key files) and press Set up. It compiles the game on your PC (about 20 minutes), then builds the shader cache (1 to 2 hours). You can pause it and continue later.
3. Press Play.

F1 opens the settings in game. Middle mouse toggles Joy-Con 2 mouse mode for placing items.

## Reporting problems

[Open an issue](../../issues/new/choose) with the log of the session where it happened: on the PS5
`/data/homebrew/PPSA90006/pkp-results/pkp.log` (plus `eboot_crash.txt` or `eboot_freeze.txt` if there is one) and your
firmware version; on PC `pkp.log`, your GPU and your Windows version.

## Contributing

- Open pull requests against `main` and say what you tested on which GPU.
- Code for this game goes in `recomp/games/pkp/` and `runtime/games/pkp/`; the rest is shared with other games.
- Building from source needs Visual Studio 2022 Build Tools or newer, LLVM, CMake, Ninja, Python 3.13, .NET 8 SDK, vcpkg with `libvpx:x64-windows-static`, Rust and Node.js. Copy the game's `exefs` files to `game\dump\exefs\`, run `recomp\scripts\build_game.cmd pkp` in a Developer Command Prompt, then `pwsh -File game-build\scripts\build.ps1`. Shaders: [tools/shaders/README.md](tools/shaders/README.md). Launcher: `npm install`, `npm run tauri build` in `launcher\`.
- Runtime tests: `pwsh -File runtime\scripts\build.ps1 -Test`.

## Credits

Built on [nxrecomp](https://github.com/drakolordx7/nxrecomp). The `.nup` format is [a55uka/nup](https://github.com/a55uka/nup); the
launcher's reader is ported from [fefw-recomp](https://github.com/Goldoire/fefw-recomp)'s. Uses code from [Ryujinx](https://github.com/Ryubing/Ryujinx), [Mesa](https://gitlab.freedesktop.org/mesa/mesa), smo-recomp, ScarletNX, ssbu-recomp, SuperMarioWonderRecomp, yuzu, Eden, libnx, [astc-encoder](https://github.com/ARM-software/astc-encoder), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), [libvpx](https://chromium.googlesource.com/webm/libvpx), AMD FidelityFX, NVIDIA Image Scaling, [SDL3](https://github.com/libsdl-org/SDL), [Tauri](https://tauri.app), [Zig](https://ziglang.org) and Python, plus [Kenney input prompts](https://kenney.nl/assets/input-prompts) and DejaVu fonts. Licences are in `runtime/THIRD_PARTY_NOTICES.md`, `recomp/THIRD_PARTY.md` and `tools/shaders/licenses/`.

## License

GPL-3.0. Third-party code keeps its own licence.
