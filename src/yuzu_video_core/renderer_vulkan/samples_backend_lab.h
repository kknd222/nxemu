#pragma once
#include <functional>
#include <memory>
#include <span>
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"
#include "yuzu_video_core/query_cache/query_stream.h"
namespace VideoCore { class RasterizerInterface; }
namespace Vulkan {
class Device; class Scheduler; class MemoryAllocator; class DescriptorPool; class UpdateDescriptorQueue;
struct SamplesBackendCopy { u64 address; size_t size; size_t offset; };
using SamplesBackendSync=std::function<void(std::span<const SamplesBackendCopy>,VkBuffer)>;
std::unique_ptr<VideoCommon::StreamerInterface> CreateSamplesBackendLab(
 size_t id, SamplesBackendSync sync, VideoCore::RasterizerInterface* rasterizer,
 const Device& device,Scheduler& scheduler,const MemoryAllocator& allocator,
 UpdateDescriptorQueue& queue,DescriptorPool& pool);
}