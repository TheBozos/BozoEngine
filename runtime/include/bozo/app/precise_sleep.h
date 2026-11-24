// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The Bozos (boubou666 and Arzaroth). MIT licence (see LICENSE at the repository root).
#pragma once

#include <chrono>

namespace bozo::app
{
// On Windows a waitable timer: MinGW's sleep_until oversleeps by up to a timer tick under some Wine builds.
void sleepUntil(std::chrono::steady_clock::time_point until);
}
