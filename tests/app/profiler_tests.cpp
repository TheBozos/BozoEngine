// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The sampling profiler (bozo/app/profiler.h): a registered thread spinning in a known function is sampled, and the
// profile names that function first; a thread that ended keeps its samples until the next start; a report is written.
#include "bozo/app/profiler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        if (!ok)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_failures;
        }
    }

    std::atomic<bool> g_stop{false};
    volatile unsigned long long g_sink = 1;
} // namespace

// Not inlined, external: the program's symbol table names it.
extern "C" __attribute__((noinline)) void bozo_profiler_test_spin()
{
    while (!g_stop.load(std::memory_order_relaxed))
        for (int i = 0; i < 1000; ++i)
            g_sink = g_sink * 6364136223846793005ull + 1442695040888963407ull;
}

int main()
{
    namespace profiler = bozo::app::profiler;
    if (!profiler::available())
    {
        std::printf("profiler not available here: skipped\n");
        return 0;
    }
    std::atomic<bool> ready{false};
    std::thread spinner([&] {
        profiler::registerThread("spinner");
        ready = true;
        bozo_profiler_test_spin();
        profiler::unregisterThread();
    });
    while (!ready)
        std::this_thread::yield();
    profiler::start();
    check(profiler::running(), "running after start");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    profiler::stop();
    check(!profiler::running(), "stopped after stop");
    g_stop = true;
    spinner.join();

    const profiler::Profile p = profiler::profile(10);
    check(p.seconds > 0.3, "seconds sampled");
    const profiler::Thread *t = nullptr;
    for (const auto &thread : p.threads)
        if (thread.name == "spinner")
            t = &thread;
    check(t != nullptr, "the spinner's thread in the profile (its samples kept after it ended)");
    if (t)
    {
        std::printf("spinner: %llu samples, top %s\n", static_cast<unsigned long long>(t->samples),
                    t->functions.empty() ? "(none)" : t->functions.front().name.c_str());
        check(t->samples >= 50, "at least 50 samples in 400 ms");
        check(!t->functions.empty() && t->functions.front().name == "bozo_profiler_test_spin", "the spinning function first");
        check(!t->functions.empty() && t->functions.front().samples * 10 >= t->samples * 8, "most samples in it");
    }

    const std::string path = (std::filesystem::temp_directory_path() / "bozo_profiler_test" / "report.txt").string();
    check(profiler::writeReport(path), "report written");
    std::ifstream in(path);
    const std::string report((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(report.find("bozo_profiler_test_spin") != std::string::npos, "report names the function");
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path).parent_path(), ec);

    if (g_failures)
        return 1;
    std::printf("profiler tests passed\n");
    return 0;
}
