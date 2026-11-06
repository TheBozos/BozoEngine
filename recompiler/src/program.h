// What the recompiler works on: a guest program's memory image, which parts of it are code, and its symbols. Built
// from an ELF file (fromElf) or directly (the tests build one from a few instruction words).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bozo
{
    class ElfFile;

    struct AddressRange
    {
        uint32_t start = 0;
        uint32_t end = 0; // exclusive
        std::string name;
        bool contains(uint32_t a) const { return a >= start && a < end; }
    };

    struct ProgramSymbol
    {
        std::string name;
        uint32_t address = 0;
        uint32_t size = 0;
        bool function = false; // STT_FUNC
    };

    struct Program
    {
        uint32_t base = 0;              // address of image[0]
        std::vector<uint8_t> image;     // the loaded memory
        uint32_t entry = 0;
        std::vector<AddressRange> code; // R5900 code to translate
        std::vector<AddressRange> data; // initialised data, scanned for code pointers
        std::vector<ProgramSymbol> symbols;

        bool contains(uint32_t addr, uint32_t size = 4) const
        {
            return addr >= base && static_cast<uint64_t>(addr - base) + size <= image.size();
        }
        uint32_t read32(uint32_t addr) const
        {
            if (!contains(addr, 4))
                return 0;
            const uint8_t *p = image.data() + (addr - base);
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
        bool isCode(uint32_t addr) const
        {
            for (const auto &r : code)
                if (r.contains(addr))
                    return true;
            return false;
        }
    };

    // Code: allocated, executable sections, except VU microcode (.vutext and the DVP overlays, which hold VU1/VU0
    // programs, not R5900 instructions). Data: allocated sections with file contents that are not code.
    Program programFromElf(const ElfFile &elf);

    // Compile-time port adaptations, applied to the in-memory analysis image only.
    // The owner selects the image and supplies its expected word so a mismatched site fails safely.
    struct InstructionPatch
    {
        uint32_t address, expected, replacement;
    };
    bool readInstructionPatches(const std::string &path, std::vector<InstructionPatch> &out, std::string &error);
    bool applyInstructionPatches(Program &program, const std::vector<InstructionPatch> &patches, std::string &error);
}
