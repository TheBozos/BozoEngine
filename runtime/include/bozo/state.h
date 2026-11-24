// SPDX-License-Identifier: MIT
// Save states: one archive that writes or reads the runtime's whole state at a dispatcher safe point (no guest code on
// the host stack: every thread's registers are in its kernel frame). One function per component walks its fields in a
// fixed order for both directions; a file is a header, then named sections whose sizes are checked as they are read.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bozo::state
{
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    class Archive
    {
    public:
        // A writer, or a reader of `data` (a whole file).
        Archive() = default;
        explicit Archive(std::vector<uint8_t> data) : data_(std::move(data)), loading_(true) {}

        bool loading() const { return loading_; }
        void raw(void *p, size_t n);
        // A named section: begin and end bracket its fields (sections nest). Reading checks the name and that the
        // fields took exactly the bytes written.
        void begin(const char *name);
        void end();
        // A place to read again from: a reader whose section turned out to be in an older layout goes back to the mark
        // and reads it the old way (the sections' size checks tell the layouts apart).
        struct Mark
        {
            size_t pos = 0, depth = 0;
        };
        Mark mark() const { return {pos_, open_.size()}; }
        void rewind(const Mark &m)
        {
            pos_ = m.pos;
            open_.resize(m.depth);
        }
        const std::vector<uint8_t> &data() const { return data_; }
        bool atEnd() const { return pos_ == data_.size(); }

    private:
        std::vector<uint8_t> data_;
        size_t pos_ = 0;
        bool loading_ = false;
        std::vector<std::pair<std::string, size_t>> open_; // name, where its size word is (writing) or its end (reading)
    };

    // A part of the state a port or a library host keeps outside the engine's own (its native hosts, its registrations
    // with the libraries, its caches of guest data). `serialize` walks its fields both ways; after a load it also
    // drops anything derived from the old state. `unsaveable` (optional) says why the state cannot be saved now.
    struct Section
    {
        std::string name;
        std::function<void(Archive &)> serialize;
        std::function<std::string()> unsaveable;
    };
    class Sections
    {
    public:
        void add(Section s)
        {
            for (const Section &other : sections_)
                if (other.name == s.name)
                    throw std::invalid_argument("save state section " + s.name + " added twice");
            sections_.push_back(std::move(s));
        }
        std::vector<Section> &all() { return sections_; }

    private:
        std::vector<Section> sections_;
    };

    // Whole files: a header (magic, format version, the game's tag) then the archive's bytes.
    void writeFile(const std::string &path, const std::string &game, const Archive &a);
    Archive readFile(const std::string &path, const std::string &game);

    // ---- Field walkers ----------------------------------------------------------------------------------------------
    // Scalars and enums as their bytes; containers as a count then their elements; every other type by an io() found
    // by argument-dependent lookup in its own namespace.
    template <class T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    void io(Archive &a, T &v)
    {
        a.raw(&v, sizeof v);
    }
    inline void io(Archive &a, bool &v)
    {
        uint8_t b = v ? 1 : 0;
        a.raw(&b, 1);
        v = b != 0;
    }
    // A trivially copyable struct with no pointers inside, as its bytes.
    template <class T> void pod(Archive &a, T &v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        a.raw(&v, sizeof v);
    }

    inline uint64_t count(Archive &a, size_t n)
    {
        uint64_t c = n;
        a.raw(&c, sizeof c);
        if (a.loading() && c > (1ull << 32))
            throw Error("save state: a container count is out of range");
        return c;
    }

    inline void io(Archive &a, std::string &s)
    {
        const uint64_t n = count(a, s.size());
        if (a.loading())
            s.resize(n);
        a.raw(s.data(), n);
    }
    template <class T, size_t N> void io(Archive &a, T (&v)[N]);
    template <class T, size_t N> void io(Archive &a, std::array<T, N> &v);
    template <class T> void io(Archive &a, std::vector<T> &v);
    template <class T> void io(Archive &a, std::deque<T> &v);
    template <class T> void io(Archive &a, std::set<T> &v);
    template <class K, class V> void io(Archive &a, std::map<K, V> &m);
    template <class K, class V> void io(Archive &a, std::unordered_map<K, V> &m);
    template <class A, class B> void io(Archive &a, std::pair<A, B> &p);
    template <class T> void io(Archive &a, std::unique_ptr<T> &p);

    template <class... T> void fields(Archive &a, T &...v) { (io(a, v), ...); }

    template <class T, size_t N> void io(Archive &a, T (&v)[N])
    {
        if constexpr (std::is_arithmetic_v<T>)
            a.raw(v, sizeof v);
        else
            for (auto &e : v)
                io(a, e);
    }
    template <class T, size_t N> void io(Archive &a, std::array<T, N> &v)
    {
        if constexpr (std::is_arithmetic_v<T>)
            a.raw(v.data(), sizeof(T) * N);
        else
            for (auto &e : v)
                io(a, e);
    }
    template <class T> void io(Archive &a, std::vector<T> &v)
    {
        const uint64_t n = count(a, v.size());
        if (a.loading())
            v.resize(n);
        if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
            a.raw(v.data(), sizeof(T) * n);
        else
            for (auto &e : v)
                io(a, e);
    }
    template <class T> void io(Archive &a, std::deque<T> &v)
    {
        const uint64_t n = count(a, v.size());
        if (a.loading())
            v.resize(n);
        for (auto &e : v)
            io(a, e);
    }
    template <class T> void io(Archive &a, std::set<T> &v)
    {
        const uint64_t n = count(a, v.size());
        if (!a.loading())
        {
            for (T e : v)
                io(a, e);
            return;
        }
        v.clear();
        for (uint64_t i = 0; i < n; ++i)
        {
            T e{};
            io(a, e);
            v.insert(std::move(e));
        }
    }
    template <class K, class V> void io(Archive &a, std::map<K, V> &m)
    {
        const uint64_t n = count(a, m.size());
        if (!a.loading())
        {
            for (auto &[k, v] : m)
            {
                K key = k;
                io(a, key);
                io(a, v);
            }
            return;
        }
        m.clear();
        for (uint64_t i = 0; i < n; ++i)
        {
            K key{};
            io(a, key);
            io(a, m[key]);
        }
    }
    // Written in key order, so the same state always gives the same bytes.
    template <class K, class V> void io(Archive &a, std::unordered_map<K, V> &m)
    {
        if (!a.loading())
        {
            std::map<K, V> sorted(m.begin(), m.end());
            io(a, sorted);
            return;
        }
        std::map<K, V> sorted;
        io(a, sorted);
        m.clear();
        for (auto &[k, v] : sorted)
            m.emplace(k, std::move(v));
    }
    template <class A, class B> void io(Archive &a, std::pair<A, B> &p)
    {
        io(a, p.first);
        io(a, p.second);
    }
    template <class T> void io(Archive &a, std::unique_ptr<T> &p)
    {
        bool present = p != nullptr;
        io(a, present);
        if (a.loading())
            p = present ? std::make_unique<T>() : nullptr;
        if (p)
            io(a, *p);
    }
}
