// bozo-replay: phase 1's exit check (docs/specs/replay.md). Replays every sample of PS2XCAP1 captures through
// BozoEngine's generated code for the game and compares the exit state with the reference's.
//
//   bozo-replay [options] <capture folder or root>...     every capture, one worker process each, then the report
//       --jobs n            workers at once (default: the number of hardware threads)
//       --out dir           where results and the report go (default: replay-results)
//       --function 0xA[,..] only these functions' samples
//       --elf path          the game's executable (default: the one the build was generated from)
//       --budget cycles     a sample that runs longer is stopped as a runaway (default 4e9)
//   bozo-replay --show <capture> <index> [--elf path]       one sample in detail: out-calls, context, memory bytes
//   bozo-replay --worker <capture> --results <file> --elf-hash h [--start index] [--function ...] [--budget n]
//       (internal) one capture, one result line per sample
#include "capture.h"
#include "replay.h"

#include "elf.h"

#include "tmb_functions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef BOZO_REPLAY_ELF
#define BOZO_REPLAY_ELF ""
#endif
#ifndef BOZO_REPLAY_EXTERNALS
#define BOZO_REPLAY_EXTERNALS ""
#endif

namespace fs = std::filesystem;
using namespace bozo;
using namespace bozo::capture;
using namespace bozo::replay;

namespace
{
    struct Options
    {
        std::vector<std::string> inputs;
        std::string out = "replay-results";
        std::string elf = BOZO_REPLAY_ELF;
        std::string externals = BOZO_REPLAY_EXTERNALS;
        std::set<uint32_t> functions;
        unsigned jobs = 0;
        uint64_t budget = 4'000'000'000ull;
        uint64_t elfHash = 0;
        uint32_t start = 0;
        std::string results;
        std::string self; // this executable, for the workers
        bool worker = false;
        bool show = false;
        uint32_t showIndex = 0;
    };

    std::set<uint32_t> parseList(const std::string &s)
    {
        std::set<uint32_t> out;
        std::stringstream in(s);
        std::string item;
        while (std::getline(in, item, ','))
            if (!item.empty())
                out.insert(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 0)));
        return out;
    }

    bool loadExternals(const std::string &path, std::map<uint32_t, uint8_t> &out)
    {
        std::ifstream in(path);
        if (!in)
            return false;
        std::string line;
        while (std::getline(in, line))
        {
            char kind[32] = {};
            unsigned long address = 0;
            if (line.empty() || line[0] == '#' || std::sscanf(line.c_str(), "%31s %lx", kind, &address) != 2)
                continue;
            out[static_cast<uint32_t>(address)] = std::strcmp(kind, "stub") == 0 ? kStub : kOverride;
        }
        return true;
    }

    std::map<uint32_t, std::string> g_symbols; // function names, from the ELF

    std::string nameOf(uint32_t address)
    {
        auto it = g_symbols.find(address);
        return it != g_symbols.end() ? it->second : std::format("func_{:x}", address);
    }

    bool loadElf(const std::string &path, uint64_t *hash)
    {
        ElfFile elf;
        if (!elf.load(path))
        {
            std::fprintf(stderr, "bozo-replay: %s\n", elf.error().c_str());
            return false;
        }
        for (const ElfSymbol &s : elf.symbols())
            if (s.type == 2 && !s.name.empty())
                g_symbols.emplace(s.value, s.name);
        if (hash)
            *hash = xxh64(elf.bytes().data(), elf.bytes().size(), 0);
        return true;
    }

    // One line of a worker's results: the sample's status and what differs, tab-separated, no tabs inside fields.
    std::string resultLine(const Sample &s, const SampleResult &r)
    {
        std::string d;
        auto add = [&](const std::string &x) {
            std::string y = x;
            std::replace(y.begin(), y.end(), '\t', ' ');
            d += (d.empty() ? "" : "; ") + y;
        };
        for (const std::string &i : r.outcallIssues)
            add(i);
        for (size_t k = 0; k < r.words.size() && k < 12; ++k)
        {
            const uint32_t w = r.words[k];
            add(std::format("{}={:08x}/{:08x}", wordName(w), r.got[w], r.want[w]));
        }
        if (r.words.size() > 12)
            add(std::format("+{} words", r.words.size() - 12));
        for (size_t k = 0; k < r.ranges.size() && k < 6; ++k)
            add(std::format("mem 0x{:x}+{}", guestAddress(r.ranges[k].flat), r.ranges[k].length));
        if (r.ranges.size() > 6)
            add(std::format("+{} ranges", r.ranges.size() - 6));
        std::set<std::string> notes(r.notes.begin(), r.notes.end());
        for (const std::string &n : notes)
            add("note: " + n);
        return std::format("S\t{}\t0x{:x}\t{}\t{}\t{}\t{}/{}\t{}\n", s.index, s.function, r.status, r.words.size(),
                           r.differingBytes, r.outcallsUsed, s.outcalls.size(), d);
    }

    // ---- Worker: one capture --------------------------------------------------------------------------------------
    int worker(const Options &o)
    {
        Reader reader;
        if (!reader.open(o.inputs.at(0)))
        {
            std::fprintf(stderr, "bozo-replay: %s\n", reader.error().c_str());
            return 3;
        }
        if (o.elfHash && reader.header().elfHash && reader.header().elfHash != o.elfHash)
        {
            std::fprintf(stderr, "bozo-replay: %s was captured from an ELF with XXH64 %016llx, not this one (%016llx)\n",
                         reader.path().c_str(), static_cast<unsigned long long>(reader.header().elfHash),
                         static_cast<unsigned long long>(o.elfHash));
            return 3;
        }
        std::map<uint32_t, uint8_t> externals;
        if (!loadExternals(o.externals, externals))
        {
            std::fprintf(stderr, "bozo-replay: cannot read the externals list %s\n", o.externals.c_str());
            return 3;
        }
        FILE *out = std::fopen(o.results.c_str(), o.start ? "ab" : "wb");
        if (!out)
        {
            std::fprintf(stderr, "bozo-replay: cannot write %s\n", o.results.c_str());
            return 3;
        }
        if (!o.start)
            std::fprintf(out, "H\t%s\t%016llx\n", reader.header().run.c_str(), static_cast<unsigned long long>(reader.header().elfHash));
        auto replayer = std::make_unique<Replayer>(tmb_function_table, tmb_function_count, externals);
        replayer->cycleBudget = o.budget;
        Chunk c;
        std::string error;
        while (reader.next(c))
        {
            if (std::strcmp(c.tag, "SAMP") != 0)
                continue;
            uint32_t function, index;
            sampleId(c.data, function, index);
            if (!replayer->chainEntry(c.data, error))
            {
                std::fprintf(stderr, "bozo-replay: %s sample %u: %s\n", reader.path().c_str(), index, error.c_str());
                return 3;
            }
            if (index < o.start || (!o.functions.empty() && !o.functions.count(function)))
                continue;
            Sample s;
            if (!parseSample(std::move(c.data), s, error))
            {
                std::fprintf(stderr, "bozo-replay: %s: %s\n", reader.path().c_str(), error.c_str());
                return 3;
            }
            std::fprintf(out, "B\t%u\n", index);
            std::fflush(out);
            const SampleResult r = replayer->run(s, false);
            std::fputs(resultLine(s, r).c_str(), out);
        }
        std::fputs("E\n", out);
        std::fclose(out);
        if (!reader.error().empty())
        {
            std::fprintf(stderr, "bozo-replay: %s\n", reader.error().c_str());
            return 3;
        }
        return 0;
    }

    // ---- Show: one sample in detail ------------------------------------------------------------------------------
    int show(const Options &o)
    {
        Reader reader;
        if (!reader.open(o.inputs.at(0)))
        {
            std::fprintf(stderr, "bozo-replay: %s\n", reader.error().c_str());
            return 3;
        }
        if (!o.elf.empty())
            loadElf(o.elf, nullptr);
        std::map<uint32_t, uint8_t> externals;
        if (!loadExternals(o.externals, externals))
        {
            std::fprintf(stderr, "bozo-replay: cannot read the externals list %s\n", o.externals.c_str());
            return 3;
        }
        auto replayer = std::make_unique<Replayer>(tmb_function_table, tmb_function_count, externals);
        replayer->cycleBudget = o.budget;
        Chunk c;
        std::string error;
        while (reader.next(c))
        {
            if (std::strcmp(c.tag, "SAMP") != 0)
                continue;
            uint32_t function, index;
            sampleId(c.data, function, index);
            if (!replayer->chainEntry(c.data, error))
            {
                std::fprintf(stderr, "bozo-replay: %s\n", error.c_str());
                return 3;
            }
            if (index != o.showIndex)
                continue;
            Sample s;
            if (!parseSample(std::move(c.data), s, error))
            {
                std::fprintf(stderr, "bozo-replay: %s\n", error.c_str());
                return 3;
            }
            std::printf("%s sample %u: %s (0x%x), frame %u, ra 0x%x, sp 0x%x, %zu out-calls\n", reader.header().run.c_str(),
                        s.index, nameOf(s.function).c_str(), s.function, s.frame, s.returnAddress, s.stackPointer,
                        s.outcalls.size());
            for (size_t k = 0; k < s.outcalls.size(); ++k)
            {
                const Outcall &x = s.outcalls[k];
                std::printf("  log #%zu %s 0x%x (%s) size %u value %08x %08x %08x %08x;", k, kindName(x.kind), x.target,
                            nameOf(x.target).c_str(), x.size, x.value[0], x.value[1], x.value[2], x.value[3]);
                for (const auto &[w, v] : x.words)
                    std::printf(" %s=%08x", wordName(w).c_str(), v);
                std::printf("; %zu runs\n", x.runs.size());
            }
            std::printf("replay:\n");
            const SampleResult r = replayer->run(s, true);
            std::printf("status: %s, %llu cycles, %u of %zu out-calls used\n", statusText(r.status).c_str(),
                        static_cast<unsigned long long>(r.cycles), r.outcallsUsed, s.outcalls.size());
            for (const std::string &i : r.outcallIssues)
                std::printf("  out-call: %s\n", i.c_str());
            for (const std::string &n : r.notes)
                std::printf("  note: %s\n", n.c_str());
            for (uint32_t w : r.words)
                std::printf("  %-12s got %08x  want %08x  (entry %08x)\n", wordName(w).c_str(), r.got[w], r.want[w], s.entry[w]);
            // Memory: the expected bytes where an exit run covers them (elsewhere the entry bytes were expected).
            for (const MemoryRange &m : r.ranges)
            {
                std::printf("  memory 0x%x (+%u bytes)\n", guestAddress(m.flat), m.length);
                for (const Run &x : s.exitRuns)
                    if (m.flat < x.address + x.length && x.address < m.flat + m.length)
                    {
                        const uint32_t from = std::max(m.flat, x.address), to = std::min(m.flat + m.length, x.address + x.length);
                        std::printf("    want (exit run 0x%x):", guestAddress(x.address));
                        for (uint32_t b = from; b < to && b < from + 64; ++b)
                            std::printf(" %02x", x.data[b - x.address]);
                        std::printf("\n");
                    }
            }
            return r.status == 0 ? 0 : 1;
        }
        std::fprintf(stderr, "bozo-replay: no sample %u\n", o.showIndex);
        return 3;
    }

    // ---- Driver --------------------------------------------------------------------------------------------------
    std::vector<std::string> captureFiles(const std::vector<std::string> &args)
    {
        std::vector<std::string> out;
        for (const std::string &a : args)
        {
            if (fs::is_regular_file(a))
                out.push_back(a);
            else if (fs::is_regular_file(fs::path(a) / "capture.bin"))
                out.push_back((fs::path(a) / "capture.bin").string());
            else if (fs::is_directory(a))
            {
                std::vector<std::string> sub;
                for (const auto &e : fs::directory_iterator(a))
                    if (e.is_directory() && fs::is_regular_file(e.path() / "capture.bin"))
                        sub.push_back((e.path() / "capture.bin").string());
                std::sort(sub.begin(), sub.end());
                out.insert(out.end(), sub.begin(), sub.end());
            }
        }
        return out;
    }

    int spawn(const std::vector<std::string> &args)
    {
        std::vector<const char *> argv;
        for (const std::string &a : args)
            argv.push_back(a.c_str());
        argv.push_back(nullptr);
#ifdef _WIN32
        std::vector<std::string> quoted;
        for (const std::string &a : args)
            quoted.push_back(a.find(' ') != std::string::npos ? "\"" + a + "\"" : a);
        std::vector<const char *> qv;
        for (const std::string &a : quoted)
            qv.push_back(a.c_str());
        qv.push_back(nullptr);
        return static_cast<int>(_spawnv(_P_WAIT, args[0].c_str(), qv.data()));
#else
        const pid_t pid = fork();
        if (pid == 0)
        {
            execv(args[0].c_str(), const_cast<char *const *>(argv.data()));
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
#endif
    }

    struct Line
    {
        uint32_t index = 0, function = 0, status = 0;
        std::string rest; // words, bytes, out-calls, details
        std::string detail;
    };

    struct RunResult
    {
        std::string name;
        std::string file;
        std::vector<Line> lines;
        int exitCode = 0;
        std::string problem;
    };

    // Reads a worker's results; returns the index of a sample begun and never finished (a crash), or -1.
    int64_t readResults(const std::string &path, RunResult &rr)
    {
        std::ifstream in(path);
        std::string line;
        int64_t open = -1;
        while (std::getline(in, line))
        {
            std::vector<std::string> f;
            std::stringstream ss(line);
            std::string x;
            while (std::getline(ss, x, '\t'))
                f.push_back(x);
            if (f.empty())
                continue;
            if (f[0] == "H" && f.size() >= 2)
                rr.name = f[1];
            else if (f[0] == "B" && f.size() >= 2)
                open = std::strtoll(f[1].c_str(), nullptr, 10);
            else if (f[0] == "S" && f.size() >= 7)
            {
                Line l;
                l.index = static_cast<uint32_t>(std::strtoul(f[1].c_str(), nullptr, 10));
                l.function = static_cast<uint32_t>(std::strtoul(f[2].c_str(), nullptr, 0));
                l.status = static_cast<uint32_t>(std::strtoul(f[3].c_str(), nullptr, 10));
                l.rest = std::format("{} words, {} bytes, out-calls {}", f[4], f[5], f[6]);
                l.detail = f.size() > 7 ? f[7] : "";
                // Lines of a sample that crashed and was rerun are replaced.
                if (!rr.lines.empty() && rr.lines.back().index == l.index)
                    rr.lines.back() = l;
                else
                    rr.lines.push_back(l);
                if (open == static_cast<int64_t>(l.index))
                    open = -1;
            }
        }
        return open;
    }

    int driver(Options &o)
    {
        if (o.elf.empty() || !loadElf(o.elf, &o.elfHash))
        {
            std::fprintf(stderr, "bozo-replay: the game's executable is needed (--elf)\n");
            return 2;
        }
        auto files = captureFiles(o.inputs);
        if (files.empty())
        {
            std::fprintf(stderr, "bozo-replay: no capture found\n");
            return 2;
        }
        // Largest first: the slowest start first.
        std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
            return fs::file_size(a) > fs::file_size(b);
        });
        fs::create_directories(o.out);
        std::vector<RunResult> results(files.size());
        std::atomic<size_t> nextJob{0};
        std::mutex printLock;
        const auto t0 = std::chrono::steady_clock::now();
        const unsigned jobs = o.jobs ? o.jobs : std::max(1u, std::thread::hardware_concurrency());
        auto work = [&]() {
            for (;;)
            {
                const size_t j = nextJob++;
                if (j >= files.size())
                    return;
                RunResult &rr = results[j];
                rr.file = files[j];
                const fs::path folder = fs::path(files[j]).parent_path();
                const std::string resultFile = (fs::path(o.out) / (folder.filename().string() + ".tsv")).string();
                uint32_t start = 0;
                for (int attempt = 0; attempt < 64; ++attempt)
                {
                    std::vector<std::string> args = {o.self, "--worker", files[j], "--results", resultFile,
                                                     "--elf-hash", std::format("0x{:x}", o.elfHash), "--budget",
                                                     std::to_string(o.budget), "--externals", o.externals};
                    if (start)
                    {
                        args.push_back("--start");
                        args.push_back(std::to_string(start));
                    }
                    if (!o.functions.empty())
                    {
                        std::string list;
                        for (uint32_t f : o.functions)
                            list += std::format("{}0x{:x}", list.empty() ? "" : ",", f);
                        args.push_back("--function");
                        args.push_back(list);
                    }
                    rr.exitCode = spawn(args);
                    if (rr.exitCode == 0 || rr.exitCode == 3)
                        break;
                    // The worker died: mark the sample it was in, and go on after it.
                    RunResult partial;
                    const int64_t crashed = readResults(resultFile, partial);
                    if (crashed < 0)
                    {
                        rr.problem = std::format("worker exited with {} outside a sample", rr.exitCode);
                        break;
                    }
                    std::ofstream app(resultFile, std::ios::app | std::ios::binary);
                    app << std::format("S\t{}\t0x0\t{}\t0\t0\t0/0\tthe worker process died (exit code {})\n", crashed,
                                       static_cast<uint32_t>(kCrashed), rr.exitCode);
                    start = static_cast<uint32_t>(crashed + 1);
                }
                if (rr.exitCode == 3)
                    rr.problem = "the worker could not read the capture (see its message)";
                readResults(resultFile, rr);
                std::lock_guard<std::mutex> lk(printLock);
                size_t matched = 0;
                for (const Line &l : rr.lines)
                    matched += l.status == 0;
                std::printf("  %-12s %5zu samples, %5zu match%s\n", rr.name.c_str(), rr.lines.size(), matched,
                            rr.problem.empty() ? "" : (" (" + rr.problem + ")").c_str());
                std::fflush(stdout);
            }
        };
        std::vector<std::thread> threads;
        for (unsigned k = 0; k < jobs; ++k)
            threads.emplace_back(work);
        for (auto &t : threads)
            t.join();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        // ---- The report ----
        std::sort(results.begin(), results.end(), [](const RunResult &a, const RunResult &b) { return a.name < b.name; });
        std::string rep;
        rep += std::format("bozo-replay: {} captures, {:.0f} s\n\n", results.size(), seconds);
        rep += "| Run | Samples | Match | Functions | All samples match |\n|---|---|---|---|---|\n";
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> perFunction; // function -> (samples, matching)
        std::map<uint32_t, std::vector<std::string>> mismatchesByFunction;
        std::map<uint32_t, uint32_t> statusCounts;
        uint64_t totalSamples = 0, totalMatch = 0;
        for (const RunResult &rr : results)
        {
            std::map<uint32_t, std::pair<uint32_t, uint32_t>> fn;
            size_t match = 0;
            for (const Line &l : rr.lines)
            {
                auto &e = fn[l.function];
                ++e.first;
                e.second += l.status == 0;
                match += l.status == 0;
                auto &g = perFunction[l.function];
                ++g.first;
                g.second += l.status == 0;
                if (l.status)
                {
                    ++statusCounts[l.status];
                    mismatchesByFunction[l.function].push_back(std::format("{} #{}: {} ({}): {}", rr.name, l.index,
                                                                           statusText(l.status), l.rest, l.detail));
                }
            }
            size_t fnMatch = 0;
            for (const auto &[a, e] : fn)
                fnMatch += e.first == e.second;
            totalSamples += rr.lines.size();
            totalMatch += match;
            rep += std::format("| {} | {} | {} | {} | {} |\n", rr.name, rr.lines.size(), match, fn.size(), fnMatch);
        }
        size_t fnAll = 0;
        for (const auto &[a, e] : perFunction)
            fnAll += e.first == e.second;
        rep += std::format("| **all** | **{}** | **{}** | **{}** | **{}** |\n\n", totalSamples, totalMatch, perFunction.size(), fnAll);
        rep += "Mismatching samples by status:\n";
        for (const auto &[s, n] : statusCounts)
            rep += std::format("  {:>6}  {}\n", n, statusText(s));
        rep += "\nMismatching functions:\n";
        for (const auto &[a, list] : mismatchesByFunction)
        {
            const auto &e = perFunction[a];
            rep += std::format("0x{:x} {} ({} of {} samples differ)\n", a, nameOf(a), e.first - e.second, e.first);
            for (size_t k = 0; k < list.size() && k < 4; ++k)
                rep += "    " + list[k] + "\n";
        }
        for (const RunResult &rr : results)
            if (!rr.problem.empty())
                rep += std::format("problem: {}: {}\n", rr.file, rr.problem);
        const std::string reportPath = (fs::path(o.out) / "report.txt").string();
        std::ofstream(reportPath, std::ios::binary) << rep;
        std::printf("%llu samples, %llu match; %zu functions, %zu with every sample matching; %.0f s. Report: %s\n",
                    static_cast<unsigned long long>(totalSamples), static_cast<unsigned long long>(totalMatch),
                    perFunction.size(), fnAll, seconds, reportPath.c_str());
        return totalSamples == totalMatch ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    Options o;
    o.self = fs::absolute(argv[0]).string();
    for (int k = 1; k < argc; ++k)
    {
        const std::string a = argv[k];
        auto value = [&]() -> std::string {
            if (k + 1 >= argc)
            {
                std::fprintf(stderr, "bozo-replay: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++k];
        };
        if (a == "--worker")
        {
            o.worker = true;
            o.inputs.push_back(value());
        }
        else if (a == "--show")
        {
            o.show = true;
            o.inputs.push_back(value());
            o.showIndex = static_cast<uint32_t>(std::strtoul(value().c_str(), nullptr, 0));
        }
        else if (a == "--results")
            o.results = value();
        else if (a == "--elf-hash")
            o.elfHash = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--start")
            o.start = static_cast<uint32_t>(std::strtoul(value().c_str(), nullptr, 0));
        else if (a == "--function")
        {
            const auto l = parseList(value());
            o.functions.insert(l.begin(), l.end());
        }
        else if (a == "--budget")
            o.budget = static_cast<uint64_t>(std::strtod(value().c_str(), nullptr));
        else if (a == "--jobs")
            o.jobs = static_cast<unsigned>(std::strtoul(value().c_str(), nullptr, 0));
        else if (a == "--out")
            o.out = value();
        else if (a == "--elf")
            o.elf = value();
        else if (a == "--externals")
            o.externals = value();
        else if (!a.empty() && a[0] == '-')
        {
            std::fprintf(stderr, "bozo-replay: unknown option %s\n", a.c_str());
            return 2;
        }
        else
            o.inputs.push_back(a);
    }
    if (o.worker)
        return worker(o);
    if (o.show)
        return show(o);
    if (o.inputs.empty())
    {
        std::fprintf(stderr, "usage: bozo-replay [--jobs n] [--out dir] [--function 0xA,..] <capture folder or root>...\n"
                             "       bozo-replay --show <capture> <index>\n");
        return 2;
    }
    return driver(o);
}
