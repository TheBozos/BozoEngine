// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The PNG writer (bozo/app/png.h): the PNG specification's chunks (IHDR, IDAT, IEND, each with its CRC-32), the image
// data a zlib stream of stored deflate blocks (RFC 1950 / 1951) with its Adler-32.
#include "bozo/app/png.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace bozo::app
{
    namespace
    {
        const std::array<uint32_t, 256> &crcTable()
        {
            static const std::array<uint32_t, 256> table = []
            {
                std::array<uint32_t, 256> t{};
                for (uint32_t n = 0; n < 256; ++n)
                {
                    uint32_t c = n;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[n] = c;
                }
                return t;
            }();
            return table;
        }

        uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0)
        {
            const auto &table = crcTable();
            crc = ~crc;
            for (size_t i = 0; i < size; ++i)
                crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
            return ~crc;
        }

        void putBig32(std::vector<uint8_t> &out, uint32_t v)
        {
            out.push_back(static_cast<uint8_t>(v >> 24));
            out.push_back(static_cast<uint8_t>(v >> 16));
            out.push_back(static_cast<uint8_t>(v >> 8));
            out.push_back(static_cast<uint8_t>(v));
        }

        void chunk(std::vector<uint8_t> &out, const char type[4], const std::vector<uint8_t> &data)
        {
            putBig32(out, static_cast<uint32_t>(data.size()));
            const size_t at = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            putBig32(out, crc32(out.data() + at, out.size() - at));
        }
    }

    bool writePng(const std::string &path, const uint8_t *pixels, uint32_t width, uint32_t height, bool alpha)
    {
        if (!pixels || width == 0u || height == 0u)
            return false;
        const uint32_t channels = alpha ? 4u : 3u;
        // The filtered image: each row a filter byte (0, none) then its pixels.
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(height) * (1u + width * channels));
        for (uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0);
            const uint8_t *row = pixels + static_cast<size_t>(y) * width * 4u;
            for (uint32_t x = 0; x < width; ++x)
                raw.insert(raw.end(), row + x * 4u, row + x * 4u + channels);
        }
        // zlib: header (deflate, 32K window, no dictionary, fastest), stored blocks, Adler-32.
        std::vector<uint8_t> z = {0x78, 0x01};
        uint32_t a = 1, b = 0;
        for (uint8_t byte : raw)
        {
            a = (a + byte) % 65521u;
            b = (b + a) % 65521u;
        }
        for (size_t at = 0; at < raw.size() || at == 0;)
        {
            const size_t n = std::min<size_t>(65535u, raw.size() - at);
            const bool last = at + n == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(static_cast<uint8_t>(n));
            z.push_back(static_cast<uint8_t>(n >> 8));
            z.push_back(static_cast<uint8_t>(~n));
            z.push_back(static_cast<uint8_t>(~n >> 8));
            z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += n;
            if (last)
                break;
        }
        putBig32(z, (b << 16) | a);

        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        std::vector<uint8_t> header;
        putBig32(header, width);
        putBig32(header, height);
        header.insert(header.end(), {8, static_cast<uint8_t>(alpha ? 6 : 2), 0, 0, 0}); // 8 bits, RGB(A), deflate, no interlace
        chunk(png, "IHDR", header);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
        return std::fclose(f) == 0 && ok;
    }
}
