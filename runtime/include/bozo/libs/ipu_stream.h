// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// A game's own IPU streams, decoded natively: the ".ipu" files Sony's IPU sample format defines (header "ipum", u32 file
// size, u16 width, u16 height, u32 frame count, then the frames' IPU bitstream: intra-only MPEG macroblocks). A game
// that drives the IPU itself (BDEC/IDEC commands, the to-IPU and from-IPU DMA channels; JC2's animated textures) can
// answer its decode routine with this instead: no IPU, no DMA. FFmpeg's "ipu" parser splits the frames and its decoder
// decodes them (libavcodec, the same build as libmpeg's; without FFmpeg open() fails and the port keeps its own path).
//
// The output is what the IPU's colour conversion (CSC, RGB32) writes: per 16 x 16 macroblock, in raster order of the
// macroblocks, its 256 pixels row by row, 4 bytes R, G, B, A; or, with decodeRaster, the picture's rows.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bozo::libs
{
    class IpuStream
    {
    public:
        IpuStream();
        ~IpuStream();
        IpuStream(const IpuStream &) = delete;
        IpuStream &operator=(const IpuStream &) = delete;

        // Whether this build can decode IPU streams (FFmpeg with its "ipu" decoder and parser).
        static bool available();

        // The whole file in memory (copied). False: not an "ipum" file, or no decoder.
        bool open(const uint8_t *file, size_t bytes);
        bool isOpen() const;
        int width() const;
        int height() const;
        int frames() const; // the frames found in the stream (the header's count when they can all be split)

        // Frame `index` (0-based, any order: IPU frames are intra-only) as RGB32 macroblocks (the CSC's layout above),
        // width x height x 4 bytes; alpha is the value given (the CSC's 0x80 when the colour is not under the threshold).
        // False: no such frame, or it did not decode (the caller keeps its previous picture).
        bool decodeMacroblocks(int index, uint8_t *out, uint8_t alpha = 0x80);
        // The same frame as rows of RGBA pixels (width x height x 4 bytes).
        bool decodeRaster(int index, uint8_t *out, uint8_t alpha = 0x80);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
