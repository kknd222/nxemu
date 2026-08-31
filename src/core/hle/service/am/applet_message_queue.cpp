// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/applet_message_queue.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nxemu_android_diagnostics.h"

namespace Service::AM {

AppletMessageQueue::AppletMessageQueue(Core::System& system)
    : service_context{system, "AppletMessageQueue"} {
    on_new_message = service_context.CreateEvent("AMMessageQueue:OnMessageReceived");
    on_operation_mode_changed = service_context.CreateEvent("AMMessageQueue:OperationModeChanged");
}

AppletMessageQueue::~AppletMessageQueue() {
    service_context.CloseEvent(on_new_message);
    service_context.CloseEvent(on_operation_mode_changed);
}

Kernel::KReadableEvent& AppletMessageQueue::GetMessageReceiveEvent() {
    return on_new_message->GetReadableEvent();
}

Kernel::KReadableEvent& AppletMessageQueue::GetOperationModeChangedEvent() {
    return on_operation_mode_changed->GetReadableEvent();
}

void AppletMessageQueue::PushMessage(AppletMessage msg) {
    NxemuAndroidDiagnostics::RecordEvent(
        "AM.Message.Push", "msg=" + std::to_string(static_cast<u32>(msg)));
    {
        std::scoped_lock lk{lock};
        messages.push(msg);
    }
    on_new_message->Signal();
}

AppletMessage AppletMessageQueue::PopMessage() {
    std::scoped_lock lk{lock};
    if (messages.empty()) {
        on_new_message->Clear();
        NxemuAndroidDiagnostics::RecordEvent("AM.Message.Pop", "msg=None queue_empty=true");
        return AppletMessage::None;
    }
    auto msg = messages.front();
    messages.pop();
    if (messages.empty()) {
        on_new_message->Clear();
    }
    NxemuAndroidDiagnostics::RecordEvent(
        "AM.Message.Pop", "msg=" + std::to_string(static_cast<u32>(msg)) +
                              " remaining=" + std::to_string(messages.size()));
    return msg;
}

std::size_t AppletMessageQueue::GetMessageCount() const {
    std::scoped_lock lk{lock};
    return messages.size();
}

void AppletMessageQueue::RequestExit() {
    NxemuAndroidDiagnostics::RecordEvent("AM.Message.RequestExit", "push Exit");
    PushMessage(AppletMessage::Exit);
}

void AppletMessageQueue::RequestResume() {
    NxemuAndroidDiagnostics::RecordEvent("AM.Message.RequestResume", "push Resume");
    PushMessage(AppletMessage::Resume);
}

void AppletMessageQueue::FocusStateChanged() {
    NxemuAndroidDiagnostics::RecordEvent("AM.Message.FocusStateChanged",
                                         "push FocusStateChanged");
    PushMessage(AppletMessage::FocusStateChanged);
}

void AppletMessageQueue::OperationModeChanged() {
    NxemuAndroidDiagnostics::RecordEvent(
        "AM.Message.OperationModeChanged", "push OperationModeChanged+PerformanceModeChanged");
    PushMessage(AppletMessage::OperationModeChanged);
    PushMessage(AppletMessage::PerformanceModeChanged);
    on_operation_mode_changed->Signal();
}

} // namespace Service::AM
