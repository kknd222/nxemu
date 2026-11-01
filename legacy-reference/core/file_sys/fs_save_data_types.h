// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <fmt/format.h>
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include <nxemu-module-spec/system_loader.h>

namespace FileSys {

using SaveDataId = uint64_t;
using SystemSaveDataId = uint64_t;
using SystemBcatSaveDataId = SystemSaveDataId;
using ProgramId = uint64_t;

using UserId = u128;
static_assert(std::is_trivially_copyable_v<UserId>, "Data type must be trivially copyable.");
static_assert(sizeof(UserId) == 0x10, "UserId has invalid size.");

constexpr inline SystemSaveDataId InvalidSystemSaveDataId = 0;
constexpr inline UserId InvalidUserId = {};

enum class SaveDataFlags : u32 {
    None = (0 << 0),
    KeepAfterResettingSystemSaveData = (1 << 0),
    KeepAfterRefurbishment = (1 << 1),
    KeepAfterResettingSystemSaveDataWithoutUserSaveData = (1 << 2),
    NeedsSecureDelete = (1 << 3),
};

enum class SaveDataMetaType : u8 {
    None = 0,
    Thumbnail = 1,
    ExtensionContext = 2,
};

struct SaveDataMetaInfo {
    u32 size;
    SaveDataMetaType type;
    INSERT_PADDING_BYTES(0xB);
};
static_assert(std::is_trivially_copyable_v<SaveDataMetaInfo>,
              "Data type must be trivially copyable.");
static_assert(sizeof(SaveDataMetaInfo) == 0x10, "SaveDataMetaInfo has invalid size.");

struct SaveDataCreationInfo {
    s64 size;
    s64 journal_size;
    s64 block_size;
    uint64_t owner_id;
    u32 flags;
    SaveDataSpaceId space_id;
    bool pseudo;
    INSERT_PADDING_BYTES(0x1A);
};
static_assert(std::is_trivially_copyable_v<SaveDataCreationInfo>, "Data type must be trivially copyable.");
static_assert(sizeof(SaveDataCreationInfo) == 0x40, "SaveDataCreationInfo has invalid size.");

struct SaveDataExtraData {
    SaveDataAttribute attr;
    uint64_t owner_id;
    s64 timestamp;
    u32 flags;
    INSERT_PADDING_BYTES(4);
    s64 available_size;
    s64 journal_size;
    s64 commit_id;
    INSERT_PADDING_BYTES(0x190);
};
static_assert(sizeof(SaveDataExtraData) == 0x200, "SaveDataExtraData has invalid size.");
static_assert(std::is_trivially_copyable_v<SaveDataExtraData>,
              "Data type must be trivially copyable.");

struct SaveDataFilter {
    bool use_program_id;
    bool use_save_data_type;
    bool use_user_id;
    bool use_save_data_id;
    bool use_index;
    SaveDataRank rank;
    SaveDataAttribute attribute;
};
static_assert(sizeof(SaveDataFilter) == 0x48, "SaveDataFilter has invalid size.");
static_assert(std::is_trivially_copyable_v<SaveDataFilter>,
              "Data type must be trivially copyable.");

struct HashSalt {
    static constexpr size_t Size = 32;

    std::array<u8, Size> value;
};
static_assert(std::is_trivially_copyable_v<HashSalt>, "Data type must be trivially copyable.");
static_assert(sizeof(HashSalt) == HashSalt::Size);

} // namespace FileSys
