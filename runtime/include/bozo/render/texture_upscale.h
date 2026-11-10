// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// Texture upscaling, a player's option (off by default): a port's texture assets (Texture::asset, decoded from its game's
// own data) drawn from a copy 2 or 4 times as large each way, made on the CPU from the texels after a palette was applied.
// Pictures made at run time (movie frames, frame reads, render targets) are never scaled.
//
// Filters, both written for BozoEngine from the published descriptions of the methods (no code taken from anywhere):
// - Edge: edge-directed, in the family of Hyllian's xBR. For each corner of a texel, the colour differences along the two
//   diagonals of its 5x5 neighbourhood decide whether an edge cuts that corner, and whether it is at 45 degrees, shallow or
//   steep; the corner then takes the neighbouring colour by the area the edge's line covers in each output texel (exact
//   coverage, so any power-of-two factor works). Straight edges and flat areas stay as they are, texel for texel.
// - Smooth: Catmull-Rom bicubic, each value held within its four nearest texels (no ringing).
// Both blend with premultiplied alpha, so a transparent texel's colour never bleeds into a visible one (no halo at a
// cutout's edge); a texture that repeats is scaled with the opposite edge as its neighbour, one that clamps with its own.
//
// Mip levels: the copy's levels 1..k-1 (k = log2 factor) are halved from its level 0, level k is the texture's level 0 as it
// is and the texture's own levels follow, so the game's levels keep their content k levels down (the present side adds k to
// a level the game chose, from Q or fixed).
#pragma once

#include "bozo/render/texture.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace bozo::render
{
    enum class UpscaleFilter : uint8_t
    {
        Edge,
        Smooth,
    };

    struct TextureUpscale
    {
        uint8_t factor = 1; // 1 (off), 2 or 4
        UpscaleFilter filter = UpscaleFilter::Edge;
        bool on() const { return factor > 1u; }
        bool operator==(const TextureUpscale &other) const { return factor == other.factor && filter == other.filter; }
        bool operator!=(const TextureUpscale &other) const { return !(*this == other); }
    };

    // "off" (or 1), "2x" (or 2), "4x" (or 4); false for anything else.
    bool parseUpscaleFactor(std::string_view text, uint8_t &factor);
    // "edge" or "smooth".
    bool parseUpscaleFilter(std::string_view text, UpscaleFilter &filter);
    const char *upscaleFactorName(uint8_t factor);
    const char *upscaleFilterName(UpscaleFilter filter);

    // The largest side a scaled copy may have: a texture that would pass it is scaled by a lower factor (1: not at all).
    constexpr uint32_t kUpscaleMaxSide = 8192;
    uint8_t upscaleFactorFor(const Texture &source, uint8_t factor);

    // The scaled copy (asset, key and pages as the source's); wrapU / wrapV: the texture repeats that way. With a factor of
    // 1 (or one upscaleFactorFor lowers to 1), a copy of the source.
    Texture upscaleTexture(const Texture &source, TextureUpscale upscale, bool wrapU, bool wrapV);

    // Scaled copies made by worker threads, kept by content (the same texels from another Texture object find the same
    // copy) up to a memory budget (256 MB unless set), least recently used dropped first. A copy sent to the GPU needs no
    // place here: the budget only spares remaking one for another Texture object with the same texels.
    class TextureUpscaler
    {
    public:
        static TextureUpscaler &instance();
        // The copy when it is made; null while it is being made (queued on the first call).
        std::shared_ptr<const Texture> find(const std::shared_ptr<const Texture> &source, TextureUpscale upscale, bool wrapU, bool wrapV);
        // Tests and checks: returns once the queue is empty and no worker is busy.
        void waitIdle();
        void setBudget(size_t bytes);
        struct Stats
        {
            uint64_t made = 0, found = 0, queued = 0, dropped = 0;
            double seconds = 0.0; // workers' time making copies
            size_t bytes = 0;     // copies kept
        };
        Stats stats() const;

    private:
        TextureUpscaler();
        struct State;
        std::shared_ptr<State> m_state;
    };
}
