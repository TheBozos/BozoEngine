// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// A PNG writer with no dependency (window captures): 8-bit RGB or RGBA, rows top-down, the image data in stored
// (uncompressed) deflate blocks, so no zlib is needed.
#pragma once

#include <cstdint>
#include <string>

namespace bozo::app
{
    // `pixels`: width x height pixels of 4 bytes (R, G, B, A), rows top-down. alpha false: written as RGB.
    bool writePng(const std::string &path, const uint8_t *pixels, uint32_t width, uint32_t height, bool alpha = false);
}
