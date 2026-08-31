// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>
#include "yuzu_common/common_types.h"
#include "yuzu_common/swap.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

class NCA;

enum class NAXContentType : u8 {
    Save = 0,
    NCA = 1,
};

class NAX : public ReadOnlyVfsDirectory {
public:
    explicit NAX(VirtualFile file);
    explicit NAX(VirtualFile file, std::array<u8, 0x10> nca_id);
    ~NAX() override;

    LoaderResultStatus GetStatus() const;

    VirtualFile GetDecrypted() const;

    std::unique_ptr<NCA> AsNCA() const;

    NAXContentType GetContentType() const;

    std::vector<VirtualFile> GetFiles() const override;

    std::vector<VirtualDir> GetSubdirectories() const override;

    std::string GetName() const override;

    VirtualDir GetParentDirectory() const override;

private:
    LoaderResultStatus Parse(std::string_view path);

    VirtualFile file;
    LoaderResultStatus status;
    NAXContentType type{};

    VirtualFile dec_file;
};
} // namespace FileSys
