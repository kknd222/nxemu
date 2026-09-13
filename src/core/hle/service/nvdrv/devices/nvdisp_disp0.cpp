// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>

#include "yuzu_common/nvdata.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/yuzu_assert.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/perf_stats.h"
#include <nxemu-module-spec/video.h>
#if defined(ANDROID) || defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace Service::Nvidia::Devices {

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

Tegra::BlendMode ConvertBlending(Service::Nvnflinger::LayerBlending blending) {
#if defined(ANDROID) || defined(__ANDROID__)
    // Most Switch titles present a single fullscreen layer. On Android the main framebuffer alpha
    // channel can contain transient/undefined values from effects (Kirby water/particles), and
    // respecting HWC coverage blending exposes the blue clear layer until opaque geometry redraws
    // it. PC presentation effectively treats this fullscreen layer as opaque, so do the same by
    // default on Android; keep a property for A/B testing overlay regressions.
    if (ReadAndroidBoolProperty("debug.nxemu.force_opaque_layers", true)) {
        return Tegra::BlendMode::Opaque;
    }
#endif
    switch (blending) {
    case Service::Nvnflinger::LayerBlending::None:
    default:
        return Tegra::BlendMode::Opaque;
    case Service::Nvnflinger::LayerBlending::Premultiplied:
        return Tegra::BlendMode::Premultiplied;
    case Service::Nvnflinger::LayerBlending::Coverage:
        return Tegra::BlendMode::Coverage;
    }
}

} // namespace

nvdisp_disp0::nvdisp_disp0(Core::System& system_, NvCore::Container& core)
    : nvdevice{system_}, container{core}, nvmap{core.GetNvMapFile()} {}
nvdisp_disp0::~nvdisp_disp0() = default;

NvResult nvdisp_disp0::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                              std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvdisp_disp0::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                              std::span<const u8> inline_input, std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvdisp_disp0::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                              std::span<u8> output, std::span<u8> inline_output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvdisp_disp0::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {}
void nvdisp_disp0::OnClose(DeviceFD fd) {}

void nvdisp_disp0::Composite(std::span<const Nvnflinger::HwcLayer> sorted_layers) {
    std::vector<VideoFramebufferConfig> output_layers;
    std::vector<VideoNvFence> output_fences;
    output_layers.reserve(sorted_layers.size());
    output_fences.reserve(sorted_layers.size());

    for (auto& layer : sorted_layers) {
        output_layers.emplace_back(VideoFramebufferConfig{
            .address = nvmap.GetHandleAddress(layer.buffer_handle),
            .offset = layer.offset,
            .width = layer.width,
            .height = layer.height,
            .stride = layer.stride,
            .pixelFormat = (uint32_t)layer.format,
            .transformFlags = (uint32_t)layer.transform,
            .cropLeft = layer.crop_rect.left,
            .cropTop = layer.crop_rect.top,
            .cropRight = layer.crop_rect.right,
            .cropBottom = layer.crop_rect.bottom,
            .blending = (uint32_t)ConvertBlending(layer.blending),
        });

        for (size_t i = 0; i < layer.acquire_fence.num_fences; i++) {
            output_fences.emplace_back(VideoNvFence{
                .id = layer.acquire_fence.fences[i].id,
                .value = layer.acquire_fence.fences[i].value
            });
        }
    }

    system.GetVideo().RequestComposite(output_layers.data(), (uint32_t)output_layers.size(), output_fences.data(), (uint32_t)output_fences.size());
    system.SpeedLimiter().DoSpeedLimiting(system.CoreTiming().GetGlobalTimeUs());
    system.GetPerfStats().EndSystemFrame();
    system.GetPerfStats().BeginSystemFrame();
}

Kernel::KEvent* nvdisp_disp0::QueryEvent(u32 event_id) {
    LOG_CRITICAL(Service_NVDRV, "Unknown DISP Event {}", event_id);
    return nullptr;
}

} // namespace Service::Nvidia::Devices
