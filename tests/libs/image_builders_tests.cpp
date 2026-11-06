// Native ports discard image packet definitions; generic SDK callers keep their output ABI.
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
    struct Port : bozo::port::Host
    {
        bozo::libs::Libraries &libs;
        bool native = true;
        explicit Port(bozo::libs::Libraries &value) : libs(value) {}
        bozo::libs::Libraries &libraries() override { return libs; }
        void frameEnd(Runtime *, R5900Context *) override {}
        bool callLibrary(Runtime *, R5900Context *, uint32_t) override { return false; }
        bool nativeGraphics() const override { return native; }
    };
}
int main()
{
    auto rt = std::make_unique<Runtime>();
    bozo::libs::Libraries libs;
    Port port(libs);
    rt->port = &port;
    R5900Context ctx;
    bozo::libs::Journal journal;
    bozo::libs::Guest guest(*rt, &ctx, &journal);
    int failures = 0;
    const auto check = [&](bool pass, const char *what) {
        if (!pass) { ++failures; std::printf("FAIL %s\n", what); }
    };
    for (const char *name : {"sceGsSetDefLoadImage", "sceGsSetDefStoreImage"})
    {
        for (bool native : {false, true})
        {
            port.native = native;
            journal.clear();
            std::memset(rt->memory.ram() + 0x100000, 0xa5, 256);
            const uint32_t args[] = {0x100000, 0, 1, 0, 0, 0, 8, 8};
            for (unsigned i = 0; i < 8; ++i) ctx.r[4+i].ud[0] = args[i];
            ctx.r[29].uw[0] = 0x12000000;
            ctx.r[2].sd[0] = -99;
            libs.find(name)->handler(libs, guest);
            check(ctx.r[2].sw[0] == 0, "image builder result");
            check(journal.entries.empty() == native, "native mode alone suppresses SDK packet output");
            if (native)
                for (unsigned i = 0; i < 256; ++i)
                    check(rt->memory.ram()[0x100000+i] == 0xa5, "native image builder leaves guarded output intact");
        }
        port.native = true;
        journal.clear();
        ctx.r[4].uw[0] = 0x12000000;
        ctx.r[2].sd[0] = -99;
        libs.find(name)->handler(libs, guest);
        check(ctx.r[2].sw[0] == 0 && journal.entries.empty(), "native image builder never dereferences inaccessible output");
    }
    std::printf("native image builders: %d failures\n", failures);
    return failures ? 1 : 0;
}
