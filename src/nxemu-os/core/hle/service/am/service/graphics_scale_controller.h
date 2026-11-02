// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/service.h"

namespace Service::AM {

// IApplicationProxy::60, following Eglantine's observed nn::oe contract.
// Only the known scale query is implemented; other commands remain unhandled.
class IGraphicsScaleController final : public ServiceFramework<IGraphicsScaleController> {
public:
    explicit IGraphicsScaleController(Core::System& system_)
        : ServiceFramework{system_, "IGraphicsScaleController"} {
        static const FunctionInfo functions[] = {
            {10, D<&IGraphicsScaleController::GetCurrentGraphicsScale>, "GetCurrentGraphicsScale"},
        };
        RegisterHandlers(functions);
    }

private:
    Result GetCurrentGraphicsScale(Out<u32> out_scale) {
        // The SDK accepts 10, 20 or 30. Use the reference's fixed baseline scale.
        LOG_INFO(Service_AM, "Switch 2 graphics scale query: scale=10");
        *out_scale = 10;
        R_SUCCEED();
    }
};

} // namespace Service::AM
