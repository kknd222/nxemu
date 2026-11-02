// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace VideoCommon {
// Conservative write coverage since the last queued complete snapshot.
// An unknown writer, a new image or incompatible metadata means every layer.
// A snapshot owns its copy plan; resetting this tracker cannot change it.
class NativeDirtyLayers {
public:
    void MarkAll() noexcept { full = true; ++whole_writes; }
    void MarkRange(int first, int count, int total) noexcept {
        ++ranged_writes;
        if (total <= 0 || total > MaxLayers || first < 0 || count <= 0 ||
            first >= total || count > total - first || (!full && total != layers)) {
            MarkAll();
            return;
        }
        if (full) return;
        for (int layer = first; layer < first + count; ++layer)
            bits[layer / 64] |= std::uint64_t{1} << (layer % 64);
    }
    bool Includes(int layer) const noexcept {
        return full || (layer >= 0 && layer < layers &&
                        (bits[layer / 64] & (std::uint64_t{1} << (layer % 64))) != 0);
    }
    bool IsFull() const noexcept { return full; }
    std::uint64_t WholeWrites() const noexcept { return whole_writes; }
    std::uint64_t RangedWrites() const noexcept { return ranged_writes; }
    void SnapshotQueued(int total) noexcept {
        if (total <= 0 || total > MaxLayers) { full = true; return; }
        bits.fill(0);
        layers = total;
        full = false;
        whole_writes = ranged_writes = 0;
    }
private:
    static constexpr int MaxLayers = 4096;
    std::array<std::uint64_t, MaxLayers / 64> bits{};
    int layers{};
    bool full{true};
    std::uint64_t whole_writes{}, ranged_writes{};
};
} // namespace VideoCommon
