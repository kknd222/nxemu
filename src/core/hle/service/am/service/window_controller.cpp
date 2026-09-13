// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/service/window_controller.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/nxemu_android_diagnostics.h"

namespace Service::AM {

IWindowController::IWindowController(Core::System& system_, std::shared_ptr<Applet> applet)
    : ServiceFramework{system_, "IWindowController"}, m_applet{std::move(applet)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "CreateWindow"},
        {1,  D<&IWindowController::GetAppletResourceUserId>, "GetAppletResourceUserId"},
        {2,  D<&IWindowController::GetAppletResourceUserIdOfCallerApplet>, "GetAppletResourceUserIdOfCallerApplet"},
        {10, D<&IWindowController::AcquireForegroundRights>, "AcquireForegroundRights"},
        {11, D<&IWindowController::ReleaseForegroundRights>, "ReleaseForegroundRights"},
        {12, D<&IWindowController::RejectToChangeIntoBackground>, "RejectToChangeIntoBackground"},
        {20, D<&IWindowController::SetAppletWindowVisibility>, "SetAppletWindowVisibility"},
        {21, D<&IWindowController::SetAppletGpuTimeSlice>, "SetAppletGpuTimeSlice"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IWindowController::~IWindowController() = default;

Result IWindowController::GetAppletResourceUserId(Out<AppletResourceUserId> out_aruid) {
    LOG_INFO(Service_AM, "called");
    NxemuAndroidDiagnostics::RecordEvent("AM.Window.GetAppletResourceUserId",
                                         "aruid=" + std::to_string(m_applet->aruid));
    *out_aruid = m_applet->aruid;
    R_SUCCEED();
}

Result IWindowController::GetAppletResourceUserIdOfCallerApplet(
    Out<AppletResourceUserId> out_aruid) {
    LOG_INFO(Service_AM, "called");

    if (auto caller_applet = m_applet->caller_applet.lock(); caller_applet != nullptr) {
        *out_aruid = caller_applet->aruid;
    } else {
        *out_aruid = AppletResourceUserId{};
    }

    R_SUCCEED();
}

Result IWindowController::AcquireForegroundRights() {
    LOG_INFO(Service_AM, "called");
    NxemuAndroidDiagnostics::RecordEvent("AM.Window.AcquireForegroundRights", "called");
    R_SUCCEED();
}

Result IWindowController::ReleaseForegroundRights() {
    LOG_INFO(Service_AM, "called");
    NxemuAndroidDiagnostics::RecordEvent("AM.Window.ReleaseForegroundRights", "called");
    R_SUCCEED();
}

Result IWindowController::RejectToChangeIntoBackground() {
    LOG_INFO(Service_AM, "called");
    NxemuAndroidDiagnostics::RecordEvent("AM.Window.RejectToChangeIntoBackground", "called");
    R_SUCCEED();
}

Result IWindowController::SetAppletWindowVisibility(bool visible) {
    NxemuAndroidDiagnostics::RecordEvent("AM.Window.SetAppletWindowVisibility",
                                         std::string{"visible="} +
                                             (visible ? "true" : "false"));
    m_applet->display_layer_manager.SetWindowVisibility(visible);

    if (visible) {
        m_applet->message_queue.PushMessage(AppletMessage::ChangeIntoForeground);
        m_applet->focus_state = FocusState::InFocus;
    } else {
        m_applet->focus_state = FocusState::NotInFocus;
    }

    m_applet->message_queue.PushMessage(AppletMessage::FocusStateChanged);

    R_SUCCEED();
}

Result IWindowController::SetAppletGpuTimeSlice(s64 time_slice) {
    LOG_WARNING(Service_AM, "(STUBBED) called, time_slice={}", time_slice);
    NxemuAndroidDiagnostics::RecordEvent("AM.Window.SetAppletGpuTimeSlice",
                                         "time_slice=" + std::to_string(time_slice));
    R_SUCCEED();
}

} // namespace Service::AM
