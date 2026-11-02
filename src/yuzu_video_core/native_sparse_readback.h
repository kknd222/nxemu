// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <vector>
namespace VideoCommon {
// Preserve guest layer indices while packing only mapped layer runs in staging.
// The publisher must consume this same plan, not reconstruct a full-array copy.
template <typename Copy, typename IsMapped>
std::vector<Copy> PlanNativeSparseReadback(const Copy& full, IsMapped&& is_mapped) {
    const auto layers = full.image_subresource.num_layers;
    if (layers <= 0 || full.buffer_size % layers != 0) return {};
    const std::size_t per_layer = full.buffer_size / layers;
    std::vector<Copy> copies;
    std::size_t packed_offset = 0;
    for (decltype(full.image_subresource.num_layers) layer = 0; layer < layers;) {
        if (!is_mapped(layer)) { ++layer; continue; }
        const auto first = layer++;
        while (layer < layers && is_mapped(layer)) ++layer;
        auto copy = full;
        copy.image_subresource.base_layer = full.image_subresource.base_layer + first;
        copy.image_subresource.num_layers = layer - first;
        copy.buffer_offset = packed_offset;
        copy.buffer_size = per_layer * (layer - first);
        packed_offset += copy.buffer_size;
        copies.push_back(copy);
    }
    return copies;
}
} // namespace VideoCommon
