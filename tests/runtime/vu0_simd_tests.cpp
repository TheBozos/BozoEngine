// bozo-vu0-simd-tests: VU0's four-field fast path (ops_vu0.h, simd::fmac) against the field-by-field path, bit for bit,
// flags included, over random operands that mix ordinary numbers with zeros, denormals, exponent-255 numbers and
// exponents far apart or near the ends (products and sums that leave the normal range).
#include "bozo/ops_vu0.h"

#include <cstdio>
#include <random>

namespace
{
    namespace v = bozo::vu0;
    using Vec = Vu0Vector;
    int g_failures = 0;

    // The field-by-field path (ops_vu0.h before the fast path).
    template <typename F>
    void exactFmac(R5900Context *ctx, Vec &d, Vec s, Vec t, uint32_t dest, F f)
    {
        v::fmac(ctx, d, s, t, dest, f);
    }
    void exactMadd(R5900Context *ctx, Vec &d, Vec acc, Vec s, Vec t, uint32_t dest, bool sub)
    {
        uint32_t r[4] = {}, fl[4] = {};
        for (int i = 0; i < 4; ++i)
            if (v::fieldOn(dest, i))
            {
                uint32_t pf = 0;
                const uint32_t p = v::mulf(s.u[i], t.u[i], pf);
                r[i] = sub ? v::subf(acc.u[i], p, fl[i]) : v::addf(acc.u[i], p, fl[i]);
            }
        for (int i = 0; i < 4; ++i)
            if (v::fieldOn(dest, i))
                d.u[i] = r[i];
        v::setFlags(ctx, dest, r, fl);
    }

    uint32_t operand(std::mt19937 &rng)
    {
        const uint32_t sign = (rng() & 1u) << 31, mant = rng() & 0x7FFFFFu;
        switch (rng() % 16u)
        {
        case 0:
            return sign; // zero
        case 1:
            return sign | mant; // denormal
        case 2:
            return sign | 0x7F800000u | mant; // exponent 255
        case 3:
            return sign | (1u + rng() % 6u) << 23 | mant; // near the bottom
        case 4:
            return sign | (249u + rng() % 6u) << 23 | mant; // near the top
        case 5:
            return sign | 127u << 23; // +/-1
        default:
            return sign | (100u + rng() % 56u) << 23 | mant; // ordinary
        }
    }
    Vec vec(std::mt19937 &rng)
    {
        Vec x;
        for (auto &u : x.u)
            u = operand(rng);
        return x;
    }
    void compare(const char *what, int n, const Vec &got, const Vec &want, const R5900Context &a, const R5900Context &b)
    {
        bool same = a.vu0_mac == b.vu0_mac && a.vu0_status == b.vu0_status;
        for (int i = 0; i < 4; ++i)
            same &= got.u[i] == want.u[i];
        if (same)
            return;
        if (++g_failures <= 10)
            std::printf("FAIL %s case %d: %08x %08x %08x %08x mac %x st %x, expected %08x %08x %08x %08x mac %x st %x\n", what, n,
                        got.u[0], got.u[1], got.u[2], got.u[3], a.vu0_mac, a.vu0_status, want.u[0], want.u[1], want.u[2],
                        want.u[3], b.vu0_mac, b.vu0_status);
    }
}

int main()
{
    std::mt19937 rng(20261121u);
    uint64_t fast = 0;
    for (int n = 0; n < 2000000; ++n)
    {
        const Vec s = vec(rng), acc = vec(rng), d0 = vec(rng);
        Vec t = vec(rng);
        if (rng() % 8u == 0) // cancellation: t = -s on some fields
            for (auto &u : t.u)
                if (rng() & 1u)
                    u = s.u[&u - t.u] ^ 0x80000000u;
        const uint32_t dest = rng() % 16u;
        const uint32_t status = rng() & 0xFC0u; // sticky bits and the divider's, kept or not
        auto run = [&](const char *what, auto fastOp, auto exactOp)
        {
            R5900Context a{}, b{};
            a.vu0_status = b.vu0_status = status;
            a.vu0_mac = b.vu0_mac = 0x1234u;
            Vec got = d0, want = d0;
            fastOp(&a, got);
            exactOp(&b, want);
            compare(what, n, got, want, a, b);
        };
        run("add", [&](R5900Context *c, Vec &d) { v::add(c, d, s, t, dest); },
            [&](R5900Context *c, Vec &d) { exactFmac(c, d, s, t, dest, v::addf); });
        run("sub", [&](R5900Context *c, Vec &d) { v::sub(c, d, s, t, dest); },
            [&](R5900Context *c, Vec &d) { exactFmac(c, d, s, t, dest, v::subf); });
        run("mul", [&](R5900Context *c, Vec &d) { v::mul(c, d, s, t, dest); },
            [&](R5900Context *c, Vec &d) { exactFmac(c, d, s, t, dest, v::mulf); });
        run("madd", [&](R5900Context *c, Vec &d) { v::madd(c, d, acc, s, t, dest); },
            [&](R5900Context *c, Vec &d) { exactMadd(c, d, acc, s, t, dest, false); });
        run("msub", [&](R5900Context *c, Vec &d) { v::msub(c, d, acc, s, t, dest); },
            [&](R5900Context *c, Vec &d) { exactMadd(c, d, acc, s, t, dest, true); });
#ifdef BOZO_VU0_SIMD
        R5900Context c{};
        Vec x = d0;
        fast += v::simd::fmac<v::simd::Op::Madd>(&c, x, acc, s, t, dest) ? 1u : 0u;
#endif
    }
#ifdef BOZO_VU0_SIMD
    std::printf("vu0 simd: %llu of 2000000 madd cases on the fast path\n", static_cast<unsigned long long>(fast));
#else
    std::printf("vu0 simd: not built (no SSE2 or BOZO_PS2F_EXACT_ONLY): the exact path checked against itself\n");
#endif
    if (g_failures)
        std::printf("%d failures\n", g_failures);
    return g_failures ? 1 : 0;
}
