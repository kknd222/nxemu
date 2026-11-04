#include "yuzu_video_core/native_draw_invocation.h"
#include "yuzu_video_core/textures/decoders.h"
// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "surface_clip_state.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include "yuzu_video_core/native_debug_document.h"
#include <memory>
#include <mutex>

#if defined(ANDROID) || defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "yuzu_video_core/renderer_vulkan/renderer_vulkan.h"

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_video_core/buffer_cache/buffer_cache.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/engines/fast_copy.h"
#include "yuzu_video_core/engines/draw_manager.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/host1x/gpu_device_memory_manager.h"
#include "yuzu_video_core/renderer_vulkan/blit_image.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_vulkan/maxwell_to_vk.h"
#include "yuzu_video_core/renderer_vulkan/source_pixel_probe.h"
#include "yuzu_video_core/image_writer_trace.h"
#include "yuzu_video_core/renderer_vulkan/vk_buffer_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_query_cache.h"
#include "yuzu_video_core/draw_admission_trace.h"
#include "yuzu_video_core/renderer_vulkan/vk_rasterizer.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/shader_cache.h"
#include "yuzu_video_core/texture_cache/texture_cache_base.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using MaxwellDrawState = Tegra::Engines::DrawManager::State;

namespace {
bool ReadAndroidBoolProperty(const char* name, bool default_value) {
#if defined(ANDROID) || defined(__ANDROID__)
    char value[PROP_VALUE_MAX]{};
    const int len = __system_property_get(name, value);
    if (len <= 0) {
        return default_value;
    }
    if (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' || value[0] == 't' ||
        value[0] == 'T') {
        return true;
    }
    if (value[0] == '0' || value[0] == 'n' || value[0] == 'N' || value[0] == 'f' ||
        value[0] == 'F') {
        return false;
    }
#endif
    return default_value;
}

bool UseSquashedIteratedBlend() {
#if defined(ANDROID) || defined(__ANDROID__)
    return ReadAndroidBoolProperty("debug.nxemu.squashed_iterated_blend", true);
#else
    return false;
#endif
}

bool UseAndroidRenderDiag() {
#if defined(ANDROID) || defined(__ANDROID__)
    return ReadAndroidBoolProperty("debug.nxemu.render_diag", false);
#else
    return false;
#endif
}

bool ShouldAndroidRenderDiagLog(u64 count) {
#if defined(ANDROID) || defined(__ANDROID__)
    if (!UseAndroidRenderDiag()) {
        return false;
    }
    // Keep logcat usable: dense at startup, then powers of two and coarse periodic samples.
    return count <= 32 || (count <= 8192 && (count & (count - 1)) == 0) || count % 600 == 0;
#else
    return false;
#endif
}
} // namespace
using VideoCommon::ImageViewId;
using VideoCommon::ImageViewType;

namespace {
struct DrawParams {
    u32 base_instance;
    u32 num_instances;
    u32 base_vertex;
    u32 num_vertices;
    u32 first_index;
    bool is_indexed;
};

/// Returns the depth offset that lets ordered depth tests pass on values a 24-bit buffer would
/// store as equal, for hosts that keep those buffers in a floating point format
// TODO: confirm on AMD for safety
float EmulatedDepthCompareOffset(const Device& device, const Maxwell& regs) {
    if (device.SupportsD24DepthBuffer() || regs.depth_test_enable == 0 ||
        regs.depth_write_enabled != 0) {
        return 0.0f;
    }
    const bool is_d24 = regs.zeta.format == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
                        regs.zeta.format == Tegra::DepthFormat::X8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::S8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::V8Z24_UNORM;
    if (!is_d24) {
        return 0.0f;
    }
    static constexpr float step = 1.0f / 16777215.0f;
    switch (regs.depth_test_func) {
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return step;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return -step;
    default:
        return 0.0f;
    }
}

} // Anonymous namespace

VkViewport GetViewportState(const Device& device, const Maxwell& regs, size_t index, float scale,
                            float depth_offset) {
    const auto& src = regs.viewport_transform[index];
    const auto conv = [scale](float value) {
        float new_value = value * scale;
        if (scale < 1.0f) {
            const bool sign = std::signbit(value);
            new_value = std::round(std::abs(new_value));
            new_value = sign ? -new_value : new_value;
        }
        return new_value;
    };
    const float x = conv(src.translate_x - src.scale_x);
    const float width = conv(src.scale_x * 2.0f);
    float y = conv(src.translate_y - src.scale_y);
    float height = conv(src.scale_y * 2.0f);

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const bool y_negate = !device.IsNvViewportSwizzleSupported() &&
                          src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY;

    if (lower_left) {
        // Flip by surface clip height
        y += conv(static_cast<f32>(regs.surface_clip.height));
        height = -height;
    }

    if (y_negate) {
        // Flip by viewport height
        y += height;
        height = -height;
    }

    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
    VkViewport viewport{
        .x = x,
        .y = y,
        .width = width != 0.0f ? width : 1.0f,
        .height = height != 0.0f ? height : 1.0f,
        .minDepth = src.translate_z - src.scale_z * reduce_z + depth_offset,
        .maxDepth = src.translate_z + src.scale_z + depth_offset,
    };
    if (!device.IsExtDepthRangeUnrestrictedSupported()) {
        viewport.minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
        viewport.maxDepth = std::clamp(viewport.maxDepth, 0.0f, 1.0f);
    }
    return viewport;
}

namespace {

VkRect2D GetScissorState(const Maxwell& regs, size_t index, u32 up_scale = 1, u32 down_shift = 0) {
    const auto& src = regs.scissor_test[index];
    VkRect2D scissor;
    const auto scale_up = [&](s32 value) -> s32 {
        if (value == 0) {
            return 0U;
        }
        const s32 upset = value * up_scale;
        s32 acumm = 0;
        if ((up_scale >> down_shift) == 0) {
            acumm = upset % 2;
        }
        const s32 converted_value = (value * up_scale) >> down_shift;
        return value < 0 ? std::min<s32>(converted_value - acumm, -1)
                         : std::max<s32>(converted_value + acumm, 1);
    };

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = regs.surface_clip.height;

    // Flip coordinates if lower left
    s32 min_y = lower_left ? (clip_height - src.max_y) : src.min_y.Value();
    s32 max_y = lower_left ? (clip_height - src.min_y) : src.max_y.Value();

    // Bound to render area
    min_y = std::max(min_y, 0);
    max_y = std::max(max_y, 0);

    if (src.enable) {
        scissor.offset.x = scale_up(src.min_x);
        scissor.offset.y = scale_up(min_y);
        scissor.extent.width = scale_up(src.max_x - src.min_x);
        scissor.extent.height = scale_up(max_y - min_y);
    } else {
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = std::numeric_limits<s32>::max();
        scissor.extent.height = std::numeric_limits<s32>::max();
    }
    return scissor;
}

DrawParams MakeDrawParams(const MaxwellDrawState& draw_state, u32 num_instances, bool is_indexed) {
    DrawParams params{
        .base_instance = draw_state.base_instance,
        .num_instances = num_instances,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .is_indexed = is_indexed,
    };
    // 6 triangle vertices per quad, base vertex is part of the index
    // See BindQuadIndexBuffer for more details
    if (draw_state.topology == Maxwell::PrimitiveTopology::Quads) {
        params.num_vertices = (params.num_vertices / 4) * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    } else if (draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
        params.num_vertices = (params.num_vertices - 2) / 2 * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    }
    return params;
}
} // Anonymous namespace

RasterizerVulkan::RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                                   Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                   const Device& device_, MemoryAllocator& memory_allocator_,
                                   StateTracker& state_tracker_, Scheduler& scheduler_)
    : gpu{gpu_}, device_memory{device_memory_}, device{device_},
      has_dynamic_vertex_input{SupportsDirectVertexInput(device)},
      memory_allocator{memory_allocator_}, state_tracker{state_tracker_}, scheduler{scheduler_},
      staging_pool(device, memory_allocator, scheduler),
      indirect_scratch(device, scheduler, staging_pool), descriptor_pool(device, scheduler),
      guest_descriptor_queue(device, scheduler), compute_pass_descriptor_queue(device, scheduler),
      blit_image(device, scheduler, state_tracker, descriptor_pool), render_pass_cache(device),
      texture_cache_runtime{
          device,     scheduler,         memory_allocator, staging_pool,
          blit_image, render_pass_cache, descriptor_pool,  compute_pass_descriptor_queue},
      texture_cache(texture_cache_runtime, device_memory),
      buffer_cache_runtime(device, memory_allocator, scheduler, staging_pool,
                           guest_descriptor_queue, compute_pass_descriptor_queue, descriptor_pool),
      buffer_cache(device_memory, buffer_cache_runtime),
      query_cache_runtime(this, device_memory, buffer_cache, device, memory_allocator, scheduler,
                          staging_pool, compute_pass_descriptor_queue, descriptor_pool),
      query_cache(gpu, *this, device_memory, query_cache_runtime),
      pipeline_cache(device_memory, device, scheduler, descriptor_pool, guest_descriptor_queue,
                     render_pass_cache, buffer_cache, texture_cache, gpu.ShaderNotify(),
                     staging_pool),
      accelerate_dma(buffer_cache, texture_cache, scheduler),
      fence_manager(*this, gpu, texture_cache, buffer_cache, query_cache, device, scheduler) {
    scheduler.SetQueryCache(query_cache);
}

RasterizerVulkan::~RasterizerVulkan() {
    // Query and buffer caches are destroyed before texture_cache. Drain while
    // all scheduler callbacks and resources are still alive.
    texture_cache.DrainGCReadbacks();
}

template <typename Func>
void RasterizerVulkan::PrepareDraw(bool is_indexed, Func&& draw_func) {
    gpu.DebugRecord(Tegra::RenderDebugCounter::DrawRequests);
    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();
    gpu_memory->FlushCaching();

    query_cache.NotifySegment(true);

    ++frame_draws;
    // Optional CPU admission evidence. Read raw guest RT state and existing birth registry
    // only: do not resolve/create/update framebuffer attachments to observe a failed draw.
    namespace A = VideoCommon::DrawAdmission;
    A::Scoped admission;
    if (A::Enabled()) {
        const auto& regs = maxwell3d->regs;
        for (u32 output = 0; output < std::min<u32>(regs.rt_control.count, 8); ++output) {
            const u32 slot = regs.rt_control.Map(output);
            if (slot >= 8) continue;
            const auto& rt = regs.rt[slot];
            if (!rt.Address() || rt.format == Tegra::RenderTargetFormat::NONE) continue;
            u32 tile_mode{};
            std::memcpy(&tile_mode, &rt.tile_mode, sizeof(tile_mode));
            admission.BeginTarget({rt.Address(),slot,rt.width,rt.height,
                static_cast<u32>(rt.format),rt.base_layer,rt.depth,rt.array_pitch,
                tile_mode,regs.color_mask[regs.color_mask_common ? 0 : output].raw,output});
        }
    }
    GraphicsPipeline* pipeline{pipeline_cache.CurrentGraphicsPipeline()};
    if (!pipeline) {
        if (!pipeline_cache.SkippedUnfinished()) {
            admission.Finish(A::Outcome::PipelineNull);
            return;
        }
        // Skipping a draw whose pipeline is still compiling only hides it until a later frame
        // draws it again. A target never drawn in an earlier frame may be drawn only this once
        // (a texture the game renders once and keeps), so the draw waits for its pipeline.
        bool drawn_before{};
        {
            std::scoped_lock lock{texture_cache.mutex};
            texture_cache.UpdateRenderTargets(false);
            drawn_before = texture_cache.RenderTargetsDrawnBefore();
        }
        if (drawn_before) {
            ++frame_skipped_draws;
            admission.Finish(A::Outcome::AsyncSkipped);
            return;
        }
        pipeline = pipeline_cache.WaitForCurrentGraphicsPipeline();
        if (!pipeline) {
            admission.Finish(A::Outcome::WaitFailed);
            return;
        }
    }
    admission.Finish(A::Outcome::PipelineReady);
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    // update engine as channel may be different.
    pipeline->SetEngine(maxwell3d, gpu_memory);
    pipeline->SetVtgInstanceCount(std::exchange(vtg_instance_count, 0));
    pipeline->SetVtgIndirect(std::exchange(vtg_indirect, false), vtg_indirect_instance_mask);
    // Update the guest counter state before Configure opens the draw's render pass.
    // Host queries begin/end within that pass and accumulate across pass boundaries.
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              maxwell3d->regs.zpass_pixel_count_enable);
    pipeline->Configure(is_indexed);
    image_write_stages |= pipeline->ImageWriteStages();

    UpdateDynamicStates();

    HandleTransformFeedback();
    gpu.DebugRecord(Tegra::RenderDebugCounter::DrawPipelineReady);
    draw_func(pipeline);
    buffer_cache.CopySparseWrites();

    const auto& regs = maxwell3d->regs;
    u32 color_mask = 0;
    for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
        const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
        if (mask.R || mask.G || mask.B || mask.A) {
            color_mask |= 1U << index;
        }
    }
    texture_cache.MarkRenderTargetsWritten(color_mask,
                                           regs.depth_write_enabled != 0 || regs.stencil_enable != 0);
}

void RasterizerVulkan::DrawVtgAsCompute(const GraphicsPipeline* pipeline) {
    // The stages running as compute wrote the strips to draw into an index buffer
    const auto [index_buffer, index_offset, index_count]{pipeline->VtgDraw()};
    if (index_count == 0) {
        return;
    }
    const bool dynamic_restart{device.IsExtExtendedDynamicState2Supported()};
    if (dynamic_restart) {
        // The guest state is restored by the next draw
        maxwell3d->dirty.flags[Dirty::PrimitiveRestartEnable] = true;
        maxwell3d->dirty.flags[Dirty::StateEnable] = true;
    }
    scheduler.Record([pipeline, index_buffer = index_buffer, index_offset = index_offset,
                      index_count = index_count, dynamic_restart,
                      indirect = pipeline->VtgIndirectDraw()](vk::CommandBuffer cmdbuf) {
        if (!pipeline->IsBuilt()) {
            return;
        }
        if (dynamic_restart) {
            cmdbuf.SetPrimitiveRestartEnableEXT(true);
        }
        cmdbuf.BindIndexBuffer(index_buffer, index_offset, VK_INDEX_TYPE_UINT32);
        if (indirect) {
            cmdbuf.DrawIndexedIndirect(indirect->first, indirect->second, 1,
                                       sizeof(VkDrawIndexedIndirectCommand));
        } else {
            cmdbuf.DrawIndexed(index_count, 1, 0, 0, 0);
        }
    });
}

void RasterizerVulkan::Draw(bool is_indexed, u32 instance_count) {
    namespace DI=VideoCommon::NativeDrawInvocation;
    const DI::Scope diagnostic_invocation{DI::Enabled()?DI::MakeDirect(maxwell3d->draw_manager->GetDrawState(),is_indexed,instance_count):DI::Snapshot{}};
    vtg_instance_count = instance_count;
    PrepareDraw(is_indexed, [this, is_indexed, instance_count](const GraphicsPipeline* pipeline) {
        if (pipeline->IsVtgAsCompute()) {
            DrawVtgAsCompute(pipeline);
            return;
        }
        const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
        const u32 num_instances{instance_count};
        const DrawParams draw_params{MakeDrawParams(draw_state, num_instances, is_indexed)};
        if (VideoCommon::ImageWriters::Enabled()) {
            // The disabled path keeps its original small command closure. Birth epochs and
            // shader keys are fixed at enqueue, never looked up from a later current pipeline.
            auto events = pipeline->DiagnosticDrawEvents();
            for (auto& event : events) {
                event.aux = {draw_params.num_vertices, draw_params.num_instances,
                             static_cast<uint64_t>(draw_params.base_vertex), draw_params.base_instance};
            }
            scheduler.Record([pipeline, draw_params, events](vk::CommandBuffer cmdbuf) {
                namespace W = VideoCommon::ImageWriters;
                if (!pipeline->IsBuilt()) {
                    for (auto event : events) { event.op = W::Op::DrawSkipped; W::Record(event); }
                    return;
                }
                if (draw_params.is_indexed) {
                    cmdbuf.DrawIndexed(draw_params.num_vertices, draw_params.num_instances,
                                      draw_params.first_index, draw_params.base_vertex,
                                      draw_params.base_instance);
                } else {
                    cmdbuf.Draw(draw_params.num_vertices, draw_params.num_instances,
                               draw_params.base_vertex, draw_params.base_instance);
                }
                for (auto event : events) { event.op = W::Op::DrawRecorded; W::Record(event); }
            });
            return;
        }
        scheduler.Record([pipeline, draw_params](vk::CommandBuffer cmdbuf) {
            if (!pipeline->IsBuilt()) {
                return;
            }
            if (draw_params.is_indexed) {
                cmdbuf.DrawIndexed(draw_params.num_vertices, draw_params.num_instances,
                                   draw_params.first_index, draw_params.base_vertex,
                                   draw_params.base_instance);
            } else {
                cmdbuf.Draw(draw_params.num_vertices, draw_params.num_instances,
                            draw_params.base_vertex, draw_params.base_instance);
            }
        });
    });
}

bool RasterizerVulkan::DrawIndirect() {
    const auto& params = maxwell3d->draw_manager->GetIndirectParams();
    if (params.is_byte_count && !device.IsExtTransformFeedbackSupported()) {
        // The macro draws the vertices counted by the byte count it reads on the CPU
        return false;
    }
    if (!params.is_byte_count &&
        (!device.IsGeometryShaderSupported() || !device.IsExtTransformFeedbackSupported())) {
        // The stages running as compute need the parameters of the draw on the CPU
        gpu_memory->FlushCaching();
        const GraphicsPipeline* const pipeline{pipeline_cache.CurrentGraphicsPipeline()};
        if (pipeline && (pipeline->IsVtgAsCompute() || pipeline->IsTransformFeedbackEmulated())) {
            // A single non-indexed draw keeps its arguments on the GPU when only its vertex and
            // geometry stages run as compute. Draws counted by transform feedback have no
            // arguments to read.
            const bool gpu_arguments = pipeline->CanDrawVtgIndirect() &&
                                       !pipeline->IsTransformFeedbackEmulated() &&
                                       !params.is_byte_count && !params.is_indexed &&
                                       !params.include_count && params.max_draw_counts == 1;
            if (!gpu_arguments) {
                return false;
            }
            vtg_indirect = true;
            vtg_indirect_instance_mask = params.instance_mask;
        }
    }
    namespace DI=VideoCommon::NativeDrawInvocation;
    const DI::Scope diagnostic_invocation{DI::Enabled()?DI::MakeIndirect(maxwell3d->draw_manager->GetDrawState(),params):DI::Snapshot{}};
    buffer_cache.SetDrawIndirect(&params);
    PrepareDraw(params.is_indexed, [this, &params](const GraphicsPipeline* pipeline) {
        if (pipeline->IsVtgAsCompute()) {
            // Only draws whose stages took their arguments from GPU memory are drawn here. A
            // pipeline that became ready after the arguments were checked has no counts.
            if (pipeline->VtgIndirectDraw()) {
                DrawVtgAsCompute(pipeline);
            }
            return;
        }
        const auto indirect_buffer = buffer_cache.GetDrawIndirectBuffer();
        const auto& buffer = indirect_buffer.first;
        const auto& offset = indirect_buffer.second;
        if (params.is_byte_count) {
            scheduler.Record([pipeline, buffer_obj = buffer->Handle(), offset,
                              stride = params.stride](vk::CommandBuffer cmdbuf) {
                if (!pipeline->IsBuilt()) {
                    return;
                }
                cmdbuf.DrawIndirectByteCountEXT(1, 0, buffer_obj, offset, 0,
                                                static_cast<u32>(stride));
            });
            return;
        }
        if (params.include_count) {
            const auto count = buffer_cache.GetDrawIndirectCount();
            const auto& draw_buffer = count.first;
            const auto& offset_base = count.second;
            const bool can_preconvert = device.GetDriverID() == VK_DRIVER_ID_MOLTENVK &&
                !videoSettings.disable_buffer_reorder && params.is_indexed &&
                params.max_draw_counts > 0 && params.max_draw_counts <= (1U << 20) &&
                params.stride >= 20 && params.stride % 4 == 0 && offset % 4 == 0 &&
                params.stride * params.max_draw_counts + device.GetStorageBufferAlignment() <=
                    device.GetMaxStorageBufferRange() &&
                offset_base % 4 == 0 && !scheduler.IsBufferWritten(buffer->Handle()) &&
                !scheduler.IsBufferWritten(draw_buffer->Handle());
            if (can_preconvert) {
                if (!indirect_count_pass) {
                    indirect_count_pass =
                        std::make_unique<IndirectCountPass>(device, scheduler, descriptor_pool);
                }
                const auto [converted, converted_offset] =
                    indirect_scratch.Allocate(params.max_draw_counts * 20);
                indirect_count_pass->Convert(buffer->Handle(), offset, draw_buffer->Handle(),
                                            offset_base, converted, converted_offset,
                                            static_cast<u32>(params.max_draw_counts),
                                            static_cast<u32>(params.stride));
                scheduler.Record([pipeline, converted, converted_offset,
                                  draws = static_cast<u32>(params.max_draw_counts)](
                                     vk::CommandBuffer cmdbuf) {
                    if (pipeline->IsBuilt()) {
                        cmdbuf.DrawIndexedIndirect(converted, converted_offset, draws, 20);
                    }
                });
                return;
            }
            scheduler.Record([pipeline, draw_buffer_obj = draw_buffer->Handle(),
                              buffer_obj = buffer->Handle(), offset_base, offset,
                              params](vk::CommandBuffer cmdbuf) {
                if (!pipeline->IsBuilt()) {
                    return;
                }
                if (params.is_indexed) {
                    cmdbuf.DrawIndexedIndirectCount(
                        buffer_obj, offset, draw_buffer_obj, offset_base,
                        static_cast<u32>(params.max_draw_counts), static_cast<u32>(params.stride));
                } else {
                    cmdbuf.DrawIndirectCount(buffer_obj, offset, draw_buffer_obj, offset_base,
                                             static_cast<u32>(params.max_draw_counts),
                                             static_cast<u32>(params.stride));
                }
            });
            return;
        }
        scheduler.Record([pipeline, buffer_obj = buffer->Handle(), offset,
                          params](vk::CommandBuffer cmdbuf) {
            if (!pipeline->IsBuilt()) {
                return;
            }
            if (params.is_indexed) {
                cmdbuf.DrawIndexedIndirect(buffer_obj, offset,
                                           static_cast<u32>(params.max_draw_counts),
                                           static_cast<u32>(params.stride));
            } else {
                cmdbuf.DrawIndirect(buffer_obj, offset, static_cast<u32>(params.max_draw_counts),
                                    static_cast<u32>(params.stride));
            }
        });
    });
    buffer_cache.SetDrawIndirect(nullptr);
    vtg_indirect = false;
    return true;
}

void RasterizerVulkan::DrawTexture() {
    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();

    query_cache.NotifySegment(true);

    std::scoped_lock l{texture_cache.mutex};
    texture_cache.SynchronizeGraphicsDescriptors();
    texture_cache.UpdateRenderTargets(false);

    UpdateDynamicStates();

    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              maxwell3d->regs.zpass_pixel_count_enable);
    const auto& draw_texture_state = maxwell3d->draw_manager->GetDrawTextureState();
    const auto& sampler = texture_cache.GetGraphicsSampler(draw_texture_state.src_sampler);
    const auto& texture = texture_cache.GetImageView(draw_texture_state.src_texture);
    const auto* framebuffer = texture_cache.GetFramebuffer();

    const bool src_rescaling = texture_cache.IsRescaling() && texture.IsRescaled();
    const bool dst_rescaling = texture_cache.IsRescaling() && framebuffer->IsRescaled();

    const auto ScaleSrc = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return src_rescaling ? videoSettings.resolution_info.ScaleUp(dim) : dim;
    };

    const auto ScaleDst = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return dst_rescaling ? videoSettings.resolution_info.ScaleUp(dim) : dim;
    };

    Region2D dst_region = {Offset2D{.x = ScaleDst(draw_texture_state.dst_x0),
                                    .y = ScaleDst(draw_texture_state.dst_y0)},
                           Offset2D{.x = ScaleDst(draw_texture_state.dst_x1),
                                    .y = ScaleDst(draw_texture_state.dst_y1)}};
    Region2D src_region = {Offset2D{.x = ScaleSrc(draw_texture_state.src_x0),
                                    .y = ScaleSrc(draw_texture_state.src_y0)},
                           Offset2D{.x = ScaleSrc(draw_texture_state.src_x1),
                                    .y = ScaleSrc(draw_texture_state.src_y1)}};
    Extent3D src_size = {static_cast<u32>(ScaleSrc(texture.size.width)),
                         static_cast<u32>(ScaleSrc(texture.size.height)), texture.size.depth};
#if defined(ANDROID) || defined(__ANDROID__)
    static u64 draw_texture_diag_count = 0;
    ++draw_texture_diag_count;
    if (ShouldAndroidRenderDiagLog(draw_texture_diag_count)) {
        const VkExtent2D render_area = framebuffer->RenderArea();
        LOG_INFO(Render_Vulkan,
                 "[nxemu-render-diag] draw_texture#{} src={}x{}x{} srcScaled={} dstScaled={} "
                 "srcRegion=({},{})->({},{}) dstRegion=({},{})->({},{}) fbArea={}x{} fbRescaled={}",
                 draw_texture_diag_count, texture.size.width, texture.size.height,
                 texture.size.depth, src_rescaling, dst_rescaling, src_region.start.x,
                 src_region.start.y, src_region.end.x, src_region.end.y, dst_region.start.x,
                 dst_region.start.y, dst_region.end.x, dst_region.end.y, render_area.width,
                 render_area.height, framebuffer->IsRescaled());
    }
#endif
    blit_image.BlitColor(framebuffer, texture.RenderTarget(), texture.ImageHandle(),
                         sampler->Handle(), dst_region, src_region, src_size);
    texture_cache.MarkRenderTargetsWritten(~0U, false);
}

void RasterizerVulkan::Clear(u32 layer_count) {
    gpu.DebugRecord(Tegra::RenderDebugCounter::ClearRequests);
    FlushWork();
    gpu_memory->FlushCaching();

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              maxwell3d->regs.zpass_pixel_count_enable);

    auto& regs = maxwell3d->regs;
    const bool use_color = regs.clear_surface.R || regs.clear_surface.G || regs.clear_surface.B ||
                           regs.clear_surface.A;
    const bool use_depth = regs.clear_surface.Z;
    const bool use_stencil = regs.clear_surface.S;
    if (!use_color && !use_depth && !use_stencil) {
        return;
    }

    std::scoped_lock lock{texture_cache.mutex};
    // After the clear has opened its render pass
    SCOPE_EXIT {
        texture_cache.MarkRenderTargetsWritten(~0U, true);
    };
    texture_cache.UpdateRenderTargets(true, layer_count);
    const Framebuffer* framebuffer = texture_cache.GetFramebuffer();
    const VkExtent2D render_area = framebuffer->RenderArea();
    const u32 color_write_mask = use_color ? (1U << regs.clear_surface.RT) : 0;
    const u32 color_scratch_mask = framebuffer->ColorScratchMask(color_write_mask);
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = videoSettings.resolution_info.up_scale;
        down_shift = videoSettings.resolution_info.down_shift;
    }
    UpdateViewportsState(regs);

    VkRect2D default_scissor;
    default_scissor.offset.x = 0;
    default_scissor.offset.y = 0;
    default_scissor.extent.width = std::numeric_limits<s32>::max();
    default_scissor.extent.height = std::numeric_limits<s32>::max();

    VkClearRect clear_rect{
        .rect = regs.clear_control.use_scissor ? GetScissorState(regs, 0, up_scale, down_shift)
                                               : default_scissor,
        .baseArrayLayer = regs.clear_surface.layer,
        .layerCount = layer_count,
    };
    if (clear_rect.rect.extent.width == 0 || clear_rect.rect.extent.height == 0) {
        return;
    }
    clear_rect.rect.extent = VkExtent2D{
        .width = std::min(clear_rect.rect.extent.width, render_area.width),
        .height = std::min(clear_rect.rect.extent.height, render_area.height),
    };

    const u32 color_attachment = regs.clear_surface.RT;
    if (use_color && framebuffer->HasAspectColorBit(color_attachment)) {
        const auto format =
            VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[color_attachment].format);
        bool is_integer = IsPixelFormatInteger(format);
        bool is_signed = IsPixelFormatSignedInteger(format);
        size_t int_size = PixelComponentSizeBitsInteger(format);
        VkClearValue clear_value{};
        if (device.GetDriverID() == VK_DRIVER_ID_MOLTENVK || !is_integer) {
            // Clear registers contain raw float/int/uint bits selected by the target format.
            // Copying the words also preserves integer values whose float view is a NaN.
            std::memcpy(clear_value.color.float32, regs.clear_color.data(),
                        regs.clear_color.size() * sizeof(f32));
        } else if (!is_signed) {
            for (size_t i = 0; i < 4; i++) {
                clear_value.color.uint32[i] = static_cast<u32>(
                    static_cast<f32>(static_cast<u64>(int_size) << 1U) * regs.clear_color[i]);
            }
        } else {
            for (size_t i = 0; i < 4; i++) {
                clear_value.color.int32[i] =
                    static_cast<s32>(static_cast<f32>(static_cast<s64>(int_size - 1) << 1) *
                                     (regs.clear_color[i] - 0.5f));
            }
        }

        // MoltenVK's native attachment clears can round large 32-bit integers.
        // The typed shader clear preserves their exact values, including full clears.
        const bool integer_shader_clear =
            device.GetDriverID() == VK_DRIVER_ID_MOLTENVK && is_integer;
        if (regs.clear_surface.R && regs.clear_surface.G && regs.clear_surface.B &&
            regs.clear_surface.A && !integer_shader_clear) {
            scheduler.RequestRenderpass(framebuffer, color_scratch_mask);
            if (VideoCommon::ImageWriters::Enabled()) {
                namespace W = VideoCommon::ImageWriters;
                auto event = W::Stamp(W::Handle(framebuffer->DiagnosticColorImage(color_attachment, color_scratch_mask)),
                                      W::Op::ClearRecorded);
                event.clear_region_known = true;
                event.clear_rect = {
                    static_cast<uint64_t>(static_cast<int64_t>(clear_rect.rect.offset.x)),
                    static_cast<uint64_t>(static_cast<int64_t>(clear_rect.rect.offset.y)),
                    clear_rect.rect.extent.width, clear_rect.rect.extent.height};
                event.clear_layers = {clear_rect.baseArrayLayer, clear_rect.layerCount};
                for (size_t channel = 0; channel < event.aux.size(); ++channel) {
                    uint32_t bits{};
                    std::memcpy(&bits, &clear_value.color.float32[channel], sizeof(bits));
                    event.aux[channel] = bits;
                }
                scheduler.Record([color_attachment, clear_value, clear_rect, event](vk::CommandBuffer cmdbuf) {
                    const VkClearAttachment attachment{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .colorAttachment = color_attachment,
                        .clearValue = clear_value,
                    };
                    cmdbuf.ClearAttachments(attachment, clear_rect);
                    W::Record(event);
                });
            } else {
            scheduler.Record([color_attachment, clear_value, clear_rect](vk::CommandBuffer cmdbuf) {
                const VkClearAttachment attachment{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .colorAttachment = color_attachment,
                    .clearValue = clear_value,
                };
                cmdbuf.ClearAttachments(attachment, clear_rect);
            });
            }
        } else {
            u8 color_mask = static_cast<u8>(regs.clear_surface.R | regs.clear_surface.G << 1 |
                                            regs.clear_surface.B << 2 | regs.clear_surface.A << 3);
            Region2D dst_region = {
                Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
                Offset2D{.x = clear_rect.rect.offset.x +
                              static_cast<s32>(clear_rect.rect.extent.width),
                         .y = clear_rect.rect.offset.y +
                              static_cast<s32>(clear_rect.rect.extent.height)}};
            if (device.GetDriverID() == VK_DRIVER_ID_MOLTENVK) {
                for (u32 layer = 0; layer < clear_rect.layerCount; ++layer) {
                    const Framebuffer* clear_framebuffer = texture_cache.GetClearFramebuffer(
                        color_attachment, clear_rect.baseArrayLayer + layer);
                    blit_image.ClearColor(clear_framebuffer, color_mask, clear_value.color, format,
                                          dst_region);
                }
                // The framebuffer slot storage can grow while creating the helper framebuffer.
                framebuffer = texture_cache.GetFramebuffer();
            } else {
                blit_image.ClearColor(framebuffer, color_mask, regs.clear_color, dst_region);
            }
        }
    }

    if (!use_depth && !use_stencil) {
        return;
    }
    VkImageAspectFlags aspect_flags = 0;
    if (use_depth && framebuffer->HasAspectDepthBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (use_stencil && framebuffer->HasAspectStencilBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (aspect_flags == 0) {
        return;
    }

#if defined(ANDROID) || defined(__ANDROID__)
    static u64 clear_ds_diag_count = 0;
    ++clear_ds_diag_count;
    if (ShouldAndroidRenderDiagLog(clear_ds_diag_count)) {
        LOG_INFO(Render_Vulkan,
                 "[nxemu-render-diag] clear_ds#{} depth={} stencil={} aspects=0x{:x} "
                 "depthValue={:.6f} stencilValue={} stencilMask=0x{:x} rect=({},{} {}x{}) area={}x{} "
                 "layer={} layers={} scissor={}",
                 clear_ds_diag_count, use_depth, use_stencil, aspect_flags, regs.clear_depth,
                 regs.clear_stencil, regs.stencil_front_mask, clear_rect.rect.offset.x,
                 clear_rect.rect.offset.y, clear_rect.rect.extent.width,
                 clear_rect.rect.extent.height, render_area.width, render_area.height,
                 clear_rect.baseArrayLayer, clear_rect.layerCount, regs.clear_control.use_scissor);
    }
#endif

    if (use_stencil && framebuffer->HasAspectStencilBit() && regs.stencil_front_mask != 0xFF &&
        regs.stencil_front_mask != 0) {
        Region2D dst_region = {
            Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
            Offset2D{.x = clear_rect.rect.offset.x + static_cast<s32>(clear_rect.rect.extent.width),
                     .y = clear_rect.rect.offset.y +
                          static_cast<s32>(clear_rect.rect.extent.height)}};
        if (device.GetDriverID() == VK_DRIVER_ID_MOLTENVK) {
            for (u32 layer = 0; layer < clear_rect.layerCount; ++layer) {
                const Framebuffer* clear_framebuffer = texture_cache.GetClearFramebuffer(
                    Maxwell::NumRenderTargets, clear_rect.baseArrayLayer + layer);
                blit_image.ClearDepthStencil(clear_framebuffer, use_depth, regs.clear_depth,
                                             static_cast<u8>(regs.stencil_front_mask),
                                             regs.clear_stencil, regs.stencil_front_func_mask,
                                             dst_region);
            }
        } else {
            blit_image.ClearDepthStencil(
                framebuffer, use_depth, regs.clear_depth, static_cast<u8>(regs.stencil_front_mask),
                regs.clear_stencil, regs.stencil_front_func_mask, dst_region);
        }
    } else {
        // A masked color clear may have selected a color-only helper framebuffer.
        scheduler.RequestRenderpass(framebuffer, color_scratch_mask);
        scheduler.Record([clear_depth = regs.clear_depth, clear_stencil = regs.clear_stencil,
                          clear_rect, aspect_flags](vk::CommandBuffer cmdbuf) {
            VkClearAttachment attachment;
            attachment.aspectMask = aspect_flags;
            attachment.colorAttachment = 0;
            attachment.clearValue.depthStencil.depth = clear_depth;
            attachment.clearValue.depthStencil.stencil = clear_stencil;
            cmdbuf.ClearAttachments(attachment, clear_rect);
        });
    }
}

bool RasterizerVulkan::DispatchNativeCompute() {
    DispatchCompute();
    return native_compute_submitted;
}

void RasterizerVulkan::DispatchCompute() {
    native_compute_submitted = false;
    gpu.DebugRecord(Tegra::RenderDebugCounter::ComputeRequests);
    FlushWork();
    gpu_memory->FlushCaching();

    ComputePipeline* const pipeline{pipeline_cache.CurrentComputePipeline()};
    if (!pipeline) {
        return;
    }
    if (kepler_compute->IsNativeTuring() && !pipeline->WaitForNativeBuild()) return;
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
    gpu.DebugRecord(Tegra::RenderDebugCounter::ComputePipelineReady);
    pipeline->Configure(*kepler_compute, *gpu_memory, scheduler, buffer_cache, texture_cache);
    if (pipeline->WritesImages()) {
        image_write_stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    SCOPE_EXIT {
        buffer_cache.CopySparseWrites();
    };

    const auto& qmd{kepler_compute->launch_description};
    auto indirect_address = kepler_compute->GetIndirectComputeAddress();
    if (indirect_address) {
        // DispatchIndirect
        static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
        // The parameters are only read, what the GPU wrote there still has to reach the guest
        const auto post_op = VideoCommon::ObtainBufferOperation::DoNothing;
        const auto [buffer, offset] =
            buffer_cache.ObtainBuffer(*indirect_address, 12, sync_info, post_op);
        // The dispatch reads its parameters later in this command buffer. An upload reordered
        // before it would replace parameters an earlier shader wrote.
        buffer->MarkUsage(offset, 12);
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([pipeline, indirect_buffer = buffer->Handle(),
                          indirect_offset = offset](vk::CommandBuffer cmdbuf) {
            if (pipeline->HasBuildFailed()) {
                return;
            }
            cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);
            Scheduler::ComputeMemoryBarrier(cmdbuf);
        });
        pipeline->FinishRuntimeImageWrites(scheduler, texture_cache);
        native_compute_submitted = true;
        return;
    }
    const std::array<u32, 3> dim{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([pipeline, dim](vk::CommandBuffer cmdbuf) {
        if (pipeline->HasBuildFailed()) {
            return;
        }
        cmdbuf.Dispatch(dim[0], dim[1], dim[2]);
        Scheduler::ComputeMemoryBarrier(cmdbuf);
    });
    pipeline->FinishRuntimeImageWrites(scheduler, texture_cache);
    native_compute_submitted = true;
}

void RasterizerVulkan::ResetCounter(VideoCommon::QueryType type) {
    if (type != VideoCommon::QueryType::ZPassPixelCount64) {
        LOG_DEBUG(Render_Vulkan, "Unimplemented counter reset={}", type);
        return;
    }
    query_cache.CounterReset(type);
}

void RasterizerVulkan::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                             VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    query_cache.CounterReport(gpu_addr, type, flags, payload, subreport);
}

void RasterizerVulkan::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                 u32 size) {
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void Vulkan::RasterizerVulkan::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}

void RasterizerVulkan::FlushAll() {}

void RasterizerVulkan::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    FlushRegionImpl(addr, size, which, false);
}

void RasterizerVulkan::FlushNativeRegion(DAddr addr, u64 size) {
    FlushRegionImpl(addr, size, VideoCommon::CacheType::All, true);
}

void RasterizerVulkan::FlushRegionImpl(DAddr addr, u64 size, VideoCommon::CacheType which,
                                      bool native_read) {
    if (addr == 0 || size == 0) {
        return;
    }
    // Registered pages retain the existing lock/download/fence path.
    which = device_memory.FilterFlushCaches(addr, size, which);
    if (False(which & (VideoCommon::CacheType::TextureCache |
                       VideoCommon::CacheType::BufferCache |
                       VideoCommon::CacheType::QueryCache))) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size, native_read);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.FlushRegion(addr, size);
    }
}

bool RasterizerVulkan::MustFlushSharedPage(DAddr addr, u64 size) {
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size)) {
            return true;
        }
    }
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.HasUnpublishedReadImage(addr, size);
}

bool RasterizerVulkan::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    which = device_memory.FilterFlushCaches(addr, size, which);
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        if (buffer_cache.MayHaveGpuWrites(addr, size)) {
            std::scoped_lock lock{buffer_cache.mutex};
            if (buffer_cache.IsRegionGpuModified(addr, size)) {
                return true;
            }
        }
    }
    if (!Settings::IsGPULevelHigh()) {
        return false;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(addr, size);
    }
    return false;
}

RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {
    if (True(device_memory.FilterFlushCaches(addr, size, VideoCommon::CacheType::TextureCache))) {
        std::scoped_lock lock{texture_cache.mutex};
        auto area = texture_cache.GetFlushArea(addr, size);
        if (area) {
            return *area;
        }
    }
    RasterizerDownloadArea new_area{
        .startAddress = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),
        .endAddress = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),
        .preemtive = true,
    };
    return new_area;
}

void RasterizerVulkan::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.InvalidateRegion(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::ShaderCache))) {
        pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerVulkan::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            texture_cache.WriteMemory(addr, size);
        }
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            buffer_cache.WriteMemory(addr, size);
        }
    }
    {
        for (const auto& [addr, size] : sequences) {
            query_cache.InvalidateRegion(addr, size);
            pipeline_cache.InvalidateRegion(addr, size);
        }
    }
}

bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {
    if (addr == 0 || size == 0) {
        return false;
    }

    const auto requested = VideoCommon::CacheType::BufferCache | VideoCommon::CacheType::TextureCache;
    const auto caches = device_memory.FilterFlushCaches(addr, size, requested);

    if (True(caches & VideoCommon::CacheType::BufferCache)) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
    }

    if (True(caches & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }

    pipeline_cache.InvalidateRegion(addr, size);
    return false;
}

void RasterizerVulkan::OnCacheInvalidation(DAddr addr, u64 size) {
    if (addr == 0 || size == 0) {
        return;
    }

    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
}

void RasterizerVulkan::InvalidateGPUCache() {
    gpu.InvalidateGPUCache();
}

void RasterizerVulkan::UnmapMemory(DAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.OnCacheInvalidation(addr, size);
}

void RasterizerVulkan::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size, bool preserve_contents) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapGPUMemory(as_id, addr, size, preserve_contents);
    }
}

void RasterizerVulkan::SignalFence(std::function<void()>&& func) {
    fence_manager.SignalQueryFence(std::move(func));
}

void RasterizerVulkan::SignalMemoryOperationFence(std::function<void()>&& func) {
    fence_manager.SignalMemoryOperationFence(std::move(func));
}

void RasterizerVulkan::ShutdownAsyncReadbacks() {
    fence_manager.Shutdown();
}

void RasterizerVulkan::WaitForFence() {
    fence_manager.WaitForFence();
}

void RasterizerVulkan::SyncOperation(std::function<void()>&& func) {
    fence_manager.SyncOperation(std::move(func));
}

void RasterizerVulkan::SignalSyncPoint(u32 value) {
    fence_manager.SignalSyncPoint(value);
}

void RasterizerVulkan::SignalReference() {
    fence_manager.SignalReference();
}

void RasterizerVulkan::ReleaseFences(bool force) {
    fence_manager.WaitPendingFences(force);
}

void RasterizerVulkan::FlushAndInvalidateRegion(DAddr addr, u64 size,
                                                VideoCommon::CacheType which) {
    if (Settings::IsGPULevelExtreme()) {
        FlushRegion(addr, size, which);
    }
    InvalidateRegion(addr, size, which);
}

void RasterizerVulkan::WaitForIdle() {
    // Everything but wait pixel operations. This intentionally includes FRAGMENT_SHADER_BIT because
    // fragment shaders can still write storage buffers.
    VkPipelineStageFlags flags =
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
    }

    query_cache.NotifyWFI();

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([flags](vk::CommandBuffer cmdbuf) {
        // WFI must publish prior writes, not just order execution. In particular, compute
        // output may become a vertex buffer or an indirect draw argument after this point.
        static constexpr VkMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(flags, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, barrier);
    });
    fence_manager.SignalOrdering();
}

void RasterizerVulkan::FragmentBarrier() {
    // Ending the render pass already makes color/depth attachment writes visible.
    scheduler.RequestOutsideRenderPassOperationContext();

    if (image_write_stages == 0) {
        return;
    }

    // Naming the tessellation and geometry stages needs their device features, so use the
    // generic graphics stage for the readers.
    static constexpr VkPipelineStageFlags SHADER_STAGES =
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    static constexpr VkMemoryBarrier IMAGE_WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };

    const VkPipelineStageFlags src_stages = image_write_stages;
    image_write_stages = 0;
    scheduler.Record([src_stages](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(src_stages, SHADER_STAGES, 0, IMAGE_WRITE_BARRIER);
    });
}

void RasterizerVulkan::TiledCacheBarrier() {
    // EndRenderPass publishes previous attachment writes before subsequent guest reads.
    if (device.GetDriverID() == VK_DRIVER_ID_MOLTENVK) {
        scheduler.RequestOutsideRenderPassOperationContext();
    }
    // As in Ryubing, apply the texture barrier when shaders wrote to images. Ending the render
    // pass on every call would split render passes for no reason on other drivers.
    if (image_write_stages != 0) {
        FragmentBarrier();
    }
}

void RasterizerVulkan::FlushCommands() {
    if (draw_counter == 0) {
        return;
    }
    draw_counter = 0;
    scheduler.Flush();
}

RasterizerVulkan::FrameDraws RasterizerVulkan::TakeFrameDraws() {
    bool new_render_targets{};
    {
        std::scoped_lock lock{texture_cache.mutex};
        new_render_targets = texture_cache.TakeNewRenderTargets();
    }
    return {std::exchange(frame_draws, 0), std::exchange(frame_skipped_draws, 0),
            new_render_targets};
}

void RasterizerVulkan::TickFrame() {
    pipeline_cache.RecordDiagnosticFrame();
    draw_counter = 0;
    fence_manager.TickFrame();
    staging_pool.TickFrame();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.TickFrame();
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.TickFrame();
    }
}

bool RasterizerVulkan::AccelerateConditionalRendering() {
    gpu_memory->FlushCaching();
    if (query_cache.AccelerateHostConditionalRendering()) {
        return true;
    }
    // Without host conditional rendering, reading a result the GPU has not produced yet
    // waits for the host GPU. Fast readback draws instead: an occluded draw is still correct.
    const auto& regs = maxwell3d->regs;
    if (Settings::IsGpuReadbackExact() || device.IsExtConditionalRendering() ||
        regs.render_enable_override != Maxwell::RenderEnable::Override::UseRenderEnable) {
        return false;
    }
    const auto mode = regs.render_enable.mode;
    if (mode != Maxwell::RenderEnable::Mode::Conditional &&
        mode != Maxwell::RenderEnable::Mode::IfEqual &&
        mode != Maxwell::RenderEnable::Mode::IfNotEqual) {
        return false;
    }
    const GPUVAddr address = regs.render_enable.Address();
    constexpr size_t size = sizeof(Maxwell::ReportSemaphore::Compare);
    const auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    return (cpu_addr && query_cache.IsRegionGpuModified(*cpu_addr, size)) ||
           gpu_memory->IsMemoryDirty(address, size, VideoCommon::CacheType::BufferCache);
}

bool RasterizerVulkan::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                             const Tegra::Engines::Fermi2D::Surface& dst,
                                             const Tegra::Engines::Fermi2D::Config& copy_config) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, copy_config);
}

Tegra::Engines::AccelerateDMAInterface& RasterizerVulkan::AccessAccelerateDMA() {
    return accelerate_dma;
}

void RasterizerVulkan::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                                std::span<const u8> memory) {
    auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    if (!cpu_addr) [[unlikely]] {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
        return;
    }
    gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size);
    {
        std::unique_lock<std::recursive_mutex> lock{buffer_cache.mutex};
        if (!buffer_cache.InlineMemory(*cpu_addr, copy_size, memory)) {
            buffer_cache.WriteMemory(*cpu_addr, copy_size);
        }
    }
    {
        std::scoped_lock lock_texture{texture_cache.mutex};
        texture_cache.WriteMemory(*cpu_addr, copy_size);
    }
    pipeline_cache.InvalidateRegion(*cpu_addr, copy_size);
    query_cache.InvalidateRegion(*cpu_addr, copy_size);
}

std::optional<FramebufferTextureInfo> RasterizerVulkan::AccelerateDisplay(
    const Tegra::FramebufferConfig& config, DAddr framebuffer_addr, u32 pixel_stride) {
    if (!framebuffer_addr) {
        return {};
    }
#if defined(ANDROID) || defined(__ANDROID__)
    // Diagnostic/compatibility switch: bypass the GPU texture-cache backed display path and fall
    // back to raw framebuffer upload in present::Layer.  Some Android Vulkan drivers are very
    // sensitive to stale render-target/layout/cache state; this lets us distinguish a bad final
    // composite from a bad accelerated framebuffer image.
    if (ReadAndroidBoolProperty("debug.nxemu.disable_accelerated_display", false)) {
        return {};
    }
#endif
    const auto debug_begin = std::chrono::steady_clock::now();
    bool debug_accelerated{};
    SCOPE_EXIT {
        if (config.native_tiling && VideoCommon::DebugDocument::Enabled()) {
            std::ostringstream doc;
            doc << "{\"address\":" << framebuffer_addr << ",\"accelerated\":" << (debug_accelerated ? "true" : "false")
                << ",\"width\":" << config.width << ",\"height\":" << config.height << ",\"stride\":" << pixel_stride
                << ",\"lookup_flush_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-debug_begin).count()
                << ",\"accuracy\":" << static_cast<u32>(videoSettings.gpu_accuracy)
                << ",\"resolution\":" << static_cast<u32>(videoSettings.resolution_setup)
                << ",\"async_gpu\":" << (videoSettings.use_asynchronous_gpu_emulation ? "true" : "false")
                << ",\"async_shaders\":" << (videoSettings.use_asynchronous_shaders ? "true" : "false")
                << ",\"reactive_flushing\":" << (videoSettings.use_reactive_flushing ? "true" : "false")
                << ",\"astc_recompression\":" << static_cast<u32>(videoSettings.astc_recompression) << "}";
            VideoCommon::DebugDocument::Publish("display", doc.str());
        }
    };
    std::unique_lock lock{texture_cache.mutex};
    const auto [image_view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    if (!image_view) {
        lock.unlock();
        if (config.native_tiling) {
            const auto size = Tegra::Texture::CalculateSize(true, 4, config.stride, config.height, 1,
                                                           config.block_height_log2, 0);
            gpu.DebugRecord(Tegra::RenderDebugCounter::DisplayFallback);
            gpu.DebugRecord(Tegra::RenderDebugCounter::DisplayFallbackBytes, size);
            const auto fallback_start = std::chrono::steady_clock::now();
            FlushRegionImpl(framebuffer_addr, size, VideoCommon::CacheType::All, false);
            gpu.DebugRecord(Tegra::RenderDebugCounter::DisplayFallbackUs,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - fallback_start).count());
            static u32 fallback_count{};
            if (fallback_count++ < 8)
                LOG_INFO(HW_GPU, "NX2_DISPLAY accelerated=false address={:#x} bytes={} path=cpu_readback", framebuffer_addr, size);
        }
        return {};
    }
    debug_accelerated = true;
    if (config.native_tiling) {
        gpu.DebugRecord(Tegra::RenderDebugCounter::DisplayAccelerated);
        static u32 accelerated_count{};
        if (accelerated_count++ < 8)
            LOG_INFO(HW_GPU, "NX2_DISPLAY accelerated=true address={:#x} size={}x{} path=texture_cache",
                     framebuffer_addr, image_view->size.width, image_view->size.height);
    }
    query_cache.NotifySegment(false);

    const auto& resolution = videoSettings.resolution_info;

    FramebufferTextureInfo info{};
    info.image = image_view->ImageHandle();
    info.image_view = image_view->Handle(Shader::TextureType::Color2D);
    info.width = image_view->size.width;
    info.height = image_view->size.height;
    info.scaled_width = scaled ? resolution.ScaleUp(info.width) : info.width;
    info.scaled_height = scaled ? resolution.ScaleUp(info.height) : info.height;
    if (SourcePixelProbe::Active()) {
        try {
            const auto& range = image_view->range;
            const bool dimension_supported =
                (image_view->type == VideoCommon::ImageViewType::e2D ||
                 image_view->type == VideoCommon::ImageViewType::e2DArray) &&
                False(image_view->flags & VideoCommon::ImageViewFlagBits::Slice) &&
                range.base.level >= 0 && range.base.layer >= 0 &&
                !(scaled && range.base.level != 0);
            const VkFormat view_format = MaxwellToVK::SurfaceFormat(
                device, FormatType::Optimal, true, image_view->format).format;
            const auto components = image_view->Color2DComponents();
            SourcePixelProbe::Record(
                scheduler, memory_allocator, info.image, info.image_view, view_format,
                info.scaled_width, info.scaled_height, static_cast<uint32_t>(range.base.level),
                static_cast<uint32_t>(range.base.layer), image_view->Samples(),
                image_view->HasIdentitySwizzle(), dimension_supported, range.extent.levels == 1,
                {static_cast<uint32_t>(components.r), static_cast<uint32_t>(components.g),
                 static_cast<uint32_t>(components.b), static_cast<uint32_t>(components.a)},
                image_view->Color2DComponentsKnown());
        } catch (const std::exception&) {
            SourcePixelProbe::Failed("std::exception in source probe");
        } catch (...) {
            SourcePixelProbe::Failed("unknown source probe exception");
        }
    }
    return info;
}

void RasterizerVulkan::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                         const VideoCore::DiskResourceLoadCallback& callback, std::string_view content_identity, [[maybe_unused]] std::string_view game_path) {
    pipeline_cache.LoadDiskResources(title_id, stop_loading, callback, content_identity, game_path);
}

void RasterizerVulkan::FlushWork() {
#ifdef ANDROID
    static constexpr u32 DRAWS_TO_DISPATCH = 1024;
#else
    static constexpr u32 DRAWS_TO_DISPATCH = 4096;
#endif // ANDROID

    static constexpr u32 DRAWS_TO_SUBMIT_BETWEEN_PASSES = 64;

    // Submit the passes already recorded when the guest moves to other render targets, so the
    // host GPU works on them while the next ones are recorded. No render pass is split.
    if (draw_counter >= DRAWS_TO_SUBMIT_BETWEEN_PASSES &&
        maxwell3d->dirty.flags[VideoCommon::Dirty::RenderTargets]) {
        scheduler.Flush();
        draw_counter = 0;
    }

    // Only check multiples of 8 draws
    static_assert(DRAWS_TO_DISPATCH % 8 == 0);
    if ((++draw_counter & 7) != 7) {
        return;
    }
    if (draw_counter < DRAWS_TO_DISPATCH) {
        // Batch opportunistic worker handoffs. Explicit guest flushes/waits
        // and the periodic driver submission below retain their original cadence.
        if ((draw_counter & 31) == 31) {
            scheduler.DispatchWork();
        }
        return;
    }
    // Otherwise (every certain number of draws) flush execution.
    // This submits commands to the Vulkan driver.
    scheduler.Flush();
    draw_counter = 0;
}

AccelerateDMA::AccelerateDMA(BufferCache& buffer_cache_, TextureCache& texture_cache_,
                             Scheduler& scheduler_)
    : buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, scheduler{scheduler_} {}

bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMAClear(src_address, amount, value);
}

bool AccelerateDMA::BufferCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMACopy(src_address, dest_address, amount);
}

template <bool IS_IMAGE_UPLOAD>
bool AccelerateDMA::DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                                       const Tegra::DMA::BufferOperand& buffer_operand,
                                       const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_IMAGE_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) {
        return false;
    }
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto post_op = IS_IMAGE_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                                         : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(buffer_operand.address, buffer_size, sync_info, post_op);

    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        copy_info, buffer_operand, image_operand, image_id, IS_IMAGE_UPLOAD);
    const std::span copy_span{&copy, 1};

    if constexpr (IS_IMAGE_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copy_span);
    } else {
        if (offset % BytesPerBlock(image->info.format)) {
            return false;
        }
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copy_span,
                                              buffer_operand.address, buffer_size);
    }
    return true;
}

bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::ImageOperand& image_operand,
                                  const Tegra::DMA::BufferOperand& buffer_operand) {
    return DmaBufferImageCopy<false>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::BufferOperand& buffer_operand,
                                  const Tegra::DMA::ImageOperand& image_operand) {
    return DmaBufferImageCopy<true>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::ImageToImage(Tegra::MemoryManager& gpu_memory,
                                 const Tegra::DMA::ImageCopy& copy_info,
                                 const Tegra::DMA::ImageOperand& src,
                                 const Tegra::DMA::ImageOperand& dst) {
    const size_t size = Tegra::Engines::DmaImageCopySize(copy_info, src, dst);
    if (size == 0) {
        return false;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto cpu_src = gpu_memory.GpuToCpuAddress(src.address);
    const auto cpu_dst = gpu_memory.GpuToCpuAddress(dst.address);
    if (!cpu_src || !cpu_dst || gpu_memory.MaxContinuousRange(src.address, size) != size ||
        gpu_memory.MaxContinuousRange(dst.address, size) != size ||
        buffer_cache.IsRegionRegistered(*cpu_src, size) ||
        buffer_cache.IsRegionRegistered(*cpu_dst, size)) {
        return false;
    }
    return texture_cache.DmaImageToImage(copy_info, src, dst);
}

void RasterizerVulkan::UpdateDynamicStates() {
    auto& regs = maxwell3d->regs;
    UpdateViewportsState(regs);
    UpdateScissorsState(regs);
    UpdateDepthBias(regs);
    UpdateBlendConstants(regs);
    UpdateDepthBounds(regs);
    UpdateStencilFaces(regs);
    UpdateLineWidth(regs);
    if (device.IsExtExtendedDynamicStateSupported()) {
        UpdateCullMode(regs);
        UpdateDepthCompareOp(regs);
        UpdateFrontFace(regs);
        UpdateStencilOp(regs);

        if (state_tracker.TouchStateEnable()) {
            UpdateDepthBoundsTestEnable(regs);
            UpdateDepthTestEnable(regs);
            UpdateDepthWriteEnable(regs);
            UpdateStencilTestEnable(regs);
            if (device.IsExtExtendedDynamicState2Supported()) {
                UpdatePrimitiveRestartEnable(regs);
                UpdateRasterizerDiscardEnable(regs);
            }
            if (device.IsExtExtendedDynamicState3EnablesSupported()) {
                UpdateLogicOpEnable(regs);
                UpdateDepthClampEnable(regs);
            }
        }
        // Topology can select a different polygon-offset enable without writing any
        // of the StateEnable registers. Check it even when that group is clean.
        if (device.IsExtExtendedDynamicState2Supported()) {
            UpdateDepthBiasEnable(regs);
        }
        if (device.IsExtExtendedDynamicState2ExtrasSupported()) {
            UpdateLogicOp(regs);
        }
        if (device.IsExtExtendedDynamicState3BlendingSupported()) {
            UpdateBlending(regs);
        }
    }
    if (has_dynamic_vertex_input) {
        UpdateVertexInput(regs);
    }
}

void RasterizerVulkan::HandleTransformFeedback() {
    const auto& regs = maxwell3d->regs;
    if (!device.IsExtTransformFeedbackSupported()) {
        // The pipelines write what transform feedback captures themselves
        buffer_cache.runtime.SetEmulatedTransformFeedbackEnabled(
            regs.transform_feedback_enabled != 0);
        return;
    }
    query_cache.CounterEnable(VideoCommon::QueryType::StreamingByteCount,
                              regs.transform_feedback_enabled);
    if (regs.transform_feedback_enabled != 0) {
        UNIMPLEMENTED_IF(regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
                         regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation));
    }
}

void RasterizerVulkan::UpdateViewportsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    const float depth_offset = EmulatedDepthCompareOffset(device, regs);
    if (depth_compare_offset != depth_offset) {
        depth_compare_offset = depth_offset;
        state_tracker.InvalidateViewports();
    }
    if (!state_tracker.TouchViewports()) {
        return;
    }
    maxwell3d->dirty.flags[Dirty::Scissors] = true;
    if (!regs.viewport_scale_offset_enabled) {
        const float scale = texture_cache.IsRescaling() ? videoSettings.resolution_info.up_factor : 1.0f;
        const VkViewport viewport = SurfaceClipViewport(
            regs.surface_clip.x, regs.surface_clip.y, regs.surface_clip.width, regs.surface_clip.height,
            regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft, scale);
        scheduler.Record([this, viewport](vk::CommandBuffer cmdbuf) {
            const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkViewport, Maxwell::NumViewports> viewport_list{};
            viewport_list.fill(viewport);
            const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
            cmdbuf.SetViewport(0, viewports);
        });
        return;
    }
    const bool is_rescaling{texture_cache.IsRescaling()};
    const float scale = is_rescaling ? videoSettings.resolution_info.up_factor : 1.0f;
    const auto viewport = [&](size_t index) {
        return GetViewportState(device, regs, index, scale, depth_offset);
    };
    const std::array viewport_list{
        viewport(0),  viewport(1),  viewport(2),  viewport(3),  viewport(4),  viewport(5),
        viewport(6),  viewport(7),  viewport(8),  viewport(9),  viewport(10), viewport(11),
        viewport(12), viewport(13), viewport(14), viewport(15),
    };
    scheduler.Record([this, viewport_list](vk::CommandBuffer cmdbuf) {
        const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
        cmdbuf.SetViewport(0, viewports);
    });
}

void RasterizerVulkan::UpdateScissorsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchScissors()) {
        return;
    }
    if (!regs.viewport_scale_offset_enabled) {
        const bool scaled = texture_cache.IsRescaling();
        const VkRect2D scissor = SurfaceClipScissor(
            regs.surface_clip.x, regs.surface_clip.y, regs.surface_clip.width, regs.surface_clip.height,
            scaled ? videoSettings.resolution_info.up_scale : 1U,
            scaled ? videoSettings.resolution_info.down_shift : 0U);
        scheduler.Record([this, scissor](vk::CommandBuffer cmdbuf) {
            const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkRect2D, Maxwell::NumViewports> scissor_list{};
            scissor_list.fill(scissor);
            const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
            cmdbuf.SetScissor(0, scissors);
        });
        return;
    }
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = videoSettings.resolution_info.up_scale;
        down_shift = videoSettings.resolution_info.down_shift;
    }
    const std::array scissor_list{
        GetScissorState(regs, 0, up_scale, down_shift),
        GetScissorState(regs, 1, up_scale, down_shift),
        GetScissorState(regs, 2, up_scale, down_shift),
        GetScissorState(regs, 3, up_scale, down_shift),
        GetScissorState(regs, 4, up_scale, down_shift),
        GetScissorState(regs, 5, up_scale, down_shift),
        GetScissorState(regs, 6, up_scale, down_shift),
        GetScissorState(regs, 7, up_scale, down_shift),
        GetScissorState(regs, 8, up_scale, down_shift),
        GetScissorState(regs, 9, up_scale, down_shift),
        GetScissorState(regs, 10, up_scale, down_shift),
        GetScissorState(regs, 11, up_scale, down_shift),
        GetScissorState(regs, 12, up_scale, down_shift),
        GetScissorState(regs, 13, up_scale, down_shift),
        GetScissorState(regs, 14, up_scale, down_shift),
        GetScissorState(regs, 15, up_scale, down_shift),
    };
    scheduler.Record([this, scissor_list](vk::CommandBuffer cmdbuf) {
        const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
        cmdbuf.SetScissor(0, scissors);
    });
}

void RasterizerVulkan::UpdateDepthBias(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBias()) {
        return;
    }
    float units = regs.depth_bias / 2.0f;
    const bool is_d24 = regs.zeta.format == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
                        regs.zeta.format == Tegra::DepthFormat::X8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::S8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::V8Z24_UNORM;
    if (is_d24 && !device.SupportsD24DepthBuffer() && program_id == 0x1006A800016E000ULL) {
        // Only activate this in Super Smash Brothers Ultimate
        // the base formulas can be obtained from here:
        //   https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias
        const double rescale_factor =
            static_cast<double>(1ULL << (32 - 24)) / (static_cast<double>(0x1.ep+127));
        units = static_cast<float>(static_cast<double>(units) * rescale_factor);
    }
    scheduler.Record([constant = units, clamp = regs.depth_bias_clamp,
                      factor = regs.slope_scale_depth_bias](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthBias(constant, clamp, factor);
    });
}

void RasterizerVulkan::UpdateBlendConstants(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchBlendConstants()) {
        return;
    }
    const std::array blend_color = {regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                                    regs.blend_color.a};
    scheduler.Record(
        [blend_color](vk::CommandBuffer cmdbuf) { cmdbuf.SetBlendConstants(blend_color.data()); });
}

void RasterizerVulkan::UpdateDepthBounds(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBounds() || !device.CanUseDepthBoundsDynamicState()) {
        return;
    }
    scheduler.Record([min = regs.depth_bounds[0], max = regs.depth_bounds[1]](
                         vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBounds(min, max); });
}

void RasterizerVulkan::UpdateStencilFaces(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilProperties()) {
        return;
    }
    bool update_references = state_tracker.TouchStencilReference();
    bool update_write_mask = state_tracker.TouchStencilWriteMask();
    bool update_compare_masks = state_tracker.TouchStencilCompare();
    if (state_tracker.TouchStencilSide(regs.stencil_two_side_enable != 0)) {
        update_references = true;
        update_write_mask = true;
        update_compare_masks = true;
    }
    if (update_references) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_back_ref);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_front_ref);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_ref = regs.stencil_front_ref, back_ref = regs.stencil_back_ref,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_ref != back_ref;
                // Front face
                cmdbuf.SetStencilReference(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_ref);
                if (set_back) {
                    cmdbuf.SetStencilReference(VK_STENCIL_FACE_BACK_BIT, back_ref);
                }
            });
        }();
    }
    if (update_write_mask) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_back_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_front_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_write_mask = regs.stencil_front_mask,
                              back_write_mask = regs.stencil_back_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_write_mask != back_write_mask;
                // Front face
                cmdbuf.SetStencilWriteMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_write_mask);
                if (set_back) {
                    cmdbuf.SetStencilWriteMask(VK_STENCIL_FACE_BACK_BIT, back_write_mask);
                }
            });
        }();
    }
    if (update_compare_masks) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_back_func_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_front_func_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_test_mask = regs.stencil_front_func_mask,
                              back_test_mask = regs.stencil_back_func_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_test_mask != back_test_mask;
                // Front face
                cmdbuf.SetStencilCompareMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                      : VK_STENCIL_FACE_FRONT_AND_BACK,
                                             front_test_mask);
                if (set_back) {
                    cmdbuf.SetStencilCompareMask(VK_STENCIL_FACE_BACK_BIT, back_test_mask);
                }
            });
        }();
    }
    state_tracker.ClearStencilReset();
}

void RasterizerVulkan::UpdateLineWidth(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLineWidth()) {
        return;
    }
    const float width =
        regs.line_anti_alias_enable ? regs.line_width_smooth : regs.line_width_aliased;
    scheduler.Record([width](vk::CommandBuffer cmdbuf) { cmdbuf.SetLineWidth(width); });
}

void RasterizerVulkan::UpdateCullMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchCullMode()) {
        return;
    }
    scheduler.Record([enabled = regs.gl_cull_test_enabled,
                      cull_face = regs.gl_cull_face](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetCullModeEXT(enabled ? MaxwellToVK::CullFace(cull_face) : VK_CULL_MODE_NONE);
    });
}

void RasterizerVulkan::UpdateDepthBoundsTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBoundsTestEnable() || !device.CanUseDepthBoundsDynamicState()) {
        return;
    }
    bool enabled = regs.depth_bounds_enable;
    if (enabled && !device.IsDepthBoundsSupported()) {
        LOG_WARNING(Render_Vulkan, "Depth bounds is enabled but not supported");
        enabled = false;
    }
    scheduler.Record([enable = enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthBoundsTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_test_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthWriteEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_write_enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthWriteEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdatePrimitiveRestartEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchPrimitiveRestartEnable()) {
        return;
    }
    scheduler.Record([enable = regs.primitive_restart.enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetPrimitiveRestartEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateRasterizerDiscardEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchRasterizerDiscardEnable()) {
        return;
    }
    scheduler.Record([disable = regs.rasterize_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetRasterizerDiscardEnableEXT(disable == 0);
    });
}

void RasterizerVulkan::UpdateDepthBiasEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    // The guest has separate point, line and polygon enables. Changing topology
    // selects a different enable even when none of those registers were written.
    const auto topology = maxwell3d->draw_manager->GetDrawState().topology;
    const bool topology_changed = state_tracker.ChangePrimitiveTopology(topology);
    if (!state_tracker.TouchDepthBiasEnable() && !topology_changed) {
        return;
    }
    constexpr size_t POINT = 0;
    constexpr size_t LINE = 1;
    constexpr size_t POLYGON = 2;
    static constexpr std::array POLYGON_OFFSET_ENABLE_LUT = {
        POINT,   // Points
        LINE,    // Lines
        LINE,    // LineLoop
        LINE,    // LineStrip
        POLYGON, // Triangles
        POLYGON, // TriangleStrip
        POLYGON, // TriangleFan
        POLYGON, // Quads
        POLYGON, // QuadStrip
        POLYGON, // Polygon
        LINE,    // LinesAdjacency
        LINE,    // LineStripAdjacency
        POLYGON, // TrianglesAdjacency
        POLYGON, // TriangleStripAdjacency
        POLYGON, // Patches
    };
    const std::array enabled_lut{
        regs.polygon_offset_point_enable,
        regs.polygon_offset_line_enable,
        regs.polygon_offset_fill_enable,
    };
    const u32 topology_index = static_cast<u32>(topology);
    const u32 enable = enabled_lut[POLYGON_OFFSET_ENABLE_LUT[topology_index]];
    scheduler.Record(
        [enable](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBiasEnableEXT(enable != 0); });
}

void RasterizerVulkan::UpdateLogicOpEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOpEnable()) {
        return;
    }
    scheduler.Record([enable = regs.logic_op.enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLogicOpEnableEXT(enable != 0);
    });
}

void RasterizerVulkan::UpdateDepthClampEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthClampEnable()) {
        return;
    }
    bool is_enabled = !(regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::Passthrough ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumXYZ ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumZ);
    scheduler.Record(
        [is_enabled](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthClampEnableEXT(is_enabled); });
}

void RasterizerVulkan::UpdateDepthCompareOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthCompareOp()) {
        return;
    }
    scheduler.Record([func = regs.depth_test_func](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthCompareOpEXT(MaxwellToVK::ComparisonOp(func));
    });
}

void RasterizerVulkan::UpdateFrontFace(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchFrontFace()) {
        return;
    }

    VkFrontFace front_face = MaxwellToVK::FrontFace(regs.gl_front_face);
    if (regs.window_origin.flip_y != 0) {
        front_face = front_face == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                           : VK_FRONT_FACE_CLOCKWISE;
    }
    scheduler.Record(
        [front_face](vk::CommandBuffer cmdbuf) { cmdbuf.SetFrontFaceEXT(front_face); });
}

void RasterizerVulkan::UpdateStencilOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilOp()) {
        return;
    }
    const Maxwell::StencilOp::Op fail = regs.stencil_front_op.fail;
    const Maxwell::StencilOp::Op zfail = regs.stencil_front_op.zfail;
    const Maxwell::StencilOp::Op zpass = regs.stencil_front_op.zpass;
    const Maxwell::ComparisonOp compare = regs.stencil_front_op.func;
    if (regs.stencil_two_side_enable) {
        // Separate stencil op per face
        const Maxwell::StencilOp::Op back_fail = regs.stencil_back_op.fail;
        const Maxwell::StencilOp::Op back_zfail = regs.stencil_back_op.zfail;
        const Maxwell::StencilOp::Op back_zpass = regs.stencil_back_op.zpass;
        const Maxwell::ComparisonOp back_compare = regs.stencil_back_op.func;
        scheduler.Record([fail, zfail, zpass, compare, back_fail, back_zfail, back_zpass,
                          back_compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_BIT, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_BACK_BIT, MaxwellToVK::StencilOp(back_fail),
                                   MaxwellToVK::StencilOp(back_zpass),
                                   MaxwellToVK::StencilOp(back_zfail),
                                   MaxwellToVK::ComparisonOp(back_compare));
        });
    } else {
        // Front face defines the stencil op of both faces
        scheduler.Record([fail, zfail, zpass, compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_AND_BACK, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
        });
    }
}

void RasterizerVulkan::UpdateLogicOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOp()) {
        return;
    }
    const auto op_value = static_cast<u32>(regs.logic_op.op);
    auto op = op_value >= 0x1500 && op_value < 0x1510 ? static_cast<VkLogicOp>(op_value - 0x1500)
                                                      : VK_LOGIC_OP_NO_OP;
    scheduler.Record([op](vk::CommandBuffer cmdbuf) { cmdbuf.SetLogicOpEXT(op); });
}

void RasterizerVulkan::UpdateBlending(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchBlending()) {
        return;
    }

    if (state_tracker.TouchColorMask()) {
        std::array<VkColorComponentFlags, Maxwell::NumRenderTargets> setup_masks{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
            auto& current = setup_masks[index];
            if (mask.R) {
                current |= VK_COLOR_COMPONENT_R_BIT;
            }
            if (mask.G) {
                current |= VK_COLOR_COMPONENT_G_BIT;
            }
            if (mask.B) {
                current |= VK_COLOR_COMPONENT_B_BIT;
            }
            if (mask.A) {
                current |= VK_COLOR_COMPONENT_A_BIT;
            }
        }
        scheduler.Record([setup_masks](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorWriteMaskEXT(0, setup_masks);
        });
#if defined(ANDROID) || defined(__ANDROID__)
        static u64 blend_mask_diag_count = 0;
        ++blend_mask_diag_count;
        if (ShouldAndroidRenderDiagLog(blend_mask_diag_count)) {
            LOG_INFO(Render_Vulkan,
                     "[nxemu-render-diag] blend_mask#{} common={} masks=[0x{:x},0x{:x},0x{:x},"
                     "0x{:x},0x{:x},0x{:x},0x{:x},0x{:x}]",
                     blend_mask_diag_count, regs.color_mask_common,
                     static_cast<u32>(setup_masks[0]), static_cast<u32>(setup_masks[1]),
                     static_cast<u32>(setup_masks[2]), static_cast<u32>(setup_masks[3]),
                     static_cast<u32>(setup_masks[4]), static_cast<u32>(setup_masks[5]),
                     static_cast<u32>(setup_masks[6]), static_cast<u32>(setup_masks[7]));
        }
#endif
    }

    if (state_tracker.TouchBlendEnable()) {
        std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
            bool enable = regs.blend.enable[index] != 0;
            const auto rt_format = regs.rt[index].format;
            // Blending is not defined for integer formats.
            if (enable && rt_format != Tegra::RenderTargetFormat::NONE) {
                const auto format =
                    VideoCore::Surface::PixelFormatFromRenderTargetFormat(rt_format);
                enable = !IsPixelFormatInteger(format);
            }
            setup_enables[index] = enable ? VK_TRUE : VK_FALSE;
        }
        scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEnableEXT(0, setup_enables);
        });
#if defined(ANDROID) || defined(__ANDROID__)
        static u64 blend_enable_diag_count = 0;
        ++blend_enable_diag_count;
        if (ShouldAndroidRenderDiagLog(blend_enable_diag_count)) {
            u32 enable_mask = 0;
            u32 integer_mask = 0;
            for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
                if (setup_enables[index] != VK_FALSE) {
                    enable_mask |= 1u << index;
                }
                if (regs.rt[index].format != Tegra::RenderTargetFormat::NONE) {
                    const auto format =
                        VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[index].format);
                    if (IsPixelFormatInteger(format)) {
                        integer_mask |= 1u << index;
                    }
                }
            }
            LOG_INFO(Render_Vulkan,
                     "[nxemu-render-diag] blend_enable#{} enableMask=0x{:x} integerRtMask=0x{:x} "
                     "guestEnable=[{},{},{},{},{},{},{},{}] rtFormat=[{},{},{},{},{},{},{},{}]",
                     blend_enable_diag_count, enable_mask, integer_mask, regs.blend.enable[0],
                     regs.blend.enable[1], regs.blend.enable[2], regs.blend.enable[3],
                     regs.blend.enable[4], regs.blend.enable[5], regs.blend.enable[6],
                     regs.blend.enable[7], static_cast<u32>(regs.rt[0].format),
                     static_cast<u32>(regs.rt[1].format), static_cast<u32>(regs.rt[2].format),
                     static_cast<u32>(regs.rt[3].format), static_cast<u32>(regs.rt[4].format),
                     static_cast<u32>(regs.rt[5].format), static_cast<u32>(regs.rt[6].format),
                     static_cast<u32>(regs.rt[7].format));
        }
#endif
    }

    if (state_tracker.TouchBlendEquations()) {
        std::array<VkColorBlendEquationEXT, Maxwell::NumRenderTargets> setup_blends{};
        const auto blend_setup = [&](auto& host_blend, const auto& guest_blend) {
            host_blend.srcColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_source);
            host_blend.dstColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_dest);
            host_blend.colorBlendOp = MaxwellToVK::BlendEquation(guest_blend.color_op);
            host_blend.srcAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_source);
            host_blend.dstAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_dest);
            host_blend.alphaBlendOp = MaxwellToVK::BlendEquation(guest_blend.alpha_op);
        };

        if (!regs.blend_per_target_enabled) {
            if (regs.iterated_blend.enable && UseSquashedIteratedBlend()) {
                setup_blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].colorBlendOp = VK_BLEND_OP_ADD;
                setup_blends[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                setup_blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                setup_blends[0].alphaBlendOp = VK_BLEND_OP_ADD;
            } else {
                blend_setup(setup_blends[0], regs.blend);
            }
            for (size_t index = 1; index < Maxwell::NumRenderTargets; index++) {
                setup_blends[index] = setup_blends[0];
            }
        } else {
            for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
                blend_setup(setup_blends[index], regs.blend_per_target[index]);
            }
        }
        scheduler.Record([setup_blends](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEquationEXT(0, setup_blends);
        });
#if defined(ANDROID) || defined(__ANDROID__)
        static u64 blend_eq_diag_count = 0;
        ++blend_eq_diag_count;
        if (ShouldAndroidRenderDiagLog(blend_eq_diag_count)) {
            const auto& b0 = setup_blends[0];
            LOG_INFO(Render_Vulkan,
                     "[nxemu-render-diag] blend_eq#{} perTarget={} iterated={} squashed={} "
                     "rt0(srcC={},dstC={},opC={},srcA={},dstA={},opA={}) guestCommon(srcC={},dstC={},opC={},srcA={},dstA={},opA={})",
                     blend_eq_diag_count, regs.blend_per_target_enabled, regs.iterated_blend.enable,
                     regs.iterated_blend.enable && UseSquashedIteratedBlend(),
                     static_cast<u32>(b0.srcColorBlendFactor),
                     static_cast<u32>(b0.dstColorBlendFactor), static_cast<u32>(b0.colorBlendOp),
                     static_cast<u32>(b0.srcAlphaBlendFactor),
                     static_cast<u32>(b0.dstAlphaBlendFactor), static_cast<u32>(b0.alphaBlendOp),
                     static_cast<u32>(regs.blend.color_source),
                     static_cast<u32>(regs.blend.color_dest), static_cast<u32>(regs.blend.color_op),
                     static_cast<u32>(regs.blend.alpha_source),
                     static_cast<u32>(regs.blend.alpha_dest), static_cast<u32>(regs.blend.alpha_op));
        }
#endif
    }
}

void RasterizerVulkan::UpdateStencilTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.stencil_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetStencilTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateVertexInput(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!maxwell3d->dirty.flags[Dirty::VertexInput]) {
        return;
    }
    RefreshVertexInput(regs);
}

void RasterizerVulkan::RefreshVertexInput(Tegra::Engines::Maxwell3D::Regs& regs) {
    auto& dirty{maxwell3d->dirty.flags};
    dirty[Dirty::VertexInput] = false;

    boost::container::static_vector<VkVertexInputBindingDescription2EXT, 32> bindings;
    boost::container::static_vector<VkVertexInputAttributeDescription2EXT, 32> attributes;

    // vkCmdSetVertexInputEXT replaces the complete vertex input state. In particular, attributes
    // absent from the command become undefined, even when only one guest attribute was dirty.
    const u32 max_attributes = Maxwell::NumVertexAttributes;
    const u32 max_bindings = Maxwell::NumVertexArrays;
    static_assert(max_attributes == VertexInputMapping::NUM_SLOTS &&
                  max_bindings == VertexInputMapping::NUM_SLOTS);
    u32 used_bindings{};
    for (u32 index = 0; index < max_attributes; ++index) {
        const Maxwell::VertexAttribute attribute{regs.vertex_attrib_format[index]};
        const u32 binding{attribute.buffer};
        if (attribute.constant || binding >= max_bindings) {
            continue;
        }
        attributes.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .location = index,
            .binding = binding,
            .format = MaxwellToVK::VertexFormat(device, attribute.type, attribute.size),
            .offset = attribute.offset,
        });
        used_bindings |= u32{1} << binding;
    }
    while (used_bindings != 0) {
        const u32 binding = static_cast<u32>(std::countr_zero(used_bindings));
        used_bindings &= used_bindings - 1;
        const auto& input_binding{regs.vertex_streams[binding]};
        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding)};
        bindings.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .binding = binding,
            .stride = input_binding.stride,
            .inputRate = is_instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = is_instanced ? input_binding.frequency : 1,
        });
    }
    // Clear the per-slot flags together instead of 64 read/modify/write operations.
    static const auto clean_mask = [] {
        Tegra::Engines::Maxwell3D::DirtyState::Flags mask;
        mask.set();
        for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
            mask.reset(Dirty::VertexAttribute0 + index);
        }
        for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
            mask.reset(Dirty::VertexBinding0 + index);
        }
        return mask;
    }();
    dirty &= clean_mask;
    scheduler.Record([bindings, attributes](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetVertexInputEXT(bindings, attributes);
    });
}

void RasterizerVulkan::InitializeChannel(Tegra::Control::ChannelState& channel) {
    CreateChannel(channel);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.CreateChannel(channel);
        buffer_cache.CreateChannel(channel);
    }
    pipeline_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
    state_tracker.SetupTables(channel);
}

void RasterizerVulkan::BindChannel(Tegra::Control::ChannelState& channel) {
    const s32 channel_id = channel.bind_id;
    BindToChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.BindToChannel(channel_id);
        buffer_cache.BindToChannel(channel_id);
    }
    pipeline_cache.BindToChannel(channel_id);
    query_cache.BindToChannel(channel_id);
    state_tracker.ChangeChannel(channel);
    state_tracker.InvalidateState();
}

void RasterizerVulkan::ReleaseChannel(s32 channel_id) {
    EraseChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.EraseChannel(channel_id);
        buffer_cache.EraseChannel(channel_id);
    }
    pipeline_cache.EraseChannel(channel_id);
    query_cache.EraseChannel(channel_id);
}
} // namespace Vulkan
