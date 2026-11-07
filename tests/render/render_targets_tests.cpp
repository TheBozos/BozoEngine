// SPDX-License-Identifier: MIT
// GPU readback fixtures for native render-target operations. No game data.
#include "bozo/render/gl.h"
#include "bozo/render/renderer.h"
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace bozo::render;
namespace
{
    int failures = 0;
    constexpr float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Screen screen{0, 0, 16, 16};
    Renderer &renderer = Renderer::instance();
    void quad(Material m, uint32_t colour, float y0 = 0, float y1 = 16, float z = 1)
    {
        Vertex v[6];
        const float xy[6][2] = {{0, y0}, {16, y0}, {0, y1}, {16, y0}, {16, y1}, {0, y1}};
        for (int i = 0; i < 6; ++i)
        {
            v[i].x = xy[i][0];
            v[i].y = xy[i][1];
            v[i].z = z;
            v[i].u = xy[i][0] / 16;
            v[i].v = xy[i][1] / 16;
            v[i].rgba = colour;
        }
        renderer.draw(m, identity, v, 6);
    }
    void pixel(const char *name, int y, std::array<int, 3> expected)
    {
        uint8_t rgba[4]{};
        gl::api().ReadPixels(16, y, 1, 1, gl::kRgba, gl::kUnsignedByte, rgba);
        for (int c = 0; c < 3; ++c)
            if (std::abs(int(rgba[c]) - expected[c]) > 3)
            {
                std::printf("FAIL %s: RGB %u,%u,%u expected %d,%d,%d\n", name, rgba[0], rgba[1], rgba[2], expected[0],
                            expected[1], expected[2]);
                ++failures;
                break;
            }
    }
    void present()
    {
        renderer.endFrame();
        if (!renderer.present(0, 0, 32, 32))
        {
            ++failures;
            std::puts("FAIL present");
        }
    }
} // namespace
int main()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::puts(SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    auto *window = SDL_CreateWindow("Bozo native render tests", 32, 32, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window)
    {
        std::puts(SDL_GetError());
        return 1;
    }
    const auto context = SDL_GL_CreateContext(window);
    if (!context || !gl::load([](const char *name) { return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name)); }))
        return 1;
    gl::api().Viewport(0, 0, 32, 32);
    renderer.setEnabled(true);
    renderer.setPresentsAlone(true);
    Material plain;
    plain.textured = false;
    plain.depthTest = false;
    plain.depthWrite = false;
    plain.blend = Material::Blend::Opaque;
    Material copy = plain;
    copy.fromFrame = 1;
    copy.nearest = true;
    renderer.setTargetSize(1, 8, 8);
    renderer.beginFrame(screen);
    auto target = plain;
    target.target = 1;
    quad(target, 0x80000080, 0, 8);
    quad(target, 0x80800000, 8, 16);
    copy.source = 1;
    quad(copy, 0x80808080);
    present();
    pixel("auxiliary top stays top", 24, {255, 0, 0});
    pixel("auxiliary bottom stays bottom", 8, {0, 0, 255});
    renderer.beginFrame(screen);
    copy.source = Material::kPreviousFrame;
    quad(copy, 0x80808080);
    present();
    pixel("previous frame", 24, {255, 0, 0});
    renderer.present(0, 0, 32, 32);
    pixel("repeated presentation preserves history", 8, {0, 0, 255});
    renderer.beginFrame(screen);
    quad(plain, 0x80008000);
    copy.source = 0;
    quad(copy, 0x80808080);
    present();
    pixel("copy current target", 16, {0, 255, 0});
    renderer.beginFrame(screen);
    quad(plain, 0x80000040);
    auto alpha = plain;
    alpha.blend = Material::Blend::AlphaOnly;
    quad(alpha, 0x40000000);
    auto add = plain;
    add.blend = Material::Blend::AdditiveByDestination;
    quad(add, 0x80008000);
    present();
    pixel("alpha-only then destination-alpha blend", 16, {128, 128, 0});
    renderer.beginFrame(screen);
    quad(plain, 0x80000040);
    auto destination = plain;
    destination.blend = Material::Blend::DestinationBySource;
    quad(destination, 0x40004000);
    present();
    pixel("source plus destination times source alpha", 16, {64, 128, 0});
    // The same asynchronous native depth cache used by game visibility queries. A real
    // depth-writing primitive must be distinguishable from the next frame's empty depth.
    Screen depthScreen = screen;
    depthScreen.depthMax = 1000;
    auto depthMaterial = plain;
    depthMaterial.depthTest = depthMaterial.depthWrite = true;
    renderer.beginFrame(depthScreen);
    quad(depthMaterial, 0x80808080, 0, 16, 500);
    renderer.wantDepth();
    present();
    auto expectDepth = [&](uint32_t expected)
    {
        uint32_t z = ~0u;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            renderer.wantDepth();
            const bool available = renderer.depthAt(8, 8, z);
            if (available && std::abs(int(z) - int(expected)) <= 1)
                return;
            renderer.present(0, 0, 32, 32);
            SDL_GL_SwapWindow(window);
            SDL_Delay(2);
        }
        std::printf("FAIL native visibility depth: %u, expected %u\n", z, expected);
        ++failures;
    };
    expectDepth(500);
    renderer.beginFrame(depthScreen);
    quad(plain, 0x80808080);
    present();
    expectDepth(0);
    // The GS's own blend, (A - B) * C + D, with FIX and on the frame so far.
    const auto gsBlend = [](uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t fix = 0)
    {
        Material m = Material{};
        m.textured = false;
        m.depthTest = m.depthWrite = false;
        m.gsBlend = true;
        m.blendA = a, m.blendB = b, m.blendC = c, m.blendD = d, m.blendFix = fix;
        return m;
    };
    Material flat;
    flat.textured = false;
    flat.blend = Material::Blend::Opaque;
    flat.depthTest = flat.depthWrite = false;
    renderer.beginFrame(screen);
    quad(flat, 0x80404040); // 0x40 is half of 1.0: 128 on the target
    quad(gsBlend(0, 2, 2, 1, 0x40), 0x80404040);
    present();
    pixel("GS blend Cs * fix + Cd", 8, {192, 192, 192});
    renderer.beginFrame(screen);
    quad(flat, 0x80808080);
    quad(gsBlend(2, 0, 2, 1, 0x40), 0x80808080);
    present();
    pixel("GS blend Cd - Cs * fix", 8, {128, 128, 128});
    renderer.beginFrame(screen);
    quad(flat, 0x80808080);
    quad(gsBlend(1, 2, 0, 2), 0x40FFFFFF);
    present();
    pixel("GS blend Cd * As", 8, {128, 128, 128});
    renderer.beginFrame(screen);
    quad(flat, 0x80404040);
    quad(gsBlend(0, 1, 0, 1), 0x40808080);
    present();
    pixel("GS blend (Cs - Cd) * As + Cd", 8, {192, 192, 192});

    // TEX0.TFX and TCC, on the frame so far as the texture: 128 on the target, Cf 0x40 (0.5), Af 0x20.
    const auto sampled = [&flat](Material::TextureFunction function, bool textureAlpha = true)
    {
        Material m = flat;
        m.textured = true;
        m.fromFrame = 1u;
        m.nearest = true;
        m.textureFunction = function;
        m.textureAlpha = textureAlpha;
        return m;
    };
    const auto function = [&](const char *name, Material m, uint32_t colour, std::array<int, 3> expected)
    {
        renderer.beginFrame(screen);
        quad(flat, 0x80404040);
        quad(m, colour);
        present();
        pixel(name, 8, expected);
    };
    function("TFX modulate", sampled(Material::TextureFunction::Modulate), 0x20404040, {64, 64, 64});
    function("TFX decal", sampled(Material::TextureFunction::Decal), 0x20404040, {128, 128, 128});
    function("TFX highlight", sampled(Material::TextureFunction::Highlight), 0x20404040, {96, 96, 96});
    function("TFX highlight2, per channel", sampled(Material::TextureFunction::Highlight2), 0x20802040, {96, 64, 160});
    {
        Material m = sampled(Material::TextureFunction::Highlight2, false);
        m.blend = Material::Blend::Alpha;
        m.gsBlend = true;
        m.blendA = 0, m.blendB = 1, m.blendC = 0, m.blendD = 1;
        function("TCC off: the vertex's alpha blends", m, 0x20808080, {128 + 8, 128 + 8, 128 + 8});
    }

    // FBMSK for the channels: only green written.
    renderer.beginFrame(screen);
    quad(flat, 0x80000000);
    {
        Material m = flat;
        m.colourMask = 2u;
        quad(m, 0x80808080);
    }
    present();
    pixel("colour mask", 8, {0, 255, 0});

    // TEST: a draw passes by alpha as its function says, and by GREATER depth.
    renderer.beginFrame(screen);
    quad(flat, 0x80000000);
    {
        Material m = flat;
        m.alphaTest = true;
        m.alphaFunc = Material::AlphaFunc::Less;
        m.alphaRef = 0x40;
        quad(m, 0x80808080);
        quad(m, 0x20008000, 0, 8);
    }
    present();
    pixel("alpha test LESS: the alpha above the reference fails", 8, {0, 0, 0});
    pixel("alpha test LESS: the alpha below the reference passes", 24, {0, 255, 0});

    Screen screenDepth = screen;
    screenDepth.depthMax = 1000.0f;
    renderer.beginFrame(screenDepth);
    {
        Material m = flat;
        m.depthTest = m.depthWrite = true;
        quad(m, 0x80000080, 0, 16, 500.0f);
        m.depthGreater = true;
        quad(m, 0x80008000, 0, 16, 500.0f);
    }
    present();
    pixel("depth GREATER: an equal Z fails", 8, {255, 0, 0});

    // The stencil: a volume counted up and down marks where the counts differ; a draw then lands only there.
    renderer.beginFrame(screen);
    quad(flat, 0x80000000);
    {
        Material volume = flat;
        volume.colourMask = 0u;
        volume.stencilClear = true;
        volume.stencilOp = Material::StencilOp::IncrementWrap;
        quad(volume, 0x80808080);
        volume.stencilClear = false;
        volume.stencilOp = Material::StencilOp::DecrementWrap;
        quad(volume, 0x80808080, 0, 8);
        Material marked = flat;
        marked.stencilNotZero = true;
        quad(marked, 0x80008000);
    }
    present();
    pixel("stencil 0: not drawn", 24, {0, 0, 0});
    pixel("stencil not 0: drawn", 8, {0, 255, 0});

    // Q: a screen-space triangle's texture coordinates in perspective, its colour across the screen.
    {
        auto checker = std::make_shared<Texture>();
        checker->width = 2;
        checker->height = 1;
        checker->rgba = {0x800000FFu, 0x80FF0000u};
        checker->key = 0x5151u;
        Material m;
        m.depthTest = m.depthWrite = false;
        m.blend = Material::Blend::Opaque;
        m.useQ = true;
        m.nearest = true;
        m.clampU = m.clampV = true;
        renderer.beginFrame(screen);
        Vertex v[3];
        v[0].x = 0, v[0].y = 0, v[0].u = 0, v[0].q = 0.125f;
        v[1].x = 16, v[1].y = 0, v[1].u = 1, v[1].q = 1.0f;
        v[2].x = 0, v[2].y = 16, v[2].u = 0, v[2].q = 0.125f;
        for (Vertex &vertex : v)
            vertex.rgba = 0x80808080;
        // With Q 1/8 at the left and 1 at the right, u = 0.5 is a ninth of the way along the top edge (3.5 window pixels):
        // an affine interpolation would have it in the middle.
        renderer.draw(m, identity, v, 3, checker);
        present();
    }
    {
        uint8_t rgba[4] = {};
        const int left = 1, right = 8, y = 31;
        gl::api().ReadPixels(left, y, 1, 1, gl::kRgba, gl::kUnsignedByte, rgba);
        if (rgba[0] < 200)
        {
            std::printf("FAIL Q left of the middle: red %u\n", rgba[0]);
            ++failures;
        }
        gl::api().ReadPixels(right, y, 1, 1, gl::kRgba, gl::kUnsignedByte, rgba);
        if (rgba[2] < 200)
        {
            std::printf("FAIL Q right of the middle: blue %u\n", rgba[2]);
            ++failures;
        }
    }

    // Mip levels: the texture's own (level 0 red, 1 green, 2 blue), or made from level 0 (half red, half blue: 2 is purple).
    {
        auto own = std::make_shared<Texture>();
        own->width = own->height = 4;
        own->rgba.assign(16, 0x800000FFu);
        own->levels = {std::vector<uint32_t>(4, 0x8000FF00u), std::vector<uint32_t>(1, 0x80FF0000u)};
        own->key = 0x3170u;
        auto made = std::make_shared<Texture>();
        made->width = made->height = 4;
        for (int i = 0; i < 16; ++i)
            made->rgba.push_back((i & 3) < 2 ? 0x800000FFu : 0x80FF0000u);
        made->key = 0x3171u;
        Material m;
        m.depthTest = m.depthWrite = false;
        m.blend = Material::Blend::Opaque;
        const auto mipQuad = [&](const Material &material, const std::shared_ptr<const Texture> &texture, float q = 1.0f)
        {
            Vertex v[6];
            const float xy[6][2] = {{0, 0}, {16, 0}, {0, 16}, {16, 0}, {16, 16}, {0, 16}};
            for (int i = 0; i < 6; ++i)
            {
                v[i].x = xy[i][0];
                v[i].y = xy[i][1];
                v[i].z = 1;
                v[i].u = xy[i][0] / 16;
                v[i].v = xy[i][1] / 16;
                v[i].q = q;
                v[i].rgba = 0x80808080;
            }
            renderer.beginFrame(screen);
            renderer.draw(material, identity, v, 6, texture);
            present();
        };
        mipQuad(m, own);
        pixel("mipmap off: level 0", 16, {255, 0, 0});
        Material fixed = m;
        fixed.mipmap.levels = 2;
        fixed.mipmap.linearTexel = false;
        fixed.mipmap.lod = Material::Mipmap::Lod::Fixed;
        fixed.mipmap.bias = 1.0f;
        mipQuad(fixed, own);
        pixel("fixed LOD 1: the texture's level 1", 16, {0, 255, 0});
        fixed.mipmap.bias = 2.0f;
        mipQuad(fixed, own);
        pixel("fixed LOD 2: the texture's level 2", 16, {0, 0, 255});
        fixed.mipmap.levels = 1;
        mipQuad(fixed, own);
        pixel("LOD past MXL: the last level allowed", 16, {0, 255, 0});
        Material fromQ = m;
        fromQ.useQ = true;
        fromQ.mipmap = mipmapOf((1ull << 2 | 1ull << 3) | (2ull << 6) | (uint64_t(0x1000 - 16) << 32));
        mipQuad(fromQ, own, 0.25f);
        pixel("Q 1/4, K -1: LOD 1", 16, {0, 255, 0});
        mipQuad(fromQ, own, 1.0f);
        pixel("Q 1, K -1: level 0", 16, {255, 0, 0});
        Material screenLod = m;
        screenLod.mipmap.levels = 2;
        screenLod.mipmap.bias = 8.0f;
        mipQuad(screenLod, own);
        pixel("screen LOD with a large bias: the coarsest level", 16, {0, 0, 255});
        fixed.mipmap.levels = 2;
        fixed.mipmap.bias = 2.0f;
        mipQuad(fixed, made);
        pixel("levels made at upload: level 2 the average", 16, {128, 0, 128});
    }

    // A native depth mask writes independently of testing. The far half must
    // overwrite a previous near clear; the tested draw must then keep its usual
    // comparison. Cover the main and auxiliary targets and an inverted reuse.
    auto writeOnly = plain;
    writeOnly.depthWrite = true;
    for (uint8_t targetId : {uint8_t(0), uint8_t(2)})
        for (int inverted = 0; inverted < 2; ++inverted)
        {
            renderer.beginFrame(depthScreen);
            auto clearMask = writeOnly;
            clearMask.target = targetId;
            quad(clearMask, 0x80000080, 0, 16, 1000);
            quad(clearMask, 0x80800000, inverted ? 8 : 0, inverted ? 16 : 8, 0);
            auto tested = depthMaterial;
            tested.target = targetId;
            quad(tested, 0x80008000, 0, 16, 0);
            if (targetId)
            {
                copy.source = targetId;
                quad(copy, 0x80808080);
            }
            present();
            pixel("unconditional far write opens only the mask", inverted ? 8 : 24, {0,255,0});
            pixel("tested draw preserves the near clear outside mask", inverted ? 24 : 8, {255,0,0});
        }
    renderer.releaseGpu();
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf("native render targets: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
