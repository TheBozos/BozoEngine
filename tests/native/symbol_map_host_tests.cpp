// The free host names and binds an executable without symbols from a symbol map; no game code.
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    int failures = 0;

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

    std::vector<uint8_t> strippedElf()
    {
        constexpr uint32_t kBase = 0x100000, kCodeAt = 0x54, kWords = 16;
        std::vector<uint8_t> out(kCodeAt + kWords * 4);
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
        put32(out, 68, kWords * 4);
        put32(out, 72, kWords * 4);
        put32(out, 76, 5);
        put32(out, 80, 4);
        for (uint32_t i = 0; i < kWords; i += 2)
            put32(out, kCodeAt + i * 4, 0x03E00008u);
        return out;
    }

    template <typename T> void writeFile(const std::filesystem::path &path, const T &bytes)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

int main()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bozo_symbol_map_host_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path elf = dir / "stripped.elf", map = dir / "map.txt", bad = dir / "bad.txt";
    writeFile(elf, strippedElf());
    writeFile(map, std::string("# names\nscePadRead 0x100008\nWaitSema 0x100010\nFlushCache 0x100018\ngameFunction 0x100020\n"));
    writeFile(bad, std::string("scePadRead 0x200000\n"));

    const BozoExternalEntry externals[] = {{0x100008, 1}, {0x100010, 1}, {0x100018, 1}};

    {
        bozo::host::FreeOptions options;
        options.elf = elf.string();
        options.symbolMap = map.string();
        bozo::host::FreeHost host(options);
        bozo::ee::Dispatcher d(host);
        std::string error;
        expect(host.setUp(d, externals, 3, nullptr, error), "a map binds a stripped executable");
        expect(host.symbol("scePadRead") == 0x100008, "the map's library function is named");
        expect(host.symbol("WaitSema") == 0x100010, "the map's kernel call is named");
        expect(host.functionAt(0x100024) == "gameFunction+0x4", "the map names unbound functions too");
    }
    {
        bozo::host::FreeOptions options;
        options.elf = elf.string();
        bozo::host::FreeHost host(options);
        bozo::ee::Dispatcher d(host);
        std::string error;
        expect(host.setUp(d, externals, 3, nullptr, error), "no map: nothing is named, the set-up still works");
        expect(host.symbol("scePadRead") == 0, "no map: no name");
    }
    {
        bozo::host::FreeOptions options;
        options.elf = elf.string();
        options.symbolMap = bad.string();
        bozo::host::FreeHost host(options);
        bozo::ee::Dispatcher d(host);
        std::string error;
        expect(!host.setUp(d, externals, 3, nullptr, error), "a map outside the code is refused");
        expect(error.find("not an instruction") != std::string::npos, "the refusal says why");
    }
    std::filesystem::remove_all(dir);
    if (failures == 0)
        std::printf("symbol map host tests passed\n");
    return failures == 0 ? 0 : 1;
}
