// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// A PNG reader with no dependency (texture replacements, bozo/render/texture_replace.h): written from the PNG
// specification and RFC 1950 / 1951 (zlib, deflate: stored, fixed and dynamic Huffman blocks). 8-bit images of every
// colour type (grey, RGB, palette with or without tRNS, grey + alpha, RGBA), not interlaced, all five row filters.
// 16-bit, sub-byte and interlaced images are refused with a message.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bozo::render
{
    // rgba: width x height texels, R in the low byte, rows top-down; alpha 255 where the image has none.
    bool decodePng(const uint8_t *data, size_t size, uint32_t &width, uint32_t &height, std::vector<uint32_t> &rgba,
                   std::string *error = nullptr);
    bool readPng(const std::string &path, uint32_t &width, uint32_t &height, std::vector<uint32_t> &rgba,
                 std::string *error = nullptr);
}
