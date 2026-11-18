// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// Texture replacements, a player's option: a folder of PNG files, each named by the identity of the texture it replaces
// (16 hex digits, textureIdentity below: the value the draw-list hash gives that texture), drawn instead of it. A port's
// texture assets only (Texture::asset). The folder is the player's own: made on their PC (an upscaler run over a dump of
// their game's textures, or hand edits); nothing of a game ships with the engine.
//
// - Size: the texture's own, or 2, 4 or 8 times it each way (the same factor both ways, at most kUpscaleMaxSide). Any
//   other is refused once, with a message. A larger copy gets the upscaled copy's mip levels (texture_upscale.h): levels
//   1..k-1 halved from it, level k the texture's level 0 as it is, then its own levels; the present side moves a level
//   the game chose k levels down.
// - Texels: as the port's Texture holds them (alpha as the port decoded it: the GS's 0x80 is 1.0 for a PS2 game's).
// - Loading: worker threads read and decode the files; until a replacement is ready the texture is drawn as it is (or
//   from its upscaled copy). Nothing waits for a file on the thread that draws.
// - A replacement takes precedence over texture upscaling.
#pragma once

#include "bozo/render/texture.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace bozo::render
{
    // XXH64 of the texture's level-0 RGBA, seeded with XXH64 of {width, height}: the draw-list hash's texture identity.
    uint64_t textureIdentity(const Texture &texture);

    class TextureReplacements
    {
    public:
        static TextureReplacements &instance();

        // The folder of <identity>.png files (empty: none). Its files are listed now; set again to list them again.
        void setFolder(const std::string &folder);
        std::string folder() const;
        bool active() const;
        // Lists the folder again and drops loaded replacements whose file changed or went (a development hot reload);
        // returns whether anything changed (generation() then moves on).
        bool rescan();
        uint64_t generation() const;

        // The replacement of this texture: its copy when loaded; null with `loading` set while a worker reads it (queued on
        // the first call); null without it when there is none (no file, or one refused).
        std::shared_ptr<const Texture> find(const std::shared_ptr<const Texture> &source, bool &loading);

        // Tests and checks: returns once the queue is empty and no worker is busy.
        void waitIdle();
        void setBudget(size_t bytes); // loaded copies kept on the CPU, least recently used dropped first (512 MB)
        struct Stats
        {
            uint64_t files = 0, loaded = 0, refused = 0, found = 0;
            double seconds = 0.0; // workers' time reading and decoding
            size_t bytes = 0;     // copies kept
        };
        Stats stats() const;

    private:
        TextureReplacements();
        struct State;
        std::shared_ptr<State> m_state;
    };
}
