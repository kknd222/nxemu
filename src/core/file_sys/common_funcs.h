// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_common/common_types.h"

namespace FileSys {

constexpr uint64_t AOC_TITLE_ID_MASK = 0x7FF;
constexpr uint64_t AOC_TITLE_ID_OFFSET = 0x1000;
constexpr uint64_t BASE_TITLE_ID_MASK = 0xFFFFFFFFFFFFE000;

/**
 * Gets the base title ID from a given title ID.
 *
 * @param title_id The title ID.
 * @returns The base title ID.
 */
[[nodiscard]] constexpr uint64_t GetBaseTitleID(uint64_t title_id) {
    return title_id & BASE_TITLE_ID_MASK;
}

/**
 * Gets the base title ID with a program index offset from a given title ID.
 *
 * @param title_id The title ID.
 * @param program_index The program index.
 * @returns The base title ID with a program index offset.
 */
[[nodiscard]] constexpr uint64_t GetBaseTitleIDWithProgramIndex(uint64_t title_id, uint64_t program_index) {
    return GetBaseTitleID(title_id) + program_index;
}

/**
 * Gets the AOC (Add-On Content) base title ID from a given title ID.
 *
 * @param title_id The title ID.
 * @returns The AOC base title ID.
 */
[[nodiscard]] constexpr uint64_t GetAOCBaseTitleID(uint64_t title_id) {
    return GetBaseTitleID(title_id) + AOC_TITLE_ID_OFFSET;
}

/**
 * Gets the AOC (Add-On Content) ID from a given AOC title ID.
 *
 * @param aoc_title_id The AOC title ID.
 * @returns The AOC ID.
 */
[[nodiscard]] constexpr uint64_t GetAOCID(uint64_t aoc_title_id) {
    return aoc_title_id & AOC_TITLE_ID_MASK;
}

} // namespace FileSys
