// The instruction test cases. Each one: code (assembled from fields), a setup, and the checks, with the expected values
// worked out from docs/specs/r5900.md. Where the spec records a choice on an open question (Qn), the case tests the
// choice and says so.
#include "cases.h"

#include <cstdio>

using namespace tasm;

namespace itest
{
    void Checker::eq(const char *what, uint64_t got, uint64_t want)
    {
        if (got == want)
            return;
        ++failures_;
        std::printf("FAIL %s: %s = 0x%llx, expected 0x%llx\n", name_.c_str(), what, static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(want));
    }

    void Checker::events(const Runtime &rt, const std::vector<std::string> &want)
    {
        if (rt.events == want)
            return;
        ++failures_;
        std::printf("FAIL %s: runtime events [", name_.c_str());
        for (const auto &e : rt.events)
            std::printf(" '%s'", e.c_str());
        std::printf(" ], expected [");
        for (const auto &e : want)
            std::printf(" '%s'", e.c_str());
        std::printf(" ]\n");
    }

    namespace
    {
        using Ctx = R5900Context;
        using Setup = std::function<void(Ctx &, uint8_t *, Runtime &)>;
        using Check = std::function<void(Checker &, Ctx &, uint8_t *, Runtime &)>;

        std::vector<Case> g_cases;

        Env nextEnv() { return envFor(g_cases.size()); }

        void add(const std::string &name, std::vector<uint32_t> code, Setup setup, Check check,
                 std::vector<uint32_t> callee = {}, std::vector<std::pair<uint32_t, uint32_t>> data = {})
        {
            Case c;
            c.name = name;
            c.code = std::move(code);
            c.callee = std::move(callee);
            c.data = std::move(data);
            c.setup = std::move(setup);
            c.check = std::move(check);
            g_cases.push_back(std::move(c));
        }

        // FCR31 flag bits (r5900.md "FPU"): invalid, division by zero, overflow, underflow, and their sticky copies.
        constexpr uint32_t kI = 1u << 17, kD = 1u << 16, kO = 1u << 15, kU = 1u << 14;
        constexpr uint32_t kSI = 1u << 6, kSD = 1u << 5, kSO = 1u << 4, kSU = 1u << 3;
        constexpr uint32_t kDivZero = 1u << 20; // set by a division by zero, as the reference does (Q7)
        constexpr uint32_t kFlagMask = kI | kD | kO | kU | kSI | kSD | kSO | kSU | kDivZero;

        void set(Ctx &c, int n, uint64_t v) { c.r[n].ud[0] = v; }
        uint64_t get(const Ctx &c, int n) { return c.r[n].ud[0]; }
        const Setup none = [](Ctx &, uint8_t *, Runtime &) {};
        uint32_t hi16(uint32_t a) { return (a + 0x8000u) >> 16; }  // %hi: with the carry of a negative %lo
        int32_t lo16(uint32_t a) { return static_cast<int16_t>(a & 0xFFFFu); }

        // ---------------------------------------------------------------------------------------------------------
        void integerCases()
        {
            add("addiu sign-extends its 32-bit result", {addiu(v0, a0, 1), addiu(v1, a1, -6)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0x7FFFFFFF); set(c, a1, 5); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("v0", get(c, v0), 0xFFFFFFFF80000000ull);
                    k.eq("v1", get(c, v1), ~0ull);
                });
            add("addu ignores the upper 32 bits of its operands", {addu(v0, a0, a1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0x12345678FFFFFFFFull); set(c, a1, 1); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) { k.eq("v0", get(c, v0), 0); });
            add("add and addi: overflow does not trap (not modelled)", {add(v0, a0, a1), addi(v1, a0, 1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0x7FFFFFFF); set(c, a1, 1); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                    k.eq("v0", get(c, v0), 0xFFFFFFFF80000000ull);
                    k.eq("v1", get(c, v1), 0xFFFFFFFF80000000ull);
                    k.events(rt, {});
                });
            add("subu and sub wrap at 32 bits", {subu(v0, zero, a0), sub(v1, a1, a0)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 1); set(c, a1, 0x80000000u); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("v0", get(c, v0), ~0ull);
                    k.eq("v1", get(c, v1), 0x7FFFFFFF);
                });
            add("daddu / dsubu / daddiu are 64-bit", {daddu(v0, a0, a1), dsubu(v1, zero, a1), daddiu(t0, a2, -1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0x7FFFFFFFFFFFFFFFull); set(c, a1, 1); set(c, a2, 0xFFFFFFFFull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("v0", get(c, v0), 0x8000000000000000ull);
                    k.eq("v1", get(c, v1), ~0ull);
                    k.eq("t0", get(c, t0), 0xFFFFFFFEull);
                });
            add("lui sign-extends; ori/andi/xori zero-extend", {lui(v0, 0x8000), ori(v1, zero, 0x8000), andi(t0, a0, 0xFFFF), xori(t1, a0, 0xFFFF)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, ~0ull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("v0", get(c, v0), 0xFFFFFFFF80000000ull);
                    k.eq("v1", get(c, v1), 0x8000);
                    k.eq("t0", get(c, t0), 0xFFFF);
                    k.eq("t1", get(c, t1), 0xFFFFFFFFFFFF0000ull);
                });
            add("slt/sltu/slti/sltiu: signed and unsigned 64-bit compares",
                {slt(v0, a0, a1), sltu(v1, a0, a1), slti(t0, a0, 0), sltiu(t1, a1, -1), sltiu(t2, a0, -1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, ~0ull); set(c, a1, 1); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("slt", get(c, v0), 1);
                    k.eq("sltu", get(c, v1), 0);
                    k.eq("slti", get(c, t0), 1);
                    k.eq("sltiu 1 < (u64)-1", get(c, t1), 1);
                    k.eq("sltiu -1 < -1", get(c, t2), 0);
                });
            add("and/or/xor/nor are 64-bit", {and_(v0, a0, a1), or_(v1, a0, a1), xor_(t0, a0, a1), nor(t1, a0, a1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0xFF00FF00FF00FF00ull); set(c, a1, 0x0FF00FF00FF00FF0ull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("and", get(c, v0), 0x0F000F000F000F00ull);
                    k.eq("or", get(c, v1), 0xFFF0FFF0FFF0FFF0ull);
                    k.eq("xor", get(c, t0), 0xF0F0F0F0F0F0F0F0ull);
                    k.eq("nor", get(c, t1), 0x000F000F000F000Full);
                });
            add("sll/srl/sra on 32 bits, sign-extended",
                {sll(v0, a0, 31), sll(v1, a1, 0), srl(t0, a1, 1), srl(t1, a2, 0), sra(t2, a2, 4)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 1); set(c, a1, 0x0000000180000000ull); set(c, a2, 0x80000000u); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("sll 31", get(c, v0), 0xFFFFFFFF80000000ull);
                    k.eq("sll 0 (sign-extends the low word)", get(c, v1), 0xFFFFFFFF80000000ull);
                    k.eq("srl 1", get(c, t0), 0x40000000);
                    k.eq("srl 0", get(c, t1), 0xFFFFFFFF80000000ull);
                    k.eq("sra 4", get(c, t2), 0xFFFFFFFFF8000000ull);
                });
            add("sllv/srlv/srav use the low 5 bits of rs", {sllv(v0, a0, a1), srlv(v1, a2, a1), srav(t0, a2, a1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 3); set(c, a1, 33); set(c, a2, 0x80000000u); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("sllv", get(c, v0), 6);
                    k.eq("srlv", get(c, v1), 0x40000000);
                    k.eq("srav", get(c, t0), 0xFFFFFFFFC0000000ull);
                });
            add("dsll/dsrl/dsra and the 32 forms",
                {dsll(v0, a0, 4), dsrl(v1, a1, 4), dsra(t0, a1, 4), dsll32(t1, a0, 1), dsrl32(t2, a1, 0), dsra32(t3, a1, 4)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0x0123456789ABCDEFull); set(c, a1, 0x8000000000000010ull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("dsll", get(c, v0), 0x123456789ABCDEF0ull);
                    k.eq("dsrl", get(c, v1), 0x0800000000000001ull);
                    k.eq("dsra", get(c, t0), 0xF800000000000001ull);
                    k.eq("dsll32", get(c, t1), 0x13579BDE00000000ull);
                    k.eq("dsrl32", get(c, t2), 0x80000000ull);
                    k.eq("dsra32", get(c, t3), 0xFFFFFFFFF8000000ull);
                });
            add("dsllv/dsrlv/dsrav use the low 6 bits of rs", {dsllv(v0, a0, a1), dsrlv(v1, a2, a1), dsrav(t0, a2, a1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 1); set(c, a1, 64 + 36); set(c, a2, 0x8000000000000000ull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("dsllv", get(c, v0), 1ull << 36);
                    k.eq("dsrlv", get(c, v1), 0x8000000ull);
                    k.eq("dsrav", get(c, t0), 0xFFFFFFFFF8000000ull);
                });
            add("movz/movn test all 64 bits", {movz(v0, a0, a1), movn(v1, a0, a1), movz(t0, a0, zero)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 7); set(c, a1, 0x100000000ull); set(c, v0, 1); set(c, v1, 1); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("movz not moved", get(c, v0), 1);
                    k.eq("movn moved", get(c, v1), 7);
                    k.eq("movz moved", get(c, t0), 7);
                });
            add("writes to r0 are dropped", {addiu(zero, zero, 5), lui(zero, 0x1234), addu(v0, zero, zero)}, none,
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("r0", get(c, 0), 0);
                    k.eq("v0", get(c, v0), 0);
                });
            add("32/64-bit instructions keep the register's upper 64 bits", {addiu(v0, zero, 1), ld(v1, 0, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) {
                    c.r[v0].ud[1] = 0xAAAAAAAAAAAAAAAAull;
                    c.r[v1].ud[1] = 0xBBBBBBBBBBBBBBBBull;
                    set(c, a0, 0x2000);
                    wr64(ram, 0x2000, 0x1122334455667788ull);
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("v0 upper", c.r[v0].ud[1], 0xAAAAAAAAAAAAAAAAull);
                    k.eq("v1 upper", c.r[v1].ud[1], 0xBBBBBBBBBBBBBBBBull);
                    k.eq("v1", get(c, v1), 0x1122334455667788ull);
                });
        }

        // ---------------------------------------------------------------------------------------------------------
        void multiplyDivideCases()
        {
            add("mult: 3-operand, HI/LO sign-extended", {mult(v0, a0, a1), mfhi(v1), mflo(t0)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, static_cast<uint64_t>(-2)); set(c, a1, 3); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("rd", get(c, v0), static_cast<uint64_t>(-6));
                    k.eq("hi", get(c, v1), ~0ull);
                    k.eq("lo", get(c, t0), static_cast<uint64_t>(-6));
                });
            add("multu: unsigned product, halves sign-extended", {multu(zero, a0, a1), mfhi(v1), mflo(t0)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0xFFFFFFFFu); set(c, a1, 2); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("hi", get(c, v1), 1);
                    k.eq("lo", get(c, t0), 0xFFFFFFFFFFFFFFFEull);
                });
            add("div: quotient toward zero, remainder has the dividend's sign", {div(a0, a1), mflo(v0), mfhi(v1), div(a2, a1), mflo(t0), mfhi(t1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 7); set(c, a1, static_cast<uint64_t>(-2)); set(c, a2, static_cast<uint64_t>(-7)); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("7/-2 lo", get(c, v0), static_cast<uint64_t>(-3));
                    k.eq("7/-2 hi", get(c, v1), 1);
                    k.eq("-7/-2 lo", get(c, t0), 3);
                    k.eq("-7/-2 hi", get(c, t1), static_cast<uint64_t>(-1));
                });
            add("div by zero (Q5): LO = -1 or +1 by the dividend's sign, HI = dividend",
                {div(a0, zero), mflo(v0), mfhi(v1), div(a1, zero), mflo(t0), mfhi(t1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 5); set(c, a1, static_cast<uint64_t>(-5)); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("5/0 lo", get(c, v0), ~0ull);
                    k.eq("5/0 hi", get(c, v1), 5);
                    k.eq("-5/0 lo", get(c, t0), 1);
                    k.eq("-5/0 hi", get(c, t1), static_cast<uint64_t>(-5));
                });
            add("div 0x80000000 / -1 (Q5)", {div(a0, a1), mflo(v0), mfhi(v1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, sx(0x80000000u)); set(c, a1, ~0ull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("lo", get(c, v0), 0xFFFFFFFF80000000ull);
                    k.eq("hi", get(c, v1), 0);
                });
            add("divu, and divu by zero (Q5)", {divu(a0, a1), mflo(v0), mfhi(v1), divu(a0, zero), mflo(t0), mfhi(t1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, sx(0xFFFFFFF0u)); set(c, a1, 16); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("lo", get(c, v0), 0x0FFFFFFF);
                    k.eq("hi", get(c, v1), 0);
                    k.eq("/0 lo", get(c, t0), ~0ull);
                    k.eq("/0 hi", get(c, t1), 0xFFFFFFFFFFFFFFF0ull);
                });
            add("mthi/mtlo then madd accumulates 64 bits", {mthi(a2), mtlo(a3), madd(v0, a0, a1), mfhi(v1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0x10000); set(c, a1, 0x10000); set(c, a2, 0); set(c, a3, sx(0xFFFFFFFFu)); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    // 0x00000000FFFFFFFF + 0x100000000 = 0x1FFFFFFFF
                    k.eq("rd = lo", get(c, v0), ~0ull);
                    k.eq("hi", get(c, v1), 1);
                });
            add("mult1/div1 use HI1/LO1 and leave HI/LO", {mult(zero, a0, a0), mult1(v0, a0, a1), mflo1(v1), mfhi1(t0), mflo(t1), div1(a1, a0), mflo1(t2), mfhi1(t3)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 3); set(c, a1, 10); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("mult1 rd", get(c, v0), 30);
                    k.eq("lo1", get(c, v1), 30);
                    k.eq("hi1", get(c, t0), 0);
                    k.eq("lo untouched by mult1", get(c, t1), 9);
                    k.eq("div1 lo1", get(c, t2), 3);
                    k.eq("div1 hi1", get(c, t3), 1);
                });
        }

        // ---------------------------------------------------------------------------------------------------------
        void memoryCases()
        {
            add("lb/lbu/lh/lhu/lw/lwu extend correctly",
                {lb(v0, 0, a0), lbu(v1, 0, a0), lh(t0, 0, a0), lhu(t1, 0, a0), lw(t2, 0, a0), lwu(t3, 0, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) { set(c, a0, 0x3000); wr32(ram, 0x3000, 0x8000FF80u); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("lb", get(c, v0), 0xFFFFFFFFFFFFFF80ull);
                    k.eq("lbu", get(c, v1), 0x80);
                    k.eq("lh", get(c, t0), 0xFFFFFFFFFFFFFF80ull);
                    k.eq("lhu", get(c, t1), 0xFF80);
                    k.eq("lw", get(c, t2), 0xFFFFFFFF8000FF80ull);
                    k.eq("lwu", get(c, t3), 0x8000FF80ull);
                });
            add("sb/sh/sw/sd store the low bytes, little-endian; negative offsets",
                {sb(a1, 0, a0), sh(a1, 2, a0), sw(a1, 4, a0), sd(a1, 8, a0), lw(v0, -4, a2)},
                [](Ctx &c, uint8_t *ram, Runtime &) { set(c, a0, 0x3100); set(c, a1, 0x1122334455667788ull); set(c, a2, 0x3108); },
                [](Checker &k, Ctx &c, uint8_t *ram, Runtime &) {
                    k.eq("word 0", rd32(ram, 0x3100), 0x77880088u);
                    k.eq("word 1", rd32(ram, 0x3104), 0x55667788u);
                    k.eq("dword", rd64(ram, 0x3108), 0x1122334455667788ull);
                    k.eq("lw -4", get(c, v0), 0x55667788u);
                });
            add("lq/sq: 128 bits, the low 4 address bits ignored", {lq(v0, 7, a0), sq(v0, 0x13, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) {
                    set(c, a0, 0x3200);
                    wr64(ram, 0x3200, 0x0706050403020100ull);
                    wr64(ram, 0x3208, 0x0F0E0D0C0B0A0908ull);
                },
                [](Checker &k, Ctx &c, uint8_t *ram, Runtime &) {
                    k.eq("lo", c.r[v0].ud[0], 0x0706050403020100ull);
                    k.eq("hi", c.r[v0].ud[1], 0x0F0E0D0C0B0A0908ull);
                    k.eq("stored lo", rd64(ram, 0x3210), 0x0706050403020100ull);
                    k.eq("stored hi", rd64(ram, 0x3218), 0x0F0E0D0C0B0A0908ull);
                });
            for (int off = 1; off <= 3; ++off)
                add("lwl/lwr pair loads an unaligned word at +" + std::to_string(off), {lwl(v0, off + 3, a0), lwr(v0, off, a0)},
                    [](Ctx &c, uint8_t *ram, Runtime &) {
                        set(c, a0, 0x3300);
                        wr64(ram, 0x3300, 0x8877665544332211ull);
                        set(c, v0, 0x123456789ull);
                    },
                    [off](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        const uint32_t want = static_cast<uint32_t>(0x8877665544332211ull >> (8 * off));
                        k.eq("v0", get(c, v0), sx(want));
                    });
            add("lwr alone (Q4): offset 0 sign-extends, offset 2 keeps the upper 32 bits", {lwr(v0, 0, a0), lwr(v1, 2, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) {
                    set(c, a0, 0x3400);
                    wr32(ram, 0x3400, 0x80112233u);
                    set(c, v1, 0xAAAAAAAABBBBBBBBull);
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("lwr +0", get(c, v0), 0xFFFFFFFF80112233ull);
                    k.eq("lwr +2", get(c, v1), 0xAAAAAAAABBBB8011ull);
                });
            add("lwl alone merges the high bytes and sign-extends", {lwl(v0, 1, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) { set(c, a0, 0x3400); wr32(ram, 0x3400, 0x00008899u); set(c, v0, 0x11223344u); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) { k.eq("lwl +1", get(c, v0), 0xFFFFFFFF88993344ull); });
            add("swl/swr pair stores an unaligned word", {swl(a1, 5, a0), swr(a1, 2, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) { set(c, a0, 0x3500); set(c, a1, 0xAABBCCDDu); wr64(ram, 0x3500, 0x1111111111111111ull); },
                [](Checker &k, Ctx &, uint8_t *ram, Runtime &) { k.eq("bytes", rd64(ram, 0x3500), 0x1111AABBCCDD1111ull); });
            add("ldl/ldr pair loads an unaligned doubleword", {ldl(v0, 10, a0), ldr(v0, 3, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) {
                    set(c, a0, 0x3600);
                    for (int i = 0; i < 16; ++i)
                        ram[0x3600 + i] = static_cast<uint8_t>(0x10 + i);
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) { k.eq("v0", get(c, v0), 0x1A19181716151413ull); });
            add("sdl/sdr pair stores an unaligned doubleword", {sdl(a1, 12, a0), sdr(a1, 5, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) { set(c, a0, 0x3700); set(c, a1, 0x8877665544332211ull); std::memset(ram + 0x3700, 0xEE, 16); },
                [](Checker &k, Ctx &, uint8_t *ram, Runtime &) {
                    k.eq("low dword", rd64(ram, 0x3700), 0x332211EEEEEEEEEEull);
                    k.eq("high dword", rd64(ram, 0x3708), 0xEEEEEE8877665544ull);
                });
            add("scratchpad at 0x70000000", {lui(a0, 0x7000), addiu(a1, zero, 0x1234), sw(a1, 0x10, a0), lw(v0, 0x10, a0)}, none,
                [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                    k.eq("v0", get(c, v0), 0x1234);
                    k.eq("scratchpad", rd32(rt.scratchpad, 0x10), 0x1234);
                });
            add("KSEG0 and uncached views reach main RAM",
                {lui(a0, 0x8000), lui(a2, 0x2000), addiu(a1, zero, 0x55), sw(a1, 0x1000, a0), sw(a1, 0x1004, a2), lw(v0, 0x1000, zero)}, none,
                [](Checker &k, Ctx &c, uint8_t *ram, Runtime &) {
                    k.eq("0x80001000", rd32(ram, 0x1000), 0x55);
                    k.eq("0x20001004", rd32(ram, 0x1004), 0x55);
                    k.eq("v0", get(c, v0), 0x55);
                });
            add("hardware addresses go to the runtime", {lui(a0, 0x1000), lw(v0, 0, a0), sw(a0, 0x10, a0)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, v0, 77); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                    k.eq("v0", get(c, v0), 0);
                    k.events(rt, {"io-read 0x10000000", "io-write 0x10000010"});
                });
            for (bool handled : {false, true})
                for (bool delaySlot : {false, true})
                {
                    Asm s;
                    if (delaySlot)
                        s.branch(beq(zero, zero), "done")(sw(a1, 0, a0)).label("done")(addiu(v0, zero, 7));
                    else
                        s(sw(a1, 0, a0))(addiu(v0, zero, 7));
                    add(std::string("native output ") + (handled ? "handled" : "checker fallback") + (delaySlot ? " in delay slot" : " inline"),
                        s.finish(), [handled](Ctx &c, uint8_t *, Runtime &rt) {
                            set(c, a0, 0x1000a000); set(c, a1, 0x145); rt.nativeHandled = handled;
                        }, [handled](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                            k.eq("native calls", rt.nativeCalls, 1);
                            k.eq("continued after replacement", get(c, v0), 7);
                            k.eq("returned to caller", c.pc, kReturnSentinel);
                            if (!handled)
                                k.eq("checker retains the store's pc", rt.lastIoPc, rt.lastNativePc);
                            k.events(rt, handled ? std::vector<std::string>{} : std::vector<std::string>{"io-write 0x1000a000"});
                        });
                    g_cases.back().nativeOffsets = {delaySlot ? 4u : 0u};
                }
        }

        // ---------------------------------------------------------------------------------------------------------
        void branchCases()
        {
            for (bool likely : {false, true})
                for (bool taken : {false, true})
                {
                    Asm s;
                    s.branch(likely ? beql(a0, zero) : beq(a0, zero), "done")(lw(v0, 0, a1))
                        (addiu(v1, zero, 11)).label("done")(nop);
                    add(std::string("guarded adaptation in ") + (likely ? "likely" : "ordinary") + (taken ? " taken slot" : " untaken slot"),
                        s.finish(), [taken](Ctx &c, uint8_t *, Runtime &) { set(c, a0, taken ? 0 : 1); set(c, a1, 0x12000000); },
                        [likely, taken](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                            k.eq("adapted delay result", get(c, v0), likely && !taken ? 0 : 7);
                            k.eq("fallthrough", get(c, v1), taken ? 0 : 11);
                            k.eq("returned", c.pc, kReturnSentinel);
                            k.eq("ra preserved", get(c, ra), kReturnSentinel);
                            k.events(rt, {});
                        });
                    g_cases.back().adaptations = {{4, addiu(v0, zero, 7)}};
                }
            {
                Asm s;
                s(addiu(a0, zero, 3)).label("loop")(addiu(a0, a0, -1)).branch(bne(a0, zero), "loop")(lw(v0, 0, a1));
                add("guarded adaptation preserves loop checkpoints", s.finish(),
                    [](Ctx &c, uint8_t *, Runtime &) { set(c, a1, 0x12000000); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                        k.eq("adapted loop slots", get(c, v0), 3);
                        k.eq("cycles", c.cycles, 64);
                        k.eq("checkpoints", c.checkpoints, 2);
                        k.eq("returned", c.pc, kReturnSentinel);
                        k.events(rt, {});
                    });
                g_cases.back().adaptations = {{12, addiu(v0, v0, 1)}};
            }
            {
                const Env e = nextEnv();
                Asm s(e.base);
                s.jumpTo(2, e.callee + 8)(nop);
                add("guarded adaptation preserves an interior entry", s.finish(),
                    [](Ctx &c, uint8_t *, Runtime &) { set(c, a1, 0x12000000); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                        k.eq("skipped callee prefix", get(c, v0) | get(c, v1), 0);
                        k.eq("adapted interior result", get(c, a0), 9);
                        k.eq("returned", c.pc, kReturnSentinel);
                        k.eq("ra preserved", get(c, ra), kReturnSentinel);
                        k.events(rt, {});
                    }, {addiu(v0, zero, 1), addiu(v1, zero, 2), lw(a0, 0, a1)});
                g_cases.back().adaptations = {{kCalleeOffset + 8, addiu(a0, zero, 9)}};
            }
            {
                Asm s;
                s.branch(beq(a0, a1), "t")(addiu(v1, zero, 1)) // delay slot: runs either way
                    (addiu(v0, zero, 2))
                    .label("t")(nop);
                add("beq taken: the delay slot runs", s.finish(), [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 4); set(c, a1, 4); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0 (skipped)", get(c, v0), 0);
                        k.eq("v1 (delay)", get(c, v1), 1);
                    });
                add("beq not taken: the delay slot runs", s.finish(), [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 4); set(c, a1, 5); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0", get(c, v0), 2);
                        k.eq("v1 (delay)", get(c, v1), 1);
                    });
            }
            {
                Asm s;
                s.branch(beql(a0, a1), "t")(addiu(v1, zero, 1))(addiu(v0, zero, 2)).label("t")(nop);
                add("beql taken: the delay slot runs", s.finish(), [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 4); set(c, a1, 4); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0", get(c, v0), 0);
                        k.eq("v1 (delay)", get(c, v1), 1);
                    });
                add("beql not taken: the delay slot is skipped", s.finish(), [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 4); set(c, a1, 5); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0", get(c, v0), 2);
                        k.eq("v1 (delay skipped)", get(c, v1), 0);
                    });
            }
            // Each conditional branch kind, on -1, 0 and 1: v0 bit i set when taken for input i.
            struct Kind
            {
                const char *name;
                uint32_t word;
                bool likely;
                uint32_t takenMask; // bit 0: a0 = -1, bit 1: a0 = 0, bit 2: a0 = 1
            };
            const Kind kinds[] = {
                {"bne a0, zero", bne(a0, zero), false, 0b101}, {"blez", blez(a0), false, 0b011},
                {"bgtz", bgtz(a0), false, 0b100},              {"bltz", bltz(a0), false, 0b001},
                {"bgez", bgez(a0), false, 0b110},              {"bnel", bnel(a0, zero), true, 0b101},
                {"blezl", blezl(a0), true, 0b011},             {"bgtzl", bgtzl(a0), true, 0b100},
                {"bltzl", bltzl(a0), true, 0b001},             {"bgezl", bgezl(a0), true, 0b110},
            };
            for (const Kind &kd : kinds)
            {
                Asm s;
                // For each input: v1 counts delay slots run, v0 collects taken bits.
                for (int i = 0; i < 3; ++i)
                {
                    const std::string t = "t" + std::to_string(i), n = "n" + std::to_string(i);
                    s(addiu(a0, zero, i - 1))
                        .branch(kd.word, t)(addiu(v1, v1, 1))
                        .branch(beq(zero, zero), n)(nop)
                        .label(t)(ori(v0, v0, 1u << i))
                        .label(n)(nop);
                }
                const uint32_t mask = kd.takenMask;
                const bool likely = kd.likely;
                add(std::string(kd.name) + " on -1, 0, 1", s.finish(), none,
                    [mask, likely](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("taken", get(c, v0), mask);
                        const uint32_t taken = static_cast<uint32_t>(__builtin_popcount(mask));
                        k.eq("delay slots run", get(c, v1), likely ? taken : 3u);
                    });
            }
            {
                Asm s;
                s.branch(beq(a0, a1), "t")(addiu(a0, a0, 1))(addiu(v0, zero, 2)).branch(beq(zero, zero), "e")(nop)
                    .label("t")(addiu(v0, zero, 1))
                    .label("e")(nop);
                add("the condition is read before the delay slot changes it", s.finish(),
                    [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 5); set(c, a1, 5); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0 (taken)", get(c, v0), 1);
                        k.eq("a0", get(c, a0), 6);
                    });
            }
            {
                Asm s;
                s(addiu(v0, zero, 0))(addiu(a0, zero, 10)).label("loop")(addu(v0, v0, a0))(addiu(a0, a0, -1)).branch(bne(a0, zero), "loop")(nop);
                add("a backward loop, with checkpoints", s.finish(), none,
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("sum 1..10", get(c, v0), 55);
                        k.eq("cycles counted", c.cycles >= 10 ? 1 : 0, 1);
                    });
            }
            {
                Asm s;
                s(addiu(a0, zero, 3)).label("loop")(addiu(v0, v0, 1)).branch(bnel(a0, zero), "loop")(addiu(a0, a0, -1));
                add("bnel loop: the delay slot only on taken iterations", s.finish(), none,
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0", get(c, v0), 4);
                        k.eq("a0", get(c, a0), 0);
                    });
                Asm u;
                u(addiu(a0, zero, 3)).label("loop")(addiu(v0, v0, 1)).branch(bne(a0, zero), "loop")(addiu(a0, a0, -1));
                add("bne loop: the delay slot on every iteration", u.finish(), none,
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0", get(c, v0), 4);
                        k.eq("a0", get(c, a0), ~0ull);
                    });
            }
            {
                // A delay slot that is also a branch target (0x10 below).
                Asm s;
                s.branch(bne(a0, zero), "ds")(nop)                // 0x00, 0x04
                    (addiu(v1, zero, 7))                          // 0x08
                    .branch(beq(a1, zero), "end")                 // 0x0c
                    .label("ds")(addiu(v0, v0, 1))                // 0x10: delay slot of 0x0c, target of 0x00
                    (addiu(t0, zero, 100))                        // 0x14
                    .label("end")(nop);                           // 0x18
                const auto code = s.finish();
                add("delay slot as a branch target: reached by the jump", code, [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 1); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0", get(c, v0), 1);
                        k.eq("v1", get(c, v1), 0);
                        k.eq("t0", get(c, t0), 100);
                    });
                add("delay slot as a branch target: its branch taken", code, [](Ctx &c, uint8_t *, Runtime &) { set(c, a1, 0); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0 (once)", get(c, v0), 1);
                        k.eq("v1", get(c, v1), 7);
                        k.eq("t0", get(c, t0), 0);
                    });
                add("delay slot as a branch target: its branch not taken", code, [](Ctx &c, uint8_t *, Runtime &) { set(c, a1, 1); },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0 (once)", get(c, v0), 1);
                        k.eq("v1", get(c, v1), 7);
                        k.eq("t0", get(c, t0), 100);
                    });
            }
            {
                Asm s(nextEnv().base); // j needs the absolute address
                s.jump(2, "t")(addiu(v1, zero, 1))(addiu(v0, zero, 2)).label("t")(nop);
                add("j inside the function, with its delay slot", s.finish(), none,
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0", get(c, v0), 0);
                        k.eq("v1", get(c, v1), 1);
                    });
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        void callCases()
        {
            {
                const Env e = nextEnv();
                Asm s(e.base);
                s.jumpTo(3, e.callee)(addiu(a0, zero, 5))(addu(v1, v0, zero));
                add("jal: link, delay slot before the call, continue after", s.finish(), none,
                    [e](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0", get(c, v0), 6);
                        k.eq("v1", get(c, v1), 6);
                        k.eq("ra", get(c, ra), e.base + 8);
                    },
                    {addiu(v0, a0, 1)});
            }
            {
                const Env e = nextEnv();
                Asm s(e.base);
                s(lui(t9, e.callee >> 16))(ori(t9, t9, e.callee & 0xFFFF))(jalr(ra, t9))(addiu(a0, zero, 7));
                add("jalr through the function table", s.finish(), none,
                    [e](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                        k.eq("v0", get(c, v0), 8);
                        k.eq("ra", get(c, ra), e.base + 16);
                        k.events(rt, {});
                    },
                    {addiu(v0, a0, 1)});
            }
            {
                const Env e = nextEnv();
                Asm s(e.base);
                s.branchTo(bgezal(zero), e.callee)(addiu(a0, zero, 1))(addiu(a1, zero, 1)).branchTo(bltzal(a1), e.callee)(addiu(a0, zero, 50));
                add("bal calls; bltzal not taken still links", s.finish(), none,
                    [e](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0 (one call)", get(c, v0), 2);
                        k.eq("ra (from bltzal)", get(c, ra), e.base + 20);
                        k.eq("a0", get(c, a0), 50);
                    },
                    {addiu(v0, a0, 1)});
            }
            {
                // An external address marked "stub" (EmitOptions::externals): every transfer to it goes to the runtime.
                const Env e = nextEnv();
                Asm s(e.base);
                s.jumpTo(3, e.callee)(nop).jumpTo(2, e.callee)(nop);
                add("a stub address: jal and j both go through bozo_call_indirect", s.finish(), none,
                    [e](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                        k.eq("v0 (the callee ran twice)", get(c, v0), 2);
                        k.eq("transfers through the runtime", rt.indirect.size(), 2);
                        k.eq("target", rt.indirect.empty() ? 0 : rt.indirect[0], e.callee);
                    },
                    {addiu(v0, v0, 1)});
                g_cases.back().calleeExternal = 1;
            }
            {
                // "override": calls go to the runtime; a direct jump runs the guest code there.
                const Env e = nextEnv();
                Asm s(e.base);
                s.jumpTo(3, e.callee)(nop).jumpTo(2, e.callee)(nop);
                add("an override address: jal goes through bozo_call_indirect, j runs the guest code", s.finish(), none,
                    [e](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                        k.eq("v0 (the callee ran twice)", get(c, v0), 2);
                        k.eq("transfers through the runtime", rt.indirect.size(), 1);
                    },
                    {addiu(v0, v0, 1)});
                g_cases.back().calleeExternal = 2;
            }
            for (int input : {1, 5})
            {
                const Env e = nextEnv();
                Asm s(e.base);
                const uint32_t table = e.data;
                s(sltiu(at, a0, 3))
                    .branch(beq(at, zero), "default")(sll(v0, a0, 2))
                    (lui(at, hi16(table)))(addu(at, at, v0))(lw(v0, lo16(table), at))(jr(v0))(nop)
                    .label("c0")(addiu(v1, zero, 100)).branch(beq(zero, zero), "end")(nop)
                    .label("c1")(addiu(v1, zero, 101)).branch(beq(zero, zero), "end")(nop)
                    .label("c2")(addiu(v1, zero, 102)).branch(beq(zero, zero), "end")(nop)
                    .label("default")(addiu(v1, zero, 99))
                    .label("end")(nop);
                const uint64_t want = input == 1 ? 101 : 99;
                add("jump table (switch) with input " + std::to_string(input), s.finish(),
                    [input](Ctx &c, uint8_t *, Runtime &) { set(c, a0, static_cast<uint64_t>(input)); },
                    [want](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                        k.eq("v1", get(c, v1), want);
                        k.events(rt, {});
                    },
                    {}, {{table, s.address("c0")}, {table + 4, s.address("c1")}, {table + 8, s.address("c2")}});
            }
            {
                const Env e = nextEnv();
                Asm s(e.base);
                s(lui(t9, e.callee >> 16))(ori(t9, t9, e.callee & 0xFFFF))(jr(t9))(addiu(a0, zero, 41));
                add("jr through a register: a tail call", s.finish(), none,
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                        k.eq("v0", get(c, v0), 42);
                        k.events(rt, {});
                    },
                    {addiu(v0, a0, 1)});
            }
            {
                const Env e = nextEnv();
                Asm s(e.base);
                s.jumpTo(2, e.callee + 8)(nop);
                add("j into the middle of another function (an extra entry)", s.finish(), none,
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0 (before the entry)", get(c, v0), 0);
                        k.eq("v1 (before the entry)", get(c, v1), 0);
                        k.eq("a0", get(c, a0), 3);
                    },
                    {addiu(v0, zero, 1), addiu(v1, zero, 2), addiu(a0, zero, 3)});
            }
            {
                const Env e = nextEnv();
                Asm s(e.base);
                s(addu(t9, ra, zero)).jumpTo(3, e.callee)(nop)(jr(t9))(addiu(v1, zero, 9));
                add("return through a copy of ra", s.finish(), none,
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                        k.eq("v0", get(c, v0), 1);
                        k.eq("v1 (delay slot of the return)", get(c, v1), 9);
                        k.events(rt, {});
                    },
                    {addiu(v0, zero, 1)});
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        void systemCases()
        {
            add("syscall goes to the runtime (number in v1)", {addiu(v1, zero, 0x3c), syscall()}, none,
                [](Checker &k, Ctx &, uint8_t *, Runtime &rt) { k.events(rt, {"syscall 0x3c"}); });
            add("an interrupt-safe syscall's number is negative in v1", {addiu(v1, zero, -0x43), syscall()}, none,
                [](Checker &k, Ctx &, uint8_t *, Runtime &rt) { k.events(rt, {"syscall 0xffffffbd"}); });
            add("break goes to the runtime with its code", {brk(7)}, none,
                [](Checker &k, Ctx &, uint8_t *, Runtime &rt) { k.events(rt, {"break 0x7"}); });
            {
                const Env e = nextEnv();
                add("teq traps when equal, tne does not", {teq(a0, a1), tne(a0, a1)},
                    [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 3); set(c, a1, 3); },
                    [e](Checker &k, Ctx &, uint8_t *, Runtime &rt) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "trap 0x%x", e.base);
                        k.events(rt, {buf});
                    });
            }
            add("sync, pref and cache do nothing", {sync, pref(0, 0, a0), cache(0x18, 0, a0), addiu(v0, zero, 1)}, none,
                [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                    k.eq("v0", get(c, v0), 1);
                    k.events(rt, {});
                });
            add("mfc0/mtc0 and ei/di go to the runtime", {mfc0(v0, 12), mtc0(a0, 12), di, ei, di},
                [](Ctx &c, uint8_t *, Runtime &rt) { rt.cop0[12] = 0x70030C13u; set(c, a0, 0x1234); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                    k.eq("v0", get(c, v0), 0x70030C13u);
                    k.eq("cop0[12]", rt.cop0[12], 0x1234);
                    k.eq("interrupts", rt.interruptsEnabled, 0);
                });
            {
                Asm s;
                s.branch(bc0f(), "t")(nop)(addiu(v0, zero, 1)).label("t")(nop);
                add("bc0f reads the runtime's condition", s.finish(), [](Ctx &, uint8_t *, Runtime &rt) { rt.cop0Condition = true; },
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) { k.eq("v0 (not taken)", get(c, v0), 1); });
            }
            for (bool handled : {false, true})
            {
                Asm s;
                s.branch(bc0f(), "done")(addiu(v1, zero, 9))(addiu(v0, zero, 1)).label("done")(nop);
                add(std::string("native completion ") + (handled ? "ready" : "checker fallback"), s.finish(),
                    [handled](Ctx &, uint8_t *, Runtime &rt) { rt.nativeHandled = handled; rt.cop0Condition = false; },
                    [handled](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                        k.eq("native calls", rt.nativeCalls, 1);
                        k.eq("completion branch", get(c, v0), handled ? 1 : 0);
                        k.eq("delay slot", get(c, v1), 9);
                    });
                g_cases.back().nativeOffsets = {0};
            }
            add("mtsab/mtsah set SA in bits; mfsa reads it", {mtsab(a0, 1), mfsa(v0), mtsah(a0, 0), mfsa(v1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0x13); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("mtsab (0x13^1)&15 * 8", get(c, v0), 0x10);
                    k.eq("mtsah (0x13&7) * 16", get(c, v1), 0x30);
                });
        }

        // ---------------------------------------------------------------------------------------------------------
        // FPU: a0..a3 carry the operand bits; results are read back with mfc1 into v0, v1, t0...
        void fpuCase(const std::string &name, std::vector<uint32_t> body, std::vector<uint32_t> in,
                     std::vector<std::pair<int, uint32_t>> wantF, uint32_t wantFlags = 0xFFFFFFFFu)
        {
            std::vector<uint32_t> code;
            for (size_t i = 0; i < in.size(); ++i)
                code.push_back(mtc1(a0 + static_cast<uint32_t>(i), 1 + static_cast<uint32_t>(i))); // f1.. = a0..
            code.insert(code.end(), body.begin(), body.end());
            add(name, code,
                [in](Ctx &c, uint8_t *, Runtime &) {
                    for (size_t i = 0; i < in.size(); ++i)
                        set(c, a0 + static_cast<int>(i), in[i]);
                },
                [wantF, wantFlags](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    for (const auto &[reg, bits] : wantF)
                    {
                        char what[16];
                        std::snprintf(what, sizeof(what), "f%d", reg);
                        k.eq(what, c.fu[reg], bits);
                    }
                    if (wantFlags != 0xFFFFFFFFu)
                        k.eq("fcr31 flags", c.fcr31 & kFlagMask, wantFlags);
                });
        }

        void fpuCases()
        {
            add("mtc1/mfc1 move bits; mfc1 sign-extends", {mtc1(a0, 5), mfc1(v0, 5)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0x80000001u); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("f5", c.fu[5], 0x80000001u);
                    k.eq("v0", get(c, v0), 0xFFFFFFFF80000001ull);
                });
            fpuCase("add.s 1 + 2", {add_s(10, 1, 2)}, {0x3F800000u, 0x40000000u}, {{10, 0x40400000u}}, 0);
            fpuCase("add.s rounds toward zero (1 + 0.75 ulp = 1)", {add_s(10, 1, 2)}, {0x3F800000u, 0x33C00000u}, {{10, 0x3F800000u}});
            fpuCase("sub.s with one guard bit (Q1): 1 - (1 - 2^-24) = 2^-24 exactly", {sub_s(10, 1, 2)}, {0x3F800000u, 0x3F7FFFFFu}, {{10, 0x33800000u}});
            // 1 - 1.5 * 2^-24: exact and truncated 0x3F7FFFFE; no guard bit 1.0; one guard bit 1 - 2^-24.
            fpuCase("sub.s with one guard bit (Q1): 1 - 0.75 ulp = 1 - 2^-24", {sub_s(10, 1, 2)}, {0x3F800000u, 0x33C00000u}, {{10, 0x3F7FFFFFu}});
            // Captured (story #657, mathfManhatDist): -44.654480 - (-2^-20): the subtrahend is past the guard bit.
            fpuCase("sub.s drops an operand below the guard bit (Q1)", {sub_s(10, 1, 2)}, {0xC2329E30u, 0xB5800000u}, {{10, 0xC2329E30u}});
            fpuCase("sub.s exact cancellation gives +0, also for negatives", {sub_s(10, 1, 1), sub_s(11, 2, 2)}, {0x3F800000u, 0xBF800000u},
                    {{10, 0}, {11, 0}});
            fpuCase("add.s of zeros (Q2): -0 + -0 = -0, -0 + +0 = +0", {add_s(10, 1, 1), add_s(11, 1, 2)}, {0x80000000u, 0}, {{10, 0x80000000u}, {11, 0}});
            fpuCase("add.s overflow saturates to +max; FCR31 unchanged (Q7)", {add_s(10, 1, 1)}, {0x7FFFFFFFu}, {{10, 0x7FFFFFFFu}}, 0);
            fpuCase("add.s reaching exponent 255 is not an overflow", {add_s(10, 1, 1)}, {0x7F7FFFFFu}, {{10, 0x7FFFFFFFu}}, 0);
            fpuCase("exponent 255 is an ordinary number: 'Inf' * 0.5", {mul_s(10, 1, 2)}, {0x7F800000u, 0x3F000000u}, {{10, 0x7F000000u}}, 0);
            fpuCase("exponent 255 is an ordinary number: 'NaN' * 0.5", {mul_s(10, 1, 2)}, {0x7FC00000u, 0x3F000000u}, {{10, 0x7F400000u}}, 0);
            fpuCase("mul.s overflow saturates with the product's sign; FCR31 unchanged", {mul_s(10, 1, 2)}, {0x7F000000u, 0xC0800000u}, {{10, 0xFFFFFFFFu}}, 0);
            fpuCase("denormal operands are zero", {mul_s(10, 1, 2), add_s(11, 1, 1)}, {0x00400000u, 0x40000000u}, {{10, 0}, {11, 0}});
            fpuCase("mul.s underflow flushes to zero; FCR31 unchanged", {mul_s(10, 1, 2)}, {0x00800000u, 0x3F000000u}, {{10, 0}}, 0);
            fpuCase("mul.s rounds to nearest (Q10): 1.5 * (1.5 + 2^-23) = 2.25 + 0.75 ulp", {mul_s(10, 1, 2)}, {0x3FC00000u, 0x3FC00001u}, {{10, 0x40100001u}});
            // (1 + u) * 1.5 = 1.5 + 1.5 u, (1 + 3u) * 1.5 = 1.5 + 4.5 u (u = 2^-23): halves go to the even neighbour.
            fpuCase("mul.s ties go to even", {mul_s(10, 1, 2), mul_s(11, 3, 2)}, {0x3F800001u, 0x3FC00000u, 0x3F800003u},
                    {{10, 0x3FC00002u}, {11, 0x3FC00004u}});
            fpuCase("mul.s underflow keeps the sign", {mul_s(10, 1, 2)}, {0x80800000u, 0x3F000000u}, {{10, 0x80000000u}});
            fpuCase("mul.s 1.5 * -2.5", {mul_s(10, 1, 2)}, {0x3FC00000u, 0xC0200000u}, {{10, 0xC0700000u}}, 0);
            fpuCase("div.s rounds to nearest (Q3): 1/3, 2/3", {div_s(10, 1, 2), div_s(11, 3, 2)}, {0x3F800000u, 0x40400000u, 0x40000000u},
                    {{10, 0x3EAAAAABu}, {11, 0x3F2AAAABu}}, 0);
            fpuCase("div.s by zero: +max, FCR31 bit 20 only (Q7)", {div_s(10, 1, 2)}, {0x3F800000u, 0}, {{10, 0x7FFFFFFFu}}, kDivZero);
            fpuCase("div.s -1 by zero: -max", {div_s(10, 1, 2)}, {0xBF800000u, 0}, {{10, 0xFFFFFFFFu}}, kDivZero);
            fpuCase("div.s 0/0: max (Q3), FCR31 bit 20", {div_s(10, 1, 2)}, {0, 0}, {{10, 0x7FFFFFFFu}}, kDivZero);
            fpuCase("sqrt.s of 4, 2 and 3 (rounded to nearest)", {sqrt_s(10, 1), sqrt_s(11, 2), sqrt_s(12, 3)}, {0x40800000u, 0x40000000u, 0x40400000u},
                    {{10, 0x40000000u}, {11, 0x3FB504F3u}, {12, 0x3FDDB3D7u}}, 0);
            fpuCase("sqrt.s of -4: sqrt(|x|), FCR31 unchanged (Q3)", {sqrt_s(10, 1)}, {0xC0800000u}, {{10, 0x40000000u}}, 0);
            fpuCase("rsqrt.s 1/sqrt(4)", {rsqrt_s(10, 1, 2)}, {0x3F800000u, 0x40800000u}, {{10, 0x3F000000u}}, 0);
            fpuCase("abs.s / neg.s / mov.s are bit operations", {abs_s(10, 1), neg_s(11, 2), mov_s(12, 1), neg_s(13, 3)},
                    {0xFFC00000u, 0, 0x00000001u}, {{10, 0x7FC00000u}, {11, 0x80000000u}, {12, 0xFFC00000u}, {13, 0x80000001u}});
            fpuCase("cvt.w.s truncates and saturates", {cvt_w_s(10, 1), cvt_w_s(11, 2), cvt_w_s(12, 3), cvt_w_s(13, 4)},
                    {fbits(2.9f), fbits(-2.9f), fbits(3e9f), fbits(-3e9f)},
                    {{10, 2}, {11, 0xFFFFFFFEu}, {12, 0x7FFFFFFFu}, {13, 0x80000000u}});
            fpuCase("cvt.w.s of 0.5 and of a denormal is 0", {cvt_w_s(10, 1), cvt_w_s(11, 2)}, {0x3F000000u, 0x00000005u}, {{10, 0}, {11, 0}});
            fpuCase("cvt.s.w rounds to nearest even past 24 bits (Q9)", {cvt_s_w(10, 1), cvt_s_w(11, 2), cvt_s_w(12, 3), cvt_s_w(13, 4), cvt_s_w(14, 5)},
                    {16777217u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x80000000u, 16777219u},
                    {{10, 0x4B800000u}, {11, 0x4F000000u}, {12, 0xBF800000u}, {13, 0xCF000000u}, {14, 0x4B800002u}});
            fpuCase("max.s / min.s", {max_s(10, 1, 2), min_s(11, 1, 2)}, {fbits(-1.0f), fbits(-2.0f)}, {{10, fbits(-1.0f)}, {11, fbits(-2.0f)}});
            fpuCase("the accumulator: adda, madd, msub, mula, madda, msuba, suba",
                    {adda_s(1, 2), madd_s(10, 2, 3), msub_s(11, 2, 3), mula_s(2, 3), madda_s(1, 2), madd_s(12, 4, 4), msuba_s(1, 1),
                     madd_s(13, 4, 4), suba_s(3, 1), madd_s(14, 4, 4)},
                    {fbits(1.0f), fbits(2.0f), fbits(4.0f), 0},
                    {{10, fbits(11.0f)}, {11, fbits(-5.0f)}, {12, fbits(10.0f)}, {13, fbits(9.0f)}, {14, fbits(3.0f)}});
            {
                // Compares: v0 gets bit i when the i-th compare sets C (read with bc1t).
                struct Cmp
                {
                    uint32_t word;
                    uint32_t a, b;
                    bool want;
                };
                const Cmp cmps[] = {
                    {c_eq_s(1, 2), 0, 0x80000000u, true},          // +0 == -0
                    {c_eq_s(1, 2), 0x00000001u, 0, true},          // a denormal equals zero
                    {c_lt_s(1, 2), fbits(1.0f), fbits(2.0f), true},
                    {c_lt_s(1, 2), fbits(-1.0f), fbits(-2.0f), false},
                    {c_le_s(1, 2), fbits(2.0f), fbits(2.0f), true},
                    {c_lt_s(1, 2), 0x7F800000u, 0x7FFFFFFFu, true}, // exponent 255 values order normally
                    {c_f_s(1, 2), 0, 0, false},
                };
                Asm s;
                uint32_t mask = 0;
                for (size_t i = 0; i < std::size(cmps); ++i)
                {
                    const std::string t = "t" + std::to_string(i);
                    s(lui(a0, cmps[i].a >> 16))(ori(a0, a0, cmps[i].a & 0xFFFF))(mtc1(a0, 1))
                        (lui(a0, cmps[i].b >> 16))(ori(a0, a0, cmps[i].b & 0xFFFF))(mtc1(a0, 2))(cmps[i].word)
                        .branch(bc1f(), t)(nop)(ori(v0, v0, 1u << i))
                        .label(t)(nop);
                    if (cmps[i].want)
                        mask |= 1u << i;
                }
                add("c.eq/c.lt/c.le/c.f with bc1f", s.finish(), none,
                    [mask](Checker &k, Ctx &c, uint8_t *, Runtime &) { k.eq("compare results", get(c, v0), mask); });
            }
            {
                Asm s;
                s(c_eq_s(1, 1)).branch(bc1tl(), "a")(addiu(v0, zero, 1))(addiu(v1, zero, 2))
                    .label("a")(c_f_s(1, 1))
                    .branch(bc1tl(), "b")(addiu(t0, zero, 3))
                    .label("b")(nop);
                add("bc1tl taken runs its delay slot; not taken skips it", s.finish(), none,
                    [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                        k.eq("v0", get(c, v0), 1);
                        k.eq("v1", get(c, v1), 0);
                        k.eq("t0", get(c, t0), 0);
                    });
            }
            add("lwc1/swc1 move bits through memory", {lwc1(3, 0, a0), swc1(3, 4, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) { set(c, a0, 0x3800); wr32(ram, 0x3800, 0x7FC00001u); },
                [](Checker &k, Ctx &c, uint8_t *ram, Runtime &) {
                    k.eq("f3", c.fu[3], 0x7FC00001u);
                    k.eq("stored", rd32(ram, 0x3804), 0x7FC00001u);
                });
        }

        // ---------------------------------------------------------------------------------------------------------
        void setQ(Ctx &c, int n, uint64_t lo, uint64_t hi)
        {
            c.r[n].ud[0] = lo;
            c.r[n].ud[1] = hi;
        }
        void eqQ(Checker &k, const char *what, const Ctx &c, int n, uint64_t lo, uint64_t hi)
        {
            k.eq((std::string(what) + " lo").c_str(), c.r[n].ud[0], lo);
            k.eq((std::string(what) + " hi").c_str(), c.r[n].ud[1], hi);
        }

        void mmiCases()
        {
            add("paddw / psubw wrap per word", {paddw(v0, a0, a1), psubw(v1, a0, a1)},
                [](Ctx &c, uint8_t *, Runtime &) { setQ(c, a0, 0x00000001FFFFFFFFull, 0x800000007FFFFFFFull); setQ(c, a1, 0x0000000100000001ull, 0x0000000100000001ull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqQ(k, "paddw", c, v0, 0x0000000200000000ull, 0x8000000180000000ull);
                    eqQ(k, "psubw", c, v1, 0x00000000FFFFFFFEull, 0x7FFFFFFF7FFFFFFEull);
                });
            add("paddh / psubb / paddsw / padduw", {paddh(v0, a0, a1), psubb(v1, a0, a1), paddsw(t0, a2, a3), padduw(t1, a2, a3)},
                [](Ctx &c, uint8_t *, Runtime &) {
                    setQ(c, a0, 0xFFFF000100020003ull, 0);
                    setQ(c, a1, 0x0001000100010001ull, 0);
                    setQ(c, a2, 0x7FFFFFFF80000000ull, 0xFFFFFFF000000005ull);
                    setQ(c, a3, 0x00000001FFFFFFFFull, 0x0000002000000005ull);
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqQ(k, "paddh", c, v0, 0x0000000200030004ull, 0);
                    eqQ(k, "psubb", c, v1, 0xFFFE000000010002ull, 0);
                    eqQ(k, "paddsw", c, t0, 0x7FFFFFFF80000000ull, 0x000000100000000Aull);
                    eqQ(k, "padduw", c, t1, 0x80000000FFFFFFFFull, 0xFFFFFFFF0000000Aull);
                });
            add("pextlw / pextlb / pextub / ppacb", {pextlw(v0, a0, a1), pextlb(v1, a0, a1), pextub(t0, a0, a1), ppacb(t1, a0, a1)},
                [](Ctx &c, uint8_t *, Runtime &) {
                    setQ(c, a0, 0x0706050403020100ull, 0x0F0E0D0C0B0A0908ull);
                    setQ(c, a1, 0x1716151413121110ull, 0x1F1E1D1C1B1A1918ull);
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqQ(k, "pextlw", c, v0, 0x0302010013121110ull, 0x0706050417161514ull);
                    eqQ(k, "pextlb", c, v1, 0x0313021201110010ull, 0x0717061605150414ull);
                    eqQ(k, "pextub", c, t0, 0x0B1B0A1A09190818ull, 0x0F1F0E1E0D1D0C1Cull);
                    eqQ(k, "ppacb", c, t1, 0x1E1C1A1816141210ull, 0x0E0C0A0806040200ull);
                });
            add("pcpyld / pcpyud / pcpyh / prot3w", {pcpyld(v0, a0, a1), pcpyud(v1, a0, a1), pcpyh(t0, a0), prot3w(t1, a0)},
                [](Ctx &c, uint8_t *, Runtime &) {
                    setQ(c, a0, 0x1111222233334444ull, 0x5555666677778888ull);
                    setQ(c, a1, 0xAAAABBBBCCCCDDDDull, 0xEEEEFFFF00001111ull);
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqQ(k, "pcpyld", c, v0, 0xAAAABBBBCCCCDDDDull, 0x1111222233334444ull);
                    eqQ(k, "pcpyud", c, v1, 0x5555666677778888ull, 0xEEEEFFFF00001111ull);
                    eqQ(k, "pcpyh", c, t0, 0x4444444444444444ull, 0x8888888888888888ull);
                    eqQ(k, "prot3w", c, t1, 0x7777888811112222ull, 0x5555666633334444ull);
                });
            add("pand / por / pxor / pnor on 128 bits", {pand(v0, a0, a1), por(v1, a0, a1), pxor(t0, a0, a1), pnor(t1, a0, a1)},
                [](Ctx &c, uint8_t *, Runtime &) { setQ(c, a0, 0xFF00FF00FF00FF00ull, 0xF0F0F0F0F0F0F0F0ull); setQ(c, a1, 0x0FF00FF00FF00FF0ull, 0xFFFFFFFF00000000ull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqQ(k, "pand", c, v0, 0x0F000F000F000F00ull, 0xF0F0F0F000000000ull);
                    eqQ(k, "por", c, v1, 0xFFF0FFF0FFF0FFF0ull, 0xFFFFFFFFF0F0F0F0ull);
                    eqQ(k, "pxor", c, t0, 0xF0F0F0F0F0F0F0F0ull, 0x0F0F0F0FF0F0F0F0ull);
                    eqQ(k, "pnor", c, t1, 0x000F000F000F000Full, 0x000000000F0F0F0Full);
                });
            add("psllh / psrlh / psrah / psllw", {psllh(v0, a0, 4), psrlh(v1, a0, 4), psrah(t0, a0, 4), psllw(t1, a0, 8)},
                [](Ctx &c, uint8_t *, Runtime &) { setQ(c, a0, 0x8001F00F12340FF0ull, 0); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqQ(k, "psllh", c, v0, 0x001000F02340FF00ull, 0);
                    eqQ(k, "psrlh", c, v1, 0x08000F00012300FFull, 0);
                    eqQ(k, "psrah", c, t0, 0xF800FF00012300FFull, 0);
                    eqQ(k, "psllw", c, t1, 0x01F00F00340FF000ull, 0);
                });
            add("pcgth / pmaxh / pminh / pceqw", {pcgth(v0, a0, a1), pmaxh(v1, a0, a1), pminh(t0, a0, a1), pceqw(t1, a0, a1)},
                [](Ctx &c, uint8_t *, Runtime &) { setQ(c, a0, 0x8000000100050003ull, 0x1111111122222222ull); setQ(c, a1, 0x7FFF000100040003ull, 0x1111111122222223ull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqQ(k, "pcgth", c, v0, 0x00000000FFFF0000ull, 0);
                    eqQ(k, "pmaxh", c, v1, 0x7FFF000100050003ull, 0x1111111122222223ull);
                    eqQ(k, "pminh", c, t0, 0x8000000100040003ull, 0x1111111122222222ull);
                    eqQ(k, "pceqw", c, t1, 0x0000000000000000ull, 0xFFFFFFFF00000000ull);
                });
            add("qfsrv with mtsab: an unaligned 128-bit load", {lq(t0, 0, a0), lq(t1, 16, a0), mtsab(a1, 0), qfsrv(v0, t1, t0)},
                [](Ctx &c, uint8_t *ram, Runtime &) {
                    set(c, a0, 0x3900);
                    set(c, a1, 0x3903);
                    for (int i = 0; i < 32; ++i)
                        ram[0x3900 + i] = static_cast<uint8_t>(i);
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) { eqQ(k, "qfsrv", c, v0, 0x0A09080706050403ull, 0x1211100F0E0D0C0Bull); });
            add("pmfhi / pmflo / pmthi: HI1:HI and LO1:LO", {mult(zero, a0, a1), mult1(zero, a1, a1), pmfhi(v0), pmflo(v1), pmthi(a2), mfhi(t0), mfhi1(t1)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 0x10000); set(c, a1, 0x30000); setQ(c, a2, 5, 6); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqQ(k, "pmfhi", c, v0, 3, 9);
                    eqQ(k, "pmflo", c, v1, 0, 0);
                    k.eq("hi", get(c, t0), 5);
                    k.eq("hi1", get(c, t1), 6);
                });
            add("plzcw counts leading sign bits minus one", {plzcw(v0, a0)},
                [](Ctx &c, uint8_t *, Runtime &) { setQ(c, a0, 0xFFFFFFFF00000001ull, 0); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) { k.eq("words", get(c, v0), (31ull << 32) | 30ull); });
        }

        // ---------------------------------------------------------------------------------------------------------
        void setV(Ctx &c, int n, float x, float y, float z, float w)
        {
            c.vu0_vf[n].u[0] = fbits(x);
            c.vu0_vf[n].u[1] = fbits(y);
            c.vu0_vf[n].u[2] = fbits(z);
            c.vu0_vf[n].u[3] = fbits(w);
        }
        void eqV(Checker &k, const char *what, const Ctx &c, int n, float x, float y, float z, float w)
        {
            const float want[4] = {x, y, z, w};
            for (int i = 0; i < 4; ++i)
            {
                const std::string field = std::string(what) + "." + "xyzw"[i];
                k.eq(field.c_str(), c.vu0_vf[n].u[i], fbits(want[i]));
            }
        }

        void vu0Cases()
        {
            add("qmtc2 / qmfc2 move 128 bits; vf0 cannot be written", {qmtc2(a0, 5), qmfc2(v0, 5), qmtc2(a0, 0)},
                [](Ctx &c, uint8_t *, Runtime &) { setQ(c, a0, 0x1111111122222222ull, 0x3333333344444444ull); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqQ(k, "v0", c, v0, 0x1111111122222222ull, 0x3333333344444444ull);
                    eqV(k, "vf0", c, 0, 0, 0, 0, 1);
                });
            add("lqc2 / sqc2", {lqc2(7, 0x10, a0), sqc2(7, 0x20, a0)},
                [](Ctx &c, uint8_t *ram, Runtime &) { set(c, a0, 0x3A00); wr64(ram, 0x3A10, 0x4000000040400000ull); wr64(ram, 0x3A18, 0x3F80000040800000ull); },
                [](Checker &k, Ctx &c, uint8_t *ram, Runtime &) {
                    eqV(k, "vf7", c, 7, 3.0f, 2.0f, 4.0f, 1.0f);
                    k.eq("stored", rd64(ram, 0x3A28), 0x3F80000040800000ull);
                });
            add("vadd / vsub / vmul, all fields", {vadd(XYZW, 3, 1, 2), vsub(XYZW, 4, 1, 2), vmul(XYZW, 5, 1, 2)},
                [](Ctx &c, uint8_t *, Runtime &) { setV(c, 1, 1, 2, 3, 4); setV(c, 2, 0.5f, -1, 2, 8); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqV(k, "vadd", c, 3, 1.5f, 1, 5, 12);
                    eqV(k, "vsub", c, 4, 0.5f, 3, 1, -4);
                    eqV(k, "vmul", c, 5, 0.5f, -2, 6, 32);
                });
            add("vaddbc writes only its dest fields", {vaddbc(X | Z, 3, 1, 2, 3)},
                [](Ctx &c, uint8_t *, Runtime &) { setV(c, 1, 1, 2, 3, 4); setV(c, 2, 0, 0, 0, 10); setV(c, 3, 7, 7, 7, 7); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) { eqV(k, "vf3", c, 3, 11, 7, 13, 7); });
            add("vmulabc / vmaddabc / vmaddbc: a matrix times a vector",
                {vmulabc(XYZW, 1, 5, 0), vmaddabc(XYZW, 2, 5, 1), vmaddbc(XYZW, 6, 3, 5, 2)},
                [](Ctx &c, uint8_t *, Runtime &) {
                    setV(c, 1, 1, 0, 0, 0); // columns of a matrix
                    setV(c, 2, 0, 2, 0, 0);
                    setV(c, 3, 0, 0, 3, 1);
                    setV(c, 5, 4, 5, 6, 1); // the vector
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) { eqV(k, "result", c, 6, 4, 10, 18, 6); });
            add("vmula / vmadda / vmadd / vmsub", {vmula(XYZW, 1, 2), vmadda(XYZW, 1, 1), vmadd(XYZW, 3, 2, 2), vmsub(XYZW, 4, 2, 2)},
                [](Ctx &c, uint8_t *, Runtime &) { setV(c, 1, 1, 2, 3, 4); setV(c, 2, 2, 2, 2, 2); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    // acc = vf1*vf2 + vf1*vf1 = (3, 8, 15, 24)
                    eqV(k, "vmadd", c, 3, 7, 12, 19, 28);
                    eqV(k, "vmsub", c, 4, -1, 4, 11, 20);
                });
            add("vopmula / vopmsub: the cross product", {vopmula(1, 2), vopmsub(3, 2, 1)},
                [](Ctx &c, uint8_t *, Runtime &) { setV(c, 1, 1, 2, 3, 9); setV(c, 2, 4, 5, 6, 9); setV(c, 3, 0, 0, 0, 77); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) { eqV(k, "a x b", c, 3, -3, 6, -3, 77); });
            add("vmr32 / vmove / vabs", {vmr32(XYZW, 3, 1), vmove(X | W, 4, 1), vabs(XYZW, 5, 2)},
                [](Ctx &c, uint8_t *, Runtime &) { setV(c, 1, 1, 2, 3, 4); setV(c, 2, -1, 2, -3, -0.0f); setV(c, 4, 9, 9, 9, 9); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqV(k, "vmr32", c, 3, 2, 3, 4, 1);
                    eqV(k, "vmove", c, 4, 1, 9, 9, 4);
                    eqV(k, "vabs", c, 5, 1, 2, 3, 0);
                });
            add("vdiv sets Q; vmulq / vmaddq use it", {vdiv(1, 0, 2, 1), vmulq(XYZW, 3, 1), vmula(XYZW, 1, 0), vmaddq(XYZW, 4, 2)},
                [](Ctx &c, uint8_t *, Runtime &) { setV(c, 1, 6, 1, 2, 3); setV(c, 2, 0, 3, 0, 0); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("Q", c.vu0_q_bits, fbits(2.0f));
                    eqV(k, "vmulq", c, 3, 12, 2, 4, 6);
                    // acc = vf1 * vf0 = (0, 0, 0, 3); vf4 = acc + vf2 * Q
                    eqV(k, "vmaddq", c, 4, 0, 6, 0, 3);
                });
            add("vitof0 / vftoi4", {vitof0(XYZW, 3, 1), vftoi4(XYZW, 4, 2)},
                [](Ctx &c, uint8_t *, Runtime &) {
                    c.vu0_vf[1].u[0] = 3;
                    c.vu0_vf[1].u[1] = static_cast<uint32_t>(-5);
                    c.vu0_vf[1].u[2] = 0;
                    c.vu0_vf[1].u[3] = 16777217;
                    setV(c, 2, 1.5f, -0.03125f, 200000000.0f, -1e30f);
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    eqV(k, "vitof0", c, 3, 3, -5, 0, 16777216.0f);
                    k.eq("vftoi4.x", c.vu0_vf[4].u[0], 24);
                    k.eq("vftoi4.y", c.vu0_vf[4].u[1], 0);
                    k.eq("vftoi4.z (saturated)", c.vu0_vf[4].u[2], 0x7FFFFFFFu);
                    k.eq("vftoi4.w (saturated)", c.vu0_vf[4].u[3], 0x80000000u);
                });
            add("vclipw judges x, y, z against |w|", {vclipw(1, 2), vclipw(3, 2)},
                [](Ctx &c, uint8_t *, Runtime &) { setV(c, 1, 2, -2, 0.5f, 0); setV(c, 2, 0, 0, 0, -1); setV(c, 3, 0, 0, -5, 0); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) { k.eq("clip flags", c.vu0_clip_flags, (9u << 6) | 0x20u); });
            add("ctc2 / cfc2 / viadd / viaddi on the 16-bit VI registers",
                {ctc2(a0, 1), ctc2(a1, 2), viadd(3, 1, 2), viaddi(4, 1, -2), cfc2(v0, 3), cfc2(v1, 4), cfc2(t0, 2)},
                [](Ctx &c, uint8_t *, Runtime &) { set(c, a0, 3); set(c, a1, 0x1FFFF); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("vi3", get(c, v0), 2);
                    k.eq("vi4", get(c, v1), 1);
                    k.eq("vi2", get(c, t0), 0xFFFF);
                });
            add("vlqi / vsqi: VU0 memory, post-increment", {ctc2(a0, 1), ctc2(a1, 2), vlqi(XYZW, 5, 1), vsqi(XYZ, 5, 2), cfc2(v0, 1), cfc2(v1, 2)},
                [](Ctx &c, uint8_t *, Runtime &rt) {
                    set(c, a0, 2);
                    set(c, a1, 7);
                    for (int i = 0; i < 4; ++i)
                        wr32(rt.vu0Memory, 2 * 16 + 4 * i, 0x100u + static_cast<uint32_t>(i));
                    wr32(rt.vu0Memory, 7 * 16 + 12, 0xDEADu);
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                    k.eq("vf5.x", c.vu0_vf[5].u[0], 0x100);
                    k.eq("vf5.w", c.vu0_vf[5].u[3], 0x103);
                    k.eq("vi1", get(c, v0), 3);
                    k.eq("vi2", get(c, v1), 8);
                    k.eq("quad 7 x", rd32(rt.vu0Memory, 7 * 16), 0x100);
                    k.eq("quad 7 w (not in dest)", rd32(rt.vu0Memory, 7 * 16 + 12), 0xDEAD);
                });
            add("VU0's add rounds to nearest (the FPU's truncates)", {vadd(X, 3, 1, 2), vsub(Y, 3, 1, 2)},
                [](Ctx &c, uint8_t *, Runtime &) {
                    c.vu0_vf[1].u[0] = c.vu0_vf[1].u[1] = 0x3F800000u; // 1.0
                    c.vu0_vf[2].u[0] = c.vu0_vf[2].u[1] = 0x33C00000u; // 0.75 ulp of 1.0
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("1 + 0.75 ulp", c.vu0_vf[3].u[0], 0x3F800001u);
                    k.eq("1 - 1.5 * 2^-24, a tie: to even", c.vu0_vf[3].u[1], 0x3F7FFFFEu);
                });
            add("FMAC flags (Q11): MAC Z/S per field, status and sticky; vf0 as destination still sets them",
                {vsub(XYZW, 0, 1, 2), cfc2(v0, 17), cfc2(v1, 16), vadd(X, 3, 1, 1), cfc2(t0, 17), cfc2(t1, 16)},
                [](Ctx &c, uint8_t *, Runtime &) { setV(c, 1, 1, 2, 3, 4); setV(c, 2, 1, 3, 2, 4); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("MAC after (0, -1, 1, 0)", get(c, v0), 0x49);
                    k.eq("status: Z, S and their sticky bits", get(c, v1), 0xC3);
                    eqV(k, "vf0", c, 0, 0, 0, 0, 1);
                    k.eq("MAC after vadd.x (fields outside dest are 0)", get(c, t0), 0);
                    k.eq("status: sticky bits kept", get(c, t1), 0xC0);
                });
            add("FMAC flags: exponent 255 is finite; saturation sets O; max/mini leave the flags",
                {vmul(X, 3, 1, 2), cfc2(v0, 17), cfc2(v1, 16), vmul(Y, 3, 1, 1), cfc2(t0, 17), vmax(XYZW, 4, 2, 2), cfc2(t1, 17)},
                [](Ctx &c, uint8_t *, Runtime &) {
                    c.vu0_vf[1].u[0] = c.vu0_vf[1].u[1] = 0x7F000000u; // 2^127
                    c.vu0_vf[2].u[0] = 0x40000000u;                     // 2
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("2^128 = 0x7F800000", c.vu0_vf[3].u[0], 0x7F800000u);
                    k.eq("MAC: no overflow at 2^128", get(c, v0), 0);
                    k.eq("status: no overflow at 2^128", get(c, v1), 0);
                    k.eq("2^254 saturates", c.vu0_vf[3].u[1], 0x7FFFFFFFu);
                    k.eq("MAC O.y", get(c, t0), 0x4000);
                    k.eq("MAC after vmax", get(c, t1), 0x4000);
                });
            add("FMAC flags: largest finite magnitude sets O, its predecessor does not",
                {vadd(XYZW, 3, 1, 2), cfc2(v0, 17), cfc2(v1, 16)},
                [](Ctx &c, uint8_t *, Runtime &) {
                    c.vu0_vf[1].u[0] = 0x7FFFFFFFu;
                    c.vu0_vf[1].u[1] = 0xFFFFFFFFu;
                    c.vu0_vf[1].u[2] = 0x7FFFFFFEu;
                    c.vu0_vf[1].u[3] = 0xFFFFFFFEu;
                },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("MAC O.xy and S.yw", get(c, v0), 0xC050);
                    k.eq("status S/O and sticky", get(c, v1), 0x28A);
                });
            add("vdiv by zero: Q = max, status D and DS", {vdiv(1, 0, 2, 0), cfc2(v0, 16)},
                [](Ctx &c, uint8_t *, Runtime &) { setV(c, 1, 3, 0, 0, 0); setV(c, 2, 0, 0, 0, 0); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &) {
                    k.eq("Q", c.vu0_q_bits, 0x7FFFFFFFu);
                    k.eq("status", get(c, v0), 0x820);
                });
            add("vcallms, cfc2 VPU-STAT, ctc2 FBRST go to the runtime; vnop", {vnop, vcallms(0x278), cfc2(v0, 29), ctc2(a0, 28)},
                [](Ctx &c, uint8_t *, Runtime &rt) { rt.vpuStat = 0x100; set(c, a0, 2); },
                [](Checker &k, Ctx &c, uint8_t *, Runtime &rt) {
                    k.eq("vpu-stat", get(c, v0), 0x100);
                    k.events(rt, {"callms 0x278", "ctc2 28 0x2"});
                });
        }

        void build()
        {
            integerCases();
            multiplyDivideCases();
            memoryCases();
            branchCases();
            callCases();
            systemCases();
            fpuCases();
            mmiCases();
            vu0Cases();
        }
    }

    const std::vector<Case> &allCases()
    {
        if (g_cases.empty())
            build();
        return g_cases;
    }
}
