// The SIF layer as the native runtime answers it (libraries.md section 14): the EE side of the EE-IOP interface with
// no IOP behind it. RPC calls go to native servers (TMB's only one: the 989SND sound driver, snd.h); module loading
// loads nothing and gives the reference's module ids; the IOP heap is a model of the IOP memory the reference's
// allocator handed out (989SND's banks and buffers come from it too).
#pragma once

#include <cstdint>
#include <map>
#include <functional>
#include <string>
#include <unordered_map>
#include <span>
#include <vector>

namespace bozo::libs
{
    class Libraries;
    class Guest;
    struct NativeRpcRequest {
        uint32_t command, send, sendBytes, receive, receiveBytes;
    };
    // IOP memory as the reference's IOP allocator gave it out (libraries.md section 14.3): blocks aligned to 16
    // bytes, first fit by address, from the first byte its loaded modules left free; a freed block merges with its free
    // neighbours.
    class IopHeap
    {
    public:
        static constexpr uint32_t kStart = 0x7FE60u; // the reference's first free byte after the IOP modules
        static constexpr uint32_t kEnd = 0x200000u;  // 2 MB of IOP RAM
        IopHeap() { reset(); }
        void reset();
        uint32_t allocate(uint32_t bytes); // 0 when no free block is large enough
        bool release(uint32_t address);    // a block's start; false when it is not one in use
        // The block in use that contains `address` (its start), 0 if none.
        uint32_t blockAt(uint32_t address) const;
        uint32_t largestFree() const;
        uint32_t totalFree() const;

    private:
        std::map<uint32_t, uint32_t> free_; // start -> bytes
        std::map<uint32_t, uint32_t> used_;
    };

    struct SifState
    {
        // The reference keeps the IOP side of each bound server (its SifRpcServerData) in EE RAM; the 989SND's at
        // 0x1f10000. A call writes the request's fields there.
        static constexpr uint32_t kServerData = 0x1F10000u;
        // Native service providers registered by a port or reusable library. No IOP/device backend.
        using RpcHandler = std::function<void(Libraries &, Guest &, const NativeRpcRequest &)>;
        std::map<uint32_t, RpcHandler> servers;
        // Native EE services that compiled IOP modules bind (sceSifBindRpc) and call (sceSifCallRpc) by service id: the
        // command, the bytes sent, and the reply to fill (the caller's receive size). The call completes when it returns.
        using EeRpcHandler = std::function<void(uint32_t, std::span<const uint8_t>, std::span<uint8_t>)>;
        std::map<uint32_t, EeRpcHandler> eeServers;
        std::function<void(uint32_t, const uint8_t *, uint32_t)> transfer;
        std::function<int32_t(const std::string &)> moduleLoader;
        // Takes the place of moduleLoader when set: also given sceSifLoadModule's arguments, split at their NULs.
        std::function<int32_t(const std::string &, const std::vector<std::string> &)> moduleLoaderWithArguments;
        std::function<void()> resetProvider;
        std::function<uint32_t(uint32_t)> allocateBuffer;
        std::function<bool(uint32_t)> freeBuffer;
        // Direct native user-message receiver: command, data, extra destination/data.
        // The receiver copies data and queues its actual application callback.
        std::function<uint32_t(uint32_t,std::span<const uint8_t>,uint32_t,std::span<const uint8_t>)> receiveCommand;
        uint32_t rpcId = 0;                       // the client headers' rpc_id: one more at each bind attempt
        std::map<uint32_t, uint32_t> bindAttempts; // per service id
        // The service each bound client header calls (by the client's address). The server data in EE RAM is the game's
        // memory as much as ours (a level's heap reaches 0x1f10000 and overwrites it), so a call finds its server here.
        std::map<uint32_t, uint32_t> bound;
        uint32_t sreg[32] = {};
        // The IOP side's 32 software registers, as compiled modules read and write them (sifcmd sceSifGetSreg 6,
        // sceSifSetSreg 7). The SDK's set-sreg system command (0x80000001: index at +0x10, value at +0x14 of the packet)
        // writes the other side's: a module's sceSifSendCmd sets sreg above; a port answering the EE's sets this one.
        uint32_t iopSreg[32] = {};
        uint32_t dmaId = 0;  // the last SIF DMA transfer's id (sceSifSetDma: 1, 2, ...)
        uint32_t modules = 0; // modules loaded since the IOP's reboot (their ids are 1, 2, ...)
        IopHeap heap;
    };
}
