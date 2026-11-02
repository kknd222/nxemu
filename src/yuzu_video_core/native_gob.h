// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <cstring>
#include "yuzu_common/common_types.h"
namespace Tegra::Texture {
constexpr u32 NativeGobOffset(u32 offset) {
    const u32 difference = ((offset >> 5) ^ (offset >> 6)) & 1;
    return offset ^ (difference << 5) ^ (difference << 6);
}
inline bool NormalizeNativeGobs(std::span<u8> output, std::span<const u8> input) {
    if (output.size() != input.size() || input.size() % 512 != 0) return false;
    for (size_t base = 0; base < input.size(); base += 512) {
        for (u32 sector = 0; sector < 512; sector += 32) {
            std::memcpy(output.data() + base + sector,
                        input.data() + base + NativeGobOffset(sector), 32);
        }
    }
    return true;
}
// The native/Maxwell sector permutation is an involution. Convert in place
// using one 32-byte sector, including upload staging and readback destinations.
inline bool NormalizeNativeGobsInPlace(std::span<u8> bytes) {
    if (bytes.size() % 512 != 0) return false;
    u8 temporary[32];
    for (size_t base = 0; base < bytes.size(); base += 128) {
        std::memcpy(temporary, bytes.data() + base + 32, 32);
        std::memcpy(bytes.data() + base + 32, bytes.data() + base + 64, 32);
        std::memcpy(bytes.data() + base + 64, temporary, 32);
    }
    return true;
}

}
