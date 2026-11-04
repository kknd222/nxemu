// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <fmt/ranges.h>

#include "frontend/graphics_context.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/polyfill_ranges.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_common/stb.h"
#include "yuzu_video_core/capture.h"
#include "yuzu_video_core/diagnostic_frame_capture.h"
#include "yuzu_video_core/frame_capture_context.h"
#include "yuzu_video_core/paired_pixel_capture.h"
#include "yuzu_video_core/draw_admission_trace.h"
#include "yuzu_video_core/native_draw_input_fixture.h"
#include "yuzu_video_core/image_writer_trace.h"
#include "yuzu_video_core/renderer_vulkan/source_pixel_probe.h"
#include "yuzu_video_core/gpu.h"
#include "yuzu_video_core/host_shaders/watermark_frag_spv.h"
#include "yuzu_video_core/host_shaders/watermark_vert_spv.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/renderer_vulkan.h"
#include "nxemu-core/settings/identifiers.h"
#include <nxemu-module-spec/base.h>
#include <atomic>
#include "yuzu_video_core/renderer_vulkan/vk_blit_screen.h"
#include "yuzu_video_core/renderer_vulkan/vk_rasterizer.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_swapchain.h"
#include "yuzu_video_core/textures/decoders.h"
#include "yuzu_video_core/vulkan_common/vulkan_debug_callback.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_library.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include "yuzu_video_core/vulkan_common/vulkan_surface.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"
#include "yuzu_video_core/watermark.h"


extern IModuleSettings * g_settings;

#if defined(__ANDROID__)
#define NXEMU_ANDROID_DIAG_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#else
#define NXEMU_ANDROID_DIAG_EXPORT
#endif

namespace {
std::atomic<uint64_t> g_android_composite_count{};
std::atomic<uint64_t> g_android_present_count{};
std::atomic<uint32_t> g_android_last_fb_width{};
std::atomic<uint32_t> g_android_last_fb_height{};
std::atomic<uint32_t> g_android_last_fb_stride{};
std::atomic<uint32_t> g_android_last_fb_format{};

bool NxemuShouldKeepVideoDiag(uint32_t count) {
    return count <= 8 || count == 16 || count == 32 || count == 64 || (count % 600) == 0;
}

void NxemuVideoDiag(const char* category, const std::string& detail) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "NxEmuHleDiag", "%s %s", category, detail.c_str());
#else
    (void)category;
    (void)detail;
#endif
}
void NxemuVideoDiag(const char* category, const char* detail) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "NxEmuHleDiag", "%s %s", category, detail);
#else
    (void)category;
    (void)detail;
#endif
}
}

extern "C" NXEMU_ANDROID_DIAG_EXPORT uint64_t NxemuAndroidGetVulkanCompositeCount() {
    return g_android_composite_count.load(std::memory_order_relaxed);
}

extern "C" NXEMU_ANDROID_DIAG_EXPORT uint64_t NxemuAndroidGetVulkanPresentCount() {
    return g_android_present_count.load(std::memory_order_relaxed);
}

extern "C" NXEMU_ANDROID_DIAG_EXPORT uint32_t NxemuAndroidGetVulkanLastFbWidth() {
    return g_android_last_fb_width.load(std::memory_order_relaxed);
}

extern "C" NXEMU_ANDROID_DIAG_EXPORT uint32_t NxemuAndroidGetVulkanLastFbHeight() {
    return g_android_last_fb_height.load(std::memory_order_relaxed);
}

extern "C" NXEMU_ANDROID_DIAG_EXPORT uint32_t NxemuAndroidGetVulkanLastFbStride() {
    return g_android_last_fb_stride.load(std::memory_order_relaxed);
}

extern "C" NXEMU_ANDROID_DIAG_EXPORT uint32_t NxemuAndroidGetVulkanLastFbFormat() {
    return g_android_last_fb_format.load(std::memory_order_relaxed);
}

namespace Vulkan
{
namespace
{
constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version)
{
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device & device)
{
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY)
    {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS)
    {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>> & available_extensions)
{
    return fmt::format("{}", fmt::join(available_extensions, ","));
}
} // Anonymous namespace

Device CreateDevice(const vk::Instance & instance, const vk::InstanceDispatch & dld,
                    VkSurfaceKHR surface)
{
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    const s32 device_index = videoSettings.vulkan_device;
    if (device_index < 0 || device_index >= static_cast<s32>(devices.size()))
    {
        LOG_ERROR(Render_Vulkan, "Invalid device index {}!", device_index);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow & emu_window,
                               Tegra::MaxwellDeviceMemoryManager & device_memory_, Tegra::GPU & gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_,
                               Display & display_)
try :
    RendererBase(emu_window, std::move(context_)),
    device_memory(device_memory_), gpu(gpu_), display(display_), device(display.GetDevice()),
    memory_allocator(display.GetAllocator()), swapchain(display.GetSwapchain()), state_tracker(),
    scheduler(device, state_tracker),
    present_manager(render_window, device, memory_allocator, scheduler, display),
    blit_swapchain(device_memory, device, memory_allocator, present_manager, scheduler,
                   PresentFiltersForDisplay),
    blit_capture(device_memory, device, memory_allocator, present_manager, scheduler,
                 PresentFiltersForDisplay),
    blit_applet(device_memory, device, memory_allocator, present_manager, scheduler,
                PresentFiltersForAppletCapture),
    rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker,
               scheduler),
    applet_frame(),
    wm{}
{
    if (videoSettings.renderer_force_max_clock && device.ShouldBoostClocks())
    {
        turbo_mode.emplace(display.GetInstance(), display.GetDispatch());
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }
    // Runtime watermark is disabled; game presentation stays unobstructed.
    Report();
}
catch (const vk::Exception & exception)
{
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan()
{
    if (VideoCommon::DiagnosticFrameCapture::Enabled()) {
        VideoCommon::DiagnosticFrameCapture::CancelQueued(
            VideoCommon::DiagnosticFrameCapture::Root(),VideoCommon::DiagnosticFrameCapture::RunId(),
            VideoCommon::DiagnosticFrameCapture::Generation());
    }
    scheduler.RegisterOnSubmit([] {});
    rasterizer.WaitForShaderBuilds();
    scheduler.Finish();
    {
        std::scoped_lock lock{device.QueueMutex()};
        void(device.GetLogical().WaitIdle());
    }
    CleanupWatermark();
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers)
{
    static std::atomic<uint32_t> composite_log_counter{};
    const uint32_t composite_ordinal =
        composite_log_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    g_android_composite_count.fetch_add(1, std::memory_order_relaxed);
    if (!framebuffers.empty()) {
        g_android_last_fb_width.store(framebuffers[0].width, std::memory_order_relaxed);
        g_android_last_fb_height.store(framebuffers[0].height, std::memory_order_relaxed);
        g_android_last_fb_stride.store(framebuffers[0].stride, std::memory_order_relaxed);
        g_android_last_fb_format.store(static_cast<u32>(framebuffers[0].pixel_format),
                                       std::memory_order_relaxed);
    }
    if (NxemuShouldKeepVideoDiag(composite_ordinal)) {
        NxemuVideoDiag(
            "RendererVulkan.Composite",
            std::string{"count="} + std::to_string(framebuffers.size()) +
                " ordinal=" + std::to_string(composite_ordinal) +
                (framebuffers.empty() ? std::string{}
                                      : (" first_addr=" + std::to_string(framebuffers[0].address) +
                                         " first_w=" + std::to_string(framebuffers[0].width) +
                                         " first_h=" + std::to_string(framebuffers[0].height) +
                                         " first_stride=" + std::to_string(framebuffers[0].stride) +
                                         " first_format=" + std::to_string(static_cast<u32>(framebuffers[0].pixel_format)))));
    }
    if (framebuffers.empty())
    {
        return;
    }

    SCOPE_EXIT
    {
        render_window.OnFrameDisplayed();
    };

    RenderAppletCaptureLayer(framebuffers);

    std::unique_ptr<VideoCommon::FrameCaptureContext::Scope> frame_context;
    namespace P = VideoCommon::PairedPixelCapture;
    std::optional<VideoCommon::DiagnosticFrameCapture::Ticket> paired_ticket;
    vk::Buffer paired_offscreen, paired_target_buffer;
    uint64_t paired_offscreen_ns{}, paired_started_ns{}, paired_readback_ns{};
    std::unique_ptr<SourcePixelProbe::Scope> source_probe;
    P::Image paired_target;
    bool paired_target_queued{}, paired_gpu_finished{}, paired_gpu_drained{};
    SCOPE_EXIT {
        if (paired_ticket) {
            // Never release a queued download allocation on an exception/early return before
            // the renderer's existing completion mechanism has waited for its commands.
            try {
                if (!paired_gpu_drained && (paired_target_queued ||
                    (source_probe && source_probe->data.queued))) {
                    try {
                        scheduler.Finish();paired_gpu_drained=true;
                        paired_gpu_finished=!device.IsLost();
                    } catch(...) {
                        // A failed wait is not a completion proof. Retain at most this one
                        // request's allocations and disable future paired allocation.
                        paired_readback_disabled=true;
                        if(source_probe && source_probe->data.queued){
                            paired_failed_buffers[0]=std::move(source_probe->data.buffer);
                            source_probe->data.queued=false;
                            source_probe->data.reason="finish_failed_buffer_retained";
                        }
                        if(paired_target_queued){
                            paired_failed_buffers[1]=std::move(paired_target_buffer);
                            paired_target_queued=false;
                            paired_target.reason="finish_failed_buffer_retained";
                        }
                    }
                }
                P::Image source;
                if (source_probe) {
                    auto& d=source_probe->data;
                    source.width=d.width;source.height=d.height;source.format=d.format;
                    source.mip=d.mip;source.layer=d.layer;source.samples=d.samples;
                    source.image=VideoCommon::FrameCaptureContext::Handle(d.source_image);
                    source.view=VideoCommon::FrameCaptureContext::Handle(d.source_view);
                    source.swizzle_known=d.swizzle_known;source.identity_swizzle=d.identity_swizzle;
                    source.swizzle=d.swizzle;
                    source.reason=d.reason;
                    if(device.IsLost())source.reason="device_lost_not_pixel_evidence";
                    if (d.queued && paired_gpu_finished) {
                        d.buffer.Invalidate();source.pixels=d.buffer.Mapped();source.gpu_completed=true;
                    }
                }
                if (paired_target_queued && paired_gpu_finished) {
                    paired_target_buffer.Invalidate();
                    paired_target.pixels=paired_target_buffer.Mapped();paired_target.gpu_completed=true;
                }
                if(device.IsLost())paired_target.reason="device_lost_not_pixel_evidence";
                if(VideoCommon::ImageWriters::Enabled()){
                    try {
                    namespace W=VideoCommon::ImageWriters;
                    const auto writer_snapshot=W::Read(source.image);
                    const auto snapshot=W::Serialize(writer_snapshot);
                    std::string admissions;
                    try {
                        admissions=VideoCommon::DrawAdmission::Serialize(
                            VideoCommon::DrawAdmission::Read(writer_snapshot));
                    } catch(...) {
                        VideoCommon::DrawAdmission::Snapshot failed;
                        failed.selected=source.image;
                        failed.reason="admission_serialization_failed";
                        admissions=VideoCommon::DrawAdmission::Serialize(failed);
                    }
                    using namespace VideoCommon::DiagnosticFrameCapture;
                    std::ostringstream writer;
                    writer<<"{\"schema\":1,\"request_id\":"<<Quote(paired_ticket->request.id)
                     <<",\"run_id\":"<<Quote(paired_ticket->request.run_id)
                     <<",\"generation\":"<<Quote(paired_ticket->request.generation)
                     <<",\"composite_ordinal\":"<<composite_ordinal<<",\"snapshot_epoch_ms\":"<<EpochMs()
                     <<",\"gpu_finish_boundary\":"<<(paired_gpu_finished&&!device.IsLost()?"true":"false")
                     <<",\"registry\":"<<snapshot
                     <<",\"admissions\":"<<admissions
                     <<",\"native_draw_input_diagnostics\":"
                     <<(VideoCommon::NativeDrawInputs::Enabled()?VideoCommon::NativeDrawInputs::CountersJson():"null")
                     <<",\"coverage\":\"original/scaled birth; direct draw; native full-color clear; ordinary upload/copy/blit-scale; PrepareDraw admission guest-owner birth candidates\""
                     <<",\"missing\":[\"VTG/indirect/DrawTexture\",\"masked/helper clear\",\"storage-image compute/reinterpret/MSAA\"]}";
                    const auto text=writer.str();
                    if(text.size()<=W::MaxBytes)AtomicText(paired_ticket->root/("frame-capture-"+paired_ticket->request.id+".writers.json"),text);
                    } catch(...) { /* Optional writer history must not discard paired pixels. */ }
                }
                P::Publish(*paired_ticket,composite_ordinal,source,paired_target,paired_readback_ns);
            } catch(const std::exception& error) {
                LOG_WARNING(Render_Vulkan,"Paired pixel capture publication failed: {}",error.what());
            } catch(...) { LOG_WARNING(Render_Vulkan,"Paired pixel capture publication failed"); }
            // Opt-in pair metadata is published before the existing completion marker. The
            // legacy (pair disabled) branch below retains its original completion timing.
            try {
                if(device.IsLost())VideoCommon::DiagnosticFrameCapture::Failure(*paired_ticket,"device_lost_not_pixel_evidence");
                else VideoCommon::DiagnosticFrameCapture::Complete(*paired_ticket,paired_offscreen.Mapped(),paired_offscreen_ns);
            }
            catch(...) {
                try { VideoCommon::DiagnosticFrameCapture::Failure(*paired_ticket,"deferred capture publication exception"); }
                catch(...) { /* Capture IO must not terminate renderer scope exit. */ }
            }
        }
    };
    // Explicit offscreen diagnostic capture precedes visibility checks. It does not depend on
    // the OS window being restored, and owns the download buffer until GPU Finish completes.
    if (VideoCommon::DiagnosticFrameCapture::Enabled()) {
        using namespace VideoCommon::DiagnosticFrameCapture;
        if (auto ticket=Poll(Root(),RunId(),Generation(),render_window.IsShown())) {
            try {
                namespace C = VideoCommon::FrameCaptureContext;
                frame_context=std::make_unique<C::Scope>(ticket->root,ticket->request.id,ticket->request.run_id,ticket->request.generation,composite_ordinal,ticket->window_is_shown);
                std::array<C::Input,C::MaxLayers> inputs{};
                const auto count=std::min(framebuffers.size(),C::MaxLayers);
                for(size_t n=0;n<count;++n){const auto& f=framebuffers[n];inputs[n]={f.address,f.offset,f.width,f.height,f.stride,static_cast<uint32_t>(f.pixel_format),static_cast<uint32_t>(f.transform_flags),static_cast<uint32_t>(f.blending),f.block_height_log2,{f.crop_rect.left,f.crop_rect.top,f.crop_rect.right,f.crop_rect.bottom},f.native_tiling};}
                frame_context->Inputs(std::span(inputs.data(),count));frame_context->data->input_total=framebuffers.size();
            } catch(...) { /* Optional context cannot reject a render or capture request. */ }
            try {
                const Layout::FramebufferLayout layout{
                    .width=Width,.height=Height,.screen={0,0,Width,Height},.is_srgb=false};
                const auto started=SteadyNs();
                auto buffer=RenderToBuffer(framebuffers,layout,VK_FORMAT_B8G8R8A8_UNORM,
                    VkDeviceSize{Width}*Height*4);
                if(device.IsLost())throw std::runtime_error("device_lost_not_pixel_evidence");
                if (Common::DebugDocument::Environment("NXEMU_PAIRED_PIXELS")=="1" && !paired_readback_disabled) {
                    paired_offscreen=std::move(buffer);paired_offscreen_ns=SteadyNs()-started;
                    paired_ticket=*ticket;
                } else {
                    if(paired_readback_disabled && Common::DebugDocument::Environment("NXEMU_PAIRED_PIXELS")=="1"){
                        P::Image missing;missing.reason="paired_capture_disabled_after_failed_wait";
                        try { P::Publish(*ticket,composite_ordinal,missing,missing,0); } catch(...) {}
                    }
                    Complete(*ticket,buffer.Mapped(),SteadyNs()-started);
                }
            } catch (const std::exception& error) {
                Failure(*ticket,error.what());
                LOG_WARNING(Render_Vulkan,"Diagnostic frame capture failed: {}",error.what());
            }
        }
    }

    VideoCommon::FrameCaptureContext::SetRole(VideoCommon::FrameCaptureContext::Role::Other);
    if (!render_window.IsShown())
    {
        VideoCommon::FrameCaptureContext::Path("hidden_return",false);
        NxemuVideoDiag("RendererVulkan.CompositeSkip",
                                                      "render_window_hidden");
        return;
    }

    RenderScreenshot(framebuffers);
#if defined(__ANDROID__)
    static std::atomic<uint32_t> android_skip_counter{};
    const int frame_skip = g_settings != nullptr ? g_settings->GetInt(NXCoreSetting::AndroidFrameSkip) : 0;
    if (frame_skip > 0) {
        const uint32_t index = android_skip_counter.fetch_add(1, std::memory_order_relaxed);
        const uint32_t period = static_cast<uint32_t>(frame_skip + 1);
        if ((index % period) != 0) {
            if (NxemuShouldKeepVideoDiag(composite_ordinal)) {
                NxemuVideoDiag("RendererVulkan.FrameSkip",
                               std::string{"skip="} + std::to_string(frame_skip) +
                                   " ordinal=" + std::to_string(composite_ordinal));
            }
            gpu.RendererFrameEndNotify();
            rasterizer.TickFrame();
            return;
        }
    }
#endif
    const bool hold_incomplete = HoldIncompleteFrame();
    if (hold_incomplete)
    {
        VideoCommon::FrameCaptureContext::Path("held_incomplete_return",true,true,true);
        scheduler.Flush();
        gpu.RendererFrameEndNotify();
        rasterizer.TickFrame();
        return;
    }
    presented_once = true;
    Frame * frame = present_manager.GetRenderFrame();
    VideoCommon::FrameCaptureContext::Path("swapchain_draw_entered",true,true,false);
    VideoCommon::FrameCaptureContext::SetRole(VideoCommon::FrameCaptureContext::Role::Swapchain);
    if (paired_ticket) {
        paired_started_ns=VideoCommon::DiagnosticFrameCapture::SteadyNs();
        try { source_probe=std::make_unique<SourcePixelProbe::Scope>(true); }
        catch(...) { /* Optional probe allocation must not reject actual drawing. */ }
    }
    blit_swapchain.DrawToFrame(rasterizer, frame, framebuffers,
                               render_window.GetFramebufferLayout(), swapchain.GetImageCount(),
                               swapchain.GetImageViewFormat());
    VideoCommon::FrameCaptureContext::Target(VideoCommon::FrameCaptureContext::Handle(*frame->image),VideoCommon::FrameCaptureContext::Handle(*frame->image_view));
    if (paired_ticket) {
        try {
            paired_target.width=frame->width;paired_target.height=frame->height;
            paired_target.format=swapchain.GetImageViewFormat();
            paired_target.image=VideoCommon::FrameCaptureContext::Handle(*frame->image);
            paired_target.view=VideoCommon::FrameCaptureContext::Handle(*frame->image_view);
            paired_target.swizzle_known=true;paired_target.identity_swizzle=true;
            const auto bytes=P::Bytes(frame->width,frame->height);
            if (!bytes || !P::Supported(paired_target.format)) {
                paired_target.reason="unsupported target format or byte budget";
            } else {
                paired_target_buffer=CreateWrappedBuffer(memory_allocator,bytes,MemoryUsage::Download);
                const VkImage image=*frame->image;const VkBuffer buffer=*paired_target_buffer;
                const VkExtent3D extent{frame->width,frame->height,1};
                scheduler.RequestOutsideRenderPassOperationContext();
                scheduler.Record([image,buffer,extent](vk::CommandBuffer cmdbuf) {
                    DownloadColorImage(cmdbuf,image,buffer,extent);
                    const VkMemoryBarrier host{
                        .sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.pNext=nullptr,
                        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
                    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,host);
                });
                paired_target_queued=true;paired_target.reason.clear();
            }
            if (paired_target_queued || (source_probe && source_probe->data.queued)) {
                scheduler.Finish();paired_gpu_drained=true;paired_gpu_finished=!device.IsLost();
            }
            paired_readback_ns=VideoCommon::DiagnosticFrameCapture::SteadyNs()-paired_started_ns;
        } catch(const std::exception& error) {
            paired_target.reason="paired readback exception";
            LOG_WARNING(Render_Vulkan,"Paired pixel readback failed: {}",error.what());
        } catch(...) { paired_target.reason="paired readback exception"; }
        // Stop request-local source interception before the next draw or callback.
        // Scope/allocation itself remains alive through publication at function exit.
        SourcePixelProbe::current=nullptr;
    }
    // Do not overlay the NXEmu logo on running game frames.
    scheduler.Flush(*frame->render_ready);
    present_manager.Present(frame);
    VideoCommon::FrameCaptureContext::Path("present_enqueued_not_present_completion",true,true,false);
    g_android_present_count.fetch_add(1, std::memory_order_relaxed);
    if (NxemuShouldKeepVideoDiag(composite_ordinal)) {
        NxemuVideoDiag("RendererVulkan.Present",
                       std::string{"submitted ordinal="} + std::to_string(composite_ordinal));
    }

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
}

bool RendererVulkan::HoldIncompleteFrame()
{
    // A scene that just loaded compiles many pipelines at once, and its first frames miss many
    // draws. Those frames are not shown: the screen keeps the previous frame, often the
    // transition, while emulation goes on. During play, a few missing draws are better than a
    // frozen screen, so frames are held only right after a scene change, and never for long.
    constexpr std::chrono::milliseconds scene_change_window{1000};
    constexpr std::chrono::milliseconds max_hold{1500};
    constexpr u32 incomplete_percent = 10;
    const auto draws = rasterizer.TakeFrameDraws();
    const auto now = std::chrono::steady_clock::now();
    if (draws.new_render_targets)
    {
        last_new_render_target = now;
    }
    const bool incomplete = draws.skipped > 0 && draws.skipped * 100 >= draws.total * incomplete_percent;
    if (!incomplete)
    {
        incomplete_since.reset();
    }
    else if (!incomplete_since)
    {
        incomplete_since = now;
        hold_incomplete = last_new_render_target && now - *last_new_render_target < scene_change_window;
    }
    return incomplete && hold_incomplete && presented_once && now - *incomplete_since < max_hold;
}

void RendererVulkan::InitializeWatermark()
{
    if (wm.valid)
    {
        return;
    }
    wm.timer_started = false;

    int ch = 0;
    unsigned char * pixels = stbi_load_from_memory(watermark_png, watermark_png_len, &wm.width, &wm.height, &ch, 4);
    if (!pixels)
    {
        LOG_ERROR(Render_Vulkan, "watermark load failed: {}", stbi_failure_reason());
        return;
    }

    const VkExtent2D dims{(uint32_t)wm.width, (uint32_t)wm.height};
    const VkFormat swapFmt = swapchain.GetImageViewFormat();
    const VkFormat texFmt = (swapFmt == VK_FORMAT_B8G8R8A8_SRGB || swapFmt == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    wm.image = Vulkan::CreateWrappedImage(memory_allocator, dims, texFmt);
    Vulkan::UploadImage(device, memory_allocator, scheduler, wm.image, dims, texFmt, std::span<const u8>(pixels, wm.width * wm.height * 4));
    stbi_image_free(pixels);
    wm.image_view = Vulkan::CreateWrappedImageView(device, wm.image, texFmt);
    wm.sampler = Vulkan::CreateWrappedSampler(device, VK_FILTER_LINEAR);
    wm.dsl = Vulkan::CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    wm.dpool = Vulkan::CreateWrappedDescriptorPool(device, 1, 1, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    std::array<VkDescriptorSetLayout, 1> layouts{*wm.dsl};
    vk::DescriptorSets sets = Vulkan::CreateWrappedDescriptorSets(wm.dpool, vk::Span<VkDescriptorSetLayout>(layouts.data(), layouts.size()));
    wm.set = sets[0];

    std::vector<VkDescriptorImageInfo> img_infos;
    img_infos.reserve(1);
    const VkWriteDescriptorSet write = Vulkan::CreateWriteDescriptorSet(img_infos, *wm.sampler, *wm.image_view, wm.set, 0);
    device.GetLogical().UpdateDescriptorSets({write}, {});

    CreateWatermarkRenderPass();
    CreateWatermarkPipeline();
    CreateWatermarkVertexBuffer();

    wm.show = true;
    wm.valid = true;
}

void RendererVulkan::CleanupWatermark()
{
    if (!wm.valid)
    {
        return;
    }

    wm.image = {};
    wm.image_view = {};
    wm.sampler = {};
    wm.dsl = {};
    wm.dpool = {};
    wm.set = {};
    wm.render_pass = {};
    wm.pipeline = {};
    wm.pipeline_layout = {};
    wm.vertex_buffer = {};

    wm.show = false;
    wm.valid = false;
}

void RendererVulkan::RenderWatermark(Frame & frame)
{
    if (!wm.timer_started)
    {
        wm.timer_started = true;
        wm.start_time = std::chrono::steady_clock::now();
    }

    std::chrono::steady_clock::time_point current_time = std::chrono::steady_clock::now();
    long long elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(current_time - wm.start_time).count();
    const float elapsed = static_cast<float>(elapsed_seconds);

    float fade_alpha;
    const float fade_duration = 300.0f;
    if (elapsed >= fade_duration)
    {
        fade_alpha = 0.0f;
        wm.show = false;
        return;
    }
    else
    {
        float progress = elapsed / fade_duration;
        float eased = 0.5f * (1.0f + cosf(progress * 3.14159f));
        fade_alpha = 0.8f * eased;
    }

    const float frame_w = static_cast<float>(frame.width);
    const float frame_h = static_cast<float>(frame.height);
    float watermark_scale = 0.14f;
    float aspect_ratio = static_cast<float>(wm.height) / static_cast<float>(wm.width);
    float watermark_screen_width = frame_w * watermark_scale;
    float watermark_screen_height = watermark_screen_width * aspect_ratio;

    float padding = 18.0f;
    float x = frame_w - watermark_screen_width - padding;
    float y = padding;

    float ndc_left = (x / frame_w) * 2.0f - 1.0f;
    float ndc_right = ((x + watermark_screen_width) / frame_w) * 2.0f - 1.0f;
    float ndc_bottom = -(((y + watermark_screen_height) / frame_h) * 2.0f - 1.0f);
    float ndc_top = -((y / frame_h) * 2.0f - 1.0f);

    float vertices[] = {
        ndc_left, ndc_bottom, 0.0f, 0.0f,  // Bottom-left -> tex (0,0)
        ndc_right, ndc_bottom, 1.0f, 0.0f, // Bottom-right -> tex (1,0)
        ndc_left, ndc_top, 0.0f, 1.0f,     // Top-left -> tex (0,1)

        ndc_right, ndc_bottom, 1.0f, 0.0f, // Bottom-right -> tex (1,0)
        ndc_right, ndc_top, 1.0f, 1.0f,    // Top-right -> tex (1,1)
        ndc_left, ndc_top, 0.0f, 1.0f      // Top-left -> tex (0,1)
    };

    std::memcpy(wm.vertex_buffer.Mapped().data(), vertices, sizeof(vertices));

    if (!frame.watermark_framebuffer)
    {
        frame.watermark_framebuffer = CreateWrappedFramebuffer(device, wm.render_pass, frame.image_view, VkExtent2D{.width = frame.width, .height = frame.height});
    }

    const VkFramebuffer framebuffer{*frame.watermark_framebuffer};
    const VkRenderPass renderpass{*wm.render_pass};
    const VkImage image{*frame.image};
    const VkExtent2D extent{.width = frame.width, .height = frame.height};
    const VkPipeline pipeline{*wm.pipeline};
    const VkPipelineLayout pipeline_layout{*wm.pipeline_layout};
    const VkDescriptorSet descriptor_set{wm.set};
    const VkBuffer vertex_buffer{*wm.vertex_buffer};
    const std::array<float, 4> push_constants{fade_alpha, 0.0f, 0.0f, 0.0f};

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([framebuffer, renderpass, image, extent, pipeline, pipeline_layout, descriptor_set, vertex_buffer, push_constants](vk::CommandBuffer cmdbuf)
    {
        TransitionImageLayout(cmdbuf, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        BeginRenderPass(cmdbuf, renderpass, framebuffer, extent);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, descriptor_set, {});
        cmdbuf.PushConstants(pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, static_cast<u32>(push_constants.size() * sizeof(float)), push_constants.data());
        cmdbuf.BindVertexBuffers(0, 1, &vertex_buffer, std::array<VkDeviceSize, 1>{0}.data());
        cmdbuf.Draw(6, 1, 0, 0);
        cmdbuf.EndRenderPass();

        const VkImageMemoryBarrier transition{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, transition);
    });
    scheduler.InvalidateState();
}

void RendererVulkan::CreateWatermarkRenderPass()
{
    VkAttachmentDescription color_attachment{
        .flags = 0,
        .format = swapchain.GetImageViewFormat(),
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, // Keep existing content
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference color_ref{
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = nullptr,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };

    VkRenderPassCreateInfo render_pass_info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    };

    wm.render_pass = device.GetLogical().CreateRenderPass(render_pass_info);
}

void RendererVulkan::CreateWatermarkPipeline()
{
    Vulkan::vk::ShaderModule vertex_shader = BuildShader(device, std::span<const u32>{WATERMARK_VERT_SPV, sizeof(WATERMARK_VERT_SPV) / sizeof(u32)});
    Vulkan::vk::ShaderModule fragment_shader = BuildShader(device, std::span<const u32>{WATERMARK_FRAG_SPV, sizeof(WATERMARK_FRAG_SPV) / sizeof(u32)});

    VkPushConstantRange push_constant_range{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(float) * 4, // vec4 = 16 bytes
    };

    VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = wm.dsl.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };

    wm.pipeline_layout = device.GetLogical().CreatePipelineLayout(layout_info);

    VkVertexInputBindingDescription binding_desc{
        .binding = 0,
        .stride = 4 * sizeof(float), // x, y, u, v
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    std::array<VkVertexInputAttributeDescription, 2> attribute_descs{{
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = 0,
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = 2 * sizeof(float),
        },
    }};

    VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding_desc,
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attribute_descs.size()),
        .pVertexAttributeDescriptions = attribute_descs.data(),
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr, // Dynamic
        .scissorCount = 1,
        .pScissors = nullptr, // Dynamic
    };

    VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisampling{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
    };

    // Enable alpha blending like OpenGL version
    VkPipelineColorBlendAttachmentState color_blend_attachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blending{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    std::array<VkDynamicState, 2> dynamic_states = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    std::array<VkPipelineShaderStageCreateInfo, 2> shader_stages{{
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = *vertex_shader,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = *fragment_shader,
            .pName = "main",
        },
    }};

    VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<uint32_t>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = *wm.pipeline_layout,
        .renderPass = *wm.render_pass,
        .subpass = 0,
    };

    wm.pipeline = device.GetLogical().CreateGraphicsPipeline(pipeline_info);
}

void RendererVulkan::CreateWatermarkVertexBuffer()
{
    // Create vertex buffer for a quad (2 triangles = 6 vertices)
    // Each vertex has 4 floats: x, y, u, v
    const size_t buffer_size = 6 * 4 * sizeof(float);

    wm.vertex_buffer = memory_allocator.CreateBuffer({
                                                         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                         .pNext = nullptr,
                                                         .flags = 0,
                                                         .size = buffer_size,
                                                         .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                     },
                                                     MemoryUsage::Upload);
}

void RendererVulkan::Report() const
{
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers, const Layout::FramebufferLayout & layout, VkFormat format, VkDeviceSize buffer_size)
{
    auto frame = [&]() {
        Frame f{};
        f.image = CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(rasterizer, &frame, framebuffers, layout, 1, format);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer, VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers)
{
    if (!renderer_settings.screenshot_requested)
    {
        return;
    }

    const auto & layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(framebuffers, layout, VK_FORMAT_B8G8R8A8_UNORM, layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(), dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer()
{
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image)
    {
        return out;
    }

    const auto dst_buffer = CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth, LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers)
{
    if (!applet_frame.image)
    {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    // Several resource sets, so drawing a frame does not wait for the previous one on the GPU.
    blit_applet.DrawToFrame(rasterizer, &applet_frame, framebuffers, VideoCore::Capture::Layout, 3, CaptureFormat);
}
} // namespace Vulkan

// Platform diagnostic IO stays at the owner translation-unit tail, never in a shared header.
#include "yuzu_common/debug_document.h"
namespace VideoCommon::DiagnosticFrameCapture {
bool Enabled() {
    static const bool enabled=[] {
        const auto value=Common::DebugDocument::Environment("NXEMU_FRAME_CAPTURE");
        return value.empty() ? Common::DebugDocument::Enabled() :
               value == "1" && !Common::DebugDocument::Environment("NXEMU_NATIVE_SHADER_DIAG_DIR").empty();
    }();
    return enabled;
}
std::filesystem::path Root() {
    return std::filesystem::u8path(Common::DebugDocument::Environment("NXEMU_NATIVE_SHADER_DIAG_DIR"));
}
std::string RunId() { return Common::DebugDocument::Environment("NXEMU_DEBUG_RUN_ID"); }
std::string Generation() { return Common::DebugDocument::Environment("NXEMU_DEBUG_GENERATION"); }
} // namespace VideoCommon::DiagnosticFrameCapture
