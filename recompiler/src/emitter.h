// The C++ emitter: one C++ function per guest function (abi.md section 1), a header declaring them all and the
// function table.
#pragma once

#include "analysis.h"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bozo
{
    // How the generated code reaches an external address.
    enum class ExternalKind
    {
        Stub,     // a library or kernel function implemented natively: any transfer (call, jump, branch, fall-through)
        Override, // a port function bound to the address: calls (jal, jalr, bal) only; a direct jump (j, a branch) or a
                  // fall-through runs the guest code there
    };
    // The EE cycles a checkpoint charges (calllog.md section 2: the reference's time model).
    constexpr uint32_t kBackwardCycles = 32; // a taken backward branch or jump to a label of the same function
    constexpr uint32_t kLeaveCycles = 8;     // a call or jump that leaves the function

    // Reads an externals file: one address per line, "stub 0x1234" or "override 0x1234"; '#' starts a comment.
    // "store 0x1234" names a store instruction the generated code sets ctx->pc before (a port's patch of its game's
    // register stores tells the stores apart by their instruction: bozo/port.h, setStorePatch).
    // "native 0x1234" replaces an instruction through bozo_native_instruction (also in delay slots). A BC0 test
    // queries native completion instead; when unhandled the original instruction/condition runs unchanged.
    bool readExternals(const std::string &path, std::map<uint32_t, ExternalKind> &out, std::string &error,
                       std::set<uint32_t> *storePcs = nullptr, std::set<uint32_t> *nativePcs = nullptr);

    struct EmitOptions
    {
        std::string prefix = "bozo";          // file names: <prefix>_functions.h, <prefix>_table.cpp, <prefix>_NNN.cpp
        std::string symbolPrefix;            // isolate function symbols of separately compiled overlay images
        size_t maxBytesPerFile = 1u << 20;    // start a new .cpp past this size (parallel builds, compiler memory)
        bool comments = true;                 // a comment with each instruction's address and disassembly
        // When non-zero: exactly this many <prefix>_NNN.cpp files, the functions spread evenly by size (a build system
        // can then list the outputs in advance). maxBytesPerFile is ignored.
        uint32_t files = 0;
        // Guest addresses the runtime answers instead of the guest code there (abi.md section 5, replay.md): every
        // transfer to one goes through bozo_call_indirect, where the runtime's override table answers it.
        std::map<uint32_t, ExternalKind> externals;
        // Store instructions that set ctx->pc to their own address first ("store" lines of the externals file).
        std::set<uint32_t> storePcs;
        // Instructions replaced by a port's native operation ("native 0xADDR"). The original remains for checkers.
        std::set<uint32_t> nativePcs;
        // Overrides also answer a direct jump, a branch or a fall-through to their address (bozo_tail_call), not only
        // calls: a port whose game tail-jumps into a function it replaces. Off: the reference build's rule (capture.md).
        bool overridesTakeJumps = false;
    };

    struct EmitStats
    {
        uint32_t files = 0;
        uint32_t functions = 0;
        uint32_t entries = 0;                 // table entries: functions + extra entries
        uint32_t instructions = 0;
        uint64_t bytes = 0;                   // generated source size
        uint32_t filesWritten = 0;            // files whose contents changed (the others are left untouched)
        uint32_t externalCalls = 0;           // transfers emitted to external addresses
        std::array<uint32_t, kOpCount> untranslated{}; // decoded but not translated (emitted as bozo_unimplemented)
    };

    class Emitter
    {
    public:
        Emitter(const Analysis &analysis, EmitOptions options) : a_(analysis), opt_(std::move(options)) {}

        // The definition of one function (and of its extra entries' wrappers).
        void emitFunction(const Function &f, std::string &out);
        // The header declaring every function, and the table.
        void emitHeader(std::string &out) const;
        void emitTable(std::string &out) const;
        // Everything into a directory. Returns false (with a message) on a write error.
        bool emitAll(const std::string &dir, std::string &error);

        const EmitStats &stats() const { return stats_; }

    private:
        std::string symbolName(const std::string &name, uint32_t address) const;
        std::string translate(const Insn &i);
        std::string translateBody(const Insn &i);
        // A transfer to `target` leaving the function: a C++ call of its entry, or the runtime for an external address
        // or an address that is not an entry. `call`: reached by jal/jalr/bal (else a jump, branch or fall-through).
        std::string callTo(uint32_t target, bool call);
        bool isExternal(uint32_t target, bool call) const;
        std::string entryName(const Function &f, uint32_t addr) const;

        const Analysis &a_;
        EmitOptions opt_;
        EmitStats stats_;
    };
}
