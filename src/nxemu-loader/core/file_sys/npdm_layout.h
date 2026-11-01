// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace FileSys {
struct NpdmLayout {
    std::size_t acid_body;
    std::size_t acid_fac;
    std::size_t aci_fah;
    std::size_t aci_kac;
    std::size_t aci_kac_size;
    bool switch2;
};

// Inspect both ACID formats without reinterpreting unaligned guest bytes.
// All embedded offsets are relative to their ACID/ACI block, not the NPDM.
inline std::optional<NpdmLayout> InspectNpdmLayout(std::span<const std::uint8_t> bytes) {
    const auto contains = [&](std::size_t offset, std::size_t size) {
        return offset <= bytes.size() && size <= bytes.size() - offset;
    };
    const auto magic = [&](std::size_t offset, const char* value) {
        return contains(offset, 4) && std::memcmp(bytes.data() + offset, value, 4) == 0;
    };
    const auto word = [&](std::size_t offset) -> std::uint32_t {
        return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
               (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
    };
    if (!contains(0, 0x80) || !magic(0, "META")) return std::nullopt;
    const std::size_t aci = word(0x70), aci_size = word(0x74);
    const std::size_t acid = word(0x78), acid_size = word(0x7c);
    if (aci < 0x80 || acid < 0x80 || !contains(aci, aci_size) ||
        !contains(acid, acid_size) || aci_size < 0x40 || acid_size < 0x240 ||
        !magic(aci, "ACI0")) return std::nullopt;
    std::size_t signature = 0x200;
    if (!magic(acid + signature, "ACID")) {
        signature = 0x300;
        if (acid_size < 0x340 || !magic(acid + signature, "ACID")) return std::nullopt;
    }
    const std::size_t body = acid + signature;
    const std::size_t fac = word(body + 0x20), fac_size = word(body + 0x24);
    const std::size_t fah = word(aci + 0x20), fah_size = word(aci + 0x24);
    const std::size_t kac = word(aci + 0x30), kac_size = word(aci + 0x34);
    const auto region = [](std::size_t offset, std::size_t size, std::size_t limit) {
        return offset <= limit && size <= limit - offset;
    };
    if (fac < signature + 0x40 || fac_size < 0x2c || !region(fac, fac_size, acid_size) ||
        fah < 0x40 || fah_size < 0x1c || !region(fah, fah_size, aci_size) ||
        (kac_size && kac < 0x40) || (kac_size % 4) || !region(kac, kac_size, aci_size))
        return std::nullopt;
    return NpdmLayout{body, acid + fac, aci + fah, aci + kac, kac_size, signature == 0x300};
}
} // namespace FileSys
