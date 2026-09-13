// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <string>
#include "yuzu_common/common_types.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

struct RomFSBuildDirectoryContext;
struct RomFSBuildFileContext;
struct RomFSDirectoryEntry;
struct RomFSFileEntry;

class RomFSBuildContext {
public:
    explicit RomFSBuildContext(VirtualDir base, VirtualDir ext = nullptr);
    ~RomFSBuildContext();

    // This finalizes the context.
    std::vector<std::pair<uint64_t, VirtualFile>> Build();

private:
    VirtualDir base;
    VirtualDir ext;
    std::shared_ptr<RomFSBuildDirectoryContext> root;
    std::vector<std::shared_ptr<RomFSBuildDirectoryContext>> directories;
    std::vector<std::shared_ptr<RomFSBuildFileContext>> files;
    uint64_t num_dirs = 0;
    uint64_t num_files = 0;
    uint64_t dir_table_size = 0;
    uint64_t file_table_size = 0;
    uint64_t dir_hash_table_size = 0;
    uint64_t file_hash_table_size = 0;
    uint64_t file_partition_size = 0;

    void VisitDirectory(VirtualDir filesys, VirtualDir ext_dir,
                        std::shared_ptr<RomFSBuildDirectoryContext> parent);

    bool AddDirectory(std::shared_ptr<RomFSBuildDirectoryContext> parent_dir_ctx,
                      std::shared_ptr<RomFSBuildDirectoryContext> dir_ctx);
    bool AddFile(std::shared_ptr<RomFSBuildDirectoryContext> parent_dir_ctx,
                 std::shared_ptr<RomFSBuildFileContext> file_ctx);
};

} // namespace FileSys
