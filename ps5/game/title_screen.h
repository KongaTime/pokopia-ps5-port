// SPDX-License-Identifier: GPL-3.0-or-later
// The title's own start screen (PS5): shown after the window and the renderer exist and before the game starts. The first time it
// offers Prepare game, which checks the game files and the shader cache with a progress bar; once that has passed it offers Play.
#pragma once

namespace nxrt::gfx { class Device; class Renderer; }

namespace pkp_ps5 {

// Runs the screen until the player picks Play (true) or Exit (false). game_dir is the title folder (/app0).
bool title_screen_run(nxrt::gfx::Device& device, nxrt::gfx::Renderer& renderer, const char* game_dir);

}  // namespace pkp_ps5
