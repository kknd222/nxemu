// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
namespace Service::Nx2ChatProtocol {
inline constexpr std::uint32_t EventLimit = 63; // independent reference, not an official ABI constant.
constexpr bool Packet(std::span<const std::uint32_t> words, std::uint32_t command, std::uint32_t size) {
    return words.size() >= 2 + size && words[0] == command + 16 && words[1] == size;
}
constexpr bool ValidCount(std::uint32_t count) { return count <= EventLimit; }
constexpr bool ValidSlot(std::uint32_t slot) { return slot < EventLimit; }
}
