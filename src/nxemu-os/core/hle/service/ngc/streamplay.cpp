// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Service names, scoped interfaces and command tables are adapted from Eden.
// Unsupported operations use error-only replies rather than reporting success.

#include "core/hle/service/ngc/streamplay.h"
#include <span>
#include <cstring>
#include "os_settings.h"
#include "core/hle/kernel/k_shared_memory.h"
#include <nxemu-module-spec/system_loader.h>
#include "core/core.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service::NGC {
namespace {

class StreamPlayShim final : public ServiceFramework<StreamPlayShim> {
public:
    StreamPlayShim(Core::System& system_, bool system_scope)
        : ServiceFramework{system_, system_scope ? "ISystemShimScopedObject" : "IUserShimScopedObject"} {
        const std::span<const StreamPlayCommand> commands = system_scope
            ? std::span<const StreamPlayCommand>{SystemShimCommands}
            : std::span<const StreamPlayCommand>{UserShimCommands};
        if (!system_scope) {
            static const FunctionInfo native_commands[]{
                {1, &StreamPlayShim::GetRequiredMemorySize, "GetRequiredMemorySize"},
                {2, &StreamPlayShim::Initialize, "Initialize"},
                {300, &StreamPlayShim::GetGuestHidSharedMemory, "GetGuestHidSharedMemory"},
            };
            RegisterHandlers(native_commands);
        }
        for (const auto& command : commands) {
            const FunctionInfo function{command.id, &StreamPlayShim::Unsupported, command.name};
            RegisterHandlers(&function, 1);
        }
    }

    ~StreamPlayShim() override { if (guest_memory) guest_memory->Close(); }

private:
    void GetRequiredMemorySize(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto params = rp.PopRaw<StreamPlayParameters>();
        const auto type = params.type; const auto mode = params.mode;
        const auto guests = params.guests;
        const auto error = ValidateStreamPlay(type, mode, guests, false);
        LOG_INFO(Service_NGC, "NX2_STREAMPLAY memory type={} mode={} guests={} result={:#x}", type, mode, guests, error);
        IPC::ResponseBuilder rb{ctx, error ? 2u : 3u}; rb.Push(Result{error});
        if (!error) rb.Push(StreamPlayMemorySize(guests));
    }
    void Initialize(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto params = rp.PopRaw<StreamPlayParameters>();
        const auto type = params.type; const auto mode = params.mode;
        const auto guests = params.guests;
        const auto error = ValidateStreamPlay(type, mode, guests, true);
        LOG_INFO(Service_NGC, "NX2_STREAMPLAY initialize type={} mode={} guests={} result={:#x}", type, mode, guests, error);
        IPC::ResponseBuilder rb{ctx, 2}; rb.Push(Result{error});
    }
    void GetGuestHidSharedMemory(HLERequestContext& ctx) {
        constexpr u64 size = 0x84000;
        if (!guest_memory) {
            auto* memory = Kernel::KSharedMemory::Create(system.Kernel());
            const auto result = memory->Initialize(system.DeviceMemory(), nullptr,
                Kernel::Svc::MemoryPermission::ReadWrite, Kernel::Svc::MemoryPermission::Read, size);
            if (result.IsError()) {
                memory->Close(); IPC::ResponseBuilder rb{ctx, 2}; rb.Push(result); return;
            }
            // Empty pad/touch rings and zero style sets: no connected remote players.
            std::memset(memory->GetPointer(), 0, size);
            Kernel::KSharedMemory::Register(system.Kernel(), memory); guest_memory = memory;
        }
        LOG_INFO(Service_NGC, "NX2_STREAMPLAY guest HID shared memory size={:#x}", size);
        IPC::ResponseBuilder rb{ctx, 6, 1}; rb.Push(ResultSuccess);
        rb.Push(size); rb.Push(u64{0}); rb.PushCopyObjects(*guest_memory);
    }
    Kernel::KSharedMemory* guest_memory{};
    void Unsupported(HLERequestContext& ctx) {
        LOG_WARNING(Service_NGC, "StreamPlay operation unavailable: {} command={}",
                    GetServiceName(), ctx.GetCommand());
        // No save handles, writes, events or asynchronous objects exist yet. An
        // error-only response must not imply a successful read or durable write.
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(Result{StreamPlayNotSupported});
    }
};

class StreamPlayService final : public ServiceFramework<StreamPlayService> {
public:
    StreamPlayService(Core::System& system_, bool system_scope_)
        : ServiceFramework{system_, system_scope_ ? "stpl:sys" : "stpl:u"},
          system_scope{system_scope_} {
        static const FunctionInfo functions[]{
            {0, &StreamPlayService::CreateShim, "CreateShimScopedObject"},
        };
        RegisterHandlers(functions);
    }

private:
    void CreateShim(HLERequestContext& ctx) {
        if (!system_scope) {
            IPC::RequestParser parser{ctx};
            const auto mode = parser.Pop<u32>();
            LOG_DEBUG(Service_NGC, "StreamPlay user shim requested, mode={:#x}", mode);
        }
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<StreamPlayShim>(system, system_scope);
    }

    bool system_scope;
};

} // namespace

void RegisterStreamPlayServices(ServerManager& manager, Core::System& system) {
    char version[32]{};
    system.GetSystemloader().GetInstalledFirmwareDisplayVersion(version, sizeof(version));
    const bool native_title = osSettings.current_program_is_switch2;
    if (!ShouldRegisterStreamPlay(native_title, version)) {
        return;
    }
    LOG_INFO(Service_NGC, "NX2_STREAMPLAY register native={} firmware={}", native_title, version);
    manager.RegisterNamedService("stpl:u", std::make_shared<StreamPlayService>(system, false), 4);
    manager.RegisterNamedService("stpl:sys", std::make_shared<StreamPlayService>(system, true), 4);
}

} // namespace Service::NGC
