// Function discovery. An executable whose sized function symbols cover at least half of its code goes by steps 1 to 4;
// any other is discovered from its code (docs/specs/discovery.md, discoverFunctions below).
//
// 1. Every ELF function symbol (STT_FUNC with a size) inside the code is a function with the symbol's range.
// 2. Code no symbol covers (C++ thunks, hand-written assembly, padding) is carved into functions: each one starts at
//    an address something points at (the entry point, a jal or branch target, a symbol of any type, a code pointer in
//    data or built by lui/addiu) or at the first non-zero word of the gap, and runs to the next start.
// 3. Jump tables: a `jr` whose register was loaded from (lui hi) + index + lo is a switch; its table's entries become
//    labels of the function.
// 4. Entries into the middle of a function (a jal there, a branch or jump from another function, a code pointer that
//    is not a jump table entry) are recorded as extra entries: the emitter lets the function be entered there.
// A function that runs off its end continues into the next one (the emitter tail-calls it), so a wrong split between
// two pieces of code costs a call, never correctness.
#include "analysis.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace bozo
{
    namespace
    {
        // The general register an instruction writes, or -1 (enough for the jump table patterns: ALU, loads, calls).
        int gprWritten(const Insn &i)
        {
            switch (i.op)
            {
            case Op::ADDI: case Op::ADDIU: case Op::SLTI: case Op::SLTIU: case Op::ANDI: case Op::ORI: case Op::XORI:
            case Op::LUI: case Op::DADDI: case Op::DADDIU: case Op::LB: case Op::LH: case Op::LW: case Op::LBU:
            case Op::LHU: case Op::LWU: case Op::LD: case Op::LQ: case Op::LWL: case Op::LWR: case Op::LDL:
            case Op::LDR: case Op::MFC1: case Op::CFC1: case Op::MFC0: case Op::QMFC2: case Op::CFC2:
                return static_cast<int>(i.rt());
            case Op::JAL:
                return 31;
            case Op::JALR:
                return static_cast<int>(i.rd());
            default:
                break;
            }
            const uint32_t opc = i.opcode();
            if ((opc == 0 && i.op != Op::JR && i.op != Op::SYSCALL && i.op != Op::BREAK && i.op != Op::SYNC &&
                 i.op != Op::MTHI && i.op != Op::MTLO && i.op != Op::MTSA) ||
                opc == 28)
                return static_cast<int>(i.rd());
            return -1;
        }

        // Execution never continues past this instruction's delay slot.
        bool endsFlow(const Insn &i)
        {
            if (i.op == Op::J || i.op == Op::JR)
                return true;
            // beq r, r and bgez zero: `b`.
            return (i.op == Op::BEQ && i.rs() == i.rt()) || (i.op == Op::BGEZ && i.rs() == 0);
        }

        // addiu sp, sp, -n: a function setting up its frame.
        bool isFrameAllocation(const Insn &i)
        {
            return i.op == Op::ADDIU && i.rs() == 29 && i.rt() == 29 && i.simm() < 0;
        }

        std::string sanitize(const std::string &name)
        {
            std::string out;
            out.reserve(name.size());
            for (char c : name)
            {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                out += ok ? c : '_';
            }
            if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
                out = "_" + out;
            return out;
        }
    }

    std::string cppName(const std::string &symbol, uint32_t address)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "_0x%x", address);
        return (symbol.empty() ? std::string("func") : sanitize(symbol)) + buf;
    }

    const Function *Analysis::functionContaining(uint32_t addr) const
    {
        auto it = std::upper_bound(functions_.begin(), functions_.end(), addr,
                                   [](uint32_t a, const Function &f) { return a < f.start; });
        if (it == functions_.begin())
            return nullptr;
        --it;
        return addr < it->end ? &*it : nullptr;
    }

    Function *Analysis::mutableContaining(uint32_t addr)
    {
        return const_cast<Function *>(functionContaining(addr));
    }

    const Function *Analysis::functionAt(uint32_t addr) const
    {
        const Function *f = functionContaining(addr);
        return f && f->start == addr ? f : nullptr;
    }

    bool Analysis::isEntry(uint32_t addr) const
    {
        const Function *f = functionContaining(addr);
        return f && (f->start == addr || f->extraEntries.count(addr) != 0);
    }

    void Analysis::addSymbolFunctions()
    {
        std::map<uint32_t, const ProgramSymbol *> best; // one function per address: the first one listed
        std::map<uint32_t, int> namePriority;           // the name kept for an address: a function's over a label's
        for (const auto &s : program_.symbols)
        {
            if (!program_.isCode(s.address))
                continue;
            // Compiler markers (gcc2_compiled., __gnu_compiled_c...) name no code.
            const bool marker = s.name.rfind("gcc2_compiled", 0) == 0 || s.name.rfind("__gnu_compiled", 0) == 0;
            const int priority = s.function ? 2 : 1;
            auto known = namePriority.find(s.address);
            if (!marker && (known == namePriority.end() || priority > known->second))
            {
                namePriority[s.address] = priority;
                anySymbolAt_[s.address] = s.name;
            }
            if (!s.function || s.size == 0 || (s.address & 3u))
                continue;
            if (!best.count(s.address))
                best[s.address] = &s;
        }
        for (const auto &[addr, s] : best)
        {
            Function f;
            f.start = addr;
            f.end = addr + s->size;
            for (const auto &r : program_.code)
                if (r.contains(addr))
                    f.end = std::min(f.end, r.end);
            f.symbol = s->name;
            f.fromSymbol = true;
            functions_.push_back(std::move(f));
        }
        // Overlaps (should not happen in a sane ELF): the earlier function ends where the next one starts.
        for (size_t i = 0; i + 1 < functions_.size(); ++i)
            if (functions_[i].end > functions_[i + 1].start)
            {
                stats_.warnings.push_back("function symbols overlap at 0x" + std::to_string(functions_[i + 1].start));
                functions_[i].end = functions_[i + 1].start;
            }
        stats_.symbolFunctions = static_cast<uint32_t>(functions_.size());
    }

    void Analysis::collectBuiltPointers(const Insn &lui, uint32_t rangeEnd)
    {
        const uint32_t reg = lui.rt();
        const bool saved = (reg >= 16 && reg <= 23) || reg == 28 || reg == 30;
        bool lastSlot = false;
        for (uint32_t b = lui.addr + 4; b < lui.addr + 256 && b + 4 <= rangeEnd; b += 4)
        {
            const Insn n = decodeAt(program_.read32(b), b);
            if ((n.op == Op::ADDIU || n.op == Op::ORI) && n.rs() == reg)
            {
                const uint32_t v = n.op == Op::ADDIU ? (lui.uimm() << 16) + static_cast<uint32_t>(n.simm())
                                                     : (lui.uimm() << 16) | n.uimm();
                if (program_.isCode(v) && (v & 3u) == 0)
                    codePointers_.insert(v);
            }
            if (lastSlot || gprWritten(n) == static_cast<int>(reg))
                return;
            // The instruction in the delay slot still sees the register; after a call only a saved one holds.
            lastSlot = endsFlow(n) || (!saved && (n.op == Op::JAL || n.op == Op::JALR));
        }
    }

    void Analysis::collectSeeds(std::set<uint32_t> &seeds)
    {
        seeds.insert(program_.entry);
        seeds.insert(program_.entries.begin(), program_.entries.end());
        std::set<uint32_t> calls;
        for (const auto &r : program_.code)
        {
            for (uint32_t a = r.start; a + 4 <= r.end; a += 4)
            {
                const Insn i = decodeAt(program_.read32(a), a);
                if (i.op == Op::JAL)
                {
                    calls.insert(i.jumpTarget());
                    seeds.insert(i.jumpTarget());
                }
                else if (i.op == Op::J)
                    seeds.insert(i.jumpTarget());
                else if (isBranch(i.op))
                {
                    seeds.insert(i.branchTarget());
                    if (opFlags(i.op) & LNK)
                        calls.insert(i.branchTarget());
                }
                else if (i.op == Op::LUI)
                {
                    // A code address built by lui + addiu/ori in the next few instructions (a function pointer).
                    const uint32_t reg = i.rt();
                    if (!stats_.symbolDriven)
                    {
                        collectBuiltPointers(i, r.end);
                        continue;
                    }
                    for (uint32_t b = a + 4; b < a + 40 && b + 4 <= r.end; b += 4)
                    {
                        const Insn n = decodeAt(program_.read32(b), b);
                        if ((n.op == Op::ADDIU || n.op == Op::ORI) && n.rs() == reg)
                        {
                            const uint32_t v = n.op == Op::ADDIU ? (i.uimm() << 16) + static_cast<uint32_t>(n.simm())
                                                                 : (i.uimm() << 16) | n.uimm();
                            if (program_.isCode(v) && (v & 3u) == 0)
                                codePointers_.insert(v);
                            break;
                        }
                        if (gprWritten(n) == static_cast<int>(reg) || hasDelaySlot(n.op))
                            break;
                    }
                }
            }
        }
        callTargets_ = calls;
        stats_.callTargets = static_cast<uint32_t>(calls.size());
        stats_.codePointerConstants = static_cast<uint32_t>(codePointers_.size());
        seeds.insert(calls.begin(), calls.end());
        seeds.insert(codePointers_.begin(), codePointers_.end());
        // Code pointers in initialised data (vtables, callback tables, jump tables).
        for (const auto &r : program_.data)
            for (uint32_t a = (r.start + 3u) & ~3u; a + 4 <= r.end; a += 4)
            {
                const uint32_t v = program_.read32(a);
                if ((v & 3u) == 0 && program_.isCode(v))
                    dataPointers_.insert(a);
            }
        for (uint32_t a : dataPointers_)
            seeds.insert(program_.read32(a));
        for (const auto &[addr, name] : anySymbolAt_)
            seeds.insert(addr);
    }

    void Analysis::carveGaps(const std::set<uint32_t> &seeds)
    {
        std::vector<Function> added;
        for (const auto &r : program_.code)
        {
            uint32_t a = r.start;
            while (a < r.end)
            {
                const Function *f = functionContaining(a);
                if (f)
                {
                    a = f->end;
                    continue;
                }
                // A gap: up to the next function or the end of the range.
                auto next = std::upper_bound(functions_.begin(), functions_.end(), a,
                                             [](uint32_t x, const Function &g) { return x < g.start; });
                const uint32_t gapEnd = (next != functions_.end() && next->start < r.end) ? next->start : r.end;
                std::vector<uint32_t> starts;
                for (auto it = seeds.lower_bound(a); it != seeds.end() && *it < gapEnd; ++it)
                    if ((*it & 3u) == 0)
                        starts.push_back(*it);
                // Code before the first seed: start at its first non-zero word.
                const uint32_t firstSeed = starts.empty() ? gapEnd : starts.front();
                for (uint32_t b = a; b < firstSeed; b += 4)
                    if (program_.read32(b) != 0)
                    {
                        starts.insert(starts.begin(), b);
                        break;
                    }
                for (size_t k = 0; k < starts.size(); ++k)
                {
                    Function g;
                    g.start = starts[k];
                    g.end = k + 1 < starts.size() ? starts[k + 1] : gapEnd;
                    auto sym = anySymbolAt_.find(g.start);
                    if (sym != anySymbolAt_.end())
                        g.symbol = sym->second;
                    added.push_back(std::move(g));
                }
                a = gapEnd;
            }
        }
        stats_.gapFunctions = static_cast<uint32_t>(added.size());
        functions_.insert(functions_.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
        std::sort(functions_.begin(), functions_.end(), [](const Function &x, const Function &y) { return x.start < y.start; });
    }

    void Analysis::findJumpTables(Function &f)
    {
        // The instruction defining `reg` before `from`, looking back at most `limit` instructions in the function.
        auto findDef = [&](uint32_t from, uint32_t reg, int limit, Insn &out) -> bool {
            for (uint32_t a = from; a > f.start && limit-- > 0;)
            {
                a -= 4;
                const Insn i = decodeAt(program_.read32(a), a);
                if (gprWritten(i) == static_cast<int>(reg))
                {
                    out = i;
                    return true;
                }
            }
            return false;
        };

        for (uint32_t a = f.start; a + 4 <= f.end; a += 4)
        {
            const Insn jr = decodeAt(program_.read32(a), a);
            if (jr.op != Op::JR || jr.rs() == 31)
                continue;
            bool found = false;
            Insn load;
            if (findDef(a, jr.rs(), 16, load) && (load.op == Op::LW || load.op == Op::LWU))
            {
                Insn sum;
                if (findDef(load.addr, load.rs(), 16, sum) && (sum.op == Op::ADDU || sum.op == Op::DADDU))
                {
                    for (uint32_t reg : {sum.rs(), sum.rt()})
                    {
                        Insn hi;
                        if (!findDef(sum.addr, reg, 16, hi))
                            continue;
                        uint32_t base = 0;
                        if (hi.op == Op::LUI)
                            base = (hi.uimm() << 16) + static_cast<uint32_t>(load.simm());
                        else if (hi.op == Op::ADDIU)
                        {
                            Insn lui;
                            // The base may be kept in a saved register set up far earlier (a loop-invariant lui).
                            if (!findDef(hi.addr, hi.rs(), 4096, lui) || lui.op != Op::LUI)
                                continue;
                            base = (lui.uimm() << 16) + static_cast<uint32_t>(hi.simm()) + static_cast<uint32_t>(load.simm());
                        }
                        else
                            continue;
                        // The bound: the nearest sltiu before the load (the switch's range check), if any.
                        uint32_t bound = 4096;
                        for (uint32_t b = load.addr; b > f.start && b + 96 > load.addr;)
                        {
                            b -= 4;
                            const Insn s = decodeAt(program_.read32(b), b);
                            if (s.op == Op::SLTIU)
                            {
                                bound = s.uimm();
                                break;
                            }
                        }
                        JumpTable t;
                        t.jrAddress = a;
                        t.tableAddress = base;
                        // The reference resolves a table it can bound: its base built in place and an sltiu range check.
                        t.inPlace = (hi.op == Op::LUI || hi.rt() == hi.rs()) && bound != 4096;
                        for (uint32_t k = 0; k < bound; ++k)
                        {
                            const uint32_t e = base + 4 * k;
                            if (!program_.contains(e))
                                break;
                            const uint32_t target = program_.read32(e);
                            if (target < f.start || target >= f.end || (target & 3u))
                                break;
                            t.targets.push_back(target);
                        }
                        if (!t.targets.empty())
                        {
                            if (bound != 4096 && t.targets.size() != bound)
                                stats_.warnings.push_back("jump table at " + std::to_string(base) + " shorter than its bound");
                            stats_.jumpTableTargets += static_cast<uint32_t>(t.targets.size());
                            f.jumpTables[a] = std::move(t);
                            found = true;
                            break;
                        }
                    }
                }
            }
            if (!found)
            {
                // A return through a copy of ra (hand-written code that saves ra in another register around a
                // call): the register was set by a move from ra, and nothing wrote ra before that move.
                Insn move;
                if (findDef(a, jr.rs(), 4096, move) &&
                    (move.op == Op::ADDU || move.op == Op::DADDU || move.op == Op::OR) &&
                    ((move.rs() == 31 && move.rt() == 0) || (move.rs() == 0 && move.rt() == 31)))
                {
                    Insn earlier;
                    if (!findDef(move.addr, 31, 4096, earlier))
                    {
                        f.returnJr.insert(a);
                        found = true;
                    }
                }
            }
            if (!found)
                f.unresolvedJr.insert(a);
        }
        stats_.returnJr += static_cast<uint32_t>(f.returnJr.size());
        stats_.jumpTables += static_cast<uint32_t>(f.jumpTables.size());
        stats_.unresolvedJr += static_cast<uint32_t>(f.unresolvedJr.size());
    }

    bool Analysis::isCopyOfEntryRa(const Function &f, uint32_t jrAddress, uint32_t reg) const
    {
        const auto findDef = [&](uint32_t from, uint32_t wanted, Insn &def) {
            for (uint32_t a = from; a > f.start;)
            {
                a -= 4;
                const Insn i = decodeAt(program_.read32(a), a);
                if (gprWritten(i) == static_cast<int>(wanted))
                {
                    def = i;
                    return true;
                }
            }
            return false;
        };
        Insn move, earlier;
        return findDef(jrAddress, reg, move) && (move.op == Op::ADDU || move.op == Op::DADDU || move.op == Op::OR) &&
               ((move.rs() == 31 && move.rt() == 0) || (move.rs() == 0 && move.rt() == 31)) &&
               !findDef(move.addr, 31, earlier);
    }

    bool Analysis::plausibleStart(uint32_t addr) const
    {
        if ((addr & 3u) || !program_.isCode(addr))
            return false;
        const uint32_t word = program_.read32(addr);
        if (word == 0 || decode(word) == Op::Unknown)
            return false;
        // The delay slot of a branch is never where a function starts.
        return !(program_.isCode(addr - 4) && hasDelaySlot(decode(program_.read32(addr - 4))));
    }

    bool Analysis::followsTerminator(uint32_t addr) const
    {
        uint32_t a = addr;
        for (int padding = 0; padding < 16 && program_.isCode(a - 4) && program_.read32(a - 4) == 0; ++padding)
            a -= 4;
        if (!program_.isCode(a - 4))
            return true;
        // Padding is zero words, and so can be the delay slot before it.
        for (uint32_t b : {a - 8, a - 4, addr - 8})
            if (program_.isCode(b) && endsFlow(decodeAt(program_.read32(b), b)))
                return true;
        const Op last = decode(program_.read32(a - 4));
        return last == Op::SYSCALL || last == Op::BREAK || last == Op::ERET;
    }

    bool Analysis::afterEnd(uint32_t addr, uint32_t floor, bool nothingAhead) const
    {
        uint32_t a = addr;
        while (a > floor && program_.read32(a - 4) == 0)
            a -= 4;
        const auto insn = [&](uint32_t at) { return at >= floor ? decodeAt(program_.read32(at), at) : Insn{}; };
        for (uint32_t at : {a - 4, a - 8})
        {
            const Insn i = insn(at);
            if (endsFlow(i))
                return true;
            if (nothingAhead && (i.op == Op::JAL || i.op == Op::JALR))
                return true;
        }
        return nothingAhead && (insn(a - 4).op == Op::SYSCALL || insn(a - 4).op == Op::BREAK);
    }

    bool Analysis::resolveTable(uint32_t jrAddress, uint32_t scanStart, uint32_t low, uint32_t high, JumpTable &out) const
    {
        const auto findDef = [&](uint32_t from, uint32_t reg, int window, Insn &def) {
            for (uint32_t a = from; a > scanStart && window-- > 0;)
            {
                a -= 4;
                const Insn i = decodeAt(program_.read32(a), a);
                if (gprWritten(i) == static_cast<int>(reg))
                {
                    def = i;
                    return true;
                }
            }
            return false;
        };
        const Insn jr = decodeAt(program_.read32(jrAddress), jrAddress);
        Insn load, sum;
        if (!findDef(jrAddress, jr.rs(), 32, load))
            return false;
        if (load.op == Op::ADDU || load.op == Op::DADDU)
        {
            // A jump to base + (index << k): slots of code at a fixed stride, each a target.
            for (const auto &[baseReg, indexReg] : {std::pair{load.rs(), load.rt()}, std::pair{load.rt(), load.rs()}})
            {
                Insn add, lui, shift;
                if (!findDef(load.addr, baseReg, 32, add) || add.op != Op::ADDIU || !findDef(add.addr, add.rs(), 32, lui) ||
                    lui.op != Op::LUI || !findDef(load.addr, indexReg, 32, shift) || shift.op != Op::SLL ||
                    shift.sa() < 2 || shift.sa() > 8)
                    continue;
                const uint32_t base = (lui.uimm() << 16) + static_cast<uint32_t>(add.simm());
                if ((base & 3u) || base < low || base >= high)
                    continue;
                out.jrAddress = jrAddress;
                out.tableAddress = base;
                out.inPlace = true;
                out.slots = true;
                out.targets.clear();
                for (uint32_t t = base; t < high; t += 1u << shift.sa())
                    out.targets.push_back(t);
                return true;
            }
            return false;
        }
        if (load.op != Op::LW && load.op != Op::LWU)
            return false;
        if (!findDef(load.addr, load.rs(), 32, sum) || (sum.op != Op::ADDU && sum.op != Op::DADDU))
            return false;
        for (uint32_t reg : {sum.rs(), sum.rt()})
        {
            Insn hi, lui;
            uint32_t base = 0;
            if (!findDef(sum.addr, reg, 32, hi))
                continue;
            if (hi.op == Op::LUI)
                base = (hi.uimm() << 16) + static_cast<uint32_t>(load.simm());
            else if (hi.op == Op::ADDIU && findDef(hi.addr, hi.rs(), 4096, lui) && lui.op == Op::LUI)
                base = (lui.uimm() << 16) + static_cast<uint32_t>(hi.simm()) + static_cast<uint32_t>(load.simm());
            else if (hi.op == Op::ORI && findDef(hi.addr, hi.rs(), 4096, lui) && lui.op == Op::LUI)
                base = ((lui.uimm() << 16) | hi.uimm()) + static_cast<uint32_t>(load.simm());
            else
                continue;
            if ((base & 3u) || !program_.contains(base))
                continue;
            uint32_t bound = 0;
            for (uint32_t b = load.addr; b > scanStart && b + 128 > load.addr && !bound;)
            {
                b -= 4;
                const Insn s = decodeAt(program_.read32(b), b);
                if (s.op == Op::SLTIU && s.uimm() != 0)
                    bound = s.uimm();
                else if (s.op == Op::ANDI && gprWritten(s) != 0 && s.uimm() < 4096 && (s.uimm() & (s.uimm() + 1)) == 0)
                    bound = s.uimm() + 1;
            }
            out.jrAddress = jrAddress;
            out.tableAddress = base;
            out.inPlace = true;
            out.targets.clear();
            const uint32_t count = bound ? bound : 4096;
            for (uint32_t k = 0; k < count && program_.contains(base + 4 * k); ++k)
            {
                const uint32_t target = program_.read32(base + 4 * k);
                if (target < low || target >= high || (target & 3u))
                    break;
                out.targets.push_back(target);
            }
            if (!out.targets.empty())
                return true;
        }
        return false;
    }

    void Analysis::sweepGap(uint32_t from, uint32_t to, const std::set<uint32_t> &hard, std::map<uint32_t, bool> &starts) const
    {
        uint32_t a = from;
        while (a < to && program_.read32(a) == 0 && !hard.count(a))
            a += 4;
        if (a >= to)
            return;
        uint32_t current = a, reach = 0;
        starts[current] = hard.count(current) != 0;
        auto nextHard = hard.upper_bound(a);
        for (; a + 4 <= to; a += 4)
        {
            if (a != current && hard.count(a))
            {
                current = a;
                reach = 0;
                starts[a] = true;
            }
            while (nextHard != hard.end() && *nextHard <= a)
                ++nextHard;
            const uint32_t limit = nextHard != hard.end() && *nextHard < to ? *nextHard : to;
            const Insn i = decodeAt(program_.read32(a), a);
            // Not an instruction: data among the code, whose "branches" say nothing.
            if (i.op == Op::Unknown)
                reach = 0;
            const auto reached = [&](uint32_t t) {
                if (t > a && t < limit)
                    reach = std::max(reach, t);
            };
            if (a != current && isFrameAllocation(i) && afterEnd(a, current, a >= reach))
            {
                current = a;
                reach = 0;
                starts[a] = false;
            }
            if (isBranch(i.op))
                reached(i.branchTarget());
            else if (i.op == Op::J)
            {
                // A jump to the next instruction is a tail call to the function placed right after this one.
                if (i.jumpTarget() != a + 8)
                    reached(i.jumpTarget());
            }
            else if (i.op == Op::JR && i.rs() != 31)
            {
                JumpTable table;
                if (resolveTable(a, current, current, limit, table))
                {
                    if (table.slots)
                        reached(table.targets.front());
                    else
                        for (uint32_t t : table.targets)
                            reached(t);
                }
            }
            if (!endsFlow(i) || reach >= a + 8 || a + 8 > limit)
                continue;
            uint32_t next = a + 8;
            while (next < limit && program_.read32(next) == 0)
                next += 4;
            if (next < limit)
            {
                current = next;
                reach = 0;
                starts[next] = false;
            }
            a = next - 4;
        }
    }

    void Analysis::mergeSplitBodies(std::map<uint32_t, bool> &starts) const
    {
        for (bool changed = true; changed;)
        {
            changed = false;
            std::vector<uint32_t> drop;
            for (auto it = starts.begin(); it != starts.end(); ++it)
            {
                const auto next = std::next(it);
                const uint32_t start = it->first;
                if (functionAt(start))
                    continue;
                const auto range = std::find_if(program_.code.begin(), program_.code.end(),
                                                [&](const AddressRange &r) { return r.contains(start); });
                const uint32_t end = next != starts.end() ? std::min(next->first, range->end) : range->end;
                // A table's targets can only be in pieces nothing separates from this one.
                uint32_t windowLow = start, windowHigh = end;
                for (auto low = it; low != starts.begin() && !low->second; --low)
                    windowLow = std::prev(low)->first;
                for (auto high = next; high != starts.end() && !high->second; ++high)
                    windowHigh = std::next(high) != starts.end() ? std::next(high)->first : high->first;
                const auto refer = [&](uint32_t t) {
                    if (t >= start && t < end)
                        return;
                    auto holder = starts.upper_bound(t);
                    if (holder == starts.begin())
                        return;
                    --holder;
                    if (holder->first == t || !range->contains(t))
                        return;
                    // A direct transfer into another piece's body: the pieces are one function, when nothing proves
                    // a function starts between them.
                    const uint32_t low = std::min(t, start), high = std::max(t, start);
                    auto b = starts.upper_bound(low);
                    std::vector<uint32_t> between;
                    for (; b != starts.end() && b->first <= high; ++b)
                    {
                        if (b->second || isFrameAllocation(decodeAt(program_.read32(b->first), b->first)))
                            return;
                        between.push_back(b->first);
                    }
                    drop.insert(drop.end(), between.begin(), between.end());
                };
                for (uint32_t a = start; a < end; a += 4)
                {
                    const Insn i = decodeAt(program_.read32(a), a);
                    if (i.op == Op::Unknown)
                        break;
                    if (isBranch(i.op) && !(opFlags(i.op) & LNK))
                        refer(i.branchTarget());
                    else if (i.op == Op::J)
                        refer(i.jumpTarget());
                    else if (i.op == Op::JR && i.rs() != 31)
                    {
                        JumpTable table;
                        if (resolveTable(a, start, windowLow, windowHigh, table) && !table.slots)
                            for (uint32_t t : table.targets)
                                refer(t);
                    }
                }
            }
            for (uint32_t d : drop)
                changed |= starts.erase(d) != 0;
        }
    }

    void Analysis::discoverFunctions()
    {
        std::set<uint32_t> seeds;
        collectSeeds(seeds);
        std::set<uint32_t> hard;
        if (program_.isCode(program_.entry))
            hard.insert(program_.entry);
        for (uint32_t a : program_.entries)
            hard.insert(a);
        for (const auto &[a, name] : anySymbolAt_)
            if (!(a & 3u))
                hard.insert(a);
        const auto opensFrame = [&](uint32_t a) { return isFrameAllocation(decodeAt(program_.read32(a), a)); };
        for (uint32_t a : callTargets_)
            if ((plausibleStart(a) && followsTerminator(a)) || (program_.isCode(a) && opensFrame(a)))
                hard.insert(a);
        for (uint32_t a : codePointers_)
            if (plausibleStart(a) && opensFrame(a))
                hard.insert(a);
        for (uint32_t w : dataPointers_)
        {
            const uint32_t a = program_.read32(w);
            if (plausibleStart(a) && opensFrame(a))
                hard.insert(a);
        }
        for (const Function &f : functions_)
            hard.erase(f.start);

        std::map<uint32_t, bool> starts;
        for (const auto &r : program_.code)
        {
            uint32_t a = r.start;
            while (a < r.end)
            {
                if (const Function *f = functionContaining(a))
                {
                    a = f->end;
                    continue;
                }
                auto next = std::upper_bound(functions_.begin(), functions_.end(), a,
                                             [](uint32_t x, const Function &g) { return x < g.start; });
                const uint32_t gapEnd = (next != functions_.end() && next->start < r.end) ? next->start : r.end;
                sweepGap(a, gapEnd, hard, starts);
                a = gapEnd;
            }
        }
        for (const Function &f : functions_)
            starts[f.start] = true;
        mergeSplitBodies(starts);
        for (const Function &f : functions_)
            starts.erase(f.start);

        std::vector<Function> added;
        for (auto it = starts.begin(); it != starts.end(); ++it)
        {
            Function g;
            g.start = it->first;
            uint32_t end = g.start;
            for (const auto &r : program_.code)
                if (r.contains(g.start))
                    end = r.end;
            const auto next = std::next(it);
            if (next != starts.end())
                end = std::min(end, next->first);
            auto after = std::upper_bound(functions_.begin(), functions_.end(), g.start,
                                          [](uint32_t x, const Function &h) { return x < h.start; });
            if (after != functions_.end())
                end = std::min(end, after->start);
            // A section can end inside a word, which is no instruction.
            g.end = end & ~3u;
            auto sym = anySymbolAt_.find(g.start);
            if (sym != anySymbolAt_.end())
                g.symbol = sym->second;
            added.push_back(std::move(g));
        }
        stats_.gapFunctions = static_cast<uint32_t>(added.size());
        functions_.insert(functions_.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
        std::sort(functions_.begin(), functions_.end(), [](const Function &x, const Function &y) { return x.start < y.start; });

        for (Function &f : functions_)
        {
            for (uint32_t a = f.start; a + 4 <= f.end; a += 4)
            {
                const Insn jr = decodeAt(program_.read32(a), a);
                if (jr.op != Op::JR || jr.rs() == 31)
                    continue;
                JumpTable t;
                if (resolveTable(a, f.start, f.start, f.end, t))
                {
                    stats_.jumpTableTargets += static_cast<uint32_t>(t.targets.size());
                    f.jumpTables[a] = std::move(t);
                }
                else if (isCopyOfEntryRa(f, a, jr.rs()))
                    f.returnJr.insert(a);
                else
                    f.unresolvedJr.insert(a);
            }
            stats_.returnJr += static_cast<uint32_t>(f.returnJr.size());
            stats_.jumpTables += static_cast<uint32_t>(f.jumpTables.size());
            stats_.unresolvedJr += static_cast<uint32_t>(f.unresolvedJr.size());
        }
    }

    std::string Analysis::report() const
    {
        std::string out;
        char line[128];
        const auto put = [&](const char *kind, uint32_t a, uint32_t b, const std::string &text) {
            std::snprintf(line, sizeof(line), "%s 0x%x 0x%x ", kind, a, b);
            out += line;
            out += text;
            out += '\n';
        };
        out += stats_.symbolDriven ? "mode symbols\n" : "mode code\n";
        for (const Function &f : functions_)
        {
            put("function", f.start, f.end, f.symbol);
            for (uint32_t e : f.extraEntries)
                put("entry", e, f.start, "");
            for (const auto &[jr, t] : f.jumpTables)
            {
                std::string targets = t.inPlace ? "goto" : "dispatch";
                for (uint32_t target : t.targets)
                {
                    std::snprintf(line, sizeof(line), " 0x%x", target);
                    targets += line;
                }
                put("table", jr, t.tableAddress, targets);
            }
            for (uint32_t jr : f.returnJr)
                put("return", jr, f.start, "");
            for (uint32_t jr : f.unresolvedJr)
                put("unresolved", jr, f.start, "");
        }
        return out;
    }

    void Analysis::addEntry(uint32_t addr, const char *why)
    {
        Function *f = mutableContaining(addr);
        if (!f)
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s 0x%x is outside every function", why, addr);
            stats_.warnings.push_back(buf);
            return;
        }
        if (f->start == addr || (addr & 3u))
            return;
        if (f->extraEntries.insert(addr).second)
        {
            auto sym = anySymbolAt_.find(addr);
            if (sym != anySymbolAt_.end())
                f->entrySymbols[addr] = sym->second;
        }
    }

    void Analysis::addEntriesFromControlFlow()
    {
        // Calls, branches and jumps that land in the middle of another function, and calls into the middle of any.
        for (const Function &f : functions_)
        {
            for (uint32_t a = f.start; a + 4 <= f.end; a += 4)
            {
                const Insn i = decodeAt(program_.read32(a), a);
                if (hasDelaySlot(i.op) && a + 8 <= f.end && hasDelaySlot(decode(program_.read32(a + 4))))
                    ++stats_.branchesInDelaySlots;
                if (i.op == Op::JAL)
                    addEntry(i.jumpTarget(), "call target");
                else if (i.op == Op::J)
                {
                    if (i.jumpTarget() < f.start || i.jumpTarget() >= f.end)
                        addEntry(i.jumpTarget(), "jump target");
                }
                else if (isBranch(i.op))
                {
                    const uint32_t t = i.branchTarget();
                    if ((opFlags(i.op) & LNK) || t < f.start || t >= f.end)
                        addEntry(t, "branch target");
                }
            }
        }
        addEntry(program_.entry, "entry point");
        for (uint32_t entry : program_.entries)
            addEntry(entry, "explicit entry");
        const auto pointerCounts = [&](uint32_t target) { return stats_.symbolDriven || plausibleStart(target); };
        for (uint32_t p : codePointers_)
            if (pointerCounts(p))
                addEntry(p, "code pointer");
        // Data words pointing at code, except jump table entries (those are labels of their own function).
        std::set<uint32_t> tableWords;
        for (const Function &f : functions_)
            for (const auto &[jr, t] : f.jumpTables)
                for (size_t k = 0; k < t.targets.size() && !t.slots; ++k)
                    tableWords.insert(t.tableAddress + 4u * static_cast<uint32_t>(k));
        for (uint32_t w : dataPointers_)
            if (!tableWords.count(w) && pointerCounts(program_.read32(w)))
            {
                ++stats_.dataCodePointers;
                addEntry(program_.read32(w), "data pointer");
            }
    }

    void Analysis::run()
    {
        addSymbolFunctions();
        uint64_t codeBytes = 0, symbolBytes = 0;
        for (const auto &r : program_.code)
            codeBytes += r.end - r.start;
        for (const Function &f : functions_)
            symbolBytes += f.end - f.start;
        stats_.symbolDriven = symbolBytes * 2 >= codeBytes;
        if (stats_.symbolDriven)
        {
            std::set<uint32_t> seeds;
            collectSeeds(seeds);
            carveGaps(seeds);
            for (Function &f : functions_)
                findJumpTables(f);
        }
        else
            discoverFunctions();
        addEntriesFromControlFlow();
        for (const Function &f : functions_)
            stats_.extraEntries += static_cast<uint32_t>(f.extraEntries.size());
    }
}
