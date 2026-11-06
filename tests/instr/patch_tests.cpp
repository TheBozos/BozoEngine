// Guarded native adaptations must never silently target another executable or change input files.
#include "program.h"
#include "decoder.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace
{
    int failures = 0;
    void check(bool ok, const char *what)
    {
        if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
    }
    bozo::Program fixture()
    {
        bozo::Program p;
        p.base = 0x1000;
        p.image.resize(32);
        p.code.push_back({0x1000, 0x1010, "synthetic code"});
        const uint32_t words[] = {0x8c220000, 0xac200000, 0x03e00008, 0};
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned b = 0; b < 4; ++b) p.image[i * 4 + b] = words[i] >> (b * 8);
        return p;
    }
}
int main()
{
    std::string error;
    auto p = fixture();
    check(bozo::applyInstructionPatches(p, {{0x1000, 0x8c220000, 0x24020000}, {0x1004, 0xac200000, 0}}, error),
          "valid native diagnostic removal");
    check(p.read32(0x1000) == 0x24020000 && p.read32(0x1004) == 0 && p.read32(0x1008) == 0x03e00008,
          "only declared instructions are adapted");
    uint32_t unknown = 0;
    while (bozo::decode(unknown) != bozo::Op::Unknown) ++unknown;
    for (const auto &batch : std::vector<std::vector<bozo::InstructionPatch>>{
             {{0x1000, 0x8c220000, 0}, {0x1004, 123, 0}}, // later mismatch must not partly apply
             {{0x1000, 0x8c220000, 0}, {0x1000, 0x8c220000, 0}},
             {{0x1001, 0, 0}}, {{0x1010, 0, 0}}, {{0x2000, 0, 0}},
             {{0x1000, 0x8c220000, unknown}}})
    {
        auto original = fixture();
        const auto bytes = original.image;
        check(!bozo::applyInstructionPatches(original, batch, error), "unsafe adaptation is rejected");
        check(original.image == bytes, "failed batch leaves the complete analysis image unchanged");
    }
    const auto path = std::filesystem::temp_directory_path() /
                      ("bozo-native-patches-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    for (const char *text : {"# comment\n0x1000 8c220000 24020000 # native replacement\n",
                            "1000 8c220000\n", "1000 8c220000 0 extra\n", "100000000 0 0\n",
                            "1000 8c220000 0\n1004 invalid 0\n"})
    {
        { std::ofstream out(path); out << text; }
        std::vector<bozo::InstructionPatch> patches;
        const bool ok = bozo::readInstructionPatches(path.string(), patches, error);
        const bool valid = text[0] == '#';
        check(ok == valid, "strict patch-file parsing");
        check(valid ? patches.size() == 1 : patches.empty(), "failed parsing has no partial output");
    }
    std::filesystem::remove(path);
    std::printf("guarded instruction adaptations: %d failures\n", failures);
    return failures ? 1 : 0;
}
