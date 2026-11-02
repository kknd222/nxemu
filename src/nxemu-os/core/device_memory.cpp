// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/device_memory.h"
#include "hle/kernel/board/nintendo/nx/k_system_control.h"

namespace Core {

#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
constexpr size_t VirtualReserveSize = 1ULL << 38;
#else
constexpr size_t VirtualReserveSize = 1ULL << 39;
#endif

DeviceMemory::DeviceMemory()
    : buffer{Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(),
             VirtualReserveSize} {}

void DeviceMemory::ReconfigureForProgram() {
    const auto size = Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize();
    if (buffer.BackingSize() != size) {
        // Keep the IDeviceMemory object stable for other modules. Called before kernel/fastmem.
        buffer = Common::HostMemory{size, VirtualReserveSize};
    }
}
DeviceMemory::~DeviceMemory() = default;

} // namespace Core
