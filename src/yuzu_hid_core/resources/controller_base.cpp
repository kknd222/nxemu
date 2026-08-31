// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>

#include "yuzu_hid_core/resources/controller_base.h"

namespace Service::HID {

ControllerBase::ControllerBase(Core::HID::HIDCore& hid_core_) : hid_core(hid_core_) {}
ControllerBase::~ControllerBase() = default;

Result ControllerBase::Activate() {
    if (is_activated) {
        return ResultSuccess;
    }
    is_activated = true;
    OnInit();
    return ResultSuccess;
}

Result ControllerBase::Activate(u64 aruid) {
    return Activate();
}

void ControllerBase::DeactivateController() {
    if (is_activated) {
        OnRelease();
    }
    is_activated = false;
}

bool ControllerBase::IsControllerActivated() const {
    // Android can still receive a HostTiming HID update while the Android activity/runtime is
    // being torn down or re-created. A stale/null resource callback was observed on realme
    // RMX3700 as a SEGV at address 0x8 inside this getter, which makes the UI look like a
    // "load then return to menu" failure. Treat a null base pointer as inactive so the caller
    // clears/skips its shared-memory update instead of aborting the process.
    if (reinterpret_cast<std::uintptr_t>(this) == 0) {
        return false;
    }
    return is_activated;
}

void ControllerBase::SetAppletResource(std::shared_ptr<AppletResource> resource,
                                       std::recursive_mutex* resource_mutex) {
    applet_resource = resource;
    shared_mutex = resource_mutex;
}

} // namespace Service::HID
