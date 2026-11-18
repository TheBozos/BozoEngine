// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The PNG reader and texture replacements (bozo/render/png_read.h, texture_replace.h), with no GPU: zlib streams made by
// Python's zlib (dynamic and fixed Huffman, stored; all five row filters; RGBA, RGB, grey + alpha, palette + tRNS:
// png_fixtures.h, synthetic images), then replacements read from a temporary folder by the worker threads.
#include "bozo/render/png_read.h"
#include "bozo/render/texture_replace.h"

#include "png_fixtures.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace bozo::render;

namespace
{
    void check(bool ok, const char *what)
    {
        if (!ok)
            throw std::runtime_error(what);
    }

    uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return (r & 255u) | (g & 255u) << 8 | (b & 255u) << 16 | (a & 255u) << 24; }

    void decoded(const uint8_t *data, size_t size, uint32_t (*expected)(uint32_t, uint32_t), const char *what)
    {
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> pixels;
        std::string error;
        if (!decodePng(data, size, w, h, pixels, &error))
            throw std::runtime_error(std::string(what) + ": " + error);
        check(w == 37u && h == 23u && pixels.size() == 37u * 23u, what);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                if (pixels[y * w + x] != expected(x, y))
                    throw std::runtime_error(std::string(what) + ": texel " + std::to_string(x) + "," + std::to_string(y));
    }

    // ---- a PNG writer for the replacement folder (stored deflate) ----
    uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0)
    {
        c = ~c;
        for (size_t i = 0; i < n; ++i)
        {
            c ^= p[i];
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        }
        return ~c;
    }
    void be(std::vector<uint8_t> &o, uint32_t v)
    {
        for (int s = 24; s >= 0; s -= 8)
            o.push_back(static_cast<uint8_t>(v >> s));
    }
    void chunk(std::vector<uint8_t> &o, const char *t, const std::vector<uint8_t> &d)
    {
        be(o, static_cast<uint32_t>(d.size()));
        const size_t at = o.size();
        o.insert(o.end(), t, t + 4);
        o.insert(o.end(), d.begin(), d.end());
        be(o, crc(o.data() + at, o.size() - at));
    }
    void writePng(const std::filesystem::path &path, uint32_t w, uint32_t h, const std::vector<uint32_t> &px)
    {
        std::vector<uint8_t> raw;
        for (uint32_t y = 0; y < h; ++y)
        {
            raw.push_back(0);
            for (uint32_t x = 0; x < w; ++x)
                for (int s = 0; s < 32; s += 8)
                    raw.push_back(static_cast<uint8_t>(px[y * w + x] >> s));
        }
        std::vector<uint8_t> z = {0x78, 0x01, 1, static_cast<uint8_t>(raw.size()), static_cast<uint8_t>(raw.size() >> 8),
                                  static_cast<uint8_t>(~raw.size()), static_cast<uint8_t>(~raw.size() >> 8)};
        z.insert(z.end(), raw.begin(), raw.end());
        uint32_t a = 1, b = 0;
        for (uint8_t c : raw)
            a = (a + c) % 65521u, b = (b + a) % 65521u;
        be(z, b << 16 | a);
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}, header;
        be(header, w);
        be(header, h);
        header.insert(header.end(), {8, 6, 0, 0, 0});
        chunk(png, "IHDR", header);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
    }

    std::shared_ptr<const Texture> asset(uint32_t w, uint32_t h, uint32_t seed, bool isAsset = true)
    {
        auto t = std::make_shared<Texture>();
        t->width = w;
        t->height = h;
        t->asset = isAsset;
        for (uint32_t i = 0; i < w * h; ++i)
            t->rgba.push_back(rgba(i * 13u + seed, i * 7u, seed, 0x80u));
        return t;
    }

    std::string name(uint64_t identity)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%016llx.png", static_cast<unsigned long long>(identity));
        return text;
    }

    std::shared_ptr<const Texture> waitFor(const std::shared_ptr<const Texture> &source, bool &loading)
    {
        TextureReplacements &r = TextureReplacements::instance();
        std::shared_ptr<const Texture> copy = r.find(source, loading);
        r.waitIdle();
        return copy ? copy : r.find(source, loading);
    }
}

int main()
{
    try
    {
        const auto rgbaExpected = [](uint32_t x, uint32_t y) { return rgba(x * 7 + y * 3, x * y, 255 - x * 5, y * 11 + x); };
        decoded(kRgbaDynamic, sizeof(kRgbaDynamic), rgbaExpected, "RGBA, dynamic Huffman");
        decoded(kRgbaFixed, sizeof(kRgbaFixed), rgbaExpected, "RGBA, fixed Huffman");
        decoded(kRgbaStored, sizeof(kRgbaStored), rgbaExpected, "RGBA, stored");
        decoded(kRgb, sizeof(kRgb), [](uint32_t x, uint32_t y) { return rgba(x * 7 + y * 3, x * y, 255 - x * 5, 255); }, "RGB");
        decoded(kGreyAlpha, sizeof(kGreyAlpha), [](uint32_t x, uint32_t y) { const uint32_t g = (x + y) & 255u; return rgba(g, g, g, x * 9); },
                "grey + alpha");
        decoded(kPalette, sizeof(kPalette),
                [](uint32_t x, uint32_t y)
                {
                    static const uint32_t p[4] = {rgba(10, 20, 30, 0), rgba(40, 50, 60, 128), rgba(70, 80, 90, 255), rgba(100, 110, 120, 255)};
                    return p[(x + y) % 4u];
                },
                "palette + tRNS");
        {
            uint32_t w, h;
            std::vector<uint32_t> px;
            std::vector<uint8_t> broken(kRgbaDynamic, kRgbaDynamic + sizeof(kRgbaDynamic));
            broken.resize(broken.size() / 2);
            check(!decodePng(broken.data(), broken.size(), w, h, px), "a cut file is refused");
            check(!decodePng(kRgb, 7, w, h, px), "no signature is refused");
        }

        // ---- replacements ----
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "bozo-texture-replace-tests";
        std::filesystem::remove_all(folder);
        std::filesystem::create_directories(folder);
        const auto a = asset(4, 4, 1), b = asset(8, 4, 2), c = asset(4, 4, 3), notAsset = asset(4, 4, 1, false), none = asset(4, 4, 9);
        check(textureIdentity(*a) == textureIdentity(*notAsset) && textureIdentity(*a) != textureIdentity(*c), "identity by content");
        std::vector<uint32_t> big(16 * 16);
        for (uint32_t i = 0; i < big.size(); ++i)
            big[i] = rgba(i, i >> 4, 7, 0x80);
        writePng(folder / name(textureIdentity(*a)), 16, 16, big);                                 // 4x
        writePng(folder / name(textureIdentity(*b)), 8, 4, std::vector<uint32_t>(32, rgba(1, 2, 3, 4))); // the same size
        writePng(folder / name(textureIdentity(*c)), 12, 12, std::vector<uint32_t>(144, 0));          // 3x: refused
        std::ofstream(folder / "readme.txt") << "not a replacement";

        TextureReplacements &r = TextureReplacements::instance();
        check(!r.active(), "inactive before a folder");
        r.setFolder(folder.string());
        check(r.active() && r.stats().files == 3u, "three replacements listed");
        bool loading = false;
        check(!r.find(none, loading) && !loading, "no file: none, not loading");
        check(!r.find(notAsset, loading) && !loading, "not an asset: never replaced");
        std::shared_ptr<const Texture> copy = r.find(a, loading);
        check(!copy && loading, "first find queues the load");
        copy = waitFor(a, loading);
        check(copy && !loading && copy->width == 16u && copy->height == 16u && copy->rgba == big && copy->asset, "4x replacement read");
        check(copy->levels.size() == 2u && copy->levels[0].size() == 64u && copy->levels[1] == a->rgba, "levels: halved, then the texture's own");
        check(copy->levels[0][0] == rgba((0 + 1 + 16 + 17 + 2) / 4, (0 + 0 + 1 + 1 + 2) / 4, 7, 0x80), "a halved texel is the mean of four");
        const auto same = waitFor(b, loading);
        check(same && same->width == 8u && same->levels.empty() && same->rgba[0] == rgba(1, 2, 3, 4), "same-size replacement");
        check(!waitFor(c, loading) && !loading && r.stats().refused == 1u, "3x refused, not retried");
        const uint64_t generation = r.generation();
        check(!r.rescan() && r.generation() == generation, "an unchanged folder");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        big[0] = rgba(200, 0, 0, 0x80);
        writePng(folder / name(textureIdentity(*a)), 16, 16, big);
        std::filesystem::last_write_time(folder / name(textureIdentity(*a)),
                                         std::filesystem::last_write_time(folder / name(textureIdentity(*a))) + std::chrono::seconds(2));
        check(r.rescan() && r.generation() != generation, "a changed file is seen");
        copy = waitFor(a, loading);
        check(copy && copy->rgba[0] == rgba(200, 0, 0, 0x80), "the changed file is read again");
        std::filesystem::remove(folder / name(textureIdentity(*b)));
        check(r.rescan() && !r.find(b, loading) && !loading, "a removed file is no replacement");
        r.setFolder("");
        check(!r.active() && !r.find(a, loading), "off with no folder");
        std::filesystem::remove_all(folder);
        std::puts("png reader (dynamic/fixed/stored, 5 filters, 5 colour types) and texture replacements (4x levels, same size, refusal, non-assets, rescan) passed");
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "texture replacement tests failed: %s\n", e.what());
        return 1;
    }
}
