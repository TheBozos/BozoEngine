// A minimal ELF32 little-endian loader: what the recompiler needs from a PS2 executable.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bozo
{
    struct ElfSegment // a PT_LOAD program header
    {
        uint32_t vaddr = 0;
        uint32_t fileOffset = 0;
        uint32_t fileSize = 0;
        uint32_t memSize = 0;
        uint32_t flags = 0; // PF_X = 1, PF_W = 2, PF_R = 4
    };

    struct ElfSection
    {
        std::string name;
        uint32_t type = 0;
        uint32_t flags = 0; // SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4
        uint32_t addr = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
        bool executable() const { return (flags & 4u) != 0 && (flags & 2u) != 0 && type == 1u; }
    };

    struct ElfSymbol
    {
        std::string name;
        uint32_t value = 0;
        uint32_t size = 0;
        uint8_t type = 0; // STT_NOTYPE 0, STT_OBJECT 1, STT_FUNC 2, STT_SECTION 3, STT_FILE 4
        uint8_t bind = 0; // STB_LOCAL 0, STB_GLOBAL 1, STB_WEAK 2
        uint16_t shndx = 0;
    };

    class ElfFile
    {
    public:
        // Reads and parses the file. On failure returns false and sets error().
        bool load(const std::string &path);
        bool parse(std::vector<uint8_t> bytes);
        // Adds a symbol map's functions to symbols(), after the ELF's own (docs/specs/symbol-map.md). All or nothing: on
        // failure returns false, sets error() and keeps symbols() as it was.
        bool loadSymbolMap(const std::string &path);
        bool parseSymbolMap(const std::string &text, const std::string &origin);
        // Forgets the ELF's own symbols: the program as if stripped, to measure discovery against them.
        void clearSymbols() { symbols_.clear(); }

        const std::string &error() const { return error_; }
        uint32_t entry() const { return entry_; }
        uint16_t machine() const { return machine_; }
        const std::vector<ElfSegment> &segments() const { return segments_; }
        const std::vector<ElfSection> &sections() const { return sections_; }
        const std::vector<ElfSymbol> &symbols() const { return symbols_; }
        const std::vector<uint8_t> &bytes() const { return bytes_; }

        // The loaded image: every PT_LOAD segment placed at its address (the part beyond the file size is zero),
        // as one block from imageBase() to imageBase() + image().size().
        uint32_t imageBase() const { return imageBase_; }
        const std::vector<uint8_t> &image() const { return image_; }
        bool contains(uint32_t addr, uint32_t size = 1) const
        {
            return addr >= imageBase_ && addr - imageBase_ + size <= image_.size();
        }
        uint32_t read32(uint32_t addr) const; // 0 outside the image

    private:
        bool fail(const std::string &message);

        std::vector<uint8_t> bytes_;
        std::vector<uint8_t> image_;
        uint32_t imageBase_ = 0;
        uint32_t entry_ = 0;
        uint16_t machine_ = 0;
        std::vector<ElfSegment> segments_;
        std::vector<ElfSection> sections_;
        std::vector<ElfSymbol> symbols_;
        std::string error_;
    };
}
