// Function discovery.
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

    void Analysis::collectSeeds(std::set<uint32_t> &seeds)
    {
        seeds.insert(program_.entry);
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
                        t.inPlace = hi.op == Op::LUI || hi.rt() == hi.rs();
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
        for (uint32_t p : codePointers_)
            addEntry(p, "code pointer");
        // Data words pointing at code, except jump table entries (those are labels of their own function).
        std::set<uint32_t> tableWords;
        for (const Function &f : functions_)
            for (const auto &[jr, t] : f.jumpTables)
                for (size_t k = 0; k < t.targets.size(); ++k)
                    tableWords.insert(t.tableAddress + 4u * static_cast<uint32_t>(k));
        for (uint32_t w : dataPointers_)
            if (!tableWords.count(w))
            {
                ++stats_.dataCodePointers;
                addEntry(program_.read32(w), "data pointer");
            }
    }

    void Analysis::run()
    {
        addSymbolFunctions();
        std::set<uint32_t> seeds;
        collectSeeds(seeds);
        carveGaps(seeds);
        for (Function &f : functions_)
            findJumpTables(f);
        addEntriesFromControlFlow();
        for (const Function &f : functions_)
            stats_.extraEntries += static_cast<uint32_t>(f.extraEntries.size());
    }
}
