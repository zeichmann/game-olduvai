// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <array>

namespace olduvai::presentation {
// Presentation-only motion sampling: repeated dirty passes do not advance it.
struct SkinAnimation {
    bool initialized = false;
    float last_x = 0, last_y = 0;
    double run_distance = 0, climb_distance = 0, last_motion = -10;
    int direction = 1;
    int sample(float x, float y, int pose, double seconds) {
        const float signed_dx = initialized ? x - last_x : 0;
        const float dx = initialized ? std::abs(x - last_x) : 0;
        const float dy = initialized ? std::abs(y - last_y) : 0;
        last_x = x; last_y = y; initialized = true;
        if (pose == 6) {
            if (dy < 16) climb_distance += dy;
            return 16 + static_cast<int>(climb_distance / 3.0) % 4;
        }
        if (pose == 7) return 14;
        if (pose == 5) return 13;
        if (pose == 4) return 12;
        if (dx > 0.001f && dx < 16) {
            direction = signed_dx < 0 ? -1 : 1;
            run_distance += dx;
            last_motion = seconds;
        }
        if (seconds - last_motion < 0.09)
            return 4 + static_cast<int>(run_distance / 5.0) % 8;
        const double blink = std::fmod(seconds, 4.2);
        if (blink >= 4.02 && blink < 4.10) return 2;
        if (blink >= 3.98 && blink < 4.14) return 1;
        return 0;
    }
};
}
