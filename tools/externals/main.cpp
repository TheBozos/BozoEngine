// bozo-externals: the --externals list bozo-recomp generates a port's code with (abi.md section 5, recompiler/README.md),
// from what BozoEngine implements and what the port overrides:
// - stub: every ELF function symbol that names a kernel call the dispatcher answers natively or a native library
//   function (bozo/libs/libraries.h), and the names of `--stubs` files (functions a port answers as library stubs);
// - override: every address of `--overrides` files (a port's registrations: TMB_Recomp's tools/tmb_overrides_list.py);
// - store: the store instructions of those files that a port's store patch needs to tell apart (bozo/port.h).
//
//   bozo-externals <elf> <out.txt> [--overrides file]... [--stubs file]... [--check <log.clg>...]
//
// An overrides file holds "override 0xADDR" and "store 0xADDR" lines (anything after '#' is a comment); a stubs file
// one symbol name per line. --check compares the stubs with a call log's NAME chunk (the reference's stubs: a pinned
// check needs the same set) and lists the differences.
#include "elf.h"

#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef BOZO_EXTERNALS_CALLLOG
#include "calllog.h"
#endif

namespace
{
    int usage()
    {
        std::fprintf(stderr, "usage: bozo-externals <elf> <out.txt> [--overrides file]... [--stubs file]... [--check log.clg]...\n");
        return 2;
    }

    std::string stripComment(std::string line)
    {
        if (const size_t h = line.find('#'); h != std::string::npos)
            line.resize(h);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.pop_back();
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t'))
            ++s;
        return line.substr(s);
    }
}

int main(int argc, char **argv)
{
    using namespace bozo;
    if (argc < 3)
        return usage();
    const std::string elfPath = argv[1], outPath = argv[2];
    std::vector<std::string> overrideFiles, stubFiles, checkLogs;
    for (int k = 3; k < argc; ++k)
    {
        const std::string a = argv[k];
        if (k + 1 >= argc)
            return usage();
        if (a == "--overrides")
            overrideFiles.push_back(argv[++k]);
        else if (a == "--stubs")
            stubFiles.push_back(argv[++k]);
        else if (a == "--check")
            checkLogs.push_back(argv[++k]);
        else
            return usage();
    }

    ElfFile elf;
    if (!elf.load(elfPath))
    {
        std::fprintf(stderr, "%s: %s\n", elfPath.c_str(), elf.error().c_str());
        return 2;
    }

    // Names a port answers as stubs (beyond the native libraries).
    std::set<std::string> extraStubs;
    for (const std::string &path : stubFiles)
    {
        std::ifstream in(path);
        if (!in)
        {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 2;
        }
        for (std::string line; std::getline(in, line);)
            if (const std::string n = stripComment(line); !n.empty())
                extraStubs.insert(n);
    }

    // The stubs: function symbols BozoEngine answers.
    libs::Libraries libraries;
    std::map<uint32_t, std::string> stubs;
    for (const ElfSymbol &s : elf.symbols())
    {
        if (s.type != 2 || s.value == 0 || s.name.empty())
            continue;
        const bool kernel = ee::kernelCallByName(s.name) != ee::KernelCall::None;
        if (kernel || libraries.find(s.name) || extraStubs.count(s.name))
            stubs.emplace(s.value, s.name);
    }

    // The port's overrides and patched stores.
    std::set<uint32_t> overrides, stores, nativePcs;
    for (const std::string &path : overrideFiles)
    {
        std::ifstream in(path);
        if (!in)
        {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 2;
        }
        int n = 0;
        for (std::string line; std::getline(in, line);)
        {
            ++n;
            const std::string t = stripComment(line);
            if (t.empty())
                continue;
            std::istringstream words(t);
            std::string kind, address;
            words >> kind >> address;
            const uint32_t a = static_cast<uint32_t>(std::stoul(address, nullptr, 16));
            if (kind == "override")
                overrides.insert(a);
            else if (kind == "store")
                stores.insert(a);
            else if (kind == "native")
                nativePcs.insert(a);
            else
            {
                std::fprintf(stderr, "%s:%d: expected \"override 0xADDR\" or \"store 0xADDR\"\n", path.c_str(), n);
                return 2;
            }
        }
    }

    std::ofstream out(outPath);
    if (!out)
    {
        std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
        return 2;
    }
    out << "# Generated by bozo-externals from " << elfPath << ". Do not edit.\n";
    for (const auto &[a, name] : stubs)
        out << "stub 0x" << std::hex << a << std::dec << " # " << name << "\n";
    for (uint32_t a : overrides)
        if (!stubs.count(a))
            out << "override 0x" << std::hex << a << std::dec << "\n";
    for (uint32_t a : stores)
        out << "store 0x" << std::hex << a << std::dec << "\n";
    for (uint32_t a : nativePcs)
        out << "native 0x" << std::hex << a << std::dec << "\n";
    std::printf("bozo-externals: %zu stubs, %zu overrides, %zu patched stores, %zu native instructions -> %s\n", stubs.size(), overrides.size(),
                stores.size(), nativePcs.size(), outPath.c_str());

#ifdef BOZO_EXTERNALS_CALLLOG
    for (const std::string &path : checkLogs)
    {
        std::map<uint32_t, calllog::NameInfo> names;
        std::set<uint32_t> logOverrides;
        std::string err;
        if (!calllog::readNames(path, names, logOverrides, err))
        {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            return 2;
        }
        size_t onlyLog = 0, onlyHere = 0;
        for (const auto &[a, info] : names)
            if (info.cls != 3 && !stubs.count(a))
            {
                std::printf("  stub of the reference, not here: 0x%x %s\n", a, info.name.c_str());
                ++onlyLog;
            }
        for (const auto &[a, name] : stubs)
            if (auto it = names.find(a); it == names.end() || it->second.cls == 3)
            {
                std::printf("  stub here, not the reference's: 0x%x %s\n", a, name.c_str());
                ++onlyHere;
            }
        size_t overridesMissing = 0;
        for (uint32_t a : logOverrides)
            if (!overrides.count(a) && !stubs.count(a))
            {
                std::printf("  override of the reference, not the port's: 0x%x\n", a);
                ++overridesMissing;
            }
        std::printf("%s: %zu stubs only the reference has, %zu only here; %zu of its overrides the port does not register\n",
                    path.c_str(), onlyLog, onlyHere, overridesMissing);
    }
#else
    if (!checkLogs.empty())
        std::fprintf(stderr, "--check needs the call-log reader (BOZO_BUILD_TOOLS)\n");
#endif
    return 0;
}
