// A tiny R5900 assembler for the tests: instruction words from their fields (encodings as in r5900.md), and a builder
// with labels for branches. Register numbers are plain integers (r0..r31); the usual names are below.
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace tasm
{
    enum Reg : uint32_t
    {
        zero = 0, at = 1, v0 = 2, v1 = 3, a0 = 4, a1 = 5, a2 = 6, a3 = 7, t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12,
        t5 = 13, t6 = 14, t7 = 15, s0 = 16, s1 = 17, s2 = 18, s3 = 19, s4 = 20, s5 = 21, s6 = 22, s7 = 23, t8 = 24,
        t9 = 25, k0 = 26, k1 = 27, gp = 28, sp = 29, fp = 30, ra = 31,
    };

    constexpr uint32_t R(uint32_t op, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t fn)
    {
        return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn;
    }
    constexpr uint32_t I(uint32_t op, uint32_t rs, uint32_t rt, int32_t imm)
    {
        return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<uint32_t>(imm) & 0xFFFFu);
    }
    constexpr uint32_t J(uint32_t op, uint32_t target) { return (op << 26) | ((target >> 2) & 0x3FFFFFFu); }

    // ---- SPECIAL ----
    constexpr uint32_t nop = 0;
    constexpr uint32_t sll(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, 0, rt, rd, sa, 0); }
    constexpr uint32_t srl(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, 0, rt, rd, sa, 2); }
    constexpr uint32_t sra(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, 0, rt, rd, sa, 3); }
    constexpr uint32_t sllv(uint32_t rd, uint32_t rt, uint32_t rs) { return R(0, rs, rt, rd, 0, 4); }
    constexpr uint32_t srlv(uint32_t rd, uint32_t rt, uint32_t rs) { return R(0, rs, rt, rd, 0, 6); }
    constexpr uint32_t srav(uint32_t rd, uint32_t rt, uint32_t rs) { return R(0, rs, rt, rd, 0, 7); }
    constexpr uint32_t jr(uint32_t rs) { return R(0, rs, 0, 0, 0, 8); }
    constexpr uint32_t jalr(uint32_t rd, uint32_t rs) { return R(0, rs, 0, rd, 0, 9); }
    constexpr uint32_t movz(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 10); }
    constexpr uint32_t movn(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 11); }
    constexpr uint32_t syscall(uint32_t code = 0) { return (code << 6) | 12; }
    constexpr uint32_t brk(uint32_t code = 0) { return (code << 6) | 13; }
    constexpr uint32_t sync = 15;
    constexpr uint32_t mfhi(uint32_t rd) { return R(0, 0, 0, rd, 0, 16); }
    constexpr uint32_t mthi(uint32_t rs) { return R(0, rs, 0, 0, 0, 17); }
    constexpr uint32_t mflo(uint32_t rd) { return R(0, 0, 0, rd, 0, 18); }
    constexpr uint32_t mtlo(uint32_t rs) { return R(0, rs, 0, 0, 0, 19); }
    constexpr uint32_t dsllv(uint32_t rd, uint32_t rt, uint32_t rs) { return R(0, rs, rt, rd, 0, 20); }
    constexpr uint32_t dsrlv(uint32_t rd, uint32_t rt, uint32_t rs) { return R(0, rs, rt, rd, 0, 22); }
    constexpr uint32_t dsrav(uint32_t rd, uint32_t rt, uint32_t rs) { return R(0, rs, rt, rd, 0, 23); }
    constexpr uint32_t mult(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 24); }
    constexpr uint32_t multu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 25); }
    constexpr uint32_t div(uint32_t rs, uint32_t rt) { return R(0, rs, rt, 0, 0, 26); }
    constexpr uint32_t divu(uint32_t rs, uint32_t rt) { return R(0, rs, rt, 0, 0, 27); }
    constexpr uint32_t add(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 32); }
    constexpr uint32_t addu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 33); }
    constexpr uint32_t sub(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 34); }
    constexpr uint32_t subu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 35); }
    constexpr uint32_t and_(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 36); }
    constexpr uint32_t or_(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 37); }
    constexpr uint32_t xor_(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 38); }
    constexpr uint32_t nor(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 39); }
    constexpr uint32_t mfsa(uint32_t rd) { return R(0, 0, 0, rd, 0, 40); }
    constexpr uint32_t mtsa(uint32_t rs) { return R(0, rs, 0, 0, 0, 41); }
    constexpr uint32_t slt(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 42); }
    constexpr uint32_t sltu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 43); }
    constexpr uint32_t dadd(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 44); }
    constexpr uint32_t daddu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 45); }
    constexpr uint32_t dsubu(uint32_t rd, uint32_t rs, uint32_t rt) { return R(0, rs, rt, rd, 0, 47); }
    constexpr uint32_t teq(uint32_t rs, uint32_t rt) { return R(0, rs, rt, 0, 0, 52); }
    constexpr uint32_t tne(uint32_t rs, uint32_t rt) { return R(0, rs, rt, 0, 0, 54); }
    constexpr uint32_t dsll(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, 0, rt, rd, sa, 56); }
    constexpr uint32_t dsrl(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, 0, rt, rd, sa, 58); }
    constexpr uint32_t dsra(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, 0, rt, rd, sa, 59); }
    constexpr uint32_t dsll32(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, 0, rt, rd, sa, 60); }
    constexpr uint32_t dsrl32(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, 0, rt, rd, sa, 62); }
    constexpr uint32_t dsra32(uint32_t rd, uint32_t rt, uint32_t sa) { return R(0, 0, rt, rd, sa, 63); }

    // ---- REGIMM ----
    constexpr uint32_t mtsab(uint32_t rs, uint32_t imm) { return I(1, rs, 24, static_cast<int32_t>(imm)); }
    constexpr uint32_t mtsah(uint32_t rs, uint32_t imm) { return I(1, rs, 25, static_cast<int32_t>(imm)); }

    // ---- Immediate ----
    constexpr uint32_t addi(uint32_t rt, uint32_t rs, int32_t imm) { return I(8, rs, rt, imm); }
    constexpr uint32_t addiu(uint32_t rt, uint32_t rs, int32_t imm) { return I(9, rs, rt, imm); }
    constexpr uint32_t slti(uint32_t rt, uint32_t rs, int32_t imm) { return I(10, rs, rt, imm); }
    constexpr uint32_t sltiu(uint32_t rt, uint32_t rs, int32_t imm) { return I(11, rs, rt, imm); }
    constexpr uint32_t andi(uint32_t rt, uint32_t rs, uint32_t imm) { return I(12, rs, rt, static_cast<int32_t>(imm)); }
    constexpr uint32_t ori(uint32_t rt, uint32_t rs, uint32_t imm) { return I(13, rs, rt, static_cast<int32_t>(imm)); }
    constexpr uint32_t xori(uint32_t rt, uint32_t rs, uint32_t imm) { return I(14, rs, rt, static_cast<int32_t>(imm)); }
    constexpr uint32_t lui(uint32_t rt, uint32_t imm) { return I(15, 0, rt, static_cast<int32_t>(imm)); }
    constexpr uint32_t daddiu(uint32_t rt, uint32_t rs, int32_t imm) { return I(25, rs, rt, imm); }
    // Loads and stores: op rt, offset(base).
    constexpr uint32_t ldl(uint32_t rt, int32_t off, uint32_t b) { return I(26, b, rt, off); }
    constexpr uint32_t ldr(uint32_t rt, int32_t off, uint32_t b) { return I(27, b, rt, off); }
    constexpr uint32_t lq(uint32_t rt, int32_t off, uint32_t b) { return I(30, b, rt, off); }
    constexpr uint32_t sq(uint32_t rt, int32_t off, uint32_t b) { return I(31, b, rt, off); }
    constexpr uint32_t lb(uint32_t rt, int32_t off, uint32_t b) { return I(32, b, rt, off); }
    constexpr uint32_t lh(uint32_t rt, int32_t off, uint32_t b) { return I(33, b, rt, off); }
    constexpr uint32_t lwl(uint32_t rt, int32_t off, uint32_t b) { return I(34, b, rt, off); }
    constexpr uint32_t lw(uint32_t rt, int32_t off, uint32_t b) { return I(35, b, rt, off); }
    constexpr uint32_t lbu(uint32_t rt, int32_t off, uint32_t b) { return I(36, b, rt, off); }
    constexpr uint32_t lhu(uint32_t rt, int32_t off, uint32_t b) { return I(37, b, rt, off); }
    constexpr uint32_t lwr(uint32_t rt, int32_t off, uint32_t b) { return I(38, b, rt, off); }
    constexpr uint32_t lwu(uint32_t rt, int32_t off, uint32_t b) { return I(39, b, rt, off); }
    constexpr uint32_t sb(uint32_t rt, int32_t off, uint32_t b) { return I(40, b, rt, off); }
    constexpr uint32_t sh(uint32_t rt, int32_t off, uint32_t b) { return I(41, b, rt, off); }
    constexpr uint32_t swl(uint32_t rt, int32_t off, uint32_t b) { return I(42, b, rt, off); }
    constexpr uint32_t sw(uint32_t rt, int32_t off, uint32_t b) { return I(43, b, rt, off); }
    constexpr uint32_t sdl(uint32_t rt, int32_t off, uint32_t b) { return I(44, b, rt, off); }
    constexpr uint32_t sdr(uint32_t rt, int32_t off, uint32_t b) { return I(45, b, rt, off); }
    constexpr uint32_t swr(uint32_t rt, int32_t off, uint32_t b) { return I(46, b, rt, off); }
    constexpr uint32_t cache(uint32_t op, int32_t off, uint32_t b) { return I(47, b, op, off); }
    constexpr uint32_t lwc1(uint32_t ft, int32_t off, uint32_t b) { return I(49, b, ft, off); }
    constexpr uint32_t pref(uint32_t hint, int32_t off, uint32_t b) { return I(51, b, hint, off); }
    constexpr uint32_t lqc2(uint32_t ft, int32_t off, uint32_t b) { return I(54, b, ft, off); }
    constexpr uint32_t ld(uint32_t rt, int32_t off, uint32_t b) { return I(55, b, rt, off); }
    constexpr uint32_t swc1(uint32_t ft, int32_t off, uint32_t b) { return I(57, b, ft, off); }
    constexpr uint32_t sqc2(uint32_t ft, int32_t off, uint32_t b) { return I(62, b, ft, off); }
    constexpr uint32_t sd(uint32_t rt, int32_t off, uint32_t b) { return I(63, b, rt, off); }

    // ---- MMI (opcode 28) ----
    constexpr uint32_t mmi(uint32_t rd, uint32_t rs, uint32_t rt, uint32_t sa, uint32_t fn) { return R(28, rs, rt, rd, sa, fn); }
    constexpr uint32_t madd(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi(rd, rs, rt, 0, 0); }
    constexpr uint32_t plzcw(uint32_t rd, uint32_t rs) { return mmi(rd, rs, 0, 0, 4); }
    constexpr uint32_t mfhi1(uint32_t rd) { return mmi(rd, 0, 0, 0, 16); }
    constexpr uint32_t mflo1(uint32_t rd) { return mmi(rd, 0, 0, 0, 18); }
    constexpr uint32_t mult1(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi(rd, rs, rt, 0, 24); }
    constexpr uint32_t div1(uint32_t rs, uint32_t rt) { return mmi(0, rs, rt, 0, 26); }
    constexpr uint32_t psllh(uint32_t rd, uint32_t rt, uint32_t sa) { return mmi(rd, 0, rt, sa, 52); }
    constexpr uint32_t psrlh(uint32_t rd, uint32_t rt, uint32_t sa) { return mmi(rd, 0, rt, sa, 54); }
    constexpr uint32_t psrah(uint32_t rd, uint32_t rt, uint32_t sa) { return mmi(rd, 0, rt, sa, 55); }
    constexpr uint32_t psllw(uint32_t rd, uint32_t rt, uint32_t sa) { return mmi(rd, 0, rt, sa, 60); }
    constexpr uint32_t mmi0(uint32_t sub, uint32_t rd, uint32_t rs, uint32_t rt) { return mmi(rd, rs, rt, sub, 8); }
    constexpr uint32_t mmi1(uint32_t sub, uint32_t rd, uint32_t rs, uint32_t rt) { return mmi(rd, rs, rt, sub, 40); }
    constexpr uint32_t mmi2(uint32_t sub, uint32_t rd, uint32_t rs, uint32_t rt) { return mmi(rd, rs, rt, sub, 9); }
    constexpr uint32_t mmi3(uint32_t sub, uint32_t rd, uint32_t rs, uint32_t rt) { return mmi(rd, rs, rt, sub, 41); }
    constexpr uint32_t paddw(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(0, rd, rs, rt); }
    constexpr uint32_t psubw(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(1, rd, rs, rt); }
    constexpr uint32_t pmaxh(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(7, rd, rs, rt); }
    constexpr uint32_t paddh(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(4, rd, rs, rt); }
    constexpr uint32_t pcgth(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(6, rd, rs, rt); }
    constexpr uint32_t psubb(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(9, rd, rs, rt); }
    constexpr uint32_t paddsw(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(16, rd, rs, rt); }
    constexpr uint32_t pextlw(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(18, rd, rs, rt); }
    constexpr uint32_t pextlb(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(26, rd, rs, rt); }
    constexpr uint32_t ppacb(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi0(27, rd, rs, rt); }
    constexpr uint32_t pminh(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi1(7, rd, rs, rt); }
    constexpr uint32_t pceqw(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi1(2, rd, rs, rt); }
    constexpr uint32_t padduw(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi1(16, rd, rs, rt); }
    constexpr uint32_t pextub(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi1(26, rd, rs, rt); }
    constexpr uint32_t qfsrv(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi1(27, rd, rs, rt); }
    constexpr uint32_t pmfhi(uint32_t rd) { return mmi2(8, rd, 0, 0); }
    constexpr uint32_t pmflo(uint32_t rd) { return mmi2(9, rd, 0, 0); }
    constexpr uint32_t pcpyld(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi2(14, rd, rs, rt); }
    constexpr uint32_t pand(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi2(18, rd, rs, rt); }
    constexpr uint32_t pxor(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi2(19, rd, rs, rt); }
    constexpr uint32_t prot3w(uint32_t rd, uint32_t rt) { return mmi2(31, rd, 0, rt); }
    constexpr uint32_t pmthi(uint32_t rs) { return mmi3(8, 0, rs, 0); }
    constexpr uint32_t pcpyud(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi3(14, rd, rs, rt); }
    constexpr uint32_t por(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi3(18, rd, rs, rt); }
    constexpr uint32_t pnor(uint32_t rd, uint32_t rs, uint32_t rt) { return mmi3(19, rd, rs, rt); }
    constexpr uint32_t pcpyh(uint32_t rd, uint32_t rt) { return mmi3(27, rd, 0, rt); }

    // ---- COP0 ----
    constexpr uint32_t mfc0(uint32_t rt, uint32_t rd) { return R(16, 0, rt, rd, 0, 0); }
    constexpr uint32_t mtc0(uint32_t rt, uint32_t rd) { return R(16, 4, rt, rd, 0, 0); }
    constexpr uint32_t ei = R(16, 16, 0, 0, 0, 56);
    constexpr uint32_t di = R(16, 16, 0, 0, 0, 57);

    // ---- COP1 ----
    constexpr uint32_t mfc1(uint32_t rt, uint32_t fs) { return R(17, 0, rt, fs, 0, 0); }
    constexpr uint32_t cfc1(uint32_t rt, uint32_t fs) { return R(17, 2, rt, fs, 0, 0); }
    constexpr uint32_t mtc1(uint32_t rt, uint32_t fs) { return R(17, 4, rt, fs, 0, 0); }
    constexpr uint32_t ctc1(uint32_t rt, uint32_t fs) { return R(17, 6, rt, fs, 0, 0); }
    constexpr uint32_t fpuS(uint32_t fn, uint32_t fd, uint32_t fs, uint32_t ft) { return R(17, 16, ft, fs, fd, fn); }
    constexpr uint32_t add_s(uint32_t fd, uint32_t fs, uint32_t ft) { return fpuS(0, fd, fs, ft); }
    constexpr uint32_t sub_s(uint32_t fd, uint32_t fs, uint32_t ft) { return fpuS(1, fd, fs, ft); }
    constexpr uint32_t mul_s(uint32_t fd, uint32_t fs, uint32_t ft) { return fpuS(2, fd, fs, ft); }
    constexpr uint32_t div_s(uint32_t fd, uint32_t fs, uint32_t ft) { return fpuS(3, fd, fs, ft); }
    constexpr uint32_t sqrt_s(uint32_t fd, uint32_t ft) { return fpuS(4, fd, 0, ft); }
    constexpr uint32_t abs_s(uint32_t fd, uint32_t fs) { return fpuS(5, fd, fs, 0); }
    constexpr uint32_t mov_s(uint32_t fd, uint32_t fs) { return fpuS(6, fd, fs, 0); }
    constexpr uint32_t neg_s(uint32_t fd, uint32_t fs) { return fpuS(7, fd, fs, 0); }
    constexpr uint32_t rsqrt_s(uint32_t fd, uint32_t fs, uint32_t ft) { return fpuS(22, fd, fs, ft); }
    constexpr uint32_t adda_s(uint32_t fs, uint32_t ft) { return fpuS(24, 0, fs, ft); }
    constexpr uint32_t suba_s(uint32_t fs, uint32_t ft) { return fpuS(25, 0, fs, ft); }
    constexpr uint32_t mula_s(uint32_t fs, uint32_t ft) { return fpuS(26, 0, fs, ft); }
    constexpr uint32_t madd_s(uint32_t fd, uint32_t fs, uint32_t ft) { return fpuS(28, fd, fs, ft); }
    constexpr uint32_t msub_s(uint32_t fd, uint32_t fs, uint32_t ft) { return fpuS(29, fd, fs, ft); }
    constexpr uint32_t madda_s(uint32_t fs, uint32_t ft) { return fpuS(30, 0, fs, ft); }
    constexpr uint32_t msuba_s(uint32_t fs, uint32_t ft) { return fpuS(31, 0, fs, ft); }
    constexpr uint32_t cvt_w_s(uint32_t fd, uint32_t fs) { return fpuS(36, fd, fs, 0); }
    constexpr uint32_t max_s(uint32_t fd, uint32_t fs, uint32_t ft) { return fpuS(40, fd, fs, ft); }
    constexpr uint32_t min_s(uint32_t fd, uint32_t fs, uint32_t ft) { return fpuS(41, fd, fs, ft); }
    constexpr uint32_t c_f_s(uint32_t fs, uint32_t ft) { return fpuS(48, 0, fs, ft); }
    constexpr uint32_t c_eq_s(uint32_t fs, uint32_t ft) { return fpuS(50, 0, fs, ft); }
    constexpr uint32_t c_lt_s(uint32_t fs, uint32_t ft) { return fpuS(52, 0, fs, ft); }
    constexpr uint32_t c_le_s(uint32_t fs, uint32_t ft) { return fpuS(54, 0, fs, ft); }
    constexpr uint32_t cvt_s_w(uint32_t fd, uint32_t fs) { return R(17, 20, 0, fs, fd, 32); }

    // ---- COP2 (VU0 macro) ----
    constexpr uint32_t X = 8, Y = 4, Z = 2, W = 1, XYZ = 14, XYZW = 15;
    constexpr uint32_t qmfc2(uint32_t rt, uint32_t vf) { return R(18, 1, rt, vf, 0, 0); }
    constexpr uint32_t cfc2(uint32_t rt, uint32_t cr) { return R(18, 2, rt, cr, 0, 0); }
    constexpr uint32_t qmtc2(uint32_t rt, uint32_t vf) { return R(18, 5, rt, vf, 0, 0); }
    constexpr uint32_t ctc2(uint32_t rt, uint32_t cr) { return R(18, 6, rt, cr, 0, 0); }
    // special1: fn 0..59; dest x=8 y=4 z=2 w=1.
    constexpr uint32_t vu1(uint32_t fn, uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft)
    {
        return (18u << 26) | (1u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn;
    }
    // special2: index 0..127 (fn 60..63 + bits 10..6).
    constexpr uint32_t vu2(uint32_t index, uint32_t dest, uint32_t ft, uint32_t fs)
    {
        return (18u << 26) | (1u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((index >> 2) << 6) | 60u | (index & 3u);
    }
    constexpr uint32_t vaddbc(uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t bc) { return vu1(0 + bc, dest, fd, fs, ft); }
    constexpr uint32_t vmulbc(uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t bc) { return vu1(24 + bc, dest, fd, fs, ft); }
    constexpr uint32_t vmaddbc(uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft, uint32_t bc) { return vu1(8 + bc, dest, fd, fs, ft); }
    constexpr uint32_t vmulq(uint32_t dest, uint32_t fd, uint32_t fs) { return vu1(28, dest, fd, fs, 0); }
    constexpr uint32_t vmaddq(uint32_t dest, uint32_t fd, uint32_t fs) { return vu1(33, dest, fd, fs, 0); }
    constexpr uint32_t vadd(uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft) { return vu1(40, dest, fd, fs, ft); }
    constexpr uint32_t vmadd(uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft) { return vu1(41, dest, fd, fs, ft); }
    constexpr uint32_t vmul(uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft) { return vu1(42, dest, fd, fs, ft); }
    constexpr uint32_t vsub(uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft) { return vu1(44, dest, fd, fs, ft); }
    constexpr uint32_t vmsub(uint32_t dest, uint32_t fd, uint32_t fs, uint32_t ft) { return vu1(45, dest, fd, fs, ft); }
    constexpr uint32_t vopmsub(uint32_t fd, uint32_t fs, uint32_t ft) { return vu1(46, XYZ, fd, fs, ft); }
    constexpr uint32_t viadd(uint32_t id, uint32_t is, uint32_t it) { return vu1(48, 0, id, is, it); }
    constexpr uint32_t viaddi(uint32_t it, uint32_t is, int32_t imm5) { return vu1(50, 0, static_cast<uint32_t>(imm5) & 31u, is, it); }
    constexpr uint32_t vcallms(uint32_t addr) { return (18u << 26) | (1u << 25) | (((addr / 8) & 0x7FFFu) << 6) | 56u; }
    constexpr uint32_t vmaddabc(uint32_t dest, uint32_t fs, uint32_t ft, uint32_t bc) { return vu2(8 + bc, dest, ft, fs); }
    constexpr uint32_t vitof0(uint32_t dest, uint32_t ft, uint32_t fs) { return vu2(16, dest, ft, fs); }
    constexpr uint32_t vftoi4(uint32_t dest, uint32_t ft, uint32_t fs) { return vu2(21, dest, ft, fs); }
    constexpr uint32_t vmulabc(uint32_t dest, uint32_t fs, uint32_t ft, uint32_t bc) { return vu2(24 + bc, dest, ft, fs); }
    constexpr uint32_t vabs(uint32_t dest, uint32_t ft, uint32_t fs) { return vu2(29, dest, ft, fs); }
    constexpr uint32_t vclipw(uint32_t fs, uint32_t ft) { return vu2(31, XYZ, ft, fs); }
    constexpr uint32_t vmadda(uint32_t dest, uint32_t fs, uint32_t ft) { return vu2(41, dest, ft, fs); }
    constexpr uint32_t vmula(uint32_t dest, uint32_t fs, uint32_t ft) { return vu2(42, dest, ft, fs); }
    constexpr uint32_t vopmula(uint32_t fs, uint32_t ft) { return vu2(46, XYZ, ft, fs); }
    constexpr uint32_t vnop = vu2(47, 0, 0, 0);
    constexpr uint32_t vmove(uint32_t dest, uint32_t ft, uint32_t fs) { return vu2(48, dest, ft, fs); }
    constexpr uint32_t vmr32(uint32_t dest, uint32_t ft, uint32_t fs) { return vu2(49, dest, ft, fs); }
    constexpr uint32_t vlqi(uint32_t dest, uint32_t ft, uint32_t is) { return vu2(52, dest, ft, is); }
    constexpr uint32_t vsqi(uint32_t dest, uint32_t fs, uint32_t it) { return vu2(53, dest, it, fs); }
    // vdiv Q, vf[fs].fsf, vf[ft].ftf: fsf in bits 22..21, ftf in bits 24..23 (the dest field's place).
    constexpr uint32_t vdiv(uint32_t fs, uint32_t fsf, uint32_t ft, uint32_t ftf) { return vu2(56, (ftf << 2) | fsf, ft, fs); }

    // A program builder with labels: branches name a label, resolved by finish().
    class Asm
    {
    public:
        explicit Asm(uint32_t base = 0) : base_(base) {}
        uint32_t base() const { return base_; }
        uint32_t here() const { return base_ + 4u * static_cast<uint32_t>(words_.size()); }
        Asm &operator()(uint32_t w)
        {
            words_.push_back(w);
            return *this;
        }
        Asm &label(const std::string &name)
        {
            labels_[name] = words_.size();
            return *this;
        }
        // A branch word without its offset (beq rs, rt; bne...; regimm) and the label it goes to.
        Asm &branch(uint32_t word, const std::string &target)
        {
            fixups_.push_back({words_.size(), target, false});
            words_.push_back(word);
            return *this;
        }
        // A branch to an absolute address (another function).
        Asm &branchTo(uint32_t word, uint32_t target)
        {
            const int32_t off = static_cast<int32_t>(target - (here() + 4u)) / 4;
            words_.push_back(word | (static_cast<uint32_t>(off) & 0xFFFFu));
            return *this;
        }
        Asm &jumpTo(uint32_t opcode, uint32_t target) // j / jal to an absolute address
        {
            words_.push_back((opcode << 26) | ((target >> 2) & 0x3FFFFFFu));
            return *this;
        }
        Asm &jump(uint32_t opcode, const std::string &target) // j / jal to a label in this program
        {
            fixups_.push_back({words_.size(), target, true});
            words_.push_back(opcode << 26);
            return *this;
        }
        uint32_t address(const std::string &name) const { return base_ + 4u * static_cast<uint32_t>(labels_.at(name)); }
        std::vector<uint32_t> finish() const
        {
            std::vector<uint32_t> out = words_;
            for (const auto &f : fixups_)
            {
                auto it = labels_.find(f.label);
                if (it == labels_.end())
                    throw std::runtime_error("undefined label " + f.label);
                if (f.absolute)
                    out[f.at] |= ((base_ + 4u * static_cast<uint32_t>(it->second)) >> 2) & 0x3FFFFFFu;
                else
                    out[f.at] |= static_cast<uint32_t>(static_cast<int32_t>(it->second) - static_cast<int32_t>(f.at) - 1) & 0xFFFFu;
            }
            return out;
        }

    private:
        struct Fixup
        {
            size_t at;
            std::string label;
            bool absolute;
        };
        uint32_t base_;
        std::vector<uint32_t> words_;
        std::map<std::string, size_t> labels_;
        std::vector<Fixup> fixups_;
    };

    // Branch words without offsets, for Asm::branch.
    constexpr uint32_t beq(uint32_t rs, uint32_t rt) { return I(4, rs, rt, 0); }
    constexpr uint32_t bne(uint32_t rs, uint32_t rt) { return I(5, rs, rt, 0); }
    constexpr uint32_t blez(uint32_t rs) { return I(6, rs, 0, 0); }
    constexpr uint32_t bgtz(uint32_t rs) { return I(7, rs, 0, 0); }
    constexpr uint32_t beql(uint32_t rs, uint32_t rt) { return I(20, rs, rt, 0); }
    constexpr uint32_t bnel(uint32_t rs, uint32_t rt) { return I(21, rs, rt, 0); }
    constexpr uint32_t blezl(uint32_t rs) { return I(22, rs, 0, 0); }
    constexpr uint32_t bgtzl(uint32_t rs) { return I(23, rs, 0, 0); }
    constexpr uint32_t bltz(uint32_t rs) { return I(1, rs, 0, 0); }
    constexpr uint32_t bgez(uint32_t rs) { return I(1, rs, 1, 0); }
    constexpr uint32_t bltzl(uint32_t rs) { return I(1, rs, 2, 0); }
    constexpr uint32_t bgezl(uint32_t rs) { return I(1, rs, 3, 0); }
    constexpr uint32_t bltzal(uint32_t rs) { return I(1, rs, 16, 0); }
    constexpr uint32_t bgezal(uint32_t rs) { return I(1, rs, 17, 0); }
    constexpr uint32_t bc1f() { return R(17, 8, 0, 0, 0, 0); }
    constexpr uint32_t bc1t() { return R(17, 8, 1, 0, 0, 0); }
    constexpr uint32_t bc1fl() { return R(17, 8, 2, 0, 0, 0); }
    constexpr uint32_t bc1tl() { return R(17, 8, 3, 0, 0, 0); }
    constexpr uint32_t bc0f() { return R(16, 8, 0, 0, 0, 0); }
}
