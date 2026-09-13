// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/frontend/applet_cabinet.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/mii/mii_manager.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/yuzu_assert.h"

namespace Service::AM::Frontend
{

Cabinet::Cabinet(Core::System & system_, std::shared_ptr<Applet> applet_, LibraryAppletMode applet_mode_) :
    FrontendApplet{system_, applet_, applet_mode_}, 
    service_context{system_,"CabinetApplet"}
{

    availability_change_event = service_context.CreateEvent("CabinetApplet:AvailabilityChangeEvent");
}

Cabinet::~Cabinet()
{
    service_context.CloseEvent(availability_change_event);
};

void Cabinet::Initialize()
{
    UNIMPLEMENTED();
}

Result Cabinet::GetStatus() const
{
    return ResultSuccess;
}

void Cabinet::ExecuteInteractive()
{
    ASSERT_MSG(false, "Attempted to call interactive execution on non-interactive applet.");
}

void Cabinet::Execute()
{
    if (is_complete)
    {
        return;
    }
    UNIMPLEMENTED();
}

void Cabinet::DisplayCompleted(bool apply_changes, std::string_view amiibo_name)
{
    Service::Mii::MiiManager manager;
    UNIMPLEMENTED();
}

void Cabinet::Cancel()
{
    UNIMPLEMENTED();
}

Result Cabinet::RequestExit()
{
    UNIMPLEMENTED();
    R_SUCCEED();
}

} // namespace Service::AM::Frontend
