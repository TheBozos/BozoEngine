// A streaming reader for PS2XCLG1 call logs (docs/specs/calllog.md): chunk by chunk (zlib payloads inflated), one
// record at a time, so a multi-gigabyte log is never held whole. A record stays valid until the reader has moved two
// chunks past it (the records made inside a call are always in the chunk of their CALL record).
#pragma once

#include "bozo/context_words.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bozo::calllog
{
    constexpr uint32_t kHeaderBytes = 128;
    constexpr uint32_t kRecordHeaderBytes = 40;
    constexpr uint8_t kInside = 0x80; // made inside the out-call whose CALL record comes before it

    enum Type : uint8_t
    {
        kCall = 1,
        kMem = 2,
        kCtx = 3,
        kInvoke = 4,
        kInvokeEnd = 5,
        kSwitch = 6,
        kFrame = 7,
        kOp = 8,
        kInit = 9,
    };
    const char *typeName(uint32_t type);

    enum CallFlags : uint8_t
    {
        kUnwound = 1,
        kFrameInside = 2,
        kCut = 4,
        kContextLost = 8,
        kTransfer = 16,
    };

    enum OutcallKind : uint8_t
    {
        kStub = 1,
        kOverride = 2,
        kSyscall = 3,
        kVu0 = 4,
        kIoLoad = 5,
        kIoStore = 6,
        kException = 7,
    };
    const char *kindName(uint32_t kind);

    struct Header
    {
        uint32_t version = 0, headerBytes = 0, contextBytes = 0, flatBytes = 0, ramBytes = 0, scratchpadBytes = 0,
                 vu0DataBytes = 0, pageBytes = 0, frameLimit = 0, flags = 0, eeClock = 0, recordHeaderBytes = 0;
        uint64_t elfHash = 0;
        std::string run;
    };

    struct Run
    {
        uint32_t address = 0; // flat address
        uint32_t length = 0;
        const uint8_t *data = nullptr;
    };

    // A traced call (calllog.md 4.10): an entry of the reference record's `calls` list.
    struct Trace
    {
        uint32_t id = 0, address = 0, a[4] = {}, v0 = 0, flags = 0;
    };

    struct Record
    {
        uint64_t index = 0;
        uint8_t type = 0, flags = 0;
        uint16_t sub = 0;
        uint32_t bytes = 0;
        uint64_t cycle = 0, seq = 0;
        uint32_t frame = 0;
        int32_t thread = 0;
        uint32_t checkpoints = 0, pc = 0;
        const uint8_t *p = nullptr; // the whole record (header included)

        uint32_t u32(uint32_t offset) const;
        uint64_t u64(uint32_t offset) const;
        bool inside() const { return type != kCall && (flags & kInside); }
    };

    struct Call
    {
        uint32_t ra = 0, sp = 0, t[4] = {}, f12 = 0, f13 = 0;
        uint64_t cycleAfter = 0;
        uint32_t timerBefore = 0, timerAfter = 0, depth = 0;
        uint8_t kind = 0, size = 0;
        uint16_t oflags = 0;
        uint32_t target = 0, value[4] = {};
        std::vector<std::pair<uint32_t, uint32_t>> words; // (context word, value), in word order
        std::vector<Run> runs;
        std::vector<Trace> traces;
    };

    struct Mem
    {
        uint64_t originCall = 0;
        std::vector<Run> runs;
    };

    struct Ctx
    {
        int32_t thread = 0;
        uint32_t depth = 0;
        std::vector<std::pair<uint32_t, uint32_t>> words;
    };

    struct Invoke
    {
        int32_t thread = 0;
        uint32_t depth = 0, pc = 0, flags = 0;
        uint64_t tag = 0, sequence = 0;
        uint32_t a[4] = {}, gp = 0, sp = 0, ra = 0;
    };

    struct InvokeEnd
    {
        int32_t thread = 0;
        uint32_t depth = 0, v0 = 0, v1 = 0;
        uint64_t sequence = 0;
    };

    struct Switch
    {
        int32_t from = 0;
        uint32_t fromDepth = 0;
        int32_t to = 0;
        uint32_t toDepth = 0, waitReason = 0, waitObject = 0;
        uint64_t token = 0;
    };

    struct Frame
    {
        uint32_t ended = 0;
        std::vector<Run> runs;
        std::vector<Trace> traces;
    };

    struct Op
    {
        uint32_t origin = 0, depth = 0;
        uint64_t originCall = 0, wide = 0;
        std::vector<uint32_t> args;
    };

    // Parsers: false with `error` set on a malformed record.
    bool parse(const Record &r, Call &out, std::string &error);
    bool parse(const Record &r, Mem &out, std::string &error);  // MEM and INIT
    bool parse(const Record &r, Ctx &out, std::string &error);
    bool parse(const Record &r, Invoke &out, std::string &error);
    bool parse(const Record &r, InvokeEnd &out, std::string &error);
    bool parse(const Record &r, Switch &out, std::string &error);
    bool parse(const Record &r, Frame &out, std::string &error);
    bool parse(const Record &r, Op &out, std::string &error);

    struct NameInfo
    {
        std::string name;
        uint32_t cls = 0; // 1 kernel, 2 library, 3 override
    };

    class Reader
    {
    public:
        ~Reader();
        bool open(const std::string &path);
        const Header &header() const { return header_; }
        const std::string &error() const { return error_; }
        // The next record (nullptr at the end of the log or on an error: error() set).
        const Record *peek();
        void advance();
        // NAME and OVRD (read from the start of the file as soon as the log is opened).
        const std::map<uint32_t, NameInfo> &names() const { return names_; }
        const std::set<uint32_t> &overrides() const { return overrides_; }
        bool ended() const { return endSeen_; }
        uint64_t endRecords() const { return endRecords_; }
        uint64_t endCalls() const { return endCalls_; }

    private:
        bool loadChunk(); // the next RECS chunk into cur_ (the previous one kept in prev_)
        bool readChunk(std::string &tag, std::vector<uint8_t> &payload);
        void readMeta(const std::string &tag, const std::vector<uint8_t> &payload);

        std::FILE *f_ = nullptr;
        Header header_;
        std::string error_;
        std::vector<uint8_t> cur_, prev_, raw_;
        size_t pos_ = 0;
        bool haveRecord_ = false;
        Record rec_;
        uint64_t index_ = 0;
        bool eof_ = false, endSeen_ = false;
        uint64_t endRecords_ = 0, endCalls_ = 0;
        std::map<uint32_t, NameInfo> names_;
        std::set<uint32_t> overrides_;
    };

    // NAME and OVRD of a log without reading its records.
    bool readNames(const std::string &path, std::map<uint32_t, NameInfo> &names, std::set<uint32_t> &overrides,
                   std::string &error);
}
