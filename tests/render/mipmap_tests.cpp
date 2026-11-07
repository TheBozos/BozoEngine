// SPDX-License-Identifier: MIT
// Material::Mipmap from TEX1, and the draw-list digest with and without it. No GPU, no game data.
#include "bozo/render/renderer.h"
#include <cmath>
#include <cstdio>
#include <memory>
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
    uint64_t digest(const Material &m, std::shared_ptr<const Texture> texture = nullptr)
    {
        static constexpr float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Renderer &renderer = Renderer::instance();
        renderer.takeFrameDraws();
        renderer.beginFrame(Screen{0, 0, 16, 16});
        Vertex v[3];
        v[1].x = 16, v[2].y = 16;
        renderer.draw(m, identity, v, 3, std::move(texture));
        renderer.endFrame();
        return renderer.takeFrameDraws().hash;
    }
} // namespace
int main()
{
    {
        const Material::Mipmap off = mipmapOf(0x61u);
        check(off.levels == 0u, "MXL 0: level 0 only");
        const Material::Mipmap single = mipmapOf(0x40u | (1u << 2));
        check(single.levels == 0u, "MMIN LINEAR with MXL 1: no mip filter, level 0 only");
    }
    {
        const Material::Mipmap m = mipmapOf(0xFE700000128ull);
        check(m.levels == 2u, "MXL 2");
        check(m.linearTexel && !m.linearLevels, "MMIN 4: LINEAR_MIPMAP_NEAREST");
        check(m.lod == Material::Mipmap::Lod::FromQ, "LCM 0: from Q");
        check(m.shift == 0u, "L 0");
        check(std::fabs(m.bias + 1.5625f) < 1e-6f, "K -1.5625 (0xFE7, 4 fraction bits)");
    }
    {
        const uint64_t tex1 = 1u | (6u << 2) | (5u << 6) | (2ull << 19) | (0x024ull << 32);
        const Material::Mipmap m = mipmapOf(tex1);
        check(m.levels == 6u && m.linearTexel && m.linearLevels, "MXL 6, MMIN 5: LINEAR_MIPMAP_LINEAR");
        check(m.lod == Material::Mipmap::Lod::Fixed && m.shift == 2u, "LCM 1: fixed, L 2");
        check(std::fabs(m.bias - 2.25f) < 1e-6f, "K +2.25");
        const Material::Mipmap n = mipmapOf((2u << 6) | (7u << 2));
        check(n.levels == 6u, "MXL 7 held at 6");
        check(!n.linearTexel && !n.linearLevels, "MMIN 2: NEAREST_MIPMAP_NEAREST");
        check(mipmapOf(3u << 6 | 4u).linearLevels && !mipmapOf(3u << 6 | 4u).linearTexel, "MMIN 3: NEAREST_MIPMAP_LINEAR");
        check(mipmapOf(6u << 6 | 4u).levels == 0u, "MMIN 6: not a filter, level 0 only");
    }
    {
        Material plain;
        auto texture = std::make_shared<Texture>();
        texture->width = texture->height = 2;
        texture->rgba = {1, 2, 3, 4};
        const uint64_t base = digest(plain, texture);
        Material unused = plain;
        unused.mipmap.bias = 3.0f;
        check(digest(unused, texture) == base, "a mipmap with no levels leaves the digest as it was");
        Material mipped = plain;
        mipped.mipmap = mipmapOf(0xFE700000128ull);
        check(digest(mipped, texture) != base, "mip levels change the digest");
        auto withLevels = std::make_shared<Texture>(*texture);
        withLevels->levels = {{9}};
        check(digest(plain, withLevels) != base, "a texture's own levels change the digest");
        auto copy = std::make_shared<Texture>(*texture);
        check(digest(plain, copy) == base, "the same texels, no levels: the same digest");
    }
    std::printf("mipmap: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
