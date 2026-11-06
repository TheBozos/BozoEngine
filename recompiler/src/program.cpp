#include "program.h"

#include "elf.h"
#include "decoder.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <format>
#include <set>
#include <sstream>

namespace bozo
{
    bool readInstructionPatches(const std::string &path, std::vector<InstructionPatch> &out, std::string &error)
    {
        std::ifstream in(path);
        if (!in) { error = "cannot read instruction patches: " + path; return false; }
        std::vector<InstructionPatch> parsed;
        std::string line;
        unsigned number = 0;
        auto word = [](std::string text, uint32_t &value)
        {
            if (text.starts_with("0x") || text.starts_with("0X")) text.erase(0, 2);
            if (text.empty()) return false;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        };
        while (std::getline(in, line))
        {
            ++number;
            line.resize(line.find('#') == std::string::npos ? line.size() : line.find('#'));
            std::istringstream fields(line);
            std::string a, b, c, extra;
            if (!(fields >> a)) continue;
            InstructionPatch patch{};
            if (!(fields >> b >> c) || (fields >> extra) || !word(a, patch.address) ||
                !word(b, patch.expected) || !word(c, patch.replacement))
            {
                error = std::format("{}:{}: expected three hexadecimal words: address original replacement", path, number);
                return false;
            }
            parsed.push_back(patch);
        }
        if (in.bad()) { error = "cannot finish reading instruction patches: " + path; return false; }
        out.insert(out.end(), parsed.begin(), parsed.end());
        return true;
    }

    bool applyInstructionPatches(Program &program, const std::vector<InstructionPatch> &patches, std::string &error)
    {
        // Validate the complete batch before changing any bytes, including duplicate entries.
        std::set<uint32_t> seen;
        for (const auto &p : patches)
        {
            const bool codeWord = std::any_of(program.code.begin(), program.code.end(), [&](const AddressRange &range) {
                return p.address >= range.start && static_cast<uint64_t>(p.address) + 4u <= range.end;
            });
            if ((p.address & 3u) || !program.contains(p.address, 4) || !codeWord || !seen.insert(p.address).second)
            {
                error = std::format("invalid or duplicate instruction patch address 0x{:08x}", p.address);
                return false;
            }
            if (program.read32(p.address) != p.expected)
            {
                error = std::format("instruction patch at 0x{:08x}: expected 0x{:08x}, found 0x{:08x}",
                                    p.address, p.expected, program.read32(p.address));
                return false;
            }
            if (decode(p.replacement) == Op::Unknown)
            {
                error = std::format("instruction patch at 0x{:08x}: unsupported replacement 0x{:08x}", p.address, p.replacement);
                return false;
            }
        }
        for (const auto &p : patches)
            for (unsigned byte = 0; byte < 4; ++byte)
                program.image[p.address - program.base + byte] = static_cast<uint8_t>(p.replacement >> (byte * 8));
        return true;
    }

    namespace
    {
        bool isVuSection(const std::string &name)
        {
            return name.rfind(".vu", 0) == 0 || name.rfind(".DVP", 0) == 0;
        }
    }

    Program programFromElf(const ElfFile &elf)
    {
        Program p;
        p.base = elf.imageBase();
        p.image = elf.image();
        p.entry = elf.entry();
        for (const auto &s : elf.sections())
        {
            if ((s.flags & 2u) == 0 || s.size == 0) // not allocated
                continue;
            if (s.executable())
            {
                if (!isVuSection(s.name))
                    p.code.push_back({s.addr, s.addr + s.size, s.name});
            }
            else if (s.type == 1u && !isVuSection(s.name)) // SHT_PROGBITS: initialised data
                p.data.push_back({s.addr, s.addr + s.size, s.name});
        }
        // No section headers: treat executable segments as code.
        if (p.code.empty())
            for (const auto &seg : elf.segments())
                if (seg.flags & 1u)
                    p.code.push_back({seg.vaddr, seg.vaddr + seg.fileSize, "segment"});
        for (const auto &sym : elf.symbols())
        {
            if (sym.name.empty() || sym.type == 3u || sym.type == 4u) // sections, files
                continue;
            p.symbols.push_back({sym.name, sym.value, sym.size, sym.type == 2u});
        }
        // A sized data object (STT_OBJECT) inside an executable section (a writable code segment holding a game's
        // tables) is data: it leaves the code ranges and joins the data ones.
        std::vector<AddressRange> objects;
        for (const auto &sym : elf.symbols())
            if (sym.type == 1u && sym.size != 0)
                objects.push_back({sym.value, sym.value + sym.size, sym.name});
        std::sort(objects.begin(), objects.end(), [](const AddressRange &a, const AddressRange &b) { return a.start < b.start; });
        std::vector<AddressRange> code;
        for (const auto &r : p.code)
        {
            uint32_t at = r.start;
            for (const auto &o : objects)
            {
                if (o.end <= at || o.start >= r.end)
                    continue;
                const uint32_t start = std::max(o.start, at), end = std::min(o.end, r.end);
                if (start > at)
                    code.push_back({at, start, r.name});
                p.data.push_back({start, end, r.name});
                at = std::max(at, end);
            }
            if (at < r.end)
                code.push_back({at, r.end, r.name});
        }
        p.code = std::move(code);
        return p;
    }
}
