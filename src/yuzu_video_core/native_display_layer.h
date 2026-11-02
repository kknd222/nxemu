// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <optional>
namespace VideoCommon {
// Presentation may select one layer of a joined native swapchain image, never an arbitrary byte alias.
struct NativeDisplayLayerLayout {
    std::uint64_t cpu_base{}, guest_bytes{}, layer_stride{};
    std::uint32_t layers{}, levels{}, width{}, height{}, block_height{}, samples{};
    bool native{}, image_2d{}, block_depth_zero{}, compatible{};
};
inline std::optional<std::uint32_t> NativeDisplayLayer(
    const NativeDisplayLayerLayout& l, std::uint64_t address,
    std::uint32_t width, std::uint32_t height, std::uint32_t stride, std::uint32_t block_height) {
    if (!l.native || !l.image_2d || !l.block_depth_zero || !l.compatible || l.levels != 1 ||
        l.samples != 1 || !l.layers || !l.layer_stride || !width || !height ||
        l.width != stride || l.width < width || l.height < height || l.block_height != block_height ||
        address < l.cpu_base) return std::nullopt;
    const auto offset = address - l.cpu_base;
    if (offset % l.layer_stride || offset >= l.guest_bytes || l.layer_stride > l.guest_bytes - offset)
        return std::nullopt;
    const auto layer = offset / l.layer_stride;
    if (layer >= l.layers) return std::nullopt;
    return static_cast<std::uint32_t>(layer);
}
// A sparse image's CPU base can name its first mapped layer, not GPU layer zero.
// The mapper must validate the whole layer and return its exact CPU start.
template <typename Mapper>
inline std::optional<std::uint32_t> NativeDisplayMappedLayer(
    const NativeDisplayLayerLayout& l, std::uint64_t address,
    std::uint32_t width, std::uint32_t height, std::uint32_t stride,
    std::uint32_t block_height, Mapper&& mapper) {
    if (!NativeDisplayLayer(l, l.cpu_base, width, height, stride, block_height))
        return std::nullopt;
    const auto matches = [&](std::uint32_t layer) {
        const auto offset = std::uint64_t{layer} * l.layer_stride;
        if (offset >= l.guest_bytes || l.layer_stride > l.guest_bytes - offset)
            return false;
        const auto mapped = mapper(offset, l.layer_stride);
        return mapped && *mapped == address;
    };
    const auto hint = NativeDisplayLayer(l, address, width, height, stride, block_height);
    if (hint && matches(*hint)) return hint;
    // Unusual huge arrays keep the existing fallback rather than unbounded frame work.
    if (l.layers > 128) return std::nullopt;
    for (std::uint32_t layer = 0; layer < l.layers; ++layer) {
        if ((!hint || layer != *hint) && matches(layer)) return layer;
    }
    return std::nullopt;
}
} // namespace VideoCommon
