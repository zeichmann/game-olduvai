// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "presentation/render/game_render.hpp"

namespace olduvai::presentation {
// Optional local HD character sheet. Returns false when the original art is used.
bool draw_player_skin(RenderTarget& t, float x, float y, bool left,
                      int pose = 0, float brightness = 1.0f);
}
