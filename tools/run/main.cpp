// bozo-run: TMB on BozoEngine's own EE kernel, dispatcher and cycle model, every library it does not implement yet
// answered from a call log (docs/specs/calllog.md), every decision checked against the log, and the per-frame
// reference record (PS2XREF1) written and compared with the reference build's. See tools/run/README.md.
#include "calllog.h"
#include "log_host.h"

#include "bozo/hash.h"

#include "tmb_functions.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef BOZO_RUN_ELF
#define BOZO_RUN_ELF ""
#endif

namespace
{
    int usage()
    {
        std::fprintf(stderr,
                     "usage: bozo-run <log.clg> [--ref <run.ref>] [--out <out.ref>] [--frames n] [--elf <elf>]\n"
                     "                [--max-errors n] [--trace-from record] [--trace-checkpoints]\n"
                     "                [--pinned] [--native libc,libm,gs,pad,mc,cd,vu0,io,mpeg,sif,snd|all|none]\n"
                     "                [--pad-script [port=]file] [--card-dir dir] [--iso image] [--quiet]\n"
                     "                [--snd-log-replies on|off|<ids>]\n");
        return 2;
    }

    bool readFile(const std::string &path, std::vector<uint8_t> &out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    const char *pct(uint32_t n, uint32_t d, char *buf)
    {
        std::snprintf(buf, 32, "%u/%u", n, d);
        return buf;
    }
}

int main(int argc, char **argv)
{
    using namespace bozo;
    if (argc < 2)
        return usage();
    std::string logPath = argv[1], refPath, outPath, elfPath = BOZO_RUN_ELF;
    std::vector<std::pair<int, std::string>> padScripts;
    std::string cardDir, isoPath;
    run::Options opt;
    for (int k = 2; k < argc; ++k)
    {
        const std::string a = argv[k];
        auto value = [&]() -> std::string { return k + 1 < argc ? argv[++k] : ""; };
        if (a == "--ref")
            refPath = value();
        else if (a == "--out")
            outPath = value();
        else if (a == "--frames")
            opt.frames = static_cast<uint32_t>(std::stoul(value()));
        else if (a == "--elf")
            elfPath = value();
        else if (a == "--max-errors")
            opt.maxErrors = static_cast<uint32_t>(std::stoul(value()));
        else if (a == "--trace-from")
            opt.traceFrom = std::stoull(value());
        else if (a == "--trace-checkpoints")
            opt.traceCheckpoints = true;
        else if (a == "--quiet")
            opt.quiet = true;
        else if (a == "--pinned")
            opt.pinned = true;
        else if (a == "--snd-log-replies")
        {
            // on (every reply the reference's IOP time decides from the log), off (none: 989SND's model answers
            // them), or the command ids still taken from the log (hex, comma-separated: 11,19,21,2c).
            const std::string v = value();
            if (v == "on")
                opt.sndFromLog = 0xF;
            else if (v == "off")
                opt.sndFromLog = 0;
            else
            {
                opt.sndFromLog = 0;
                size_t at = 0;
                while (at < v.size())
                {
                    const size_t end = v.find(',', at);
                    const std::string id = v.substr(at, end == std::string::npos ? std::string::npos : end - at);
                    if (id == "11") opt.sndFromLog |= 1;
                    else if (id == "19") opt.sndFromLog |= 2;
                    else if (id == "21") opt.sndFromLog |= 4;
                    else if (id == "2c") opt.sndFromLog |= 8;
                    else return usage();
                    if (end == std::string::npos)
                        break;
                    at = end + 1;
                }
            }
        }
        else if (a == "--pad-script")
        {
            // [port=]file: port 1 or 2 (1 when omitted)
            std::string v = value();
            int port = 0;
            if (v.size() > 2 && v[1] == '=' && (v[0] == '1' || v[0] == '2'))
            {
                port = v[0] - '1';
                v = v.substr(2);
            }
            padScripts.emplace_back(port, v);
        }
        else if (a == "--card-dir")
            cardDir = value();
        else if (a == "--iso")
            isoPath = value();
        else if (a == "--native")
        {
            if (!libs::parseLibraries(value(), opt.native))
                return usage();
        }
        else
            return usage();
    }

    calllog::Reader log;
    if (!log.open(logPath))
    {
        std::fprintf(stderr, "%s\n", log.error().c_str());
        return 2;
    }
    log.peek(); // reads NAME and OVRD
    std::vector<uint8_t> elf;
    if (!readFile(elfPath, elf) || elf.size() < 0x40 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0)
    {
        std::fprintf(stderr, "cannot read the game's ELF (%s): --elf <path>\n", elfPath.c_str());
        return 2;
    }
    const uint64_t elfHash = hash::xxh64(elf.data(), elf.size(), 0);
    if (elfHash != log.header().elfHash)
        std::printf("warning: the ELF's XXH64 %016llx is not the log's %016llx\n", static_cast<unsigned long long>(elfHash),
                    static_cast<unsigned long long>(log.header().elfHash));
    uint32_t entry;
    std::memcpy(&entry, elf.data() + 24, 4);

    std::vector<reference::FrameRecord> refFrames;
    reference::Header refHeader;
    if (!refPath.empty())
    {
        std::string err;
        if (!reference::read(refPath, refHeader, refFrames, err))
        {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
    }

    if ((opt.native & libs::kCd) && isoPath.empty())
    {
        // The CD library reads the game's disc image: without one its calls stay answered from the log.
        if (const char *env = std::getenv("BOZO_ISO"); env && *env)
            isoPath = env;
        else
        {
            opt.native &= ~static_cast<uint32_t>(libs::kCd);
            std::printf("note: no disc image (--iso or BOZO_ISO): the CD library is answered from the log\n");
        }
    }
    if (!isoPath.empty())
    {
        libs::DiscImage probe;
        std::string err;
        if (!probe.open(isoPath, err))
        {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
    }
    if ((opt.native & libs::kMpeg) && !libs::haveVideoDecoder())
    {
        opt.native &= ~static_cast<uint32_t>(libs::kMpeg);
        std::printf("note: built without FFmpeg: libmpeg is answered from the log\n");
    }
    run::LogHost host(log, opt);
    libs::ScriptInput padScript;
    for (const auto &[port, path] : padScripts)
    {
        std::vector<uint8_t> text;
        std::string err;
        if (!readFile(path, text) || !padScript.load(port, std::string(text.begin(), text.end()), err))
        {
            std::fprintf(stderr, "pad script %s: %s\n", path.c_str(), err.empty() ? "cannot read it" : err.c_str());
            return 2;
        }
    }
    host.libraries().pad.input = &padScript;
    host.libraries().mc.hostDirectory = cardDir; // empty: cards in memory, fresh and formatted
    host.libraries().cd.imagePath = isoPath;
    // The movies' sound: counted (the mixer takes it in the real runtime).
    struct CountingSink final : libs::MovieSoundSink
    {
        uint64_t frames = 0, movies = 0;
        libs::MovieSoundFormat last;
        void format(const libs::MovieSoundFormat &f) override
        {
            last = f;
            ++movies;
        }
        void samples(const int16_t *, size_t count, int64_t) override { frames += count; }
    } movieSound;
    host.libraries().movieSound = &movieSound;
    // 989SND's output: counted (the mixer takes it in phase 4).
    struct CountingSoundSink final : libs::SoundSink
    {
        uint64_t banks = 0, sounds = 0, params = 0, stops = 0, streams = 0;
        void bankLoaded(const libs::SndBank &) override { ++banks; }
        void soundStarted(uint32_t, const libs::SndBank &, uint32_t, int32_t, int32_t, int32_t, int32_t, uint64_t) override { ++sounds; }
        void soundParams(uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t) override { ++params; }
        void soundStopped(uint32_t) override { ++stops; }
        void streamStarted(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint64_t) override { ++streams; }
    } soundSink;
    host.libraries().snd.sink = &soundSink;
    ee::Dispatcher d(host);
    d.pinned = opt.pinned;
    d.setFunctions(tmb_function_table, tmb_function_count, tmb_resume_table, tmb_resume_count);
    const uint32_t kernel = host.bind(d);
    if (!host.init(d))
    {
        std::fprintf(stderr, "%s: the log does not start with its INIT record\n", logPath.c_str());
        return 2;
    }
    // The reference's boot: the main thread at the ELF's entry, its stack at the top of RAM (calllog: the first CTX).
    d.boot(entry, 0x1FFFFF0u);

    reference::Writer writer;
    if (!outPath.empty())
    {
        reference::Header h;
        h.elfHash = elfHash;
        h.run = log.header().run;
        h.build = "BozoEngine bozo-run";
        h.elfName = "SCUS_971.01";
        if (!writer.open(outPath, h))
        {
            std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
            return 2;
        }
        host.setWriter(&writer);
    }
    if (!refFrames.empty())
        host.setReference(&refFrames);

    if (!opt.quiet)
        std::printf("bozo-run: %s (run %s, %u frames), %zu names (%u native kernel calls), %zu overrides\n", logPath.c_str(),
                    log.header().run.c_str(), log.header().frameLimit, log.names().size(), kernel, log.overrides().size());
    const auto t0 = std::chrono::steady_clock::now();
    try
    {
        d.run();
    }
    catch (const ee::EndRun &)
    {
    }
    writer.close();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const run::FieldStats &s = host.stats();
    char b1[32], b2[32], b3[32], b4[32], b5[32];
    std::printf("frames %u; records matched %llu; calls %llu (%llu by the native kernel); dispatches %llu, stops %llu, "
                "handlers %llu; %.1f s\n",
                s.frames, static_cast<unsigned long long>(host.recordsMatched()),
                static_cast<unsigned long long>(host.calls()), static_cast<unsigned long long>(host.kernelCalls()),
                static_cast<unsigned long long>(d.dispatches), static_cast<unsigned long long>(d.stops),
                static_cast<unsigned long long>(d.handlersRun), secs);
    if (!refFrames.empty())
        std::printf("frames matching the reference: ram %s, scratchpad %s, calls %s, threads %s, eeCycle %s\n",
                    pct(s.ram, s.frames, b1), pct(s.scratchpad, s.frames, b2), pct(s.calls, s.frames, b3),
                    pct(s.threads, s.frames, b4), pct(s.eeCycle, s.frames, b5));
    {
        const run::NativeStats &n = host.nativeStats();
        std::printf("native libraries:");
        uint64_t total = 0;
        for (unsigned bit = 0; bit < libs::kLibraryCount; ++bit)
            if ((opt.native & (1u << bit)) && (1u << bit) != libs::kSnd) // 989SND's replies: its own line
            {
                std::printf(" %s %llu", libs::libraryName(static_cast<libs::Library>(1u << bit)),
                            static_cast<unsigned long long>(n.calls[bit]));
                total += n.calls[bit];
            }
        std::printf("%s (%llu calls answered natively); %llu answered calls followed; %llu divergences",
                    opt.native ? "" : " none", static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(n.observed), static_cast<unsigned long long>(n.divergences));
        if (n.notNative)
            std::printf("; %llu calls with no native answer (from the log)", static_cast<unsigned long long>(n.notNative));
        if (n.timeDiffers)
            std::printf("; %llu timed calls at another EE time than the log's (kept)", static_cast<unsigned long long>(n.timeDiffers));
        std::printf("\n");
    }
    if (movieSound.movies)
        std::printf("movie sound: %llu streams, %llu sample frames (%u Hz, %u channels)\n",
                    static_cast<unsigned long long>(movieSound.movies), static_cast<unsigned long long>(movieSound.frames),
                    movieSound.last.rate, movieSound.last.channels);
    if (opt.native & libs::kSnd)
    {
        const libs::SndStats &st = host.libraries().snd.stats;
        uint64_t nat = 0, pin = 0;
        std::string per;
        for (unsigned i = 0; i < 0x50; ++i)
        {
            nat += st.native[i];
            pin += st.pinned[i];
            if (st.native[i] || st.pinned[i])
                per += std::format(" {:02x}:{}/{}", i, st.native[i], st.pinned[i]);
        }
        std::printf("989snd: %llu replies native, %llu decided by the reference's IOP time (pinned); per command native/pinned:%s\n",
                    static_cast<unsigned long long>(nat), static_cast<unsigned long long>(pin), per.c_str());
        std::printf("989snd: %llu commands followed from calls answered from the log (%llu calls lost), %llu followed allocations "
                    "elsewhere than the native heap's, %llu pinned lifetimes the native estimate gets wrong, %llu of %llu pinned handles the native model gives; output: %llu banks, "
                    "%llu sounds, %llu parameter changes, %llu stops, %llu streams\n",
                    static_cast<unsigned long long>(st.followed), static_cast<unsigned long long>(st.lost),
                    static_cast<unsigned long long>(st.followMismatch), static_cast<unsigned long long>(st.estimateWrong),
                    static_cast<unsigned long long>(st.modelAgrees), static_cast<unsigned long long>(st.modelAgrees + st.modelDiffers),
                    static_cast<unsigned long long>(soundSink.banks), static_cast<unsigned long long>(soundSink.sounds),
                    static_cast<unsigned long long>(soundSink.params), static_cast<unsigned long long>(soundSink.stops),
                    static_cast<unsigned long long>(soundSink.streams));
        std::string agree;
        for (unsigned i = 0; i < 0x50; ++i)
            if (st.engineAgrees[i] || st.engineDiffers[i])
                agree += std::format(" {:02x}:{}/{}", i, st.engineAgrees[i], st.engineDiffers[i]);
        std::printf("989snd model against the log (agree/differ):%s", agree.c_str());
        if (st.firstDifferCycle)
            std::printf("; first difference at EE cycle %llu: command %02x (0x%x), log 0x%x, model 0x%x",
                        static_cast<unsigned long long>(st.firstDifferCycle), st.firstDifferId, st.firstDifferArg,
                        st.firstDifferLog, st.firstDifferEngine);
        std::putchar('\n');
    }
    if (opt.pinned)
    {
        const run::PinnedStats &p = host.pinnedStats();
        std::printf("pinned: %llu events at the log's points", static_cast<unsigned long long>(p.events));
        if (p.shifted)
        {
            std::printf(" (%llu one checkpoint later:", static_cast<unsigned long long>(p.shifted));
            for (uint64_t i : p.shiftedRecords)
                std::printf(" #%llu", static_cast<unsigned long long>(i));
            std::printf(")");
        }
        std::printf("; not checked: %llu records at another cycle", static_cast<unsigned long long>(p.cycleDiffers));
        if (p.cycleDiffers)
            std::printf(" (first #%llu)", static_cast<unsigned long long>(p.firstCycleRecord));
        std::printf(", %llu at another checkpoint count", static_cast<unsigned long long>(p.checkpointsDiffer));
        if (p.checkpointsDiffer)
            std::printf(" (first #%llu)", static_cast<unsigned long long>(p.firstCheckpointRecord));
        std::printf("\n");
    }
    // Pinned mode checks every record and ram, scratchpad, calls, threads; eeCycle is reported only.
    const bool ok = host.errors() == 0 && host.completed() &&
                    (refFrames.empty() || (s.ram == s.frames && s.scratchpad == s.frames && s.calls == s.frames &&
                                           s.threads == s.frames && (opt.pinned || s.eeCycle == s.frames)));
    std::printf("%s\n", ok ? "MATCH" : "DIFFERS");
    return ok ? 0 : 1;
}
