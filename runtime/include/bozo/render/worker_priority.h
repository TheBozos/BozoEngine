// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
//
// The renderer's background workers (texture upscaling, texture replacements) run below the game's threads: they use
// what the game and the present thread leave, and never take a core from them while a level's copies are being made.
#pragma once

namespace bozo::render
{
    // The calling thread's scheduling priority lowered (Windows: THREAD_PRIORITY_LOWEST; elsewhere: nice 10).
    void lowerWorkerPriority();
}
