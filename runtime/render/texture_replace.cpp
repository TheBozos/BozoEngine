// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// Texture replacements (bozo/render/texture_replace.h).
#include "bozo/render/texture_replace.h"

#include "bozo/hash.h"
#include "bozo/render/png_read.h"
#include "bozo/render/texture_upscale.h"
#include "bozo/render/worker_priority.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace bozo::render
{
    uint64_t textureIdentity(const Texture &texture)
    {
        const uint32_t size[2] = {texture.width, texture.height};
        return hash::xxh64(texture.rgba.data(), texture.rgba.size() * sizeof(uint32_t), hash::xxh64(size, sizeof(size), 0));
    }

    namespace
    {
        // Each way halved, each texel the mean of its four (the last row or column alone on an odd side).
        std::vector<uint32_t> halve(const std::vector<uint32_t> &rgba, uint32_t width, uint32_t height, uint32_t &outWidth,
                                    uint32_t &outHeight)
        {
            outWidth = std::max(1u, width / 2u);
            outHeight = std::max(1u, height / 2u);
            std::vector<uint32_t> out(static_cast<size_t>(outWidth) * outHeight);
            for (uint32_t y = 0; y < outHeight; ++y)
                for (uint32_t x = 0; x < outWidth; ++x)
                {
                    const uint32_t x0 = std::min(width - 1u, x * 2u), x1 = std::min(width - 1u, x * 2u + 1u);
                    const uint32_t y0 = std::min(height - 1u, y * 2u), y1 = std::min(height - 1u, y * 2u + 1u);
                    const uint32_t c[4] = {rgba[y0 * width + x0], rgba[y0 * width + x1], rgba[y1 * width + x0], rgba[y1 * width + x1]};
                    uint32_t texel = 0;
                    for (int shift = 0; shift < 32; shift += 8)
                    {
                        uint32_t sum = 2u;
                        for (uint32_t v : c)
                            sum += (v >> shift) & 0xffu;
                        texel |= (sum / 4u) << shift;
                    }
                    out[static_cast<size_t>(y) * outWidth + x] = texel;
                }
            return out;
        }

        bool identityName(const std::filesystem::path &file, uint64_t &identity)
        {
            const std::string stem = file.stem().string(), ext = file.extension().string();
            if (stem.size() != 16u || (ext != ".png" && ext != ".PNG"))
                return false;
            identity = 0;
            for (char c : stem)
            {
                const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0)
                    return false;
                identity = identity << 4 | static_cast<uint64_t>(v);
            }
            return true;
        }
    }

    struct TextureReplacements::State
    {
        struct File
        {
            std::string path;
            std::filesystem::file_time_type time{};
            uintmax_t size = 0;
        };
        enum class Status : uint8_t
        {
            Queued,
            Ready,
            Refused,
        };
        struct Entry
        {
            Status status = Status::Queued;
            std::shared_ptr<const Texture> copy;
            size_t bytes = 0;
            std::list<uint64_t>::iterator recent;
        };
        struct Job
        {
            uint64_t identity = 0;
            uint64_t generation = 0;
            std::string path;
            std::shared_ptr<const Texture> source;
        };
        struct Known
        {
            std::weak_ptr<const Texture> texture;
            uint64_t identity = 0;
        };

        mutable std::mutex mutex;
        std::condition_variable wake, idle;
        std::string folder;
        std::unordered_map<uint64_t, File> files;
        std::unordered_map<uint64_t, Entry> entries;
        std::list<uint64_t> recent; // ready copies, most recent first
        std::unordered_map<const Texture *, Known> known;
        std::deque<Job> jobs;
        std::vector<std::thread> workers;
        unsigned busy = 0;
        uint64_t generation = 0;
        size_t budget = size_t(512) << 20;
        Stats stats;

        uint64_t identityOf(const std::shared_ptr<const Texture> &texture)
        {
            Known &k = known[texture.get()];
            if (k.texture.expired() || k.texture.lock() != texture)
            {
                k = Known{texture, textureIdentity(*texture)};
                if (known.size() > 65536u)
                {
                    const Known keep = k;
                    known.clear();
                    known[texture.get()] = keep;
                }
            }
            return known[texture.get()].identity;
        }

        void drop(uint64_t identity)
        {
            const auto it = entries.find(identity);
            if (it == entries.end())
                return;
            if (it->second.status == Status::Ready)
            {
                stats.bytes -= it->second.bytes;
                recent.erase(it->second.recent);
            }
            entries.erase(it);
        }

        void trim()
        {
            while (stats.bytes > budget && recent.size() > 1u)
                drop(recent.back());
        }

        // The folder's <identity>.png files; true when the list differs from the one before (entries of changed files
        // dropped).
        bool list()
        {
            std::unordered_map<uint64_t, File> now;
            std::error_code ec;
            if (!folder.empty())
                for (std::filesystem::directory_iterator it(std::filesystem::path(folder), ec), end; !ec && it != end; it.increment(ec))
                {
                    uint64_t identity = 0;
                    if (!identityName(it->path(), identity))
                        continue;
                    std::error_code fe;
                    File f{it->path().string(), it->last_write_time(fe), it->file_size(fe)};
                    now.emplace(identity, std::move(f));
                }
            bool changed = now.size() != files.size();
            for (const auto &[identity, f] : files)
            {
                const auto it = now.find(identity);
                if (it == now.end() || it->second.time != f.time || it->second.size != f.size)
                {
                    changed = true;
                    drop(identity);
                }
            }
            files = std::move(now);
            stats.files = files.size();
            if (changed)
                ++generation;
            return changed;
        }

        // A replacement's copy from its file, or null (refused: the reason in `why`).
        static std::shared_ptr<const Texture> load(const Job &job, std::string &why)
        {
            uint32_t width = 0, height = 0;
            std::vector<uint32_t> rgba;
            if (!readPng(job.path, width, height, rgba, &why))
                return nullptr;
            const Texture &source = *job.source;
            const uint32_t factor = source.width ? width / source.width : 0u;
            if (factor == 0u || (factor & (factor - 1u)) != 0u || factor > 8u || width != source.width * factor ||
                height != source.height * factor || width > kUpscaleMaxSide || height > kUpscaleMaxSide)
            {
                char text[160];
                std::snprintf(text, sizeof(text), "%ux%u for a %ux%u texture (the same size, or 2, 4 or 8 times it each way)",
                              width, height, source.width, source.height);
                why = text;
                return nullptr;
            }
            auto copy = std::make_shared<Texture>();
            copy->width = width;
            copy->height = height;
            copy->key = source.key;
            copy->version = source.version;
            copy->pages = source.pages;
            copy->asset = true;
            if (factor == 1u)
                copy->levels = source.levels;
            else
            {
                uint32_t w = width, h = height;
                const std::vector<uint32_t> *last = &rgba;
                for (uint32_t f = factor; f > 2u; f >>= 1)
                {
                    uint32_t nw = 0, nh = 0;
                    copy->levels.push_back(halve(*last, w, h, nw, nh));
                    last = &copy->levels.back();
                    w = nw;
                    h = nh;
                }
                copy->levels.push_back(source.rgba);
                copy->levels.insert(copy->levels.end(), source.levels.begin(), source.levels.end());
            }
            copy->rgba = std::move(rgba);
            return copy;
        }

        void work()
        {
            std::unique_lock<std::mutex> lock(mutex);
            for (;;)
            {
                wake.wait(lock, [&] { return !jobs.empty(); });
                Job job = std::move(jobs.front());
                jobs.pop_front();
                ++busy;
                lock.unlock();
                const auto start = std::chrono::steady_clock::now();
                std::string why;
                std::shared_ptr<const Texture> copy = load(job, why);
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                lock.lock();
                --busy;
                stats.seconds += seconds;
                const auto it = entries.find(job.identity);
                if (job.generation == generation && it != entries.end() && it->second.status == Status::Queued)
                {
                    if (copy)
                    {
                        Entry &e = it->second;
                        e.status = Status::Ready;
                        e.copy = copy;
                        e.bytes = copy->rgba.size() * sizeof(uint32_t);
                        for (const std::vector<uint32_t> &level : copy->levels)
                            e.bytes += level.size() * sizeof(uint32_t);
                        recent.push_front(job.identity);
                        e.recent = recent.begin();
                        stats.bytes += e.bytes;
                        ++stats.loaded;
                        trim();
                    }
                    else
                    {
                        it->second.status = Status::Refused;
                        ++stats.refused;
                        std::fprintf(stderr, "[render] texture replacement %s refused: %s\n", job.path.c_str(), why.c_str());
                    }
                }
                if (jobs.empty() && busy == 0u)
                    idle.notify_all();
            }
        }
    };

    TextureReplacements &TextureReplacements::instance()
    {
        // Never destroyed: its workers may still be running when the program exits.
        static TextureReplacements *s_instance = new TextureReplacements();
        return *s_instance;
    }

    TextureReplacements::TextureReplacements() : m_state(std::make_shared<State>()) {}

    void TextureReplacements::setFolder(const std::string &folder)
    {
        State &s = *m_state;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.folder = folder;
        s.files.clear();
        while (!s.recent.empty())
            s.drop(s.recent.back());
        s.entries.clear();
        s.list();
        ++s.generation;
        if (!folder.empty())
            std::fprintf(stderr, "[render] texture replacements: %zu in %s\n", s.files.size(), folder.c_str());
    }

    std::string TextureReplacements::folder() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->folder;
    }

    bool TextureReplacements::active() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return !m_state->files.empty();
    }

    bool TextureReplacements::rescan()
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->list();
    }

    uint64_t TextureReplacements::generation() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->generation;
    }

    std::shared_ptr<const Texture> TextureReplacements::find(const std::shared_ptr<const Texture> &source, bool &loading)
    {
        loading = false;
        State &s = *m_state;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.files.empty() || !source || !source->asset || source->width == 0u)
            return nullptr;
        const uint64_t identity = s.identityOf(source);
        const auto file = s.files.find(identity);
        if (file == s.files.end())
            return nullptr;
        const auto found = s.entries.find(identity);
        if (found != s.entries.end())
        {
            State::Entry &e = found->second;
            if (e.status == State::Status::Ready)
            {
                s.recent.splice(s.recent.begin(), s.recent, e.recent);
                ++s.stats.found;
                return e.copy;
            }
            loading = e.status == State::Status::Queued;
            return nullptr;
        }
        s.entries[identity].status = State::Status::Queued;
        s.jobs.push_back(State::Job{identity, s.generation, file->second.path, source});
        if (s.workers.empty())
        {
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            for (unsigned i = 0, n = std::clamp(cores / 4u, 1u, 2u); i < n; ++i)
            {
                s.workers.emplace_back([state = m_state] { lowerWorkerPriority(); state->work(); });
                s.workers.back().detach();
            }
        }
        s.wake.notify_one();
        loading = true;
        return nullptr;
    }

    void TextureReplacements::waitIdle()
    {
        State &s = *m_state;
        std::unique_lock<std::mutex> lock(s.mutex);
        s.idle.wait(lock, [&] { return s.jobs.empty() && s.busy == 0u; });
    }

    void TextureReplacements::setBudget(size_t bytes)
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->budget = bytes;
        m_state->trim();
    }

    TextureReplacements::Stats TextureReplacements::stats() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->stats;
    }
}
