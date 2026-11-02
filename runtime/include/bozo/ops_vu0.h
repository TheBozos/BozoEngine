// VU0 macro mode (COP2) for recompiled code. Spec: docs/specs/r5900.md, "COP2".
//
// The upper (FMAC) instructions work field by field on x, y, z, w (index 0..3), each enabled by a bit of the
// instruction's dest mask (x = 8, y = 4, z = 2, w = 1), with the PS2 float arithmetic of ops_fpu.h. VU0's MAC,
// status and clipping flags are not computed by macro instructions here (open question Q11: TMB never reads them).
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

    // d = s op t on the enabled fields. Sources are copies, so d may be s or t.
    template <typename F>
    inline void apply(Vec &d, Vec s, Vec t, uint32_t dest, F f)
    {
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = f(s.u[i], t.u[i]);
    }

    inline uint32_t addf(uint32_t a, uint32_t b) { uint32_t fl = 0; return ps2f::add(a, b, fl); }
    inline uint32_t subf(uint32_t a, uint32_t b) { uint32_t fl = 0; return ps2f::sub(a, b, fl); }
    inline uint32_t mulf(uint32_t a, uint32_t b) { uint32_t fl = 0; return ps2f::mul(a, b, fl); }

    inline void add(Vec &d, Vec s, Vec t, uint32_t dest) { apply(d, s, t, dest, addf); }
    inline void sub(Vec &d, Vec s, Vec t, uint32_t dest) { apply(d, s, t, dest, subf); }
    inline void mul(Vec &d, Vec s, Vec t, uint32_t dest) { apply(d, s, t, dest, mulf); }
    inline void max(Vec &d, Vec s, Vec t, uint32_t dest) { apply(d, s, t, dest, ps2f::max); }
    inline void mini(Vec &d, Vec s, Vec t, uint32_t dest) { apply(d, s, t, dest, ps2f::min); }
    // madd: d = acc + s * t (the product truncated, then the sum truncated). msub: d = acc - s * t.
    inline void madd(Vec &d, Vec acc, Vec s, Vec t, uint32_t dest)
    {
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = addf(acc.u[i], mulf(s.u[i], t.u[i]));
    }
    inline void msub(Vec &d, Vec acc, Vec s, Vec t, uint32_t dest)
    {
        for (int i = 0; i < 4; ++i)
            if (fieldOn(dest, i))
                d.u[i] = subf(acc.u[i], mulf(s.u[i], t.u[i]));
    }
    // The outer product pair: opmula acc.xyz = (s.y*t.z, s.z*t.x, s.x*t.y); opmsub d.xyz = acc.xyz - (s.y*t.z, ...).
    // The instruction's dest is xyz by definition (w untouched).
    inline void opmula(Vec &acc, Vec s, Vec t)
    {
        acc.u[0] = mulf(s.u[1], t.u[2]);
        acc.u[1] = mulf(s.u[2], t.u[0]);
        acc.u[2] = mulf(s.u[0], t.u[1]);
    }
    inline void opmsub(Vec &d, Vec acc, Vec s, Vec t)
    {
        d.u[0] = subf(acc.u[0], mulf(s.u[1], t.u[2]));
        d.u[1] = subf(acc.u[1], mulf(s.u[2], t.u[0]));
        d.u[2] = subf(acc.u[2], mulf(s.u[0], t.u[1]));
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
    inline uint32_t div(uint32_t a, uint32_t b) { uint32_t fl = 0; return ps2f::div(a, b, fl); }
    inline uint32_t sqrt(uint32_t b) { uint32_t fl = 0; return ps2f::sqrt(b, fl); }
    inline uint32_t rsqrt(uint32_t a, uint32_t b) { uint32_t fl = 0; return ps2f::rsqrt(a, b, fl); }

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
