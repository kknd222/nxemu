// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/vi/conductor.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/nxemu_android_diagnostics.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/display_list.h"
#include "core/hle/service/vi/vsync_manager.h"
#include "os_settings.h"
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-video/video_settings_identifiers.h>

constexpr auto FrameNs = std::chrono::nanoseconds{1000000000 / 60};

extern IModuleSettings * g_settings;

namespace Service::VI
{

Conductor::Conductor(Core::System & system, Container & container, DisplayList & displays) :
    m_system(system), m_container(container)
{
    displays.ForEachDisplay([&](Display & display) {
        m_vsync_managers.insert({display.GetId(), VsyncManager{}});
    });

    if (system.IsMulticore())
    {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                m_signal.Set();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
        m_thread = std::jthread([this](std::stop_token token) { this->VsyncThread(token); });
    }
    else
    {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64 time,
                   std::chrono::nanoseconds ns_late) -> std::optional<std::chrono::nanoseconds> {
                this->ProcessVsync();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });

        system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
    }
}

Conductor::~Conductor()
{
    m_system.CoreTiming().UnscheduleEvent(m_event);

    if (m_system.IsMulticore())
    {
        m_thread.request_stop();
        m_signal.Set();
    }
}

void Conductor::LinkVsyncEvent(u64 display_id, Event * event)
{
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end())
    {
        it->second.LinkVsyncEvent(event);
    }
}

void Conductor::UnlinkVsyncEvent(u64 display_id, Event * event)
{
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end())
    {
        it->second.UnlinkVsyncEvent(event);
    }
}

void Conductor::ProcessVsync()
{
    static u64 vsync_counter{};
    ++vsync_counter;

    for (auto & [display_id, manager] : m_vsync_managers)
    {
        const bool composed =
            m_container.ComposeOnDisplay(&m_swap_interval, &m_compose_speed_scale, display_id);

        if (vsync_counter <= 20 || (vsync_counter % 300) == 0)
        {
            NxemuAndroidDiagnostics::RecordEvent(
                "VI.Conductor.ProcessVsync",
                "display_id=" + std::to_string(display_id) +
                    " composed=" + (composed ? std::string{"true"} : std::string{"false"}) +
                    " swap_interval=" + std::to_string(m_swap_interval) +
                    " speed_scale=" + std::to_string(m_compose_speed_scale));
        }

        manager.SignalVsync();
    }
}

void Conductor::VsyncThread(std::stop_token token)
{
    Common::SetCurrentThreadName("VSyncThread");

    while (!token.stop_requested())
    {
        m_signal.Wait();

        if (m_system.IsShuttingDown())
        {
            return;
        }

        this->ProcessVsync();
    }
}

s64 Conductor::GetNextTicks() const
{
    auto speed_scale = 1.f;
    if (osSettings.use_multi_core)
    {
        if (osSettings.use_speed_limit)
        {
            // Scales the speed based on speed_limit setting on MC. SC is handled by
            // SpeedLimiter::DoSpeedLimiting.
            speed_scale = 100.f / osSettings.speed_limit;
        }
        else
        {
            // Run at unlocked framerate.
            speed_scale = 0.01f;
        }
    }

    // Adjust by speed limit determined during composition.
    speed_scale /= m_compose_speed_scale;

    if (m_system.GetNVDECActive() && g_settings->GetBool(NXVideoSetting::SyncToFramerateOfVideoPlayback))
    {
        // Run at intended presentation rate during video playback.
        speed_scale = 1.f;
    }

    const f32 effective_fps = 60.f / static_cast<f32>(m_swap_interval);
    return static_cast<s64>(speed_scale * (1000000000.f / effective_fps));
}

} // namespace Service::VI
