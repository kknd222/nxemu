// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-FileCopyrightText: 2022 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_common/yuzu_assert.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nxemu_android_diagnostics.h"
#include <nxemu-module-spec/video.h>
#include <string>

namespace Service::Nvidia::NvCore {

SyncpointManager::SyncpointManager(IVideo & video_) : video{video_} {
    constexpr u32 VBlank0SyncpointId{26};
    constexpr u32 VBlank1SyncpointId{27};

    // Reserve both vblank syncpoints as client managed as they use Continuous Mode
    // Refer to section 14.3.5.3 of the TRM for more information on Continuous Mode
    // https://github.com/Jetson-TX1-AndroidTV/android_kernel_jetson_tx1_hdmi_primary/blob/8f74a72394efb871cb3f886a3de2998cd7ff2990/drivers/gpu/host1x/drm/dc.c#L660
    ReserveSyncpoint(VBlank0SyncpointId, true);
    ReserveSyncpoint(VBlank1SyncpointId, true);

    for (u32 syncpoint_id : channel_syncpoints) {
        if (syncpoint_id) {
            ReserveSyncpoint(syncpoint_id, false);
        }
    }
}

SyncpointManager::~SyncpointManager() = default;

u32 SyncpointManager::ReserveSyncpoint(u32 id, bool client_managed) {
    auto& syncpoint = syncpoints.at(id);

    if (syncpoint.reserved) {
        ASSERT_MSG(false, "Requested syncpoint is in use");
        return 0;
    }

    syncpoint.reserved = true;
    syncpoint.interface_managed = client_managed;
    NxemuAndroidDiagnostics::RecordEvent(
        "NVDRV.Syncpoint.Reserve",
        "id=" + std::to_string(id) +
            " client_managed=" + (client_managed ? std::string{"true"} : std::string{"false"}));

    return id;
}

u32 SyncpointManager::FindFreeSyncpoint() {
    for (u32 i{1}; i < syncpoints.size(); i++) {
        if (!syncpoints[i].reserved) {
            return i;
        }
    }
    ASSERT_MSG(false, "Failed to find a free syncpoint!");
    return 0;
}

u32 SyncpointManager::AllocateSyncpoint(bool client_managed) {
    std::lock_guard lock(reservation_lock);
    return ReserveSyncpoint(FindFreeSyncpoint(), client_managed);
}

void SyncpointManager::FreeSyncpoint(u32 id) {
    std::lock_guard lock(reservation_lock);
    auto& syncpoint = syncpoints.at(id);
    ASSERT(syncpoint.reserved);
    syncpoint.reserved = false;
}

bool SyncpointManager::IsSyncpointAllocated(u32 id) const {
    return (id < SyncpointCount) && syncpoints[id].reserved;
}

bool SyncpointManager::HasSyncpointExpired(u32 id, u32 threshold) const {
    const SyncpointInfo& syncpoint{syncpoints.at(id)};

    if (!syncpoint.reserved) {
        ASSERT(false);
        return false;
    }

    // If the interface manages counters then we don't keep track of the maximum value as it handles
    // sanity checking the values then
    if (syncpoint.interface_managed) {
        return static_cast<s32>(syncpoint.counter_min - threshold) >= 0;
    } else {
        return (syncpoint.counter_max - threshold) >= (syncpoint.counter_min - threshold);
    }
}

u32 SyncpointManager::IncrementSyncpointMaxExt(u32 id, u32 amount) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    const u32 value = syncpoint.counter_max += amount;
    NxemuAndroidDiagnostics::RecordEvent(
        "NVDRV.Syncpoint.IncrementMax",
        "id=" + std::to_string(id) + " amount=" + std::to_string(amount) +
            " max=" + std::to_string(value) +
            " min=" + std::to_string(syncpoint.counter_min.load()));
    return value;
}

u32 SyncpointManager::ReadSyncpointMinValue(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    const u32 value = syncpoint.counter_min;
    NxemuAndroidDiagnostics::RecordEvent(
        "NVDRV.Syncpoint.ReadMin",
        "id=" + std::to_string(id) + " min=" + std::to_string(value) +
            " max=" + std::to_string(syncpoint.counter_max.load()));
    return value;
}

u32 SyncpointManager::UpdateMin(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return 0;
    }

    const u32 old_min = syncpoint.counter_min;
    syncpoint.counter_min = video.HostSyncpointValue(id);
    NxemuAndroidDiagnostics::RecordEvent(
        "NVDRV.Syncpoint.UpdateMin",
        "id=" + std::to_string(id) + " old=" + std::to_string(old_min) +
            " new=" + std::to_string(syncpoint.counter_min.load()) +
            " max=" + std::to_string(syncpoint.counter_max.load()));
    return syncpoint.counter_min;
}

NvFence SyncpointManager::GetSyncpointFence(u32 id) {
    auto& syncpoint = syncpoints.at(id);

    if (!syncpoint.reserved) {
        ASSERT(false);
        return NvFence{};
    }

    NxemuAndroidDiagnostics::RecordEvent(
        "NVDRV.Syncpoint.GetFence",
        "id=" + std::to_string(id) +
            " value=" + std::to_string(syncpoint.counter_max.load()));
    return {
        .id = static_cast<s32>(id),
        .value = syncpoint.counter_max,
    };
}

} // namespace Service::Nvidia::NvCore
