// ELF32 parsing from the System V ABI's description of the format: the file header, the program headers (what is
// loaded where) and the section headers (to find .text and the symbol table).
#include "elf.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

namespace bozo
{
    namespace
    {
        uint16_t rd16(const std::vector<uint8_t> &b, size_t o) { return static_cast<uint16_t>(b[o] | (b[o + 1] << 8)); }
        uint32_t rd32(const std::vector<uint8_t> &b, size_t o)
        {
            return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
                   (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
        }
    }

    bool ElfFile::fail(const std::string &message)
    {
        error_ = message;
        return false;
    }

    bool ElfFile::load(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return fail("cannot open " + path);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return parse(std::move(bytes));
    }

    bool ElfFile::loadSymbolMap(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return fail("cannot open symbol map " + path);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return parseSymbolMap(text, path);
    }

    bool ElfFile::parseSymbolMap(const std::string &text, const std::string &origin)
    {
        std::vector<ElfSymbol> added;
        std::map<std::string, uint32_t> seen;
        std::istringstream lines(text);
        std::string line;
        for (int number = 1; std::getline(lines, line); ++number)
        {
            std::istringstream words(line.substr(0, line.find('#')));
            std::string name, address, extra;
            if (!(words >> name))
                continue;
            const std::string where = origin + ":" + std::to_string(number) + ": ";
            if (!(words >> address) || (words >> extra))
                return fail(where + "expected \"name 0xADDR\"");
            char *end = nullptr;
            const unsigned long long value = std::strtoull(address.c_str(), &end, 0);
            if (address[0] == '-' || *end != '\0' || value == 0 || value > 0xFFFFFFFFull)
                return fail(where + "bad address " + address);
            ElfSymbol symbol;
            symbol.name = name;
            symbol.value = static_cast<uint32_t>(value);
            symbol.type = 2;
            symbol.bind = 1;
            const bool code = (symbol.value & 3u) == 0 &&
                (std::any_of(sections_.begin(), sections_.end(), [&](const ElfSection &s) {
                    return s.executable() && symbol.value >= s.addr && symbol.value - s.addr + 4 <= s.size;
                }) ||
                 std::any_of(segments_.begin(), segments_.end(), [&](const ElfSegment &s) {
                     return (s.flags & 1u) && symbol.value >= s.vaddr && symbol.value - s.vaddr + 4 <= s.fileSize;
                 }));
            if (!code)
                return fail(where + name + " at " + address + " is not an instruction of the ELF's code");
            auto [known, fresh] = seen.emplace(name, symbol.value);
            if (!fresh && known->second != symbol.value)
                return fail(where + name + " has two addresses");
            if (fresh)
                added.push_back(std::move(symbol));
        }
        symbols_.insert(symbols_.end(), added.begin(), added.end());
        return true;
    }

    bool ElfFile::parse(std::vector<uint8_t> bytes)
    {
        bytes_ = std::move(bytes);
        const auto &b = bytes_;
        if (b.size() < 52 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0)
            return fail("not an ELF file");
        if (b[4] != 1)
            return fail("not a 32-bit ELF");
        if (b[5] != 1)
            return fail("not little-endian");
        machine_ = rd16(b, 18);
        entry_ = rd32(b, 24);
        const uint32_t phoff = rd32(b, 28);
        const uint32_t shoff = rd32(b, 32);
        const uint16_t phentsize = rd16(b, 42);
        const uint16_t phnum = rd16(b, 44);
        const uint16_t shentsize = rd16(b, 46);
        const uint16_t shnum = rd16(b, 48);
        const uint16_t shstrndx = rd16(b, 50);

        // Program headers: the PT_LOAD segments.
        for (uint32_t i = 0; i < phnum; ++i)
        {
            const size_t o = phoff + static_cast<size_t>(i) * phentsize;
            if (o + 32 > b.size())
                return fail("program header outside the file");
            if (rd32(b, o) != 1u) // PT_LOAD
                continue;
            ElfSegment s;
            s.fileOffset = rd32(b, o + 4);
            s.vaddr = rd32(b, o + 8);
            s.fileSize = rd32(b, o + 16);
            s.memSize = rd32(b, o + 20);
            s.flags = rd32(b, o + 24);
            if (static_cast<size_t>(s.fileOffset) + s.fileSize > b.size())
                return fail("segment outside the file");
            segments_.push_back(s);
        }
        if (segments_.empty())
            return fail("no loadable segment");

        // The image: one block covering every segment.
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (const auto &s : segments_)
        {
            lo = std::min(lo, s.vaddr);
            hi = std::max(hi, s.vaddr + std::max(s.memSize, s.fileSize));
        }
        imageBase_ = lo;
        image_.assign(hi - lo, 0);
        for (const auto &s : segments_)
            std::memcpy(image_.data() + (s.vaddr - lo), b.data() + s.fileOffset, s.fileSize);

        // Section headers (optional for loading, needed for symbols and to know which bytes are code).
        if (shoff != 0 && shnum != 0)
        {
            if (static_cast<size_t>(shoff) + static_cast<size_t>(shnum) * shentsize > b.size())
                return fail("section headers outside the file");
            std::vector<uint32_t> nameOffsets;
            std::vector<uint32_t> links;
            for (uint32_t i = 0; i < shnum; ++i)
            {
                const size_t o = shoff + static_cast<size_t>(i) * shentsize;
                ElfSection s;
                nameOffsets.push_back(rd32(b, o));
                s.type = rd32(b, o + 4);
                s.flags = rd32(b, o + 8);
                s.addr = rd32(b, o + 12);
                s.offset = rd32(b, o + 16);
                s.size = rd32(b, o + 20);
                links.push_back(rd32(b, o + 24));
                sections_.push_back(s);
            }
            auto cstr = [&](uint32_t off) -> std::string {
                if (off >= b.size())
                    return {};
                const char *p = reinterpret_cast<const char *>(b.data() + off);
                return std::string(p, strnlen(p, b.size() - off));
            };
            if (shstrndx < sections_.size())
                for (size_t i = 0; i < sections_.size(); ++i)
                    sections_[i].name = cstr(sections_[shstrndx].offset + nameOffsets[i]);

            // Symbol tables (SHT_SYMTAB = 2), names in the linked string table.
            for (size_t i = 0; i < sections_.size(); ++i)
            {
                const auto &s = sections_[i];
                if (s.type != 2u || links[i] >= sections_.size())
                    continue;
                const uint32_t strOff = sections_[links[i]].offset;
                for (uint32_t o = s.offset; o + 16 <= s.offset + s.size && o + 16 <= b.size(); o += 16)
                {
                    ElfSymbol sym;
                    sym.name = cstr(strOff + rd32(b, o));
                    sym.value = rd32(b, o + 4);
                    sym.size = rd32(b, o + 8);
                    sym.type = b[o + 12] & 15u;
                    sym.bind = b[o + 12] >> 4;
                    sym.shndx = rd16(b, o + 14);
                    symbols_.push_back(std::move(sym));
                }
            }
        }
        return true;
    }

    uint32_t ElfFile::read32(uint32_t addr) const
    {
        if (!contains(addr, 4))
            return 0;
        const size_t o = addr - imageBase_;
        return static_cast<uint32_t>(image_[o]) | (static_cast<uint32_t>(image_[o + 1]) << 8) |
               (static_cast<uint32_t>(image_[o + 2]) << 16) | (static_cast<uint32_t>(image_[o + 3]) << 24);
    }
}
