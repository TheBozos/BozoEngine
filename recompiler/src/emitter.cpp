// The C++ emitter. Each guest function becomes a C++ function whose body is the guest's instructions in address
// order, with a label wherever control can arrive from elsewhere. Semantics of each instruction: docs/specs/r5900.md;
// the helpers called here are the inline functions of runtime/include/bozo/.
//
// Control flow:
//  - A branch computes its condition, then runs its delay slot, then jumps (`goto` inside the function; a call then
//    `return` when the target is in another function: a tail call). "Likely" branches run the delay slot only when
//    taken.
//  - jal / jalr / bal: the return address is written, the delay slot runs, then the callee is called as a C++
//    function; execution continues after the delay slot when it returns.
//  - jr ra returns. jr through a known jump table is a C++ switch on the target; any other jr is a tail call through
//    the runtime's function table.
//  - If the delay slot of a branch is itself a label (reached by another branch), it is emitted a second time as an
//    ordinary instruction after the branch, and the branch's fall-through path skips over that copy.
//  - Scheduler checkpoints at function entry and before every backward goto.
#include "emitter.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <set>

namespace bozo
{
    namespace
    {
        std::string hex(uint32_t v) { return std::format("0x{:x}u", v); }
        std::string hex64(uint64_t v) { return std::format("0x{:x}ull", v); }
        uint64_t sext32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

        // The address operand of a load or store: base + offset, 32-bit wrap-around.
        std::string ea(const Insn &i)
        {
            if (i.simm() == 0)
                return std::format("BZ_W({})", i.rs());
            return std::format("(BZ_W({}) + {})", i.rs(), hex(static_cast<uint32_t>(i.simm())));
        }
    }

    std::string Emitter::entryName(const Function &f, uint32_t addr) const
    {
        if (addr == f.start)
            return cppName(f.symbol, addr);
        auto it = f.entrySymbols.find(addr);
        return cppName(it != f.entrySymbols.end() ? it->second : std::string(), addr);
    }

    bool readExternals(const std::string &path, std::map<uint32_t, ExternalKind> &out, std::string &error)
    {
        std::ifstream in(path);
        if (!in)
        {
            error = "cannot read " + path;
            return false;
        }
        std::string line;
        int n = 0;
        while (std::getline(in, line))
        {
            ++n;
            if (const size_t h = line.find('#'); h != std::string::npos)
                line.resize(h);
            char kind[32] = {};
            unsigned long address = 0;
            if (std::sscanf(line.c_str(), "%31s %lx", kind, &address) != 2)
            {
                if (line.find_first_not_of(" \t\r") == std::string::npos)
                    continue;
                error = std::format("{}:{}: expected \"stub 0xADDR\" or \"override 0xADDR\"", path, n);
                return false;
            }
            const std::string k = kind;
            if (k == "stub")
                out[static_cast<uint32_t>(address)] = ExternalKind::Stub;
            else if (k == "override")
                out.emplace(static_cast<uint32_t>(address), ExternalKind::Override); // a stub entry wins
            else
            {
                error = std::format("{}:{}: unknown kind \"{}\"", path, n, k);
                return false;
            }
        }
        return true;
    }

    bool Emitter::isExternal(uint32_t target, bool call) const
    {
        const auto it = opt_.externals.find(target);
        return it != opt_.externals.end() && (call || it->second == ExternalKind::Stub);
    }

    std::string Emitter::callTo(uint32_t target, bool call)
    {
        if (isExternal(target, call))
        {
            ++stats_.externalCalls;
            return std::format("bozo_call_indirect(rdram, ctx, rt, {});", hex(target));
        }
        if (const Function *g = a_.functionContaining(target))
            if (g->start == target || g->extraEntries.count(target))
                return entryName(*g, target) + "(rdram, ctx, rt);";
        return std::format("bozo_call_indirect(rdram, ctx, rt, {});", hex(target));
    }

    // One instruction that is not a branch or jump. Writes to r0 are dropped (r0 stays zero).
    std::string Emitter::translate(const Insn &i)
    {
        const uint32_t s = i.rs(), t = i.rt(), d = i.rd(), sa = i.sa();
        const uint32_t imm = static_cast<uint32_t>(i.simm());
        const uint32_t ft = i.ft(), fs = i.fs(), fd = i.fd();
        const uint32_t dest = i.vdest();
        auto unimpl = [&]() {
            ++stats_.untranslated[static_cast<unsigned>(i.op)];
            return std::format("ctx->pc = {}; bozo_unimplemented(rdram, ctx, rt, {});", hex(i.addr), hex(i.word));
        };
        auto toT = [&](const std::string &e) { return t == 0 ? std::string() : std::format("BZ_D({}) = {};", t, e); };
        auto toD = [&](const std::string &e) { return d == 0 ? std::string() : std::format("BZ_D({}) = {};", d, e); };
        auto mmi = [&](const char *fn, const std::string &args) {
            return d == 0 ? std::string() : std::format("BZ_Q({}) = bozo::mmi::{}({});", d, fn, args);
        };
        auto st = [&]() { return std::format("BZ_Q({}), BZ_Q({})", s, t); };
        auto trap = [&](const std::string &cond) {
            return std::format("if ({}) {{ ctx->pc = {}; bozo_trap(rdram, ctx, rt); }}", cond, hex(i.addr));
        };
        // VU0: the second operand of an FMAC instruction.
        auto vbc = [&]() { return std::format("bozo::vu0::bc(BZ_VF({}), {})", ft, i.vbc()); };
        const std::string vq = "bozo::vu0::broadcast(ctx->vu0_q_bits)";
        const std::string vi = "bozo::vu0::broadcast(ctx->vu0_i_bits)";
        auto vop = [&](const char *fn, const std::string &second) {
            if (fd == 0)
                return std::string(); // vf00 is constant
            return std::format("bozo::vu0::{}(BZ_VF({}), BZ_VF({}), {}, {});", fn, fd, fs, second, dest);
        };
        auto vacc = [&](const char *fn, const std::string &second) {
            return std::format("bozo::vu0::{}(ctx->vu0_acc, BZ_VF({}), {}, {});", fn, fs, second, dest);
        };
        auto vmac = [&](const char *fn, const std::string &second) { // madd/msub into fd
            if (fd == 0)
                return std::string();
            return std::format("bozo::vu0::{}(BZ_VF({}), ctx->vu0_acc, BZ_VF({}), {}, {});", fn, fd, fs, second, dest);
        };
        auto vmaca = [&](const char *fn, const std::string &second) { // madda/msuba into ACC
            return std::format("bozo::vu0::{}(ctx->vu0_acc, ctx->vu0_acc, BZ_VF({}), {}, {});", fn, fs, second, dest);
        };
        auto vunary = [&](const char *fn, const std::string &extra) { // ft = f(fs) (abs, move, mr32, itof, ftoi)
            if (ft == 0)
                return std::string();
            return std::format("bozo::vu0::{}(BZ_VF({}), BZ_VF({}), {}{});", fn, ft, fs, dest, extra);
        };
        auto viReg = [](uint32_t n) { return std::format("ctx->vi[{}]", n & 15u); };

        switch (i.op)
        {
        // ---- Integer arithmetic and logic ----
        case Op::ADDI: // overflow trap not modelled (r5900.md "Exceptions")
        case Op::ADDIU: return toT(std::format("bozo::sx32(BZ_W({}) + {})", s, hex(imm)));
        case Op::DADDI:
        case Op::DADDIU: return toT(std::format("BZ_D({}) + {}", s, hex64(sext32(imm))));
        case Op::SLTI: return toT(std::format("BZ_SD({}) < {}ll ? 1u : 0u", s, i.simm()));
        case Op::SLTIU: return toT(std::format("BZ_D({}) < {} ? 1u : 0u", s, hex64(sext32(imm))));
        case Op::ANDI: return toT(std::format("BZ_D({}) & {}", s, hex64(i.uimm())));
        case Op::ORI: return toT(std::format("BZ_D({}) | {}", s, hex64(i.uimm())));
        case Op::XORI: return toT(std::format("BZ_D({}) ^ {}", s, hex64(i.uimm())));
        case Op::LUI: return toT(hex64(sext32(i.uimm() << 16)));
        case Op::SLL: return toD(std::format("bozo::sx32(BZ_W({}) << {})", t, sa));
        case Op::SRL: return toD(std::format("bozo::sx32(BZ_W({}) >> {})", t, sa));
        case Op::SRA: return toD(std::format("bozo::sx32(static_cast<uint32_t>(BZ_SW({}) >> {}))", t, sa));
        case Op::SLLV: return toD(std::format("bozo::sx32(BZ_W({}) << (BZ_W({}) & 31u))", t, s));
        case Op::SRLV: return toD(std::format("bozo::sx32(BZ_W({}) >> (BZ_W({}) & 31u))", t, s));
        case Op::SRAV: return toD(std::format("bozo::sx32(static_cast<uint32_t>(BZ_SW({}) >> (BZ_W({}) & 31u)))", t, s));
        case Op::DSLL: return toD(std::format("BZ_D({}) << {}", t, sa));
        case Op::DSRL: return toD(std::format("BZ_D({}) >> {}", t, sa));
        case Op::DSRA: return toD(std::format("static_cast<uint64_t>(BZ_SD({}) >> {})", t, sa));
        case Op::DSLL32: return toD(std::format("BZ_D({}) << {}", t, sa + 32));
        case Op::DSRL32: return toD(std::format("BZ_D({}) >> {}", t, sa + 32));
        case Op::DSRA32: return toD(std::format("static_cast<uint64_t>(BZ_SD({}) >> {})", t, sa + 32));
        case Op::DSLLV: return toD(std::format("BZ_D({}) << (BZ_W({}) & 63u)", t, s));
        case Op::DSRLV: return toD(std::format("BZ_D({}) >> (BZ_W({}) & 63u)", t, s));
        case Op::DSRAV: return toD(std::format("static_cast<uint64_t>(BZ_SD({}) >> (BZ_W({}) & 63u))", t, s));
        case Op::MOVZ: return d == 0 ? "" : std::format("if (BZ_D({}) == 0) BZ_D({}) = BZ_D({});", t, d, s);
        case Op::MOVN: return d == 0 ? "" : std::format("if (BZ_D({}) != 0) BZ_D({}) = BZ_D({});", t, d, s);
        case Op::ADD: // overflow trap not modelled
        case Op::ADDU: return toD(std::format("bozo::sx32(BZ_W({}) + BZ_W({}))", s, t));
        case Op::SUB:
        case Op::SUBU: return toD(std::format("bozo::sx32(BZ_W({}) - BZ_W({}))", s, t));
        case Op::DADD:
        case Op::DADDU: return toD(std::format("BZ_D({}) + BZ_D({})", s, t));
        case Op::DSUB:
        case Op::DSUBU: return toD(std::format("BZ_D({}) - BZ_D({})", s, t));
        case Op::AND: return toD(std::format("BZ_D({}) & BZ_D({})", s, t));
        case Op::OR: return toD(std::format("BZ_D({}) | BZ_D({})", s, t));
        case Op::XOR: return toD(std::format("BZ_D({}) ^ BZ_D({})", s, t));
        case Op::NOR: return toD(std::format("~(BZ_D({}) | BZ_D({}))", s, t));
        case Op::SLT: return toD(std::format("BZ_SD({}) < BZ_SD({}) ? 1u : 0u", s, t));
        case Op::SLTU: return toD(std::format("BZ_D({}) < BZ_D({}) ? 1u : 0u", s, t));

        // ---- HI/LO, multiply, divide (pipeline 0 and pipeline 1) ----
        case Op::MFHI: return toD("ctx->hi");
        case Op::MFLO: return toD("ctx->lo");
        case Op::MTHI: return std::format("ctx->hi = BZ_D({});", s);
        case Op::MTLO: return std::format("ctx->lo = BZ_D({});", s);
        case Op::MFHI1: return toD("ctx->hi1");
        case Op::MFLO1: return toD("ctx->lo1");
        case Op::MTHI1: return std::format("ctx->hi1 = BZ_D({});", s);
        case Op::MTLO1: return std::format("ctx->lo1 = BZ_D({});", s);
        case Op::MULT:
        case Op::MULTU:
        case Op::MADD:
        case Op::MADDU:
        case Op::MULT1:
        case Op::MULTU1:
        case Op::MADD1:
        case Op::MADDU1:
        {
            const bool one = i.op == Op::MULT1 || i.op == Op::MULTU1 || i.op == Op::MADD1 || i.op == Op::MADDU1;
            const char *fn = (i.op == Op::MULT || i.op == Op::MULT1)     ? "mult"
                             : (i.op == Op::MULTU || i.op == Op::MULTU1) ? "multu"
                             : (i.op == Op::MADD || i.op == Op::MADD1)   ? "madd"
                                                                         : "maddu";
            const std::string call = std::format("bozo::{}(ctx->hi{}, ctx->lo{}, BZ_W({}), BZ_W({}))", fn, one ? "1" : "",
                                                 one ? "1" : "", s, t);
            return d == 0 ? call + ";" : std::format("BZ_D({}) = {};", d, call); // the R5900's 3-operand form
        }
        case Op::DIV: return std::format("bozo::div(ctx->hi, ctx->lo, BZ_W({}), BZ_W({}));", s, t);
        case Op::DIVU: return std::format("bozo::divu(ctx->hi, ctx->lo, BZ_W({}), BZ_W({}));", s, t);
        case Op::DIV1: return std::format("bozo::div(ctx->hi1, ctx->lo1, BZ_W({}), BZ_W({}));", s, t);
        case Op::DIVU1: return std::format("bozo::divu(ctx->hi1, ctx->lo1, BZ_W({}), BZ_W({}));", s, t);

        // ---- SA register ----
        case Op::MFSA: return toD("ctx->sa");
        case Op::MTSA: return std::format("ctx->sa = BZ_W({}) & 0xFFu;", s);
        case Op::MTSAB: return std::format("ctx->sa = ((BZ_W({}) ^ {}) & 15u) * 8u;", s, hex(i.uimm()));
        case Op::MTSAH: return std::format("ctx->sa = ((BZ_W({}) ^ {}) & 7u) * 16u;", s, hex(i.uimm()));

        // ---- Traps and system ----
        case Op::TGE: return trap(std::format("BZ_SD({}) >= BZ_SD({})", s, t));
        case Op::TGEU: return trap(std::format("BZ_D({}) >= BZ_D({})", s, t));
        case Op::TLT: return trap(std::format("BZ_SD({}) < BZ_SD({})", s, t));
        case Op::TLTU: return trap(std::format("BZ_D({}) < BZ_D({})", s, t));
        case Op::TEQ: return trap(std::format("BZ_D({}) == BZ_D({})", s, t));
        case Op::TNE: return trap(std::format("BZ_D({}) != BZ_D({})", s, t));
        case Op::TGEI: return trap(std::format("BZ_SD({}) >= {}ll", s, i.simm()));
        case Op::TGEIU: return trap(std::format("BZ_D({}) >= {}", s, hex64(sext32(imm))));
        case Op::TLTI: return trap(std::format("BZ_SD({}) < {}ll", s, i.simm()));
        case Op::TLTIU: return trap(std::format("BZ_D({}) < {}", s, hex64(sext32(imm))));
        case Op::TEQI: return trap(std::format("BZ_SD({}) == {}ll", s, i.simm()));
        case Op::TNEI: return trap(std::format("BZ_SD({}) != {}ll", s, i.simm()));
        case Op::SYSCALL:
            return std::format("ctx->pc = {}; bozo_syscall(rdram, ctx, rt, {});", hex(i.addr), hex((i.word >> 6) & 0xFFFFFu));
        case Op::BREAK:
            return std::format("ctx->pc = {}; bozo_break(rdram, ctx, rt, {});", hex(i.addr), hex((i.word >> 6) & 0xFFFFFu));
        case Op::SYNC:
        case Op::PREF:
        case Op::CACHE: return "/* no caches, no pipeline to drain */";

        // ---- Loads and stores ----
        case Op::LB: return toT(std::format("static_cast<uint64_t>(static_cast<int8_t>(bozo::load<uint8_t>(rdram, ctx, rt, {})))", ea(i)));
        case Op::LBU: return toT(std::format("bozo::load<uint8_t>(rdram, ctx, rt, {})", ea(i)));
        case Op::LH: return toT(std::format("static_cast<uint64_t>(static_cast<int16_t>(bozo::load<uint16_t>(rdram, ctx, rt, {})))", ea(i)));
        case Op::LHU: return toT(std::format("bozo::load<uint16_t>(rdram, ctx, rt, {})", ea(i)));
        case Op::LW: return toT(std::format("bozo::sx32(bozo::load<uint32_t>(rdram, ctx, rt, {}))", ea(i)));
        case Op::LWU: return toT(std::format("bozo::load<uint32_t>(rdram, ctx, rt, {})", ea(i)));
        case Op::LD: return toT(std::format("bozo::load<uint64_t>(rdram, ctx, rt, {})", ea(i)));
        case Op::LQ: return t == 0 ? "" : std::format("bozo::load128(rdram, ctx, rt, {}, &BZ_Q({}));", ea(i), t);
        case Op::LWL: return toT(std::format("bozo::lwl(rdram, ctx, rt, {}, BZ_D({}))", ea(i), t));
        case Op::LWR: return toT(std::format("bozo::lwr(rdram, ctx, rt, {}, BZ_D({}))", ea(i), t));
        case Op::LDL: return toT(std::format("bozo::ldl(rdram, ctx, rt, {}, BZ_D({}))", ea(i), t));
        case Op::LDR: return toT(std::format("bozo::ldr(rdram, ctx, rt, {}, BZ_D({}))", ea(i), t));
        case Op::SB: return std::format("bozo::store<uint8_t>(rdram, ctx, rt, {}, static_cast<uint8_t>(BZ_W({})));", ea(i), t);
        case Op::SH: return std::format("bozo::store<uint16_t>(rdram, ctx, rt, {}, static_cast<uint16_t>(BZ_W({})));", ea(i), t);
        case Op::SW: return std::format("bozo::store<uint32_t>(rdram, ctx, rt, {}, BZ_W({}));", ea(i), t);
        case Op::SD: return std::format("bozo::store<uint64_t>(rdram, ctx, rt, {}, BZ_D({}));", ea(i), t);
        case Op::SQ: return std::format("bozo::store128(rdram, ctx, rt, {}, &BZ_Q({}));", ea(i), t);
        case Op::SWL: return std::format("bozo::swl(rdram, ctx, rt, {}, BZ_D({}));", ea(i), t);
        case Op::SWR: return std::format("bozo::swr(rdram, ctx, rt, {}, BZ_D({}));", ea(i), t);
        case Op::SDL: return std::format("bozo::sdl(rdram, ctx, rt, {}, BZ_D({}));", ea(i), t);
        case Op::SDR: return std::format("bozo::sdr(rdram, ctx, rt, {}, BZ_D({}));", ea(i), t);
        case Op::LWC1: return std::format("BZ_F({}) = bozo::load<uint32_t>(rdram, ctx, rt, {});", ft, ea(i));
        case Op::SWC1: return std::format("bozo::store<uint32_t>(rdram, ctx, rt, {}, BZ_F({}));", ea(i), ft);
        case Op::LQC2:
            return ft == 0 ? "" : std::format("bozo::load128(rdram, ctx, rt, {}, &BZ_VF({}));", ea(i), ft);
        case Op::SQC2: return std::format("bozo::store128(rdram, ctx, rt, {}, &BZ_VF({}));", ea(i), ft);

        // ---- MMI ----
        case Op::PADDW: return mmi("paddw", st());
        case Op::PSUBW: return mmi("psubw", st());
        case Op::PCGTW: return mmi("pcgtw", st());
        case Op::PMAXW: return mmi("pmaxw", st());
        case Op::PADDH: return mmi("paddh", st());
        case Op::PSUBH: return mmi("psubh", st());
        case Op::PCGTH: return mmi("pcgth", st());
        case Op::PMAXH: return mmi("pmaxh", st());
        case Op::PADDB: return mmi("paddb", st());
        case Op::PSUBB: return mmi("psubb", st());
        case Op::PCGTB: return mmi("pcgtb", st());
        case Op::PADDSW: return mmi("paddsw", st());
        case Op::PSUBSW: return mmi("psubsw", st());
        case Op::PEXTLW: return mmi("pextlw", st());
        case Op::PPACW: return mmi("ppacw", st());
        case Op::PADDSH: return mmi("paddsh", st());
        case Op::PSUBSH: return mmi("psubsh", st());
        case Op::PEXTLH: return mmi("pextlh", st());
        case Op::PPACH: return mmi("ppach", st());
        case Op::PADDSB: return mmi("paddsb", st());
        case Op::PSUBSB: return mmi("psubsb", st());
        case Op::PEXTLB: return mmi("pextlb", st());
        case Op::PPACB: return mmi("ppacb", st());
        case Op::PEXT5: return mmi("pext5", std::format("BZ_Q({})", t));
        case Op::PPAC5: return mmi("ppac5", std::format("BZ_Q({})", t));
        case Op::PABSW: return mmi("pabsw", std::format("BZ_Q({})", t));
        case Op::PCEQW: return mmi("pceqw", st());
        case Op::PMINW: return mmi("pminw", st());
        case Op::PADSBH: return mmi("padsbh", st());
        case Op::PABSH: return mmi("pabsh", std::format("BZ_Q({})", t));
        case Op::PCEQH: return mmi("pceqh", st());
        case Op::PMINH: return mmi("pminh", st());
        case Op::PCEQB: return mmi("pceqb", st());
        case Op::PADDUW: return mmi("padduw", st());
        case Op::PSUBUW: return mmi("psubuw", st());
        case Op::PEXTUW: return mmi("pextuw", st());
        case Op::PADDUH: return mmi("padduh", st());
        case Op::PSUBUH: return mmi("psubuh", st());
        case Op::PEXTUH: return mmi("pextuh", st());
        case Op::PADDUB: return mmi("paddub", st());
        case Op::PSUBUB: return mmi("psubub", st());
        case Op::PEXTUB: return mmi("pextub", st());
        case Op::QFSRV: return mmi("qfsrv", st() + ", ctx->sa");
        case Op::PSLLVW: return mmi("psllvw", std::format("BZ_Q({}), BZ_Q({})", t, s));
        case Op::PSRLVW: return mmi("psrlvw", std::format("BZ_Q({}), BZ_Q({})", t, s));
        case Op::PSRAVW: return mmi("psravw", std::format("BZ_Q({}), BZ_Q({})", t, s));
        case Op::PINTH: return mmi("pinth", st());
        case Op::PINTEH: return mmi("pinteh", st());
        case Op::PCPYLD: return mmi("pcpyld", st());
        case Op::PCPYUD: return mmi("pcpyud", st());
        case Op::PAND: return mmi("pand", st());
        case Op::POR: return mmi("por", st());
        case Op::PXOR: return mmi("pxor", st());
        case Op::PNOR: return mmi("pnor", st());
        case Op::PEXEH: return mmi("pexeh", std::format("BZ_Q({})", t));
        case Op::PREVH: return mmi("prevh", std::format("BZ_Q({})", t));
        case Op::PEXCH: return mmi("pexch", std::format("BZ_Q({})", t));
        case Op::PCPYH: return mmi("pcpyh", std::format("BZ_Q({})", t));
        case Op::PEXEW: return mmi("pexew", std::format("BZ_Q({})", t));
        case Op::PEXCW: return mmi("pexcw", std::format("BZ_Q({})", t));
        case Op::PROT3W: return mmi("prot3w", std::format("BZ_Q({})", t));
        case Op::PLZCW: return mmi("plzcw", std::format("BZ_Q({}), BZ_Q({})", s, d));
        case Op::PSLLH: return mmi("psllh", std::format("BZ_Q({}), {}", t, sa));
        case Op::PSRLH: return mmi("psrlh", std::format("BZ_Q({}), {}", t, sa));
        case Op::PSRAH: return mmi("psrah", std::format("BZ_Q({}), {}", t, sa));
        case Op::PSLLW: return mmi("psllw", std::format("BZ_Q({}), {}", t, sa));
        case Op::PSRLW: return mmi("psrlw", std::format("BZ_Q({}), {}", t, sa));
        case Op::PSRAW: return mmi("psraw", std::format("BZ_Q({}), {}", t, sa));
        case Op::PMFHI: return d == 0 ? "" : std::format("BZ_Q({0}).ud[0] = ctx->hi; BZ_Q({0}).ud[1] = ctx->hi1;", d);
        case Op::PMFLO: return d == 0 ? "" : std::format("BZ_Q({0}).ud[0] = ctx->lo; BZ_Q({0}).ud[1] = ctx->lo1;", d);
        case Op::PMTHI: return std::format("ctx->hi = BZ_Q({0}).ud[0]; ctx->hi1 = BZ_Q({0}).ud[1];", s);
        case Op::PMTLO: return std::format("ctx->lo = BZ_Q({0}).ud[0]; ctx->lo1 = BZ_Q({0}).ud[1];", s);

        // ---- COP0 ----
        case Op::MFC0: return toT(std::format("bozo::sx32((ctx->pc = {}, bozo_cop0_read(ctx, rt, {})))", hex(i.addr), d));
        case Op::MTC0: return std::format("ctx->pc = {}; bozo_cop0_write(ctx, rt, {}, BZ_W({}));", hex(i.addr), d, t);
        case Op::EI: return std::format("ctx->pc = {}; bozo_set_interrupts(rdram, ctx, rt, true);", hex(i.addr));
        case Op::DI: return std::format("ctx->pc = {}; bozo_set_interrupts(rdram, ctx, rt, false);", hex(i.addr));

        // ---- COP1 (FPU) ----
        case Op::MFC1: return toT(std::format("bozo::sx32(BZ_F({}))", fs));
        case Op::MTC1: return std::format("BZ_F({}) = BZ_W({});", fs, t);
        case Op::CFC1: return toT(fs == 31 ? std::string("bozo::sx32(ctx->fcr31)") : fs == 0 ? hex64(0x2E30) : "0");
        case Op::CTC1: return fs == 31 ? std::format("ctx->fcr31 = BZ_W({});", t) : "/* ctc1 to a read-only register */";
        case Op::ADD_S: return std::format("BZ_F({}) = bozo::fadd(ctx, BZ_F({}), BZ_F({}));", fd, fs, ft);
        case Op::SUB_S: return std::format("BZ_F({}) = bozo::fsub(ctx, BZ_F({}), BZ_F({}));", fd, fs, ft);
        case Op::MUL_S: return std::format("BZ_F({}) = bozo::fmul(ctx, BZ_F({}), BZ_F({}));", fd, fs, ft);
        case Op::DIV_S: return std::format("BZ_F({}) = bozo::fdiv(ctx, BZ_F({}), BZ_F({}));", fd, fs, ft);
        case Op::SQRT_S: return std::format("BZ_F({}) = bozo::fsqrt(ctx, BZ_F({}));", fd, ft);
        case Op::RSQRT_S: return std::format("BZ_F({}) = bozo::frsqrt(ctx, BZ_F({}), BZ_F({}));", fd, fs, ft);
        case Op::ABS_S: return std::format("BZ_F({}) = BZ_F({}) & 0x7FFFFFFFu;", fd, fs);
        case Op::NEG_S: return std::format("BZ_F({}) = BZ_F({}) ^ 0x80000000u;", fd, fs);
        case Op::MOV_S: return std::format("BZ_F({}) = BZ_F({});", fd, fs);
        case Op::ADDA_S: return std::format("ctx->facc = bozo::fadd(ctx, BZ_F({}), BZ_F({}));", fs, ft);
        case Op::SUBA_S: return std::format("ctx->facc = bozo::fsub(ctx, BZ_F({}), BZ_F({}));", fs, ft);
        case Op::MULA_S: return std::format("ctx->facc = bozo::fmul(ctx, BZ_F({}), BZ_F({}));", fs, ft);
        case Op::MADD_S: return std::format("BZ_F({}) = bozo::fmadd(ctx, ctx->facc, BZ_F({}), BZ_F({}));", fd, fs, ft);
        case Op::MSUB_S: return std::format("BZ_F({}) = bozo::fmsub(ctx, ctx->facc, BZ_F({}), BZ_F({}));", fd, fs, ft);
        case Op::MADDA_S: return std::format("ctx->facc = bozo::fmadd(ctx, ctx->facc, BZ_F({}), BZ_F({}));", fs, ft);
        case Op::MSUBA_S: return std::format("ctx->facc = bozo::fmsub(ctx, ctx->facc, BZ_F({}), BZ_F({}));", fs, ft);
        case Op::CVT_W_S: return std::format("BZ_F({}) = bozo::ps2f::toInt(BZ_F({}));", fd, fs);
        case Op::CVT_S_W: return std::format("BZ_F({}) = bozo::ps2f::fromInt(BZ_F({}));", fd, fs);
        case Op::MAX_S: return std::format("BZ_F({}) = bozo::ps2f::max(BZ_F({}), BZ_F({}));", fd, fs, ft);
        case Op::MIN_S: return std::format("BZ_F({}) = bozo::ps2f::min(BZ_F({}), BZ_F({}));", fd, fs, ft);
        case Op::C_F_S: return "bozo::fcompare(ctx, false);";
        case Op::C_EQ_S: return std::format("bozo::fcompare(ctx, bozo::ps2f::eq(BZ_F({}), BZ_F({})));", fs, ft);
        case Op::C_LT_S: return std::format("bozo::fcompare(ctx, bozo::ps2f::lt(BZ_F({}), BZ_F({})));", fs, ft);
        case Op::C_LE_S: return std::format("bozo::fcompare(ctx, bozo::ps2f::le(BZ_F({}), BZ_F({})));", fs, ft);

        // ---- COP2 (VU0 macro mode) ----
        case Op::QMFC2: return t == 0 ? "" : std::format("std::memcpy(&BZ_Q({}), &BZ_VF({}), 16);", t, d);
        case Op::QMTC2: return d == 0 ? "" : std::format("std::memcpy(&BZ_VF({}), &BZ_Q({}), 16);", d, t);
        case Op::CFC2:
            if (d < 16)
                return toT(std::format("ctx->vi[{}] & 0xFFFFu", d));
            return toT(std::format("bozo::sx32(bozo::vu0::readControl(ctx, rt, {}))", d));
        case Op::CTC2:
            if (d == 0)
                return "";
            if (d < 16)
                return std::format("ctx->vi[{}] = BZ_W({}) & 0xFFFFu;", d, t);
            return std::format("bozo::vu0::writeControl(ctx, rt, {}, BZ_W({}));", d, t);
        case Op::VADDbc: return vop("add", vbc());
        case Op::VSUBbc: return vop("sub", vbc());
        case Op::VMULbc: return vop("mul", vbc());
        case Op::VMAXbc: return vop("max", vbc());
        case Op::VMINIbc: return vop("mini", vbc());
        case Op::VMADDbc: return vmac("madd", vbc());
        case Op::VMSUBbc: return vmac("msub", vbc());
        case Op::VADDq: return vop("add", vq);
        case Op::VSUBq: return vop("sub", vq);
        case Op::VMULq: return vop("mul", vq);
        case Op::VMADDq: return vmac("madd", vq);
        case Op::VMSUBq: return vmac("msub", vq);
        case Op::VADDi: return vop("add", vi);
        case Op::VSUBi: return vop("sub", vi);
        case Op::VMULi: return vop("mul", vi);
        case Op::VMAXi: return vop("max", vi);
        case Op::VMINIi: return vop("mini", vi);
        case Op::VMADDi: return vmac("madd", vi);
        case Op::VMSUBi: return vmac("msub", vi);
        case Op::VADD: return vop("add", std::format("BZ_VF({})", ft));
        case Op::VSUB: return vop("sub", std::format("BZ_VF({})", ft));
        case Op::VMUL: return vop("mul", std::format("BZ_VF({})", ft));
        case Op::VMAX: return vop("max", std::format("BZ_VF({})", ft));
        case Op::VMINI: return vop("mini", std::format("BZ_VF({})", ft));
        case Op::VMADD: return vmac("madd", std::format("BZ_VF({})", ft));
        case Op::VMSUB: return vmac("msub", std::format("BZ_VF({})", ft));
        case Op::VOPMSUB:
            return fd == 0 ? "" : std::format("bozo::vu0::opmsub(BZ_VF({}), ctx->vu0_acc, BZ_VF({}), BZ_VF({}));", fd, fs, ft);
        case Op::VADDAbc: return vacc("add", vbc());
        case Op::VSUBAbc: return vacc("sub", vbc());
        case Op::VMULAbc: return vacc("mul", vbc());
        case Op::VMADDAbc: return vmaca("madd", vbc());
        case Op::VMSUBAbc: return vmaca("msub", vbc());
        case Op::VADDAq: return vacc("add", vq);
        case Op::VSUBAq: return vacc("sub", vq);
        case Op::VMULAq: return vacc("mul", vq);
        case Op::VMADDAq: return vmaca("madd", vq);
        case Op::VMSUBAq: return vmaca("msub", vq);
        case Op::VADDAi: return vacc("add", vi);
        case Op::VSUBAi: return vacc("sub", vi);
        case Op::VMULAi: return vacc("mul", vi);
        case Op::VMADDAi: return vmaca("madd", vi);
        case Op::VMSUBAi: return vmaca("msub", vi);
        case Op::VADDA: return vacc("add", std::format("BZ_VF({})", ft));
        case Op::VSUBA: return vacc("sub", std::format("BZ_VF({})", ft));
        case Op::VMULA: return vacc("mul", std::format("BZ_VF({})", ft));
        case Op::VMADDA: return vmaca("madd", std::format("BZ_VF({})", ft));
        case Op::VMSUBA: return vmaca("msub", std::format("BZ_VF({})", ft));
        case Op::VOPMULA: return std::format("bozo::vu0::opmula(ctx->vu0_acc, BZ_VF({}), BZ_VF({}));", fs, ft);
        case Op::VNOP: return "/* vnop */";
        case Op::VABS: return vunary("abs", "");
        case Op::VMOVE: return vunary("move", "");
        case Op::VMR32: return vunary("mr32", "");
        case Op::VITOF0: return vunary("itof", ", 0");
        case Op::VITOF4: return vunary("itof", ", 4");
        case Op::VITOF12: return vunary("itof", ", 12");
        case Op::VITOF15: return vunary("itof", ", 15");
        case Op::VFTOI0: return vunary("ftoi", ", 0");
        case Op::VFTOI4: return vunary("ftoi", ", 4");
        case Op::VFTOI12: return vunary("ftoi", ", 12");
        case Op::VFTOI15: return vunary("ftoi", ", 15");
        case Op::VCLIPw:
            return std::format("{{ const uint32_t w = BZ_VF({1}).u[3] & 0x7FFFFFFFu; uint32_t c = 0; for (int k = 0; k < 3; ++k) {{ "
                               "const uint32_t v = BZ_VF({0}).u[k]; if (bozo::ps2f::lt(w, v)) c |= 1u << (2 * k); "
                               "if (bozo::ps2f::lt(v, w ^ 0x80000000u)) c |= 2u << (2 * k); }} "
                               "ctx->vu0_clip_flags = ((ctx->vu0_clip_flags << 6) | c) & 0xFFFFFFu; }}",
                               fs, ft);
        case Op::VIADD:
            return (fd & 15u) == 0 ? "" : std::format("{} = ({} + {}) & 0xFFFFu;", viReg(fd), viReg(fs), viReg(ft));
        case Op::VISUB:
            return (fd & 15u) == 0 ? "" : std::format("{} = ({} - {}) & 0xFFFFu;", viReg(fd), viReg(fs), viReg(ft));
        case Op::VIAND:
            return (fd & 15u) == 0 ? "" : std::format("{} = {} & {};", viReg(fd), viReg(fs), viReg(ft));
        case Op::VIOR:
            return (fd & 15u) == 0 ? "" : std::format("{} = {} | {};", viReg(fd), viReg(fs), viReg(ft));
        case Op::VIADDI:
        {
            const int32_t imm5 = static_cast<int32_t>(sa << 27) >> 27;
            return (ft & 15u) == 0 ? "" : std::format("{} = ({} + {}u) & 0xFFFFu;", viReg(ft), viReg(fs), static_cast<uint32_t>(imm5) & 0xFFFFu);
        }
        case Op::VLQI: // vlqi vf[ft], (vi[is]++)
            return std::format("{}{}", ft == 0 ? std::string() : std::format("bozo::vu0::lq(rt, BZ_VF({}), {}, {}); ", ft, viReg(fs), dest),
                               (fs & 15u) == 0 ? std::string() : std::format("{0} = ({0} + 1u) & 0xFFFFu;", viReg(fs)));
        case Op::VSQI: // vsqi vf[fs], (vi[it]++)
            return std::format("bozo::vu0::sq(rt, BZ_VF({}), {}, {});{}", fs, viReg(ft), dest,
                               (ft & 15u) == 0 ? std::string() : std::format(" {0} = ({0} + 1u) & 0xFFFFu;", viReg(ft)));
        case Op::VLQD: // vlqd vf[ft], (--vi[is])
            return std::format("{{ {0} if ({1} != 0) bozo::vu0::lq(rt, BZ_VF({1}), {2}, {3}); }}",
                               (fs & 15u) == 0 ? std::string() : std::format("{0} = ({0} - 1u) & 0xFFFFu;", viReg(fs)), ft,
                               viReg(fs), dest);
        case Op::VSQD: // vsqd vf[fs], (--vi[it])
            return std::format("{} bozo::vu0::sq(rt, BZ_VF({}), {}, {});",
                               (ft & 15u) == 0 ? std::string() : std::format("{0} = ({0} - 1u) & 0xFFFFu;", viReg(ft)), fs,
                               viReg(ft), dest);
        case Op::VDIV:
            return std::format("ctx->vu0_q_bits = bozo::vu0::div(BZ_VF({}).u[{}], BZ_VF({}).u[{}]);", fs, i.vfsf(), ft, i.vftf());
        case Op::VSQRT: return std::format("ctx->vu0_q_bits = bozo::vu0::sqrt(BZ_VF({}).u[{}]);", ft, i.vftf());
        case Op::VRSQRT:
            return std::format("ctx->vu0_q_bits = bozo::vu0::rsqrt(BZ_VF({}).u[{}], BZ_VF({}).u[{}]);", fs, i.vfsf(), ft, i.vftf());
        case Op::VWAITQ: return "/* vwaitq: Q is ready at once */";
        case Op::VMTIR: // vmtir vi[it], vf[fs].fsf
            return (ft & 15u) == 0 ? "" : std::format("{} = BZ_VF({}).u[{}] & 0xFFFFu;", viReg(ft), fs, i.vfsf());
        case Op::VMFIR: // vmfir vf[ft].dest, vi[is]: the integer sign-extended to 32 bits in each field
            return ft == 0 ? "" : std::format("bozo::vu0::move(BZ_VF({}), bozo::vu0::broadcast(static_cast<uint32_t>(static_cast<int16_t>({}))), {});", ft, viReg(fs), dest);
        case Op::VILWR: // vilwr.f vi[it], (vi[is]): the 16 low bits of the selected field
        {
            int field = 0;
            while (field < 3 && !((dest >> (3 - field)) & 1u))
                ++field;
            return (ft & 15u) == 0 ? "" : std::format("{{ uint32_t v; std::memcpy(&v, bozo::vu0::quad(rt, {}) + {}, 4); {} = v & 0xFFFFu; }}", viReg(fs), 4 * field, viReg(ft));
        }
        case Op::VISWR: // viswr.dest vi[it], (vi[is]): the integer (zero-extended) into each selected field
            return std::format("{{ const Vu0Vector v = bozo::vu0::broadcast({} & 0xFFFFu); bozo::vu0::sq(rt, v, {}, {}); }}", viReg(ft), viReg(fs), dest);
        case Op::VCALLMS:
            return std::format("ctx->pc = {}; bozo_vu0_callms(rdram, ctx, rt, {});", hex(i.addr), hex(((i.word >> 6) & 0x7FFFu) * 8u));
        case Op::VCALLMSR:
            return std::format("ctx->pc = {}; bozo_vu0_callms(rdram, ctx, rt, ctx->vu0_cmsar0 * 8u);", hex(i.addr));

        default:
            return unimpl();
        }
    }

    void Emitter::emitFunction(const Function &f, std::string &out)
    {
        const Program &p = a_.program();

        // Labels: branch and jump targets inside the function, jump table entries, extra entries, and the address after
        // a delay slot that is itself a label (the branch's fall-through skips the delay slot's second copy).
        std::set<uint32_t> labels(f.extraEntries.begin(), f.extraEntries.end());
        for (const auto &[jr, t] : f.jumpTables)
            labels.insert(t.targets.begin(), t.targets.end());
        for (uint32_t a = f.start; a + 4 <= f.end; a += 4)
        {
            const Insn i = decodeAt(p.read32(a), a);
            uint32_t target = 0;
            if (isBranch(i.op))
                target = i.branchTarget();
            else if (i.op == Op::J)
                target = i.jumpTarget();
            else
                continue;
            if (target >= f.start && target < f.end && !(opFlags(i.op) & LNK))
                labels.insert(target);
        }
        std::set<uint32_t> extra;
        for (uint32_t a = f.start; a + 4 <= f.end; a += 4)
        {
            const Insn i = decodeAt(p.read32(a), a);
            if (hasDelaySlot(i.op) && labels.count(a + 4) && !(opFlags(i.op) & ENDS))
                extra.insert(a + 8);
        }
        labels.insert(extra.begin(), extra.end());

        auto label = [](uint32_t a) { return std::format("L_{:x}", a); };
        // A transfer to `target` from the instruction at `from`: goto inside the function, else a tail call.
        auto jumpTo = [&](uint32_t target, uint32_t from) -> std::string {
            if (target >= f.start && target < f.end && !isExternal(target, false))
            {
                if (target <= from)
                    return std::format("bozo::checkpoint(rdram, ctx, rt, {}); goto {};", (from - target) / 4 + 2, label(target));
                return std::format("goto {};", label(target));
            }
            return callTo(target, false) + " return;";
        };

        const bool multiEntry = !f.extraEntries.empty();
        const std::string name = cppName(f.symbol, f.start);
        out += std::format("// {} [0x{:x}, 0x{:x})\n", f.symbol.empty() ? "(no symbol)" : f.symbol, f.start, f.end);
        if (multiEntry)
            out += std::format("static void body_0x{:x}(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t entry)\n{{\n", f.start);
        else
            out += std::format("void {}(uint8_t *rdram, R5900Context *ctx, Runtime *rt)\n{{\n", name);

        // Entry: the scheduler checkpoint, charged with the instructions up to the first control transfer.
        uint32_t straight = 1;
        for (uint32_t a = f.start; a + 4 <= f.end && straight < 64; a += 4, ++straight)
            if (hasDelaySlot(decode(p.read32(a))))
                break;
        out += std::format("    ctx->pc = {};\n    bozo::checkpoint(rdram, ctx, rt, {});\n", multiEntry ? "entry" : hex(f.start), straight);
        // A jr through a register we could not resolve may be a return through a copy of ra: compare with the
        // return address the function was entered with.
        if (!f.unresolvedJr.empty())
            out += "    const uint32_t entryRa = BZ_W(31);\n";
        if (multiEntry)
        {
            out += "    switch (entry)\n    {\n";
            for (uint32_t e : f.extraEntries)
                out += std::format("    case {}: goto {};\n", hex(e), label(e));
            out += "    default: break;\n    }\n";
        }

        auto line = [&](const Insn &i, const std::string &code) {
            if (opt_.comments)
            {
                char dis[96];
                disassemble(i, dis, sizeof(dis));
                out += std::format("    {:<60} // {:x}: {}\n", code, i.addr, dis);
            }
            else if (!code.empty())
                out += "    " + code + "\n";
        };

        for (uint32_t a = f.start; a + 4 <= f.end;)
        {
            if (labels.count(a))
                out += label(a) + ":;\n";
            const Insn i = decodeAt(p.read32(a), a);
            ++stats_.instructions;
            if (!hasDelaySlot(i.op))
            {
                if (i.op == Op::ERET)
                    line(i, std::format("ctx->pc = {}; bozo_eret(rdram, ctx, rt); return;", hex(a)));
                else
                    line(i, translate(i));
                a += 4;
                continue;
            }

            // A branch or jump and its delay slot.
            const Insn ds = decodeAt(p.read32(a + 4), a + 4);
            std::string delay = hasDelaySlot(ds.op) ? std::string("/* branch in a delay slot: not executed */") : translate(ds);
            const uint32_t flags = opFlags(i.op);
            std::string code;
            const uint32_t s = i.rs(), t = i.rt();
            if (isBranch(i.op))
            {
                std::string cond;
                switch (i.op)
                {
                case Op::BEQ: case Op::BEQL: cond = s == t ? "true" : std::format("BZ_D({}) == BZ_D({})", s, t); break;
                case Op::BNE: case Op::BNEL: cond = s == t ? "false" : std::format("BZ_D({}) != BZ_D({})", s, t); break;
                case Op::BLEZ: case Op::BLEZL: cond = std::format("BZ_SD({}) <= 0", s); break;
                case Op::BGTZ: case Op::BGTZL: cond = std::format("BZ_SD({}) > 0", s); break;
                case Op::BLTZ: case Op::BLTZL: case Op::BLTZAL: case Op::BLTZALL: cond = std::format("BZ_SD({}) < 0", s); break;
                case Op::BGEZ: case Op::BGEZL: case Op::BGEZAL: case Op::BGEZALL:
                    cond = s == 0 ? "true" : std::format("BZ_SD({}) >= 0", s);
                    break;
                case Op::BC1F: case Op::BC1FL: cond = "!bozo::fcond(ctx)"; break;
                case Op::BC1T: case Op::BC1TL: cond = "bozo::fcond(ctx)"; break;
                case Op::BC0F: case Op::BC0FL: cond = "!bozo_cop0_condition(ctx, rt)"; break;
                case Op::BC0T: case Op::BC0TL: cond = "bozo_cop0_condition(ctx, rt)"; break;
                case Op::BC2F: case Op::BC2FL: cond = "true"; break;  // VU0 is never busy (r5900.md "COP2")
                case Op::BC2T: case Op::BC2TL: cond = "false"; break;
                default: cond = "false"; break;
                }
                const uint32_t target = i.branchTarget();
                const std::string link = (flags & LNK) ? std::format("BZ_D(31) = {}; ", hex64(a + 8)) : std::string();
                const std::string go = (flags & LNK) ? callTo(target, true) : jumpTo(target, a);
                if (cond == "true")
                    code = std::format("{}{} {}", link, delay, go);
                else if (cond == "false")
                    code = (flags & LK) ? link : std::format("{}{}", link, delay);
                else if (flags & LK)
                    code = std::format("{{ const bool c = {}; {}if (c) {{ {} {} }} }}", cond, link, delay, go);
                else
                    code = std::format("{{ const bool c = {}; {}{} if (c) {{ {} }} }}", cond, link, delay, go);
            }
            else if (i.op == Op::J)
                code = delay + " " + jumpTo(i.jumpTarget(), a);
            else if (i.op == Op::JAL)
                code = std::format("BZ_D(31) = {}; {} {}", hex64(a + 8), delay, callTo(i.jumpTarget(), true));
            else if (i.op == Op::JR)
            {
                if (s == 31 || f.returnJr.count(a))
                    code = delay + " return;";
                else if (auto jt = f.jumpTables.find(a); jt != f.jumpTables.end())
                {
                    code = std::format("{{ const uint32_t target = BZ_W({}); {} switch (target) {{ ", s, delay);
                    std::set<uint32_t> seen;
                    for (uint32_t tg : jt->second.targets)
                        if (seen.insert(tg).second)
                            code += std::format("case {}: {} ", hex(tg), jumpTo(tg, a));
                    code += "default: break; } bozo_call_indirect(rdram, ctx, rt, target); return; }";
                }
                else
                    code = std::format("{{ const uint32_t target = BZ_W({}); {} if (target == entryRa) return; "
                                       "bozo_call_indirect(rdram, ctx, rt, target); return; }}",
                                       s, delay);
            }
            else if (i.op == Op::JALR)
            {
                const std::string link = i.rd() == 0 ? std::string() : std::format("BZ_D({}) = {}; ", i.rd(), hex64(a + 8));
                code = std::format("{{ const uint32_t target = BZ_W({}); {}{} bozo_call_indirect(rdram, ctx, rt, target); }}", s, link, delay);
            }
            line(i, code);
            if (labels.count(a + 4))
            {
                // The delay slot is also reached by a jump: emit it again as an ordinary instruction.
                if (!(flags & ENDS))
                    out += std::format("    goto {};\n", label(a + 8));
                out += label(a + 4) + ":;\n";
                line(ds, delay);
            }
            ++stats_.instructions;
            a += 8;
        }

        // Falling off the end: continue into the next function.
        if (labels.count(f.end))
            out += label(f.end) + ":;\n";
        out += "    " + callTo(f.end, false) + "\n}\n";

        if (multiEntry)
        {
            out += std::format("void {}(uint8_t *rdram, R5900Context *ctx, Runtime *rt) {{ body_0x{:x}(rdram, ctx, rt, {}); }}\n",
                               name, f.start, hex(f.start));
            for (uint32_t e : f.extraEntries)
                out += std::format("void {}(uint8_t *rdram, R5900Context *ctx, Runtime *rt) {{ body_0x{:x}(rdram, ctx, rt, {}); }}\n",
                                   entryName(f, e), f.start, hex(e));
        }
        out += "\n";
    }

    void Emitter::emitHeader(std::string &out) const
    {
        out += "// Generated by bozo-recomp: every recompiled function of the program. Do not edit.\n#pragma once\n\n"
               "#include \"bozo/recomp.h\"\n\n#include <cstddef>\n\n";
        for (const Function &f : a_.functions())
        {
            out += std::format("void {}(uint8_t *rdram, R5900Context *ctx, Runtime *rt);\n", cppName(f.symbol, f.start));
            for (uint32_t e : f.extraEntries)
                out += std::format("void {}(uint8_t *rdram, R5900Context *ctx, Runtime *rt);\n", entryName(f, e));
        }
        out += std::format("\n// Every function and extra entry by address, sorted.\nextern const BozoFunctionEntry {0}_function_table[];\n"
                           "extern const size_t {0}_function_count;\n",
                           opt_.prefix);
    }

    void Emitter::emitTable(std::string &out) const
    {
        std::vector<std::pair<uint32_t, std::string>> entries;
        for (const Function &f : a_.functions())
        {
            entries.emplace_back(f.start, cppName(f.symbol, f.start));
            for (uint32_t e : f.extraEntries)
                entries.emplace_back(e, entryName(f, e));
        }
        std::sort(entries.begin(), entries.end());
        out += std::format("// Generated by bozo-recomp. Do not edit.\n#include \"{}_functions.h\"\n\nconst BozoFunctionEntry {}_function_table[] = {{\n",
                           opt_.prefix, opt_.prefix);
        for (const auto &[addr, n] : entries)
            out += std::format("    {{0x{:x}u, {}}},\n", addr, n);
        out += std::format("}};\nconst size_t {0}_function_count = sizeof({0}_function_table) / sizeof({0}_function_table[0]);\n", opt_.prefix);
    }


    bool Emitter::emitAll(const std::string &dir, std::string &error)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::set<std::string> written;
        // A file is rewritten only when its contents change, so a build system recompiles only what changed.
        auto write = [&](const std::string &name, const std::string &text) -> bool {
            const fs::path path = fs::path(dir) / name;
            written.insert(name);
            stats_.bytes += text.size();
            {
                std::ifstream in(path, std::ios::binary);
                if (in)
                {
                    std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    if (old == text)
                        return true;
                }
            }
            std::ofstream o(path, std::ios::binary);
            o.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!o)
            {
                error = "cannot write " + path.string();
                return false;
            }
            ++stats_.filesWritten;
            return true;
        };

        std::string text;
        emitHeader(text);
        if (!write(opt_.prefix + "_functions.h", text))
            return false;
        text.clear();
        emitTable(text);
        if (!write(opt_.prefix + "_table.cpp", text))
            return false;

        const std::string prologue = std::format("// Generated by bozo-recomp. Do not edit.\n#include \"{}_functions.h\"\n\n", opt_.prefix);
        std::vector<std::string> bodies;
        bodies.reserve(a_.functions().size());
        size_t total = 0;
        for (const Function &f : a_.functions())
        {
            bodies.emplace_back();
            emitFunction(f, bodies.back());
            total += bodies.back().size();
            ++stats_.functions;
            stats_.entries += 1 + static_cast<uint32_t>(f.extraEntries.size());
        }
        // Split: at maxBytesPerFile, or into exactly opt_.files files of about the same size.
        const size_t limit = opt_.files ? (total + opt_.files - 1) / opt_.files : opt_.maxBytesPerFile;
        uint32_t fileIndex = 0;
        text = prologue;
        for (const std::string &b : bodies)
        {
            text += b;
            if (text.size() >= limit && (!opt_.files || fileIndex + 1 < opt_.files))
            {
                if (!write(std::format("{}_{:03}.cpp", opt_.prefix, fileIndex++), text))
                    return false;
                text = prologue;
            }
        }
        if (text.size() > prologue.size() || (opt_.files && fileIndex < opt_.files))
            if (!write(std::format("{}_{:03}.cpp", opt_.prefix, fileIndex++), text))
                return false;
        while (opt_.files && fileIndex < opt_.files) // fewer functions than files: empty ones keep the list fixed
            if (!write(std::format("{}_{:03}.cpp", opt_.prefix, fileIndex++), prologue))
                return false;
        stats_.files = fileIndex + 2;

        // Old generated files would otherwise be compiled with the new ones.
        for (const auto &e : fs::directory_iterator(dir, ec))
        {
            const std::string n = e.path().filename().string();
            if (n.rfind(opt_.prefix + "_", 0) == 0 && (e.path().extension() == ".cpp" || e.path().extension() == ".h") &&
                !written.count(n))
                fs::remove(e.path(), ec);
        }
        return true;
    }
}
