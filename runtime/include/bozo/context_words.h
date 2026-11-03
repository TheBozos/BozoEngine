// A guest context as the 332 words of the records (capture.md section 5, calllog.md): the GPRs (128 bits each),
// HI/LO and HI1/LO1, pc, sa, the FPU, VU0's macro-mode state and COP0 Status. The runtime keeps Status outside the
// context (Runtime::cop0Status for the running one), so it is passed separately.
#pragma once

#include "bozo/context.h"

#include <array>
#include <cstdint>

namespace bozo
{
    constexpr uint32_t kContextWords = 332;
    constexpr uint32_t kContextMaskWords = (kContextWords + 31) / 32; // 11

    constexpr uint32_t kWordHi = 128, kWordLo = 130, kWordHi1 = 132, kWordLo1 = 134;
    constexpr uint32_t kWordPc = 136, kWordSa = 137, kWordF0 = 138, kWordFcr31 = 170, kWordFacc = 171;
    constexpr uint32_t kWordVf0 = 172, kWordAcc = 300, kWordVi0 = 304, kWordQ = 320, kWordI = 321, kWordP = 322,
                       kWordR = 323, kWordVuStatus = 324, kWordVuMac = 325, kWordVuClip = 326, kWordCmsar0 = 327,
                       kWordCop0Status = 328;
    constexpr uint32_t gprWord(uint32_t reg, uint32_t lane = 0) { return reg * 4 + lane; }

    using ContextWords = std::array<uint32_t, kContextWords>;

    namespace detail
    {
        inline uint32_t lo32(uint64_t v) { return static_cast<uint32_t>(v); }
        inline uint32_t hi32(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
        inline void setLo(uint64_t &v, uint32_t x) { v = (v & 0xFFFFFFFF00000000ull) | x; }
        inline void setHi(uint64_t &v, uint32_t x) { v = (v & 0xFFFFFFFFull) | (static_cast<uint64_t>(x) << 32); }
    }

    inline uint32_t contextWord(const R5900Context &c, uint32_t status, uint32_t i)
    {
        using namespace detail;
        if (i < 128)
            return c.r[i / 4].uw[i % 4];
        switch (i)
        {
        case kWordHi: return lo32(c.hi);
        case kWordHi + 1: return hi32(c.hi);
        case kWordLo: return lo32(c.lo);
        case kWordLo + 1: return hi32(c.lo);
        case kWordHi1: return lo32(c.hi1);
        case kWordHi1 + 1: return hi32(c.hi1);
        case kWordLo1: return lo32(c.lo1);
        case kWordLo1 + 1: return hi32(c.lo1);
        case kWordPc: return c.pc;
        case kWordSa: return c.sa;
        case kWordFcr31: return c.fcr31;
        case kWordFacc: return c.facc;
        case kWordQ: return c.vu0_q_bits;
        case kWordI: return c.vu0_i_bits;
        case kWordP: return c.vu0_p_bits;
        case kWordR: return c.vu0_r;
        case kWordVuStatus: return c.vu0_status;
        case kWordVuMac: return c.vu0_mac;
        case kWordVuClip: return c.vu0_clip_flags;
        case kWordCmsar0: return c.vu0_cmsar0;
        case kWordCop0Status: return status;
        default: break;
        }
        if (i >= kWordF0 && i < kWordFcr31)
            return c.fu[i - kWordF0];
        if (i >= kWordVf0 && i < kWordAcc)
            return c.vu0_vf[(i - kWordVf0) / 4].u[(i - kWordVf0) % 4];
        if (i >= kWordAcc && i < kWordVi0)
            return c.vu0_acc.u[i - kWordAcc];
        if (i >= kWordVi0 && i < kWordQ)
            return c.vi[i - kWordVi0];
        return 0; // reserved
    }

    inline void setContextWord(R5900Context &c, uint32_t &status, uint32_t i, uint32_t v)
    {
        using namespace detail;
        if (i < 128)
        {
            c.r[i / 4].uw[i % 4] = v;
            return;
        }
        switch (i)
        {
        case kWordHi: setLo(c.hi, v); return;
        case kWordHi + 1: setHi(c.hi, v); return;
        case kWordLo: setLo(c.lo, v); return;
        case kWordLo + 1: setHi(c.lo, v); return;
        case kWordHi1: setLo(c.hi1, v); return;
        case kWordHi1 + 1: setHi(c.hi1, v); return;
        case kWordLo1: setLo(c.lo1, v); return;
        case kWordLo1 + 1: setHi(c.lo1, v); return;
        case kWordPc: c.pc = v; return;
        case kWordSa: c.sa = v; return;
        case kWordFcr31: c.fcr31 = v; return;
        case kWordFacc: c.facc = v; return;
        case kWordQ: c.vu0_q_bits = v; return;
        case kWordI: c.vu0_i_bits = v; return;
        case kWordP: c.vu0_p_bits = v; return;
        case kWordR: c.vu0_r = v; return;
        case kWordVuStatus: c.vu0_status = v; return;
        case kWordVuMac: c.vu0_mac = v; return;
        case kWordVuClip: c.vu0_clip_flags = v; return;
        case kWordCmsar0: c.vu0_cmsar0 = v; return;
        case kWordCop0Status: status = v; return;
        default: break;
        }
        if (i >= kWordF0 && i < kWordFcr31)
            c.fu[i - kWordF0] = v;
        else if (i >= kWordVf0 && i < kWordAcc)
            c.vu0_vf[(i - kWordVf0) / 4].u[(i - kWordVf0) % 4] = v;
        else if (i >= kWordAcc && i < kWordVi0)
            c.vu0_acc.u[i - kWordAcc] = v;
        else if (i >= kWordVi0 && i < kWordQ)
            c.vi[i - kWordVi0] = v;
    }

    inline void toWords(const R5900Context &c, uint32_t status, ContextWords &out)
    {
        for (uint32_t i = 0; i < kContextWords; ++i)
            out[i] = contextWord(c, status, i);
    }
    inline void fromWords(R5900Context &c, uint32_t &status, const ContextWords &in)
    {
        for (uint32_t i = 0; i < kContextWords; ++i)
            setContextWord(c, status, i, in[i]);
    }
}
