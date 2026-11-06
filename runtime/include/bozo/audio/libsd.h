// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// libsd (the IOP's sound library) answered at its API on the native mixer (bozo/audio/mixer.h): the sound drivers a
// game runs on the IOP keep calling libsd, and each call acts on the mixer: a voice parameter, address or switch is
// that voice's playback state, an upload (sceSdVoiceTrans) a copy into the mixer's sound memory, done when the call
// returns. libsd's entries are a parameter, switch or address id | voice << 1 | core.
//
// The caller decodes libsd's calling convention (the export's arguments, its result in v0) and gives the IOP's side
// through LibsdHost. When the mixer is clocked, every call first brings it up to the IOP's cycle (Mixer::advanceTo),
// so what a driver sees changes only with the IOP's own time (docs/specs/libraries.md section 15.14).
//
// Moved from the team's own code on the PS2Recomp fork (PLAN.md section 7).
#pragma once

#include "bozo/audio/mixer.h"

#include <cstdint>
#include <unordered_map>

namespace bozo::audio
{
    // What libsd needs of the IOP.
    class LibsdHost
    {
    public:
        virtual ~LibsdHost() = default;
        // The IOP's clock now (its cycles: 36.864 MHz).
        virtual uint64_t iopCycle() = 0;
        // The IOP's RAM (2 MB): addresses are taken & 0x1FFFFFFF and wrap at its size.
        virtual uint8_t *iopRam() = 0;
        virtual uint32_t iopRamBytes() = 0;
        // Calls function(a0, a1) with gp as an interrupt handler, delay IOP cycles from the next point the IOP takes
        // queued interrupt calls.
        virtual void interruptCall(uint32_t function, uint32_t a0, uint32_t a1, uint32_t gp, uint32_t delay) = 0;
    };

    class Libsd
    {
    public:
        // Sets the mixer's input-half callback (each half played: the channel's transfer end).
        Libsd(Mixer &mixer, LibsdHost &host);
        ~Libsd();
        Libsd(const Libsd &) = delete;
        Libsd &operator=(const Libsd &) = delete;

        // libsd's export numbers this class answers (4 sceSdInit ... 27 sceSdSetSpu2IntrHandler).
        static bool handles(uint32_t ordinal);
        // One export by its number: args are its arguments (a0-a3, then the stack's), gp the caller's; the result is
        // v0. False when the number is not one handles() takes.
        bool call(uint32_t ordinal, const uint32_t args[5], uint32_t gp, uint32_t &result);

        // The exports. call() and every one below that takes IOP memory or a channel bring the clocked mixer up to the
        // IOP's cycle first; the per-entry ones (setParam ... getCoreAttr, also a batch's entries) do not.
        void init();                                   // 4 sceSdInit
        void quit();                                   // 2 sceSdQuit
        void setParam(uint32_t entry, uint32_t value); // 5
        uint32_t getParam(uint32_t entry);             // 6
        void setSwitch(uint32_t entry, uint32_t value); // 7
        uint32_t getSwitch(uint32_t entry);             // 8
        void setAddr(uint32_t entry, uint32_t value);   // 9
        uint32_t getAddr(uint32_t entry);               // 10
        void setCoreAttr(uint32_t entry, uint32_t value); // 11
        uint32_t getCoreAttr(uint32_t entry);             // 12
        // 15, 16: batches of 8-byte entries {u16 function, u16 entry, u32 value} in IOP memory; results to returns.
        uint32_t procBatch(uint32_t batch, uint32_t returns, uint32_t count);
        uint32_t procBatchEx(uint32_t batch, uint32_t returns, uint32_t count, uint32_t voices);
        // 17: mode 0 (or 3) writes, 1 reads, 2 stops; done when it returns, the channel's end signalled after.
        uint32_t voiceTrans(uint32_t channel, uint32_t mode, uint32_t iop, uint32_t sound, uint32_t size);
        uint32_t voiceTransStatus(uint32_t channel);   // 19: 1 (done)
        // 18: the sound data input of core channel from the driver's buffer in IOP memory (mode 0 from its start, 3
        // from start, | 0x10 loops; 2 stops and returns where it was; 1 not done). Each half played signals the
        // channel's end.
        uint32_t blockTrans(uint32_t channel, uint32_t mode, uint32_t iop, uint32_t size, uint32_t start);
        // 20: where the input plays now, | 1 << 24 in the 2nd half; while a signalled half is unread, the start of the
        // half that followed it (each signalled half answered once, in turn).
        uint32_t blockTransStatus(uint32_t channel);
        uint32_t setTransCallback(uint32_t channel, uint32_t function, uint32_t gp);                       // 21
        uint32_t setTransIntrHandler(uint32_t channel, uint32_t function, uint32_t argument, uint32_t gp); // 26
        uint32_t setIrqCallback(uint32_t function);                                                        // 22
        uint32_t setEffectAttr(uint32_t core, uint32_t attr); // 23: attr {core, mode, depth L/R, delay, feedback}
        uint32_t getEffectAttr(uint32_t core, uint32_t attr); // 24
        uint32_t setEffectMode(uint32_t core, uint32_t attr);       // 32: mode, delay, feedback (not the depth)
        uint32_t setEffectModeParams(uint32_t core, uint32_t attr); // 33: delay, feedback, depth of the current mode
        uint32_t clearEffectWorkArea(uint32_t core, uint32_t channel, uint32_t mode); // 25, and 31 (Clean: waits for it)

        // The transfer-end delay (IOP cycles) after a transfer or an input half.
        static constexpr uint32_t kTransferDelay = 2000u;

    private:
        void sync();
        uint32_t batchOne(uint32_t function, uint32_t entry, uint32_t value);
        void transferDone(uint32_t channel);
        void inputHalf(uint32_t channel, uint32_t half);
        uint32_t blockPosition(uint32_t channel);

        uint32_t physical(uint32_t address);
        uint8_t r8(uint32_t address);
        uint16_t r16(uint32_t address);
        uint32_t r32(uint32_t address);
        void w8(uint32_t address, uint8_t value);
        void w16(uint32_t address, uint16_t value);
        void w32(uint32_t address, uint32_t value);

        Mixer &m_mixer;
        LibsdHost &m_host;
        struct Callback
        {
            uint32_t function = 0, argument = 0, gp = 0;
        };
        struct State
        {
            std::unordered_map<uint32_t, uint32_t> params, switches, addresses, coreAttributes; // the rest, as set
            uint32_t transferAddress[2] = {};
            Callback transCallback[2], transHandler[2]; // per DMA channel
            uint32_t irqCallback = 0;
            uint32_t blockBuffer[2] = {}, blockSize[2] = {}; // the block transfer playing as each core's input
            // The halves signalled and not yet read by sceSdBlockTransStatus (oldest first, at most two).
            uint32_t halves[2][2] = {}, halvesPending[2] = {};
            uint32_t effectEnd[2] = {0x1DFFFFu, 0x1FFFFFu}; // sceSdInit's effect end addresses
            struct Effect
            {
                uint32_t mode = 0, delay = 0, feedback = 0;
            } effect[2];
        } m_state;
    };
}
