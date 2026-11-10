// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// A texture as the native renderer takes it: RGBA8 texels a port decoded from its game's own data (texture packs,
// EE memory). Moved from the fork's native_texture_cache.h (its Texture struct and csm1Position only: the GS-memory
// cache itself is emulation and stays behind, phase4-inventory.md section 2.1).
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace bozo::render
{
    struct Texture
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint32_t> rgba; // R in the low byte; alpha scaled from the GS's 0x80 = opaque to 255
        uint64_t key = 0;
        uint64_t version = 0; // new for every decode: a renderer re-sends the image to its GPU when it changes
        std::vector<std::pair<uint32_t, uint32_t>> pages; // (page, generation) the texture was decoded from
        // Mip levels 1.. as the game stores them, each max(1, width >> n) x max(1, height >> n), same texel layout as rgba.
        std::vector<std::vector<uint32_t>> levels;
        // Decoded from the game's own texture data (a pack, the disc), not a picture made at run time: texture upscaling may
        // scale it (bozo/render/texture_upscale.h).
        bool asset = false;
    };

    // Index of palette entry `index` in a CSM1 palette stored as a 16-pixel-wide CT32 / CT16 image: (x, y).
    inline void csm1Position(uint32_t index, uint32_t &x, uint32_t &y)
    {
        x = (index & 7u) | ((index & 0x10u) >> 1);
        y = ((index >> 5) << 1) | ((index >> 3) & 1u);
    }
}
