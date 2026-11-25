// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// A sampling profiler of a port's threads: where a slow frame's time goes, in the recompiled game's functions
// (Ot_draw_0x12e7f0, body_0x12e7f0) and in the port's and the engine's own. While it runs, a thread of its own reads
// each registered thread's instruction pointer about every millisecond (Windows: the thread suspended for the read;
// Linux: a SIGPROF the thread answers from its handler); a profile counts the samples by function, named from the
// executable's own symbol table (PE/COFF or ELF, demangled), a sample in a system library by the library's export it
// falls after ("ntdll.dll!NtWaitForSingleObject": a thread waiting).
// Only the function a sample falls in counts ("self" time): no call stacks.
//
// The window registers its game thread as "game" and its own (the renderer's, the present's) as "window"; a port registers its own workers (registerThread on that thread).
// The settings window (F2) has a Profiler tab: start, stop, the functions of each thread, a report saved to the user
// folder's profiles/. PS2X_PROFILE=<from>:<to>[:<file>]: from game frame <from> to <to>, the report written to <file>
// (or the profiles folder) and its path on stderr, for scripted runs. In bozo_profiler (no SDL3): a headless program
// links it too. Elsewhere than Windows and Linux available() is false and nothing is sampled.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bozo::app::profiler
{
    // Sampling works on this platform.
    bool available();

    // The calling thread, sampled under `name` while the profiler runs (a second call renames it). A thread that ends
    // unregisters first; its samples stay in the profile until the next start.
    void registerThread(const std::string &name);
    void unregisterThread();

    // Starts sampling the registered threads (the last profile cleared); stop ends it. Any thread.
    void start();
    void stop();
    bool running();

    struct Function
    {
        std::string name; // demangled, without its parameters; "module.dll!Export" in a library ("[module]": no export)
        uint64_t samples = 0;
    };
    struct Thread
    {
        std::string name;
        uint64_t samples = 0;
        std::vector<Function> functions; // the most sampled first
    };
    struct Profile
    {
        double seconds = 0.0; // sampled so far
        std::vector<Thread> threads;
    };
    // The samples so far (running or stopped), each thread's `top` functions.
    Profile profile(size_t top = 50);
    // A report as text: each thread's functions, their share of its samples.
    std::string text(const Profile &profile);
    // The report (`top` functions per thread) written to `path`, its folder made. False when it could not be.
    bool writeReport(const std::string &path, size_t top = 200);

    // PS2X_PROFILE's frames: call at each frame count seen (the window, after a present); starts and stops the
    // profiler at the variable's frames and writes its report (without a file named: into `folder`, timestamped).
    void frames(uint64_t ended, const std::string &folder);
    // "<folder>/profile_<date>_<time>.txt".
    std::string reportPath(const std::string &folder);
} // namespace bozo::app::profiler
