// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "core/hle/result.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/kernel_helpers.h"


namespace Kernel {
class KEvent;
class KReadableEvent;
} // namespace Kernel

namespace Core {
class System;
} // namespace Core

namespace Service::NFC {
class NfcDevice;
}

namespace Service::AM::Frontend {

enum class CabinetAppletVersion : u32 {
    Version1 = 0x1,
};

enum class CabinetFlags : u8 {
    None = 0,
    DeviceHandle = 1 << 0,
    TagInfo = 1 << 1,
    RegisterInfo = 1 << 2,
    All = DeviceHandle | TagInfo | RegisterInfo,
};
DECLARE_ENUM_FLAG_OPERATORS(CabinetFlags)

enum class CabinetResult : u8 {
    Cancel = 0,
    TagInfo = 1 << 1,
    RegisterInfo = 1 << 2,
    All = TagInfo | RegisterInfo,
};
DECLARE_ENUM_FLAG_OPERATORS(CabinetResult)

// This is nn::nfp::AmiiboSettingsStartParam
struct AmiiboSettingsStartParam {
    u64 device_handle;
    std::array<u8, 0x20> param_1;
    u8 param_2;
};
static_assert(sizeof(AmiiboSettingsStartParam) == 0x30,
              "AmiiboSettingsStartParam is an invalid size");


class Cabinet final : public FrontendApplet {
public:
    explicit Cabinet(Core::System& system_, std::shared_ptr<Applet> applet_,
                     LibraryAppletMode applet_mode_);
    ~Cabinet() override;

    void Initialize() override;

    Result GetStatus() const override;
    void ExecuteInteractive() override;
    void Execute() override;
    void DisplayCompleted(bool apply_changes, std::string_view amiibo_name);
    void Cancel();
    Result RequestExit() override;

private:
    bool is_complete{false};
    std::shared_ptr<Service::NFC::NfcDevice> nfp_device;
    Kernel::KEvent* availability_change_event;
    KernelHelpers::ServiceContext service_context;
};

} // namespace Service::AM::Frontend
