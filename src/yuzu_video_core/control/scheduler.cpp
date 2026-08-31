// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/control/scheduler.h"
#include "yuzu_video_core/gpu.h"
#if defined(__ANDROID__) && defined(NXEMU_ANDROID_FULL_DIAG)
#include <android/log.h>
#endif

namespace Tegra::Control {
Scheduler::Scheduler(GPU& gpu_) : gpu{gpu_} {}

Scheduler::~Scheduler() = default;

void Scheduler::Push(s32 channel, CommandList&& entries) {
#if defined(__ANDROID__) && defined(NXEMU_ANDROID_FULL_DIAG)
    __android_log_print(ANDROID_LOG_INFO, "NxEmuHleDiag",
                        "Video.Scheduler.Push channel=%d entries=%zu prefetch=%zu", channel,
                        entries.command_lists.size(), entries.prefetch_command_list.size());
#endif
    std::unique_lock lk(scheduling_guard);
    auto it = channels.find(channel);
    ASSERT(it != channels.end());
    auto channel_state = it->second;
    gpu.BindChannel(channel_state->bind_id);
    channel_state->dma_pusher->Push(std::move(entries));
    channel_state->dma_pusher->DispatchCalls();
#if defined(__ANDROID__) && defined(NXEMU_ANDROID_FULL_DIAG)
    __android_log_print(ANDROID_LOG_INFO, "NxEmuHleDiag",
                        "Video.Scheduler.DispatchDone channel=%d", channel);
#endif
}

void Scheduler::DeclareChannel(std::shared_ptr<ChannelState> new_channel) {
    s32 channel = new_channel->bind_id;
    std::unique_lock lk(scheduling_guard);
    channels.emplace(channel, new_channel);
}

} // namespace Tegra::Control
