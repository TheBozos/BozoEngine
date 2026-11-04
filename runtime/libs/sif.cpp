// The SIF layer (libraries.md section 14): RPC to native servers, the IOP heap, module loading, the SIF registers and
// DMA, with no IOP. What TMB calls: sceSifInitRpc, sceSifBindRpc, sceSifCallRpc, sceSifCheckStatRpc, sceSifGetSreg,
// sceSifSetSreg, sceSifInitIopHeap, sceSifAllocIopHeap, sceSifFreeIopHeap, sceSifLoadModule, sceSifRebootIop,
// sceSifSyncIop (and, inside port overrides only, sceSifSetDma and sceSifDmaStat).
#include "bozo/libs/libraries.h"
#include "bozo/ee/dispatcher.h"

#include <algorithm>

namespace bozo::libs
{
    // ---- IopHeap -------------------------------------------------------------------------------------------------

    void IopHeap::reset()
    {
        free_.clear();
        used_.clear();
        free_[kStart] = kEnd - kStart;
    }

    uint32_t IopHeap::allocate(uint32_t bytes)
    {
        const uint32_t n = std::max<uint32_t>(16u, (bytes + 15u) & ~15u);
        for (auto it = free_.begin(); it != free_.end(); ++it)
            if (it->second >= n)
            {
                const uint32_t a = it->first, left = it->second - n;
                free_.erase(it);
                if (left)
                    free_[a + n] = left;
                used_[a] = n;
                return a;
            }
        return 0;
    }

    bool IopHeap::release(uint32_t address)
    {
        auto u = used_.find(address);
        if (u == used_.end())
            return false;
        uint32_t a = address, n = u->second;
        used_.erase(u);
        auto next = free_.lower_bound(a);
        if (next != free_.end() && next->first == a + n)
        {
            n += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == a)
            {
                prev->second += n;
                return true;
            }
        }
        free_[a] = n;
        return true;
    }

    uint32_t IopHeap::blockAt(uint32_t address) const
    {
        auto it = used_.upper_bound(address);
        if (it == used_.begin())
            return 0;
        --it;
        return address < it->first + it->second ? it->first : 0;
    }

    // ---- The library ---------------------------------------------------------------------------------------------

    namespace
    {
        // The services a native server answers: TMB's 989SND.
        constexpr uint32_t k989snd = 0x123456u;

        // sceSifInitRpc(mode): 0. The RPC layer needs no setup here.
        void initRpc(Libraries &, Guest &g) { g.ret(0); }

        // sceSifBindRpc(client, service, mode): the client header's rpc_id is the next one and its semaphore -1; once
        // the service's server exists, the client points at its server data (the reference keeps it in EE RAM at
        // 0x1f10000) and the server data starts with the service id. The reference's 989SND registered its server
        // after the game's first attempt (its IOP still starting the module), so a service binds at the second one.
        void bindRpc(Libraries &l, Guest &g)
        {
            const uint32_t client = g.arg(0), service = g.arg(1);
            SifState &s = l.sif;
            g.write32(client + 4, ++s.rpcId);
            g.write32(client + 8, 0xFFFFFFFFu);
            if (service == k989snd && s.bindAttempts[service]++ >= 1)
            {
                g.write32(client + 0x24, SifState::kServerData);
                g.write32(SifState::kServerData, service);
                s.bound[client] = service;
            }
            g.ret(0);
        }

        // Words of guest memory (zero past a null pointer).
        std::vector<uint32_t> words(Guest &g, uint32_t at, uint32_t bytes)
        {
            std::vector<uint32_t> w((bytes + 3) / 4, 0);
            if (at)
                for (size_t i = 0; i < w.size(); ++i)
                    w[i] = g.read32(at + static_cast<uint32_t>(4 * i));
            return w;
        }

        // sceSifCallRpc(client, fno, mode, send, ssize, receive, rsize, end_function, end_param): the server answers
        // within the call (the reference's replies all land inside it), so the call is complete when it returns; 0.
        // The client header keeps the mode and the function; the server data the request's fields.
        void callRpc(Libraries &l, Guest &g)
        {
            const uint32_t client = g.arg(0), fno = g.arg(1), mode = g.arg(2), send = g.arg(3);
            const uint32_t ssize = g.arg(4), recv = g.arg(5), rsize = g.arg(6);
            g.write32(client + 0x0C, mode);
            g.write32(client + 0x10, fno);
            const uint32_t server = g.read32(client + 0x24);
            if (server)
            {
                g.write32(server + 0x0C, ssize);
                g.write32(server + 0x1C, client);
                g.write32(server + 0x24, fno);
                g.write32(server + 0x28, recv);
                g.write32(server + 0x2C, rsize);
            }
            // 989SND's client (bound at sceSifBindRpc): not the server data's first word, which the game's own data can
            // overwrite (the story's second level loads over 0x1f10000; the call then went unanswered and the client
            // waited for its reply for ever).
            if (auto b = l.sif.bound.find(client); b != l.sif.bound.end() && b->second == k989snd)
            {
                SndDriver &snd = l.snd;
                snd.noteBuffers(send, recv);
                const std::vector<uint32_t> request = words(g, send, ssize);
                std::vector<uint32_t> reply;
                if (l.sndNative)
                {
                    snd.call(l, g, fno, request, recv, rsize, reply, l.pin.logWord ? &l.pin.logWord : nullptr, false);
                    for (size_t i = 0; i < reply.size() && 4 * i + 4 <= rsize; ++i)
                        g.write32(recv + static_cast<uint32_t>(4 * i), reply[i]);
                }
                else if (l.pin.logWord)
                {
                    // The driver answered from the log: its reply words, then the driver's state follows them.
                    for (uint32_t i = 0; i + 4 <= rsize; i += 4)
                    {
                        uint32_t v;
                        if (l.pin.logWord(recv + i, v))
                            g.write32(recv + i, v);
                    }
                    SndDriver::LogWord memory = [&g](uint32_t a, uint32_t &v) {
                        v = g.read32(a);
                        return true;
                    };
                    snd.call(l, g, fno, request, recv, rsize, reply, &memory, true);
                }
            }
            g.ret(0);
        }

        // sceSifCheckStatRpc(client): 0, the call is never in flight.
        void checkStatRpc(Libraries &, Guest &g) { g.ret(0); }

        // sceSifGetSreg(reg), sceSifSetSreg(reg, value) (returns the old value): the 32 SIF software registers.
        void getSreg(Libraries &l, Guest &g) { g.retU(l.sif.sreg[g.arg(0) & 31]); }
        void setSreg(Libraries &l, Guest &g)
        {
            uint32_t &r = l.sif.sreg[g.arg(0) & 31];
            const uint32_t old = r;
            r = g.arg(1);
            g.retU(old);
        }

        // sceSifInitIopHeap(): 0. sceSifAllocIopHeap(bytes): the block's IOP address (0 if none). sceSifFreeIopHeap
        // (address): 0.
        void initIopHeap(Libraries &, Guest &g) { g.ret(0); }
        void allocIopHeap(Libraries &l, Guest &g) { g.retU(l.sif.heap.allocate(g.arg(0))); }
        void freeIopHeap(Libraries &l, Guest &g)
        {
            l.sif.heap.release(g.arg(0));
            g.ret(0);
        }

        // sceSifLoadModule(path, args, argp): nothing is loaded (the driver behind each module is native); the module
        // id, 1 for the first module since the IOP's reboot, then 2, 3...
        void loadModule(Libraries &l, Guest &g) { g.ret(static_cast<int32_t>(++l.sif.modules)); }
        // sceSifRebootIop(image): the IOP starts again with no module; 1. sceSifSyncIop(): 1 (the IOP is up).
        void rebootIop(Libraries &l, Guest &g)
        {
            l.sif.modules = 0;
            l.snd.iopReset(l.now(g));
            g.ret(1);
        }
        void syncIop(Libraries &, Guest &g) { g.ret(1); }

        // sceSifSetDma(transfers, count): the transfer's id (1, 2, ...): the data reaches the IOP at once, and the
        // DMAC interrupt of channel 5 (SIF0) is raised as the call returns (boot #2157635: inside TMB's
        // audioDecSendToIOP override, at sceSifSetDma's return address, one per call).
        // sceSifDmaStat(id): -1, finished.
        // The transfers (16 bytes each: EE source, IOP destination, bytes, attributes) go to the sound driver's IOP RAM
        // (TMB's only transfers: the movie's PCM into 989SND's movie buffer).
        void setDma(Libraries &l, Guest &g)
        {
            if (l.sndNative)
            {
                const uint32_t transfers = g.arg(0), count = g.arg(1);
                for (uint32_t i = 0; i < count && i < 32; ++i)
                {
                    const uint32_t t = transfers + 16u * i;
                    const uint32_t source = g.read32(t), dest = g.read32(t + 4), bytes = g.read32(t + 8);
                    if (bytes && bytes <= 0x200000u)
                        l.snd.iopWrite(l, l.now(g), dest, g.in(source, bytes), bytes);
                }
            }
            g.retU(++l.sif.dmaId);
            l.io.stat |= 1u << 5;
            l.dispatcher()->kernel().irq(true, 5);
        }
        void dmaStat(Libraries &, Guest &g) { g.ret(-1); }
    }

    void addSif(std::vector<Function> &out)
    {
        out.push_back({"sceSifInitRpc", kSif, initRpc});
        out.push_back({"sceSifBindRpc", kSif, bindRpc});
        out.push_back({"sceSifCallRpc", kSif, callRpc, false, false, true});
        out.push_back({"sceSifCheckStatRpc", kSif, checkStatRpc});
        out.push_back({"sceSifGetSreg", kSif, getSreg});
        out.push_back({"sceSifSetSreg", kSif, setSreg});
        out.push_back({"sceSifInitIopHeap", kSif, initIopHeap});
        out.push_back({"sceSifAllocIopHeap", kSif, allocIopHeap});
        out.push_back({"sceSifFreeIopHeap", kSif, freeIopHeap});
        out.push_back({"sceSifLoadModule", kSif, loadModule});
        out.push_back({"sceSifRebootIop", kSif, rebootIop});
        out.push_back({"sceSifSyncIop", kSif, syncIop});
        out.push_back({"sceSifSetDma", kSif, setDma});
        out.push_back({"sceSifDmaStat", kSif, dmaStat});
    }
}
