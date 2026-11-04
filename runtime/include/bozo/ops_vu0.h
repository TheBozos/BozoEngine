// VU0 macro mode (COP2) for recompiled code. Spec: docs/specs/r5900.md, "COP2".
//
// The upper (FMAC) instructions work field by field on x, y, z, w (index 0..3), each enabled by a bit of the
// instruction's dest mask (x = 8, y = 4, z = 2, w = 1), with the PS2 float arithmetic of ops_fpu.h, and set the MAC
// and status flags (Q11).
// vf00 is constant (0, 0, 0, 1): writes to it are dropped by the emitter.
#pragma once

#include "bozo/context.h"
#include "bozo/ops_fpu.h"
#include "bozo/runtime_api.h"

#include <cstring>

namespace bozo::vu0
{
    using Vec = Vu0Vector;

    inline bool fieldOn(uint32_t dest, int i) { return (dest >> (3 - i)) & 1u; }

    // The second operand of an FMAC instruction: a vector, one field broadcast, Q or I.
    inline Vec broadcast(uint32_t bits)
    {
        Vec v;
        v.u[0] = v.u[1] = v.u[2] = v.u[3] = bits;
        return v;
    }
    inline Vec bc(const Vec &t, uint32_t field) { return broadcast(t.u[field]); }

    // ---- Arithmetic of one field ----
    // VU0's adder rounds to nearest even (unlike the FPU's, ops_fpu.h); its multiplier is the FPU's.
    // A result below the smallest normal becomes zero without the U flag (a sum that cancels, a product of a tiny
    // factor): the reference never sets U (nor its sticky bit) in TMB's runs (match1p #41542199: vu0MulMatrix3x3's
    // first mul inside TMB's aiUpdateInputs override, its z product underflowing to 0, status 0x2c2 where U would make
    // it 0x3c2).
    inline uint32_t addf(uint32_t a, uint32_t b, uint32_t &fl)
    {
        uint32_t f = 0;
        const uint32_t r = ps2f::addNearest(a, b, f);
        fl |= f & ~(ps2f::kFlagU | ps2f::kFlagSU);
        return r;
    }
    inline uint32_t subf(uint32_t a, uint32_t b, uint32_t &fl)
    {
        uint32_t f = 0;
        const uint32_t r = ps2f::subNearest(a, b, f);
        fl |= f & ~(ps2f::kFlagU | ps2f::kFlagSU);
        return r;
    }
    inline uint32_t mulf(uint32_t a, uint32_t b, uint32_t &fl)
    {
        uint32_t f = 0;
        const uint32_t r = ps2f::mul(a, b, f);
        fl |= f & ~(ps2f::kFlagU | ps2f::kFlagSU);
        return r;
    }

    // ---- Flags (Q11) ----
    // The MAC flags of an FMAC instruction's result: for each field (x = bit 3 ... w = bit 0 of each nibble) Z (bits
    // 0-3: the result is +/-0), S (4-7: its sign bit), U (8-11: it underflowed to 0), O (12-15: it overflowed, or it
    // is the largest magnitude, 0x7fffffff); fields outside dest are 0. The status flag's Z, S, U, O (bits 0-3)
    // are the OR of the MAC's, and its sticky copies (bits 6-9) accumulate them; I, D (4, 5) and their sticky bits
    // (10, 11) are the divider's. All as the reference captures show them (r5900.md Q11).
    inline void setFlags(R5900Context *ctx, uint32_t dest, const uint32_t r[4], const uint32_t fl[4])
    {
        uint32_t mac = 0;
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
            {
                const uint32_t bit = 3u - static_cast<uint32_t>(i);
                if ((r[i] & 0x7FFFFFFFu) == 0)
                    mac |= 1u << bit;
                if (r[i] & ps2f::kSign)
                    mac |= 1u << (bit + 4);
                if (fl[i] & ps2f::kFlagU)
                    mac |= 1u << (bit + 8);
                // O: an overflow, or a result at the largest magnitude (the saturated value): 0xffffffff plus a small
                // number flags it (match1p capture #670, 0x1335c8: status 0x2c0; stage-level3 #15818358), 0xfffffffe
                // plus a small number or times 1.0 does not (stage-level3 #15807299, match4p #76722184:
                // mathfDotProduct in BuildRadarStuff, status 0xc2 / 0xc0). Other exponent-255 results are ordinary.
                if ((fl[i] & ps2f::kFlagO) || (r[i] & ps2f::kMax) == ps2f::kMax)
                    mac |= 1u << (bit + 12);
            }
        ctx->vu0_mac = mac;
        uint32_t st = ctx->vu0_status & ~0xFu;
        if (mac & 0x000Fu)
            st |= 1u;
        if (mac & 0x00F0u)
            st |= 2u;
        if (mac & 0x0F00u)
            st |= 4u;
        if (mac & 0xF000u)
            st |= 8u;
        ctx->vu0_status = st | ((st & 0xFu) << 6);
    }

    // d = s op t on the enabled fields, with the flags. Sources are copies, so d may be s or t.
    template <typename F>
    inline void fmac(R5900Context *ctx, Vec &d, Vec s, Vec t, uint32_t dest, F f)
    {
        uint32_t r[4] = {}, fl[4] = {};
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                r[i] = f(s.u[i], t.u[i], fl[i]);
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = r[i];
        setFlags(ctx, dest, r, fl);
    }
    template <typename F>
    inline void apply(Vec &d, Vec s, Vec t, uint32_t dest, F f)
    {
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = f(s.u[i], t.u[i]);
    }

    inline void add(R5900Context *ctx, Vec &d, Vec s, Vec t, uint32_t dest) { fmac(ctx, d, s, t, dest, addf); }
    inline void sub(R5900Context *ctx, Vec &d, Vec s, Vec t, uint32_t dest) { fmac(ctx, d, s, t, dest, subf); }
    inline void mul(R5900Context *ctx, Vec &d, Vec s, Vec t, uint32_t dest) { fmac(ctx, d, s, t, dest, mulf); }
    // max / mini do not change the flags.
    inline void max(Vec &d, Vec s, Vec t, uint32_t dest) { apply(d, s, t, dest, ps2f::max); }
    inline void mini(Vec &d, Vec s, Vec t, uint32_t dest) { apply(d, s, t, dest, ps2f::min); }
    // madd: d = acc + s * t (the product rounded first, then the sum: Q6). msub: d = acc - s * t.
    inline void madd(R5900Context *ctx, Vec &d, Vec acc, Vec s, Vec t, uint32_t dest)
    {
        uint32_t r[4] = {}, fl[4] = {};
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
            {
                uint32_t pf = 0; // the flags are the sum's (whether the product's count is unchecked)
                r[i] = addf(acc.u[i], mulf(s.u[i], t.u[i], pf), fl[i]);
            }
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = r[i];
        setFlags(ctx, dest, r, fl);
    }
    inline void msub(R5900Context *ctx, Vec &d, Vec acc, Vec s, Vec t, uint32_t dest)
    {
        uint32_t r[4] = {}, fl[4] = {};
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
            {
                uint32_t pf = 0;
                r[i] = subf(acc.u[i], mulf(s.u[i], t.u[i], pf), fl[i]);
            }
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = r[i];
        setFlags(ctx, dest, r, fl);
    }
    // The outer product pair: opmula acc.xyz = (s.y*t.z, s.z*t.x, s.x*t.y); opmsub d.xyz = acc.xyz - (s.y*t.z, ...).
    // The instruction's dest is xyz by definition (w untouched).
    inline void opmula(R5900Context *ctx, Vec &acc, Vec s, Vec t)
    {
        uint32_t r[4] = {}, fl[4] = {};
        r[0] = mulf(s.u[1], t.u[2], fl[0]);
        r[1] = mulf(s.u[2], t.u[0], fl[1]);
        r[2] = mulf(s.u[0], t.u[1], fl[2]);
        acc.u[0] = r[0];
        acc.u[1] = r[1];
        acc.u[2] = r[2];
        setFlags(ctx, 0xEu, r, fl);
    }
    inline void opmsub(R5900Context *ctx, Vec &d, Vec acc, Vec s, Vec t)
    {
        uint32_t r[4] = {}, fl[4] = {}, pf = 0;
        r[0] = subf(acc.u[0], mulf(s.u[1], t.u[2], pf), fl[0]);
        r[1] = subf(acc.u[1], mulf(s.u[2], t.u[0], pf), fl[1]);
        r[2] = subf(acc.u[2], mulf(s.u[0], t.u[1], pf), fl[2]);
        d.u[0] = r[0];
        d.u[1] = r[1];
        d.u[2] = r[2];
        setFlags(ctx, 0xEu, r, fl);
    }
    inline void abs(Vec &d, Vec s, uint32_t dest)
    {
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = s.u[i] & 0x7FFFFFFFu;
    }
    inline void move(Vec &d, Vec s, uint32_t dest)
    {
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = s.u[i];
    }
    // mr32: rotate the fields: d.x = s.y, d.y = s.z, d.z = s.w, d.w = s.x.
    inline void mr32(Vec &d, Vec s, uint32_t dest)
    {
        const uint32_t r[4] = {s.u[1], s.u[2], s.u[3], s.u[0]};
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = r[i];
    }
    inline void itof(Vec &d, Vec s, uint32_t dest, int fractionBits)
    {
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = ps2f::fromFixed(s.u[i], fractionBits);
    }
    inline void ftoi(Vec &d, Vec s, uint32_t dest, int fractionBits)
    {
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = ps2f::toFixed(s.u[i], fractionBits);
    }

    // Q = s.fsf / t.ftf (vdiv), sqrt(t.ftf) (vsqrt), s.fsf / sqrt(t.ftf) (vrsqrt). Q is available at once in this
    // translation (macro mode interlocks on Q: open question Q12).
    // The divider sets the status flag's I (bit 4: 0/0, a root of a negative) and D (bit 5: division by zero), and
    // their sticky copies (10, 11).
    inline void divFlags(R5900Context *ctx, uint32_t fl)
    {
        uint32_t id = 0;
        if (fl & ps2f::kFlagI)
            id |= 0x10u;
        if (fl & ps2f::kFlagD)
            id |= 0x20u;
        ctx->vu0_status = (ctx->vu0_status & ~0x30u) | id | (id << 6);
    }
    inline uint32_t div(R5900Context *ctx, uint32_t a, uint32_t b)
    {
        uint32_t fl = 0;
        const uint32_t q = ps2f::div(a, b, fl);
        divFlags(ctx, fl);
        return q;
    }
    inline uint32_t sqrt(R5900Context *ctx, uint32_t b)
    {
        uint32_t fl = 0;
        const uint32_t q = ps2f::sqrt(b, fl);
        divFlags(ctx, fl);
        return q;
    }
    inline uint32_t rsqrt(R5900Context *ctx, uint32_t a, uint32_t b)
    {
        uint32_t fl = 0;
        const uint32_t q = ps2f::rsqrt(a, b, fl);
        divFlags(ctx, fl);
        return q;
    }

    // VU0 data memory: 4 KB, addressed in quadwords by a VI register; the address wraps at 4 KB (Q13).
    inline uint8_t *quad(Runtime *rt, uint32_t index) { return bozo_vu0_memory(rt) + ((index & 0xFFu) << 4); }
    inline void lq(Runtime *rt, Vec &d, uint32_t index, uint32_t dest)
    {
        Vec m;
        std::memcpy(&m, quad(rt, index), 16);
        move(d, m, dest);
    }
    inline void sq(Runtime *rt, const Vec &s, uint32_t index, uint32_t dest)
    {
        uint8_t *p = quad(rt, index);
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                std::memcpy(p + 4 * i, &s.u[i], 4);
    }

    // cfc2 / ctc2 on control registers 16..31 (0..15 are vi[], handled inline).
    inline uint32_t readControl(R5900Context *ctx, Runtime *rt, uint32_t reg)
    {
        switch (reg)
        {
        case 16: return ctx->vu0_status;
        case 17: return ctx->vu0_mac;
        case 18: return ctx->vu0_clip_flags;
        case 20: return ctx->vu0_r;
        case 21: return ctx->vu0_i_bits;
        case 22: return ctx->vu0_q_bits;
        case 27: return ctx->vu0_cmsar0;
        default: return bozo_vu0_control_read(ctx, rt, reg);
        }
    }
    inline void writeControl(R5900Context *ctx, Runtime *rt, uint32_t reg, uint32_t v)
    {
        switch (reg)
        {
        case 16: ctx->vu0_status = v; break; // (only the sticky flags are writable on the console: Q11)
        case 17: break;                      // MAC flags: read-only
        case 18: ctx->vu0_clip_flags = v; break;
        case 20: ctx->vu0_r = v & 0x7FFFFFu; break;
        case 21: ctx->vu0_i_bits = v; break;
        case 22: ctx->vu0_q_bits = v; break;
        case 27: ctx->vu0_cmsar0 = v & 0xFFFFu; break;
        default: bozo_vu0_control_write(ctx, rt, reg, v); break;
        }
    }
}
