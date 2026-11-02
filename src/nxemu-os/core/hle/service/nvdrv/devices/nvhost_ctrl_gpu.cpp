// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/core_timing.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/devices/ioctl_serialization.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl_gpu.h"
#include "core/hle/service/nvdrv/nvdrv.h"

namespace Service::Nvidia::Devices {
namespace { void ApplyNx2CharacteristicsTail(std::span<u8> block); }

nvhost_ctrl_gpu::nvhost_ctrl_gpu(Core::System& system_, EventInterface& events_interface_,
                                 NvCore::Container& core_)
    : nvdevice{system_}, events_interface{events_interface_}, core{core_} {
    error_notifier_event = events_interface.CreateEvent("CtrlGpuErrorNotifier");
    unknown_event = events_interface.CreateEvent("CtrlGpuUnknownEvent");
}
nvhost_ctrl_gpu::~nvhost_ctrl_gpu() {
    events_interface.FreeEvent(error_notifier_event);
    events_interface.FreeEvent(unknown_event);
}

NvResult nvhost_ctrl_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                                 std::span<u8> output) {
    switch (command.group) {
    case 'G':
        switch (command.cmd) {
        case 0x1:
            return WrapFixed(this, &nvhost_ctrl_gpu::ZCullGetCtxSize, input, output);
        case 0x2:
            return WrapFixed(this, &nvhost_ctrl_gpu::ZCullGetInfo, input, output);
        case 0x3:
            return WrapFixed(this, &nvhost_ctrl_gpu::ZBCSetTable, input, output);
        case 0x4:
            return WrapFixed(this, &nvhost_ctrl_gpu::ZBCQueryTable, input, output);
        case 0x5:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetCharacteristics1, input, output);
        case 0x6:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetTPCMasks1, input, output);
        case 0x7:
            return WrapFixed(this, &nvhost_ctrl_gpu::FlushL2, input, output);
        case 0x12:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetNumVsms, input, output);
        case 0x13:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetVsmsMapping, input, output);
        case 0x14:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetActiveSlotMask, input, output);
        case 0x1c:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetGpuTime, input, output);
        default:
            break;
        }
        break;
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_ctrl_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                                 std::span<const u8> inline_input, std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_ctrl_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                                 std::span<u8> output, std::span<u8> inline_output) {
    switch (command.group) {
    case 'G':
        switch (command.cmd) {
        case 0x5:
            {const auto result = WrapFixedInlOut(this, &nvhost_ctrl_gpu::GetCharacteristics3, input, output, inline_output);
             if (nx2_caller && result == NvResult::Success) {
                 ApplyNx2CharacteristicsTail(inline_output);
             }
             return result;}

        case 0x6:
            return WrapFixedInlOut(this, &nvhost_ctrl_gpu::GetTPCMasks3, input, output,
                                   inline_output);
        case 0x12:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetNumVsms, input, output);
        case 0x13:
            return WrapFixed(this, &nvhost_ctrl_gpu::GetVsmsMapping, input, output);
        default:
            break;
        }
        break;
    default:
        break;
    }
    if (nx2_caller && command.group == 'G' && command.cmd == 0x1f) {
        return Nx2GetEngineInfo(input, output, inline_output);
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

// nvgpu GET_ENGINE_INFO as the Switch 2 NvRm uses it: args {u32 buf_size, u32 reserved,
// u64 buf_addr}, queried once for the size and once with an inline output buffer of 16-byte items
// {u32 engine_id, u32 engine_instance, s32 runlist_id, u32 reserved}. NvRm picks the channel
// runlist from this list. Report the graphics engine (runlist 0) and an async copy engine (1).
NvResult nvhost_ctrl_gpu::Nx2GetEngineInfo(std::span<const u8> input, std::span<u8> output,
                                           std::span<u8> inline_output) {
    struct Item {
        u32 engine_id;
        u32 engine_instance;
        s32 runlist_id;
        u32 reserved;
    };
    static constexpr std::array<Item, 2> items{{
        {0, 0, 0, 0}, // NVGPU_ENGINE_GR
        {2, 0, 1, 0}, // NVGPU_ENGINE_ASYNC_COPY
    }};
    constexpr u32 needed = static_cast<u32>(items.size() * sizeof(Item));

    u32 buf_size{};
    if (input.size() >= sizeof(buf_size)) {
        std::memcpy(&buf_size, input.data(), sizeof(buf_size));
    }
    LOG_WARNING(Service_NVDRV, "(STUBBED) Switch 2 GetEngineInfo buf_size={:#x} inline_out={:#x}",
                buf_size, inline_output.size());
    if (buf_size >= needed && inline_output.size() >= needed) {
        std::memcpy(inline_output.data(), items.data(), needed);
    }
    std::fill(output.begin(), output.end(), u8{0});
    std::copy_n(input.begin(), std::min(input.size(), output.size()), output.begin());
    if (output.size() >= sizeof(needed)) {
        std::memcpy(output.data(), &needed, sizeof(needed));
    }
    return NvResult::Success;
}

void nvhost_ctrl_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {
    const auto session = core.GetSessionReference(session_id);
    nx2_caller = session && session->process && session->process->IsSwitch2();
}
void nvhost_ctrl_gpu::OnClose(DeviceFD fd) {}

namespace {

// Switch 2 SDKs ask for a 0xD0-byte characteristics block (Switch 1: 0xA0). Their NvRm only
// accepts chip ids 0x17E, 0x12B and 0x12E and its NVN path rejects the Maxwell ones, so report the
// Switch 2 Ampere chip. Fields past the Switch 1 layout are left zero. Values are best guesses.
constexpr u64 Nx2CharacteristicsSize = 0xD0;

template <typename GC>
void ApplyNx2Characteristics(GC& gc) {
    gc.arch = 0x170;               // Ampere
    gc.impl = 0xE;                 // chip id 0x17E as listed in the Switch 2 NvRm chip table
    gc.num_tpc_per_gpc = 0x6;
    gc.sm_arch_sm_version = 0x808; // NVN2 only accepts SM 0x808 (its class table lookup)
    gc.sm_arch_spa_version = 0x808;
    gc.threed_class = 0xC797;      // AMPERE_B
    gc.compute_class = 0xC7C0;     // AMPERE_COMPUTE_B
    gc.gpfifo_class = 0xC56F;      // AMPERE_CHANNEL_GPFIFO_A
    gc.dma_copy_class = 0xC7B5;    // AMPERE_DMA_COPY_B
    gc.chipname = 0x6631306167;    // "ga10f"
    gc.flags |= 0x100;             // NVGPU_GPU_FLAGS_SUPPORT_TSG: NvRm binds channels to a TSG
    gc.flags |= 1ULL << 22;        // NVGPU_GPU_FLAGS_SUPPORT_TSG_SUBCONTEXTS (see below)
    gc.flags |= 1ULL << 26;        // NVGPU_GPU_FLAGS_SUPPORT_SYNCPOINT_ADDRESS: syncpoint shim
    // NVGPU_GPU_FLAGS_SUPPORT_MAPPING_MODIFY: NVN2 gives a texture in a compressible pool its
    // address only after NvRmGpuMappingModify switched the pool's mapping to the texture's kind.
    // Without the flag NvRm fails that call and the texture stays at GPU address 0 (title 1's
    // movie and the passes rendered into compressible pools never showed).
    gc.flags |= 1ULL << 34;
    // Native SDK filters the 2D/3D/compute/inline class families using bits40..43.
    // Merely writing Ampere class IDs does not make those classes enumerable.
    // Match the running Eden native ABI; without these bits the SDK binds class0.
    gc.flags |= 0xfULL << 40;

    // The Switch 2 layout keeps nine s16 interface versions at 0x70..0x80 (like nvgpu's
    // *_ioctl_nr_last); its NvRm requires e.g. [0x78] >= 6 and enables features above 0xa, 0x1f
    // and 0x26. In the Switch 1 layout these bytes are max_fbps_count..max_tex_per_tpc.
    auto* const raw = reinterpret_cast<u8*>(&gc);
    for (size_t off = 0x70; off <= 0x80; off += 2) {
        const s16 version = 0x40;
        std::memcpy(raw + off, &version, sizeof(version));
    }
}

// Fields of the Switch 2 block past the Switch 1 layout. NvRm sizes each TSG's subcontext (VEID)
// allocator from the u32 at 0xb4, but only when the TSG interface version (0x72) is above 0xa and
// flags bits 8 and 22 are set; otherwise every TSG gets one subcontext and a second queue's
// NvRmGpuTSGSubcontextCreate fails (0x11), leaving that NVN queue without command memory.
void ApplyNx2CharacteristicsTail(std::span<u8> block) {
    constexpr size_t MaxSubcontextsOffset = 0xb4;
    constexpr u32 MaxSubcontexts = 64; // Ampere VEIDs per TSG
    if (block.size() >= MaxSubcontextsOffset + sizeof(MaxSubcontexts)) {
        std::memcpy(block.data() + MaxSubcontextsOffset, &MaxSubcontexts, sizeof(MaxSubcontexts));
    }
}

} // namespace

NvResult nvhost_ctrl_gpu::GetCharacteristics1(IoctlCharacteristics& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.gc.arch = 0x120;
    params.gc.impl = 0xb;
    params.gc.rev = 0xa1;
    params.gc.num_gpc = 0x1;
    params.gc.l2_cache_size = 0x40000;
    params.gc.on_board_video_memory_size = 0x0;
    params.gc.num_tpc_per_gpc = 0x2;
    params.gc.bus_type = 0x20;
    params.gc.big_page_size = 0x20000;
    params.gc.compression_page_size = 0x20000;
    params.gc.pde_coverage_bit_count = 0x1B;
    params.gc.available_big_page_sizes = 0x30000;
    params.gc.gpc_mask = 0x1;
    params.gc.sm_arch_sm_version = 0x503;
    params.gc.sm_arch_spa_version = 0x503;
    params.gc.sm_arch_warp_count = 0x80;
    params.gc.gpu_va_bit_count = 0x28;
    params.gc.reserved = 0x0;
    params.gc.flags = 0x55;
    params.gc.twod_class = 0x902D;
    params.gc.threed_class = 0xB197;
    params.gc.compute_class = 0xB1C0;
    params.gc.gpfifo_class = 0xB06F;
    params.gc.inline_to_memory_class = 0xA140;
    params.gc.dma_copy_class = 0xB0B5;
    params.gc.max_fbps_count = 0x1;
    params.gc.fbp_en_mask = 0x0;
    params.gc.max_ltc_per_fbp = 0x2;
    params.gc.max_lts_per_ltc = 0x1;
    params.gc.max_tex_per_tpc = 0x0;
    params.gc.max_gpc_count = 0x1;
    params.gc.rop_l2_en_mask_0 = 0x21D70;
    params.gc.rop_l2_en_mask_1 = 0x0;
    params.gc.chipname = 0x6230326D67;
    params.gc.gr_compbit_store_base_hw = 0x0;
    params.gpu_characteristics_buf_size = 0xA0;
    params.gpu_characteristics_buf_addr = 0xdeadbeef; // Cannot be 0 (UNUSED)
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetCharacteristics3(
    IoctlCharacteristics& params, std::span<IoctlGpuCharacteristics> gpu_characteristics) {
    LOG_DEBUG(Service_NVDRV, "called");

    params.gc.arch = 0x120;
    params.gc.impl = 0xb;
    params.gc.rev = 0xa1;
    params.gc.num_gpc = 0x1;
    params.gc.l2_cache_size = 0x40000;
    params.gc.on_board_video_memory_size = 0x0;
    params.gc.num_tpc_per_gpc = 0x2;
    params.gc.bus_type = 0x20;
    params.gc.big_page_size = 0x20000;
    params.gc.compression_page_size = 0x20000;
    params.gc.pde_coverage_bit_count = 0x1B;
    params.gc.available_big_page_sizes = 0x30000;
    params.gc.gpc_mask = 0x1;
    params.gc.sm_arch_sm_version = 0x503;
    params.gc.sm_arch_spa_version = 0x503;
    params.gc.sm_arch_warp_count = 0x80;
    params.gc.gpu_va_bit_count = 0x28;
    params.gc.reserved = 0x0;
    params.gc.flags = 0x55;
    params.gc.twod_class = 0x902D;
    params.gc.threed_class = 0xB197;
    params.gc.compute_class = 0xB1C0;
    params.gc.gpfifo_class = 0xB06F;
    params.gc.inline_to_memory_class = 0xA140;
    params.gc.dma_copy_class = 0xB0B5;
    params.gc.max_fbps_count = 0x1;
    params.gc.fbp_en_mask = 0x0;
    params.gc.max_ltc_per_fbp = 0x2;
    params.gc.max_lts_per_ltc = 0x1;
    params.gc.max_tex_per_tpc = 0x0;
    params.gc.max_gpc_count = 0x1;
    params.gc.rop_l2_en_mask_0 = 0x21D70;
    params.gc.rop_l2_en_mask_1 = 0x0;
    params.gc.chipname = 0x6230326D67;
    params.gc.gr_compbit_store_base_hw = 0x0;
    params.gpu_characteristics_buf_size = 0xA0;
    params.gpu_characteristics_buf_addr = 0xdeadbeef; // Cannot be 0 (UNUSED)
    if (nx2_caller) {
        ApplyNx2Characteristics(params.gc);
        params.gpu_characteristics_buf_size = Nx2CharacteristicsSize;
        LOG_INFO(Service_NVDRV, "Switch 2 characteristics: Ampere chip=0x17e SM=0x808 size=0xd0 native-class-families=0xf");
    }
    if (!gpu_characteristics.empty()) {
        gpu_characteristics.front() = params.gc;
    }
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetTPCMasks1(IoctlGpuGetTpcMasksArgs& params) {
    LOG_DEBUG(Service_NVDRV, "called, mask_buffer_size=0x{:X}", params.mask_buffer_size);
    if (params.mask_buffer_size != 0) {
        params.tcp_mask = 3;
    }
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetTPCMasks3(IoctlGpuGetTpcMasksArgs& params, std::span<u32> tpc_mask) {
    LOG_DEBUG(Service_NVDRV, "called, mask_buffer_size=0x{:X}", params.mask_buffer_size);
    if (params.mask_buffer_size != 0) {
        params.tcp_mask = 3;
    }
    if (!tpc_mask.empty()) {
        tpc_mask.front() = params.tcp_mask;
    }
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetNumVsms(IoctlNumVsms& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.num_vsms = 2;
    params.reserved = 0;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetVsmsMapping(IoctlVsmsMapping& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.sm0_gpc_index = 0;
    params.sm0_tpc_index = 0;
    params.sm1_gpc_index = 0;
    params.sm1_tpc_index = 1;
    params.reserved = 0;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetActiveSlotMask(IoctlActiveSlotMask& params) {
    LOG_DEBUG(Service_NVDRV, "called");

    params.slot = 0x07;
    params.mask = 0x01;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::ZCullGetCtxSize(IoctlZcullGetCtxSize& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.size = 0x1;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::ZCullGetInfo(IoctlNvgpuGpuZcullGetInfoArgs& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.width_align_pixels = 0x20;
    params.height_align_pixels = 0x20;
    params.pixel_squares_by_aliquots = 0x400;
    params.aliquot_total = 0x800;
    params.region_byte_multiplier = 0x20;
    params.region_header_size = 0x20;
    params.subregion_header_size = 0xc0;
    params.subregion_width_align_pixels = 0x20;
    params.subregion_height_align_pixels = 0x40;
    params.subregion_count = 0x10;
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::ZBCSetTable(IoctlZbcSetTable& params) {
    LOG_DEBUG(Service_NVDRV, "called, type={}, format={:#x}", params.type, params.format);

    auto& zbc = core.Zbc();
    std::scoped_lock lk{zbc.mutex};

    switch (static_cast<u32>(params.type)) {
    case 1: {
        for (u32 i = 0; i < zbc.max_used_color_index; ++i) {
            auto& entry = zbc.color_table[i];
            if (entry.ref_cnt == 0 || entry.format != params.format ||
                !std::equal(entry.color_ds.begin(), entry.color_ds.end(), params.color_ds)) {
                continue;
            }

            if (!std::equal(entry.color_l2.begin(), entry.color_l2.end(), params.color_l2)) {
                return NvResult::BadParameter;
            }

            ++entry.ref_cnt;
            return NvResult::Success;
        }

        if (zbc.max_used_color_index >= NvCore::Container::ZbcState::TableSize) {
            return NvResult::InsufficientMemory;
        }

        auto& entry = zbc.color_table[zbc.max_used_color_index++];
        std::copy_n(params.color_ds, entry.color_ds.size(), entry.color_ds.begin());
        std::copy_n(params.color_l2, entry.color_l2.size(), entry.color_l2.begin());
        entry.format = params.format;
        entry.ref_cnt = 1;
        return NvResult::Success;
    }
    case 2: {
        for (u32 i = 0; i < zbc.max_used_depth_index; ++i) {
            auto& entry = zbc.depth_table[i];
            if (entry.ref_cnt != 0 && entry.depth == params.depth && entry.format == params.format) {
                ++entry.ref_cnt;
                return NvResult::Success;
            }
        }

        if (zbc.max_used_depth_index >= NvCore::Container::ZbcState::TableSize) {
            return NvResult::InsufficientMemory;
        }

        auto& entry = zbc.depth_table[zbc.max_used_depth_index++];
        entry.depth = params.depth;
        entry.format = params.format;
        entry.ref_cnt = 1;
        return NvResult::Success;
    }
    default:
        return NvResult::BadParameter;
    }
}

NvResult nvhost_ctrl_gpu::ZBCQueryTable(IoctlZbcQueryTable& params) {
    LOG_DEBUG(Service_NVDRV, "called, type={}, index={}", params.type, params.index_size);

    auto& zbc = core.Zbc();
    std::scoped_lock lk{zbc.mutex};
    const u32 type = params.type;
    const u32 index = params.index_size;

    if (type == 0) {
        params.index_size = NvCore::Container::ZbcState::TableSize;
        return NvResult::Success;
    }
    if (index >= NvCore::Container::ZbcState::TableSize) {
        return NvResult::BadParameter;
    }

    if (type == 1) {
        const auto& entry = zbc.color_table[index];
        std::copy_n(entry.color_ds.begin(), entry.color_ds.size(), params.color_ds);
        std::copy_n(entry.color_l2.begin(), entry.color_l2.size(), params.color_l2);
        params.format = entry.format;
        params.ref_cnt = entry.ref_cnt;
        return NvResult::Success;
    }
    if (type == 2) {
        const auto& entry = zbc.depth_table[index];
        params.depth = entry.depth;
        params.format = entry.format;
        params.ref_cnt = entry.ref_cnt;
        return NvResult::Success;
    }

    return NvResult::BadParameter;
}

NvResult nvhost_ctrl_gpu::FlushL2(IoctlFlushL2& params) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) called");
    return NvResult::Success;
}

NvResult nvhost_ctrl_gpu::GetGpuTime(IoctlGetGpuTime& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.gpu_time = static_cast<u64_le>(system.CoreTiming().GetGlobalTimeNs().count());
    return NvResult::Success;
}

Kernel::KEvent* nvhost_ctrl_gpu::QueryEvent(u32 event_id) {
    switch (event_id) {
    case 1:
        return error_notifier_event;
    case 2:
        return unknown_event;
    default:
        LOG_CRITICAL(Service_NVDRV, "Unknown Ctrl GPU Event {}", event_id);
        return nullptr;
    }
}

} // namespace Service::Nvidia::Devices
