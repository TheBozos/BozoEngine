#include "program.h"

#include "elf.h"

namespace bozo
{
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
        return p;
    }
}
