// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// Texture upscaling (bozo/render/texture_upscale.h).
#include "bozo/render/texture_upscale.h"
#include "bozo/render/worker_priority.h"

#include "bozo/hash.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bozo::render
{
    bool parseUpscaleFactor(std::string_view text, uint8_t &factor)
    {
        if (text == "off" || text == "1" || text == "1x")
            factor = 1;
        else if (text == "2x" || text == "2")
            factor = 2;
        else if (text == "4x" || text == "4")
            factor = 4;
        else
            return false;
        return true;
    }

    bool parseUpscaleFilter(std::string_view text, UpscaleFilter &filter)
    {
        if (text == "edge")
            filter = UpscaleFilter::Edge;
        else if (text == "smooth")
            filter = UpscaleFilter::Smooth;
        else
            return false;
        return true;
    }

    const char *upscaleFactorName(uint8_t factor) { return factor >= 4u ? "4x" : factor >= 2u ? "2x" : "off"; }

    const char *upscaleFilterName(UpscaleFilter filter) { return filter == UpscaleFilter::Smooth ? "smooth" : "edge"; }

    uint8_t upscaleFactorFor(const Texture &source, uint8_t factor)
    {
        uint8_t f = factor >= 4u ? 4u : factor >= 2u ? 2u : 1u;
        while (f > 1u && (source.width * f > kUpscaleMaxSide || source.height * f > kUpscaleMaxSide))
            f = static_cast<uint8_t>(f >> 1);
        if (source.width == 0u || source.height == 0u || source.rgba.size() < static_cast<size_t>(source.width) * source.height)
            return 1u;
        return f;
    }

    namespace
    {
        struct Premultiplied
        {
            float r = 0, g = 0, b = 0, a = 0;
        };

        struct Feature
        {
            float y = 0, cb = 0, cr = 0, a = 0;
        };

        Premultiplied premultiply(uint32_t c)
        {
            const float a = static_cast<float>(c >> 24) / 255.0f;
            return {static_cast<float>(c & 0xffu) * a, static_cast<float>((c >> 8) & 0xffu) * a, static_cast<float>((c >> 16) & 0xffu) * a, a};
        }

        uint32_t straighten(const Premultiplied &p, uint32_t fallback)
        {
            const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(std::lround(v), 0l, 255l)); };
            const uint32_t a = byte(p.a * 255.0f);
            if (a == 0u)
                return fallback & 0xffffffu;
            const float inverse = 1.0f / p.a;
            return byte(p.r * inverse) | byte(p.g * inverse) << 8 | byte(p.b * inverse) << 16 | a << 24;
        }

        Premultiplied mix(const Premultiplied &x, const Premultiplied &y, float t)
        {
            return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
        }

        // GS alpha: 0x80 is opaque, so an alpha difference counts twice its bytes.
        Feature feature(const Premultiplied &p)
        {
            const float y = 0.299f * p.r + 0.587f * p.g + 0.114f * p.b;
            return {y, p.b - y, p.r - y, p.a * 510.0f};
        }

        float distance(const Feature &p, const Feature &q)
        {
            return std::fabs(p.y - q.y) + 0.6f * (std::fabs(p.cb - q.cb) + std::fabs(p.cr - q.cr)) + std::fabs(p.a - q.a);
        }

        constexpr float kSame = 20.0f;

        struct Image
        {
            uint32_t width = 0, height = 0;
            bool wrapU = false, wrapV = false;
            std::vector<Premultiplied> colour;
            std::vector<Feature> features;

            uint32_t column(int32_t x) const
            {
                const int32_t w = static_cast<int32_t>(width);
                return static_cast<uint32_t>(wrapU ? ((x % w) + w) % w : std::clamp(x, 0, w - 1));
            }
            uint32_t row(int32_t y) const
            {
                const int32_t h = static_cast<int32_t>(height);
                return static_cast<uint32_t>(wrapV ? ((y % h) + h) % h : std::clamp(y, 0, h - 1));
            }
            size_t at(int32_t x, int32_t y) const { return static_cast<size_t>(row(y)) * width + column(x); }
        };

        Image prepare(const Texture &source, bool wrapU, bool wrapV)
        {
            Image image;
            image.width = source.width;
            image.height = source.height;
            image.wrapU = wrapU;
            image.wrapV = wrapV;
            const size_t n = static_cast<size_t>(source.width) * source.height;
            image.colour.resize(n);
            image.features.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                image.colour[i] = premultiply(source.rgba[i]);
                image.features[i] = feature(image.colour[i]);
            }
            return image;
        }

        enum Cut : uint8_t
        {
            kNone = 0,
            kDiagonal = 1,
            kShallow = 2,
            kSteep = 4,
        };

        // Fraction of each output texel of a source texel (factor x factor, row by row) on the far side of a cut toward the
        // corner (1, 1): diagonal x + y > 1.5, shallow x / 2 + y > 1, steep x + y / 2 > 1, both slopes their union.
        struct Coverage
        {
            std::array<std::vector<float>, 8> byCut;
        };

        const Coverage &coverage(uint8_t factor, int corner)
        {
            static std::mutex mutex;
            static std::unordered_map<int, Coverage> tables;
            const std::lock_guard lock(mutex);
            Coverage &table = tables[factor * 4 + corner];
            if (!table.byCut[kDiagonal].empty())
                return table;
            constexpr int kSamples = 16;
            const bool flipX = corner & 1, flipY = corner & 2;
            for (int cut = 1; cut < 8; ++cut)
            {
                std::vector<float> &out = table.byCut[cut];
                out.assign(static_cast<size_t>(factor) * factor, 0.0f);
                for (int j = 0; j < factor; ++j)
                    for (int i = 0; i < factor; ++i)
                    {
                        int inside = 0;
                        for (int sy = 0; sy < kSamples; ++sy)
                            for (int sx = 0; sx < kSamples; ++sx)
                            {
                                float x = (static_cast<float>(i) + (static_cast<float>(sx) + 0.5f) / kSamples) / factor;
                                float y = (static_cast<float>(j) + (static_cast<float>(sy) + 0.5f) / kSamples) / factor;
                                if (flipX)
                                    x = 1.0f - x;
                                if (flipY)
                                    y = 1.0f - y;
                                const bool diagonal = x + y > 1.5f;
                                const bool shallow = x * 0.5f + y > 1.0f, steep = x + y * 0.5f > 1.0f;
                                bool in = false;
                                if ((cut & kShallow) && shallow)
                                    in = true;
                                if ((cut & kSteep) && steep)
                                    in = true;
                                if (cut == kDiagonal && diagonal)
                                    in = true;
                                inside += in ? 1 : 0;
                            }
                        out[static_cast<size_t>(j) * factor + i] = static_cast<float>(inside) / (kSamples * kSamples);
                    }
            }
            return table;
        }

        // The cut at E's corner toward (dx, dy) = (+-1, +-1), and the colour the corner takes.
        uint8_t cornerCut(const Image &image, int32_t x, int32_t y, int32_t dx, int32_t dy, size_t &towards)
        {
            const auto f = [&](int32_t u, int32_t v) -> const Feature & { return image.features[image.at(x + u * dx, y + v * dy)]; };
            const Feature &E = f(0, 0), &F = f(1, 0), &H = f(0, 1), &I = f(1, 1);
            const Feature &B = f(0, -1), &C = f(1, -1), &D = f(-1, 0), &G = f(-1, 1);
            const Feature &F4 = f(2, 0), &H5 = f(0, 2), &I4 = f(2, 1), &I5 = f(1, 2);
            const auto same = [](const Feature &p, const Feature &q) { return distance(p, q) < kSame; };
            if (same(E, F) || same(E, H))
                return kNone;
            const float across = distance(E, C) + distance(E, G) + distance(I, F4) + distance(I, H5) + 4.0f * distance(F, H);
            const float along = distance(H, D) + distance(H, I5) + distance(F, B) + distance(F, I4) + 4.0f * distance(E, I);
            if (!(across < along))
                return kNone;
            const bool keepsCorner = !same(F, B) && !same(H, D);
            const bool line = same(E, I) && !same(F, I4) && !same(H, I5);
            if (!(keepsCorner || line || same(E, G) || same(E, C)))
                return kNone;
            towards = distance(E, F) <= distance(E, H) ? image.at(x + dx, y) : image.at(x, y + dy);
            uint8_t cut = kDiagonal;
            const float fg = distance(F, G), hc = distance(H, C);
            if (2.0f * fg <= hc && !same(E, G) && !same(D, G))
                cut = kShallow;
            if (2.0f * hc <= fg && !same(E, C) && !same(B, C))
                cut = static_cast<uint8_t>(cut == kShallow ? kShallow | kSteep : kSteep);
            return cut;
        }

        void scaleSmooth(const Texture &source, const Image &image, uint8_t factor, std::vector<uint32_t> &out);

        void scaleEdge(const Texture &source, const Image &image, uint8_t factor, std::vector<uint32_t> &out)
        {
            const uint32_t width = source.width * factor;
            scaleSmooth(source, image, factor, out);
            const Coverage *tables[4];
            for (int corner = 0; corner < 4; ++corner)
                tables[corner] = &coverage(factor, corner);
            static constexpr int32_t kCorners[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
            std::vector<Premultiplied> cell(static_cast<size_t>(factor) * factor);
            std::vector<uint8_t> touched(cell.size());
            for (uint32_t y = 0; y < source.height; ++y)
                for (uint32_t x = 0; x < source.width; ++x)
                {
                    const size_t e = static_cast<size_t>(y) * source.width + x;
                    const uint32_t original = source.rgba[e];
                    uint8_t cuts[4];
                    size_t towards[4] = {};
                    bool any = false;
                    for (int corner = 0; corner < 4; ++corner)
                    {
                        cuts[corner] = cornerCut(image, static_cast<int32_t>(x), static_cast<int32_t>(y), kCorners[corner][0],
                                                 kCorners[corner][1], towards[corner]);
                        any = any || cuts[corner] != kNone;
                    }
                    uint32_t *const base = &out[static_cast<size_t>(y) * factor * width + static_cast<size_t>(x) * factor];
                    if (!any)
                        continue;
                    for (uint32_t j = 0; j < factor; ++j)
                        for (uint32_t i = 0; i < factor; ++i)
                            cell[static_cast<size_t>(j) * factor + i] = premultiply(base[static_cast<size_t>(j) * width + i]);
                    std::fill(touched.begin(), touched.end(), uint8_t{0});
                    for (int corner = 0; corner < 4; ++corner)
                    {
                        if (cuts[corner] == kNone)
                            continue;
                        const std::vector<float> &cover = tables[corner]->byCut[cuts[corner]];
                        const Premultiplied &other = image.colour[towards[corner]];
                        for (size_t i = 0; i < cell.size(); ++i)
                            if (cover[i] > 0.0f)
                            {
                                cell[i] = mix(cell[i], other, cover[i]);
                                touched[i] = 1u;
                            }
                    }
                    for (uint32_t j = 0; j < factor; ++j)
                        for (uint32_t i = 0; i < factor; ++i)
                        {
                            const size_t c = static_cast<size_t>(j) * factor + i;
                            if (touched[c])
                                base[static_cast<size_t>(j) * width + i] = straighten(cell[c], original);
                        }
                }
        }

        float catmullRom(float t, int k)
        {
            const float t2 = t * t, t3 = t2 * t;
            switch (k)
            {
            case 0:
                return 0.5f * (-t3 + 2.0f * t2 - t);
            case 1:
                return 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
            case 2:
                return 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
            default:
                return 0.5f * (t3 - t2);
            }
        }

        void scaleSmooth(const Texture &source, const Image &image, uint8_t factor, std::vector<uint32_t> &out)
        {
            const uint32_t width = source.width * factor, height = source.height * factor;
            out.assign(static_cast<size_t>(width) * height, 0u);
            for (uint32_t oy = 0; oy < height; ++oy)
            {
                const float sy = (static_cast<float>(oy) + 0.5f) / factor - 0.5f;
                const int32_t y0 = static_cast<int32_t>(std::floor(sy));
                const float ty = sy - static_cast<float>(y0);
                float wy[4];
                for (int k = 0; k < 4; ++k)
                    wy[k] = catmullRom(ty, k);
                for (uint32_t ox = 0; ox < width; ++ox)
                {
                    const float sx = (static_cast<float>(ox) + 0.5f) / factor - 0.5f;
                    const int32_t x0 = static_cast<int32_t>(std::floor(sx));
                    const float tx = sx - static_cast<float>(x0);
                    Premultiplied sum, low{1e30f, 1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f, -1e30f};
                    for (int j = 0; j < 4; ++j)
                        for (int i = 0; i < 4; ++i)
                        {
                            const Premultiplied &p = image.colour[image.at(x0 - 1 + i, y0 - 1 + j)];
                            const float w = catmullRom(tx, i) * wy[j];
                            sum.r += p.r * w;
                            sum.g += p.g * w;
                            sum.b += p.b * w;
                            sum.a += p.a * w;
                            if ((i == 1 || i == 2) && (j == 1 || j == 2))
                            {
                                low = {std::min(low.r, p.r), std::min(low.g, p.g), std::min(low.b, p.b), std::min(low.a, p.a)};
                                high = {std::max(high.r, p.r), std::max(high.g, p.g), std::max(high.b, p.b), std::max(high.a, p.a)};
                            }
                        }
                    sum = {std::clamp(sum.r, low.r, high.r), std::clamp(sum.g, low.g, high.g), std::clamp(sum.b, low.b, high.b),
                           std::clamp(sum.a, low.a, high.a)};
                    const uint32_t c00 = source.rgba[image.at(x0, y0)];
                    if (c00 == source.rgba[image.at(x0 + 1, y0)] && c00 == source.rgba[image.at(x0, y0 + 1)] &&
                        c00 == source.rgba[image.at(x0 + 1, y0 + 1)])
                    {
                        out[static_cast<size_t>(oy) * width + ox] = c00;
                        continue;
                    }
                    const int32_t nx = static_cast<int32_t>(std::lround(std::floor(sx + 0.5f)));
                    const int32_t ny = static_cast<int32_t>(std::lround(std::floor(sy + 0.5f)));
                    const uint32_t nearest = source.rgba[image.at(nx, ny)];
                    out[static_cast<size_t>(oy) * width + ox] = sum.a * 255.0f < 0.5f && (nearest >> 24) == 0u ? nearest : straighten(sum, nearest);
                }
            }
        }

        std::vector<uint32_t> halve(const std::vector<uint32_t> &in, uint32_t width, uint32_t height)
        {
            const uint32_t w = std::max(1u, width >> 1), h = std::max(1u, height >> 1);
            std::vector<uint32_t> out(static_cast<size_t>(w) * h);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    Premultiplied sum;
                    uint32_t fallback = 0;
                    for (uint32_t j = 0; j < 2; ++j)
                        for (uint32_t i = 0; i < 2; ++i)
                        {
                            const uint32_t c = in[static_cast<size_t>(std::min(y * 2 + j, height - 1)) * width + std::min(x * 2 + i, width - 1)];
                            const Premultiplied p = premultiply(c);
                            sum = {sum.r + p.r * 0.25f, sum.g + p.g * 0.25f, sum.b + p.b * 0.25f, sum.a + p.a * 0.25f};
                            if (i == 0 && j == 0)
                                fallback = c;
                        }
                    out[static_cast<size_t>(y) * w + x] = straighten(sum, fallback);
                }
            return out;
        }
    }

    Texture upscaleTexture(const Texture &source, TextureUpscale upscale, bool wrapU, bool wrapV)
    {
        const uint8_t factor = upscaleFactorFor(source, upscale.factor);
        if (factor <= 1u)
            return source;
        Texture out;
        out.width = source.width * factor;
        out.height = source.height * factor;
        out.key = source.key;
        out.version = source.version;
        out.pages = source.pages;
        out.asset = source.asset;
        const Image image = prepare(source, wrapU, wrapV);
        if (upscale.filter == UpscaleFilter::Smooth)
            scaleSmooth(source, image, factor, out.rgba);
        else
            scaleEdge(source, image, factor, out.rgba);
        uint32_t w = out.width, h = out.height;
        const std::vector<uint32_t> *previous = &out.rgba;
        for (uint8_t f = static_cast<uint8_t>(factor >> 1); f > 1u; f = static_cast<uint8_t>(f >> 1))
        {
            out.levels.push_back(halve(*previous, w, h));
            w = std::max(1u, w >> 1);
            h = std::max(1u, h >> 1);
            previous = &out.levels.back();
        }
        out.levels.push_back(source.rgba);
        for (const std::vector<uint32_t> &level : source.levels)
            out.levels.push_back(level);
        return out;
    }

    struct TextureUpscaler::State
    {
        struct Entry
        {
            std::shared_ptr<const Texture> scaled; // null while queued or being made
            std::list<uint64_t>::iterator recent;
            size_t bytes = 0;
        };
        struct Job
        {
            uint64_t key = 0;
            std::shared_ptr<const Texture> source;
            TextureUpscale upscale;
            bool wrapU = false, wrapV = false;
        };
        struct Known
        {
            std::weak_ptr<const Texture> texture;
            uint64_t hash = 0;
        };

        mutable std::mutex mutex;
        std::condition_variable wake, idle;
        std::unordered_map<uint64_t, Entry> entries;
        std::list<uint64_t> recent; // most recent first; only made copies
        std::deque<Job> jobs;
        std::unordered_map<const Texture *, Known> known;
        std::vector<std::thread> workers;
        unsigned busy = 0;
        size_t budget = size_t(256) << 20;
        Stats stats;

        uint64_t contentOf(const std::shared_ptr<const Texture> &texture)
        {
            Known &k = known[texture.get()];
            if (k.texture.expired() || k.texture.lock() != texture)
            {
                const uint32_t size[2] = {texture->width, texture->height};
                uint64_t h = hash::xxh64(texture->rgba.data(), texture->rgba.size() * sizeof(uint32_t), hash::xxh64(size, sizeof(size), 0));
                for (const std::vector<uint32_t> &level : texture->levels)
                    h = hash::xxh64(level.data(), level.size() * sizeof(uint32_t), h);
                k = Known{texture, h};
                if (known.size() > 65536u)
                {
                    known.clear();
                    known[texture.get()] = Known{texture, h};
                }
                return h;
            }
            return k.hash;
        }

        void trim()
        {
            while (stats.bytes > budget && !recent.empty())
            {
                const uint64_t key = recent.back();
                recent.pop_back();
                const auto it = entries.find(key);
                stats.bytes -= it->second.bytes;
                entries.erase(it);
                ++stats.dropped;
            }
        }

        void work()
        {
            std::unique_lock lock(mutex);
            for (;;)
            {
                wake.wait(lock, [&] { return !jobs.empty(); });
                Job job = std::move(jobs.front());
                jobs.pop_front();
                ++busy;
                lock.unlock();
                const auto start = std::chrono::steady_clock::now();
                auto scaled = std::make_shared<const Texture>(upscaleTexture(*job.source, job.upscale, job.wrapU, job.wrapV));
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                job.source.reset();
                lock.lock();
                --busy;
                stats.seconds += seconds;
                ++stats.made;
                const auto it = entries.find(job.key);
                if (it != entries.end())
                {
                    size_t bytes = scaled->rgba.size() * sizeof(uint32_t);
                    for (const std::vector<uint32_t> &level : scaled->levels)
                        bytes += level.size() * sizeof(uint32_t);
                    it->second.scaled = std::move(scaled);
                    it->second.bytes = bytes;
                    recent.push_front(job.key);
                    it->second.recent = recent.begin();
                    stats.bytes += bytes;
                    trim();
                }
                if (jobs.empty() && busy == 0u)
                    idle.notify_all();
            }
        }
    };

    TextureUpscaler::TextureUpscaler() : m_state(std::make_shared<State>()) {}

    TextureUpscaler &TextureUpscaler::instance()
    {
        // Never destroyed: its workers may still be running when the program exits.
        static TextureUpscaler *upscaler = new TextureUpscaler();
        return *upscaler;
    }

    std::shared_ptr<const Texture> TextureUpscaler::find(const std::shared_ptr<const Texture> &source, TextureUpscale upscale, bool wrapU, bool wrapV)
    {
        State &s = *m_state;
        const std::lock_guard lock(s.mutex);
        const uint64_t settings = static_cast<uint64_t>(upscale.factor) | static_cast<uint64_t>(upscale.filter) << 8 |
                                  static_cast<uint64_t>(wrapU) << 16 | static_cast<uint64_t>(wrapV) << 17;
        const uint64_t key = hash::xxh64(&settings, sizeof(settings), s.contentOf(source));
        const auto it = s.entries.find(key);
        if (it != s.entries.end())
        {
            if (!it->second.scaled)
                return nullptr;
            s.recent.splice(s.recent.begin(), s.recent, it->second.recent);
            ++s.stats.found;
            return it->second.scaled;
        }
        s.entries.emplace(key, State::Entry{});
        s.jobs.push_back(State::Job{key, source, upscale, wrapU, wrapV});
        ++s.stats.queued;
        if (s.workers.empty())
        {
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            const unsigned count = std::clamp(cores / 4u, 1u, 4u);
            for (unsigned i = 0; i < count; ++i)
            {
                s.workers.emplace_back([state = m_state] { lowerWorkerPriority(); state->work(); });
                s.workers.back().detach();
            }
        }
        s.wake.notify_one();
        return nullptr;
    }

    void TextureUpscaler::waitIdle()
    {
        State &s = *m_state;
        std::unique_lock lock(s.mutex);
        s.idle.wait(lock, [&] { return s.jobs.empty() && s.busy == 0u; });
    }

    void TextureUpscaler::setBudget(size_t bytes)
    {
        State &s = *m_state;
        const std::lock_guard lock(s.mutex);
        s.budget = bytes;
        s.trim();
    }

    TextureUpscaler::Stats TextureUpscaler::stats() const
    {
        const std::lock_guard lock(m_state->mutex);
        return m_state->stats;
    }
}
