// The R5900 instruction decoder: an instruction word to an operation and its fields.
#pragma once

#include <cstdint>

namespace bozo
{
    // What an instruction does to control flow (see ops.def).
    enum OpFlags : uint32_t
    {
        BR = 1u << 0,   // conditional branch, PC-relative, with a delay slot
        LK = 1u << 1,   // "likely": the delay slot is skipped when the branch is not taken
        JMP = 1u << 2,  // unconditional jump with a delay slot
        LNK = 1u << 3,  // writes the return address (ra, or rd for jalr)
        IND = 1u << 4,  // the target comes from a register
        TRAP = 1u << 5, // may raise an exception (overflow, trap, syscall, break)
        ENDS = 1u << 6, // never falls through to the next instruction
    };

    enum class Op : uint16_t
    {
#define OP(id, mnem, flags) id,
#include "ops.def"
#undef OP
        Count
    };

    constexpr unsigned kOpCount = static_cast<unsigned>(Op::Count);

    // A decoded instruction: the word, its address and the operation. The field accessors follow the MIPS formats;
    // which ones mean something depends on the operation.
    struct Insn
    {
        uint32_t word = 0;
        uint32_t addr = 0;
        Op op = Op::Unknown;

        uint32_t opcode() const { return word >> 26; }
        uint32_t rs() const { return (word >> 21) & 31u; }
        uint32_t rt() const { return (word >> 16) & 31u; }
        uint32_t rd() const { return (word >> 11) & 31u; }
        uint32_t sa() const { return (word >> 6) & 31u; }
        uint32_t funct() const { return word & 63u; }
        uint32_t uimm() const { return word & 0xFFFFu; }
        int32_t simm() const { return static_cast<int16_t>(word & 0xFFFFu); }
        // FPU register fields (COP1): fmt = rs, ft = rt, fs = rd, fd = sa.
        uint32_t ft() const { return rt(); }
        uint32_t fs() const { return rd(); }
        uint32_t fd() const { return sa(); }
        // VU0 macro fields (COP2): the dest mask (x = bit 3 ... w = bit 0), the broadcast field, fsf/ftf.
        uint32_t vdest() const { return (word >> 21) & 15u; }
        uint32_t vbc() const { return word & 3u; }
        uint32_t vfsf() const { return (word >> 21) & 3u; }
        uint32_t vftf() const { return (word >> 23) & 3u; }

        // Targets.
        uint32_t branchTarget() const { return addr + 4u + (static_cast<uint32_t>(simm()) << 2); }
        uint32_t jumpTarget() const { return ((addr + 4u) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2); }
    };

    Op decode(uint32_t word);
    inline Insn decodeAt(uint32_t word, uint32_t addr) { return Insn{word, addr, decode(word)}; }

    const char *opName(Op op);
    uint32_t opFlags(Op op);
    inline bool isBranch(Op op) { return (opFlags(op) & BR) != 0; }
    inline bool isLikely(Op op) { return (opFlags(op) & LK) != 0; }
    inline bool isJump(Op op) { return (opFlags(op) & JMP) != 0; }
    inline bool hasDelaySlot(Op op) { return (opFlags(op) & (BR | JMP)) != 0; }

    // A one-line disassembly, for comments in the generated code and for reports.
    void disassemble(const Insn &insn, char *out, unsigned size);
}
