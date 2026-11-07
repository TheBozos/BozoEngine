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
        m.string(0x100000, "x%d");
        eq("scePrintf formats as printf", m.call("scePrintf", {0x100000, 42}), 3);
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

    {
        Machine q;
        eq("IOP RAM size", q.call("sceSifQueryMemSize", {}), 0x200000);
        eq("fresh heap's largest block", q.call("sceSifQueryMaxFreeMemSize", {}), 0x200000 - 0x7FE60);
        eq("fresh heap's free total", q.call("sceSifQueryTotalFreeMemSize", {}), 0x200000 - 0x7FE60);
        const uint32_t a = static_cast<uint32_t>(q.call("sceSifAllocIopHeap", {0x10000}));
        const uint32_t b = static_cast<uint32_t>(q.call("sceSifAllocIopHeap", {0x8000}));
        q.call("sceSifFreeIopHeap", {a});
        eq("largest block after a hole", q.call("sceSifQueryMaxFreeMemSize", {}), 0x200000 - 0x7FE60 - 0x18000);
        eq("free total counts the hole", q.call("sceSifQueryTotalFreeMemSize", {}), 0x200000 - 0x7FE60 - 0x8000);
        (void)b;
    }

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
    std::vector<std::string> arguments;
    m.libs.sif.moduleLoaderWithArguments = [&](const std::string &, const std::vector<std::string> &given)
    {
        arguments = given;
        return 7;
    };
    std::memcpy(m.rt->memory.ram() + 0x100500, "thpri=39\0spucore=2\0", 20);
    eq("a module loaded with arguments", m.call("sceSifLoadModule", {0x100400, 20, 0x100500}), 7);
    eq("its arguments split at their NULs", arguments == std::vector<std::string>({"thpri=39", "spucore=2"}), 1);
    eq("a module loaded with none", m.call("sceSifLoadModule", {0x100400, 0, 0}), 7);
    eq("no arguments", arguments.empty(), 1);
    m.libs.sif.moduleLoaderWithArguments = {};
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
void nativeMultitap() {
    Machine m;
    eq("sceMtapInit",m.call("sceMtapInit",{}),1);
    eq("MTAPMAN major version 3",m.call("sceMtapGetModVersion",{})>>8,3);
    eq("no multitap in port 2",m.call("sceMtapPortOpen",{1}),0);
    eq("multitap in port 1",m.call("sceMtapPortOpen",{0}),1);
    eq("libpad sees it",m.libs.pad.multitap,true);
    eq("port 1 connected",m.call("sceMtapGetConnection",{0}),1);
    eq("port 2 not connected",m.call("sceMtapGetConnection",{1}),0);
    eq("four slots behind the multitap",m.call("scePadGetSlotMax",{0}),4);
    eq("one slot on port 2",m.call("scePadGetSlotMax",{1}),1);
    eq("close",m.call("sceMtapPortClose",{0}),1);
    eq("libpad sees it closed",m.libs.pad.multitap,false);
    eq("one slot without the multitap",m.call("scePadGetSlotMax",{0}),1);
    eq("scePadPortClose",m.call("scePadPortClose",{0,0}),1);
    eq("scePadEnd",m.call("scePadEnd",{}),1);
    m.libs.pad.multitapAttached=false;
    eq("unplugged multitap does not open",m.call("sceMtapPortOpen",{0}),0);
    eq("unplugged multitap is not connected",m.call("sceMtapGetConnection",{0}),0);
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
        eq("native file close",m.call("sceClose",{uint32_t(fd)}),0);
        eq("closed descriptor cannot read",m.call("sceRead",{uint32_t(fd),0x103000,1}),-1);
        uint32_t lbn;std::memcpy(&lbn,m.rt->memory.ram()+0x102000,4);
        eq("no IOP memory, no IOP read",m.call("sceCdReadIOPm",{lbn,2,0x4000,0}),0);
        uint32_t iopAt=0;std::vector<uint8_t> iopData;
        m.libs.sif.transfer=[&](uint32_t at,const uint8_t *from,uint32_t bytes){iopAt=at;iopData.assign(from,from+bytes);};
        eq("sectors read into IOP memory",m.call("sceCdReadIOPm",{lbn,2,0x4000,0}),1);
        eq("the IOP address",iopAt,0x4000);
        eq("two sectors",iopData.size(),4096);
        eq("the first sector's tail",iopData[2047],0x22);
        eq("the next sector's head",iopData[2048],0x33);
        eq("sceCdSync sees the read",m.call("sceCdSync",{1}),1);
    }
    std::filesystem::remove(path);
}
void nativeDualLayerIso() {
    const auto word=[](uint8_t *at,uint32_t value){for(int i=0;i<4;++i){at[i]=uint8_t(value>>(8*i));at[7-i]=at[i];}};
    const auto record=[&](uint8_t *at,const std::string &name,uint32_t sector,uint32_t bytes,bool directory){
        const uint8_t size=uint8_t((33+name.size()+1)&~1u);
        at[0]=size;word(at+2,sector);word(at+10,bytes);at[25]=directory?2:0;
        at[28]=1;at[31]=1;at[32]=uint8_t(name.size());std::memcpy(at+33,name.data(),name.size());return size;
    };
    const auto volume=[&](uint8_t *base,uint32_t sectors,const std::string &file,uint8_t fill){
        uint8_t *pvd=base+16*2048;pvd[0]=1;std::memcpy(pvd+1,"CD001",5);pvd[6]=1;word(pvd+80,sectors);
        record(pvd+156,std::string(1,'\0'),18,2048,true);
        record(base+18*2048,"LEVELS",19,2048,true);
        record(base+19*2048,file,20,2048,false);
        std::memset(base+20*2048,fill,2048);
    };
    std::vector<uint8_t> iso((24+16)*2048);
    volume(iso.data(),24,"A.BIN;1",0xAA);
    std::vector<uint8_t> second(32*2048);
    volume(second.data(),32,"B.BIN;1",0xBB);
    std::memcpy(iso.data()+24*2048,second.data()+16*2048,16*2048);
    const auto path=std::filesystem::temp_directory_path()/("bozo-dvd9-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
    {std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char *>(iso.data()),iso.size());}
    {
        Machine m;m.libs.cd.imagePath=path.string();
        eq("dual-layer info returns",m.call("sceCdReadDvdDualInfo",{0x104000,0x104004}),1);
        uint32_t dual,start;std::memcpy(&dual,m.rt->memory.ram()+0x104000,4);std::memcpy(&start,m.rt->memory.ram()+0x104004,4);
        eq("image is dual-layer",dual,1);
        eq("second layer starts 16 sectors before its descriptor",start,8);
        m.string(0x101000,"\\LEVELS\\B.BIN;1");
        eq("layer 1 search",m.call("sceCdLayerSearchFile",{0x102000,0x101000,1}),1);
        uint32_t lbn;std::memcpy(&lbn,m.rt->memory.ram()+0x102000,4);eq("layer 1 extent is the disc's own sector",lbn,28);
        eq("layer 0 search does not see layer 1's file",m.call("sceCdSearchFile",{0x102000,0x101000}),0);
        m.string(0x101000,"\\LEVELS");
        eq("directory search on layer 1",m.call("sceCdLayerSearchFile",{0x102000,0x101000,1}),1);
        std::memcpy(&lbn,m.rt->memory.ram()+0x102000,4);eq("layer 1 directory sector",lbn,27);
        m.string(0x101000,"cdrom0:\\levels\\a.bin;1");
        eq("layer 0 search through the layer call",m.call("sceCdLayerSearchFile",{0x102000,0x101000,0}),1);
        std::memcpy(&lbn,m.rt->memory.ram()+0x102000,4);eq("layer 0 extent",lbn,20);
        eq("no third layer",m.call("sceCdLayerSearchFile",{0x102000,0x101000,2}),0);
    }
    std::filesystem::remove(path);
    std::vector<uint8_t> single(24*2048);
    volume(single.data(),24,"A.BIN;1",0xAA);
    const auto singlePath=std::filesystem::temp_directory_path()/("bozo-dvd5-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
    {std::ofstream f(singlePath,std::ios::binary);f.write(reinterpret_cast<const char *>(single.data()),single.size());}
    {
        Machine m;m.libs.cd.imagePath=singlePath.string();
        {const uint32_t ones=0xFFFFFFFFu;std::memcpy(m.rt->memory.ram()+0x104000,&ones,4);}
        eq("single-layer info returns",m.call("sceCdReadDvdDualInfo",{0x104000,0x104004}),1);
        uint32_t dual;std::memcpy(&dual,m.rt->memory.ram()+0x104000,4);eq("single-layer image",dual,0);
        m.string(0x101000,"\\LEVELS\\A.BIN;1");
        eq("no layer 1 on a single-layer image",m.call("sceCdLayerSearchFile",{0x102000,0x101000,1}),0);
    }
    std::filesystem::remove(singlePath);
}

static void padCalls()
{
    Machine m;
    m.call("scePadInit", {0});
    eq("no buffer before PortOpen", m.call("scePadGetDmaStr", {0, 0}), 0);
    m.call("scePadPortOpen", {0, 0, 0x100000});
    eq("the port's buffer", m.call("scePadGetDmaStr", {0, 0}), 0x100000);
    m.libs.framesEnded = 7;
    eq("frames counted while open", m.call("scePadGetFrameCount", {0, 0}), 7);
    eq("no count when closed", m.call("scePadGetFrameCount", {1, 0}), 0);
    eq("two ports", m.call("scePadGetPortMax", {}), 2);
    eq("the padman version libpad asks for", m.call("scePadGetModVersion", {}) >> 8, 4);
    eq("no combinations", m.call("scePadInfoComb", {0, 0, 0xFFFFFFFFu, 0}), 0);
    eq("warning level", m.call("scePadSetWarningLevel", {1}), 0);
    eq("vref", m.call("scePadSetVrefParam", {0, 0, 0x100100}), 1);
    eq("second init", m.call("scePadInit2", {0}), 1);

    eq("req state set", m.call("scePadSetReqState", {0, 0, 2}), 1);
    eq("req state read", m.call("scePadGetReqState", {0, 0}), 2);
    eq("another port's unchanged", m.call("scePadGetReqState", {1, 0}), 0);

    m.call("scePadSetMainMode", {0, 0, 1, 3});
    m.call("scePadEnterPressMode", {0, 0});
    m.libs.framesEnded = 10;
    m.call("scePadRead", {0, 0, 0x100200});
    eq("pressure mode", m.rt->memory.ram()[0x100201], 0x79);
    eq("exit press", m.call("scePadExitPressMode", {0, 0}), 1);
    eq("busy after a command", m.call("scePadGetState", {0, 0}), 5);
    m.libs.framesEnded = 12;
    m.call("scePadRead", {0, 0, 0x100200});
    eq("analog, not pressure", m.rt->memory.ram()[0x100201], 0x73);
    eq("button info", m.call("scePadSetButtonInfo", {0, 0, 0xFFF}), 1);
    eq("busy after button info", m.call("scePadGetState", {0, 0}), 5);

    m.call("scePadStateIntToStr", {6, 0x100300});
    eq("state name", std::strcmp(reinterpret_cast<const char *>(m.rt->memory.ram() + 0x100300), "STABLE"), 0);
    m.call("scePadStateIntToStr", {99, 0x100300});
    eq("no state name", m.rt->memory.ram()[0x100300], 0);
    m.call("scePadReqIntToStr", {2, 0x100300});
    eq("req name", std::strcmp(reinterpret_cast<const char *>(m.rt->memory.ram() + 0x100300), "BUSY"), 0);
}

static void cardCalls()
{
    Machine m;
    auto put = [&](uint32_t at, const char *s) { std::strcpy(reinterpret_cast<char *>(m.rt->memory.ram() + at), s); };
    put(0x100400, "DIR");
    put(0x100440, "DIR/FILE");
    put(0x100480, "NEW");
    put(0x1004C0, "DIR/NEW");
    eq("one slot", m.call("sceMcGetSlotMax", {0}), 1);
    m.call("sceMcMkdir", {0, 0, 0x100400});
    eq("mkdir", m.result(), 0);
    m.call("sceMcOpen", {0, 0, 0x100440, 0x203});
    const int32_t fd = m.result();
    m.call("sceMcWrite", {uint32_t(fd), 0x100000, 100});
    eq("wrote", m.result(), 100);

    m.call("sceMcSeek", {uint32_t(fd), 10, 0});
    eq("seek from the start", m.result(), 10);
    m.call("sceMcSeek", {uint32_t(fd), 5, 1});
    eq("seek from here", m.result(), 15);
    m.call("sceMcSeek", {uint32_t(fd), 0xFFFFFFFEu, 2});
    eq("seek from the end", m.result(), 98);
    m.call("sceMcSeek", {uint32_t(fd), 0xFFFFFF00u, 0});
    eq("before the start", m.result(), -4);
    m.call("sceMcSeek", {999, 0, 0});
    eq("seek on no file", m.result(), -4);
    m.call("sceMcFlush", {uint32_t(fd)});
    eq("flush", m.result(), 0);
    m.call("sceMcFlush", {999});
    eq("flush of no file", m.result(), -4);

    m.call("sceMcDelete", {0, 0, 0x100440});
    eq("an open file stays", m.result(), -5);
    m.call("sceMcRename", {0, 0, 0x100440, 0x100480});
    eq("an open file keeps its name", m.result(), -5);
    m.call("sceMcClose", {uint32_t(fd)});
    m.result();

    uint8_t info[64] = {};
    info[5] = 11;
    info[8 + 5] = 12;
    info[20] = 0x27;
    std::memcpy(m.rt->memory.ram() + 0x100500, info, 64);
    m.call("sceMcSetFileInfo", {0, 0, 0x100440, 0x100500, 6});
    eq("set info", m.result(), 0);
    const auto *entry = m.libs.mc.card(0).find("DIR/FILE");
    eq("created kept", entry->created.month, 6);
    eq("modified set", entry->modified.month, 12);
    eq("attributes set", entry->attributes, 0x27);
    m.call("sceMcSetFileInfo", {0, 0, 0x100480, 0x100500, 6});
    eq("info of nothing", m.result(), -4);

    m.call("sceMcGetEntSpace", {0, 0, 0x100400});
    eq("entries free", m.result() > 0, 1);
    m.call("sceMcGetEntSpace", {0, 0, 0x100480});
    eq("no such directory", m.result(), -4);

    m.call("sceMcRename", {0, 0, 0x100440, 0x100480});
    eq("rename", m.result(), 0);
    eq("under the new name", m.libs.mc.card(0).exists("DIR/NEW"), 1);
    eq("not the old", m.libs.mc.card(0).exists("DIR/FILE"), 0);
    m.call("sceMcRename", {0, 0, 0x100440, 0x100480});
    eq("rename of nothing", m.result(), -4);
    m.call("sceMcDelete", {0, 0, 0x100400});
    eq("a directory with entries", m.result(), -6);
    m.call("sceMcDelete", {0, 0, 0x1004C0});
    eq("delete", m.result(), 0);
    m.call("sceMcDelete", {0, 0, 0x100400});
    eq("the empty directory", m.result(), 0);
    m.call("sceMcDelete", {0, 0, 0x100400});
    eq("deleted", m.result(), -4);

    m.call("sceMcMkdir", {0, 0, 0x100400});
    m.result();
    m.call("sceMcUnformat", {0, 0});
    eq("unformat", m.result(), 0);
    eq("empty", m.libs.mc.card(0).exists("DIR"), 0);
    m.call("sceMcGetInfo", {0, 0, 0, 0, 0x100600});
    m.result();
    uint32_t format = 1;
    std::memcpy(&format, m.rt->memory.ram() + 0x100600, 4);
    eq("not formatted", format, 0);
    m.call("sceMcFormat", {0, 0});
    eq("format", m.result(), 0);
    m.call("sceMcGetInfo", {0, 0, 0, 0, 0x100600});
    m.result();
    std::memcpy(&format, m.rt->memory.ram() + 0x100600, 4);
    eq("formatted", format, 1);

    m.call("sceMcChangeThreadPriority", {0x20});
    eq("priority", m.result(), 0);
    eq("end", m.call("sceMcEnd", {}), 0);

    const auto dir = std::filesystem::temp_directory_path() /
                     ("bozo-mc-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Machine host;
    host.libs.mc.hostDirectory = dir.string();
    auto hostPut = [&](uint32_t at, const char *s) { std::strcpy(reinterpret_cast<char *>(host.rt->memory.ram() + at), s); };
    hostPut(0x100400, "DIR");
    hostPut(0x100440, "DIR/FILE");
    hostPut(0x100480, "NEW");
    host.call("sceMcMkdir", {0, 0, 0x100400});
    host.result();
    host.call("sceMcOpen", {0, 0, 0x100440, 0x203});
    const int32_t hostFd = host.result();
    host.call("sceMcClose", {uint32_t(hostFd)});
    host.result();
    eq("on the host", std::filesystem::exists(dir / "mc0" / "DIR" / "FILE"), 1);
    host.call("sceMcRename", {0, 0, 0x100440, 0x100480});
    host.result();
    eq("renamed on the host", std::filesystem::exists(dir / "mc0" / "DIR" / "NEW"), 1);
    eq("old name gone on the host", std::filesystem::exists(dir / "mc0" / "DIR" / "FILE"), 0);
    host.call("sceMcFormat", {0, 0});
    host.result();
    eq("formatted on the host", std::filesystem::exists(dir / "mc0" / "DIR"), 0);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

static void dmaCalls()
{
    Machine m;
    eq("VIF0's handle", m.call("sceDmaGetChan", {0}), 0x10008000);
    eq("from scratchpad", m.call("sceDmaGetChan", {8}), 0x1000D000);
    eq("to scratchpad", m.call("sceDmaGetChan", {9}), 0x1000D400);
    eq("no such channel", m.call("sceDmaGetChan", {10}), 0);

    for (uint32_t i = 0; i < 32; ++i)
        m.rt->memory.ram()[0x100000 + i] = static_cast<uint8_t>(i + 1);
    bozo::libs::ioStore(m.libs, 0x1000D480, 4, 0x20, 0);
    eq("send to the scratchpad", m.call("sceDmaSendN", {0x1000D400, 0x100000, 2}), 0);
    eq("in the scratchpad", m.rt->memory.scratchpad()[0x20 + 17], 18);
    eq("sync of a copy", m.call("sceDmaSync", {0x1000D400, 1, 30}), 0);
    bozo::libs::ioStore(m.libs, 0x1000D080, 4, 0x20, 0);
    eq("receive from the scratchpad", m.call("sceDmaRecvN", {0x1000D000, 0x100100, 2}), 0);
    eq("in memory", m.rt->memory.ram()[0x100100 + 31], 32);
    eq("the store is known", bozo::libs::ioKnown(0x1000D080, 4, true), 1);
    eq("the load is known", bozo::libs::ioKnown(0x1000D080, 4, false), 1);
    {
        uint32_t pending = 0;
        eq("and reads the store back", static_cast<uint32_t>(bozo::libs::ioLoad(m.libs, 0x1000D080, 4, pending, 0)), 0x20);
    }
    eq("SIGLBLID is stored", bozo::libs::ioKnown(0x12001080u, 8, true), 1);
    eq("SIGLBLID is not read", bozo::libs::ioKnown(0x12001080u, 8, false), 0);

    eq("no watch left", m.call("sceDmaWatch", {0x10009000, 0x100000, 1, 0}), 0);
    eq("old stall address", m.call("sceDmaPutStallAddr", {0x100000}), 0);
    eq("new stall address", m.call("sceDmaPutStallAddr", {0}), 0x100000);
    m.libs.io.channels[1].chcr = 0x145;
    eq("pause gives the old CHCR", m.call("sceDmaPause", {0x10009000}), 0x145);
    eq("restart tells it was stopped", m.call("sceDmaRestart", {0x10009000, 0x145}), 0);
    eq("restart tells it was started", m.call("sceDmaRestart", {0x10009000, 0x45}), 1);
}

static void sifCalls()
{
    Machine m;
    for (const char *name : {"sceSifExitRpc", "sceSifExitCmd", "sceSifLoadFileReset"})
        eq(name, m.call(name, {}), 0);
    eq("write back", m.call("sceSifWriteBackDCache", {0x100000, 64}), 0);
    eq("alive", m.call("sceSifIsAliveIop", {}), 1);
    const int32_t block = m.call("sceSifAllocSysMemory", {0, 0x100, 0});
    eq("a block", block != 0, 1);
    eq("freed", m.call("sceSifFreeSysMemory", {uint32_t(block)}), 0);
    eq("freed once", m.call("sceSifFreeSysMemory", {uint32_t(block)}), -1);
    std::strcpy(reinterpret_cast<char *>(m.rt->memory.ram() + 0x100400), "cdrom0:\\M.IRX;1");
    eq("module 1", m.call("sceSifLoadStartModule", {0x100400, 0, 0, 0x100500}), 1);
    uint32_t result = 1;
    std::memcpy(&result, m.rt->memory.ram() + 0x100500, 4);
    eq("resident", result, 0);
    eq("buffer", m.call("sceSifLoadModuleBuffer", {0x100000, 0, 0}), 2);
    eq("inner buffer", m.call("_sceSifLoadModuleBuffer", {0x100000, 0, 0}), 3);
    eq("start buffer", m.call("sceSifLoadStartModuleBuffer", {0x100000, 0, 0, 0}), 4);
    m.libs.sndNative = false;
    eq("reset", m.call("sceSifResetIop", {0, 0}), 1);
    eq("modules gone", m.call("sceSifLoadModuleBuffer", {0x100000, 0, 0}), 1);
}

static void driveCalls()
{
    Machine m;
    bozo::libs::CdState &cd = m.libs.cd;
    eq("no disc", m.call("sceCdGetDiskType", {}), 0);
    cd.imagePath = "game.iso";
    eq("a DVD", m.call("sceCdGetDiskType", {}), 0x14);
    eq("ready", m.call("sceCdNcmdDiskReady", {}), 2);
    eq("no S command pending", m.call("sceCdSyncS", {1}), 0);
    eq("a break with nothing to break", m.call("sceCdBreak", {}), 1);
    eq("no error from it", m.call("sceCdGetError", {}), 0);

    eq("seek accepted", m.call("sceCdSeek", {1000}), 1);
    eq("the seek takes time", static_cast<long long>(cd.readEnd), static_cast<long long>(bozo::libs::kSeekCycles));
    eq("busy", m.call("sceCdNcmdDiskReady", {}), 6);
    eq("sync sees it busy", m.call("sceCdSync", {1}), 1);
    eq("break", m.call("sceCdBreak", {}), 1);
    eq("no longer busy", m.call("sceCdSync", {1}), 0);
    eq("aborted", m.call("sceCdGetError", {}), 1);
    eq("ready again", m.call("sceCdNcmdDiskReady", {}), 2);
    m.call("sceCdSeek", {1000});
    eq("a new command clears the error", m.call("sceCdGetError", {}), 0);
}

static void debugCalls()
{
    Machine m;
    eq("tty up", m.call("sceTtyInit", {}), 1);
    eq("every byte taken", m.call("sceTtyWrite", {0x100000, 16}), 16);
    eq("nothing to read", m.call("sceTtyRead", {0x100000, 16}), 0);
    for (const char *name : {"sceDeci2Open", "sceDeci2GrpOpen", "sceDeci2ReqSend", "sceDeci2Poll", "sceDeci2ExRecv",
                             "sceDeci2ExSend", "sceDeci2ExReqSend", "sceDeci2ExLock", "sceDeci2ExUnLock",
                             "sceDeci2Close", "sceDeci2GrpClose", "Deci2Call"})
        eq(name, m.call(name, {}), -1);
}

int main()
{
    padCalls();
    cardCalls();
    dmaCalls();
    sifCalls();
    debugCalls();
    driveCalls();
    reentrantAllocator();
    cardSlotAliases();
    nativeMultitap();
    nativeIsoFiles();
    nativeDualLayerIso();
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
        libs.io.timers[0].mode = 2; // 576 kHz: 512 EE cycles per tick
        bozo::libs::ioStore(libs, 0x10000000, 4, 7, 10000);
        uint32_t pending = 0;
        eq("T0 advances from the calling context's reset epoch",
           bozo::libs::ioLoad(libs, 0x10000000, 4, pending, 10000 + 10 * 512), 17);
        eq("T0 first read is not a poll", pending, 0);
        bozo::libs::ioLoad(libs, 0x10000000, 4, pending, 10000 + 10 * 512);
        eq("T0 repeated read charges one tick", pending, 512);
    }
    {
        // Timers 1-3 and the compare registers, read from the EE clock.
        for (uint32_t address : {0x10000020u, 0x10000800u, 0x10000810u, 0x10000820u, 0x10001000u, 0x10001010u, 0x10001020u,
                                 0x10001800u, 0x10001810u, 0x10001820u})
            eq("a timer register is known", bozo::libs::ioKnown(address, 4, false), 1);
        eq("Tn_HOLD is not", bozo::libs::ioKnown(0x10000030u, 4, false), 0);
        for (const uint32_t address : {0x10003020u, 0x10003800u, 0x10003C00u})
        {
            eq("an idle path's status is read", bozo::libs::ioKnown(address, 4, false), 1);
            eq("and not stored", bozo::libs::ioKnown(address, 4, true), 0);
        }
        for (const uint32_t address : {0x10003010u, 0x10003820u, 0x10003C20u})
        {
            eq("a path's mode or error mask is stored", bozo::libs::ioKnown(address, 4, true), 1);
            eq("and not read", bozo::libs::ioKnown(address, 4, false), 0);
        }
        eq("IPU_CTRL is read", bozo::libs::ioKnown(0x10002010u, 4, false), 1);
        eq("IPU_CTRL is stored", bozo::libs::ioKnown(0x10002010u, 4, true), 1);
        eq("IPU_CMD is read", bozo::libs::ioKnown(0x10002000u, 4, false), 1);
        eq("IPU_CMD is not stored", bozo::libs::ioKnown(0x10002000u, 4, true), 0);
        eq("IMR is stored", bozo::libs::ioKnown(0x12001010u, 8, true), 1);
        eq("IMR is not read", bozo::libs::ioKnown(0x12001010u, 8, false), 0);
        eq("a timer register is a word", bozo::libs::ioKnown(0x10001800u, 2, false), 0);

        bozo::libs::Libraries libs;
        uint32_t pending = 0;
        constexpr uint64_t line = 4920115ull * 2 / 525;
        eq("T3's mode as the kernel leaves it", bozo::libs::ioLoad(libs, 0x10001810, 4, pending, 0), 0x83);
        eq("T3 counts horizontal blanks from boot", bozo::libs::ioLoad(libs, 0x10001800, 4, pending, 5 * line), 5);
        eq("T3's first read is not a poll", pending, 0);
        bozo::libs::ioLoad(libs, 0x10001800, 4, pending, 5 * line + 1);
        eq("T3's repeated read charges a line", pending, line);

        bozo::libs::ioStore(libs, 0x10001010, 4, 0x82, 1000);
        bozo::libs::ioStore(libs, 0x10001000, 4, 40, 1000);
        bozo::libs::ioStore(libs, 0x10001020, 4, 77, 1000);
        pending = 0;
        eq("T2 counts at the clock its mode names", bozo::libs::ioLoad(libs, 0x10001000, 4, pending, 1000 + 3 * 512), 43);
        eq("T2's mode reads back", bozo::libs::ioLoad(libs, 0x10001010, 4, pending, 1000), 0x82);
        eq("T2's compare reads back", bozo::libs::ioLoad(libs, 0x10001020, 4, pending, 1000), 77);
        eq("the count wraps at 16 bits", bozo::libs::ioLoad(libs, 0x10001000, 4, pending, 1000 + 0x10000ull * 512), 40);
    }
    {
        // A frame-budget calibration: T0 at BUSCLK/256 measured over 256 changes of T3's count, each poll a few cycles.
        bozo::libs::Libraries libs;
        uint64_t cycle = 123456;
        uint32_t pending = 0;
        const auto poll = [&] {
            pending = 0;
            const uint64_t v = bozo::libs::ioLoad(libs, 0x10001800, 4, pending, cycle);
            cycle += pending + 40;
            return v;
        };
        bozo::libs::ioStore(libs, 0x10000010, 4, 0x82, cycle);
        for (uint64_t first = poll(); poll() == first;)
            ;
        bozo::libs::ioStore(libs, 0x10000000, 4, 0, cycle);
        for (int i = 0; i < 256; ++i)
            for (uint64_t last = poll(); poll() == last;)
                ;
        pending = 0;
        const uint64_t ticks = bozo::libs::ioLoad(libs, 0x10000000, 4, pending, cycle);
        constexpr uint64_t console = 256ull * (4920115ull * 2 / 525) / 512;
        eq("T0 over 256 lines is the console's, within 1%", ticks >= console - console / 100 && ticks <= console + console / 100, 1);
    }
    sifAndSnd();
    memoryCard();
    nativeCardFlushPersistence();
    heapAndRand();
    cdStream();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
