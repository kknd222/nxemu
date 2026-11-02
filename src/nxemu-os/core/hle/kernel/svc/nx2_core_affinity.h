#pragma once
#include <cstdint>

namespace Kernel::Svc {
// NXEmu has four host core schedulers. Switch2 applications can request six
// virtual cores: map 4/5 to 0/1, leaving invalid IDs and negative sentinels alone.
constexpr std::int32_t FoldNx2CoreId(std::int32_t core) {
    return core >= 4 && core < 6 ? core - 4 : core;
}
constexpr std::uint64_t FoldNx2CoreMask(std::uint64_t mask) {
    return (mask & 0xFULL) | ((mask >> 4) & 0x3ULL) | (mask & ~0x3FULL);
}
}
