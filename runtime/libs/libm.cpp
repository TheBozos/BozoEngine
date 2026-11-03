// The math library calls TMB makes out of guest code (libraries.md section 3): sqrt, and the three float kernels
// the game's own sinf and cosf (guest code) call. Their results are the reference's bits, settled against the call
// logs:
//  - sqrt: the IEEE single square root of f12 (correctly rounded, every platform).
//  - __ieee754_rem_pio2f(x, y): n = x * (float)(2/pi) rounded to a float, then to the nearest integer (ties to
//    even); y[0] = (float)((double)x - n * (double)(float)(pi/2)), y[1] = 0; returns n. This is not the game's own
//    fdlibm reduction (pi/2 in three parts): -pi/2 gives y[0] = 0 exactly.
//  - __kernel_sinf(x, y, iy), __kernel_cosf(x, y): the sine and cosine of (float)(x + y) as the reference's C
//    runtime computes them (the Universal CRT's sinf and cosf on Windows): iy is not used. The game's own kernels
//    (fdlibm's float polynomials) give other bits on 17% of the reference's calls, a correctly rounded sine on 0.06%.
#include "bozo/libs/libraries.h"

#include <cmath>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace bozo::libs
{
    namespace
    {
        float asFloat(uint32_t b)
        {
            float f;
            std::memcpy(&f, &b, 4);
            return f;
        }
        uint32_t bits(float f)
        {
            uint32_t b;
            std::memcpy(&b, &f, 4);
            return b;
        }

        using FloatFn = float (*)(float);
        struct ReferenceMath
        {
            FloatFn sinf = nullptr, cosf = nullptr;
            ReferenceMath()
            {
#ifdef _WIN32
                // The Universal CRT's own functions, whatever libm the toolchain links (mingw-w64 has its own).
                if (HMODULE m = LoadLibraryA("ucrtbase.dll"))
                {
                    sinf = reinterpret_cast<FloatFn>(reinterpret_cast<void *>(GetProcAddress(m, "sinf")));
                    cosf = reinterpret_cast<FloatFn>(reinterpret_cast<void *>(GetProcAddress(m, "cosf")));
                }
#endif
            }
        };
        const ReferenceMath &referenceMath()
        {
            static const ReferenceMath m;
            return m;
        }

        // Elsewhere (no Universal CRT): the correctly rounded value, which differs from the reference's on about 6 in
        // 10,000 of TMB's calls (libraries.md section 3, open point L1).
        float sine(float x)
        {
            const ReferenceMath &m = referenceMath();
            return m.sinf ? m.sinf(x) : static_cast<float>(std::sin(static_cast<double>(x)));
        }
        float cosine(float x)
        {
            const ReferenceMath &m = referenceMath();
            return m.cosf ? m.cosf(x) : static_cast<float>(std::cos(static_cast<double>(x)));
        }

        void sqrt_(Libraries &, Guest &g) { g.retFloat(bits(std::sqrt(asFloat(g.floatArg(0))))); }

        void kernelSinf(Libraries &, Guest &g)
        {
            const float x = asFloat(g.floatArg(0)) + asFloat(g.floatArg(1));
            g.retFloat(bits(sine(x)));
        }

        void kernelCosf(Libraries &, Guest &g)
        {
            const float x = asFloat(g.floatArg(0)) + asFloat(g.floatArg(1));
            g.retFloat(bits(cosine(x)));
        }

        void remPio2f(Libraries &, Guest &g)
        {
            const float x = asFloat(g.floatArg(0));
            const float twoOverPi = asFloat(0x3F22F983u), halfPi = asFloat(0x3FC90FDBu);
            const float q = x * twoOverPi;
            const double n = std::nearbyint(static_cast<double>(q));
            const float y0 = static_cast<float>(static_cast<double>(x) - n * static_cast<double>(halfPi));
            const uint32_t y[2] = {bits(y0), 0};
            g.write(g.arg(0), y, 8);
            g.ret(static_cast<int32_t>(n));
        }
    }

    void addLibm(std::vector<Function> &out)
    {
        out.push_back({"sqrt", kLibm, sqrt_});
        out.push_back({"__kernel_sinf", kLibm, kernelSinf});
        out.push_back({"__kernel_cosf", kLibm, kernelCosf});
        out.push_back({"__ieee754_rem_pio2f", kLibm, remPio2f});
    }
}
