// SPDX-License-Identifier: MIT
// Texture upscaling on the CPU: the settings' names, the scaled copies' texels and levels, the worker cache. No GPU, no game data.
#include "bozo/render/texture_pack.h"
#include "bozo/render/texture_upscale.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
using namespace bozo::render;
namespace
{
    int failures = 0;
    void check(bool ok, const char *name)
    {
        if (!ok)
        {
            std::printf("FAIL %s\n", name);
            ++failures;
        }
    }
    Texture image(uint32_t width, uint32_t height, uint32_t (*colour)(uint32_t x, uint32_t y))
    {
        Texture t;
        t.width = width;
        t.height = height;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                t.rgba.push_back(colour(x, y));
        t.asset = true;
        return t;
    }
    uint32_t channel(uint32_t c, int n) { return (c >> (8 * n)) & 0xffu; }
    constexpr uint32_t kRed = 0x800000FFu, kBlue = 0x80FF0000u, kWhite = 0x80FFFFFFu;
    const TextureUpscale kEdge2{2, UpscaleFilter::Edge}, kEdge4{4, UpscaleFilter::Edge}, kSmooth2{2, UpscaleFilter::Smooth};
} // namespace
int main()
{
    {
        uint8_t f = 0;
        check(parseUpscaleFactor("off", f) && f == 1u, "off");
        check(parseUpscaleFactor("2x", f) && f == 2u, "2x");
        check(parseUpscaleFactor("4", f) && f == 4u, "4");
        check(!parseUpscaleFactor("3x", f) && f == 4u, "3x refused, the value kept");
        UpscaleFilter filter = UpscaleFilter::Edge;
        check(parseUpscaleFilter("smooth", filter) && filter == UpscaleFilter::Smooth, "smooth");
        check(!parseUpscaleFilter("xbrz", filter) && filter == UpscaleFilter::Smooth, "unknown filter refused");
        check(std::string_view(upscaleFactorName(1)) == "off" && std::string_view(upscaleFactorName(4)) == "4x", "factor names");
        check(!TextureUpscale{}.on() && kEdge2.on(), "off by default");
    }
    {
        Texture big = image(4096, 1, [](uint32_t, uint32_t) { return kRed; });
        check(upscaleFactorFor(big, 4) == 2u, "a 4096 texel side is scaled 2x at most");
        big.width = 8192;
        big.rgba.resize(8192, kRed);
        check(upscaleFactorFor(big, 2) == 1u, "an 8192 texel side is not scaled");
        Texture one = image(1, 1, [](uint32_t, uint32_t) { return kRed; });
        check(upscaleFactorFor(one, 1) == 1u, "factor 1");
        const Texture copy = upscaleTexture(one, TextureUpscale{}, false, false);
        check(copy.width == 1u && copy.rgba == one.rgba && copy.levels.empty(), "off: the texture as it is");
    }
    {
        Texture flat = image(4, 2, [](uint32_t, uint32_t) { return 0x40336699u; });
        flat.levels = {std::vector<uint32_t>(2, 0x40112233u), std::vector<uint32_t>(1, 0x40445566u)};
        flat.key = 77;
        for (const TextureUpscale &u : {kEdge2, kEdge4, kSmooth2})
        {
            const Texture out = upscaleTexture(flat, u, true, false);
            bool same = out.width == 4u * u.factor && out.height == 2u * u.factor && out.key == 77u && out.asset;
            for (uint32_t c : out.rgba)
                same = same && c == 0x40336699u;
            check(same, "a flat texture stays flat, texel for texel");
            const size_t k = u.factor == 4u ? 2u : 1u;
            check(out.levels.size() == k + 2u, "levels: the copy's, the texture's level 0, its own levels");
            check(out.levels[k - 1] == flat.rgba && out.levels[k] == flat.levels[0] && out.levels[k + 1] == flat.levels[1],
                  "the game's levels keep their content k levels down");
            if (u.factor == 4u)
                check(out.levels[0].size() == 8u * 4u && out.levels[0][5] == 0x40336699u, "4x: level 1 halved from level 0");
        }
    }
    {
        const Texture split = image(8, 8, [](uint32_t x, uint32_t) { return x < 4 ? kRed : kBlue; });
        const Texture out = upscaleTexture(split, kEdge4, false, false);
        bool sharp = true;
        for (uint32_t y = 0; y < out.height; ++y)
            for (uint32_t x = 0; x < out.width; ++x)
                if (x < 12 || x >= 20)
                    sharp = sharp && out.rgba[y * out.width + x] == (x < 16 ? kRed : kBlue);
        check(sharp, "edge: a straight edge blends within a texel of it only");
    }
    {
        const Texture stairs = image(8, 8, [](uint32_t x, uint32_t y) { return x > y ? kBlue : kWhite; });
        for (const TextureUpscale &u : {kEdge2, kEdge4})
        {
            const Texture out = upscaleTexture(stairs, u, false, false);
            int blended = 0, wrong = 0;
            for (uint32_t c : out.rgba)
            {
                if (c != kBlue && c != kWhite)
                    ++blended;
                if (channel(c, 2) != 0xFFu || (c >> 24) != 0x80u)
                    ++wrong;
            }
            check(blended > 0, "edge: a staircase's steps are smoothed");
            check(out.rgba != upscaleTexture(stairs, TextureUpscale{u.factor, UpscaleFilter::Smooth}, false, false).rgba,
                  "edge: the steps cut, not only interpolated");
            check(wrong == 0, "edge: only blue and white mixed");
            check(out.rgba[0] == kWhite && out.rgba[out.width - 1] == kBlue, "edge: the corners far from the diagonal kept");
            const uint32_t below = out.rgba[static_cast<size_t>(out.width) * (out.height - 1) + u.factor * 3];
            check(below == kWhite, "edge: far below the diagonal, white");
        }
    }
    {
        const auto disc = [](uint32_t x, uint32_t y)
        {
            const int dx = static_cast<int>(x) - 8, dy = static_cast<int>(y) - 8;
            return dx * dx + dy * dy < 30 ? kRed : 0x0000FF00u;
        };
        const Texture cutout = image(16, 16, disc);
        for (const TextureUpscale &u : {kEdge2, kEdge4, kSmooth2})
        {
            const Texture out = upscaleTexture(cutout, u, false, false);
            int halo = 0, partial = 0;
            for (uint32_t c : out.rgba)
            {
                if ((c >> 24) != 0u && channel(c, 1) != 0u)
                    ++halo;
                if ((c >> 24) != 0u && (c >> 24) != 0x80u)
                    ++partial;
                if ((c >> 24) > 0x80u)
                    ++halo;
            }
            check(halo == 0, "a transparent texel's colour never reaches a visible one");
            check(partial > 0, "the cutout's edge gets in-between alpha");
        }
    }
    {
        const auto pattern = [](uint32_t x, uint32_t y) { return ((x + 2 * y) % 5 < 2) ? kRed : kWhite; };
        const auto shifted = [](uint32_t x, uint32_t y) { return (((x + 1) % 10 + 2 * y) % 5 < 2) ? kRed : kWhite; };
        const Texture a = image(10, 10, pattern), b = image(10, 10, shifted);
        for (const TextureUpscale &u : {kEdge2, kSmooth2})
        {
            const Texture oa = upscaleTexture(a, u, true, true), ob = upscaleTexture(b, u, true, true);
            bool periodic = true;
            for (uint32_t y = 0; y < oa.height; ++y)
                for (uint32_t x = 0; x < oa.width; ++x)
                    periodic = periodic && ob.rgba[y * ob.width + x] == oa.rgba[y * oa.width + (x + u.factor) % oa.width];
            check(periodic, "repeat: the opposite edge is the neighbour (a shifted tile scales to the shifted copy)");
        }
    }
    {
        const Texture edges = image(6, 6, [](uint32_t x, uint32_t y) { return x == 5 ? kBlue : (x + y) % 3 == 0 ? kRed : kWhite; });
        for (const TextureUpscale &u : {kEdge2, kSmooth2})
        {
            const Texture out = upscaleTexture(edges, u, false, false);
            bool clean = true;
            for (uint32_t y = 0; y < out.height; ++y)
                clean = clean && channel(out.rgba[y * out.width], 0) == 0xFFu;
            check(clean, "clamp: the opposite edge's colour does not reach the first column");
            if (u.filter == UpscaleFilter::Smooth)
            {
                const Texture wrapped = upscaleTexture(edges, u, true, false);
                bool reached = false;
                for (uint32_t y = 0; y < wrapped.height; ++y)
                    reached = reached || channel(wrapped.rgba[y * wrapped.width], 0) != 0xFFu;
                check(reached, "repeat: it does");
            }
        }
    }
    {
        Texture noisy = image(9, 7, [](uint32_t x, uint32_t y) { return 0x80000000u | ((x * 97u + y * 31u) * 2654435761u >> 8 & 0xFFFFFFu); });
        const Texture out = upscaleTexture(noisy, kSmooth2, false, false);
        bool held = true;
        for (uint32_t y = 0; y < out.height; ++y)
            for (uint32_t x = 0; x < out.width; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    uint32_t low = 255, high = 0;
                    for (int j = -1; j <= 1; ++j)
                        for (int i = -1; i <= 1; ++i)
                        {
                            const int sx = std::clamp(static_cast<int>(x / 2) + i, 0, 8), sy = std::clamp(static_cast<int>(y / 2) + j, 0, 6);
                            low = std::min(low, channel(noisy.rgba[sy * 9 + sx], c));
                            high = std::max(high, channel(noisy.rgba[sy * 9 + sx], c));
                        }
                    const uint32_t v = channel(out.rgba[y * out.width + x], c);
                    held = held && v + 1u >= low && v <= high + 1u;
                }
        check(held, "smooth: no ringing past the nearest texels");
    }
    {
        packs::TextureRecord record;
        record.width = record.height = 1;
        record.psm = 0;
        record.texels = {0x11, 0x22, 0x33, 0x44};
        const uint32_t palette[256] = {};
        Texture decoded;
        packs::decodeRecord(decoded, 1, 1, 0, record, palette, 0);
        check(decoded.asset, "a texture decoded from a pack is an asset");
    }
    {
        TextureUpscaler &upscaler = TextureUpscaler::instance();
        auto source = std::make_shared<const Texture>(image(12, 12, [](uint32_t x, uint32_t y) { return x > y + 2 ? kBlue : kRed; }));
        check(upscaler.find(source, kEdge2, false, true) == nullptr, "queued: nothing yet");
        upscaler.waitIdle();
        const auto made = upscaler.find(source, kEdge2, false, true);
        const Texture expected = upscaleTexture(*source, kEdge2, false, true);
        check(made && made->rgba == expected.rgba && made->levels == expected.levels, "the worker's copy is the one made here");
        const uint64_t before = upscaler.stats().made;
        auto twin = std::make_shared<const Texture>(*source);
        check(upscaler.find(twin, kEdge2, false, true) == made, "the same texels from another texture: the same copy");
        check(upscaler.find(source, kEdge2, true, true) == nullptr, "another wrap: another copy");
        upscaler.waitIdle();
        check(upscaler.stats().made == before + 1u, "one copy made per content and settings");
        upscaler.setBudget(0);
        check(upscaler.stats().bytes == 0u && upscaler.stats().dropped >= 2u, "a budget of 0 keeps nothing");
        check(made->rgba == expected.rgba, "a copy in use lives on after it is dropped");
        check(upscaler.find(source, kEdge2, false, true) == nullptr, "dropped: made again");
        upscaler.waitIdle();
        upscaler.setBudget(size_t(256) << 20);
    }
    std::printf("texture upscale: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
