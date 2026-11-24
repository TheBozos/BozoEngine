// Save states (bozo/state.h): the archive's walkers and sections, the file header, and a kernel and dispatcher saved
// then loaded into a second runtime (which then saves the same bytes).
#include "bozo/ee/dispatcher.h"
#include "bozo/state.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>

using namespace bozo;

namespace
{
    int failures = 0;
    void check(bool v, const char *what)
    {
        if (!v)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }
    template <class F> bool throws(F f)
    {
        try
        {
            f();
        }
        catch (const state::Error &)
        {
            return true;
        }
        return false;
    }

    struct Point
    {
        int32_t x = 0;
        std::string name;
    };
    void io(state::Archive &a, Point &p) { state::fields(a, p.x, p.name); }

    struct NullHost final : ee::Host
    {
        void libraryCall(ee::Dispatcher &, R5900Context *, uint32_t) override {}
        void vu0Microprogram(ee::Dispatcher &, R5900Context *, uint32_t) override {}
        void ioRead(ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, uint64_t[2]) override {}
        void ioWrite(ee::Dispatcher &, R5900Context *, uint32_t, uint32_t, const uint64_t[2]) override {}
        void fault(ee::Dispatcher &, R5900Context *, const std::string &s) override { throw std::runtime_error(s); }
    };

    void archives()
    {
        state::Archive w;
        std::map<uint32_t, std::vector<Point>> m{{3, {{1, "a"}, {2, "bc"}}}, {9, {}}};
        std::unordered_map<int, std::deque<uint16_t>> u{{5, {1, 2}}, {-1, {7}}};
        std::array<uint64_t, 3> arr{1, 2, 3};
        uint32_t c[2][2] = {{1, 2}, {3, 4}};
        std::set<std::string> names{"x", "y"};
        std::unique_ptr<Point> some = std::make_unique<Point>(Point{7, "seven"}), none;
        bool flag = true;
        uint8_t b = 42;
        w.begin("outer");
        state::fields(w, m, u, arr, c, names, some, none, flag);
        w.begin("inner");
        state::fields(w, b);
        w.end();
        w.end();

        state::Archive r(w.data());
        std::map<uint32_t, std::vector<Point>> m2;
        std::unordered_map<int, std::deque<uint16_t>> u2;
        std::array<uint64_t, 3> arr2{};
        uint32_t c2[2][2] = {};
        std::set<std::string> names2;
        std::unique_ptr<Point> some2, none2 = std::make_unique<Point>();
        bool flag2 = false;
        uint8_t b2 = 0;
        r.begin("outer");
        state::fields(r, m2, u2, arr2, c2, names2, some2, none2, flag2);
        r.begin("inner");
        state::fields(r, b2);
        r.end();
        r.end();
        check(r.atEnd(), "reader at the end");
        check(m2.size() == 2 && m2[3].size() == 2 && m2[3][1].name == "bc" && m2[9].empty(), "map of vectors of structs");
        check(u2 == u, "unordered map");
        check(arr2 == arr && c2[1][0] == 3, "arrays");
        check(names2 == names, "set");
        check(some2 && some2->x == 7 && some2->name == "seven" && !none2, "unique_ptr");
        check(flag2 && b2 == 42, "scalars");

        // The same state always gives the same bytes (unordered maps in key order).
        state::Archive w2;
        w2.begin("outer");
        state::fields(w2, m2, u2, arr2, c2, names2, some2, none2, flag2);
        w2.begin("inner");
        state::fields(w2, b2);
        w2.end();
        w2.end();
        check(w2.data() == w.data(), "written again: the same bytes");

        // A reader that names another section, leaves fields unread, or reads past a section fails.
        check(throws([&] {
                  state::Archive x(w.data());
                  x.begin("other");
              }),
              "wrong section name");
        check(throws([&] {
                  state::Archive x(w.data());
                  x.begin("outer");
                  x.end();
              }),
              "fields left unread");
        check(throws([&] {
                  state::Archive x(w.data());
                  x.begin("outer");
                  std::vector<uint8_t> big(1 << 20);
                  x.raw(big.data(), big.size());
              }),
              "reading past a section");

        // An older layout (a field added since): the newer reader fails inside the section, the mark takes it back to
        // read the old way, and what follows the section reads on.
        {
            state::Archive old;
            uint32_t one = 1, two = 2, after = 3;
            old.begin("s");
            state::fields(old, one, two);
            old.end();
            old.begin("next");
            state::fields(old, after);
            old.end();
            state::Archive x(old.data());
            const state::Archive::Mark mark = x.mark();
            uint32_t r1 = 0, rNew = 0, r2 = 0, rAfter = 0;
            check(throws([&] {
                      x.begin("s");
                      state::fields(x, r1, rNew, r2);
                      x.end();
                  }),
                  "a newer layout read from an older section fails");
            x.rewind(mark);
            x.begin("s");
            state::fields(x, r1, r2);
            x.end();
            x.begin("next");
            state::fields(x, rAfter);
            x.end();
            check(r1 == 1 && r2 == 2 && rAfter == 3 && x.atEnd(), "rewound to the mark, read in the older layout");
            state::Archive y(old.data());
            check(y.nextIs("s") && !y.nextIs("next") && !y.nextIs("ss"), "nextIs names the coming section");
            y.begin("s");
            check(!y.nextIs("next"), "nextIs: fields come first");
        }
    }

    void files()
    {
        const std::string path = (std::filesystem::temp_directory_path() / "bozo_state_test.state").string();
        state::Archive w;
        uint32_t v = 0xC0FFEE;
        w.begin("s");
        state::fields(w, v);
        w.end();
        state::writeFile(path, "GAME-1", w);
        state::Archive r = state::readFile(path, "GAME-1");
        uint32_t v2 = 0;
        r.begin("s");
        state::fields(r, v2);
        r.end();
        check(v2 == v && r.atEnd(), "file round trip");
        check(throws([&] { state::readFile(path, "GAME-2"); }), "another game's file");
        check(throws([&] { state::readFile(path + ".missing", "GAME-1"); }), "a missing file");
        std::filesystem::remove(path);
    }

    void runtimes()
    {
        NullHost h1, h2;
        ee::Dispatcher d1(h1), d2(h2);
        d1.boot(0x100000, 0x1FFFFF0u);
        ee::Kernel &k = d1.kernel();
        const int32_t t = k.createThread(0x100100, 0x1000000, 0x4000, 10, 0x123456, 0, 0);
        k.startThread(t, 77, false);
        const int32_t s = k.createSema(0, 4, 0, 0);
        k.signalSema(s, false);
        k.setAlarm(123456, 0x100200, 9, 0x123456, false);
        k.addHandler(false, 2, 0x100300, 0, 5, 0x123456, 0);
        d1.runtime().memory.ram()[0x1234] = 0x5A;
        d1.runtime().memory.scratchpad()[7] = 0xA5;

        state::Archive a;
        d1.serialize(a);
        state::Archive r(a.data());
        d2.serialize(r);
        check(r.atEnd(), "dispatcher state read whole");
        check(d2.runtime().memory.ram()[0x1234] == 0x5A && d2.runtime().memory.scratchpad()[7] == 0xA5, "memory");
        check(d2.kernel().thread(t) && d2.kernel().thread(t)->priority == 10 &&
                  d2.kernel().thread(t)->frames[0]->ctx.r[4].uw[0] == 77,
              "a thread and its context");
        check(d2.kernel().nextEvent() == d1.kernel().nextEvent(), "deadlines and vblanks");
        state::Archive again;
        d2.serialize(again);
        check(again.data() == a.data(), "the loaded runtime saves the same bytes");

        // A native deadline with no kind cannot be saved; one with a kind is rebuilt by nativeRebuild.
        k.scheduleNative(0x77000001u, 1000, [](uint64_t) {});
        check(!d1.unsaveable().empty(), "an opaque native deadline is unsaveable");
        k.cancelDeadline(0x77000001u);
        int fired = 0;
        k.scheduleNative(0x77000002u, 10, 5, {1, 2, 3}, [](uint64_t) {});
        d2.kernel().nativeRebuild = [&](uint32_t kind, const std::array<uint32_t, 3> &args) {
            std::function<void(uint64_t)> fn;
            if (kind == 5 && args[2] == 3)
                fn = [&](uint64_t) { ++fired; };
            return fn;
        };
        state::Archive b;
        d1.serialize(b);
        state::Archive rb(b.data());
        d2.serialize(rb);
        d2.kernel().processEvents(10);
        check(fired == 1, "a native deadline rebuilt from its kind runs");
    }
}

int main()
{
    try
    {
        archives();
        files();
        runtimes();
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    if (failures)
        return 1;
    std::printf("state tests passed\n");
    return 0;
}
