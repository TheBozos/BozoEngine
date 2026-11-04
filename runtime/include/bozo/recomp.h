// What every generated file includes: the context, the runtime's interface and the inline helpers recompiled code uses.
// Spec: docs/specs/abi.md, docs/specs/r5900.md.
#pragma once

#include "bozo/context.h"
#include "bozo/ops_fpu.h"
#include "bozo/ops_mem.h"
#include "bozo/ops_mmi.h"
#include "bozo/ops_vu0.h"
#include "bozo/runtime_api.h"

#include <cstdint>

// Short names for the generated code (prefixed BZ_ so they do not collide with a port's own).
#define BZ_D(n) ctx->r[n].ud[0]  /* a GPR's low 64 bits, unsigned */
#define BZ_SD(n) ctx->r[n].sd[0] /* signed */
#define BZ_W(n) ctx->r[n].uw[0]  /* its low 32 bits */
#define BZ_SW(n) ctx->r[n].sw[0]
#define BZ_Q(n) ctx->r[n]         /* the whole 128-bit register */
#define BZ_F(n) ctx->fu[n]        /* an FPU register's bits */
#define BZ_VF(n) ctx->vu0_vf[n]

namespace bozo
{
    // A 32-bit result written to a 64-bit register: sign-extended (the MIPS rule for 32-bit operations).
    BOZO_INLINE uint64_t sx32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

    // The scheduler's checkpoint (abi.md section 3): count guest cycles, call the runtime past the limit. `pc` is
    // where the guest code goes on from (a transfer's target): the runtime may stop the thread here (it does not
    // return then) and run it on from ctx->pc later.
    // Inside a port's native override the runtime may make guest code leave here instead (an event due: kernel.md
    // section 3b): true, with ctx->pc where it goes on, and the generated code returns.
    BOZO_INLINE bool checkpoint(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t cycles, uint32_t pc)
    {
        ctx->cycles += cycles;
        ++ctx->checkpoints;
        if (BOZO_UNLIKELY(ctx->cycles >= ctx->cycleLimit))
        {
            ctx->pc = pc;
            bozo_yield(rdram, ctx, rt);
            return ctx->leaving != 0;
        }
        return false;
    }

    // EE time charged where guest code cannot stop (the reference's call to a label of the same function).
    BOZO_INLINE void charge(R5900Context *ctx, uint32_t cycles)
    {
        ctx->cycles += cycles;
        ++ctx->checkpoints;
    }

    // An inner call (kernel.md section 3): charged and counted as a call; the events due by then are taken here, in
    // place, but a stop they call for (a handler to run, another thread to run) waits for the call's return
    // (match1p #14757679: the irq's handler starts at the return address 0x1fba84; #24702159: an irq without a
    // handler, taken at the inner call itself). The context's pc there is the call's return address `pc` (the
    // reference's: the records made there carry it, #14757679, #24702159).
    BOZO_INLINE void innerCall(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t cycles, uint32_t pc)
    {
        ++ctx->noStop;
        ctx->cycles += cycles;
        ++ctx->checkpoints;
        if (BOZO_UNLIKELY(ctx->cycles >= ctx->cycleLimit))
        {
            ctx->pc = pc;
            ctx->inPlace = 2;
            bozo_yield(rdram, ctx, rt);
            ctx->inPlace = 0;
        }
    }

    // A return to an inner call's return address (kernel.md section 3): the call has returned (ordinary checkpoints
    // can stop guest code again once none is left), and the events due meanwhile are taken here, in place, guest
    // code's frames kept, however many inner calls are still in progress (match1p #18297743).
    // Inside a port's native override (kernel.md section 3b) the return leaves for the runtime instead, the override
    // ending there (the reference: TMB's hier override ends at texmActivateTextureAsm's inner return 0x1fba84, boot
    // #228128): true, and the generated code returns.
    BOZO_INLINE bool innerReturn(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t pc)
    {
        if (ctx->noStop)
            --ctx->noStop;
        if (BOZO_UNLIKELY(ctx->outcall))
        {
            bozo_leave(rdram, ctx, rt, pc);
            return true;
        }
        if (BOZO_UNLIKELY(ctx->cycles >= ctx->cycleLimit))
        {
            ctx->pc = pc;
            ctx->inPlace = 1;
            bozo_yield(rdram, ctx, rt);
            ctx->inPlace = 0;
        }
        return false;
    }

    // ---- Multiply and divide (r5900.md "Multiply and divide") ----
    // HI/LO (or HI1/LO1) receive the two 32-bit halves of the result, each sign-extended to 64 bits.
    BOZO_INLINE void setHiLo(uint64_t &hi, uint64_t &lo, uint64_t product)
    {
        lo = sx32(static_cast<uint32_t>(product));
        hi = sx32(static_cast<uint32_t>(product >> 32));
    }
    BOZO_INLINE uint64_t mult(uint64_t &hi, uint64_t &lo, uint32_t a, uint32_t b)
    {
        setHiLo(hi, lo, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b)));
        return lo;
    }
    BOZO_INLINE uint64_t multu(uint64_t &hi, uint64_t &lo, uint32_t a, uint32_t b)
    {
        setHiLo(hi, lo, static_cast<uint64_t>(a) * b);
        return lo;
    }
    // madd / maddu: {HI, LO} (their low words) += a * b.
    BOZO_INLINE uint64_t madd(uint64_t &hi, uint64_t &lo, uint32_t a, uint32_t b)
    {
        const uint64_t acc = (static_cast<uint64_t>(static_cast<uint32_t>(hi)) << 32) | static_cast<uint32_t>(lo);
        setHiLo(hi, lo, acc + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b)));
        return lo;
    }
    BOZO_INLINE uint64_t maddu(uint64_t &hi, uint64_t &lo, uint32_t a, uint32_t b)
    {
        const uint64_t acc = (static_cast<uint64_t>(static_cast<uint32_t>(hi)) << 32) | static_cast<uint32_t>(lo);
        setHiLo(hi, lo, acc + static_cast<uint64_t>(a) * b);
        return lo;
    }
    // div: LO = quotient, HI = remainder (sign of the dividend). By zero: LO = -1 if a >= 0 else +1, HI = a.
    // 0x80000000 / -1: LO = 0x80000000, HI = 0. (Open question Q5.)
    BOZO_INLINE void div(uint64_t &hi, uint64_t &lo, uint32_t a, uint32_t b)
    {
        const int32_t sa = static_cast<int32_t>(a), sb = static_cast<int32_t>(b);
        if (sb == 0)
        {
            lo = sx32(sa >= 0 ? 0xFFFFFFFFu : 1u);
            hi = sx32(a);
        }
        else if (sa == INT32_MIN && sb == -1)
        {
            lo = sx32(0x80000000u);
            hi = 0;
        }
        else
        {
            lo = sx32(static_cast<uint32_t>(sa / sb));
            hi = sx32(static_cast<uint32_t>(sa % sb));
        }
    }
    // divu: by zero LO = 0xFFFFFFFF (sign-extended), HI = a.
    BOZO_INLINE void divu(uint64_t &hi, uint64_t &lo, uint32_t a, uint32_t b)
    {
        if (b == 0)
        {
            lo = sx32(0xFFFFFFFFu);
            hi = sx32(a);
        }
        else
        {
            lo = sx32(a / b);
            hi = sx32(a % b);
        }
    }

    // ---- FPU wrappers ----
    // The arithmetic leaves FCR31's flags as they are (Q7: the reference captures never show an operation changing
    // them, division by zero and overflow included); only the compares change FCR31 (its C bit).
    BOZO_INLINE uint32_t fadd(R5900Context *, uint32_t a, uint32_t b) { uint32_t f = 0; return ps2f::add(a, b, f); }
    BOZO_INLINE uint32_t fsub(R5900Context *, uint32_t a, uint32_t b) { uint32_t f = 0; return ps2f::sub(a, b, f); }
    BOZO_INLINE uint32_t fmul(R5900Context *, uint32_t a, uint32_t b) { uint32_t f = 0; return ps2f::mul(a, b, f); }
    // Division by zero (x/0 and 0/0) sets FCR31 bit 20, which the reference keeps (Q7); nothing else touches it.
    BOZO_INLINE uint32_t fdiv(R5900Context *ctx, uint32_t a, uint32_t b)
    {
        uint32_t f = 0;
        if (BOZO_UNLIKELY(ps2f::isZero(b)))
            ctx->fcr31 |= ps2f::kFlagDivZeroSeen;
        return ps2f::div(a, b, f);
    }
    BOZO_INLINE uint32_t fsqrt(R5900Context *, uint32_t b) { uint32_t f = 0; return ps2f::sqrt(b, f); }
    BOZO_INLINE uint32_t frsqrt(R5900Context *, uint32_t a, uint32_t b) { uint32_t f = 0; return ps2f::rsqrt(a, b, f); }
    // Accumulator forms: madd = acc + a*b, msub = acc - a*b: the product rounded first, then the FPU's sum (Q6; TMB
    // has none of them, unchecked).
    BOZO_INLINE uint32_t fmadd(R5900Context *, uint32_t acc, uint32_t a, uint32_t b)
    {
        uint32_t f = 0;
        return ps2f::add(acc, ps2f::mul(a, b, f), f);
    }
    BOZO_INLINE uint32_t fmsub(R5900Context *, uint32_t acc, uint32_t a, uint32_t b)
    {
        uint32_t f = 0;
        return ps2f::sub(acc, ps2f::mul(a, b, f), f);
    }
    BOZO_INLINE void fcompare(R5900Context *ctx, bool c)
    {
        ctx->fcr31 = c ? (ctx->fcr31 | ps2f::kFlagC) : (ctx->fcr31 & ~ps2f::kFlagC);
    }
    BOZO_INLINE bool fcond(const R5900Context *ctx) { return (ctx->fcr31 & ps2f::kFlagC) != 0; }
}
