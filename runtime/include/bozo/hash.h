// The hashes BozoEngine's records use: XXH64 (the public xxHash specification, seed as given) and FNV-1a (32 and 64
// bits). Written from the algorithms' published descriptions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bozo::hash
{
    namespace detail
    {
        constexpr uint64_t kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full, kP3 = 0x165667B19E3779F9ull,
                           kP4 = 0x85EBCA77C2B2AE63ull, kP5 = 0x27D4EB2F165667C5ull;
        inline uint64_t rotl(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }
        inline uint64_t rd64(const uint8_t *p)
        {
            uint64_t v;
            std::memcpy(&v, p, 8);
            return v;
        }
        inline uint32_t rd32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        inline uint64_t round(uint64_t acc, uint64_t in) { return rotl(acc + in * kP2, 31) * kP1; }
        inline uint64_t merge(uint64_t acc, uint64_t v) { return (acc ^ round(0, v)) * kP1 + kP4; }
    }

    // XXH64 of `len` bytes (little-endian host).
    inline uint64_t xxh64(const void *data, size_t len, uint64_t seed = 0)
    {
        using namespace detail;
        const uint8_t *p = static_cast<const uint8_t *>(data);
        const uint8_t *const end = p + len;
        uint64_t acc;
        if (len >= 32)
        {
            uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
            const uint8_t *const limit = end - 32;
            do
            {
                v1 = round(v1, rd64(p));
                v2 = round(v2, rd64(p + 8));
                v3 = round(v3, rd64(p + 16));
                v4 = round(v4, rd64(p + 24));
                p += 32;
            } while (p <= limit);
            acc = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
            acc = merge(acc, v1);
            acc = merge(acc, v2);
            acc = merge(acc, v3);
            acc = merge(acc, v4);
        }
        else
            acc = seed + kP5;
        acc += len;
        while (p + 8 <= end)
        {
            acc ^= round(0, rd64(p));
            acc = rotl(acc, 27) * kP1 + kP4;
            p += 8;
        }
        if (p + 4 <= end)
        {
            acc ^= uint64_t(rd32(p)) * kP1;
            acc = rotl(acc, 23) * kP2 + kP3;
            p += 4;
        }
        while (p < end)
        {
            acc ^= uint64_t(*p) * kP5;
            acc = rotl(acc, 11) * kP1;
            ++p;
        }
        acc ^= acc >> 33;
        acc *= kP2;
        acc ^= acc >> 29;
        acc *= kP3;
        acc ^= acc >> 32;
        return acc;
    }

    inline uint32_t fnv1a32(const void *data, size_t len)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        uint32_t h = 0x811C9DC5u;
        for (size_t i = 0; i < len; ++i)
            h = (h ^ p[i]) * 0x01000193u;
        return h;
    }
    inline uint32_t fnv1a32(const char *s) { return fnv1a32(s, std::strlen(s)); }

    inline uint64_t fnv1a64(const void *data, size_t len, uint64_t h = 0xCBF29CE484222325ull)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < len; ++i)
            h = (h ^ p[i]) * 0x100000001B3ull;
        return h;
    }
}
