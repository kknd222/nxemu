// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace Service::RO {
struct NrrMetadata {
    uint32_t layout_tag{}, header_size{}, size{}, hashes_offset{}, num_hashes{};
    uint64_t program_id{};
    uint8_t kind{};
};
// NS1 layout from Switchbrew NRR0; extended 0x4d0 layout from an unchanged
// NS2 NRR0 sample (tag bytes 00 01 01 00), matching its NRO SHA-256.
// The three 0x180 signature/modulus regions replace three 0x100 regions.
// This parses metadata; it does not implement cryptographic signature verification.
inline bool ParseNrrMetadata(std::span<const uint8_t> bytes, uint64_t supplied_size,
                             NrrMetadata& out) {
    out = {};
    auto u32 = [&](size_t off) {
        return uint32_t(bytes[off]) | uint32_t(bytes[off+1])<<8 |
               uint32_t(bytes[off+2])<<16 | uint32_t(bytes[off+3])<<24;
    };
    if (bytes.size()<16 || u32(0)!=0x3052524e) return false;
    const uint32_t tag=u32(4)&0xffffff00;
    const size_t header_size=tag==0 ? 0x350 : tag==0x00010100 ? 0x4d0 : 0;
    if (!header_size || bytes.size()<header_size || supplied_size<header_size) return false;
    const size_t meta=header_size-0x20;
    out.layout_tag=tag;out.header_size=static_cast<uint32_t>(header_size);
    out.program_id=uint64_t(u32(meta)) | uint64_t(u32(meta+4))<<32;
    out.size=u32(meta+8);out.kind=bytes[meta+12];
    out.hashes_offset=u32(meta+16);out.num_hashes=u32(meta+20);
    if(out.size!=supplied_size || out.kind>1 || out.hashes_offset<header_size ||
       out.hashes_offset>out.size || (out.hashes_offset&7))return false;
    return out.num_hashes <= (uint64_t(out.size)-out.hashes_offset)/32;
}
}
