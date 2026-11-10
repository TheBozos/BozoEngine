// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// bozo/render/worker_priority.h.
#include "bozo/render/worker_priority.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace bozo::render
{
    void lowerWorkerPriority()
    {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(SYS_gettid)
        setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10); // Linux: a thread's own nice value
#endif
    }
}
