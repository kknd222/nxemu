// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// One renderer-owned explicit request; no global startup environment or per-frame work here.
// The scope owner must keep this allocation alive until Scheduler::Finish succeeds.
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan::SourcePixelProbe {
inline constexpr size_t MaxBytes = 8 * 1024 * 1024;

struct Data {
    vk::Buffer buffer;
    uint32_t width{}, height{}, mip{}, layer{};
    VkFormat format{};
    VkImage source_image{};
    VkImageView source_view{};
    VkSampleCountFlagBits samples{};
    std::array<uint32_t, 4> swizzle{};
    bool attempted{}, queued{}, identity_swizzle{}, swizzle_known{};
    size_t bytes{};
    std::string status = "not_selected", reason = "no accelerated source selected";
};

inline thread_local Data* current{};
inline bool Active() noexcept { return current && !current->attempted; }

inline void Failed(const char* reason) noexcept {
    if (!current) {
        return;
    }
    // Never clear queued: the scope owner must still complete any command already recorded.
    current->attempted = true;
    try {
        current->status = "error";
        current->reason = reason;
    } catch (...) {
        // Allocation failure in optional diagnostics must not propagate into rendering.
    }
}

class Scope {
    Data* previous{};
public:
    Data data;
    explicit Scope(bool enabled) noexcept : previous(current) { current = enabled ? &data : nullptr; }
    ~Scope() { current = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
};

inline bool SupportedFormat(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

// Reusable by the Vulkan small-image oracle, as well as the actual accelerated display path.
// These handles/range refer to the exact current backing used by the selected Color2D view.
inline void Record(Scheduler& scheduler, MemoryAllocator& allocator, VkImage image,
                   VkImageView view, VkFormat format, uint32_t width, uint32_t height,
                   uint32_t mip, uint32_t layer, VkSampleCountFlagBits samples,
                   bool identity_swizzle, bool supported_dimension, bool single_mip,
                   std::array<uint32_t, 4> components = {0, 0, 0, 0},
                   bool components_known = true) {
    if (!Active()) {
        return;
    }
    auto& d = *current;
    d.attempted = true;
    d.source_image = image;
    d.source_view = view;
    d.format = format;
    d.width = width;
    d.height = height;
    d.mip = mip;
    d.layer = layer;
    d.samples = samples;
    d.identity_swizzle = identity_swizzle;
    d.swizzle_known = components_known;
    d.swizzle = components;
    d.status = "unavailable";
    if (!image || !view) { d.reason = "null image or selected view"; return; }
    if (!SupportedFormat(format)) { d.reason = "unsupported source format"; return; }
    if (!components_known) { d.reason = "selected Color2D component mapping unknown"; return; }
    for (const auto component : components) {
        if (component > VK_COMPONENT_SWIZZLE_A) {
            d.reason = "invalid selected Color2D component mapping"; return;
        }
    }
    if (!supported_dimension || !single_mip) {
        d.reason = "only nonslice 2D/2D-array single-mip source; scaled nonzero mip unsupported"; return;
    }
    if (samples != VK_SAMPLE_COUNT_1_BIT) { d.reason = "multisample source not resolved"; return; }
    if (width == 0 || height == 0 || width > MaxBytes / 4 ||
        height > MaxBytes / (size_t(width) * 4)) {
        d.reason = "source byte budget exceeded or zero extent"; return;
    }
    d.bytes = size_t(width) * height * 4;
    // Allocate BEFORE changing command stream state; own through GPU completion, not record return.
    d.buffer = CreateWrappedBuffer(allocator, d.bytes, MemoryUsage::Download);
    const VkBuffer buffer = *d.buffer;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
    const VkBufferImageCopy copy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image, buffer, range, copy](vk::CommandBuffer cmdbuf) {
        const VkImageMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        const VkImageMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        const VkMemoryBarrier host_read{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, before);
        cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, copy);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                              0, host_read, nullptr, after);
    });
    d.queued = true;
    d.status = "queued";
    d.reason.clear();
}
} // namespace Vulkan::SourcePixelProbe
