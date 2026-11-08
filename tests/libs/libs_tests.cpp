// bozo-libs-tests: the native libraries' parts no call log checks yet (docs/specs/libraries.md): the memory card's
// save path (the `save` run has no call log), and the heap and rand rules on their own.
#include "bozo/libs/libraries.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <filesystem>
#include <fstream>
#include <chrono>

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
        eq("Flush submission", m.call("sceMcFlush", {static_cast<uint32_t>(fd)}), 0);
        eq("Flush completion", m.result(), 0);
        uint32_t completed = 0;
        std::memcpy(&completed, m.rt->memory.ram() + 0x100000, 4);
        eq("Flush command identity", completed, 0xA);
        eq("Flush preserves the open cursor", m.libs.mc.files.at(fd).position, 964);
        eq("Flush result consumed once", m.call("sceMcSync", {0, 0, 0}), -1);
        m.call("sceMcFlush", {static_cast<uint32_t>(fd)});
        eq("Repeated flush is supported", m.result(), 0);
        m.call("sceMcClose", {static_cast<uint32_t>(fd)});
        eq("Close", m.result(), 0);
        m.call("sceMcFlush", {static_cast<uint32_t>(fd)});
        eq("Flush rejects a closed descriptor", m.result(), -4);
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

void reentrantAllocator() {
    Machine m;
    m.libs.heap.reset(0x100000,0x200000);
    eq("malloc uses shared heap",m.call("malloc",{64}),0x100000);
    std::memset(m.rt->memory.ram()+0x100040,0xaa,32);
    eq("reentrant calloc follows malloc",m.call("_calloc_r",{0x800,2,16}),0x100040);
    eq("calloc zeroes",m.rt->memory.ram()[0x100040],0);
    m.call("_free_r",{0x800,0x100000});
    eq("reentrant free shares malloc allocator",m.call("_malloc_r",{0x800,64}),0x100000);
    const size_t blocks=m.libs.heap.blocks();
    eq("calloc overflow",m.call("_calloc_r",{0x800,0xffffffff,2}),0);
    eq("malloc alignment overflow",m.call("malloc",{0xfffffff8}),0);
    eq("overflow keeps allocator unchanged",m.libs.heap.blocks(),blocks);
}
void cardSlotAliases() {
    Machine m;
    m.call("sceMcGetInfo",{0,1,0x100010,0x100014,0x100018});
    eq("default card slot policy",m.result(),-4);
    m.libs.mc.slotMask=3;
    m.call("sceMcGetInfo",{0,1,0x100010,0x100014,0x100018});
    eq("configured alias detects card",m.result(),-1);
    uint32_t command;std::memcpy(&command,m.rt->memory.ram()+0x100000,4);
    eq("sync reports completed command",command,1);
    eq("sync without a pending operation",m.call("sceMcSync",{0,0,0}),-1);
}
void nativeCardDirectories() {
    Machine m;
    m.call("sceMcInit",{});
    m.call("sceMcGetInfo",{0,0,0,0,0});m.result();
    m.libs.mc.card(0).create("SAVE",true,m.libs.mc.date);
    m.libs.mc.card(0).create("SAVE/one",false,m.libs.mc.date);
    m.libs.mc.card(0).create("SAVE/two",false,m.libs.mc.date);
    m.string(0x110000,"/SAVE");
    m.call("sceMcChdir",{0,0,0x110000,0x110100});
    eq("change directory completes",m.result(),0);
    eq("chdir returns old root",std::strcmp(reinterpret_cast<char *>(m.rt->memory.ram()+0x110100),"/"),0);
    m.string(0x110000,"one");
    m.call("sceMcOpen",{0,0,0x110000,1});
    const int32_t fd=m.result();
    eq("open relative to current directory",fd>0,1);
    m.call("sceMcClose",{uint32_t(fd)});m.result();
    m.string(0x110000,"*");
    m.call("sceMcGetDir",{0,0,0x110000,0,2,0x120000});
    eq("first listing page includes dot entries",m.result(),2);
    m.call("sceMcGetDir",{0,0,0x110000,1,2,0x120000});
    eq("continued listing returns remaining files",m.result(),2);
    eq("continued page starts with one",std::strcmp(reinterpret_cast<char *>(m.rt->memory.ram()+0x120000+32),"one"),0);
    m.call("sceMcGetDir",{0,0,0x110000,1,2,0x120000});
    eq("listing terminates",m.result(),0);
    m.string(0x110000,"..");
    m.call("sceMcChdir",{0,0,0x110000,0});eq("parent directory",m.result(),0);
    eq("parent restored root",m.libs.mc.currentDirectory[0].empty(),1);
}
void nativeCardFlushPersistence() {
    const auto root=std::filesystem::temp_directory_path()/
        ("bozo-mc-flush-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        Machine m;m.libs.mc.hostDirectory=root.string();
        m.call("sceMcGetInfo",{0,0,0,0,0});m.result();
        m.string(0x110000,"save");m.call("sceMcOpen",{0,0,0x110000,0x203});const auto fd=m.result();
        m.string(0x111000,"native-");m.call("sceMcWrite",{uint32_t(fd),0x111000,7});eq("host write",m.result(),7);
        m.call("sceMcFlush",{uint32_t(fd)});eq("host flush",m.result(),0);
        m.string(0x111000,"save");m.call("sceMcWrite",{uint32_t(fd),0x111000,4});eq("write after flush",m.result(),4);
        m.call("sceMcFlush",{uint32_t(fd)});eq("second host flush",m.result(),0);
        std::ifstream in(root/"mc0"/"save",std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),{});
        eq("flushed bytes available before close",bytes=="native-save",1);
        m.call("sceMcClose",{uint32_t(fd)});m.result();
    }
    {
        Machine m;m.libs.mc.hostDirectory=root.string();
        m.call("sceMcGetInfo",{0,0,0,0,0});m.result();
        m.string(0x110000,"save");m.call("sceMcOpen",{0,0,0x110000,1});const auto fd=m.result();
        m.call("sceMcRead",{uint32_t(fd),0x111000,64});eq("flushed save reload size",m.result(),11);
        eq("flushed save reload bytes",std::memcmp(m.rt->memory.ram()+0x111000,"native-save",11),0);
        m.call("sceMcClose",{uint32_t(fd)});m.result();
    }
    std::filesystem::remove(root/"mc0"/"save");
    std::filesystem::remove(root/"mc0");std::filesystem::remove(root/"mc1");std::filesystem::remove(root);
}
void liveScriptInput() {
    const auto path=std::filesystem::temp_directory_path()/
        ("bozo-live-pad-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto write=[&](const char *text){std::ofstream out(path);out<<text;};
    bozo::libs::LiveScriptInput input(path.string());
    bozo::libs::PadInputState state;
    write("0:CROSS/2");input.read(100,0,state);
    eq("live script starts relative to update",state.buttons,bozo::libs::kPadCross);
    input.read(102,0,state);eq("live script releases",state.buttons,0);
    write("0:START/4");input.read(110,0,state);
    eq("changed live script restarts",state.buttons,bozo::libs::kPadStart);
    write("0:");input.read(111,0,state);
    eq("partial write keeps last valid input",state.buttons,bozo::libs::kPadStart);
    input.read(114,0,state);eq("partial write cannot hold button forever",state.buttons,0);
    std::filesystem::remove(path);
}
void nativeIsoFiles() {
    std::vector<uint8_t> iso(24*2048);
    const auto word=[](uint8_t *at,uint32_t value){for(int i=0;i<4;++i){at[i]=uint8_t(value>>(8*i));at[7-i]=at[i];}};
    const auto record=[&](uint8_t *at,const std::string &name,uint32_t sector,uint32_t bytes,bool directory){
        const uint8_t size=uint8_t((33+name.size()+1)&~1u);
        at[0]=size;word(at+2,sector);word(at+10,bytes);at[25]=directory?2:0;
        at[28]=1;at[31]=1;at[32]=uint8_t(name.size());std::memcpy(at+33,name.data(),name.size());return size;
    };
    uint8_t *pvd=iso.data()+16*2048;pvd[0]=1;std::memcpy(pvd+1,"CD001",5);pvd[6]=1;
    record(pvd+156,std::string(1,'\0'),20,2048,true);
    record(iso.data()+20*2048,"ASSET",21,2048,true);
    record(iso.data()+21*2048,"DATA.BIN;1",22,3000,false);
    std::memset(iso.data()+22*2048,0x22,2048);std::memset(iso.data()+23*2048,0x33,2048);
    const auto path=std::filesystem::temp_directory_path()/("bozo-iso-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
    {std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char *>(iso.data()),iso.size());}
    {
        Machine m;m.libs.cd.imagePath=path.string();
        m.string(0x101000,"cdrom0:\\asset\\data.bin;1");
        eq("native ISO file search",m.call("sceCdSearchFile",{0x102000,0x101000}),1);
        uint32_t size;std::memcpy(&size,m.rt->memory.ram()+0x102004,4);eq("ISO byte size",size,3000);
        const auto fd=m.call("sceOpen",{0x101000,1});eq("native file descriptor",fd,3);
        eq("native file seek",m.call("sceLseek",{uint32_t(fd),2040,0}),2040);
        eq("unaligned sector-spanning file read",m.call("sceRead",{uint32_t(fd),0x103000,32}),32);
        eq("read first sector tail",m.rt->memory.ram()[0x103007],0x22);
        eq("read next sector head",m.rt->memory.ram()[0x103008],0x33);
        eq("sector read starts",m.call("sceCdRead",{22,2,0x104000,0}),1);
        eq("sector read is initially busy",m.call("sceCdSync",{1}),1);
        eq("cancel sector read",m.call("sceCdBreak",{}),1);
        eq("cancelled read is no longer busy",m.call("sceCdSync",{1}),0);
        eq("cancelled read reports SCECdErABRT",m.call("sceCdGetError",{}),1);
        eq("break does not undo bytes already delivered",m.rt->memory.ram()[0x104000],0x22);
        eq("break while idle succeeds",m.call("sceCdBreak",{}),1);
        eq("idle break stays idle",m.call("sceCdSync",{1}),0);
        eq("read after cancellation starts",m.call("sceCdRead",{23,1,0x104000,0}),1);
        eq("new read clears abort error",m.call("sceCdGetError",{}),0);
        eq("new read delivers its own sector",m.rt->memory.ram()[0x104000],0x33);
        eq("new read has its own completion time",m.call("sceCdSync",{1}),1);
        eq("native file close",m.call("sceClose",{uint32_t(fd)}),0);
        eq("closed descriptor cannot read",m.call("sceRead",{uint32_t(fd),0x103000,1}),-1);
    }
    std::filesystem::remove(path);
}

int main()
{
    reentrantAllocator();
    cardSlotAliases();
    nativeIsoFiles();
    nativeCardDirectories();
    liveScriptInput();
    {
        // Native graphics must reject the retired graphics paths without blocking movie input, T0 or VU0 data.
        for (uint32_t address : {0x10003000u, 0x10003C00u, 0x10008000u, 0x10009030u, 0x1000A000u,
                                 0x11008000u, 0x12001000u, 0xB2001000u})
            eq("retired graphics address", bozo::libs::graphicsIo(address), 1);
        for (uint32_t address : {0x10000000u, 0x10007000u, 0x1000B400u, 0x1000B410u, 0x1000E010u, 0x11004000u})
            eq("non-graphics service stays available", bozo::libs::graphicsIo(address), 0);
    }
    {
        // T0 on a temporary guest context must use that call's clock, including its reset epoch.
        // The thread's own context can still be at an earlier cycle while an override runs the temporary one.
        bozo::libs::Libraries libs;
        libs.io.t0Mode = 2; // 576 kHz: 512 EE cycles per tick
        bozo::libs::ioStore(libs, 0x10000000, 4, 7, 10000);
        uint32_t pending = 0;
        eq("T0 advances from the calling context's reset epoch",
           bozo::libs::ioLoad(libs, 0x10000000, 4, pending, 10000 + 10 * 512), 17);
        eq("T0 first read is not a poll", pending, 0);
        bozo::libs::ioLoad(libs, 0x10000000, 4, pending, 10000 + 10 * 512);
        eq("T0 repeated read charges one tick", pending, 512);
    }
    sifAndSnd();
    memoryCard();
    nativeCardFlushPersistence();
    heapAndRand();
    cdStream();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
