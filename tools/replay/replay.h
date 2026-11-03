// The replay of one capture against BozoEngine's generated code (docs/specs/replay.md): replay-only services that
// answer every out-call from the sample's log, and the comparison of the exit state.
#pragma once

#include "capture.h"

#include "bozo/runtime.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bozo::replay
{
    // What went wrong in a sample (bits; 0 = matches bit for bit).
    enum Status : uint32_t
    {
        kContext = 1u << 0,      // exit context words differ
        kMemory = 1u << 1,       // exit memory differs
        kDiverged = 1u << 2,     // an out-call of another kind or target than the log's next one, or one too many
        kOutcallsLeft = 1u << 3, // the log has out-calls the code did not make
        kArguments = 1u << 4,    // an out-call's a0-a3 (or a store's value, an access's size) differ from the log's
        kRunaway = 1u << 5,      // the cycle budget ran out (the call did not return)
        kNoEntry = 1u << 6,      // the generated code has no entry at the sample's function
        kException = 1u << 7,    // break, a trap, an untranslated instruction, a jump to an unknown address
        kCrashed = 1u << 8,      // the worker process died in this sample (set by the driver)
    };
    std::string statusText(uint32_t status);

    struct MemoryRange
    {
        uint32_t flat = 0;   // flat address (capture.md section 3)
        uint32_t length = 0; // bytes from the first to the last differing byte
    };

    struct SampleResult
    {
        uint32_t status = 0;
        std::vector<std::string> outcallIssues; // out-call mismatches, in order
        std::vector<uint32_t> words;            // differing context words
        std::array<uint32_t, capture::kContextWords> got{}, want{};
        std::vector<MemoryRange> ranges;        // differing memory (merged; at most kMaxRanges)
        uint64_t differingBytes = 0;
        uint32_t outcallsUsed = 0;
        std::vector<std::string> notes;         // requests answered by a default (no log record exists for them)
        uint64_t cycles = 0;
    };

    // A guest address of flat memory, for messages.
    uint32_t guestAddress(uint32_t flat);

    // The capture's context words <-> the generated code's context and the runtime's COP0 Status.
    uint32_t getWord(const R5900Context &ctx, const Runtime &rt, uint32_t word);
    void setWord(R5900Context &ctx, Runtime &rt, uint32_t word, uint32_t value);

    class Replayer final : public RuntimeServices
    {
    public:
        // `externals`: the addresses bound to the replay's out-call handler (kind 1 or 2 of the captures).
        Replayer(const BozoFunctionEntry *table, size_t count, const std::map<uint32_t, uint8_t> &externals);

        Runtime &runtime() { return rt_; }
        // Chain memory (capture.md 7.2): the flat space at the next sample's entry. The working memory (the runtime's)
        // equals it between samples.
        bool chainEntry(const std::vector<uint8_t> &payload, std::string &error);
        // Runs one parsed sample (its entry runs already chained) and compares. `trace` prints each out-call.
        SampleResult run(const capture::Sample &s, bool trace);

        uint64_t cycleBudget = 4'000'000'000ull;

        // RuntimeServices: everything answered from the log.
        void unknownTarget(uint8_t *, R5900Context *, Runtime *, uint32_t target) override;
        void yield(uint8_t *, R5900Context *, Runtime *) override;
        void syscall(uint8_t *, R5900Context *, Runtime *, uint32_t code) override;
        void breakpoint(uint8_t *, R5900Context *, Runtime *, uint32_t code) override;
        void trap(uint8_t *, R5900Context *, Runtime *) override;
        void unimplemented(uint8_t *, R5900Context *, Runtime *, uint32_t word) override;
        void eret(uint8_t *, R5900Context *, Runtime *) override;
        uint32_t cop0Read(R5900Context *, Runtime *, uint32_t reg) override;
        void cop0Write(R5900Context *, Runtime *, uint32_t reg, uint32_t value) override;
        bool cop0Condition(R5900Context *, Runtime *) override;
        void vu0CallMicroprogram(uint8_t *, R5900Context *, Runtime *, uint32_t address) override;
        uint32_t vu0ControlRead(R5900Context *, Runtime *, uint32_t reg) override;
        void vu0ControlWrite(R5900Context *, Runtime *, uint32_t reg, uint32_t value) override;
        void ioRead(R5900Context *, Runtime *, uint32_t address, uint32_t size, uint64_t value[2]) override;
        void ioWrite(R5900Context *, Runtime *, uint32_t address, uint32_t size, const uint64_t value[2]) override;

        // The override bound to every external address.
        static void external(uint8_t *rdram, R5900Context *ctx, Runtime *rt);

    private:
        struct Abort
        {
        };
        // The next log record for an out-call of `kind` at `target`; throws Abort when it does not match.
        const capture::Outcall &expect(uint8_t kind, uint32_t target, const char *what);
        void apply(const capture::Outcall &o, R5900Context *ctx);
        void checkArgs(const capture::Outcall &o, const uint32_t got[4], int n, const char *what);
        void fail(uint32_t status, std::string text);

        Runtime rt_;
        std::vector<uint8_t> chain_;
        std::map<uint32_t, uint8_t> externals_;
        const capture::Sample *sample_ = nullptr;
        SampleResult *result_ = nullptr;
        size_t next_ = 0;
        bool trace_ = false;
        // The DMA status CPCOND0 is answered from (see cop0Condition).
        void trackDma(uint32_t address, uint32_t value);
        uint32_t dmaDone_ = 0, dmaPcr_ = 0;
        bool dmaPcrKnown_ = false;
    };
}
