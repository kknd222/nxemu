// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace VideoCommon {
inline constexpr uint32_t NativeGraphicsHeaderSize = 0x80;
struct GraphicsProgramLocation {
    uint64_t base, address;
    uint32_t start;
};
constexpr GraphicsProgramLocation ResolveGraphicsProgramLocation(bool native, uint64_t legacy_base,
    uint32_t offset, uint32_t native_high, uint32_t native_low) {
    if (native) {
        const auto address = (uint64_t{native_high & 0xffU} << 32) | native_low;
        return {address, address, 0};
    }
    return {legacy_base, legacy_base + offset, offset};
}
} // namespace VideoCommon
