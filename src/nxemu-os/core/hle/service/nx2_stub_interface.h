// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <fmt/format.h>
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"

namespace Service {

inline constexpr std::string_view Nx2StubPrefix = "nx2:";

// Match Eglantine's per-caller compatibility fallback without changing global config.
inline bool ShouldAutoStubNx2(bool configured, bool switch2, std::string_view name) {
    return configured || switch2 || name.starts_with(Nx2StubPrefix);
}

class INx2StubInterface final : public ServiceFramework<INx2StubInterface> {
public:
    explicit INx2StubInterface(Core::System& system_, const char* name)
        : ServiceFramework{system_, name} {}

    static const char* InternName(std::string name) {
        static std::mutex mutex;
        static std::deque<std::string> names;
        std::scoped_lock lk{mutex};
        for (const auto& n : names) {
            if (n == name) return n.c_str();
        }
        return names.emplace_back(std::move(name)).c_str();
    }
};

inline void ReturnNx2StubInterface(Core::System& system, HLERequestContext& ctx,
                                   std::string_view owner) {
    const char* name = INx2StubInterface::InternName(
        fmt::format("{}{}::{}", Nx2StubPrefix, owner, ctx.GetCommand()));
    LOG_WARNING(Service, "(STUBBED) Switch 2 {}::{} -> interface {}", owner,
                ctx.GetCommand(), name);
    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface(std::make_shared<INx2StubInterface>(system, name));
}

} // namespace Service
