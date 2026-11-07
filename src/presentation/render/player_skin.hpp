// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "presentation/render/game_render.hpp"
#include <limits>

namespace olduvai::presentation {
bool draw_frog_character(RenderTarget& t, const formats::Sprite& sprite,
                         const std::vector<formats::Rgb>& palette,
                         float x, float y, bool left, float brightness = 1.0f,
                         const formats::Sprite* carried_weapon = nullptr);
// Optional local HD character sheet. Returns false when the original art is used.
bool draw_player_skin(RenderTarget& t, float x, float y, bool left,
                      int pose = 0, float brightness = 1.0f,
                      float motion_x = std::numeric_limits<float>::quiet_NaN(),
                      float motion_y = std::numeric_limits<float>::quiet_NaN());
}
