// The R5900's 128-bit multimedia instructions (MMI), lane by lane. Spec: docs/specs/r5900.md, "MMI".
// Each helper takes its sources by value (a destination may be one of its sources) and returns the result.
#pragma once

#include "bozo/context.h"

#include <cstdint>

namespace bozo::mmi
{
    using V = Gpr128;

    template <typename T>
    inline T satAdd(int64_t v, int64_t lo, int64_t hi)
    {
        return static_cast<T>(v < lo ? lo : (v > hi ? hi : v));
    }

    // ---- Add / subtract ----
    inline V paddw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.uw[i] = s.uw[i] + t.uw[i]; return d; }
    inline V psubw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.uw[i] = s.uw[i] - t.uw[i]; return d; }
    inline V paddh(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.uh[i] = static_cast<uint16_t>(s.uh[i] + t.uh[i]); return d; }
    inline V psubh(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.uh[i] = static_cast<uint16_t>(s.uh[i] - t.uh[i]); return d; }
    inline V paddb(V s, V t) { V d; for (int i = 0; i < 16; ++i) d.ub[i] = static_cast<uint8_t>(s.ub[i] + t.ub[i]); return d; }
    inline V psubb(V s, V t) { V d; for (int i = 0; i < 16; ++i) d.ub[i] = static_cast<uint8_t>(s.ub[i] - t.ub[i]); return d; }
    inline V paddsw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.sw[i] = satAdd<int32_t>(int64_t(s.sw[i]) + t.sw[i], INT32_MIN, INT32_MAX); return d; }
    inline V psubsw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.sw[i] = satAdd<int32_t>(int64_t(s.sw[i]) - t.sw[i], INT32_MIN, INT32_MAX); return d; }
    inline V paddsh(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.sh[i] = satAdd<int16_t>(int64_t(s.sh[i]) + t.sh[i], INT16_MIN, INT16_MAX); return d; }
    inline V psubsh(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.sh[i] = satAdd<int16_t>(int64_t(s.sh[i]) - t.sh[i], INT16_MIN, INT16_MAX); return d; }
    inline V paddsb(V s, V t) { V d; for (int i = 0; i < 16; ++i) d.sb[i] = satAdd<int8_t>(int64_t(s.sb[i]) + t.sb[i], INT8_MIN, INT8_MAX); return d; }
    inline V psubsb(V s, V t) { V d; for (int i = 0; i < 16; ++i) d.sb[i] = satAdd<int8_t>(int64_t(s.sb[i]) - t.sb[i], INT8_MIN, INT8_MAX); return d; }
    inline V padduw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.uw[i] = satAdd<uint32_t>(int64_t(s.uw[i]) + t.uw[i], 0, UINT32_MAX); return d; }
    inline V psubuw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.uw[i] = satAdd<uint32_t>(int64_t(s.uw[i]) - t.uw[i], 0, UINT32_MAX); return d; }
    inline V padduh(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.uh[i] = satAdd<uint16_t>(int64_t(s.uh[i]) + t.uh[i], 0, UINT16_MAX); return d; }
    inline V psubuh(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.uh[i] = satAdd<uint16_t>(int64_t(s.uh[i]) - t.uh[i], 0, UINT16_MAX); return d; }
    inline V paddub(V s, V t) { V d; for (int i = 0; i < 16; ++i) d.ub[i] = satAdd<uint8_t>(int64_t(s.ub[i]) + t.ub[i], 0, UINT8_MAX); return d; }
    inline V psubub(V s, V t) { V d; for (int i = 0; i < 16; ++i) d.ub[i] = satAdd<uint8_t>(int64_t(s.ub[i]) - t.ub[i], 0, UINT8_MAX); return d; }
    // padsbh: the lower four halfwords subtract, the upper four add.
    inline V padsbh(V s, V t)
    {
        V d;
        for (int i = 0; i < 4; ++i) d.uh[i] = static_cast<uint16_t>(s.uh[i] - t.uh[i]);
        for (int i = 4; i < 8; ++i) d.uh[i] = static_cast<uint16_t>(s.uh[i] + t.uh[i]);
        return d;
    }

    // ---- Compare, min, max, abs ----
    inline V pcgtw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.uw[i] = s.sw[i] > t.sw[i] ? ~0u : 0u; return d; }
    inline V pcgth(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.uh[i] = s.sh[i] > t.sh[i] ? 0xFFFFu : 0u; return d; }
    inline V pcgtb(V s, V t) { V d; for (int i = 0; i < 16; ++i) d.ub[i] = s.sb[i] > t.sb[i] ? 0xFFu : 0u; return d; }
    inline V pceqw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.uw[i] = s.uw[i] == t.uw[i] ? ~0u : 0u; return d; }
    inline V pceqh(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.uh[i] = s.uh[i] == t.uh[i] ? 0xFFFFu : 0u; return d; }
    inline V pceqb(V s, V t) { V d; for (int i = 0; i < 16; ++i) d.ub[i] = s.ub[i] == t.ub[i] ? 0xFFu : 0u; return d; }
    inline V pmaxw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.sw[i] = s.sw[i] > t.sw[i] ? s.sw[i] : t.sw[i]; return d; }
    inline V pminw(V s, V t) { V d; for (int i = 0; i < 4; ++i) d.sw[i] = s.sw[i] < t.sw[i] ? s.sw[i] : t.sw[i]; return d; }
    inline V pmaxh(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.sh[i] = s.sh[i] > t.sh[i] ? s.sh[i] : t.sh[i]; return d; }
    inline V pminh(V s, V t) { V d; for (int i = 0; i < 8; ++i) d.sh[i] = s.sh[i] < t.sh[i] ? s.sh[i] : t.sh[i]; return d; }
    // pabsw / pabsh: the most negative value saturates to the most positive (open question Q8).
    inline V pabsw(V t) { V d; for (int i = 0; i < 4; ++i) d.sw[i] = t.sw[i] == INT32_MIN ? INT32_MAX : (t.sw[i] < 0 ? -t.sw[i] : t.sw[i]); return d; }
    inline V pabsh(V t) { V d; for (int i = 0; i < 8; ++i) d.sh[i] = t.sh[i] == INT16_MIN ? INT16_MAX : static_cast<int16_t>(t.sh[i] < 0 ? -t.sh[i] : t.sh[i]); return d; }

    // ---- Logic ----
    inline V pand(V s, V t) { V d; d.ud[0] = s.ud[0] & t.ud[0]; d.ud[1] = s.ud[1] & t.ud[1]; return d; }
    inline V por(V s, V t) { V d; d.ud[0] = s.ud[0] | t.ud[0]; d.ud[1] = s.ud[1] | t.ud[1]; return d; }
    inline V pxor(V s, V t) { V d; d.ud[0] = s.ud[0] ^ t.ud[0]; d.ud[1] = s.ud[1] ^ t.ud[1]; return d; }
    inline V pnor(V s, V t) { V d; d.ud[0] = ~(s.ud[0] | t.ud[0]); d.ud[1] = ~(s.ud[1] | t.ud[1]); return d; }

    // ---- Shifts (amount from the instruction's sa field, or from a register for the "v" forms) ----
    inline V psllh(V t, unsigned sa) { V d; for (int i = 0; i < 8; ++i) d.uh[i] = static_cast<uint16_t>(t.uh[i] << (sa & 15)); return d; }
    inline V psrlh(V t, unsigned sa) { V d; for (int i = 0; i < 8; ++i) d.uh[i] = static_cast<uint16_t>(t.uh[i] >> (sa & 15)); return d; }
    inline V psrah(V t, unsigned sa) { V d; for (int i = 0; i < 8; ++i) d.sh[i] = static_cast<int16_t>(t.sh[i] >> (sa & 15)); return d; }
    inline V psllw(V t, unsigned sa) { V d; for (int i = 0; i < 4; ++i) d.uw[i] = t.uw[i] << (sa & 31); return d; }
    inline V psrlw(V t, unsigned sa) { V d; for (int i = 0; i < 4; ++i) d.uw[i] = t.uw[i] >> (sa & 31); return d; }
    inline V psraw(V t, unsigned sa) { V d; for (int i = 0; i < 4; ++i) d.sw[i] = t.sw[i] >> (sa & 31); return d; }
    // psllvw / psrlvw / psravw: words 0 and 2, shift amounts from rs's words 0 and 2; results sign-extended to 64 bits.
    inline V psllvw(V t, V s)
    {
        V d;
        for (int i = 0; i < 2; ++i) d.sd[i] = static_cast<int32_t>(t.uw[2 * i] << (s.uw[2 * i] & 31));
        return d;
    }
    inline V psrlvw(V t, V s)
    {
        V d;
        for (int i = 0; i < 2; ++i) d.sd[i] = static_cast<int32_t>(t.uw[2 * i] >> (s.uw[2 * i] & 31));
        return d;
    }
    inline V psravw(V t, V s)
    {
        V d;
        for (int i = 0; i < 2; ++i) d.sd[i] = t.sw[2 * i] >> (s.uw[2 * i] & 31);
        return d;
    }

    // ---- Pack / extend / interleave ----
    // pextlw: interleave the low words: d = {t.w0, s.w0, t.w1, s.w1}.
    inline V pextlw(V s, V t) { V d; d.uw[0] = t.uw[0]; d.uw[1] = s.uw[0]; d.uw[2] = t.uw[1]; d.uw[3] = s.uw[1]; return d; }
    inline V pextuw(V s, V t) { V d; d.uw[0] = t.uw[2]; d.uw[1] = s.uw[2]; d.uw[2] = t.uw[3]; d.uw[3] = s.uw[3]; return d; }
    inline V pextlh(V s, V t) { V d; for (int i = 0; i < 4; ++i) { d.uh[2 * i] = t.uh[i]; d.uh[2 * i + 1] = s.uh[i]; } return d; }
    inline V pextuh(V s, V t) { V d; for (int i = 0; i < 4; ++i) { d.uh[2 * i] = t.uh[4 + i]; d.uh[2 * i + 1] = s.uh[4 + i]; } return d; }
    inline V pextlb(V s, V t) { V d; for (int i = 0; i < 8; ++i) { d.ub[2 * i] = t.ub[i]; d.ub[2 * i + 1] = s.ub[i]; } return d; }
    inline V pextub(V s, V t) { V d; for (int i = 0; i < 8; ++i) { d.ub[2 * i] = t.ub[8 + i]; d.ub[2 * i + 1] = s.ub[8 + i]; } return d; }
    // ppacw: the even words of t then of s: d = {t.w0, t.w2, s.w0, s.w2}.
    inline V ppacw(V s, V t) { V d; d.uw[0] = t.uw[0]; d.uw[1] = t.uw[2]; d.uw[2] = s.uw[0]; d.uw[3] = s.uw[2]; return d; }
    inline V ppach(V s, V t) { V d; for (int i = 0; i < 4; ++i) { d.uh[i] = t.uh[2 * i]; d.uh[4 + i] = s.uh[2 * i]; } return d; }
    inline V ppacb(V s, V t) { V d; for (int i = 0; i < 8; ++i) { d.ub[i] = t.ub[2 * i]; d.ub[8 + i] = s.ub[2 * i]; } return d; }
    // pext5 / ppac5: 1-5-5-5 colour <-> 8-8-8-8 (each 5-bit field to the top of its byte; alpha bit to bit 31).
    inline V pext5(V t)
    {
        V d;
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t v = t.uw[i];
            d.uw[i] = ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19) | (((v >> 15) & 1u) << 31);
        }
        return d;
    }
    inline V ppac5(V t)
    {
        V d;
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t v = t.uw[i];
            d.uw[i] = ((v >> 3) & 0x1Fu) | (((v >> 11) & 0x1Fu) << 5) | (((v >> 19) & 0x1Fu) << 10) | (((v >> 31) & 1u) << 15);
        }
        return d;
    }
    // pmulth: the odd lanes' products go to HI only; rd and LO hold the even lanes'.
    inline V pmulth(V s, V t, uint64_t &lo, uint64_t &lo1, uint64_t &hi, uint64_t &hi1)
    {
        V even, odd;
        for (int i = 0; i < 4; ++i)
        {
            even.sw[i] = static_cast<int32_t>(s.sh[2 * i]) * t.sh[2 * i];
            odd.sw[i] = static_cast<int32_t>(s.sh[2 * i + 1]) * t.sh[2 * i + 1];
        }
        lo = even.ud[0];
        lo1 = even.ud[1];
        hi = odd.ud[0];
        hi1 = odd.ud[1];
        return even;
    }
    inline V pcpyld(V s, V t) { V d; d.ud[0] = t.ud[0]; d.ud[1] = s.ud[0]; return d; }
    inline V pcpyud(V s, V t) { V d; d.ud[0] = s.ud[1]; d.ud[1] = t.ud[1]; return d; }

    // PMFHL: HI and LO (HI1:HI, LO1:LO as 128 bits) packed into rd by the format in sa: 0 LW, 1 UW, 2 SLW, 3 LH, 4 SH.
    inline V pmfhl(uint32_t fmt, V h, V l)
    {
        V d{};
        switch (fmt)
        {
        case 0:
            d.uw[0] = l.uw[0], d.uw[1] = h.uw[0], d.uw[2] = l.uw[2], d.uw[3] = h.uw[2];
            break;
        case 1:
            d.uw[0] = l.uw[1], d.uw[1] = h.uw[1], d.uw[2] = l.uw[3], d.uw[3] = h.uw[3];
            break;
        case 2:
            for (int k = 0; k < 2; ++k)
            {
                const int64_t v = static_cast<int64_t>((static_cast<uint64_t>(h.uw[2 * k]) << 32) | l.uw[2 * k]);
                d.sd[k] = satAdd<int64_t>(v, INT32_MIN, INT32_MAX);
            }
            break;
        case 3:
            d.uh[0] = l.uh[0], d.uh[1] = l.uh[2], d.uh[2] = h.uh[0], d.uh[3] = h.uh[2];
            d.uh[4] = l.uh[4], d.uh[5] = l.uh[6], d.uh[6] = h.uh[4], d.uh[7] = h.uh[6];
            break;
        case 4:
            for (int k = 0; k < 2; ++k)
            {
                d.sh[4 * k + 0] = satAdd<int16_t>(l.sw[2 * k], INT16_MIN, INT16_MAX);
                d.sh[4 * k + 1] = satAdd<int16_t>(l.sw[2 * k + 1], INT16_MIN, INT16_MAX);
                d.sh[4 * k + 2] = satAdd<int16_t>(h.sw[2 * k], INT16_MIN, INT16_MAX);
                d.sh[4 * k + 3] = satAdd<int16_t>(h.sw[2 * k + 1], INT16_MIN, INT16_MAX);
            }
            break;
        default:
            break;
        }
        return d;
    }
    inline V pcpyh(V t)
    {
        V d;
        for (int i = 0; i < 4; ++i) { d.uh[i] = t.uh[0]; d.uh[4 + i] = t.uh[4]; }
        return d;
    }
    inline V pinth(V s, V t) { V d; for (int i = 0; i < 4; ++i) { d.uh[2 * i] = t.uh[i]; d.uh[2 * i + 1] = s.uh[4 + i]; } return d; }
    inline V pinteh(V s, V t) { V d; for (int i = 0; i < 4; ++i) { d.uh[2 * i] = t.uh[2 * i]; d.uh[2 * i + 1] = s.uh[2 * i]; } return d; }
    // pexeh: swap halfwords 0 and 2 of each doubleword. prevh: reverse the 4 halfwords of each doubleword.
    inline V pexeh(V t)
    {
        V d = t;
        d.uh[0] = t.uh[2]; d.uh[2] = t.uh[0]; d.uh[4] = t.uh[6]; d.uh[6] = t.uh[4];
        return d;
    }
    inline V prevh(V t)
    {
        V d;
        for (int i = 0; i < 4; ++i) { d.uh[i] = t.uh[3 - i]; d.uh[4 + i] = t.uh[7 - i]; }
        return d;
    }
    inline V pexch(V t)
    {
        V d = t;
        d.uh[1] = t.uh[2]; d.uh[2] = t.uh[1]; d.uh[5] = t.uh[6]; d.uh[6] = t.uh[5];
        return d;
    }
    // pexew: swap words 0 and 2. pexcw: swap words 1 and 2. prot3w: rotate words 0..2 (d = {t.w1, t.w2, t.w0, t.w3}).
    inline V pexew(V t) { V d = t; d.uw[0] = t.uw[2]; d.uw[2] = t.uw[0]; return d; }
    inline V pexcw(V t) { V d = t; d.uw[1] = t.uw[2]; d.uw[2] = t.uw[1]; return d; }
    inline V prot3w(V t) { V d = t; d.uw[0] = t.uw[1]; d.uw[1] = t.uw[2]; d.uw[2] = t.uw[0]; return d; }

    // plzcw: per word of s, the number of leading bits equal to the sign bit, minus one.
    inline uint32_t leadingSignBits(uint32_t v)
    {
        const uint32_t x = (v & 0x80000000u) ? ~v : v;
        return (x == 0 ? 32u : static_cast<uint32_t>(__builtin_clz(x))) - 1u;
    }
    inline V plzcw(V s, V old)
    {
        V d = old;
        d.uw[0] = leadingSignBits(s.uw[0]);
        d.uw[1] = leadingSignBits(s.uw[1]);
        return d;
    }

    // qfsrv: the 256-bit value s:t (s high) shifted right by SA bits; the low 128 bits.
    inline V qfsrv(V s, V t, uint32_t sa)
    {
        uint8_t bytes[32];
        for (int i = 0; i < 16; ++i) { bytes[i] = t.ub[i]; bytes[16 + i] = s.ub[i]; }
        V d;
        const uint32_t byteShift = (sa >> 3) & 31u;
        const uint32_t bitShift = sa & 7u;
        for (int i = 0; i < 16; ++i)
        {
            const uint32_t lo = bytes[(i + byteShift) & 31u];
            const uint32_t hi = (i + byteShift + 1u) < 32u ? bytes[i + byteShift + 1u] : 0u;
            d.ub[i] = static_cast<uint8_t>(bitShift ? ((lo >> bitShift) | (hi << (8u - bitShift))) : lo);
        }
        // Bytes shifted in from above bit 255 are zero (never reached: SA is at most 255 and only byte/halfword
        // multiples are set by mtsab/mtsah).
        for (int i = 0; i < 16; ++i)
            if (i + byteShift >= 32u)
                d.ub[i] = 0;
        return d;
    }
}
