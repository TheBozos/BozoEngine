// Function discovery and control-flow facts for the emitter.
#pragma once

#include "decoder.h"
#include "program.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bozo
{
    // A `jr` through a table of addresses in the function (a C switch).
    struct JumpTable
    {
        uint32_t jrAddress = 0;
        uint32_t tableAddress = 0;
        std::vector<uint32_t> targets; // in table order (may repeat)
        // The base built in place (lui r; addiu r, r, lo, or lui alone) and a range check (sltiu) before it: the
        // switch form the reference resolves as a switch inside the function. Any other jr through a register goes
        // through its dispatcher (kernel.md section 3).
        bool inPlace = false;
        bool slots = false; // no table in data: jr base + (index << k), the targets are code slots at that stride
    };

    struct Function
    {
        uint32_t start = 0;
        uint32_t end = 0;        // exclusive
        std::string symbol;      // the ELF symbol, empty if none
        bool fromSymbol = false; // its range came from an ELF function symbol (else it was carved out of a gap)
        // Other addresses inside the function where execution can enter from outside (a branch or jump from another
        // function, a call or a code pointer to the middle): the generated function can be entered there (abi.md 3).
        std::set<uint32_t> extraEntries;
        std::map<uint32_t, std::string> entrySymbols; // a symbol naming an extra entry, if any
        std::map<uint32_t, JumpTable> jumpTables;     // by the jr's address
        std::set<uint32_t> returnJr;                  // jr through a register holding a copy of the entry ra: a return
        std::set<uint32_t> unresolvedJr;              // jr through a register that is neither ra nor a known table
    };

    struct AnalysisStats
    {
        uint32_t symbolFunctions = 0;   // functions from ELF function symbols
        uint32_t gapFunctions = 0;      // functions found in code not covered by a symbol
        bool symbolDriven = false;      // sized function symbols cover most of the code (else discovery from the code)
        uint32_t extraEntries = 0;      // entries into the middle of a function
        uint32_t jumpTables = 0;
        uint32_t jumpTableTargets = 0;
        uint32_t returnJr = 0;
        uint32_t unresolvedJr = 0;
        uint32_t callTargets = 0;       // distinct jal/bal targets
        uint32_t dataCodePointers = 0;  // words in data that point at code (function starts or entries)
        uint32_t codePointerConstants = 0; // lui/addiu pairs building a code address
        uint32_t branchesInDelaySlots = 0;
        std::vector<std::string> warnings;
    };

    class Analysis
    {
    public:
        explicit Analysis(const Program &program) : program_(program) {}
        void run();

        const Program &program() const { return program_; }
        const std::vector<Function> &functions() const { return functions_; }
        const AnalysisStats &stats() const { return stats_; }

        // The function whose range holds `addr`, or nullptr.
        const Function *functionContaining(uint32_t addr) const;
        // The function starting exactly at `addr`, or nullptr.
        const Function *functionAt(uint32_t addr) const;
        // Is `addr` an address the generated code can be entered at (a function start or an extra entry)?
        bool isEntry(uint32_t addr) const;

        // The whole result as text, one fact per line, for tools and for people (docs/specs/discovery.md).
        std::string report() const;

    private:
        Function *mutableContaining(uint32_t addr);
        void addSymbolFunctions();
        void collectSeeds(std::set<uint32_t> &seeds);
        void collectBuiltPointers(const Insn &lui, uint32_t rangeEnd);
        void carveGaps(const std::set<uint32_t> &seeds);
        void findJumpTables(Function &f);
        // Discovery from the code alone (ELFs whose symbols cover little of it).
        bool isCopyOfEntryRa(const Function &f, uint32_t jrAddress, uint32_t reg) const;
        bool plausibleStart(uint32_t addr) const;
        bool followsTerminator(uint32_t addr) const;
        bool afterEnd(uint32_t addr, uint32_t floor, bool nothingAhead) const;
        bool resolveTable(uint32_t jrAddress, uint32_t scanStart, uint32_t low, uint32_t high, JumpTable &out) const;
        void sweepGap(uint32_t from, uint32_t to, const std::set<uint32_t> &hard, std::map<uint32_t, bool> &starts) const;
        void mergeSplitBodies(std::map<uint32_t, bool> &starts) const;
        void discoverFunctions();
        void addEntriesFromControlFlow();
        void addEntry(uint32_t addr, const char *why);

        const Program &program_;
        std::vector<Function> functions_; // sorted by start, non-overlapping
        std::map<uint32_t, std::string> anySymbolAt_;
        std::set<uint32_t> dataPointers_;
        std::set<uint32_t> codePointers_;
        std::set<uint32_t> callTargets_;
        AnalysisStats stats_;
    };

    // The C++ name of a function or entry: <symbol>_0x<address>, or func_0x<address> (abi.md section 1).
    std::string cppName(const std::string &symbol, uint32_t address);
}
