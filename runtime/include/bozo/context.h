// The guest CPU's state as recompiled code sees it: the R5900 context, and the helpers a port uses on it.
// Spec: docs/specs/abi.md (section 2) and docs/specs/r5900.md.
#pragma once

#include <cstdint>
#include <cstring>

struct Runtime; // the runtime: opaque to recompiled code, defined by BozoEngine's runtime (or a test stub)
struct R5900Context;

// Every recompiled function has this signature (abi.md section 1).
using RecompiledFunction = void (*)(uint8_t *rdram, R5900Context *ctx, Runtime *rt);

// A 128-bit general register, readable in every lane width the MMI instructions use. Little-endian: ud[0] is the
// low doubleword (what ordinary instructions use), uw[0] the low word.
union alignas(16) Gpr128
{
    uint64_t ud[2];
    int64_t sd[2];
    uint32_t uw[4];
    int32_t sw[4];
    uint16_t uh[8];
    int16_t sh[8];
    uint8_t ub[16];
    int8_t sb[16];
};

// A VU0 vector register: four single-precision fields x, y, z, w (f[0] .. f[3]); the bits as stored (the VU's floats
// are not IEEE and are always handled as bits by the generated code).
union alignas(16) Vu0Vector
{
    float f[4];
    uint32_t u[4];
};

struct R5900Context
{
    // ---- EE core ----
    Gpr128 r[32];        // r[0] is always zero (the generated code never writes it)
    uint64_t hi = 0;     // HI and LO: the low 64 bits are the ordinary HI/LO; MULT1/DIV1 and the MMI instructions use
    uint64_t lo = 0;     //   the upper 64 bits, kept here as hi1/lo1 (so the 128-bit HI is hi1:hi, LO is lo1:lo)
    uint64_t hi1 = 0;
    uint64_t lo1 = 0;
    uint32_t sa = 0;     // the shift amount register (QFSRV), in bits (r5900.md "SA")
    uint32_t pc = 0;     // the guest address being executed: set at function entry and before runtime calls

    // ---- COP1: the FPU (single precision, not IEEE: r5900.md "FPU") ----
    union
    {
        float f[32];
        uint32_t fu[32]; // the same registers as bits: what the generated code uses
    };
    uint32_t fcr31 = 0;  // control/status: C (bit 23) and the flags
    uint32_t facc = 0;   // the accumulator of ADDA/MULA/MADD/MSUB..., as bits

    // ---- COP2: VU0 in macro mode ----
    Vu0Vector vu0_vf[32]; // vf00 is (0, 0, 0, 1) and never written
    Vu0Vector vu0_acc;
    uint32_t vi[16] = {}; // the integer registers VI00..VI15 (16 bits each; VI00 is zero)
    union
    {
        float vu0_q;
        uint32_t vu0_q_bits;
    };
    union
    {
        float vu0_i;
        uint32_t vu0_i_bits;
    };
    union
    {
        float vu0_p;
        uint32_t vu0_p_bits;
    };
    uint32_t vu0_r = 0;           // the random number register (23 bits)
    uint32_t vu0_status = 0;      // control register 16
    uint32_t vu0_mac = 0;         // control register 17
    uint32_t vu0_clip_flags = 0;  // control register 18
    uint32_t vu0_cmsar0 = 0;      // control register 27 (VCALLMSR's start address)

    // ---- Scheduling (abi.md section 3): cycles counted at checkpoints; past the limit the runtime is called ----
    uint64_t cycles = 0;
    uint64_t cycleLimit = ~0ull;
    uint64_t checkpoints = 0; // checkpoints passed (the call log's points count them)
    uint32_t noStop = 0;      // inner calls in progress: events are taken in place until they have returned
    uint32_t inPlace = 0;     // the runtime is asked at an inner call (2) or at its return (1)
    uint32_t stopDue = 0;     // an event taken at an inner call called for a stop: it waits for the return

    R5900Context()
    {
        std::memset(r, 0, sizeof(r));
        std::memset(fu, 0, sizeof(fu));
        std::memset(vu0_vf, 0, sizeof(vu0_vf));
        std::memset(&vu0_acc, 0, sizeof(vu0_acc));
        vu0_vf[0].u[3] = 0x3F800000u; // vf00.w = 1.0
        vu0_q_bits = vu0_i_bits = vu0_p_bits = 0;
    }
};

// ---- Helpers a port uses (abi.md section 2) ------------------------------------------------------------------------
// Registers hold 32-bit values sign-extended to 64 bits (the MIPS rule); the upper 64 bits are left as they are.

inline uint32_t getRegU32(const R5900Context *ctx, int n) { return ctx->r[n].uw[0]; }
inline int32_t getRegS32(const R5900Context *ctx, int n) { return ctx->r[n].sw[0]; }
inline uint64_t getRegU64(const R5900Context *ctx, int n) { return ctx->r[n].ud[0]; }
inline void setRegU32(R5900Context *ctx, int n, uint32_t v)
{
    if (n != 0)
        ctx->r[n].sd[0] = static_cast<int32_t>(v);
}
inline void setRegU64(R5900Context *ctx, int n, uint64_t v)
{
    if (n != 0)
        ctx->r[n].ud[0] = v;
}
inline void setReturnU32(R5900Context *ctx, uint32_t v) { setRegU32(ctx, 2, v); }
inline void setReturnS32(R5900Context *ctx, int32_t v) { setRegU32(ctx, 2, static_cast<uint32_t>(v)); }
inline void setReturnU64(R5900Context *ctx, uint64_t v) { setRegU64(ctx, 2, v); }
#define GPR_U32(ctx, n) getRegU32((ctx), (n))
#define SET_GPR_U32(ctx, n, v) setRegU32((ctx), (n), (v))
