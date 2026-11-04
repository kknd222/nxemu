#pragma once
#include <cstdint>

namespace Kernel::Svc {
// Six independent guest core schedulers preserve NS2 IDs/affinity0..5.
// Do not fold non-preemptible workers onto another guest core. Invalid IDs,
// negative sentinels and unsupported mask bits remain for caller validation.
constexpr std::int32_t FoldNx2CoreId(std::int32_t core) {
    return core;
}
constexpr std::uint64_t FoldNx2CoreMask(std::uint64_t mask) {
    return mask;
}
}
