// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <stdint.h>

namespace Hardware
{
constexpr uint64_t BASE_CLOCK_RATE = 1'020'000'000; // Default CPU Frequency = 1020 MHz
constexpr uint64_t CNTFREQ = 19'200'000;            // CNTPCT_EL0 Frequency = 19.2 MHz
// Capacity, not guest permission: NS1 uses 0..3; NS2 uses 0..5.
constexpr uint32_t NUM_CPU_CORES = 6;
constexpr uint32_t NUM_NS1_CPU_CORES = 4;

// Cortex-A57 supports 4 memory watchpoints
constexpr uint64_t NUM_WATCHPOINTS = 4;

} // namespace Hardware