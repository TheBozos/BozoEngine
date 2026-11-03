// A minimal runtime for the tests: enough for recompiled code to run and for a test to see what it asked for.
// Not the real runtime (phase 2): no threads, no kernel, no I/O.
#pragma once

#include "bozo/runtime_api.h"

#include <cstdint>
#include <string>
#include <vector>

struct Runtime
{
    uint8_t scratchpad[16 * 1024] = {};
    uint8_t vu0Memory[4 * 1024] = {};
    const BozoFunctionEntry *table = nullptr; // for bozo_call_indirect
    size_t tableSize = 0;

    // What the code asked for, in order ("syscall 0x3c", "break 0x7", "callms 0x278", "io-read 0x10000000"...).
    std::vector<std::string> events;
    std::vector<uint32_t> indirect; // every bozo_call_indirect target, in order
    uint32_t yields = 0;
    uint32_t cop0[32] = {};
    bool interruptsEnabled = true;
    bool cop0Condition = true;
    uint32_t vpuStat = 0;
};

// The tests' Runtime as a fresh object (no global state).
inline void resetRuntime(Runtime &rt) { rt = Runtime(); }
