// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <fmt/format.h>
#include "yuzu_common/interface_pointer_def.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/yuzu_assert.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/ioctl_serialization.h"
#include "core/hle/service/nvdrv/devices/nvhost_gpu.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nxemu_android_diagnostics.h"
#include "core/memory.h"

namespace Tegra {

// Note that, traditionally, methods are treated as 4-byte addressable locations, and hence
// their numbers are written down multiplied by 4 in Docs. Here we are not multiply by 4.
// So the values you see in docs might be multiplied by 4.
// Register documentation:
// https://github.com/NVIDIA/open-gpu-doc/blob/ab27fc22db5de0d02a4cabe08e555663b62db4d4/classes/host/cla26f.h
//
// Register Description (approx):
// https://github.com/NVIDIA/open-gpu-doc/blob/ab27fc22db5de0d02a4cabe08e555663b62db4d4/manuals/volta/gv100/dev_pbdma.ref.txt
enum class BufferMethods : u32 {
    BindObject = 0x0,
    Illegal = 0x1,
    Nop = 0x2,
    SemaphoreAddressHigh = 0x4,
    SemaphoreAddressLow = 0x5,
    SemaphoreSequencePayload = 0x6,
    SemaphoreOperation = 0x7,
    NonStallInterrupt = 0x8,
    WrcacheFlush = 0x9,
    MemOpA = 0xA,
    MemOpB = 0xB,
    MemOpC = 0xC,
    MemOpD = 0xD,
    RefCnt = 0x14,
    SemaphoreAcquire = 0x1A,
    SemaphoreRelease = 0x1B,
    SyncpointPayload = 0x1C,
    SyncpointOperation = 0x1D,
    WaitForIdle = 0x1E,
    CRCCheck = 0x1F,
    Yield = 0x20,
    NonPullerMethods = 0x40,
};

inline CommandHeader BuildCommandHeader(BufferMethods method, u32 arg_count, SubmissionMode mode) {
    CommandHeader result{};
    result.method.Assign(static_cast<u32>(method));
    result.arg_count.Assign(arg_count);
    result.mode.Assign(mode);
    return result;
}
} // namespace Tegra

namespace Tegra::Engines {

enum class FenceOperation : u32 {
    Acquire = 0,
    Increment = 1,
};

union FenceAction {
    u32 raw;
    BitField<0, 1, FenceOperation> op;
    BitField<8, 24, u32> syncpoint_id;
};

} // namespace Tegra::Engines

namespace Service::Nvidia::Devices {
namespace {
Tegra::CommandHeader BuildFenceAction(Tegra::Engines::FenceOperation op, u32 syncpoint_id) {
    Tegra::Engines::FenceAction result{};
    result.op.Assign(op);
    result.syncpoint_id.Assign(syncpoint_id);
    return {result.raw};
}
} // namespace

nvhost_gpu::nvhost_gpu(Core::System& system_, EventInterface& events_interface_,
                       NvCore::Container& core_)
    : nvdevice{system_}, events_interface{events_interface_}, core{core_},
      syncpoint_manager{core_.GetSyncpointManager()}, nvmap{core.GetNvMapFile()},
      channel_state{system_.GetVideo().AllocateChannel()} {
    channel_syncpoint = syncpoint_manager.AllocateSyncpoint(false);
    sm_exception_breakpoint_int_report_event =
        events_interface.CreateEvent("GpuChannelSMExceptionBreakpointInt");
    sm_exception_breakpoint_pause_report_event =
        events_interface.CreateEvent("GpuChannelSMExceptionBreakpointPause");
    error_notifier_event = events_interface.CreateEvent("GpuChannelErrorNotifier");
}

nvhost_gpu::~nvhost_gpu() {
    events_interface.FreeEvent(sm_exception_breakpoint_int_report_event);
    events_interface.FreeEvent(sm_exception_breakpoint_pause_report_event);
    events_interface.FreeEvent(error_notifier_event);
    syncpoint_manager.FreeSyncpoint(channel_syncpoint);
    for(auto&[id,event]:nx2_events)events_interface.FreeEvent(event);
}

NvResult nvhost_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<u8> output) {
    if(nx2_caller && command.group == 'H' && command.cmd == 0x20){
        LOG_WARNING(Service_NVDRV,"(STUBBED) Switch 2 channel ioctl 0x20");
        std::fill(output.begin(),output.end(),u8{});std::copy_n(input.begin(),std::min(input.size(),output.size()),output.begin());return NvResult::Success;
    }
    switch (command.group) {
    case 0x0:
        switch (command.cmd) {
        case 0x3:
            return WrapFixed(this, &nvhost_gpu::GetWaitbase, input, output);
        default:
            break;
        }
        break;
    case 'H':
        switch (command.cmd) {
        case 0x1:
            return WrapFixed(this, &nvhost_gpu::SetNVMAPfd, input, output);
        case 0x3:
            return WrapFixed(this, &nvhost_gpu::ChannelSetTimeout, input, output);
        case 0x8:
            return WrapFixedVariable(this, &nvhost_gpu::SubmitGPFIFOBase1, input, output, fd, false);
        case 0x9:
            return WrapFixed(this, &nvhost_gpu::AllocateObjectContext, input, output);
        case 0xb:
            return WrapFixed(this, &nvhost_gpu::ZCullBind, input, output);
        case 0xc:
            return WrapFixed(this, &nvhost_gpu::SetErrorNotifier, input, output);
        case 0xd:
            return WrapFixed(this, &nvhost_gpu::SetChannelPriority, input, output);
        case 0x18:
            return WrapFixed(this, &nvhost_gpu::AllocGPFIFOEx, input, output, fd);
        case 0x1a:
            return WrapFixed(this, &nvhost_gpu::AllocGPFIFOEx2, input, output, fd);
        case 0x1b:
            return WrapFixedVariable(this, &nvhost_gpu::SubmitGPFIFOBase1, input, output, fd, true);
        case 0x1d:
            return WrapFixed(this, &nvhost_gpu::ChannelSetTimeslice, input, output);
        default:
            break;
        }
        break;
    case 'G':
        switch (command.cmd) {
        case 0x14:
            return WrapFixed(this, &nvhost_gpu::SetClientData, input, output);
        case 0x15:
            return WrapFixed(this, &nvhost_gpu::GetClientData, input, output);
        default:
            break;
        }
        break;
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
};

NvResult nvhost_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<const u8> inline_input, std::span<u8> output) {
    switch (command.group) {
    case 'H':
        switch (command.cmd) {
        case 0x1b:
            return WrapFixedInlIn(this, &nvhost_gpu::SubmitGPFIFOBase2, input, inline_input,
                                  output);
        }
        break;
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                            std::span<u8> output, std::span<u8> inline_output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvhost_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {
    sessions[fd] = core.GetSessionReference(session_id);
    const auto session=sessions[fd].lock();
    nx2_caller=session && session->process && session->process->IsSwitch2();
}

void nvhost_gpu::OnClose(DeviceFD fd) {
    sessions.erase(fd);
    error_notifier_enabled = false;
}

NvResult nvhost_gpu::SetNVMAPfd(IoctlSetNvmapFD& params) {
    LOG_DEBUG(Service_NVDRV, "called, fd={}", params.nvmap_fd);

    nvmap_fd = params.nvmap_fd;
    return NvResult::Success;
}

NvResult nvhost_gpu::SetClientData(IoctlClientData& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    user_data = params.data;
    return NvResult::Success;
}

NvResult nvhost_gpu::GetClientData(IoctlClientData& params) {
    LOG_DEBUG(Service_NVDRV, "called");
    params.data = user_data;
    return NvResult::Success;
}

NvResult nvhost_gpu::ZCullBind(IoctlZCullBind& params) {
    zcull_params = params;
    LOG_DEBUG(Service_NVDRV, "called, gpu_va={:X}, mode={:X}", zcull_params.gpu_va,
              zcull_params.mode);
    return NvResult::Success;
}

NvResult nvhost_gpu::SetErrorNotifier(IoctlSetErrorNotifier& params) {
    error_notifier_enabled = params.mem != 0;
    LOG_DEBUG(Service_NVDRV, "called, enabled={}, offset={:X}, size={:X}",
              error_notifier_enabled, params.offset, params.size);
    return NvResult::Success;
}

NvResult nvhost_gpu::SetChannelPriority(IoctlChannelSetPriority& params) {
    u32 timeslice{};
    switch (static_cast<ChannelPriority>(params.priority)) {
    case ChannelPriority::Low:
        timeslice = 1300;
        break;
    case ChannelPriority::Medium:
        timeslice = 2600;
        break;
    case ChannelPriority::High:
        timeslice = 5200;
        break;
    default:
        return NvResult::BadParameter;
    }

    channel_priority = params.priority;
    channel_timeslice = timeslice;
    LOG_DEBUG(Service_NVDRV, "called, priority={:X}, timeslice={}", channel_priority,
              channel_timeslice);
    return NvResult::Success;
}

NvResult nvhost_gpu::AllocGPFIFOEx(IoctlAllocGpfifoEx& params, DeviceFD fd) {
    LOG_DEBUG(Service_NVDRV,
              "called, num_entries={:X}, num_jobs={:X}, flags={:X}, reserved1={:X}, "
              "reserved2={:X}, reserved3={:X}",
              params.num_entries, params.num_jobs, params.flags, params.reserved[0],
              params.reserved[1], params.reserved[2]);

    if (channel_state->Initialized()) {
        LOG_DEBUG(Service_NVDRV, "Channel already initialized; returning AlreadyAllocated");
        return NvResult::AlreadyAllocated;
    }

    const auto session = GetSessionReference(fd);
    if (!session) return NvResult::InvalidState;
    const u64 program_id = session->process->GetProgramId();

    // Init() now safely defers if the address space has not been bound yet.
    channel_state->Init(program_id);
    params.fence_out = syncpoint_manager.GetSyncpointFence(channel_syncpoint);
    return NvResult::Success;
}

NvResult nvhost_gpu::AllocGPFIFOEx2(IoctlAllocGpfifoEx& params, DeviceFD fd) {
    NxemuAndroidDiagnostics::RecordEvent("NVDRV.nvhost_gpu.AllocGPFIFOEx2",
        "fd=" + std::to_string(fd) + " entries=" + std::to_string(params.num_entries) +
        " flags=" + std::to_string(params.flags));
    return AllocGPFIFOEx(params, fd);
}

NvResult nvhost_gpu::AllocateObjectContext(IoctlAllocObjCtx& params) {
    LOG_INFO(Service_NVDRV, "called, class_num={:X}, flags={:X}, obj_id={:X}", params.class_num,
             params.flags, params.obj_id);
    NxemuAndroidDiagnostics::RecordEvent(
        "NVDRV.nvhost_gpu.AllocateObjectContext",
        "class=0x" + fmt::format("{:X}", static_cast<u32>(params.class_num)) +
            " flags=" + std::to_string(params.flags));

    params.obj_id = 0x0;
    return NvResult::Success;
}

static boost::container::small_vector<Tegra::CommandHeader, 512> BuildWaitCommandList(
    NvFence fence) {
    return {
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointPayload, 1,
                                  Tegra::SubmissionMode::Increasing),
        {fence.value},
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointOperation, 1,
                                  Tegra::SubmissionMode::Increasing),
        BuildFenceAction(Tegra::Engines::FenceOperation::Acquire, fence.id),
    };
}

static boost::container::small_vector<Tegra::CommandHeader, 512> BuildIncrementCommandList(
    NvFence fence, u32 count = 2) {
    boost::container::small_vector<Tegra::CommandHeader, 512> result{
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointPayload, 1,
                                  Tegra::SubmissionMode::Increasing),
        {}};

    for (u32 index = 0; index < count; ++index) {
        result.push_back(Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointOperation, 1,
                                                   Tegra::SubmissionMode::Increasing));
        result.push_back(
            BuildFenceAction(Tegra::Engines::FenceOperation::Increment, fence.id));
    }

    return result;
}

static boost::container::small_vector<Tegra::CommandHeader, 512> BuildIncrementWithWfiCommandList(
    NvFence fence, u32 count = 2) {
    boost::container::small_vector<Tegra::CommandHeader, 512> result{
        Tegra::BuildCommandHeader(Tegra::BufferMethods::WaitForIdle, 1,
                                  Tegra::SubmissionMode::Increasing),
        {}};
    auto increment_list{BuildIncrementCommandList(fence, count)};
    result.insert(result.end(), increment_list.begin(), increment_list.end());
    return result;
}

NvResult nvhost_gpu::SubmitGPFIFOImpl(IoctlSubmitGpfifo& params, Tegra::CommandList&& entries) {
    LOG_TRACE(Service_NVDRV, "called, gpfifo={:X}, num_entries={:X}, flags={:X}", params.address,
              params.num_entries, params.flags.raw);
    NxemuAndroidDiagnostics::RecordEvent(
        "NVDRV.nvhost_gpu.SubmitGPFIFO",
        "addr=0x" + fmt::format("{:X}", static_cast<u64>(params.address)) +
            " entries=" + std::to_string(params.num_entries) +
            " flags=" + std::to_string(params.flags.raw) +
            " wait=" + std::to_string(params.flags.fence_wait.Value()) +
            " incr=" + std::to_string(params.flags.fence_increment.Value()) +
            " suppress_wfi=" + std::to_string(params.flags.suppress_wfi.Value()) +
            " incr_value=" + std::to_string(params.flags.increment_value.Value()) +
            " in_fence_id=" + std::to_string(params.fence.id) +
            " in_fence_value=" + std::to_string(params.fence.value));

    IVideo & video = system.GetVideo();

    std::scoped_lock lock(channel_mutex);

    if (!channel_state->Initialized()) {
        LOG_ERROR(Service_NVDRV,
                  "SubmitGPFIFO before channel init; address space may not be bound yet");
        return NvResult::InvalidState;
    }

    const auto bind_id = channel_state->BindId();

    auto& flags = params.flags;

    if (flags.fence_wait.Value()) {
        if (flags.increment_value.Value()) {
            return NvResult::BadParameter;
        }
        if (params.fence.id < 0 || static_cast<u32>(params.fence.id) >= MaxSyncPoints) {
            return NvResult::BadParameter;
        }

        const bool fence_signalled = syncpoint_manager.IsFenceSignalled(params.fence);
        NxemuAndroidDiagnostics::RecordEvent(
            "NVDRV.nvhost_gpu.FenceWait",
            "id=" + std::to_string(params.fence.id) +
                " value=" + std::to_string(params.fence.value) +
                " signalled=" + (fence_signalled ? std::string{"true"} : std::string{"false"}));
        if (!fence_signalled) {
            Tegra::CommandList entries{ BuildWaitCommandList(params.fence) };
            video.PushGPUEntries(bind_id, (const uint64_t *)entries.command_lists.data(), (uint32_t)entries.command_lists.size(), (const uint32_t*)entries.prefetch_command_list.data(), (uint32_t)entries.prefetch_command_list.size());
        }
    }

    // C56F native submissions report a completion count, without host syncpoint methods.
    // Legacy increment_value streams retain their original explicit-increment behavior.
    const u32 native_completion = nx2_caller && flags.increment_value.Value() &&
                                  !flags.fence_increment.Value() ? params.fence.value : 0;
    if (native_completion > 64) return NvResult::BadParameter;
    params.fence.id = channel_syncpoint;

    u32 increment{(flags.fence_increment.Value() != 0 ? 2 : 0) +
                  (flags.increment_value.Value() != 0 ? params.fence.value : 0)};
    if (increment == 0 && params.num_entries > 0 &&
        flags.raw == (static_cast<u32>(params.flags.suppress_wfi.Value()) |
                      (static_cast<u32>(params.flags.increment_value.Value()) << 8))) {
        NxemuAndroidDiagnostics::RecordEvent(
            "NVDRV.nvhost_gpu.FenceOutCompat",
            "flags=0x" + fmt::format("{:X}", static_cast<u32>(flags.raw)) +
                " entries=" + std::to_string(params.num_entries) +
                " keeping zero increment (diagnostic only)");
    }
    params.fence.value = syncpoint_manager.IncrementSyncpointMaxExt(channel_syncpoint, increment);
    NxemuAndroidDiagnostics::RecordEvent(
        "NVDRV.nvhost_gpu.FenceOut",
        "bind=" + std::to_string(bind_id) + " syncpoint=" +
            std::to_string(channel_syncpoint) + " increment=" + std::to_string(increment) +
            " out_value=" + std::to_string(params.fence.value));
    if (nx2_caller) {
        static std::atomic<u32> trace{};
        if (trace.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_INFO(Service_NVDRV, "NX2_SUBMIT channel={} flags={:#x} entries={} prefetch={} increment={} fence={}/{}",
                     bind_id, flags.raw, entries.command_lists.size(), entries.prefetch_command_list.size(),
                     increment, params.fence.id, params.fence.value);
        }
    }
    video.PushGPUEntries(bind_id, (const uint64_t*)entries.command_lists.data(), (uint32_t)entries.command_lists.size(), (const uint32_t*)entries.prefetch_command_list.data(), (uint32_t)entries.prefetch_command_list.size());

    if (flags.fence_increment.Value()) {
        if (flags.suppress_wfi.Value()) {
            Tegra::CommandList entries{ BuildIncrementCommandList(params.fence) };
            video.PushGPUEntries(bind_id, (const uint64_t*)entries.command_lists.data(), (uint32_t)entries.command_lists.size(), (const uint32_t*)entries.prefetch_command_list.data(), (uint32_t)entries.prefetch_command_list.size());
        } else {
            Tegra::CommandList entries{ BuildIncrementWithWfiCommandList(params.fence) };
            video.PushGPUEntries(bind_id, (const uint64_t*)entries.command_lists.data(), (uint32_t)entries.command_lists.size(), (const uint32_t*)entries.prefetch_command_list.data(), (uint32_t)entries.prefetch_command_list.size());
        }
    }

    if (native_completion) {
        Tegra::CommandList completion{flags.suppress_wfi.Value()
            ? BuildIncrementCommandList(params.fence, native_completion)
            : BuildIncrementWithWfiCommandList(params.fence, native_completion)};
        video.PushGPUEntries(bind_id, (const uint64_t*)completion.command_lists.data(),
            (uint32_t)completion.command_lists.size(),
            (const uint32_t*)completion.prefetch_command_list.data(),
            (uint32_t)completion.prefetch_command_list.size());
        static std::atomic<u32> trace{};
        if (trace.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_INFO(Service_NVDRV, "NX2_ORDERED_COMPLETION channel={} increments={} fence={}/{}",
                     bind_id, native_completion, params.fence.id, params.fence.value);
        }
    }
    flags.raw = 0;

    return NvResult::Success;
}

std::shared_ptr<NvCore::Session> nvhost_gpu::GetSessionReference(DeviceFD fd) {
    const auto it = sessions.find(fd);
    if (it == sessions.end()) return nullptr;
    const auto session = it->second.lock();
    if (!session || !session->is_active.load(std::memory_order_acquire)) return nullptr;
    return session;
}

NvResult nvhost_gpu::SubmitGPFIFOBase1(IoctlSubmitGpfifo& params,
                                       std::span<Tegra::CommandListHeader> commands, DeviceFD fd,
                                       bool kickoff) {
    if (params.num_entries > commands.size()) {
        UNIMPLEMENTED();
        return NvResult::InvalidSize;
    }

    Tegra::CommandList entries(params.num_entries);
    if (kickoff) {
        const auto session = GetSessionReference(fd);
        if (!session) return NvResult::InvalidState;
        session->process->GetMemory().ReadBlock(params.address, entries.command_lists.data(),
                                               params.num_entries * sizeof(Tegra::CommandListHeader));
    } else {
        std::memcpy(entries.command_lists.data(), commands.data(),
                    params.num_entries * sizeof(Tegra::CommandListHeader));
    }

    return SubmitGPFIFOImpl(params, std::move(entries));
}

NvResult nvhost_gpu::SubmitGPFIFOBase2(IoctlSubmitGpfifo& params,
                                       std::span<const Tegra::CommandListHeader> commands) {
    if (params.num_entries > commands.size()) {
        UNIMPLEMENTED();
        return NvResult::InvalidSize;
    }

    Tegra::CommandList entries(params.num_entries);
    std::memcpy(entries.command_lists.data(), commands.data(),
                params.num_entries * sizeof(Tegra::CommandListHeader));
    return SubmitGPFIFOImpl(params, std::move(entries));
}

NvResult nvhost_gpu::GetWaitbase(IoctlGetWaitbase& params) {
    LOG_INFO(Service_NVDRV, "called, unknown=0x{:X}", params.unknown);

    params.value = 0; // Seems to be hard coded at 0
    return NvResult::Success;
}

NvResult nvhost_gpu::ChannelSetTimeout(IoctlChannelSetTimeout& params) {
    LOG_INFO(Service_NVDRV, "called, timeout=0x{:X}", params.timeout);

    channel_timeout = params.timeout;
    return NvResult::Success;
}

NvResult nvhost_gpu::ChannelSetTimeslice(IoctlSetTimeslice& params) {
    LOG_INFO(Service_NVDRV, "called, timeslice=0x{:X}", params.timeslice);

    if (params.timeslice < 1000 || params.timeslice > 50000) {
        return NvResult::BadParameter;
    }

    channel_timeslice = params.timeslice;
    return NvResult::Success;
}

Kernel::KEvent* nvhost_gpu::QueryEvent(u32 event_id) {
    NxemuAndroidDiagnostics::RecordEvent("NVDRV.nvhost_gpu.QueryEvent",
                                         "event_id=" + std::to_string(event_id));
    switch (event_id) {
    case 1:
        return sm_exception_breakpoint_int_report_event;
    case 2:
        return sm_exception_breakpoint_pause_report_event;
    case 3:
        return error_notifier_event;
    default:
        if(nx2_caller){auto*&event=nx2_events[event_id];if(!event)event=events_interface.CreateEvent(fmt::format("GpuChannelNx2Event{}",event_id));LOG_WARNING(Service_NVDRV,"(STUBBED) Switch 2 channel event {}",event_id);return event;}
        LOG_CRITICAL(Service_NVDRV, "Unknown Ctrl GPU Event {}", event_id);
        return nullptr;
    }
}

} // namespace Service::Nvidia::Devices

template class InterfacePtr<IChannelState>;
