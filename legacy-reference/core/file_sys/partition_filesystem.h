// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/swap.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

/**
 * Helper which implements an interface to parse PFS/HFS filesystems.
 * Data can either be loaded from a file path or data with an offset into it.
 */
class PartitionFilesystem : public ReadOnlyVfsDirectory {
public:
    explicit PartitionFilesystem(VirtualFile file);
    ~PartitionFilesystem() override;

    LoaderResultStatus GetStatus() const;

    std::map<std::string, uint64_t> GetFileOffsets() const;
    std::map<std::string, uint64_t> GetFileSizes() const;

    std::vector<VirtualFile> GetFiles() const override;
    std::vector<VirtualDir> GetSubdirectories() const override;
    std::string GetName() const override;
    VirtualDir GetParentDirectory() const override;
    void PrintDebugInfo() const;

private:
    struct Header {
        u32_le magic;
        u32_le num_entries;
        u32_le strtab_size;
        INSERT_PADDING_BYTES(0x4);

        bool HasValidMagicValue() const;
    };

    static_assert(sizeof(Header) == 0x10, "PFS/HFS header structure size is wrong");

#pragma pack(push, 1)
    struct FSEntry {
        u64_le offset;
        u64_le size;
        u32_le strtab_offset;
    };

    static_assert(sizeof(FSEntry) == 0x14, "FS entry structure size is wrong");

    struct PFSEntry {
        FSEntry fs_entry;
        INSERT_PADDING_BYTES(0x4);
    };

    static_assert(sizeof(PFSEntry) == 0x18, "PFS entry structure size is wrong");

    struct HFSEntry {
        FSEntry fs_entry;
        u32_le hash_region_size;
        INSERT_PADDING_BYTES(0x8);
        std::array<char, 0x20> hash;
    };

    static_assert(sizeof(HFSEntry) == 0x40, "HFS entry structure size is wrong");

#pragma pack(pop)

    LoaderResultStatus status{};

    Header pfs_header{};
    bool is_hfs = false;
    std::size_t content_offset = 0;

    std::map<std::string, uint64_t> offsets;
    std::map<std::string, uint64_t> sizes;

    std::vector<VirtualFile> pfs_files;
};

} // namespace FileSys
