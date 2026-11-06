// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// libsd at its API on the native mixer (bozo/audio/libsd.h; behaviour: docs/specs/libraries.md section 15.14).
#include "bozo/audio/libsd.h"

#include <vector>

namespace bozo::audio
{
    namespace
    {
        // Voice parameters.
        constexpr uint32_t kVolL = 0x0000u, kVolR = 0x0100u, kPitch = 0x0200u, kAdsr1 = 0x0300u, kAdsr2 = 0x0400u,
                           kEnvX = 0x0500u, kVolXL = 0x0600u, kVolXR = 0x0700u;
        // Core parameters (bit 7 set).
        constexpr uint32_t kMix = 0x0800u, kMVolL = 0x0980u, kMVolR = 0x0A80u, kEVolL = 0x0B80u, kEVolR = 0x0C80u,
                           kBVolL = 0x0F80u, kBVolR = 0x1080u;
        // Switches.
        constexpr uint32_t kKon = 0x1500u, kKoff = 0x1600u, kEndX = 0x1700u, kVMixL = 0x1800u, kVMixEL = 0x1900u,
                           kVMixR = 0x1A00u, kVMixER = 0x1B00u;
        // Addresses.
        constexpr uint32_t kEea = 0x1D00u, kTsa = 0x1E00u, kSsa = 0x2040u, kLsax = 0x2140u, kNax = 0x2240u;
        // Core attributes.
        constexpr uint32_t kEffectEnable = 0x2u;

        int coreOf(uint32_t entry)
        {
            return static_cast<int>(entry & 1u);
        }
        int voiceOf(uint32_t entry)
        {
            return static_cast<int>((entry >> 1u) & 0x1Fu) % Mixer::kVoices;
        }
    }

    Libsd::Libsd(Mixer &mixer, LibsdHost &host) : m_mixer(mixer), m_host(host)
    {
        m_mixer.setInputHalfCallback([this](int group, int half)
                                     { inputHalf(static_cast<uint32_t>(group), static_cast<uint32_t>(half)); });
    }

    Libsd::~Libsd()
    {
        m_mixer.setInputHalfCallback({});
    }

    uint32_t Libsd::physical(uint32_t address)
    {
        return address & 0x1FFFFFFFu;
    }
    uint8_t Libsd::r8(uint32_t address)
    {
        const uint32_t p = physical(address);
        return p < m_host.iopRamBytes() ? m_host.iopRam()[p] : 0u;
    }
    uint16_t Libsd::r16(uint32_t address)
    {
        return static_cast<uint16_t>(r8(address) | (r8(address + 1u) << 8u));
    }
    uint32_t Libsd::r32(uint32_t address)
    {
        return static_cast<uint32_t>(r16(address)) | (static_cast<uint32_t>(r16(address + 2u)) << 16u);
    }
    void Libsd::w8(uint32_t address, uint8_t value)
    {
        const uint32_t p = physical(address);
        if (p < m_host.iopRamBytes())
            m_host.iopRam()[p] = value;
    }
    void Libsd::w16(uint32_t address, uint16_t value)
    {
        w8(address, static_cast<uint8_t>(value));
        w8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }
    void Libsd::w32(uint32_t address, uint32_t value)
    {
        w16(address, static_cast<uint16_t>(value));
        w16(address + 2u, static_cast<uint16_t>(value >> 16u));
    }

    // Clocked: the mix brought up to the IOP's cycle before the call acts on it or reads it back.
    void Libsd::sync()
    {
        if (m_mixer.clocked())
            m_mixer.advanceTo(m_host.iopCycle());
    }

    void Libsd::setParam(uint32_t entry, uint32_t value)
    {
        const int core = coreOf(entry), voice = voiceOf(entry);
        const uint16_t v = static_cast<uint16_t>(value);
        if ((entry & 0x80u) == 0u)
        {
            switch (entry & 0xFF00u)
            {
            case kVolL: m_mixer.setVolume(core, voice, 0, v); return;
            case kVolR: m_mixer.setVolume(core, voice, 1, v); return;
            case kPitch: m_mixer.setPitch(core, voice, v); return;
            case kAdsr1: m_mixer.setAdsr(core, voice, 0, v); return;
            case kAdsr2: m_mixer.setAdsr(core, voice, 1, v); return;
            default: break;
            }
        }
        switch (entry & 0xFFFEu)
        {
        case kMVolL: m_mixer.setMasterVolume(core, 0, v); return;
        case kMVolR: m_mixer.setMasterVolume(core, 1, v); return;
        case kEVolL: m_mixer.setEffectVolume(core, 0, v); return;
        case kEVolR: m_mixer.setEffectVolume(core, 1, v); return;
        case kMix: m_mixer.setMix(core, v); return;
        // The sound data input's volume is BVOL; AVOL (the external input's: core 0's output into core 1) is kept as a
        // plain parameter.
        case kBVolL: m_mixer.setInputVolume(core, 0, v); return;
        case kBVolR: m_mixer.setInputVolume(core, 1, v); return;
        default: m_state.params[entry] = value; return;
        }
    }

    uint32_t Libsd::getParam(uint32_t entry)
    {
        const int core = coreOf(entry), voice = voiceOf(entry);
        if ((entry & 0x80u) == 0u)
        {
            switch (entry & 0xFF00u)
            {
            case kVolL: return m_mixer.volume(core, voice, 0);
            case kVolR: return m_mixer.volume(core, voice, 1);
            case kPitch: return m_mixer.pitch(core, voice);
            case kAdsr1: return m_mixer.adsr(core, voice, 0);
            case kAdsr2: return m_mixer.adsr(core, voice, 1);
            case kEnvX: return m_mixer.envelope(core, voice);
            case kVolXL: return m_mixer.volume(core, voice, 0) & 0x7FFFu;
            case kVolXR: return m_mixer.volume(core, voice, 1) & 0x7FFFu;
            default: break;
            }
        }
        switch (entry & 0xFFFEu)
        {
        case kMVolL: return m_mixer.masterVolume(core, 0);
        case kMVolR: return m_mixer.masterVolume(core, 1);
        case kEVolL: return m_mixer.effectVolume(core, 0);
        case kEVolR: return m_mixer.effectVolume(core, 1);
        case kMix: return m_mixer.mix(core);
        case kBVolL: return m_mixer.inputVolume(core, 0);
        case kBVolR: return m_mixer.inputVolume(core, 1);
        default:
        {
            const auto it = m_state.params.find(entry);
            return it == m_state.params.end() ? 0u : it->second;
        }
        }
    }

    void Libsd::setSwitch(uint32_t entry, uint32_t value)
    {
        const int core = coreOf(entry);
        switch (entry & 0xFF00u)
        {
        case kKon: m_mixer.keyOn(core, value & 0xFFFFFFu); return;
        case kKoff: m_mixer.keyOff(core, value & 0xFFFFFFu); return;
        case kEndX: m_mixer.clearEnded(core, 0xFFFFFFu); return;
        case kVMixL: m_mixer.setDryMask(core, 0, value); return;
        case kVMixR: m_mixer.setDryMask(core, 1, value); return;
        case kVMixEL: m_mixer.setWetMask(core, 0, value); return;
        case kVMixER: m_mixer.setWetMask(core, 1, value); return;
        default: m_state.switches[entry & 0xFF01u] = value; return;
        }
    }

    uint32_t Libsd::getSwitch(uint32_t entry)
    {
        const int core = coreOf(entry);
        switch (entry & 0xFF00u)
        {
        case kEndX: return m_mixer.ended(core);
        case kVMixL: return m_mixer.dryMask(core, 0);
        case kVMixR: return m_mixer.dryMask(core, 1);
        case kVMixEL: return m_mixer.wetMask(core, 0);
        case kVMixER: return m_mixer.wetMask(core, 1);
        default:
        {
            const auto it = m_state.switches.find(entry & 0xFF01u);
            return it == m_state.switches.end() ? 0u : it->second;
        }
        }
    }

    void Libsd::setAddr(uint32_t entry, uint32_t value)
    {
        const int core = coreOf(entry), voice = voiceOf(entry);
        switch (entry & 0xFFC0u)
        {
        case kSsa: m_mixer.setStart(core, voice, value); return;
        case kLsax: m_mixer.setLoop(core, voice, value); return;
        default: break;
        }
        if ((entry & 0xFF00u) == kTsa)
            m_state.transferAddress[core] = value;
        if ((entry & 0xFF00u) == kEea)
            m_state.effectEnd[core] = value | 0x1FFFFu;
        m_state.addresses[entry & 0xFF01u] = value;
    }

    uint32_t Libsd::getAddr(uint32_t entry)
    {
        const int core = coreOf(entry), voice = voiceOf(entry);
        switch (entry & 0xFFC0u)
        {
        case kSsa: return m_mixer.start(core, voice);
        case kLsax: return m_mixer.loop(core, voice);
        case kNax: return m_mixer.position(core, voice);
        default: break;
        }
        if ((entry & 0xFF00u) == kEea)
            return m_state.effectEnd[core];
        const auto it = m_state.addresses.find(entry & 0xFF01u);
        return it == m_state.addresses.end() ? 0u : it->second;
    }

    void Libsd::setCoreAttr(uint32_t entry, uint32_t value)
    {
        if ((entry & ~1u) == kEffectEnable)
            m_mixer.setEffectEnable(coreOf(entry), value != 0u);
        m_state.coreAttributes[entry] = value;
    }

    uint32_t Libsd::getCoreAttr(uint32_t entry)
    {
        const auto it = m_state.coreAttributes.find(entry);
        return it == m_state.coreAttributes.end() ? 0u : it->second;
    }

    void Libsd::init()
    {
        sync();
        m_mixer.reset();
        m_state = State{};
    }

    void Libsd::quit()
    {
        sync();
    }

    uint32_t Libsd::batchOne(uint32_t function, uint32_t entry, uint32_t value)
    {
        switch (function)
        {
        case 0x01: setParam(entry, value); return 0u;
        case 0x02: setSwitch(entry, value); return 0u;
        case 0x03: setAddr(entry, value); return 0u;
        case 0x04: setCoreAttr(entry, value); return 0u;
        case 0x05: w32(entry, value); return 0u; // a write to IOP memory
        case 0x10: return getParam(entry);
        case 0x12: return getSwitch(entry);
        case 0x13: return getAddr(entry);
        case 0x14: return getCoreAttr(entry);
        default: return 0u;
        }
    }

    uint32_t Libsd::procBatch(uint32_t batch, uint32_t returns, uint32_t count)
    {
        sync();
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t at = batch + i * 8u;
            const uint32_t result = batchOne(r16(at), r16(at + 2u), r32(at + 4u));
            if (returns != 0u)
                w32(returns + i * 4u, result);
        }
        return count;
    }

    uint32_t Libsd::procBatchEx(uint32_t batch, uint32_t returns, uint32_t count, uint32_t voices)
    {
        sync();
        uint32_t written = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t at = batch + i * 8u;
            const uint32_t function = r16(at), entry = r16(at + 2u), value = r32(at + 4u);
            for (uint32_t voice = 0; voice < 24u; ++voice)
            {
                if ((voices & (1u << voice)) == 0u)
                    continue;
                const uint32_t result = batchOne(function, (entry & ~0x3Eu) | (voice << 1u), value);
                if (returns != 0u)
                    w32(returns + written * 4u, result);
                ++written;
            }
        }
        return written;
    }

    // A transfer's end, as libsd's DMA interrupt reports it: the channel's handler (handler(channel, its argument)),
    // else its callback (callback(0)), shortly after, as an interrupt handler.
    void Libsd::transferDone(uint32_t channel)
    {
        const Callback &handler = m_state.transHandler[channel], &callback = m_state.transCallback[channel];
        if (handler.function != 0u)
            m_host.interruptCall(handler.function, channel, handler.argument, handler.gp, kTransferDelay);
        else if (callback.function != 0u)
            m_host.interruptCall(callback.function, 0u, 0u, callback.gp, kTransferDelay);
    }

    // Each half is kept until a status read answers it: a driver reading the status after its handler, once the
    // input has moved on, still learns which half to refill.
    void Libsd::inputHalf(uint32_t channel, uint32_t half)
    {
        uint32_t &pending = m_state.halvesPending[channel];
        if (pending == 2u)
        {
            m_state.halves[channel][0] = m_state.halves[channel][1];
            pending = 1u;
        }
        m_state.halves[channel][pending++] = half;
        transferDone(channel);
    }

    uint32_t Libsd::voiceTrans(uint32_t channel, uint32_t mode, uint32_t iop, uint32_t sound, uint32_t size)
    {
        sync();
        mode &= 3u;
        if (mode == 0u || mode == 3u)
        {
            std::vector<uint8_t> data(size);
            for (uint32_t i = 0; i < size; ++i)
                data[i] = r8(iop + i);
            m_mixer.write(sound, data.data(), size);
        }
        else if (mode == 1u)
        {
            std::vector<uint8_t> data(size);
            m_mixer.read(sound, data.data(), size);
            for (uint32_t i = 0; i < size; ++i)
                w8(iop + i, data[i]);
        }
        if (mode != 2u)
            transferDone(channel & 1u);
        return mode == 2u ? 0u : size;
    }

    uint32_t Libsd::voiceTransStatus(uint32_t)
    {
        sync();
        return 1u; // done
    }

    uint32_t Libsd::blockPosition(uint32_t channel)
    {
        return m_state.blockBuffer[channel] + m_mixer.inputOffset(static_cast<int>(channel));
    }

    uint32_t Libsd::blockTrans(uint32_t channel, uint32_t mode, uint32_t iop, uint32_t size, uint32_t start)
    {
        sync();
        channel &= 1u;
        switch (mode & 3u)
        {
        case 2u:
        {
            const uint32_t position = blockPosition(channel);
            m_mixer.stopInput(static_cast<int>(channel));
            m_state.halvesPending[channel] = 0u;
            return position;
        }
        case 1u:
            return 0u;
        default:
        {
            const uint32_t at = iop & 0x1FFFFFu;
            if (at + size > m_host.iopRamBytes() || size < 1024u)
                return 0xFFFFFFFFu;
            const uint32_t offset = (mode & 3u) == 3u && start >= iop ? start - iop : 0u;
            m_state.blockBuffer[channel] = iop;
            m_state.blockSize[channel] = size;
            m_state.halvesPending[channel] = 0u;
            m_mixer.startInput(static_cast<int>(channel), m_host.iopRam() + at, size, offset, (mode & 0x10u) != 0u);
            return 0u;
        }
        }
    }

    uint32_t Libsd::blockTransStatus(uint32_t channel)
    {
        sync();
        channel &= 1u;
        if (m_state.halvesPending[channel] != 0u)
        {
            const uint32_t half = m_state.halves[channel][0];
            m_state.halves[channel][0] = m_state.halves[channel][1];
            --m_state.halvesPending[channel];
            const uint32_t second = half == 0u ? 1u : 0u;
            return (second << 24u) | (m_state.blockBuffer[channel] + second * (m_state.blockSize[channel] / 2u));
        }
        if (!m_mixer.inputPlaying(static_cast<int>(channel)))
            return 0u;
        const uint32_t offset = m_mixer.inputOffset(static_cast<int>(channel));
        const uint32_t second = offset >= m_state.blockSize[channel] / 2u ? 1u : 0u;
        return (second << 24u) | (m_state.blockBuffer[channel] + offset);
    }

    uint32_t Libsd::setTransCallback(uint32_t channel, uint32_t function, uint32_t gp)
    {
        sync();
        channel &= 1u;
        const uint32_t previous = m_state.transCallback[channel].function;
        m_state.transCallback[channel] = Callback{function, 0u, gp};
        return previous;
    }

    uint32_t Libsd::setTransIntrHandler(uint32_t channel, uint32_t function, uint32_t argument, uint32_t gp)
    {
        sync();
        channel &= 1u;
        const uint32_t previous = m_state.transHandler[channel].function;
        m_state.transHandler[channel] = Callback{function, argument, gp};
        return previous;
    }

    uint32_t Libsd::setIrqCallback(uint32_t function)
    {
        sync();
        const uint32_t previous = m_state.irqCallback;
        m_state.irqCallback = function;
        return previous;
    }

    uint32_t Libsd::setEffectAttr(uint32_t core, uint32_t attr)
    {
        sync();
        core &= 1u;
        const uint32_t mode = r32(attr + 4u);
        if ((mode & 0xFFu) >= 10u)
            return 0xFFFFFFFFu;
        auto &effect = m_state.effect[core];
        effect.mode = mode & 0xFFu;
        effect.delay = r32(attr + 0xCu);
        effect.feedback = r32(attr + 0x10u);
        if ((mode & 0x100u) != 0u)
            m_mixer.clearEffectArea(static_cast<int>(effect.mode), m_state.effectEnd[core]);
        const int group = static_cast<int>(core);
        m_mixer.setEffect(group, static_cast<int>(effect.mode), effect.delay, effect.feedback, m_state.effectEnd[core]);
        m_mixer.setEffectVolume(group, 0, r16(attr + 8u));
        m_mixer.setEffectVolume(group, 1, r16(attr + 0xAu));
        return 0u;
    }

    uint32_t Libsd::setEffectMode(uint32_t core, uint32_t attr)
    {
        sync();
        core &= 1u;
        const uint32_t mode = r32(attr + 4u);
        if ((mode & 0xFFu) >= 10u)
            return 0xFFFFFFFFu;
        auto &effect = m_state.effect[core];
        effect.mode = mode & 0xFFu;
        effect.delay = r32(attr + 0xCu);
        effect.feedback = r32(attr + 0x10u);
        if ((mode & 0x100u) != 0u)
            m_mixer.clearEffectArea(static_cast<int>(effect.mode), m_state.effectEnd[core]);
        m_mixer.setEffect(static_cast<int>(core), static_cast<int>(effect.mode), effect.delay, effect.feedback,
                          m_state.effectEnd[core]);
        return 0u;
    }

    uint32_t Libsd::setEffectModeParams(uint32_t core, uint32_t attr)
    {
        sync();
        core &= 1u;
        auto &effect = m_state.effect[core];
        effect.delay = r32(attr + 0xCu);
        effect.feedback = r32(attr + 0x10u);
        const int group = static_cast<int>(core);
        m_mixer.setEffect(group, static_cast<int>(effect.mode), effect.delay, effect.feedback, m_state.effectEnd[core]);
        m_mixer.setEffectVolume(group, 0, r16(attr + 8u));
        m_mixer.setEffectVolume(group, 1, r16(attr + 0xAu));
        return 0u;
    }

    uint32_t Libsd::getEffectAttr(uint32_t core, uint32_t attr)
    {
        sync();
        core &= 1u;
        const auto &effect = m_state.effect[core];
        w32(attr + 4u, effect.mode);
        w16(attr + 8u, m_mixer.effectVolume(static_cast<int>(core), 0));
        w16(attr + 0xAu, m_mixer.effectVolume(static_cast<int>(core), 1));
        w32(attr + 0xCu, effect.delay);
        w32(attr + 0x10u, effect.feedback);
        return 0u;
    }

    uint32_t Libsd::clearEffectWorkArea(uint32_t core, uint32_t, uint32_t mode)
    {
        sync();
        mode &= 0xFFu;
        if (mode < 10u)
            m_mixer.clearEffectArea(static_cast<int>(mode), m_state.effectEnd[core & 1u]);
        return 0u;
    }

    bool Libsd::handles(uint32_t ordinal)
    {
        return ordinal == 2u || (ordinal >= 4u && ordinal <= 12u) || (ordinal >= 15u && ordinal <= 27u) || (ordinal >= 31u && ordinal <= 33u);
    }

    bool Libsd::call(uint32_t ordinal, const uint32_t a[5], uint32_t gp, uint32_t &result)
    {
        result = 0u;
        switch (ordinal)
        {
        case 2u: quit(); return true;
        case 4u: init(); return true;
        case 5u: sync(); setParam(a[0] & 0xFFFFu, a[1]); return true;
        case 6u: sync(); result = getParam(a[0] & 0xFFFFu); return true;
        case 7u: sync(); setSwitch(a[0] & 0xFFFFu, a[1]); return true;
        case 8u: sync(); result = getSwitch(a[0] & 0xFFFFu); return true;
        case 9u: sync(); setAddr(a[0] & 0xFFFFu, a[1]); return true;
        case 10u: sync(); result = getAddr(a[0] & 0xFFFFu); return true;
        case 11u: sync(); setCoreAttr(a[0] & 0xFFFFu, a[1]); return true;
        case 12u: sync(); result = getCoreAttr(a[0] & 0xFFFFu); return true;
        case 15u: result = procBatch(a[0], a[1], a[2]); return true;
        case 16u: result = procBatchEx(a[0], a[1], a[2], a[3]); return true;
        case 17u: result = voiceTrans(a[0], a[1], a[2], a[3], a[4]); return true;
        case 18u: result = blockTrans(a[0], a[1], a[2], a[3], a[4]); return true;
        case 19u: result = voiceTransStatus(a[0]); return true;
        case 20u: result = blockTransStatus(a[0]); return true;
        case 21u: result = setTransCallback(a[0], a[1], gp); return true;
        case 22u: result = setIrqCallback(a[0]); return true;
        case 23u: result = setEffectAttr(a[0], a[1]); return true;
        case 24u: result = getEffectAttr(a[0], a[1]); return true;
        case 25u: result = clearEffectWorkArea(a[0], a[1], a[2]); return true;
        case 31u: result = clearEffectWorkArea(a[0], a[1], a[2]); return true;
        case 32u: result = setEffectMode(a[0], a[1]); return true;
        case 33u: result = setEffectModeParams(a[0], a[1]); return true;
        case 26u: result = setTransIntrHandler(a[0], a[1], a[2], gp); return true;
        case 27u: sync(); return true; // sceSdSetSpu2IntrHandler
        default: return false;
        }
    }
}
