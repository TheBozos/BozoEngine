// bozo-libs-tests: the native libraries' parts no call log checks yet (docs/specs/libraries.md): the memory card's
// save path (the `save` run has no call log), and the heap and rand rules on their own.
#include "bozo/libs/libraries.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace
{
    int g_failures = 0;
    void eq(const char *what, long long got, long long want)
    {
        if (got == want)
            return;
        ++g_failures;
        std::printf("FAIL %s: %lld, expected %lld\n", what, got, want);
    }

    struct Machine
    {
        std::unique_ptr<Runtime> rt = std::make_unique<Runtime>();
        R5900Context ctx;
        bozo::libs::Libraries libs;

        // Calls a native function with up to eight arguments; returns v0.
        int32_t call(const char *name, std::initializer_list<uint32_t> args)
        {
            int i = 0;
            for (uint32_t a : args)
            {
                if (i < 4)
                    ctx.r[4 + i].sd[0] = static_cast<int32_t>(a);
                else
                    ctx.r[8 + i - 4].sd[0] = static_cast<int32_t>(a);
                ++i;
            }
            const bozo::libs::Function *f = libs.find(name);
            if (!f)
            {
                ++g_failures;
                std::printf("FAIL no native %s\n", name);
                return 0;
            }
            bozo::libs::Guest g(*rt, &ctx);
            f->handler(libs, g);
            return ctx.r[2].sw[0];
        }
        // sceMcSync's result.
        int32_t result()
        {
            call("sceMcSync", {0, 0x100000, 0x100004});
            int32_t r;
            std::memcpy(&r, rt->memory.ram() + 0x100004, 4);
            return r;
        }
        void string(uint32_t at, const char *s) { std::memcpy(rt->memory.ram() + at, s, std::strlen(s) + 1); }
    };

    void memoryCard()
    {
        Machine m;
        m.call("sceMcInit", {});
        m.call("sceMcGetInfo", {0, 0, 0x100010, 0x100014, 0x100018});
        eq("first GetInfo: a new card", m.result(), -1);
        m.call("sceMcGetInfo", {0, 0, 0x100010, 0x100014, 0x100018});
        eq("second GetInfo", m.result(), 0);
        uint32_t freeClusters;
        std::memcpy(&freeClusters, m.rt->memory.ram() + 0x100014, 4);
        eq("free clusters of an empty card", freeClusters, 0x2000);

        // The save run's sequence (tmb-calls.md section 5): the folder, its files, the save, globalFile.
        m.string(0x101000, "BASCUS-97101/globalFile");
        m.call("sceMcGetDir", {0, 0, 0x101000, 0, 1, 0x102000});
        eq("globalFile on a fresh card", m.result(), -4);
        m.string(0x101000, "/BASCUS-97101");
        m.call("sceMcMkdir", {0, 0, 0x101000});
        eq("Mkdir", m.result(), 0);
        m.call("sceMcMkdir", {0, 0, 0x101000});
        eq("Mkdir again", m.result(), -8);
        m.string(0x101000, "/BASCUS-97101/icon.sys");
        m.call("sceMcOpen", {0, 0, 0x101000, 0x203});
        const int32_t fd = m.result();
        eq("Open with CREAT", fd > 0, 1);
        m.string(0x103000, "PS2D-icon");
        m.call("sceMcWrite", {static_cast<uint32_t>(fd), 0x103000, 964});
        eq("Write", m.result(), 964);
        m.call("sceMcClose", {static_cast<uint32_t>(fd)});
        eq("Close", m.result(), 0);
        m.call("sceMcOpen", {0, 0, 0x101000, 0x1});
        const int32_t rd = m.result();
        m.call("sceMcRead", {static_cast<uint32_t>(rd), 0x104000, 2000});
        eq("Read what was written", m.result(), 964);
        eq("Read's bytes", std::memcmp(m.rt->memory.ram() + 0x104000, "PS2D-icon", 10), 0);
        m.call("sceMcClose", {static_cast<uint32_t>(rd)});
        m.string(0x101000, "BASCUS-97101/*");
        m.call("sceMcGetDir", {0, 0, 0x101000, 0, 50, 0x102000});
        eq("the folder's listing: ., .., icon.sys", m.result(), 3);
        eq("third entry's name", std::strcmp(reinterpret_cast<const char *>(m.rt->memory.ram() + 0x102000 + 128 + 32), "icon.sys"), 0);
        m.string(0x101000, "*");
        m.call("sceMcGetDir", {0, 0, 0x101000, 0, 50, 0x102000});
        eq("the root: ., .., BASCUS-97101", m.result(), 3);
        m.string(0x101000, "BASCUS-97101/missing");
        m.call("sceMcOpen", {0, 0, 0x101000, 0x1});
        eq("Open of a missing file", m.result(), -4);
    }

    void heapAndRand()
    {
        Machine m;
        m.libs.heap.reset(0x751b20, 0x2000000);
        eq("malloc 0x3c0", m.call("malloc", {0x3c0}), 0x751b20);
        eq("malloc 0x408", m.call("malloc", {0x408}), 0x751ee0);
        m.call("free", {0x751ee0});
        eq("malloc 0x1008 reuses the freed block", m.call("malloc", {0x1008}), 0x751ee0);
        eq("malloc 0x18 rounds to 16", m.call("malloc", {0x18}), 0x752ef0);
        eq("next", m.call("malloc", {0x18}), 0x752f10);
        m.call("srand", {0});
        eq("rand 1", m.call("rand", {}), 0);
        eq("rand 2", m.call("rand", {}), 0x5851f42d);
        eq("rand 3", m.call("rand", {}), 0x40b18ccf);
    }
}

// The CD stream's free-mode drive (libraries.md section 8): a sector every kSectorCycles from sceCdStStart, paused
// while the ring is full.
static void cdStream()
{
    using namespace bozo::libs;
    CdState cd;
    cd.ringSectors = 0x50;
    cd.producedAt = 1000;
    cd.advance(1000 + 10 * kSectorCycles + 5);
    eq("sectors arrived after 10 periods", static_cast<long long>(cd.produced), 10);
    eq("the next one's cycle", static_cast<long long>(cd.readyAt(11)), 1000 + 11 * kSectorCycles);
    cd.advance(1000 + 500 * kSectorCycles);
    eq("the ring fills and the drive waits", static_cast<long long>(cd.produced), 0x50);
    cd.delivered = 0x20;
    eq("reading on from a full ring", static_cast<long long>(cd.readyAt(0x40)),
       1000 + 500 * kSectorCycles + 0x10 * kSectorCycles);
}

// The SIF layer and the 989SND driver's rules that need no disc (libraries.md sections 14 and 15): the IOP heap, a
// bind, module ids, the SIF registers, a command batch and a direct call with the driver's last result.
static void sifAndSnd()
{
    using namespace bozo::libs;
    IopHeap heap;
    eq("first IOP block", heap.allocate(0x42C), 0x7FE60);
    eq("16-byte aligned", heap.allocate(0xA24), 0x80290);
    const uint32_t c = heap.allocate(0x5FC);
    eq("third", c, 0x80CC0);
    heap.release(0x80290);
    eq("first fit reuses the hole", heap.allocate(0x100), 0x80290);
    eq("block lookup", heap.blockAt(c + 8), c);

    Machine m;
    m.libs.sndNative = true;
    const uint32_t client = 0x100000, send = 0x100100, recv = 0x100200;
    eq("bind, first attempt", m.call("sceSifBindRpc", {client, 0x123456, 0}), 0);
    uint32_t server;
    std::memcpy(&server, m.rt->memory.ram() + client + 0x24, 4);
    eq("no server yet", server, 0);
    m.call("sceSifBindRpc", {client, 0x123456, 0});
    std::memcpy(&server, m.rt->memory.ram() + client + 0x24, 4);
    eq("bound at the second", server, 0x1F10000);
    eq("module 1", m.call("sceSifLoadModule", {0x100400, 0, 0}), 1);
    eq("module 2", m.call("sceSifLoadModule", {0x100400, 0, 0}), 2);
    eq("set sreg (old value)", m.call("sceSifSetSreg", {8, 5}), 0);
    eq("get sreg", m.call("sceSifGetSreg", {8}), 5);

    // A batch: one SetMasterVolume(3, 0x334); then GetMasterVolume(3) directly, then a batch whose command has no
    // result: its slot holds the driver's last result.
    const uint32_t batch[9] = {1, 3, 0x334, 0, 0, 0, 0, 0, 0x09};
    std::memcpy(m.rt->memory.ram() + send, batch, sizeof(batch));
    m.call("sceSifCallRpc", {client, 0x4A, 1, send, 36, recv, 12, 0});
    const uint32_t group = 3;
    std::memcpy(m.rt->memory.ram() + send, &group, 4);
    m.call("sceSifCallRpc", {client, 0x0A, 1, send, 4, recv, 12, 0});
    uint32_t reply[3];
    std::memcpy(reply, m.rt->memory.ram() + recv, 12);
    eq("reply opens with -1", reply[0], 0xFFFFFFFFu);
    eq("GetMasterVolume", reply[1], 0x334);
    eq("reply closes with -1", reply[2], 0xFFFFFFFFu);
    const uint32_t stop[9] = {1, 0x80000002u, 0, 0, 0, 0, 0, 0, 0x15};
    std::memcpy(m.rt->memory.ram() + send, stop, sizeof(stop));
    m.call("sceSifCallRpc", {client, 0x4A, 1, send, 36, recv, 12, 0});
    std::memcpy(reply, m.rt->memory.ram() + recv, 12);
    eq("a command with no result leaves the last one", reply[1], 0x334);
    uint32_t fno;
    std::memcpy(&fno, m.rt->memory.ram() + 0x1F10024, 4);
    eq("server data: the function", fno, 0x4A);
}

int main()
{
    sifAndSnd();
    memoryCard();
    heapAndRand();
    cdStream();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
