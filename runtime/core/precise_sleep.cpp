// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
#include "bozo/app/precise_sleep.h"

#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace bozo::app
{
#if defined(_WIN32)
namespace
{
struct Timer
{
    HANDLE handle = nullptr;
    Timer()
    {
        handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!handle)
            handle = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~Timer()
    {
        if (handle)
            CloseHandle(handle);
    }
    Timer(const Timer &) = delete;
    Timer &operator=(const Timer &) = delete;
};
}

void sleepUntil(std::chrono::steady_clock::time_point until)
{
    const auto left = until - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero())
        return;
    thread_local Timer timer;
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
    if (timer.handle && due.QuadPart < 0 && SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0)
        return;
    std::this_thread::sleep_until(until);
}
#else
void sleepUntil(std::chrono::steady_clock::time_point until) { std::this_thread::sleep_until(until); }
#endif
}
