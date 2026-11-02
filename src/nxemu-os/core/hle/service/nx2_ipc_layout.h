// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace Service {
// TIPC: two header words, optional special header, PID and copied/moved handles.
constexpr std::size_t Nx2TipcDataOffset(const std::uint32_t* words) {
 if ((words[1] & 0x80000000U) == 0) return 2;
 const auto desc=words[2];
 return 3 + ((desc & 1U) ? 2 : 0) + ((desc >> 1) & 0xfU) + ((desc >> 5) & 0xfU);
}
}
