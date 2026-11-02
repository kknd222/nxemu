// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace Tegra {
enum class RenderDebugCounter : std::size_t {
    UnboundMethods, DmaBatches, DrawRequests, DrawPipelineReady,
    ComputeRequests, ComputePipelineReady, ClearRequests,
    DisplayAccelerated, DisplayFallback, DisplayFallbackUs, DisplayFallbackBytes, Count
};
// Each GPU owns its counters. Readers never queue GPU work or take a GPU lock.
class RenderDebugCounters {
public:
    void Record(RenderDebugCounter counter, uint64_t count = 1) {
        values[static_cast<std::size_t>(counter)].fetch_add(count, std::memory_order_relaxed);
    }
    uint64_t Read(RenderDebugCounter counter) const {
        return values[static_cast<std::size_t>(counter)].load(std::memory_order_relaxed);
    }
private:
    std::array<std::atomic<uint64_t>, static_cast<std::size_t>(RenderDebugCounter::Count)> values{};
};
} // namespace Tegra
