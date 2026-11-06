// A port's VU0 microprograms and its audio stream through the free host; no game code.
#include "bozo/audio/port_stream.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    int failures = 0;
    uint32_t lastCall = 0;

    void expect(bool ok, const char *what)
    {
        if (!ok)
        {
            std::printf("FAILED: %s\n", what);
            ++failures;
        }
    }

    void put32(std::vector<uint8_t> &out, size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); }
    void put16(std::vector<uint8_t> &out, size_t at, uint16_t v) { std::memcpy(out.data() + at, &v, 2); }

    std::vector<uint8_t> elfImage()
    {
        constexpr uint32_t kBase = 0x100000, kCodeAt = 0x54, kBytes = 16;
        std::vector<uint8_t> out(kCodeAt + kBytes);
        const uint8_t ident[8] = {0x7F, 'E', 'L', 'F', 1, 1, 1, 0};
        std::memcpy(out.data(), ident, sizeof ident);
        put16(out, 16, 2);
        put16(out, 18, 8);
        put32(out, 20, 1);
        put32(out, 24, kBase);
        put32(out, 28, 52);
        put16(out, 40, 52);
        put16(out, 42, 32);
        put16(out, 44, 1);
        put16(out, 46, 40);
        put32(out, 52, 1);
        put32(out, 56, kCodeAt);
        put32(out, 60, kBase);
        put32(out, 64, kBase);
        put32(out, 68, kBytes);
        put32(out, 72, kBytes);
        put32(out, 76, 5);
        put32(out, 80, 4);
        return out;
    }

    void programA(uint8_t *, R5900Context *ctx, Runtime *) { lastCall = 0xA + ctx->r[4].uw[0]; }
    void programB(uint8_t *, R5900Context *, Runtime *) { lastCall = 0xB; }
}

int main()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bozo_port_hooks_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path elf = dir / "game.elf";
    {
        const std::vector<uint8_t> image = elfImage();
        std::ofstream out(elf, std::ios::binary);
        out.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
    }
    bozo::host::FreeOptions options;
    options.elf = elf.string();
    {
        bozo::port::Registry registry;
        registry.vu0Program(0x40, programA);
        registry.vu0Program(0x80, programB);
        registry.vu0Program(0x80, programA);
        registry.vu0Program(0x80, programB);
        expect(registry.vu0Programs().size() == 2, "one program per entry");

        bozo::host::FreeHost host(options);
        bozo::ee::Dispatcher d(host);
        std::string error;
        expect(host.setUp(d, nullptr, 0, &registry, error), "set up");
        R5900Context ctx;
        ctx.r[4].uw[0] = 3;
        const uint32_t pc = ctx.pc = 0x100000;
        host.vu0Microprogram(d, &ctx, 0x40);
        expect(lastCall == 0xD, "the port's program runs, on the calling context");
        expect(ctx.pc == pc, "and pc stays");
        host.vu0Microprogram(d, &ctx, 0x80);
        expect(lastCall == 0xB, "a later registration replaces");

        expect(!host.cop0Condition(d, &ctx), "by default no completed transfer means BC0T is not taken");

        auto &memory = d.runtime().memory;
        for (int i = 0; i < 32; ++i)
            memory.ram()[0x100200 + i] = static_cast<uint8_t>(0x80 + i);
        const auto store = [&](uint32_t address, uint64_t value)
        {
            const uint64_t words[2] = {value, 0};
            host.ioWrite(d, &ctx, address, 4, words);
        };
        store(0x1000D480, 0x70000100);
        store(0x1000D410, 0x100200);
        store(0x1000D420, 2);
        store(0x1000D400, 0x181);
        expect(memory.scratchpad()[0x100 + 31] == 0x80 + 31, "a store to CHCR copies to the scratchpad");
        store(0x1000D080, 0x70000100);
        store(0x1000D010, 0x100300);
        store(0x1000D020, 2);
        store(0x1000D000, 0x180);
        expect(memory.ram()[0x100300 + 31] == 0x80 + 31, "and from it");

        bool faulted = false;
        try
        {
            host.vu0Microprogram(d, &ctx, 0x278);
        }
        catch (const bozo::ee::EndRun &)
        {
            faulted = true;
        }
        expect(faulted, "a port with programs has no others, TMB's neither");

        const int16_t frames[4] = {100, -100, 200, -200};
        expect(bozo::port::audioStream(&d.runtime()).push(frames, 2) == 2, "the port pushes through the runtime");
        expect(host.libraries().portStream.queued() == 2, "to the libraries' stream");
    }
    {
        bozo::port::Registry registry;
        bozo::host::FreeHost host(options);
        bozo::ee::Dispatcher d(host);
        std::string error;
        expect(host.setUp(d, nullptr, 0, &registry, error), "set up without programs");
        R5900Context ctx;
        lastCall = 0;
        host.vu0Microprogram(d, &ctx, 0x278);
        expect(lastCall == 0, "no port program: the built-in table answers");
    }
    {
        bozo::port::Registry registry;
        registry.transfersAlwaysDone();
        bozo::host::FreeHost host(options);
        bozo::ee::Dispatcher d(host);
        std::string error;
        expect(host.setUp(d, nullptr, 0, &registry, error), "set up, transfers always done");
        R5900Context ctx;
        expect(host.cop0Condition(d, &ctx), "a port's transfers are always done");
        expect(host.stats().ioSites.empty(), "BC0 is profiled only when asked");
    }
    {
        bozo::port::Registry registry;
        registry.transfersAlwaysDone();
        bozo::host::FreeOptions profiled = options;
        profiled.profileIo = true;
        bozo::host::FreeHost host(profiled);
        bozo::ee::Dispatcher d(host);
        std::string error;
        expect(host.setUp(d, nullptr, 0, &registry, error), "set up, profiling");
        R5900Context ctx;
        ctx.pc = 0x123450;
        host.cop0Condition(d, &ctx);
        host.cop0Condition(d, &ctx);
        const auto &sites = host.stats().ioSites;
        const auto site = sites.find({0x123450u, bozo::host::kCop0ConditionSite});
        expect(site != sites.end() && site->second[0] == 2, "BC0 is counted by site as reads of its pseudo address");
    }
    std::filesystem::remove_all(dir);
    if (failures == 0)
        std::printf("port hooks tests passed\n");
    return failures == 0 ? 0 : 1;
}
