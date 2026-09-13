// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nfc/nfc_types.h"
#include "core/hle/service/nfp/nfp_interface.h"
#include "core/hle/service/nfp/nfp_result.h"
#include "core/hle/service/nfp/nfp_types.h"
#include "yuzu_hid_core/hid_types.h"

namespace Service::NFP {

Interface::Interface(Core::System& system_, const char* name)
    : NfcInterface{system_, name, NFC::BackendType::Nfp} {}

Interface::~Interface() = default;

void Interface::InitializeSystem(HLERequestContext& ctx) {
    Initialize(ctx);
}

void Interface::InitializeDebug(HLERequestContext& ctx) {
    Initialize(ctx);
}

void Interface::FinalizeSystem(HLERequestContext& ctx) {
    Finalize(ctx);
}

void Interface::FinalizeDebug(HLERequestContext& ctx) {
    Finalize(ctx);
}

void Interface::Mount(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::Unmount(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::OpenApplicationArea(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::GetApplicationArea(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::SetApplicationArea(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::Flush(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::Restore(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::CreateApplicationArea(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::GetRegisterInfo(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::GetCommonInfo(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::GetModelInfo(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::GetApplicationAreaSize(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::RecreateApplicationArea(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::Format(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::GetAdminInfo(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::GetRegisterInfoPrivate(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::SetRegisterInfoPrivate(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::DeleteRegisterInfo(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::DeleteApplicationArea(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::ExistsApplicationArea(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::GetAll(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::SetAll(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::FlushDebug(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::BreakTag(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::ReadBackupData(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::WriteBackupData(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

void Interface::WriteNtf(HLERequestContext& ctx) {
    UNIMPLEMENTED();
}

} // namespace Service::NFP
