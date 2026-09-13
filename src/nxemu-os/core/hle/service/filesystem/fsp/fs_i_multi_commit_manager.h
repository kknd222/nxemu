// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/service.h"
#include <vector>

namespace Service::FileSystem {

class IFileSystem;

class IMultiCommitManager final : public ServiceFramework<IMultiCommitManager> {
public:
    explicit IMultiCommitManager(Core::System& system_);
    ~IMultiCommitManager() override;

private:
    Result Add(std::shared_ptr<IFileSystem> filesystem);
    Result Commit();

    std::vector<std::shared_ptr<IFileSystem>> filesystems;
};

} // namespace Service::FileSystem
