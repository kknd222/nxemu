// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>
namespace VideoCommon {
// Count every rejected alias, but only format/publish diagnostics at a bounded
// cadence. A bad guest pointer must not turn the bridge into per-page disk IO.
class PhysicalAliasDiagnosticGate {
public:
    bool Observe(std::uint64_t now_ms) {
        ++rejected_count;
        if (rejected_count != 1 && now_ms - last_publish_ms < 250) return false;
        last_publish_ms = now_ms;
        ++published_count;
        return true;
    }
    std::uint64_t rejected_count{}, published_count{};
private:
    std::uint64_t last_publish_ms{};
};
// A rasterizer callback may reenter the physical-address walker and resize its
// thread-local scratch buffer. Do not retain scratch iterators across callbacks.
template <typename Range, typename Callback, typename Rejected>
void ForEachStablePhysicalAlias(const Range& scratch, std::uint64_t page_limit,
                                Callback&& callback, Rejected&& rejected) {
    const std::vector<std::uint32_t> snapshot(scratch.begin(), scratch.end());
    for (auto page : snapshot) {
        if (page != 0 && page < page_limit) callback(page);
        else if (page != 0) rejected(page);
    }
}
} // namespace VideoCommon
