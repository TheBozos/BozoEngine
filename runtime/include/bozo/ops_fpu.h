// The PlayStation 2's single-precision float arithmetic, for the FPU (COP1) and VU0 macro mode (COP2).
// Spec: docs/specs/r5900.md, "FPU". Every value is handled as its 32 bits, never as a host float, because the
// format is not IEEE 754:
//   - exponent 255 is an ordinary exponent (no infinity, no NaN): the largest magnitude is 0x7FFFFFFF (about 3.4e38*2);
//   - exponent 0 is zero whatever the mantissa (denormals read as zero of the same sign, and are never produced);
//   - results are rounded toward zero; a result too large saturates to +/-0x7FFFFFFF (overflow flag), one too small
//     becomes zero of its sign (underflow flag);
//   - an addition aligns the smaller operand to the larger one's last mantissa bit, dropping the bits below it, before
//     adding (no guard bits) (open question Q1).
// Everything here is plain integer arithmetic, so results do not depend on the host's float unit or its mode.
#pragma once

#include <cmath>
#include <cstdint>

namespace bozo::ps2f
{
    // FCR31 bits.
    constexpr uint32_t kFlagC = 1u << 23;   // the compare result (bc1t / bc1f)
    constexpr uint32_t kFlagI = 1u << 17;   // invalid (0/0, sqrt of a negative)
    constexpr uint32_t kFlagD = 1u << 16;   // division by zero
    constexpr uint32_t kFlagO = 1u << 15;   // overflow
    constexpr uint32_t kFlagU = 1u << 14;   // underflow
    constexpr uint32_t kFlagSI = 1u << 6;   // sticky versions
    constexpr uint32_t kFlagSD = 1u << 5;
    constexpr uint32_t kFlagSO = 1u << 4;
    constexpr uint32_t kFlagSU = 1u << 3;

    constexpr uint32_t kSign = 0x80000000u;
    constexpr uint32_t kMax = 0x7FFFFFFFu; // the largest magnitude

    inline uint32_t expOf(uint32_t x) { return (x >> 23) & 0xFFu; }
    inline uint32_t mantOf(uint32_t x) { return (x & 0x7FFFFFu) | 0x800000u; } // with the leading 1
    inline bool isZero(uint32_t x) { return expOf(x) == 0; }                    // zero or denormal

    // Pack a sign, a biased exponent (may be out of range) and a 24-bit mantissa with its leading 1, with saturation.
    // `flags` collects the O/U flags (the caller ORs them into FCR31 or drops them).
    inline uint32_t pack(uint32_t sign, int32_t exp, uint32_t mant24, uint32_t &flags)
    {
        if (exp > 255)
        {
            flags |= kFlagO | kFlagSO;
            return sign | kMax;
        }
        if (exp <= 0)
        {
            flags |= kFlagU | kFlagSU;
            return sign;
        }
        return sign | (static_cast<uint32_t>(exp) << 23) | (mant24 & 0x7FFFFFu);
    }

    // a + b.
    inline uint32_t add(uint32_t a, uint32_t b, uint32_t &flags)
    {
        uint32_t ea = expOf(a), eb = expOf(b);
        if (ea == 0 || eb == 0)
        {
            if (ea != 0)
                return a;
            if (eb != 0)
                return b;
            // Both zero: -0 only when both are -0 (r5900.md, open question Q2).
            return (a & b & kSign);
        }
        // Order by magnitude: a is the larger.
        if ((a & kMax) < (b & kMax))
        {
            const uint32_t t = a;
            a = b;
            b = t;
            ea = expOf(a);
            eb = expOf(b);
        }
        const uint32_t d = ea - eb;
        const uint32_t ma = mantOf(a);
        const uint32_t mb = d >= 24 ? 0u : (mantOf(b) >> d); // bits below a's last mantissa bit dropped
        const uint32_t sign = a & kSign;
        int32_t e = static_cast<int32_t>(ea);
        uint32_t m;
        if (((a ^ b) & kSign) == 0)
        {
            m = ma + mb;
            if (m & 0x1000000u)
            {
                m >>= 1; // round toward zero: the bit shifted out is dropped
                ++e;
            }
        }
        else
        {
            m = ma - mb; // |a| >= |b| so no borrow
            if (m == 0)
                return 0; // exact cancellation: +0
            const int lead = __builtin_clz(m) - 8; // shifts to bring the leading 1 back to bit 23
            m <<= lead;
            e -= lead;
        }
        return pack(sign, e, m, flags);
    }

    inline uint32_t sub(uint32_t a, uint32_t b, uint32_t &flags) { return add(a, b ^ kSign, flags); }

    // a * b, the exact product truncated.
    inline uint32_t mul(uint32_t a, uint32_t b, uint32_t &flags)
    {
        const uint32_t sign = (a ^ b) & kSign;
        if (isZero(a) || isZero(b))
            return sign;
        uint64_t m = static_cast<uint64_t>(mantOf(a)) * mantOf(b); // 2^46 <= m < 2^48
        int32_t e = static_cast<int32_t>(expOf(a)) + static_cast<int32_t>(expOf(b)) - 127;
        if (m >> 47)
        {
            m >>= 24;
            ++e;
        }
        else
            m >>= 23;
        return pack(sign, e, static_cast<uint32_t>(m), flags);
    }

    // a / b. Division by zero: D flag (I flag for 0/0), result +/-max with the sign of a xor b (Q3).
    inline uint32_t div(uint32_t a, uint32_t b, uint32_t &flags)
    {
        const uint32_t sign = (a ^ b) & kSign;
        if (isZero(b))
        {
            flags |= isZero(a) ? (kFlagI | kFlagSI) : (kFlagD | kFlagSD);
            return sign | kMax;
        }
        if (isZero(a))
            return sign;
        const uint64_t q = (static_cast<uint64_t>(mantOf(a)) << 24) / mantOf(b); // 2^23 <= q < 2^25, truncated
        int32_t e = static_cast<int32_t>(expOf(a)) - static_cast<int32_t>(expOf(b)) + 127;
        uint32_t m;
        if (q >> 24)
            m = static_cast<uint32_t>(q >> 1);
        else
        {
            m = static_cast<uint32_t>(q);
            --e;
        }
        return pack(sign, e, m, flags);
    }

    // The square root of |x| truncated; a negative x sets the I flag (Q3). sqrt(-0) is +0.
    inline uint32_t sqrt(uint32_t x, uint32_t &flags)
    {
        if (isZero(x))
            return 0;
        if (x & kSign)
            flags |= kFlagI | kFlagSI;
        int32_t e = static_cast<int32_t>(expOf(x)) - 127; // unbiased
        uint64_t m = mantOf(x);
        if (e & 1)
        {
            m <<= 1;
            --e;
        }
        const uint64_t n = m << 23; // 2^46 <= n < 2^48; sqrt(n) has 24 bits
        uint64_t s = static_cast<uint64_t>(std::sqrt(static_cast<double>(n)));
        while (s * s > n)
            --s;
        while ((s + 1) * (s + 1) <= n)
            ++s;
        uint32_t ignored = 0;
        return pack(0, e / 2 + 127, static_cast<uint32_t>(s), ignored);
    }

    // a / sqrt(b): the root truncated, then the quotient truncated (Q3). b = 0: D flag, +/-max with a's sign.
    inline uint32_t rsqrt(uint32_t a, uint32_t b, uint32_t &flags)
    {
        if (isZero(b))
        {
            flags |= kFlagD | kFlagSD;
            return (a & kSign) | kMax;
        }
        if (b & kSign)
            flags |= kFlagI | kFlagSI;
        uint32_t ignored = 0;
        const uint32_t root = sqrt(b & kMax, ignored);
        return div(a, root, flags);
    }

    // Compare as values: zeros (and denormals) are all equal; otherwise sign-magnitude order.
    inline int32_t orderKey(uint32_t x)
    {
        if (isZero(x))
            return 0;
        const int32_t mag = static_cast<int32_t>(x & kMax);
        return (x & kSign) ? -mag : mag;
    }
    inline bool eq(uint32_t a, uint32_t b) { return orderKey(a) == orderKey(b); }
    inline bool lt(uint32_t a, uint32_t b) { return orderKey(a) < orderKey(b); }
    inline bool le(uint32_t a, uint32_t b) { return orderKey(a) <= orderKey(b); }
    inline uint32_t max(uint32_t a, uint32_t b) { return orderKey(a) >= orderKey(b) ? a : b; }
    inline uint32_t min(uint32_t a, uint32_t b) { return orderKey(a) <= orderKey(b) ? a : b; }

    // cvt.w.s: truncate toward zero; out of range saturates to 0x7FFFFFFF / 0x80000000.
    inline uint32_t toInt(uint32_t x)
    {
        if (isZero(x))
            return 0;
        const int32_t e = static_cast<int32_t>(expOf(x)) - 127;
        if (e < 0)
            return 0;
        if (e >= 31)
            return (x & kSign) ? 0x80000000u : 0x7FFFFFFFu;
        const uint32_t m = mantOf(x);
        const uint32_t v = e >= 23 ? (m << (e - 23)) : (m >> (23 - e));
        return (x & kSign) ? (0u - v) : v;
    }

    // cvt.s.w: an integer to float, truncated toward zero when it needs more than 24 bits.
    inline uint32_t fromInt(uint32_t i)
    {
        if (i == 0)
            return 0;
        const uint32_t sign = i & kSign;
        const uint32_t mag = sign ? (0u - i) : i; // 0x80000000 stays 0x80000000 = 2^31
        const int top = 31 - __builtin_clz(mag);  // position of the leading 1
        const uint32_t m = top >= 23 ? (mag >> (top - 23)) : (mag << (23 - top));
        uint32_t ignored = 0;
        return pack(sign, top + 127, m, ignored);
    }

    // VU0's fixed-point conversions (VITOFn / VFTOIn): n fraction bits.
    inline uint32_t fromFixed(uint32_t i, int fractionBits)
    {
        const uint32_t f = fromInt(i);
        if (isZero(f) || fractionBits == 0)
            return f;
        uint32_t ignored = 0;
        return pack(f & kSign, static_cast<int32_t>(expOf(f)) - fractionBits, mantOf(f), ignored);
    }
    inline uint32_t toFixed(uint32_t x, int fractionBits)
    {
        if (isZero(x) || fractionBits == 0)
            return toInt(x);
        const int32_t e = static_cast<int32_t>(expOf(x)) + fractionBits;
        if (e > 255)
            return (x & kSign) ? 0x80000000u : 0x7FFFFFFFu;
        return toInt((x & ~(0xFFu << 23)) | (static_cast<uint32_t>(e) << 23));
    }
}
