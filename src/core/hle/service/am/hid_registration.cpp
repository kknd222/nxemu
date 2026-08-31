// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/service/am/hid_registration.h"
#include "core/hle/service/am/process.h"
#include "core/hle/service/hid/hid_server.h"
#include "core/hle/service/sm/sm.h"

namespace Service::AM {

HidRegistration::HidRegistration(Core::System& system, Process& process) : m_process(process) {
    m_hid_server = system.ServiceManager().GetService<HID::IHidServer>("hid");

}

HidRegistration::~HidRegistration() {
}

void HidRegistration::EnableAppletToGetInput(bool enable) {
}

} // namespace Service::AM
