// SPDX-License-Identifier: MIT
// Save state archives and files (bozo/state.h).
#include "bozo/state.h"

#include <cstdio>

namespace bozo::state
{
    namespace
    {
        constexpr char kMagic[8] = {'B', 'O', 'Z', 'O', 'S', 'A', 'V', 'E'};
        constexpr uint32_t kVersion = 1;
    }

    void Archive::raw(void *p, size_t n)
    {
        if (!n)
            return;
        if (!loading_)
        {
            const auto *b = static_cast<const uint8_t *>(p);
            data_.insert(data_.end(), b, b + n);
            return;
        }
        const size_t limit = open_.empty() ? data_.size() : open_.back().second;
        if (n > limit - pos_)
            throw Error("save state: section " + (open_.empty() ? std::string("<file>") : open_.back().first) +
                        " ends before its fields");
        std::memcpy(p, data_.data() + pos_, n);
        pos_ += n;
    }

    void Archive::begin(const char *name)
    {
        std::string n = name;
        io(*this, n);
        if (loading_ && n != name)
            throw Error("save state: expected section " + std::string(name) + ", found " + n);
        uint64_t size = 0;
        const size_t at = loading_ ? pos_ : data_.size();
        raw(&size, sizeof size);
        if (!loading_)
            open_.emplace_back(name, at);
        else
        {
            if (size > data_.size() - pos_)
                throw Error("save state: section " + n + " runs past the file");
            open_.emplace_back(name, pos_ + size);
        }
    }

    void Archive::end()
    {
        if (open_.empty())
            throw Error("save state: a section ended twice");
        const auto [name, at] = open_.back();
        open_.pop_back();
        if (!loading_)
        {
            const uint64_t size = data_.size() - at - sizeof(uint64_t);
            std::memcpy(data_.data() + at, &size, sizeof size);
        }
        else if (pos_ != at)
            throw Error("save state: section " + name + " has " + std::to_string(at - pos_) +
                        " bytes its reader did not take (a different build wrote it?)");
    }

    void writeFile(const std::string &path, const std::string &game, const Archive &a)
    {
        const std::string temp = path + ".tmp";
        std::FILE *f = std::fopen(temp.c_str(), "wb");
        if (!f)
            throw Error("save state: cannot write " + temp);
        Archive header;
        std::string tag = game;
        uint32_t version = kVersion;
        header.raw(const_cast<char *>(kMagic), sizeof kMagic);
        io(header, version);
        io(header, tag);
        const bool ok = std::fwrite(header.data().data(), 1, header.data().size(), f) == header.data().size() &&
                        std::fwrite(a.data().data(), 1, a.data().size(), f) == a.data().size();
        if (std::fclose(f) != 0 || !ok)
        {
            std::remove(temp.c_str());
            throw Error("save state: cannot write " + temp);
        }
        std::remove(path.c_str());
        if (std::rename(temp.c_str(), path.c_str()) != 0)
            throw Error("save state: cannot rename " + temp + " to " + path);
    }

    Archive readFile(const std::string &path, const std::string &game)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
            throw Error("save state: cannot open " + path);
        std::vector<uint8_t> bytes;
        uint8_t buffer[1 << 16];
        for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) != 0;)
            bytes.insert(bytes.end(), buffer, buffer + n);
        std::fclose(f);
        Archive header(std::move(bytes));
        char magic[sizeof kMagic];
        header.raw(magic, sizeof magic);
        if (std::memcmp(magic, kMagic, sizeof kMagic) != 0)
            throw Error("save state: " + path + " is not a save state");
        uint32_t version = 0;
        std::string tag;
        io(header, version);
        io(header, tag);
        if (version != kVersion)
            throw Error("save state: " + path + " has format " + std::to_string(version) + ", this build reads " +
                        std::to_string(kVersion));
        if (tag != game)
            throw Error("save state: " + path + " is for " + tag + ", not " + game);
        // The rest, as an archive of its own.
        std::vector<uint8_t> rest;
        const size_t used = sizeof kMagic + sizeof version + sizeof(uint64_t) + tag.size();
        rest.assign(header.data().begin() + static_cast<std::ptrdiff_t>(used), header.data().end());
        return Archive(std::move(rest));
    }
}
