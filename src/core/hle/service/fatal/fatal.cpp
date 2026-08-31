// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <ctime>
#include <fmt/chrono.h>
#include "yuzu_common/logging/log.h"
#include "yuzu_common/swap.h"
#include "core/core.h"
#include "core/hle/service/fatal/fatal.h"
#include "core/hle/service/fatal/fatal_p.h"
#include "core/hle/service/fatal/fatal_u.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"

namespace Service::Fatal {

Module::Interface::Interface(std::shared_ptr<Module> module_, Core::System& system_,
                             const char* name)
    : ServiceFramework{system_, name}, module{std::move(module_)} {}

Module::Interface::~Interface() = default;

struct FatalInfo {
    enum class Architecture : s32 {
        AArch64,
        AArch32,
    };

    const char* ArchAsString() const {
        return arch == Architecture::AArch64 ? "AArch64" : "AArch32";
    }

    std::array<u64_le, 31> registers{};
    u64_le sp{};
    u64_le pc{};
    u64_le pstate{};
    u64_le afsr0{};
    u64_le afsr1{};
    u64_le esr{};
    u64_le far{};

    std::array<u64_le, 32> backtrace{};
    u64_le program_entry_point{};

    // Bit flags that indicate which registers have been set with values
    // for this context. The service itself uses these to determine which
    // registers to specifically print out.
    u64_le set_flags{};

    u32_le backtrace_size{};
    Architecture arch{};
    u32_le unk10{}; // TODO(ogniK): Is this even used or is it just padding?
};
static_assert(sizeof(FatalInfo) == 0x250, "FatalInfo is an invalid size");

enum class FatalType : u32 {
    ErrorReportAndScreen = 0,
    ErrorReport = 1,
    ErrorScreen = 2,
};

static void GenerateErrorReport(Core::System& system, Result error_code, const FatalInfo& info) {
    UNIMPLEMENTED();
}

static void ThrowFatalError(Core::System& system, Result error_code, FatalType fatal_type,
                            const FatalInfo& info) {
    LOG_ERROR(Service_Fatal, "Threw fatal error type {} with error code 0x{:X}", fatal_type,
              error_code.raw);

    switch (fatal_type) {
    case FatalType::ErrorReportAndScreen:
        GenerateErrorReport(system, error_code, info);
        [[fallthrough]];
    case FatalType::ErrorScreen:
        // Since we have no fatal:u error screen. We should just kill execution instead
        ASSERT(false);
        break;
        // Should not throw a fatal screen but should generate an error report
    case FatalType::ErrorReport:
        GenerateErrorReport(system, error_code, info);
        break;
    }
}

void Module::Interface::ThrowFatal(HLERequestContext& ctx) {
    LOG_ERROR(Service_Fatal, "called");
    IPC::RequestParser rp{ctx};
    const auto error_code = rp.Pop<Result>();

    ThrowFatalError(system, error_code, FatalType::ErrorScreen, {});
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ThrowFatalWithPolicy(HLERequestContext& ctx) {
    LOG_ERROR(Service_Fatal, "called");
    IPC::RequestParser rp(ctx);
    const auto error_code = rp.Pop<Result>();
    const auto fatal_type = rp.PopEnum<FatalType>();

    ThrowFatalError(system, error_code, fatal_type,
                    {}); // No info is passed with ThrowFatalWithPolicy
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void Module::Interface::ThrowFatalWithCpuContext(HLERequestContext& ctx) {
    LOG_ERROR(Service_Fatal, "called");
    IPC::RequestParser rp(ctx);
    const auto error_code = rp.Pop<Result>();
    const auto fatal_type = rp.PopEnum<FatalType>();
    const auto fatal_info = ctx.ReadBuffer();
    FatalInfo info{};

    ASSERT_MSG(fatal_info.size() == sizeof(FatalInfo), "Invalid fatal info buffer size!");
    std::memcpy(&info, fatal_info.data(), sizeof(FatalInfo));

    ThrowFatalError(system, error_code, fatal_type, info);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    auto module = std::make_shared<Module>();

    server_manager->RegisterNamedService("fatal:p", std::make_shared<Fatal_P>(module, system));
    server_manager->RegisterNamedService("fatal:u", std::make_shared<Fatal_U>(module, system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::Fatal
