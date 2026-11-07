// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Krzysztof Sokołowski
// Scale-aware sprite writers: the blit_sprite / blit_sprite_keyed overloads
// (native loop + HD upscale-and-blit).  Declared in game_render.hpp.
#include "presentation/render/game_render.hpp"
#include "presentation/render/player_skin.hpp"
#include "presentation/render/skin_animation.hpp"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include "../../../third_party/stb/stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "formats/hash64.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <tuple>
#include <chrono>
#include <filesystem>
#include <vector>

namespace olduvai::presentation {

using formats::Rgb;
using formats::Sprite;

namespace {

// Palette entry `color` as opaque RGBA at `dst`; an index past the palette
// shows magenta.
inline void put_palette_pixel(std::uint8_t* dst, const std::vector<Rgb>& pal,
                              int color) {
    const Rgb c = color < static_cast<int>(pal.size())
                      ? pal[static_cast<std::size_t>(color)]
                      : Rgb{255, 0, 255};
    dst[0] = c.r;
    dst[1] = c.g;
    dst[2] = c.b;
    dst[3] = 255;
}

// The HD cache's source key for a sprite: everything its RGBA is built from
// (the planes, the size and format, the 16 palette entries a 4-bit sprite
// can index, and which builder: plain, flipped, keyed), with the scale,
// profile and bleed the asset is made at.  A hit skips the decode, the
// buffer and the pixel hash; an edited palette or another sprite is simply
// another key.
std::uint64_t sprite_source_key(const Sprite& s, const std::vector<Rgb>& pal,
                                int builder, const RenderTarget& t,
                                bool bleed) {
    formats::Hash64 k;
    k.mix(static_cast<std::uint64_t>(builder));
    k.mix(s.width);
    k.mix(s.height);
    k.mix(static_cast<std::uint64_t>(s.format));
    k.mix_bytes(s.raw_pixels.data(), s.raw_pixels.size());
    for (std::size_t i = 0; i < 16; ++i) {
        if (i < pal.size()) {
            k.mix((std::uint64_t{pal[i].r} << 16) |
                  (std::uint64_t{pal[i].g} << 8) | pal[i].b);
        } else {
            k.mix(~std::uint64_t{0});   // missing: the builders' magenta
        }
    }
    k.mix(static_cast<std::uint64_t>(t.scale));
    k.mix_str(*t.profile);
    k.mix(bleed ? 1 : 0);
    return k.value();
}

constexpr int kPlain = 0, kPlainFlipped = 1, kKeyed = 2;

// Majority-vote opaque colour = background, keyed out.  Mirrors the
// reference's background-colour detection.
int key_colour(const std::vector<formats::IndexedPixel>& pixels) {
    std::array<int, 16> counts{};
    for (const auto& p : pixels)
        if (p.opaque && p.color < 16) ++counts[p.color];
    int bg_idx = 0, best = counts[0];
    for (int ci = 1; ci < 16; ++ci) {
        if (counts[ci] > best) { best = counts[ci]; bg_idx = ci; }
    }
    return bg_idx;
}

// The keyed sprite for the HD cache: the background colour kept in RGB with
// alpha 0 and the rest opaque, so OmniScale blends edges toward the water
// colour rather than fattening thin bubbles (as the reference).  RGB is
// defined everywhere, so it is upscaled with bleed=false.
std::vector<std::uint8_t> keyed_rgba(const Sprite& s,
                                     const std::vector<Rgb>& pal) {
    const auto pixels = s.decode_indexed();
    const int bg_idx = key_colour(pixels);
    const int w = s.width, h = s.height;
    const Rgb bgc = (bg_idx < static_cast<int>(pal.size()))
                        ? pal[static_cast<std::size_t>(bg_idx)] : Rgb{0, 0, 0};
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = bgc.r; rgba[i + 1] = bgc.g; rgba[i + 2] = bgc.b;
    }
    for (int sy = 0; sy < h; ++sy)
        for (int sx = 0; sx < w; ++sx) {
            const auto& p = pixels[static_cast<std::size_t>(sy) * w + sx];
            if (!p.opaque) continue;
            if (p.color == static_cast<std::uint8_t>(bg_idx)) continue;
            put_palette_pixel(&rgba[(static_cast<std::size_t>(sy) * w + sx) * 4],
                              pal, p.color);
        }
    return rgba;
}

// Write one native palette pixel: resolve the colour, apply the dx clip, store
// opaque RGBA.  A clipped dx is a no-op.
inline void blit_pixel(RenderTarget& t, int dx, int dy,
                       const std::vector<Rgb>& pal, int color) {
    if (dx < 0 || dx >= t.w || dx < t.clip_x_lo || dx >= t.clip_x_hi) return;
    put_palette_pixel(&t.px[(static_cast<std::size_t>(dy) * t.w + dx) * 4], pal,
                      color);
}

// Rows [r.y0, r.y1) and columns [r.x0, r.x1) of `hd` placed at (ox, oy),
// alpha-composited: opaque texels copy, partial ones (the blending scaler's
// edges) blend over what is already drawn.
void write_hd_block(RenderTarget& t, const enhance::HdAsset& hd, int ox,
                    int oy, const DirtyRect& r) {
    const std::size_t n = static_cast<std::size_t>(r.x1 - r.x0) * 4;
    for (int dy = r.y0; dy < r.y1; ++dy) {
        const std::uint8_t* sp =
            hd.px.data() +
            (static_cast<std::size_t>(dy - oy) * hd.w + (r.x0 - ox)) * 4;
        std::uint8_t* dp = t.px + (static_cast<std::size_t>(dy) * t.w + r.x0) * 4;
        const std::uint8_t* const se = sp + n;
        for (; sp < se; sp += 4, dp += 4) {
            const std::uint8_t av = sp[3];
            if (av == 0) continue;
            if (av == 255) {
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
            } else {
                const int ia = 255 - av;
                dp[0] = static_cast<std::uint8_t>((sp[0] * av + dp[0] * ia) / 255);
                dp[1] = static_cast<std::uint8_t>((sp[1] * av + dp[1] * ia) / 255);
                dp[2] = static_cast<std::uint8_t>((sp[2] * av + dp[2] * ia) / 255);
            }
            dp[3] = 255;
        }
    }
}

// Blit an upscaled block at (fx, fy), clipped and alpha-composited; the HD
// half shared by both blitters (their differences are settled before `hd`
// exists).  Rounds at HD, not native: a bubble at native sub-pixels
// 0/0.33/0.67 moves to distinct HD pixels (0/1.33/2.67 at scale 4).  Integer
// positions round-trip exactly.  `key` names the asset for the dirty
// present's blit record.
void blit_hd_block(RenderTarget& t, const enhance::HdAsset& hd, float fx,
                   float fy, std::uint64_t key) {
    const int ox = static_cast<int>(std::lround((fx + t.origin_x) * t.scale));
    const int oy = static_cast<int>(std::lround(fy * t.scale));
    // The column range is fixed for the block (ox is constant): fold the clip
    // tests into [sx_lo, sx_hi).
    int sx_lo = 0;
    const int sx_hi = std::min(hd.w, std::min(t.w - ox, t.clip_x_hi - ox));
    if (sx_lo >= sx_hi) return;
    if (ox < 0) sx_lo = -ox;
    if (t.clip_x_lo > 0) sx_lo = std::max(sx_lo, t.clip_x_lo - ox);
    if (sx_lo >= sx_hi) return;
    const DirtyRect r{ox + sx_lo, std::max(oy, 0), ox + sx_hi,
                      std::min(oy + hd.h, std::min(t.h, t.clip_y))};
    if (r.empty()) return;
    if (t.blits != nullptr) t.blits->push_back({key, ox, oy, r});
    if (t.record_only) return;
    if (t.clip == nullptr) {
        write_hd_block(t, hd, ox, oy, r);
        return;
    }
    for (const DirtyRect& c : *t.clip) {
        const DirtyRect s = intersect(r, c);
        if (!s.empty()) write_hd_block(t, hd, ox, oy, s);
    }
}

}  // namespace

bool draw_player_skin(RenderTarget& t, float x, float y, bool left,
                      int pose, float brightness, float motion_x, float motion_y) {
    if (!t.hd_path()) return false;
    struct Sheet {
        std::vector<std::uint8_t> pixels;
        int w = 0, h = 0;
        std::array<DirtyRect, 20> bounds{};
        std::map<std::tuple<int, int, bool, int>, enhance::HdAsset> scaled;
        Sheet() {
            const char* path = std::getenv("OLDUVAI_PLAYER_SHEET");
            if (path == nullptr || *path == '\0') return;
            int channels = 0;
            auto* data = stbi_load(path, &w, &h, &channels, 4);
            if (data == nullptr || w < 4 || h < 2 || w > 8192 || h > 8192) {
                if (data) stbi_image_free(data);
                std::fprintf(stderr, "player skin: cannot load %s\n", path);
                w = h = 0;
                return;
            }
            pixels.assign(data, data + static_cast<std::size_t>(w) * h * 4);
            stbi_image_free(data);
            for (int i = 0; i < 20; ++i) {
                DirtyRect r{w, h, 0, 0};
                for (int py = (i / 4) * (h / 5); py < (i / 4 + 1) * (h / 5); ++py)
                    for (int px = (i % 4) * (w / 4); px < (i % 4 + 1) * (w / 4); ++px)
                        if (pixels[(static_cast<std::size_t>(py) * w + px) * 4 + 3] > 24) {
                            r.x0 = std::min(r.x0, px); r.x1 = std::max(r.x1, px + 1);
                            r.y0 = std::min(r.y0, py); r.y1 = std::max(r.y1, py + 1);
                        }
                bounds[i] = r;
            }
            std::fprintf(stderr, "player skin: Zabol v2, bandana, club, 20 animation poses\n");
        }
    };
    static Sheet sheet;
    if (sheet.pixels.empty()) return false;
    static SkinAnimation motion;
    static const auto epoch = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - epoch).count();
    if (pose == 6) left = false; // Climbing faces into the ladder, not sideways.
    pose = motion.sample(std::isfinite(motion_x) ? motion_x : x,
                         std::isfinite(motion_y) ? motion_y : y, pose, seconds);
    const int source_pose = pose;
    const int cw = sheet.w / 4, ch = sheet.h / 5;
    const DirtyRect r{(source_pose % 4) * cw, (source_pose / 4) * ch,
                      (source_pose % 4 + 1) * cw, (source_pose / 4 + 1) * ch};
    if (r.empty()) return false;
    const int light = std::clamp(static_cast<int>(brightness * 255), 0, 255);
    const auto id = std::make_tuple(pose, t.scale, left, light);
    auto it = sheet.scaled.find(id);
    if (it == sheet.scaled.end()) {
        enhance::HdAsset asset;
        const int reference_height = std::max(1, sheet.bounds[0].y1 - sheet.bounds[0].y0);
        asset.h = std::max(1, 32 * t.scale * (r.y1 - r.y0) / reference_height);
        asset.w = std::max(1, 32 * t.scale * (r.x1 - r.x0) / reference_height);
        asset.px.resize(static_cast<std::size_t>(asset.w) * asset.h * 4);
        for (int py = 0; py < asset.h; ++py) {
            for (int px = 0; px < asset.w; ++px) {
                const double u = left ? asset.w - 1 - px : px;
                const double sx = r.x0 + (u + 0.5) * (r.x1 - r.x0) / asset.w - 0.5;
                const double sy = r.y0 + (py + 0.5) * (r.y1 - r.y0) / asset.h - 0.5;
                const int x0 = std::clamp(static_cast<int>(std::floor(sx)), r.x0, r.x1 - 1);
                const int y0 = std::clamp(static_cast<int>(std::floor(sy)), r.y0, r.y1 - 1);
                const float wx = static_cast<float>(sx - std::floor(sx));
                const float wy = static_cast<float>(sy - std::floor(sy));
                float rgba[4]{};
                for (int j = 0; j < 2; ++j) for (int k = 0; k < 2; ++k) {
                    const auto off = (static_cast<std::size_t>(std::min(y0 + j, r.y1 - 1)) *
                                     sheet.w + std::min(x0 + k, r.x1 - 1)) * 4;
                    const float weight = (k ? wx : 1 - wx) * (j ? wy : 1 - wy);
                    const float alpha = sheet.pixels[off + 3] / 255.0f;
                    for (int c = 0; c < 3; ++c) rgba[c] += sheet.pixels[off + c] * alpha * weight;
                    rgba[3] += sheet.pixels[off + 3] * weight;
                }
                const auto dst = (static_cast<std::size_t>(py) * asset.w + px) * 4;
                for (int c = 0; c < 3; ++c)
                    asset.px[dst + c] = static_cast<std::uint8_t>(std::clamp(
                        rgba[3] > 0 ? rgba[c] * light / rgba[3] : 0.0f, 0.0f, 255.0f));
                asset.px[dst + 3] = static_cast<std::uint8_t>(std::clamp(rgba[3], 0.0f, 255.0f));
            }
        }
        // Match the painted palette without blurring the character.
        for (std::size_t i = 0; i < asset.px.size(); i += 4) {
            const float luma = 0.3f * asset.px[i] + 0.59f * asset.px[i + 1] +
                               0.11f * asset.px[i + 2];
            for (int c = 0; c < 3; ++c)
                asset.px[i + c] = static_cast<std::uint8_t>(asset.px[i + c] * 0.85f + luma * 0.15f);
        }
        it = sheet.scaled.emplace(id, std::move(asset)).first;
    }
    // Physics uses a 32x30 body: centre the new art over the same feet.
    const auto key = UINT64_C(0x5a41424f4c000000) +
        static_cast<std::uint64_t>(pose * 4096 + t.scale * 512 + (left ? 256 : 0) + light);
    blit_hd_block(t, it->second, x + 16 - it->second.w / (2.0f * t.scale),
                  y + 30 - sheet.bounds[0].y1 * 32.0f /
                      std::max(1, sheet.bounds[0].y1 - sheet.bounds[0].y0), key);
    return true;
}

bool draw_frog_character(RenderTarget& t, const Sprite& sprite,
                         const std::vector<Rgb>& palette,
                         float x, float y, bool left, float brightness,
                         const Sprite* carried_weapon) {
    if (!t.hd_path() || std::getenv("OLDUVAI_PLAYER_SHEET") == nullptr) return false;
    struct Heads {
        int w = 0, h = 0;
        std::vector<std::uint8_t> pixels;
        std::array<DirtyRect, 4> bounds{};
        std::map<std::tuple<int, int, bool, int>, enhance::HdAsset> cache;
        Heads() {
            const auto path = std::filesystem::path(std::getenv("OLDUVAI_PLAYER_SHEET"))
                                  .parent_path() / "stone-age-head.png";
            int channels = 0;
            auto* data = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
            if (!data) return;
            pixels.assign(data, data + static_cast<std::size_t>(w) * h * 4);
            stbi_image_free(data);
            for (int frame = 0; frame < 4; ++frame) {
                DirtyRect r{w, h, 0, 0};
                for (int yy = frame / 2 * (h / 2); yy < (frame / 2 + 1) * (h / 2); ++yy)
                    for (int xx = frame % 2 * (w / 2); xx < (frame % 2 + 1) * (w / 2); ++xx)
                        if (pixels[(static_cast<std::size_t>(yy) * w + xx) * 4 + 3] > 32) {
                            r.x0 = std::min(r.x0, xx); r.x1 = std::max(r.x1, xx + 1);
                            r.y0 = std::min(r.y0, yy); r.y1 = std::max(r.y1, yy + 1);
                        }
                bounds[frame] = r;
            }
            std::fprintf(stderr, "Zabol: complete original body animation + frog head retarget\n");
        }
    };
    static Heads heads;
    if (heads.pixels.empty()) return false;
    auto body = sprite_to_rgba(sprite, palette, left);
    const int w = sprite.width, h = sprite.height;
    int top = h;
    for (int yy = 0; yy < h; ++yy) for (int xx = 0; xx < w; ++xx)
        if (body[(static_cast<std::size_t>(yy) * w + xx) * 4 + 3]) top = std::min(top, yy);
    if (top == h) return true;
    const int neck = std::min(h, top + 13);
    // Locate the leading skin pixels at hand height in THIS body frame.
    // The weapon therefore follows the hand's animation and sprite offsets.
    int grip_x = left ? w - 22 : 22, grip_y = std::min(h - 1, top + 22);
    int leading = left ? w : -1;
    for (int yy = std::min(h - 1, top + 17); yy < std::min(h - 4, top + 25); ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const auto off = (static_cast<std::size_t>(yy) * w + xx) * 4;
            const int r = body[off], g = body[off + 1], b = body[off + 2];
            if (body[off + 3] && r > g && g >= 60 && b * 20 >= g * 9 &&
                (left ? xx < leading : xx > leading)) {
                leading = xx; grip_x = xx; grip_y = yy;
            }
        }
    int hx0 = w, hx1 = 0;
    for (int yy = top; yy < std::min(neck, top + 8); ++yy)
        for (int xx = 0; xx < w; ++xx)
            if (body[(static_cast<std::size_t>(yy) * w + xx) * 4 + 3]) {
                hx0 = std::min(hx0, xx); hx1 = std::max(hx1, xx + 1);
            }
    for (int yy = 0; yy < h; ++yy) for (int xx = 0; xx < w; ++xx) {
        const auto off = (static_cast<std::size_t>(yy) * w + xx) * 4;
        if (yy < neck) { body[off + 3] = 0; continue; }
        const int red = body[off], green = body[off + 1], blue = body[off + 2];
        // Preserve the fur/black ink; retint the warm skin ramp only.
        if (red > green && green >= 60 && blue * 20 >= green * 9 && body[off + 3]) {
            body[off] = static_cast<std::uint8_t>(green * 1.10f);
            body[off + 1] = static_cast<std::uint8_t>(std::min(255, green + 25));
            body[off + 2] = static_cast<std::uint8_t>(blue * 0.60f);
        }
    }
    formats::Hash64 hash;
    hash.mix_bytes(body.data(), body.size()); hash.mix(w); hash.mix(h);
    const auto& hd_body = t.cache->get(body, w, h, t.scale, *t.profile);
    if (carried_weapon != nullptr) {
        // Reverse the wind-up art: the club head points away from the face.
        const auto rgba = sprite_to_rgba(*carried_weapon, palette, !left);
        const int ww = carried_weapon->width, wh = carried_weapon->height;
        DirtyRect bounds{ww, wh, 0, 0};
        for (int yy = 0; yy < wh; ++yy) for (int xx = 0; xx < ww; ++xx)
            if (rgba[(static_cast<std::size_t>(yy) * ww + xx) * 4 + 3]) {
                bounds.x0 = std::min(bounds.x0, xx); bounds.x1 = std::max(bounds.x1, xx + 1);
                bounds.y0 = std::min(bounds.y0, yy); bounds.y1 = std::max(bounds.y1, yy + 1);
            }
        if (!bounds.empty()) {
            const auto& original = t.cache->get(rgba, ww, wh, t.scale, *t.profile);
            enhance::HdAsset club;
            club.w = std::max(1, static_cast<int>((bounds.x1 - bounds.x0) * t.scale * 0.55f));
            club.h = std::max(1, static_cast<int>((bounds.y1 - bounds.y0) * t.scale * 0.55f));
            club.px.resize(static_cast<std::size_t>(club.w) * club.h * 4);
            for (int yy = 0; yy < club.h; ++yy) for (int xx = 0; xx < club.w; ++xx) {
                const int sx = bounds.x0 * t.scale + xx * (bounds.x1 - bounds.x0) * t.scale / club.w;
                const int sy = bounds.y0 * t.scale + yy * (bounds.y1 - bounds.y0) * t.scale / club.h;
                const auto src = (static_cast<std::size_t>(sy) * original.w + sx) * 4;
                const auto dst = (static_cast<std::size_t>(yy) * club.w + xx) * 4;
                std::copy_n(original.px.data() + src, 4, club.px.data() + dst);
            }
            // The grip is the centre of the lowest opaque handle pixels.
            float handle_x = 0; int samples = 0;
            for (int yy = std::max(0, club.h - t.scale); yy < club.h; ++yy)
                for (int xx = 0; xx < club.w; ++xx)
                    if (club.px[(static_cast<std::size_t>(yy) * club.w + xx) * 4 + 3] > 128) {
                        handle_x += xx; ++samples;
                    }
            handle_x = samples ? handle_x / samples : club.w * 0.5f;
            // Carry it forwards below the jaw, not vertically beside the face.
            const float angle = left ? -1.15f : 1.15f;
            const float cosine = std::cos(angle), sine = std::sin(angle);
            const float handle_y = club.h - t.scale * 0.5f;
            enhance::HdAsset held;
            held.w = held.h = club.w + club.h + 2 * t.scale;
            held.px.resize(static_cast<std::size_t>(held.w) * held.h * 4, 0);
            const float centre = held.w * 0.5f;
            for (int yy = 0; yy < held.h; ++yy) for (int xx = 0; xx < held.w; ++xx) {
                const float dx = xx - centre, dy = yy - centre;
                const int sx = static_cast<int>(std::lround(handle_x + cosine * dx + sine * dy));
                const int sy = static_cast<int>(std::lround(handle_y - sine * dx + cosine * dy));
                if (sx < 0 || sy < 0 || sx >= club.w || sy >= club.h) continue;
                const auto src = (static_cast<std::size_t>(sy) * club.w + sx) * 4;
                const auto dst = (static_cast<std::size_t>(yy) * held.w + xx) * 4;
                std::copy_n(club.px.data() + src, 4, held.px.data() + dst);
            }
            blit_hd_block(t, held, x + grip_x - centre / t.scale,
                          y + grip_y - centre / t.scale,
                          hash.value() ^ UINT64_C(0x4341525259434c55));
        }
    }
    // Body is in front of the handle: the palm occludes it, rather than the
    // weapon floating over the character's head.
    blit_hd_block(t, hd_body, x, y, hash.value());
    static const auto start = std::chrono::steady_clock::now();
    const double time = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double blink = std::fmod(time, 4.2);
    const int frame = blink > 4.02 && blink < 4.10 ? 2 : blink > 3.98 && blink < 4.14 ? 1 : 0;
    const int light = std::clamp(static_cast<int>(brightness * 255), 0, 255);
    const auto id = std::make_tuple(frame, t.scale, left, light);
    auto found = heads.cache.find(id);
    if (found == heads.cache.end()) {
        const auto& r = heads.bounds[frame];
        if (r.empty()) return false;
        enhance::HdAsset head;
        head.w = 22 * t.scale; head.h = 16 * t.scale;
        head.px.resize(static_cast<std::size_t>(head.w) * head.h * 4);
        for (int yy = 0; yy < head.h; ++yy) for (int xx = 0; xx < head.w; ++xx) {
            const int sx = r.x0 + (left ? head.w - 1 - xx : xx) * (r.x1 - r.x0) / head.w;
            const int sy = r.y0 + yy * (r.y1 - r.y0) / head.h;
            const auto src = (static_cast<std::size_t>(sy) * heads.w + sx) * 4;
            const auto dst = (static_cast<std::size_t>(yy) * head.w + xx) * 4;
            for (int c = 0; c < 3; ++c) head.px[dst + c] = static_cast<std::uint8_t>(heads.pixels[src + c] * light / 255);
            head.px[dst + 3] = heads.pixels[src + 3];
        }
        found = heads.cache.emplace(id, std::move(head)).first;
    }
    blit_hd_block(t, found->second, x + (hx0 + hx1) * 0.5f - 11,
                  y + top - 2, UINT64_C(0x464f524748454144) + frame * 512 + (left ? 256 : 0) + light);
    return true;
}

void indexed_to_rgba(const std::vector<std::uint8_t>& pixels,
                     const std::vector<formats::Rgb>& pal, std::uint8_t* out,
                     std::size_t count) {
    count = std::min(count, pixels.size());
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t idx = pixels[i];
        const formats::Rgb c = idx < pal.size() ? pal[idx] : formats::Rgb{};
        out[i * 4] = c.r;
        out[i * 4 + 1] = c.g;
        out[i * 4 + 2] = c.b;
        out[i * 4 + 3] = 255;
    }
}

FrameBuffer pc1_frame(const formats::Pc1Image& img) {
    FrameBuffer fb;
    indexed_to_rgba(img.pixels, img.palette, fb.px.data(), 320u * 200u);
    return fb;
}

std::vector<std::uint8_t> sprite_to_rgba(const Sprite& s,
                                         const std::vector<Rgb>& pal,
                                         bool flip_h) {
    const int w = s.width, h = s.height;
    const auto pixels = s.decode_indexed();
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4, 0);
    for (int sy = 0; sy < h; ++sy)
        for (int sx = 0; sx < w; ++sx) {
            const auto& p =
                pixels[static_cast<std::size_t>(sy) * w +
                       static_cast<std::size_t>(flip_h ? (w - 1 - sx) : sx)];
            if (!p.opaque) continue;
            put_palette_pixel(&rgba[(static_cast<std::size_t>(sy) * w + sx) * 4],
                              pal, p.color);
        }
    return rgba;
}

void blit_sprite(RenderTarget& t, const Sprite& s,
                 const std::vector<Rgb>& pal, int x, int y, bool flip_h) {
    // Integer entry â†’ float core (round-trips exactly, positions < 2^24), so
    // every existing int caller stays byte-identical.
    blit_sprite(t, s, pal, static_cast<float>(x), static_cast<float>(y),
                flip_h);
}

void blit_sprite(RenderTarget& t, const Sprite& s,
                 const std::vector<Rgb>& pal, float fx, float fy, bool flip_h) {
    const int w = s.width, h = s.height;
    if (!t.hd_path()) {
        // Decoded here, not in the prologue: the HD path decodes inside
        // sprite_to_rgba() and would decode twice.
        const auto pixels = s.decode_indexed();
        // Classic path into a 320-wide buffer; native cannot show sub-pixels,
        // so round.
        const int x = static_cast<int>(std::lround(fx));
        const int y = static_cast<int>(std::lround(fy));
        for (int sy = 0; sy < h; ++sy) {
            const int dy = y + sy;
            if (dy < 0 || dy >= t.h || dy >= t.clip_y) continue;
            for (int sx = 0; sx < w; ++sx) {
                const auto& p =
                    pixels[static_cast<std::size_t>(sy) * w +
                           static_cast<std::size_t>(flip_h ? (w - 1 - sx)
                                                            : sx)];
                if (!p.opaque) continue;
                blit_pixel(t, x + sx + t.origin_x, dy, pal, p.color);
            }
        }
        return;
    }
    // HD: sprite_to_rgba() (palette + flip), cache upscale, blit at scaled
    // coordinates.  hd_warm.cpp hashes the same bytes (game_render.hpp).
    const std::uint64_t key =
        sprite_source_key(s, pal, flip_h ? kPlainFlipped : kPlain, t, true);
    const auto& hd = t.cache->get_by_source(
        key, [&] { return sprite_to_rgba(s, pal, flip_h); }, w, h, t.scale,
        *t.profile);
    blit_hd_block(t, hd, fx, fy, key);
}

void blit_sprite(FrameBuffer& fb, const Sprite& s,
                 const std::vector<Rgb>& pal, int x, int y, bool flip_h) {
    RenderTarget t{fb.px.data(), fb.w, fb.h, 1, nullptr, nullptr};
    blit_sprite(t, s, pal, x, y, flip_h);
}

void blit_sprite_keyed(RenderTarget& t, const Sprite& s,
                       const std::vector<Rgb>& pal, int x, int y) {
    // Integer entry to the float core; positions < 2^24 round-trip exactly.
    blit_sprite_keyed(t, s, pal, static_cast<float>(x), static_cast<float>(y));
}

void blit_sprite_keyed(RenderTarget& t, const Sprite& s,
                       const std::vector<Rgb>& pal, float fx, float fy) {
    const int w = s.width, h = s.height;
    if (!t.hd_path()) {
        const auto pixels = s.decode_indexed();
        const int bg_idx = key_colour(pixels);
        // Native target can't show sub-pixel â€” round to the nearest pixel.
        const int x = static_cast<int>(std::lround(fx));
        const int y = static_cast<int>(std::lround(fy));
        for (int sy = 0; sy < h; ++sy) {
            const int dy = y + sy;
            if (dy < 0 || dy >= t.h || dy >= t.clip_y) continue;
            for (int sx = 0; sx < w; ++sx) {
                const auto& p = pixels[static_cast<std::size_t>(sy) * w + sx];
                if (!p.opaque) continue;
                if (p.color == static_cast<std::uint8_t>(bg_idx))
                    continue;   // key out bg
                blit_pixel(t, x + sx + t.origin_x, dy, pal, p.color);
            }
        }
        return;
    }
    const std::uint64_t key = sprite_source_key(s, pal, kKeyed, t, false);
    const auto& hd = t.cache->get_by_source(
        key, [&] { return keyed_rgba(s, pal); }, w, h, t.scale, *t.profile,
        /*bleed=*/false);
    blit_hd_block(t, hd, fx, fy, key);
}

void blit_sprite_keyed(FrameBuffer& fb, const Sprite& s,
                       const std::vector<Rgb>& pal, int x, int y) {
    RenderTarget t{fb.px.data(), fb.w, fb.h, 1, nullptr, nullptr};
    blit_sprite_keyed(t, s, pal, x, y);
}

}  // namespace olduvai::presentation
