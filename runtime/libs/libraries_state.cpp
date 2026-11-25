// SPDX-License-Identifier: MIT
// Save states of the native libraries (bozo/state.h): every library's state, field by field. What a port or the
// host sets up once (callbacks, paths, switches) is not saved: a loading runtime was set up the same way.
#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"

namespace bozo::libs
{
    using state::fields;
    using state::io;
    using state::pod;

    void Heap::serialize(state::Archive &a) { fields(a, free_, used_, end_); }
    void IopHeap::serialize(state::Archive &a) { fields(a, free_, used_); }

    void io(state::Archive &a, McDate &d) { fields(a, d.second, d.minute, d.hour, d.day, d.month, d.year); }
    void io(state::Archive &a, McCard::Entry &e) { fields(a, e.directory, e.attributes, e.created, e.modified, e.data, e.order); }
    void McCard::serialize(state::Archive &a) { fields(a, entries_, next_); }

    void io(state::Archive &a, CdState::File &f) { fields(a, f.sector, f.bytes, f.position); }
    void io(state::Archive &a, McState::Open &o) { fields(a, o.port, o.path, o.position, o.mode); }
    void io(state::Archive &a, MpegState::Callback &c) { fields(a, c.function, c.data, c.gp, c.set); }
    void io(state::Archive &a, MpegState::DueCallback &c) { fields(a, c.handler, c.entry); }
    void io(state::Archive &a, TimerState::Counter &c)
    {
        fields(a, c.tag, c.running, c.armed, c.start, c.count, c.compare, c.handler, c.arg, c.gp, c.alarm, c.hostDue);
    }
    void io(state::Archive &a, Resume &r) { fields(a, r.kind, r.a); }
    void io(state::Archive &a, PadState::Pad2Socket &s) { fields(a, s.port, s.slot); }

    void io(state::Archive &a, SndTone &t) { pod(a, t); }
    void io(state::Archive &a, SndSound &s) { pod(a, s); }
    void io(state::Archive &a, SndBank &b)
    {
        fields(a, b.address, b.block, b.midiBlock, b.lbn, b.id, b.sounds, b.programs, b.samples, b.midi, b.header);
    }
    void io(state::Archive &a, SndDriver::Handle &h)
    {
        fields(a, h.handle, h.bank, h.sound, h.type, h.stopped, h.ended, h.start, h.seconds);
    }

    void SndDriver::serialize(state::Archive &a, Libraries &l)
    {
        a.begin("snd");
        fields(a, banks, handles, last, masterVolume, masterSet, playbackMode, movieBuffer, streamBlock, unseen_, recvFor_,
               toEngine_, recentPlays_, engineValid_, engineGame_, movieAlloc_, freeTiming);
        pod(a, stats);
        pod(a, timing);
        uint64_t n = state::count(a, slots_.size());
        if (!a.loading())
            for (auto &[pool, slots] : slots_)
            {
                uint32_t key = pool;
                io(a, key);
                state::count(a, slots.size());
                for (Slot &slot : slots)
                    fields(a, slot.generation, slot.occupant);
            }
        else
        {
            slots_.clear();
            for (uint64_t i = 0; i < n; ++i)
            {
                uint32_t key = 0;
                io(a, key);
                std::vector<Slot> slots(state::count(a, 0));
                for (Slot &slot : slots)
                    fields(a, slot.generation, slot.occupant);
                slots_[key] = std::move(slots);
            }
            clockAnchored_ = false; // the host's clock: anchored again at the next sync
        }
        bool made = engine != nullptr;
        io(a, made);
        if (a.loading())
        {
            if (made)
                makeEngine(l);
            else
                engine.reset();
        }
        if (made)
            engine->serialize(a);
        a.end();
    }

    std::string Libraries::unsaveable() const
    {
        for (const auto &[thread, p] : continuations_)
            if (p.resume.kind == kResumeNone)
                return "thread " + std::to_string(thread) + " waits with a library continuation that cannot be saved";
        for (const auto &[sequence, p] : handlerEnds_)
            if (p.resume.kind == kResumeNone)
                return "a queued handler has a library completion that cannot be saved";
        if (mpeg.decoder || !mpeg.decoded.empty())
            return "a movie is playing";
        return {};
    }

    void Libraries::serialize(state::Archive &a)
    {
        if (const std::string why = unsaveable(); !why.empty())
            throw state::Error("save state: " + why);
        a.begin("libraries");
        heap.serialize(a);
        fields(a, rand.state, framesEnded, dmaEnvironment);
        // A state written before ResetGraph's video parameters were kept here (5b830b6) goes on with its waits: they
        // stay unknown (zero), and the GS library reads them from the scratchpad as it did then.
        if (a.loading() && a.nextIs("waits"))
            gsVideo = {};
        else
            fields(a, gsVideo);
        a.begin("waits");
        uint64_t n = state::count(a, continuations_.size());
        if (!a.loading())
            for (auto &[thread, p] : continuations_)
            {
                int32_t t = thread;
                fields(a, t, p.resume);
            }
        else
        {
            continuations_.clear();
            for (uint64_t i = 0; i < n; ++i)
            {
                int32_t t = 0;
                Resume r;
                fields(a, t, r);
                continuations_[t] = pending(r);
            }
        }
        n = state::count(a, handlerEnds_.size());
        if (!a.loading())
            for (auto &[sequence, p] : handlerEnds_)
            {
                uint64_t s = sequence;
                fields(a, s, p.resume);
            }
        else
        {
            handlerEnds_.clear();
            for (uint64_t i = 0; i < n; ++i)
            {
                uint64_t s = 0;
                Resume r;
                fields(a, s, r);
                handlerEnds_[s] = pending(r);
            }
        }
        a.end();

        a.begin("pad");
        for (auto &port : pad.ports)
            for (auto &slot : port)
                pod(a, slot);
        fields(a, pad.initialized, pad.reads, pad.socketReads, pad.multitap, pad.multitapAttached);
        // A state written before libpad2's sockets were saved (67c5867) ends here: no socket is open.
        if (a.loading() && a.sectionLeft() == 0)
        {
            pad.pad2Sockets.clear();
            pad.pad2Next = 0;
        }
        else
            fields(a, pad.pad2Sockets, pad.pad2Next);
        a.end();

        a.begin("mc");
        for (McCard &c : mc.cards)
            c.serialize(a);
        fields(a, mc.formatted, mc.slotMask, mc.date, mc.known, mc.unformatted, mc.pending, mc.function, mc.result,
               mc.files, mc.nextDescriptor, mc.currentDirectory, mc.directoryCursor);
        a.end();
        // Cards kept in a host folder (a played game's) are outside the console, as a real memory card is: loading a
        // state reads them again from their folders and keeps them bound there. Taken from the state, the cards had no
        // folder (it is not saved), so every save made after loading a state stayed in memory and was lost at exit.
        // Without a host folder (tests, scripted runs) the state's cards are kept as saved.
        if (a.loading() && mc.formatted && !mc.hostDirectory.empty())
            for (size_t p = 0; p < mc.cards.size(); ++p)
                mc.cards[p].format(mc.hostDirectory + "/mc" + std::to_string(p), mc.date);

        a.begin("cd");
        fields(a, cd.files, cd.nextFile, cd.initialized, cd.error, cd.readEnd, cd.readBytes, cd.callback, cd.callbackGp,
               cd.callbackStack, cd.callbackConfigured, cd.ringSectors, cd.banks, cd.iopBuffer, cd.streaming,
               cd.streamLbn, cd.delivered, cd.produced, cd.producedAt, cd.paused, cd.pendingSectors, cd.pendingBuffer,
               cd.pendingError);
        a.end();

        a.begin("vu0");
        fields(a, vu0.image, vu0.hierarchyHigh);
        a.end();

        a.begin("io");
        pod(a, io);
        a.end();

        a.begin("mpeg");
        MpegState &m = mpeg;
        fields(a, m.active, m.mp, m.work, m.size, m.block, m.callbacks, m.strCallbacks, m.callbacksAdded, m.consumed,
               m.pending, m.pendingAt, m.ring, m.ringSize, m.ringBias, m.linearBase, m.programEnd, m.aborted, m.es,
               m.scanned, m.starts, m.pictureStarts, m.sequenceEnd, m.headerOpen, m.cuts, m.sent, m.flushed,
               m.callbacksDue, m.waiting, m.delivered, m.base, m.lastShown, m.firstPictureNs, m.soundHeader,
               m.soundCarry);
        // A state written before the SDK's Nodata end (MpegOptions::sdkNodata) has the sound left: the input goes on.
        if (a.loading() && a.sectionLeft() == sizeof m.sound)
            m.inputEnded = false;
        else
            fields(a, m.inputEnded);
        pod(a, m.sound);
        a.end();

        a.begin("sif");
        fields(a, sif.rpcId, sif.bindAttempts, sif.bound, sif.sreg, sif.dmaId, sif.modules);
        {
            // A state written before the IOP side's software registers has the heap next: read it as such when the
            // registers then leave bytes over (the section's end checks the layout), with the registers cleared.
            const state::Archive::Mark mark = a.mark();
            bool older = false;
            if (a.loading())
                try
                {
                    sif.heap.serialize(a);
                    a.end();
                    older = true;
                }
                catch (const state::Error &)
                {
                    a.rewind(mark);
                }
            if (older)
                std::fill(std::begin(sif.iopSreg), std::end(sif.iopSreg), 0u);
            else
            {
                fields(a, sif.iopSreg);
                sif.heap.serialize(a);
                a.end();
            }
        }

        // A state written before the 989SND driver was saved goes on with the timers: the driver stays as it is.
        if (!(a.loading() && a.nextIs("timers")))
            snd.serialize(a, *this);

        a.begin("timers");
        TimerState &t = timers;
        fields(a, t.counters, t.alarms, t.used, t.serial, t.initialized, t.prescale, t.systemRunning, t.systemStart,
               t.systemStopped, t.inHandler, t.delays);
        a.end();
        a.end();
        if (a.loading())
            portStream.clear(); // sound already mixed for the host belongs to the old state
    }
}
