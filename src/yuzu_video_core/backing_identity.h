// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace VideoCommon {
struct BackingIdentity {
    std::uint64_t base{}, size{};
    constexpr bool Matches(BackingIdentity current) const {
        return base == current.base && size == current.size;
    }
    constexpr bool Contains(std::uint64_t pointer, std::uint64_t length) const {
        if (pointer < base) return false;
        const auto offset = pointer - base;
        return offset < size && length <= size - offset;
    }
};
} // namespace VideoCommon
