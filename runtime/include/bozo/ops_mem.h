// Guest memory accesses for recompiled code (r5900.md "Loads and stores"). Little-endian, like the R5900.
//
// Fast path: an address below 32 MB is main RAM, read or written in place. Everything else (the KSEG0/KSEG1 and
// uncached mirrors of RAM, the scratchpad at 0x70000000, I/O) goes through the runtime's bozo_memory_slow, and if no
// memory is behind the address, to bozo_io_read/write.
//
// Alignment: the console raises an address error for a misaligned lh/lw/ld/sh/sw/sd; games do not rely on it and the
// recompiled code simply performs the access at the address given. lq/sq (and lqc2/sqc2) ignore the low 4 bits of the
// address, as the hardware does.
#pragma once

#include "bozo/runtime_api.h"

#include <cstring>

#if defined(__GNUC__)
#define BOZO_LIKELY(x) __builtin_expect(!!(x), 1)
#define BOZO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BOZO_INLINE inline __attribute__((always_inline))
#else
#define BOZO_LIKELY(x) (x)
#define BOZO_UNLIKELY(x) (x)
#define BOZO_INLINE inline
#endif

namespace bozo
{
    constexpr uint32_t kRamSize = 32u * 1024u * 1024u;

    // The physical address behind a guest virtual address, for the runtime's slow path: KSEG0 (0x80000000) and KSEG1
    // (0xA0000000) map onto the low 512 MB; on the EE, 0x20000000 and 0x30000000 are uncached and uncached-accelerated
    // views of RAM. The scratchpad (0x70000000) has no physical address: the runtime checks it before calling this.
    inline uint32_t physicalAddress(uint32_t addr)
    {
        if (addr >= 0x80000000u && addr < 0xC0000000u)
            return addr & 0x1FFFFFFFu;
        if (addr >= 0x20000000u && addr < 0x40000000u)
            return addr & 0x0FFFFFFFu; // RAM aliases at 0x20000000/0x30000000
        return addr;
    }

    template <typename T>
    BOZO_INLINE T loadRaw(const uint8_t *p)
    {
        T v;
        std::memcpy(&v, p, sizeof(T));
        return v;
    }
    template <typename T>
    BOZO_INLINE void storeRaw(uint8_t *p, T v)
    {
        std::memcpy(p, &v, sizeof(T));
    }

    template <typename T>
    inline T loadSlow(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr)
    {
        if (uint8_t *p = bozo_memory_slow(rdram, rt, addr, sizeof(T), false))
            return loadRaw<T>(p);
        return static_cast<T>(bozo_io_read(ctx, rt, addr, sizeof(T)));
    }
    template <typename T>
    inline void storeSlow(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, T v)
    {
        if (uint8_t *p = bozo_memory_slow(rdram, rt, addr, sizeof(T), true))
            storeRaw<T>(p, v);
        else
            bozo_io_write(ctx, rt, addr, sizeof(T), static_cast<uint64_t>(v));
    }

    template <typename T>
    BOZO_INLINE T load(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr)
    {
        if (BOZO_LIKELY(addr <= kRamSize - sizeof(T)))
            return loadRaw<T>(rdram + addr);
        return loadSlow<T>(rdram, ctx, rt, addr);
    }
    template <typename T>
    BOZO_INLINE void store(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, T v)
    {
        if (BOZO_LIKELY(addr <= kRamSize - sizeof(T)))
            storeRaw<T>(rdram + addr, v);
        else
            storeSlow<T>(rdram, ctx, rt, addr, v);
    }

    // 128-bit: the address's low 4 bits are ignored.
    inline void load128(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, void *out)
    {
        addr &= ~15u;
        if (BOZO_LIKELY(addr < kRamSize))
        {
            std::memcpy(out, rdram + addr, 16);
            return;
        }
        if (uint8_t *p = bozo_memory_slow(rdram, rt, addr, 16, false))
        {
            std::memcpy(out, p, 16);
            return;
        }
        uint64_t v[2];
        bozo_io_read128(ctx, rt, addr, v);
        std::memcpy(out, v, 16);
    }
    inline void store128(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, const void *in)
    {
        addr &= ~15u;
        if (BOZO_LIKELY(addr < kRamSize))
        {
            std::memcpy(rdram + addr, in, 16);
            return;
        }
        if (uint8_t *p = bozo_memory_slow(rdram, rt, addr, 16, true))
        {
            std::memcpy(p, in, 16);
            return;
        }
        uint64_t v[2];
        std::memcpy(v, in, 16);
        bozo_io_write128(ctx, rt, addr, v);
    }

    // ---- Unaligned word and doubleword access (lwl/lwr/swl/swr, ldl/ldr/sdl/sdr), little-endian ----------------------
    // k = the address's offset in its aligned word (or doubleword). The "left" forms move the most significant bytes
    // of the register, the "right" forms the least significant ones.

    // lwl: returns the new 64-bit register value (the merged word, sign-extended).
    inline uint64_t lwl(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, uint64_t old)
    {
        const uint32_t k = addr & 3u;
        const uint32_t mem = load<uint32_t>(rdram, ctx, rt, addr & ~3u);
        const uint32_t shift = 24u - 8u * k;
        const uint32_t keep = 0x00FFFFFFu >> (8u * k); // the register's bytes not replaced
        const uint32_t merged = (mem << shift) | (static_cast<uint32_t>(old) & keep);
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(merged)));
    }
    // lwr: when the whole word is loaded (k = 0) the result is sign-extended; otherwise the register's upper 32 bits
    // are kept (r5900.md, open question Q4).
    inline uint64_t lwr(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, uint64_t old)
    {
        const uint32_t k = addr & 3u;
        const uint32_t mem = load<uint32_t>(rdram, ctx, rt, addr & ~3u);
        if (k == 0)
            return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(mem)));
        const uint32_t keep = ~(0xFFFFFFFFu >> (8u * k));
        const uint32_t merged = (mem >> (8u * k)) | (static_cast<uint32_t>(old) & keep);
        return (old & 0xFFFFFFFF00000000ull) | merged;
    }
    inline void swl(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, uint64_t value)
    {
        const uint32_t k = addr & 3u;
        const uint32_t a = addr & ~3u;
        const uint32_t mem = load<uint32_t>(rdram, ctx, rt, a);
        const uint32_t v = static_cast<uint32_t>(value);
        const uint32_t keep = k == 3u ? 0u : (0xFFFFFF00u << (8u * k));
        store<uint32_t>(rdram, ctx, rt, a, (v >> (24u - 8u * k)) | (mem & keep));
    }
    inline void swr(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, uint64_t value)
    {
        const uint32_t k = addr & 3u;
        const uint32_t a = addr & ~3u;
        const uint32_t mem = load<uint32_t>(rdram, ctx, rt, a);
        const uint32_t v = static_cast<uint32_t>(value);
        const uint32_t keep = k == 0u ? 0u : (0xFFFFFFFFu >> (32u - 8u * k));
        store<uint32_t>(rdram, ctx, rt, a, (v << (8u * k)) | (mem & keep));
    }
    inline uint64_t ldl(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, uint64_t old)
    {
        const uint32_t k = addr & 7u;
        const uint64_t mem = load<uint64_t>(rdram, ctx, rt, addr & ~7u);
        const uint64_t keep = 0x00FFFFFFFFFFFFFFull >> (8u * k);
        return (mem << (56u - 8u * k)) | (old & keep);
    }
    inline uint64_t ldr(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, uint64_t old)
    {
        const uint32_t k = addr & 7u;
        const uint64_t mem = load<uint64_t>(rdram, ctx, rt, addr & ~7u);
        if (k == 0)
            return mem;
        const uint64_t keep = ~(~0ull >> (8u * k));
        return (mem >> (8u * k)) | (old & keep);
    }
    inline void sdl(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, uint64_t value)
    {
        const uint32_t k = addr & 7u;
        const uint32_t a = addr & ~7u;
        const uint64_t mem = load<uint64_t>(rdram, ctx, rt, a);
        const uint64_t keep = k == 7u ? 0ull : (0xFFFFFFFFFFFFFF00ull << (8u * k));
        store<uint64_t>(rdram, ctx, rt, a, (value >> (56u - 8u * k)) | (mem & keep));
    }
    inline void sdr(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t addr, uint64_t value)
    {
        const uint32_t k = addr & 7u;
        const uint32_t a = addr & ~7u;
        const uint64_t mem = load<uint64_t>(rdram, ctx, rt, a);
        const uint64_t keep = k == 0u ? 0ull : (~0ull >> (64u - 8u * k));
        store<uint64_t>(rdram, ctx, rt, a, (value << (8u * k)) | (mem & keep));
    }
}
