// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The PNG reader (bozo/render/png_read.h), from the PNG specification and RFC 1950 / 1951.
#include "bozo/render/png_read.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace bozo::render
{
    namespace
    {
        struct Fail
        {
            const char *what;
        };

        // ---- inflate (RFC 1951) ----

        class Bits
        {
        public:
            Bits(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}
            uint32_t get(int count)
            {
                while (m_count < count)
                {
                    if (m_at >= m_size)
                        throw Fail{"deflate data ends early"};
                    m_buffer |= static_cast<uint32_t>(m_data[m_at++]) << m_count;
                    m_count += 8;
                }
                const uint32_t value = m_buffer & ((1u << count) - 1u);
                m_buffer >>= count;
                m_count -= count;
                return value;
            }
            void align()
            {
                m_buffer = 0;
                m_count = 0;
            }
            uint8_t byte()
            {
                if (m_at >= m_size)
                    throw Fail{"deflate data ends early"};
                return m_data[m_at++];
            }

        private:
            const uint8_t *m_data;
            size_t m_size, m_at = 0;
            uint32_t m_buffer = 0;
            int m_count = 0;
        };

        // A canonical Huffman code (codes by length, symbols in order), decoded bit by bit (as RFC 1951's puff does).
        struct Huffman
        {
            std::array<uint16_t, 16> count{};
            std::vector<uint16_t> symbol;
            void build(const uint8_t *lengths, int n)
            {
                count.fill(0);
                for (int i = 0; i < n; ++i)
                    ++count[lengths[i]];
                count[0] = 0;
                std::array<uint16_t, 16> offset{};
                for (int len = 1; len < 15; ++len)
                    offset[len + 1] = static_cast<uint16_t>(offset[len] + count[len]);
                symbol.assign(static_cast<size_t>(n), 0);
                for (int i = 0; i < n; ++i)
                    if (lengths[i])
                        symbol[offset[lengths[i]]++] = static_cast<uint16_t>(i);
            }
            int decode(Bits &bits) const
            {
                int code = 0, first = 0, index = 0;
                for (int len = 1; len < 16; ++len)
                {
                    code |= static_cast<int>(bits.get(1));
                    const int c = count[len];
                    if (code - c < first)
                        return symbol[static_cast<size_t>(index + (code - first))];
                    index += c;
                    first += c;
                    first <<= 1;
                    code <<= 1;
                }
                throw Fail{"bad Huffman code"};
            }
        };

        constexpr uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                              35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        void codes(Bits &bits, std::vector<uint8_t> &out, const Huffman &lit, const Huffman &dist, size_t limit)
        {
            for (;;)
            {
                const int symbol = lit.decode(bits);
                if (symbol < 256)
                {
                    out.push_back(static_cast<uint8_t>(symbol));
                }
                else if (symbol == 256)
                    return;
                else
                {
                    const int s = symbol - 257;
                    if (s >= 29)
                        throw Fail{"bad length symbol"};
                    const size_t length = kLengthBase[s] + bits.get(kLengthExtra[s]);
                    const int d = dist.decode(bits);
                    if (d >= 30)
                        throw Fail{"bad distance symbol"};
                    const size_t distance = kDistBase[d] + bits.get(kDistExtra[d]);
                    if (distance > out.size())
                        throw Fail{"distance past the start"};
                    const size_t from = out.size() - distance;
                    for (size_t i = 0; i < length; ++i)
                        out.push_back(out[from + i]);
                }
                if (out.size() > limit)
                    throw Fail{"image data larger than the image"};
            }
        }

        void inflate(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t limit)
        {
            if (size < 2 || (data[0] & 0x0f) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20))
                throw Fail{"not a zlib stream"};
            Bits bits(data + 2, size - 2);
            for (bool last = false; !last;)
            {
                last = bits.get(1) != 0;
                const uint32_t type = bits.get(2);
                if (type == 0)
                {
                    bits.align();
                    const uint32_t len = bits.byte() | (bits.byte() << 8), nlen = bits.byte() | (bits.byte() << 8);
                    if ((len ^ 0xffffu) != nlen)
                        throw Fail{"bad stored block"};
                    for (uint32_t i = 0; i < len; ++i)
                        out.push_back(bits.byte());
                    if (out.size() > limit)
                        throw Fail{"image data larger than the image"};
                }
                else if (type == 1)
                {
                    static const std::pair<Huffman, Huffman> fixed = []
                    {
                        uint8_t l[288];
                        for (int i = 0; i < 288; ++i)
                            l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                        uint8_t d[30];
                        std::memset(d, 5, sizeof(d));
                        std::pair<Huffman, Huffman> h;
                        h.first.build(l, 288);
                        h.second.build(d, 30);
                        return h;
                    }();
                    codes(bits, out, fixed.first, fixed.second, limit);
                }
                else if (type == 2)
                {
                    const int nlen = static_cast<int>(bits.get(5)) + 257, ndist = static_cast<int>(bits.get(5)) + 1,
                              ncode = static_cast<int>(bits.get(4)) + 4;
                    if (nlen > 286 || ndist > 30)
                        throw Fail{"bad dynamic block"};
                    static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                    uint8_t lengths[320] = {};
                    for (int i = 0; i < ncode; ++i)
                        lengths[kOrder[i]] = static_cast<uint8_t>(bits.get(3));
                    Huffman lencode;
                    lencode.build(lengths, 19);
                    std::memset(lengths, 0, sizeof(lengths));
                    for (int i = 0; i < nlen + ndist;)
                    {
                        const int symbol = lencode.decode(bits);
                        if (symbol < 16)
                            lengths[i++] = static_cast<uint8_t>(symbol);
                        else
                        {
                            uint8_t value = 0;
                            int repeat;
                            if (symbol == 16)
                            {
                                if (i == 0)
                                    throw Fail{"repeat with no length"};
                                value = lengths[i - 1];
                                repeat = 3 + static_cast<int>(bits.get(2));
                            }
                            else if (symbol == 17)
                                repeat = 3 + static_cast<int>(bits.get(3));
                            else
                                repeat = 11 + static_cast<int>(bits.get(7));
                            if (i + repeat > nlen + ndist)
                                throw Fail{"too many lengths"};
                            while (repeat--)
                                lengths[i++] = value;
                        }
                    }
                    Huffman lit, dist;
                    lit.build(lengths, nlen);
                    dist.build(lengths + nlen, ndist);
                    codes(bits, out, lit, dist, limit);
                }
                else
                    throw Fail{"bad block type"};
            }
        }

        uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

        uint8_t paeth(int a, int b, int c)
        {
            const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
        }
    }

    bool decodePng(const uint8_t *data, size_t size, uint32_t &width, uint32_t &height, std::vector<uint32_t> &rgba,
                   std::string *error)
    {
        try
        {
            static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
            if (size < 8 || std::memcmp(data, kSignature, 8) != 0)
                throw Fail{"not a PNG file"};
            uint32_t w = 0, h = 0;
            int type = -1;
            std::vector<uint8_t> idat;
            std::vector<uint32_t> palette;
            for (size_t at = 8; at + 12 <= size;)
            {
                const uint32_t length = be32(data + at);
                if (length > size - at - 12)
                    throw Fail{"chunk past the file's end"};
                const uint8_t *chunk = data + at + 8;
                const char *name = reinterpret_cast<const char *>(data + at + 4);
                if (std::memcmp(name, "IHDR", 4) == 0)
                {
                    if (length < 13)
                        throw Fail{"short IHDR"};
                    w = be32(chunk);
                    h = be32(chunk + 4);
                    type = chunk[9];
                    if (chunk[8] != 8)
                        throw Fail{"only 8-bit PNGs are read"};
                    if (chunk[12] != 0)
                        throw Fail{"interlaced PNGs are not read"};
                    if (w == 0 || h == 0 || w > 16384 || h > 16384)
                        throw Fail{"image size out of range"};
                    if (type != 0 && type != 2 && type != 3 && type != 4 && type != 6)
                        throw Fail{"unknown colour type"};
                }
                else if (std::memcmp(name, "PLTE", 4) == 0)
                {
                    palette.assign(length / 3, 0xff000000u);
                    for (uint32_t i = 0; i < length / 3; ++i)
                        palette[i] = 0xff000000u | chunk[i * 3] | (uint32_t(chunk[i * 3 + 1]) << 8) | (uint32_t(chunk[i * 3 + 2]) << 16);
                }
                else if (std::memcmp(name, "tRNS", 4) == 0 && type == 3)
                {
                    for (uint32_t i = 0; i < length && i < palette.size(); ++i)
                        palette[i] = (palette[i] & 0xffffffu) | (uint32_t(chunk[i]) << 24);
                }
                else if (std::memcmp(name, "IDAT", 4) == 0)
                    idat.insert(idat.end(), chunk, chunk + length);
                else if (std::memcmp(name, "IEND", 4) == 0)
                    break;
                at += 12 + length;
            }
            if (type < 0 || idat.empty())
                throw Fail{"no image header or data"};
            const int channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : 4;
            const size_t stride = static_cast<size_t>(w) * channels;
            std::vector<uint8_t> raw;
            raw.reserve((stride + 1) * h);
            inflate(idat.data(), idat.size(), raw, (stride + 1) * h);
            if (raw.size() < (stride + 1) * h)
                throw Fail{"image data too short"};
            std::vector<uint8_t> pixels(stride * h);
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint8_t filter = raw[y * (stride + 1)];
                const uint8_t *in = raw.data() + y * (stride + 1) + 1;
                uint8_t *row = pixels.data() + y * stride;
                const uint8_t *up = y ? row - stride : nullptr;
                for (size_t x = 0; x < stride; ++x)
                {
                    const int a = x >= static_cast<size_t>(channels) ? row[x - channels] : 0, b = up ? up[x] : 0,
                              c = up && x >= static_cast<size_t>(channels) ? up[x - channels] : 0;
                    int v = in[x];
                    switch (filter)
                    {
                    case 0: break;
                    case 1: v += a; break;
                    case 2: v += b; break;
                    case 3: v += (a + b) >> 1; break;
                    case 4: v += paeth(a, b, c); break;
                    default: throw Fail{"bad row filter"};
                    }
                    row[x] = static_cast<uint8_t>(v);
                }
            }
            rgba.assign(static_cast<size_t>(w) * h, 0);
            for (size_t i = 0; i < rgba.size(); ++i)
            {
                const uint8_t *p = pixels.data() + i * channels;
                switch (type)
                {
                case 0: rgba[i] = 0xff000000u | p[0] | (uint32_t(p[0]) << 8) | (uint32_t(p[0]) << 16); break;
                case 2: rgba[i] = 0xff000000u | p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); break;
                case 3:
                    if (p[0] >= palette.size())
                        throw Fail{"palette index past the palette"};
                    rgba[i] = palette[p[0]];
                    break;
                case 4: rgba[i] = (uint32_t(p[1]) << 24) | p[0] | (uint32_t(p[0]) << 8) | (uint32_t(p[0]) << 16); break;
                default: rgba[i] = p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); break;
                }
            }
            width = w;
            height = h;
            return true;
        }
        catch (const Fail &f)
        {
            if (error)
                *error = f.what;
            return false;
        }
    }

    bool readPng(const std::string &path, uint32_t &width, uint32_t &height, std::vector<uint32_t> &rgba, std::string *error)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            if (error)
                *error = "cannot open the file";
            return false;
        }
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return decodePng(data.data(), data.size(), width, height, rgba, error);
    }
}
