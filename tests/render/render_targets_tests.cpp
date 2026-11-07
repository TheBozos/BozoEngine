// SPDX-License-Identifier: MIT
// GPU readback fixtures for native render-target operations. No game data.
#include "bozo/render/gl.h"
#include "bozo/render/renderer.h"
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <cstdlib>
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
