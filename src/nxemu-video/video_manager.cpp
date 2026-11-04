#include "yuzu_video_core/shader_progress_trace.h"
#include "yuzu_video_core/pipeline_fixture.h"
#include "yuzu_video_core/host1x/ffmpeg/device_cache.h"
#include "render_window.h"
#include "video_manager.h"
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile_layout.h"
#include "yuzu_video_core/shader_profile.h"
#include "yuzu_video_core/shader_profile_identity.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_settings.h"
#include "yuzu_video_core/capture.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/host1x/host1x.h"
#include "yuzu_video_core/video_core.h"
#include "yuzu_video_core/gpu.h"
#include "yuzu_video_core/renderer_base.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include <nxemu-core/settings/identifiers.h>
#include "yuzu_video_core/shader_notify.h"
#include "yuzu_video_core/textures/decoders.h"
#include "yuzu_common/polyfill_thread.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/scope_exit.h"
#include "nxemu-video/video_settings.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <future>
#include <thread>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#endif


namespace {
void NxemuVideoDiag(const char* category, const std::string& detail) {
#if defined(__ANDROID__) && defined(NXEMU_ANDROID_FULL_DIAG)
    __android_log_print(ANDROID_LOG_INFO, "NxEmuHleDiag", "%s %s", category, detail.c_str());
#else
    (void)category;
    (void)detail;
#endif
}
void NxemuVideoDiag(const char* category, const char* detail) {
#if defined(__ANDROID__) && defined(NXEMU_ANDROID_FULL_DIAG)
    __android_log_print(ANDROID_LOG_INFO, "NxEmuHleDiag", "%s %s", category, detail);
#else
    (void)category;
    (void)detail;
#endif
}
}

extern IModuleSettings * g_settings;

bool VideoManager::ProcessShaderProfile(const ShaderProfileTask & task, IShaderProfileProgress & progress,
                                        ShaderProfileResult & result, char * error, uint32_t error_size)
{
    try
    {
        const auto text = [](const char * value) { return value ? std::string(value) : std::string(); };
        const auto path = [&](const char * value) { return std::filesystem::u8path(text(value)); };
        if (!task.title_id || !task.profile || !task.game_root) throw std::runtime_error("Missing shader profile task inputs");
        if (task.importing)
        {
            if(!progress.ProfileProgress("profile",0,1))throw std::runtime_error("Shader profile operation cancelled");
            const auto metadata=VideoCommon::ShaderProfile::Inspect(path(task.profile),task.title_id);
            if(!progress.ProfileProgress("profile",1,1))throw std::runtime_error("Shader profile operation cancelled");
            result.graphics=metadata.graphics;result.compute=metadata.compute;
            result.unresolved=metadata.unresolved;result.bytes=metadata.bytes;
            result.programs=metadata.programs;result.environments=metadata.environments;
            result.versions=metadata.versions;
            if(const auto context=text(task.context);!context.empty())
                result.context_match=metadata.primary_context==context?2U:
                    std::find(metadata.contexts.begin(),metadata.contexts.end(),context)!=metadata.contexts.end()?1U:0U;
            // The reader limits each text to its buffer; the copies always fit.
            std::snprintf(result.label,sizeof(result.label),"%s",metadata.label.c_str());
            std::snprintf(result.revision,sizeof(result.revision),"%s",metadata.revision.c_str());
            std::snprintf(result.note,sizeof(result.note),"%s",metadata.note.c_str());
            result.full_game=metadata.full_game;
            std::string targets;
            for(const auto& version:metadata.targets)
                if(const auto text=VideoCommon::ShaderProfile::GameVersionText(version);!text.empty()&&targets.size()+text.size()+1<sizeof(result.targets))
                    targets+=(targets.empty()?"":"\n")+text;
            std::snprintf(result.targets,sizeof(result.targets),"%s",targets.c_str());
            return true;
        }
        VideoCommon::ShaderProfile::ExportOptions options;
        options.cache = path(task.cache);
        options.recording = path(task.recording);
        options.context = VideoCommon::ShaderProfile::RecordingContext(options.recording);
        if (!VideoCommon::ShaderProfile::ValidContext(options.context))
            throw std::runtime_error("Play and stop this base/update with identified game content before exporting a profile");
        options.game_root = path(task.game_root);
        options.output = path(task.profile);
        options.index = path(task.index);
        options.title_id = task.title_id;
        options.base = text(task.base);
        options.update = text(task.update);
        options.automatic_sources = true;
        options.label = text(task.label);
        options.previous = path(task.previous);
        options.revision = text(task.revision);
        options.note = text(task.note);
        options.full_game = task.full_game;
        options.compact = task.compact;
        options.allow_partial = true;
        options.progress = [&](const char * phase, u64 done, u64 total) { return progress.ProfileProgress(phase, done, total); };
        const auto r = VideoCommon::ShaderProfile::Export(options);
        result = {r.programs, r.environments, r.graphics, r.compute, r.unresolved, r.missing, r.reused, r.bytes, r.seconds};
        result.inherited = r.inherited;
        result.dropped_recipes = r.dropped_recipes;
        result.dropped_versions = r.dropped_versions;
        result.versions = r.versions;
        result.previous = r.previous == VideoCommon::ShaderProfile::ExportReport::Previous::Merged ? ShaderProfilePrevious::Merged
            : r.previous == VideoCommon::ShaderProfile::ExportReport::Previous::Unreadable ? ShaderProfilePrevious::Unreadable
                                                                                         : ShaderProfilePrevious::None;
        return true;
    }
    catch (const std::exception & exception)
    {
        if (error && error_size) std::snprintf(error, error_size, "%s", exception.what());
        return false;
    }
}

struct VideoManager::Impl
{
    Impl(IRenderWindow & window, ISystemModules & modules) :
        m_window(window),
        m_modules(modules)
    {
    }

    ~Impl()
    {
        Shutdown();
    }

    void StopInitialization(bool wait = true)
    {
        m_initStop.request_stop();
        if (m_gpuCore)
        {
            m_gpuCore->NotifyShutdown();
        }
        if (!wait)
        {
            return;
        }
        std::scoped_lock shutdown_lock{m_shutdownMutex};
        // The loader captures this and may still access the GPU, window and Host1x.
        if (m_init.valid())
        {
            m_init.wait();
        }
        if (m_gpuCore)
        {
            m_gpuCore->WaitForComposite();
            m_gpuCore->FinalizeShutdown();
        }
    }

    void Shutdown()
    {
        StopInitialization();
        m_gpuCore.reset();
        m_memoryManagerRegistry.Clear();
        m_display.reset();
        m_emuWindow.reset();
        m_host1x.reset();
        FFmpeg::ReleaseHardwareDecoderDevices();
    }

    RenderWindow & EnsureEmuWindow()
    {
        if (m_emuWindow == nullptr)
        {
            m_emuWindow = std::make_unique<RenderWindow>(m_window);
        }
        return *m_emuWindow;
    }

    Vulkan::Display * EnsureDisplay()
    {
        if (m_display || videoSettings.renderer_backend != RendererBackend::Vulkan)
        {
            return m_display.get();
        }
        m_display = VideoCore::CreateDisplay(EnsureEmuWindow());
        return m_display.get();
    }

    bool Initialize()
    {
        m_host1x = std::make_unique<Tegra::Host1x::Host1x>(m_modules.OperatingSystem().DeviceMemory());
        return true;
    }

    void EmulationStarting(void)
    {
        StopInitialization();
        g_settings->SetInt(NXCoreSetting::GpuInitializationState, (int32_t)GpuInitializationState::Preparing);
        m_gpuCore.reset();
        FFmpeg::WarmHardwareDecoderDevices();
        m_memoryManagerRegistry.Clear();
        g_settings->SetBool(NXCoreSetting::DisplayedFrames, false);

        // PrepareRom can replace the OS backing (NS1 4GiB -> NS2 12GiB).
        // Initialize() ran before that choice. No guest NVDRV session/GPU work
        // has started yet; refresh the entire Host1x before capturing GPU refs.
        m_host1x->MemoryManager().PublishBackingState("pre_gpu");
        if (!m_host1x->MemoryManager().BackingMatches()) {
            LOG_WARNING(HW_GPU, "Refreshing Host1x after program backing changed");
            m_host1x = std::make_unique<Tegra::Host1x::Host1x>(m_modules.OperatingSystem().DeviceMemory());
        }
        m_host1x->MemoryManager().PublishBackingState("gpu_ready");

        Layout::FramebufferLayout layout;
        if (m_emuWindow)
        {
            layout = m_emuWindow->GetFramebufferLayout();
        }
        m_emuWindow = nullptr;
        m_emuWindow = std::make_unique<RenderWindow>(m_window);
        m_emuWindow->UpdateCurrentFramebufferLayout(layout.width, layout.height);
        m_gpuCore = VideoCore::CreateGPU(m_modules, *(m_emuWindow.get()), *m_host1x, EnsureDisplay());
        if (!m_gpuCore)
        {
            NxemuVideoDiag("Video.EmulationStarting", "CreateGPU failed");
            // Wake the CPU thread and let the frontend stop the launch with an error.
            g_settings->SetInt(NXCoreSetting::GpuInitializationState, (int32_t)GpuInitializationState::Failed);
            return;
        }
        m_initStop = std::stop_source{};
        m_init = std::async(std::launch::async, [this, stop_token = m_initStop.get_token()]() {
            const auto notify_disk_cache_progress = [](VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
                if (g_settings == nullptr)
                {
                    return;
                }
                int stage_int = -1;
                switch (stage)
                {
                case VideoCore::LoadCallbackStage::Prepare:
                    stage_int = 0;
                    break;
                case VideoCore::LoadCallbackStage::Build:
                    stage_int = 1;
                    break;
                case VideoCore::LoadCallbackStage::Complete:
                    stage_int = 2;
                    break;
                case VideoCore::LoadCallbackStage::ReadProfile:
                    stage_int = 3;
                    break;
                case VideoCore::LoadCallbackStage::ImportProfile:
                    stage_int = 4;
                    break;
                case VideoCore::LoadCallbackStage::BuildProfile:
                    stage_int = 5;
                    break;
                }
                g_settings->SetInt(NXCoreSetting::DiskCacheLoadStage, stage_int);
                g_settings->SetInt(NXCoreSetting::DiskCacheLoadCurrent, static_cast<int32_t>(value));
                g_settings->SetInt(NXCoreSetting::DiskCacheLoadTotal, static_cast<int32_t>(total));
                g_settings->SetInt(NXCoreSetting::DiskCacheLoadTick, g_settings->GetInt(NXCoreSetting::DiskCacheLoadTick) + 1);
            };

            notify_disk_cache_progress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
            {
                m_gpuCore->ObtainContext();
                SCOPE_EXIT { m_gpuCore->ReleaseContext(); };
                if (videoSettings.use_disk_shader_cache && !stop_token.stop_requested())
                {
                    const u64 program_id = m_modules.OperatingSystem().GetProgramId();
                    try {
                        const auto content = VideoCommon::ShaderProfile::DescribeLoadedContent(m_modules.Systemloader(), program_id);
                        if (!content.context.empty()) {
                            const auto cache = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir) / fmt::format("{:016x}", program_id);
                            try {
                                VideoCommon::ShaderProfile::SaveProvenance(cache, content.provenance);
                            } catch (const std::exception & error) {
                                LOG_WARNING(Render, "Cannot save captured game version: {}", error.what());
                            }
                            LOG_INFO(Render, "Shader capture version: {} (context {})",
                                VideoCommon::ShaderProfile::RecordedVersionLabel(content.provenance), content.context);
                        }
                        m_gpuCore->Renderer().ReadRasterizer()->LoadDiskResources(
                            program_id, stop_token, notify_disk_cache_progress,
                            content.context,
                            g_settings->GetString(NXCoreSetting::GameFile));
                    } catch (const std::exception& exception) {
                        // std::async stores exceptions in its future. If one escapes here,
                        // Start is never called and guest GPU requests wait indefinitely.
                        LOG_ERROR(Render, "Shader cache loading failed: {}", exception.what());
                    }
                }
            }
            notify_disk_cache_progress(VideoCore::LoadCallbackStage::Complete, 0, 0);

            NxemuVideoDiag("Video.GPUCore", "Start thread");
            m_gpuCore->Start();
            if (!stop_token.stop_requested())
            {
                g_settings->SetInt(NXCoreSetting::GpuInitializationState, (int32_t)GpuInitializationState::Ready);
            }
        });
    }
    std::unique_ptr<Tegra::Host1x::Host1x> m_host1x;
    std::unique_ptr<RenderWindow> m_emuWindow;
    VideoCore::DisplayPtr m_display;
    std::unique_ptr<Tegra::GPU> m_gpuCore;
    IRenderWindow & m_window;
    ISystemModules & m_modules;
    Tegra::MemoryManagerRegistry m_memoryManagerRegistry;
    std::future<void> m_init;
    std::stop_source m_initStop;
    std::mutex m_shutdownMutex;
};

VideoManager::VideoManager(IRenderWindow & window, ISystemModules & modules) :
    impl{ std::make_unique<Impl>(window, modules) }
{
}

VideoManager::~VideoManager() = default;

void VideoManager::EmulationStarting()
{
    impl->EmulationStarting();
}

void VideoManager::EmulationStopping(bool wait)
{
    impl->StopInitialization(wait);
}

bool VideoManager::Initialize()
{
    SetupVideoSetting();
    return impl->Initialize();
}

void VideoManager::QuiesceGuestMemory()
{
    impl->StopInitialization();
}

void VideoManager::Shutdown()
{
    impl->Shutdown();
};

uint32_t VideoManager::AllocAsEx(uint64_t addressSpaceBits, uint64_t splitAddress, uint64_t bigPageBits, uint64_t pageBits)
{
    std::shared_ptr<Tegra::MemoryManager> memory = std::make_shared<Tegra::MemoryManager>(*impl->m_host1x, addressSpaceBits, splitAddress, bigPageBits, pageBits);
    impl->m_gpuCore->InitAddressSpace(*memory);

    return impl->m_memoryManagerRegistry.AddMemoryManager(memory);
}

uint64_t VideoManager::MapBufferEx(uint32_t gmmu, uint64_t gpuAddr, uint64_t deviceAddr, uint64_t size, uint16_t kind, bool isBigPages)
{
    std::shared_ptr<Tegra::MemoryManager> memory = impl->m_memoryManagerRegistry.GetMemoryManager(gmmu);
    if (memory == nullptr)
    {
        UNIMPLEMENTED();
        return 0;
    }
    // Preserve old mappings and renderer state until any readbacks complete.
    // The same guard surrounds GPU command lists and frame composition.
    auto execution_lock = impl->m_gpuCore->LockExecution();
    return memory->Map(gpuAddr, deviceAddr, size, (Tegra::PTEKind)kind, isBigPages);
}

uint64_t VideoManager::MapSparse(uint32_t gmmu, uint64_t gpuAddr, uint64_t size, bool isBigPages)
{
    std::shared_ptr<Tegra::MemoryManager> memory = impl->m_memoryManagerRegistry.GetMemoryManager(gmmu);
    if (memory == nullptr)
    {
        UNIMPLEMENTED();
        return 0;
    }
    // Preserve old mappings and renderer state until any readbacks complete.
    // The same guard surrounds GPU command lists and frame composition.
    auto execution_lock = impl->m_gpuCore->LockExecution();
    return memory->MapSparse(gpuAddr, size, isBigPages);
}

void VideoManager::Unmap(uint32_t gmmu, uint64_t gpuAddr, uint64_t size)
{
    std::shared_ptr<Tegra::MemoryManager> memory = impl->m_memoryManagerRegistry.GetMemoryManager(gmmu);
    if (memory == nullptr)
    {
        UNIMPLEMENTED();
        return;
    }
    // Preserve old mappings and renderer state until any readbacks complete.
    // The same guard surrounds GPU command lists and frame composition.
    auto execution_lock = impl->m_gpuCore->LockExecution();
    memory->Unmap(gpuAddr, size);
}

uint64_t VideoManager::Host1xMemoryAllocate(uint64_t size)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    return impl->m_host1x->MemoryManager().Allocate(size);
}

uint32_t VideoManager::Host1xAllocatorAllocate(uint32_t size)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    Common::FlatAllocator<u32, 0, 32> & gmmu_allocator = impl->m_host1x->Allocator();
    return gmmu_allocator.Allocate(static_cast<u32>(size));
}

void VideoManager::Host1xAllocatorFree(uint32_t address, uint32_t size)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    impl->m_host1x->Allocator().Free(address, size);
}

void VideoManager::Host1xGMMUMap(uint64_t address, uint64_t virtual_address, uint64_t size)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    auto & gmmu = impl->m_host1x->GMMU();
    gmmu.Map(static_cast<GPUVAddr>(address), virtual_address, size);
}

void VideoManager::Host1xGMMUUnmap(uint64_t address, uint64_t size)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    auto & gmmu = impl->m_host1x->GMMU();
    gmmu.Unmap(static_cast<GPUVAddr>(address), size);
}

void VideoManager::Host1xMemoryMap(uint64_t address, uint64_t virtualAddress, uint64_t size, uint64_t asid, bool track)
{
    m_debugMapCalls.fetch_add(1, std::memory_order_relaxed);
    m_debugMappingBytes.fetch_add(size, std::memory_order_relaxed);
    SCOPE_EXIT {m_debugMappingBytes.fetch_sub(size, std::memory_order_relaxed);};
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    impl->m_host1x->MemoryManager().Map(address, virtualAddress, size, Core::Asid{asid}, track);
    m_debugMappedBytes.fetch_add(size, std::memory_order_relaxed);
}

void VideoManager::Host1xMemoryUnmap(uint64_t address, uint64_t size)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    impl->m_host1x->MemoryManager().Unmap(address, size);
}

void VideoManager::Host1xFree(uint64_t regionStart, uint64_t regionSize)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    impl->m_host1x->MemoryManager().Free(regionStart, regionSize);
}

void VideoManager::Host1xMemoryTrackContinuity(uint64_t address, uint64_t virtualAddress, uint64_t size, uint64_t asid)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    impl->m_host1x->MemoryManager().TrackContinuity(address, virtualAddress, size, Core::Asid{asid});
}

uint64_t VideoManager::Host1xRegisterProcess(IMemory * memory)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    Core::Asid asid = impl->m_host1x->MemoryManager().RegisterProcess(memory);
    return asid.id;
}

void VideoManager::Host1xUnregisterProcess(uint64_t asid)
{
    std::scoped_lock memory_lock{impl->m_host1x->MemoryMutex()};
    Tegra::MaxwellDeviceMemoryManager & smmu = impl->m_host1x->MemoryManager();
    smmu.UnregisterProcess(Core::Asid{asid});
}

void VideoManager::RequestComposite(VideoFramebufferConfig * layers, uint32_t layerCount, VideoNvFence * fences, uint32_t fenceCount)
{
    m_debugPresents.fetch_add(1, std::memory_order_relaxed);
    NxemuVideoDiag(
        "Video.RequestComposite",
        std::string{"layers="} + std::to_string(layerCount) + " fences=" +
            std::to_string(fenceCount) +
            (layerCount > 0 ? (" first_addr=" + std::to_string(layers[0].address) +
                               " first_w=" + std::to_string(layers[0].width) +
                               " first_h=" + std::to_string(layers[0].height) +
                               " first_stride=" + std::to_string(layers[0].stride) +
                               " first_format=" + std::to_string(layers[0].pixelFormat))
                            : std::string{}));
    if (impl == nullptr || impl->m_gpuCore == nullptr)
    {
        return;
    }

    std::vector<Tegra::FramebufferConfig> output_layers;
    std::vector<Service::Nvidia::NvFence> output_fences;
    output_layers.reserve(layerCount);
    output_fences.reserve(fenceCount);

    for (uint32_t i = 0; i < layerCount; i++)
    {
        output_layers.emplace_back(Tegra::FramebufferConfig{
            .address = layers[i].address,
            .offset = layers[i].offset,
            .width = layers[i].width,
            .height = layers[i].height,
            .stride = layers[i].stride,
            .pixel_format = (Service::android::PixelFormat)layers[i].pixelFormat,
            .transform_flags = (Service::android::BufferTransformFlags)layers[i].transformFlags,
            .crop_rect = Common::Rectangle<int>{layers[i].cropLeft, layers[i].cropTop, layers[i].cropRight, layers[i].cropBottom },
            .blending = (Tegra::BlendMode)layers[i].blending,
            .native_tiling = layers[i].nativeTiling,
            .block_height_log2 = layers[i].blockHeightLog2,
        });
    }
    for (uint32_t i = 0; i < fenceCount; i++)
    {
        output_fences.emplace_back(Service::Nvidia::NvFence{
            .id = fences[i].id,
            .value = fences[i].value,
        });
    }
    impl->m_gpuCore->RequestComposite(std::move(output_layers), std::move(output_fences));
}

void VideoManager::WaitForComposite()
{
    if (impl->m_gpuCore)
    {
        impl->m_gpuCore->WaitForComposite();
    }
}

void VideoManager::CancelPendingComposite()
{
    if (impl->m_gpuCore)
    {
        impl->m_gpuCore->CancelPendingComposite();
    }
}

void VideoManager::UpdateFramebufferLayout(uint32_t width, uint32_t height)
{
    RenderWindow & emuWindow = impl->EnsureEmuWindow();
    emuWindow.RefreshRenderSurface();
    emuWindow.UpdateCurrentFramebufferLayout(width, height);
}

uint32_t VideoManager::GetLinearAppletCaptureBuffer(uint8_t * out, uint32_t out_size, uint32_t * out_width, uint32_t * out_height)
{
    using namespace VideoCore::Capture;
    constexpr uint32_t linear_size = LinearWidth * LinearHeight * BytesPerPixel;
    if (out_width != nullptr)
    {
        *out_width = LinearWidth;
    }
    if (out_height != nullptr)
    {
        *out_height = LinearHeight;
    }
    if (out == nullptr || out_size < linear_size)
    {
        return linear_size;
    }

    std::vector<uint8_t> tiled(TiledSize);
    GetAppletCaptureBuffer(tiled.data(), static_cast<uint32_t>(tiled.size()));
    std::vector<uint8_t> linear(linear_size);
    Tegra::Texture::UnswizzleTexture(linear, tiled, BytesPerPixel, LinearWidth, LinearHeight, LinearDepth, BlockHeight, BlockDepth);
    std::memcpy(out, linear.data(), linear_size);
    return linear_size;
}

bool VideoManager::AttachInterface(VideoInterfaceDevice * device)
{
    Vulkan::Display * display = impl->EnsureDisplay();
    return display != nullptr && device != nullptr && VideoCore::AttachInterface(*display, *device);
}

bool VideoManager::BeginInterfaceFrame(uint32_t width, uint32_t height, VideoInterfaceFrame * frame)
{
    if (!impl->m_display || frame == nullptr || width == 0 || height == 0)
    {
        return false;
    }
    return VideoCore::BeginInterfaceFrame(*impl->m_display, width, height, *frame);
}

void VideoManager::EndInterfaceFrame(const VideoInterfaceArea * gameArea)
{
    VideoCore::EndInterfaceFrame(*impl->m_display, gameArea);
}

void VideoManager::WaitInterfaceIdle()
{
    if (impl->m_display)
    {
        VideoCore::WaitInterfaceIdle(*impl->m_display);
    }
}

void VideoManager::NotifyWindowChanged()
{
    if (impl->m_emuWindow)
    {
        impl->m_emuWindow->RefreshRenderSurface();
    }
}

IChannelState * VideoManager::AllocateChannel()
{
    return std::make_unique<IChannelStatePtr>(*impl->m_gpuCore, impl->m_memoryManagerRegistry, std::move(impl->m_gpuCore->AllocateChannel())).release();
}

void VideoManager::PushGPUEntries(int32_t bindId, const uint64_t * commandList, uint32_t commandListSize, const uint32_t * prefetchCommandlist, uint32_t prefetchCommandlistSize)
{
    m_debugSubmissions.fetch_add(1, std::memory_order_relaxed);
    NxemuVideoDiag(
        "Video.PushGPUEntries",
        std::string{"bind="} + std::to_string(bindId) + " entries=" +
            std::to_string(commandListSize) + " prefetch=" +
            std::to_string(prefetchCommandlistSize));
    Tegra::CommandList entries(commandListSize);
    memcpy(entries.command_lists.data(), commandList, sizeof(uint64_t) * commandListSize);
    if (commandListSize > 0)
    {
        NxemuVideoDiag(
            "Video.PushGPUEntries.First",
            std::string{"raw=0x"} + fmt::format("{:X}", static_cast<uint64_t>(commandList[0])));
    }
    if (prefetchCommandlistSize > 0)
    {
        entries.prefetch_command_list.resize(prefetchCommandlistSize);
        memcpy(entries.prefetch_command_list.data(), prefetchCommandlist, sizeof(uint32_t) * prefetchCommandlistSize);
    }
    impl->m_gpuCore->PushGPUEntries(bindId, std::move(entries));
}

void VideoManager::PushCommandBuffer(int32_t bindId, const uint32_t * commandList, uint32_t commandListSize)
{
    Tegra::ChCommandHeaderList cmdlist(commandListSize);
    memcpy(cmdlist.data(), commandList, sizeof(Tegra::ChCommandHeader) * commandListSize);
    impl->m_gpuCore->PushCommandBuffer(bindId, cmdlist);
}

RasterizerDownloadArea VideoManager::HandleRasterizerDownload(const uint8_t * pointer, uint64_t size, RasterizerDownloadArea current_area)
{
    thread_local Common::ScratchBuffer<u32> scratch;
    RasterizerDownloadArea area = current_area;
    impl->m_host1x->MemoryManager().ApplyOpOnPointer(pointer, scratch, [&](DAddr address) {
        const DAddr end_address = address + size;
        if (current_area.native_read) {
            impl->m_gpuCore->OnNativeCPURead(address, size, current_area.shared_page);
            area = {address, end_address, false, true, current_area.shared_page};
            return;
        }
        if (area.startAddress <= address && end_address <= area.endAddress) [[likely]] {
            return;
        }
        area = impl->m_gpuCore->OnCPURead(address, size);
    });
    return area;
}

void VideoManager::HandleRasterizerWrite(const uint8_t * pointer, uint64_t size, uint64_t * last_page, RasterizerDirtyCollect collect, void * user_data)
{
    constexpr uint64_t page_bits = 12;
    thread_local Common::ScratchBuffer<u32> scratch;
    impl->m_host1x->MemoryManager().ApplyOpOnPointer(pointer, scratch, [&](DAddr address) {
        const uint64_t subaddress = address >> page_bits;
        bool do_collection = *last_page == subaddress;
        if (!do_collection) [[unlikely]]
        {
            do_collection = impl->m_gpuCore->OnCPUWrite(address, size);
            if (!do_collection)
            {
                return;
            }
            *last_page = subaddress;
        }
        collect(address, size, user_data);
    });
}

void VideoManager::InvalidateGPUMemory(const uint8_t * pointer, uint64_t size)
{
    thread_local Common::ScratchBuffer<u32> scratch;
    impl->m_host1x->MemoryManager().ApplyOpOnPointer(pointer, scratch, [&](DAddr address) {
        impl->m_gpuCore->InvalidateRegion(address, size);
    });
}

void VideoManager::DeregisterHostAction(uint32_t syncpoint_id, uint32_t handle)
{
    NxemuVideoDiag(
        "Video.HostSyncpoint.Deregister",
        std::string{"syncpt="} + std::to_string(syncpoint_id) + " handle=" +
            std::to_string(handle));
    impl->m_host1x->GetSyncpointManager().DeregisterHostAction(syncpoint_id, handle);
}

uint32_t VideoManager::HostSyncpointValue(uint32_t id)
{
    const uint32_t value = impl->m_host1x->GetSyncpointManager().GetHostSyncpointValue(id);
    NxemuVideoDiag(
        "Video.HostSyncpoint.Value",
        std::string{"syncpt="} + std::to_string(id) + " value=" + std::to_string(value));
    return value;
}

void VideoManager::HostSyncpointIncrement(uint32_t id)
{
    auto& syncpoints = impl->m_host1x->GetSyncpointManager();
    syncpoints.IncrementGuest(id);
    syncpoints.IncrementHost(id);
}

uint32_t VideoManager::HostSyncpointRegisterAction(uint32_t fence_id, uint32_t target_value, HostActionCallback operation, uint32_t slot, void* userData)
{
    NxemuVideoDiag(
        "Video.HostSyncpoint.Register",
        std::string{"syncpt="} + std::to_string(fence_id) + " target=" +
            std::to_string(target_value) + " slot=" + std::to_string(slot));
    const uint32_t handle = impl->m_host1x->GetSyncpointManager().RegisterHostAction(fence_id, target_value, [operation, userData, slot, fence_id, target_value]() {
        NxemuVideoDiag(
            "Video.HostSyncpoint.Action",
            std::string{"syncpt="} + std::to_string(fence_id) + " target=" +
                std::to_string(target_value) + " slot=" + std::to_string(slot));
        operation(slot, userData);
    });
    NxemuVideoDiag(
        "Video.HostSyncpoint.Registered",
        std::string{"syncpt="} + std::to_string(fence_id) + " target=" +
            std::to_string(target_value) + " slot=" + std::to_string(slot) +
            " handle=" + std::to_string(handle));
    return handle;
}

bool VideoManager::WaitHost(uint32_t syncpoint_id, uint32_t expected_value, std::stop_token stop)
{
    return impl->m_host1x->GetSyncpointManager().WaitHost(syncpoint_id, expected_value, stop);
}


uint32_t VideoManager::ShadersBuilding()
{
    if (impl != nullptr && impl->m_gpuCore != nullptr)
    {    
        auto & shader_notify = impl->m_gpuCore->ShaderNotify();
        return shader_notify.ShadersBuilding();
    }
    return 0;
}

bool VideoManager::UseNvdec()
{
    return impl->m_gpuCore->UseNvdec();
}

void VideoManager::PushCommandBufferWithResources(int32_t bindId, const uint32_t* commandList,
    uint32_t commandListSize, void* resources, void (*release)(void*))
{
    std::shared_ptr<void> lifetime(resources, release);
    Tegra::ChCommandHeaderList cmdlist(commandListSize);
    std::memcpy(cmdlist.data(), commandList, commandListSize * sizeof(uint32_t));
    impl->m_gpuCore->PushCommandBuffer(bindId, cmdlist, std::move(lifetime));
}

void VideoManager::ClearCdmaInstance(uint32_t id)
{
    impl->m_gpuCore->ClearCdmaInstance(id);
}

uint32_t VideoManager::GetAppletCaptureBuffer(uint8_t * out, uint32_t out_size)
{
    constexpr uint32_t required = (uint32_t)VideoCore::Capture::TiledSize;
    if (out == nullptr || out_size < required)
    {
        return required;
    }
    if (impl == nullptr || impl->m_gpuCore == nullptr)
    {
        std::memset(out, 0, required);
        return required;
    }
    std::vector<uint8_t> capture = impl->m_gpuCore->GetAppletCaptureBuffer();
    const uint32_t copy_size =  (uint32_t)(std::min<size_t>(capture.size(), required));
    if (copy_size < required)
    {
        std::memset(out + copy_size, 0, required - copy_size);
    }
    if (copy_size > 0)
    {
        std::memcpy(out, capture.data(), copy_size);
    }
    return required;
}

void VideoManager::InvalidateMemory(const uint8_t * pointer, uint64_t size)
{
    if (impl == nullptr || impl->m_gpuCore == nullptr || impl->m_host1x == nullptr || pointer == nullptr || size == 0)
    {
        return;
    }
    thread_local Common::ScratchBuffer<u32> scratch;
    impl->m_host1x->MemoryManager().ApplyOpOnPointer(pointer, scratch, [&](DAddr address) {
        impl->m_gpuCore->InvalidateRegion(address, size);
    });
}

void VideoManager::SynchronizeGpuCommands() { impl->m_gpuCore->SynchronizeCommands(); }

bool VideoManager::AddNativeSemaphoreRegion(uint32_t gmmu, uint64_t address) {
 auto memory=impl->m_memoryManagerRegistry.GetMemoryManager(gmmu);
 if(!memory)return false;
 memory->AddNativeSemaphoreRegion(address);
 return true;
}

bool VideoManager::SetNativeSyncpointShim(uint32_t gmmu, uint64_t address) {
 auto memory=impl->m_memoryManagerRegistry.GetMemoryManager(gmmu);
 if(!memory)return false;
 memory->SetNativeSyncpointShim(address);
 return true;
}

VideoDebugSnapshot VideoManager::DebugSnapshot() {
    // CPU diagnostic snapshot only: no GPU calls, extra queued work, or new thread.
    VideoCommon::ShaderProgress::Publish();
    VideoCommon::PipelineFixture::Publish();
    VideoDebugSnapshot s;
    s.map_calls = m_debugMapCalls.load(std::memory_order_relaxed);
    s.mapped_bytes = m_debugMappedBytes.load(std::memory_order_relaxed);
    s.mapping_bytes = m_debugMappingBytes.load(std::memory_order_relaxed);
    s.command_submissions = m_debugSubmissions.load(std::memory_order_relaxed);
    s.present_requests = m_debugPresents.load(std::memory_order_relaxed);
    s.composites_completed = impl && impl->m_gpuCore ? impl->m_gpuCore->CompletedComposites() : 0;
    if (impl && impl->m_gpuCore) {
        using C = Tegra::RenderDebugCounter;
        const auto& gpu = *impl->m_gpuCore;
        s.unbound_methods = gpu.DebugRead(C::UnboundMethods);
        s.dma_batches = gpu.DebugRead(C::DmaBatches);
        s.draw_requests = gpu.DebugRead(C::DrawRequests);
        s.draw_pipeline_ready = gpu.DebugRead(C::DrawPipelineReady);
        s.compute_requests = gpu.DebugRead(C::ComputeRequests);
        s.compute_pipeline_ready = gpu.DebugRead(C::ComputePipelineReady);
        s.clear_requests = gpu.DebugRead(C::ClearRequests);
        s.shaders_building = ShadersBuilding();
        s.display_accelerated = gpu.DebugRead(C::DisplayAccelerated);
        s.display_fallback = gpu.DebugRead(C::DisplayFallback);
        s.display_fallback_us = gpu.DebugRead(C::DisplayFallbackUs);
        s.display_fallback_bytes = gpu.DebugRead(C::DisplayFallbackBytes);
    }
    return s;
}

#include "yuzu_common/debug_document.h"
#include "yuzu_video_core/pipeline_fixture.h"
namespace VideoCommon::PipelineFixture {
std::string Environment(std::string_view name) { return Common::DebugDocument::Environment(std::string(name).c_str()); }
void Publish() noexcept {
    try {
        if (!Enabled()) return;
        constexpr std::uint64_t interval=500000000;
        static std::atomic<std::uint64_t> last{};
        const auto now=Now();auto previous=last.load(std::memory_order_relaxed);
        if (previous && now-previous<interval) return;
        if (!last.compare_exchange_strong(previous,now)) return;
        Common::DebugDocument::Publish("pipeline-capture",CountersJson(),false);
    } catch (...) { /* Snapshot failures never change pipeline execution. */ }
}
}
namespace VideoCommon::ShaderProgress {
bool Enabled() {
    const auto value=Common::DebugDocument::Environment("NXEMU_SHADER_PROGRESS");
    return value=="1" || (value.empty() && Common::DebugDocument::Enabled());
}
std::string RunId(){return Common::DebugDocument::Environment("NXEMU_DEBUG_RUN_ID");}
std::string Generation(){return Common::DebugDocument::Environment("NXEMU_DEBUG_GENERATION");}
void Publish() noexcept {
    try {
    if (!Enabled()) return;
    const auto started=Now();auto& r=registry;
    auto previous=r.last_publish_ns.load();
    if (previous && started-previous<PublishIntervalNs) return;
    if (!r.last_publish_ns.compare_exchange_strong(previous,started)) {++r.publish_drops;return;}
    try {
        if (const auto snapshot=Take(started)) {
            const auto json=Json(*snapshot);
            if (json.size()<=128*1024) Common::DebugDocument::Publish("shader-progress",json,false);
            else ++r.publish_drops;
        }
    } catch (...) {++r.publish_drops;}
    r.publish_ns+=Now()-started;
    } catch (...) {++registry.publish_drops;}
}
} // namespace VideoCommon::ShaderProgress
