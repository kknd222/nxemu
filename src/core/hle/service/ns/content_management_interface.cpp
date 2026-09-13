// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/common_funcs.h"
#include "core/core.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ns/content_management_interface.h"
#include "core/hle/service/ns/ns_types.h"

namespace Service::NS {

IContentManagementInterface::IContentManagementInterface(Core::System& system_)
    : ServiceFramework{system_, "IContentManagementInterface"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {43, D<&IContentManagementInterface::CheckSdCardMountStatus>, "CheckSdCardMountStatus"},
        {600, nullptr, "CountApplicationContentMeta"},
        {601, nullptr, "ListApplicationContentMetaStatus"},
        {605, nullptr, "ListApplicationContentMetaStatusWithRightsCheck"},
        {607, nullptr, "IsAnyApplicationRunning"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IContentManagementInterface::~IContentManagementInterface() = default;

Result IContentManagementInterface::CheckSdCardMountStatus() {
    LOG_WARNING(Service_NS, "(STUBBED) called");
    R_SUCCEED();
}

} // namespace Service::NS
