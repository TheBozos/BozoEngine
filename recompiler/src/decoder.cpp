// The R5900 instruction decoder. Encodings from the MIPS III/IV formats plus the R5900's own opcode space (MMI at
// primary opcode 28, LQ/SQ at 30/31, the FPU's accumulator operations, VU0 macro mode under COP2). Anything not listed
// here decodes to Op::Unknown, so a census of a program shows exactly what we do not handle.
#include "decoder.h"

#include <cstdio>

namespace bozo
{
    namespace
    {
        struct OpInfo
        {
            const char *name;
            uint32_t flags;
        };

        constexpr OpInfo kOps[] = {
#define OP(id, mnem, flags) {mnem, flags},
#include "ops.def"
#undef OP
        };
        static_assert(sizeof(kOps) / sizeof(kOps[0]) == kOpCount, "ops.def and the table disagree");

        constexpr Op U = Op::Unknown;

        // SPECIAL (opcode 0), by function field.
        constexpr Op kSpecial[64] = {
            Op::SLL, U, Op::SRL, Op::SRA, Op::SLLV, U, Op::SRLV, Op::SRAV,                         // 0
            Op::JR, Op::JALR, Op::MOVZ, Op::MOVN, Op::SYSCALL, Op::BREAK, U, Op::SYNC,             // 8
            Op::MFHI, Op::MTHI, Op::MFLO, Op::MTLO, Op::DSLLV, U, Op::DSRLV, Op::DSRAV,            // 16
            Op::MULT, Op::MULTU, Op::DIV, Op::DIVU, U, U, U, U,                                    // 24
            Op::ADD, Op::ADDU, Op::SUB, Op::SUBU, Op::AND, Op::OR, Op::XOR, Op::NOR,               // 32
            Op::MFSA, Op::MTSA, Op::SLT, Op::SLTU, Op::DADD, Op::DADDU, Op::DSUB, Op::DSUBU,       // 40
            Op::TGE, Op::TGEU, Op::TLT, Op::TLTU, Op::TEQ, U, Op::TNE, U,                          // 48
            Op::DSLL, U, Op::DSRL, Op::DSRA, Op::DSLL32, U, Op::DSRL32, Op::DSRA32,                // 56
        };

        // REGIMM (opcode 1), by rt field.
        constexpr Op kRegimm[32] = {
            Op::BLTZ, Op::BGEZ, Op::BLTZL, Op::BGEZL, U, U, U, U,                                  // 0
            Op::TGEI, Op::TGEIU, Op::TLTI, Op::TLTIU, Op::TEQI, U, Op::TNEI, U,                    // 8
            Op::BLTZAL, Op::BGEZAL, Op::BLTZALL, Op::BGEZALL, U, U, U, U,                          // 16
            Op::MTSAB, Op::MTSAH, U, U, U, U, U, U,                                                // 24
        };

        // MMI (opcode 28), by function field; 8, 9, 40, 41 are the MMI0-3 groups, 48/49 PMFHL/PMTHL.
        constexpr Op kMmi[64] = {
            Op::MADD, Op::MADDU, U, U, Op::PLZCW, U, U, U,                                         // 0
            U, U, U, U, U, U, U, U,                                                                // 8 (MMI0, MMI2)
            Op::MFHI1, Op::MTHI1, Op::MFLO1, Op::MTLO1, U, U, U, U,                                // 16
            Op::MULT1, Op::MULTU1, Op::DIV1, Op::DIVU1, U, U, U, U,                                // 24
            Op::MADD1, Op::MADDU1, U, U, U, U, U, U,                                               // 32
            U, U, U, U, U, U, U, U,                                                                // 40 (MMI1, MMI3)
            U, U, U, U, Op::PSLLH, U, Op::PSRLH, Op::PSRAH,                                        // 48 (PMFHL, PMTHL)
            U, U, U, U, Op::PSLLW, U, Op::PSRLW, Op::PSRAW,                                        // 56
        };

        // MMI0..3, by the sa field (bits 10..6).
        constexpr Op kMmi0[32] = {
            Op::PADDW, Op::PSUBW, Op::PCGTW, Op::PMAXW, Op::PADDH, Op::PSUBH, Op::PCGTH, Op::PMAXH,
            Op::PADDB, Op::PSUBB, Op::PCGTB, U, U, U, U, U,
            Op::PADDSW, Op::PSUBSW, Op::PEXTLW, Op::PPACW, Op::PADDSH, Op::PSUBSH, Op::PEXTLH, Op::PPACH,
            Op::PADDSB, Op::PSUBSB, Op::PEXTLB, Op::PPACB, U, U, Op::PEXT5, Op::PPAC5,
        };
        constexpr Op kMmi1[32] = {
            U, Op::PABSW, Op::PCEQW, Op::PMINW, Op::PADSBH, Op::PABSH, Op::PCEQH, Op::PMINH,
            U, U, Op::PCEQB, U, U, U, U, U,
            Op::PADDUW, Op::PSUBUW, Op::PEXTUW, U, Op::PADDUH, Op::PSUBUH, Op::PEXTUH, U,
            Op::PADDUB, Op::PSUBUB, Op::PEXTUB, Op::QFSRV, U, U, U, U,
        };
        constexpr Op kMmi2[32] = {
            Op::PMADDW, U, Op::PSLLVW, Op::PSRLVW, Op::PMSUBW, U, U, U,
            Op::PMFHI, Op::PMFLO, Op::PINTH, U, Op::PMULTW, Op::PDIVW, Op::PCPYLD, U,
            Op::PMADDH, Op::PHMADH, Op::PAND, Op::PXOR, Op::PMSUBH, Op::PHMSBH, U, U,
            U, U, Op::PEXEH, Op::PREVH, Op::PMULTH, Op::PDIVBW, Op::PEXEW, Op::PROT3W,
        };
        constexpr Op kMmi3[32] = {
            Op::PMADDUW, U, U, Op::PSRAVW, U, U, U, U,
            Op::PMTHI, Op::PMTLO, Op::PINTEH, U, Op::PMULTUW, Op::PDIVUW, Op::PCPYUD, U,
            U, U, Op::POR, Op::PNOR, U, U, U, U,
            U, U, Op::PEXCH, Op::PCPYH, U, U, Op::PEXCW, U,
        };

        // COP1 with fmt = S, by function field.
        constexpr Op kCop1S[64] = {
            Op::ADD_S, Op::SUB_S, Op::MUL_S, Op::DIV_S, Op::SQRT_S, Op::ABS_S, Op::MOV_S, Op::NEG_S, // 0
            U, U, U, U, U, U, U, U,                                                                  // 8
            U, U, U, U, U, U, Op::RSQRT_S, U,                                                        // 16
            Op::ADDA_S, Op::SUBA_S, Op::MULA_S, U, Op::MADD_S, Op::MSUB_S, Op::MADDA_S, Op::MSUBA_S, // 24
            U, U, U, U, Op::CVT_W_S, U, U, U,                                                        // 32
            Op::MAX_S, Op::MIN_S, U, U, U, U, U, U,                                                  // 40
            Op::C_F_S, U, Op::C_EQ_S, U, Op::C_LT_S, U, Op::C_LE_S, U,                               // 48
            U, U, U, U, U, U, U, U,                                                                  // 56
        };

        // COP2 macro instructions, "special1" table: by function field 0..59.
        constexpr Op kVuSpecial1[64] = {
            Op::VADDbc, Op::VADDbc, Op::VADDbc, Op::VADDbc, Op::VSUBbc, Op::VSUBbc, Op::VSUBbc, Op::VSUBbc,
            Op::VMADDbc, Op::VMADDbc, Op::VMADDbc, Op::VMADDbc, Op::VMSUBbc, Op::VMSUBbc, Op::VMSUBbc, Op::VMSUBbc,
            Op::VMAXbc, Op::VMAXbc, Op::VMAXbc, Op::VMAXbc, Op::VMINIbc, Op::VMINIbc, Op::VMINIbc, Op::VMINIbc,
            Op::VMULbc, Op::VMULbc, Op::VMULbc, Op::VMULbc, Op::VMULq, Op::VMAXi, Op::VMULi, Op::VMINIi,
            Op::VADDq, Op::VMADDq, Op::VADDi, Op::VMADDi, Op::VSUBq, Op::VMSUBq, Op::VSUBi, Op::VMSUBi,
            Op::VADD, Op::VMADD, Op::VMUL, Op::VMAX, Op::VSUB, Op::VMSUB, Op::VOPMSUB, Op::VMINI,
            Op::VIADD, Op::VISUB, Op::VIADDI, U, Op::VIAND, Op::VIOR, U, U,
            Op::VCALLMS, Op::VCALLMSR, U, U, U, U, U, U, // 60..63 select the special2 table
        };

        // COP2 macro instructions, "special2" table: index = (bits 10..6) * 4 + (bits 1..0), function field 60..63.
        constexpr Op kVuSpecial2[128] = {
            Op::VADDAbc, Op::VADDAbc, Op::VADDAbc, Op::VADDAbc, Op::VSUBAbc, Op::VSUBAbc, Op::VSUBAbc, Op::VSUBAbc,
            Op::VMADDAbc, Op::VMADDAbc, Op::VMADDAbc, Op::VMADDAbc, Op::VMSUBAbc, Op::VMSUBAbc, Op::VMSUBAbc, Op::VMSUBAbc,
            Op::VITOF0, Op::VITOF4, Op::VITOF12, Op::VITOF15, Op::VFTOI0, Op::VFTOI4, Op::VFTOI12, Op::VFTOI15,
            Op::VMULAbc, Op::VMULAbc, Op::VMULAbc, Op::VMULAbc, Op::VMULAq, Op::VABS, Op::VMULAi, Op::VCLIPw,
            Op::VADDAq, Op::VMADDAq, Op::VADDAi, Op::VMADDAi, Op::VSUBAq, Op::VMSUBAq, Op::VSUBAi, Op::VMSUBAi,
            Op::VADDA, Op::VMADDA, Op::VMULA, U, Op::VSUBA, Op::VMSUBA, Op::VOPMULA, Op::VNOP,
            Op::VMOVE, Op::VMR32, U, U, Op::VLQI, Op::VSQI, Op::VLQD, Op::VSQD,
            Op::VDIV, Op::VSQRT, Op::VRSQRT, Op::VWAITQ, Op::VMTIR, Op::VMFIR, Op::VILWR, Op::VISWR,
            Op::VRNEXT, Op::VRGET, Op::VRINIT, Op::VRXOR, U, U, U, U,
            U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U,
            U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U,
            U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U,
            U, U, U, U,
        };

        Op decodeCop0(uint32_t w)
        {
            const uint32_t rs = (w >> 21) & 31u;
            switch (rs)
            {
            case 0: return Op::MFC0;
            case 4: return Op::MTC0;
            case 8:
                switch ((w >> 16) & 31u)
                {
                case 0: return Op::BC0F;
                case 1: return Op::BC0T;
                case 2: return Op::BC0FL;
                case 3: return Op::BC0TL;
                default: return U;
                }
            case 16:
                switch (w & 63u)
                {
                case 1: return Op::TLBR;
                case 2: return Op::TLBWI;
                case 6: return Op::TLBWR;
                case 8: return Op::TLBP;
                case 24: return Op::ERET;
                case 56: return Op::EI;
                case 57: return Op::DI;
                default: return U;
                }
            default: return U;
            }
        }

        Op decodeCop1(uint32_t w)
        {
            const uint32_t rs = (w >> 21) & 31u;
            switch (rs)
            {
            case 0: return Op::MFC1;
            case 2: return Op::CFC1;
            case 4: return Op::MTC1;
            case 6: return Op::CTC1;
            case 8:
                switch ((w >> 16) & 31u)
                {
                case 0: return Op::BC1F;
                case 1: return Op::BC1T;
                case 2: return Op::BC1FL;
                case 3: return Op::BC1TL;
                default: return U;
                }
            case 16: return kCop1S[w & 63u];
            case 20: return (w & 63u) == 32u ? Op::CVT_S_W : U;
            default: return U;
            }
        }

        Op decodeCop2(uint32_t w)
        {
            const uint32_t rs = (w >> 21) & 31u;
            if (rs & 16u)
            {
                const uint32_t f = w & 63u;
                if (f < 60u)
                    return kVuSpecial1[f];
                return kVuSpecial2[(((w >> 6) & 31u) << 2) | (w & 3u)];
            }
            switch (rs)
            {
            case 1: return Op::QMFC2;
            case 2: return Op::CFC2;
            case 5: return Op::QMTC2;
            case 6: return Op::CTC2;
            case 8:
                switch ((w >> 16) & 31u)
                {
                case 0: return Op::BC2F;
                case 1: return Op::BC2T;
                case 2: return Op::BC2FL;
                case 3: return Op::BC2TL;
                default: return U;
                }
            default: return U;
            }
        }

        Op decodeMmi(uint32_t w)
        {
            const uint32_t f = w & 63u;
            const uint32_t sub = (w >> 6) & 31u;
            switch (f)
            {
            case 8: return kMmi0[sub];
            case 40: return kMmi1[sub];
            case 9: return kMmi2[sub];
            case 41: return kMmi3[sub];
            case 48:
                switch (sub)
                {
                case 0: return Op::PMFHL_LW;
                case 1: return Op::PMFHL_UW;
                case 2: return Op::PMFHL_SLW;
                case 3: return Op::PMFHL_LH;
                case 4: return Op::PMFHL_SH;
                default: return U;
                }
            case 49: return sub == 0 ? Op::PMTHL_LW : U;
            default: return kMmi[f];
            }
        }
    }

    Op decode(uint32_t w)
    {
        switch (w >> 26)
        {
        case 0: return kSpecial[w & 63u];
        case 1: return kRegimm[(w >> 16) & 31u];
        case 2: return Op::J;
        case 3: return Op::JAL;
        case 4: return Op::BEQ;
        case 5: return Op::BNE;
        case 6: return Op::BLEZ;
        case 7: return Op::BGTZ;
        case 8: return Op::ADDI;
        case 9: return Op::ADDIU;
        case 10: return Op::SLTI;
        case 11: return Op::SLTIU;
        case 12: return Op::ANDI;
        case 13: return Op::ORI;
        case 14: return Op::XORI;
        case 15: return Op::LUI;
        case 16: return decodeCop0(w);
        case 17: return decodeCop1(w);
        case 18: return decodeCop2(w);
        case 20: return Op::BEQL;
        case 21: return Op::BNEL;
        case 22: return Op::BLEZL;
        case 23: return Op::BGTZL;
        case 24: return Op::DADDI;
        case 25: return Op::DADDIU;
        case 26: return Op::LDL;
        case 27: return Op::LDR;
        case 28: return decodeMmi(w);
        case 30: return Op::LQ;
        case 31: return Op::SQ;
        case 32: return Op::LB;
        case 33: return Op::LH;
        case 34: return Op::LWL;
        case 35: return Op::LW;
        case 36: return Op::LBU;
        case 37: return Op::LHU;
        case 38: return Op::LWR;
        case 39: return Op::LWU;
        case 40: return Op::SB;
        case 41: return Op::SH;
        case 42: return Op::SWL;
        case 43: return Op::SW;
        case 44: return Op::SDL;
        case 45: return Op::SDR;
        case 46: return Op::SWR;
        case 47: return Op::CACHE;
        case 49: return Op::LWC1;
        case 51: return Op::PREF;
        case 54: return Op::LQC2;
        case 55: return Op::LD;
        case 57: return Op::SWC1;
        case 62: return Op::SQC2;
        case 63: return Op::SD;
        default: return U; // 19 (COP3), 29, 48 (LL), 50, 52, 53, 56, 58, 59, 60, 61: not on the R5900
        }
    }

    const char *opName(Op op) { return kOps[static_cast<unsigned>(op)].name; }
    uint32_t opFlags(Op op) { return kOps[static_cast<unsigned>(op)].flags; }

    void disassemble(const Insn &i, char *out, unsigned size)
    {
        // Compact and generic: the mnemonic, then the fields that the format carries. Exact assembler syntax is not
        // the point; the comment lets a reader find the instruction in the generated code.
        const char *n = opName(i.op);
        const uint32_t f = opFlags(i.op);
        if (i.word == 0)
            std::snprintf(out, size, "nop");
        else if (i.op == Op::J || i.op == Op::JAL)
            std::snprintf(out, size, "%s 0x%x", n, i.jumpTarget());
        else if (f & BR)
            std::snprintf(out, size, "%s rs=%u rt=%u -> 0x%x", n, i.rs(), i.rt(), i.branchTarget());
        else
            std::snprintf(out, size, "%s rs=%u rt=%u rd=%u sa=%u imm=0x%x", n, i.rs(), i.rt(), i.rd(), i.sa(), i.uimm());
    }
}
