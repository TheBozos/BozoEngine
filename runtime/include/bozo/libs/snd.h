// The 989SND sound driver as the native runtime answers it (libraries.md section 15): the IOP side of the RPC
// service 0x123456, with no IOP. It decodes the game's requests (the EE client's command batches and direct calls),
// keeps the driver's state (banks in IOP memory, sounds and their handles, volumes, reverb, streams, the movie's
// buffer) and writes the replies the reference's driver wrote. What should be playing goes to a SoundSink (the future
// mixer: phase 4).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bozo/libs/snd989.h"

namespace bozo::libs
{
    class Libraries;
    class Guest;

    // A tone (one sample on one voice) of a program, as the bank holds it (24 bytes).
    struct SndTone
    {
        uint8_t priority = 0, volume = 0, centerNote = 0, centerFine = 0;
        int16_t pan = 0;
        uint8_t mapLow = 0, mapHigh = 0;
        uint8_t bendDown = 0, bendUp = 0; // the pitch bend's range, semitones
        uint16_t adsr1 = 0, adsr2 = 0, flags = 0;
        uint32_t vag = 0;        // the sample's offset in the bank's sample data (SPU ADPCM)
        uint32_t vagBlocks = 0;  // its 16-byte ADPCM blocks, to the one flagged as its end
        bool loops = false;      // its end block jumps back to its loop start (flags 3)
    };

    // A sound of a bank (28 bytes): its type decides how it plays and how long it lives.
    struct SndSound
    {
        int32_t type = 0;      // 0 none, 1 one-shot program, 2 sound effect (grains), 3 looping program, 4 MIDI
        uint8_t raw[28] = {};
        uint8_t program = 0, note = 0;
    };

    // A bank as 989SND keeps it: its header (and MIDI data) in IOP memory, its samples in SPU memory.
    struct SndBank
    {
        uint32_t address = 0; // the bank's IOP address (the handle the game plays it by)
        uint32_t block = 0;   // the IOP heap block of its header
        uint32_t midiBlock = 0; // and of its MIDI data (0: none)
        uint32_t lbn = 0;     // where it was loaded from on the disc
        std::string id;       // the bank's four-character id ("SHL1", "PREA"...)
        std::vector<SndSound> sounds;
        std::vector<std::vector<SndTone>> programs;
        std::vector<uint8_t> samples; // SPU ADPCM data, the tones' `vag` offsets into it
        std::vector<uint8_t> midi;    // the third chunk when there is one
        std::vector<uint8_t> header;  // the first chunk (SBv2) as in the file, for 989SND's own model
    };

    // What should be playing, for the mixer (libraries.md section 15.6). Times are EE cycles of the call.
    class SoundSink
    {
    public:
        virtual ~SoundSink() = default;
        virtual void bankLoaded(const SndBank &) {}
        virtual void bankUnloaded(uint32_t /*bank*/) {}
        // A sound starts: `handle` 0 when the game gets none (an asynchronous play).
        virtual void soundStarted(uint32_t /*handle*/, const SndBank &, uint32_t /*sound*/, int32_t /*volume*/,
                                  int32_t /*pan*/, int32_t /*pitchModifier*/, int32_t /*pitchBend*/, uint64_t /*cycle*/) {}
        // SetSoundParams' mask: 1 volume, 4 pan, 0x10 pitch modifier, 0x20 pitch bend (the raw words are passed).
        virtual void soundParams(uint32_t /*handle*/, uint32_t /*mask*/, int32_t /*volume*/, int32_t /*pan*/,
                                 int32_t /*pitchModifier*/, int32_t /*pitchBend*/) {}
        virtual void soundPitchModifier(uint32_t /*handle*/, int32_t /*modifier*/) {}
        virtual void soundStopped(uint32_t /*handle*/) {}
        virtual void masterVolume(uint32_t /*group*/, int32_t /*volume*/) {}
        virtual void reverb(uint32_t /*core*/, int32_t /*type*/, int32_t /*left*/, int32_t /*right*/) {}
        // A streamed VAG (two mono VAGp files on the disc, left and right): started, paused, stopped.
        virtual void streamStarted(uint32_t /*handle*/, uint32_t /*lbnLeft*/, uint32_t /*lbnRight*/, uint32_t /*flags*/,
                                   int32_t /*volume*/, uint64_t /*cycle*/) {}
        virtual void streamPaused(uint32_t /*handle*/) {}
        virtual void streamStopped(uint32_t /*handle*/) {}
    };

    // Counts, per command (direct call function or batch command id), of the replies the driver made itself and those a
    // pinned run took from the log because the reference's IOP time decided them (libraries.md section 15.5).
    struct SndStats
    {
        uint64_t native[0x50] = {}, pinned[0x50] = {};
        uint64_t followed = 0;     // commands followed from calls answered from the log (port overrides)
        uint64_t lost = 0;         // commands of those calls whose request was overwritten before the call ended
        uint64_t lostInferred = 0; // lost calls the model took as a StopSound (snd.cpp followTraced)
        uint64_t estimateWrong = 0; // pinned liveness decisions the native lifetime estimate gets wrong (free mode)
        uint64_t followMismatch = 0; // followed allocations not where the native IOP heap puts them
        uint64_t modelAgrees = 0, modelDiffers = 0; // pinned handles the native handler model gives (or not)
        // The 989SND model (snd989.h) against the log's replies, per command, where the log has one.
        uint64_t engineAgrees[0x50] = {}, engineDiffers[0x50] = {};
        uint64_t firstDifferCycle = 0;
        uint32_t firstDifferId = 0, firstDifferLog = 0, firstDifferEngine = 0, firstDifferArg = 0;
    };

    class SndDriver
    {
    public:
        // The reference's reply word at an EE address (the log's call), when a pinned run has one; null otherwise.
        using LogWord = std::function<bool(uint32_t address, uint32_t &value)>;

        // One RPC to the service: function `fno`, the request (`request`, `bytes`), the reply to `recv` (`rsize`
        // bytes). `reply` receives the reply's words. `log`: the reference's reply (pinned run), or null. `follow`:
        // the call was answered from the log; its reply is in guest memory already and only the state follows it.
        void call(Libraries &l, Guest &g, uint32_t fno, const std::vector<uint32_t> &request, uint32_t recv, uint32_t rsize,
                  std::vector<uint32_t> &reply, const LogWord *log, bool follow);
        // A call answered from the log made the given RPC calls (traced inside it): the state follows them, reading
        // the requests and replies left in guest memory.
        void followTraced(Libraries &l, Guest &g, const std::vector<std::array<uint32_t, 4>> &calls);
        // The receive buffer a send buffer's calls use (learned from the calls seen).
        void noteBuffers(uint32_t send, uint32_t recv) { recvFor_[send] = recv; }

        SoundSink *sink = nullptr;
        SndStats stats;
        // Which of the replies the reference's IOP time decides (0x11, 0x19, 0x21, 0x2c) a pinned run takes from the
        // log (bit 0 0x11, 1 0x19, 2 0x21, 3 0x2c: `--snd-log-replies`); the others come from the 989SND model.
        uint32_t fromLog = 0;
        std::unique_ptr<snd989::Engine> engine; // 989SND's behaviour on the native mixer (made at the first call)
        // The IOP's time the engine runs on (snd989.h): the pinned points by default; a free run sets freeRun's, the
        // IOP reset then taken where the game reboots the IOP (iopReset).
        snd989::Timing timing;
        bool freeTiming = false;

        // sceSifRebootIop at EE cycle `ee`: free mode's IOP reset (P counts from it).
        void iopReset(uint64_t ee);
        // The engine (made if needed) brought up to EE cycle `ee`: its mixer's output so far (snd989::Engine::advanceTo).
        void advanceAudio(Libraries &l, uint64_t ee);
        // sceSifSetDma's data for the IOP's RAM (the movie's PCM): to the engine's RAM at EE cycle `ee`.
        void iopWrite(Libraries &l, uint64_t ee, uint32_t address, const uint8_t *data, uint32_t bytes);
        // The engine's mixer (null before the engine is made).
        audio::Mixer *mixer() { return engine ? &engine->mixer() : nullptr; }

        // ---- State (libraries.md section 15.3) ----
        struct Handle
        {
            uint32_t handle = 0;
            uint32_t bank = 0, sound = 0;
            int32_t type = 0;        // the sound's type; -1 a stream
            bool stopped = false, ended = false;
            uint64_t start = 0;      // EE cycle it started
            double seconds = -1;     // its native lifetime estimate (-1: until stopped)
        };
        std::map<uint32_t, SndBank> banks; // by IOP address
        std::map<uint32_t, Handle> handles;
        uint32_t last = 0;                 // the driver's return word: what a command with no result leaves in its slot
        int32_t masterVolume[32] = {};
        bool masterSet[32] = {};
        uint32_t playbackMode = 0;
        uint32_t movieBuffer = 0, streamBlock = 0;

    private:
        uint32_t command(Libraries &l, Guest &g, uint32_t id, const uint32_t a[7], uint32_t slot, const LogWord *log,
                         bool follow, bool &returns);
        uint32_t liveness(Libraries &l, Guest &g, uint32_t id, uint32_t handle, uint32_t slot, const LogWord *log, bool follow);
        uint32_t loadBank(Libraries &l, uint32_t lbn, uint32_t slot, const LogWord *log, bool follow, uint32_t id);
        void stopHandle(Handle &h);
        bool alive(Libraries &l, Guest &g, uint32_t handle) const;
        // The native model's next handle of a pool (0x80 sounds, 0x81 MIDI, 0x84 streams), and taking a slot.
        uint32_t modelHandle(Libraries &l, Guest &g, uint32_t pool);
        void occupy(uint32_t handle);
        struct Slot
        {
            uint32_t generation = 1, occupant = 0;
        };
        std::map<uint32_t, std::vector<Slot>> slots_;
        std::map<uint32_t, Handle> unseen_; // the native model's handles of asynchronous plays (the game never sees them)
        std::map<uint32_t, uint32_t> recvFor_;
        std::map<uint32_t, uint32_t> toEngine_;
        std::deque<std::pair<uint64_t, uint32_t>> recentPlays_; // (EE cycle, handle) of the latest plays // a handle the log gave -> the model's, where they differ
        bool engineValid_ = false;              // the model answered the current command
        uint32_t engineGame_ = 0;               // its answer, in the game's handles
        uint32_t movieAlloc_ = 0;               // InitMovieSound's buffer, allocated before the engine takes the command
        void makeEngine(Libraries &l);
        bool fromLogFor(uint32_t id) const
        {
            // GetTransStatus (0x40, the movie sound's position) always: the game's movie code paces itself on it, and
            // the reference's IOP time it follows is not modelled to the sample (libraries.md 15.13c).
            return (id == 0x11 && (fromLog & 1)) || (id == 0x19 && (fromLog & 2)) || (id == 0x21 && (fromLog & 4)) ||
                   (id == 0x2C && (fromLog & 8)) || id == 0x40;
        }
        void noteEngine(Libraries &l, Guest &g, uint32_t id, uint32_t logValue, uint32_t engineValue, uint32_t arg);
    };
}
