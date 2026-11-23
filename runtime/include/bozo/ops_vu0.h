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

#if !defined(BOZO_PS2F_EXACT_ONLY) && (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64))
#include <emmintrin.h>
#endif

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

    // ---- Four fields at once (x86-64) ----
    // The common cases of an FMAC instruction, all four fields through SSE together; the field-by-field path above gives
    // the same bits and flags for them:
    // - a product of two normal numbers that is normal above the smallest normals (exponent 2-254), or with a zero operand (exponent 0: zero or denormal): the
    //   signed zero, no flag (ps2f::mul);
    // - a sum of two normal numbers that is normal, or with a zero operand: the other operand, or for two zeros -0 only
    //   when both are -0 (ps2f::addZeros); a - b is a + (-b);
    // the host's arithmetic being ps2f's own shortcut for normal numbers (ops_fpu.h: IEEE's rounding to nearest even).
    // The results are then normal or +/-0, so the MAC flags are Z and S only. Any enabled field outside these cases
    // (an exponent-255 operand, a result that leaves the normal range, a sum that cancels to 0) sends the instruction to
    // the field-by-field path. Tried first by add, sub, mul, madd and msub: they were a quarter of TTA's game thread in
    // a crowded battle (game functions 0x1d5118 / 0x1d52c8, matrix code). BOZO_PS2F_EXACT_ONLY turns it off with
    // ps2f's shortcut; tests/runtime/vu0_simd_tests.cpp checks it against the field-by-field path.
#if !defined(BOZO_PS2F_EXACT_ONLY) && (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64))
#define BOZO_VU0_SIMD 1
    namespace simd
    {
        enum class Op
        {
            Add,
            Sub,
            Mul,
            Madd,
            Msub
        };
        inline __m128i load(const Vec &v) { return _mm_loadu_si128(reinterpret_cast<const __m128i *>(v.u)); }
        inline __m128i exponent(__m128i x) { return _mm_and_si128(_mm_srli_epi32(x, 23), _mm_set1_epi32(0xFF)); }
        inline __m128i zero(__m128i x) { return _mm_cmpeq_epi32(exponent(x), _mm_setzero_si128()); } // or denormal
        inline __m128i top(__m128i x) { return _mm_cmpeq_epi32(exponent(x), _mm_set1_epi32(0xFF)); }
        inline __m128i abnormal(__m128i x) { return _mm_or_si128(zero(x), top(x)); }
        // Exponent 1, the smallest normals: a product just below 2^-126 rounds up to them on the host, where the PS2
        // flushes it to zero with the underflow flags (ps2f::mul's shortcut leaves them to the exact path too).
        inline __m128i bottom(__m128i x) { return _mm_cmpeq_epi32(exponent(x), _mm_set1_epi32(1)); }
        inline __m128i select(__m128i mask, __m128i a, __m128i b) { return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b)); }
        inline __m128i signBits() { return _mm_set1_epi32(static_cast<int32_t>(0x80000000u)); }
        // Lane i on when dest enables field i (x = dest bit 3 = lane 0).
        inline __m128i lanes(uint32_t dest)
        {
            return _mm_set_epi32(-static_cast<int32_t>(dest & 1u), -static_cast<int32_t>((dest >> 1) & 1u),
                                 -static_cast<int32_t>((dest >> 2) & 1u), -static_cast<int32_t>((dest >> 3) & 1u));
        }
        // s * t; `bad` collects the lanes outside the cases above.
        inline __m128i mul(__m128i s, __m128i t, __m128i &bad)
        {
            const __m128i z = _mm_or_si128(zero(s), zero(t));
            const __m128i p = _mm_castps_si128(_mm_mul_ps(_mm_castsi128_ps(s), _mm_castsi128_ps(t)));
            bad = _mm_or_si128(bad, _mm_andnot_si128(z, _mm_or_si128(_mm_or_si128(top(s), top(t)),
                                                                     _mm_or_si128(abnormal(p), bottom(p)))));
            return select(z, _mm_and_si128(_mm_xor_si128(s, t), signBits()), p);
        }
        // a + b, likewise.
        inline __m128i add(__m128i a, __m128i b, __m128i &bad)
        {
            const __m128i za = zero(a), zb = zero(b);
            const __m128i sum = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b)));
            const __m128i either = _mm_or_si128(za, zb);
            bad = _mm_or_si128(bad, _mm_andnot_si128(either, _mm_or_si128(_mm_or_si128(top(a), top(b)), abnormal(sum))));
            const __m128i zeros = select(za, select(zb, _mm_and_si128(_mm_and_si128(a, b), signBits()), b), a);
            const __m128i r = select(either, zeros, sum);
            bad = _mm_or_si128(bad, top(r)); // the other operand of a zero, at exponent 255
            return r;
        }
        // d (op) on the enabled fields with the flags, when every enabled field is a case above; false (nothing
        // written) otherwise.
        template <Op op>
        inline bool fmac(R5900Context *ctx, Vec &d, const Vec &acc, const Vec &s, const Vec &t, uint32_t dest)
        {
            const __m128i S = load(s), T = load(t), on = lanes(dest);
            __m128i bad = _mm_setzero_si128(), R;
            if constexpr (op == Op::Add)
                R = add(S, T, bad);
            else if constexpr (op == Op::Sub)
                R = add(S, _mm_xor_si128(T, signBits()), bad);
            else if constexpr (op == Op::Mul)
                R = mul(S, T, bad);
            else
            {
                // The product rounded first, then the sum (Q6), as the field-by-field madd / msub.
                const __m128i P = mul(S, T, bad);
                R = add(load(acc), op == Op::Madd ? P : _mm_xor_si128(P, signBits()), bad);
            }
            if (_mm_movemask_epi8(_mm_and_si128(bad, on)) != 0)
                return false;
            _mm_storeu_si128(reinterpret_cast<__m128i *>(d.u), select(on, R, load(d)));
            // MAC: Z (bits 0-3, x = bit 3) and S (4-7) of the enabled fields; U and O are 0 here.
            static constexpr uint8_t kReverse[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
            const __m128i r = _mm_and_si128(on, R);
            const __m128i isZero = _mm_and_si128(on, _mm_cmpeq_epi32(_mm_andnot_si128(signBits(), r), _mm_setzero_si128()));
            const uint32_t z = kReverse[_mm_movemask_ps(_mm_castsi128_ps(isZero))];
            const uint32_t sg = kReverse[_mm_movemask_ps(_mm_castsi128_ps(r))];
            ctx->vu0_mac = z | sg << 4;
            uint32_t st = ctx->vu0_status & ~0xFu;
            st |= (z ? 1u : 0u) | (sg ? 2u : 0u);
            ctx->vu0_status = st | ((st & 0xFu) << 6);
            return true;
        }
    }
#endif

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

#ifdef BOZO_VU0_SIMD
#define BOZO_VU0_FAST(op, acc)                                                                                         \
    if (simd::fmac<simd::Op::op>(ctx, d, acc, s, t, dest))                                                             \
        return;
#else
#define BOZO_VU0_FAST(op, acc)
#endif
    inline void add(R5900Context *ctx, Vec &d, Vec s, Vec t, uint32_t dest)
    {
        BOZO_VU0_FAST(Add, s)
        fmac(ctx, d, s, t, dest, addf);
    }
    inline void sub(R5900Context *ctx, Vec &d, Vec s, Vec t, uint32_t dest)
    {
        BOZO_VU0_FAST(Sub, s)
        fmac(ctx, d, s, t, dest, subf);
    }
    inline void mul(R5900Context *ctx, Vec &d, Vec s, Vec t, uint32_t dest)
    {
        BOZO_VU0_FAST(Mul, s)
        fmac(ctx, d, s, t, dest, mulf);
    }
    // max / mini do not change the flags.
    inline void max(Vec &d, Vec s, Vec t, uint32_t dest) { apply(d, s, t, dest, ps2f::max); }
    inline void mini(Vec &d, Vec s, Vec t, uint32_t dest) { apply(d, s, t, dest, ps2f::min); }
    // madd: d = acc + s * t (the product rounded first, then the sum: Q6). msub: d = acc - s * t.
    inline void madd(R5900Context *ctx, Vec &d, Vec acc, Vec s, Vec t, uint32_t dest)
    {
        BOZO_VU0_FAST(Madd, acc)
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
        BOZO_VU0_FAST(Msub, acc)
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
#undef BOZO_VU0_FAST

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
