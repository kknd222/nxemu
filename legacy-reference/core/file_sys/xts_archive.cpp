// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <regex>
#include <string>

#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/string_util.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/xts_archive.h"
#include "core/loader/loader.h"

namespace FileSys {

constexpr uint64_t NAX_HEADER_PADDING_SIZE = 0x4000;


NAX::NAX(VirtualFile file_)
    : file(std::move(file_)) {
    std::string path = Common::FS::SanitizePath(file->GetFullPath());
    static const std::regex nax_path_regex("/registered/(000000[0-9A-F]{2})/([0-9A-F]{32})\\.nca",
                                           std::regex_constants::ECMAScript |
                                               std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(path, match, nax_path_regex)) {
        status = LoaderResultStatus::ErrorBadNAXFilePath;
        return;
    }

    const std::string two_dir = Common::ToUpper(match[1]);
    const std::string nca_id = Common::ToLower(match[2]);
    status = Parse(fmt::format("/registered/{}/{}.nca", two_dir, nca_id));
}

NAX::NAX(VirtualFile file_, std::array<u8, 0x10> nca_id)
    : file(std::move(file_)) {
    UNIMPLEMENTED();
}

NAX::~NAX() = default;

LoaderResultStatus NAX::Parse(std::string_view path) {
    UNIMPLEMENTED();
    return LoaderResultStatus::ErrorNotImplemented;
}

LoaderResultStatus NAX::GetStatus() const {
    return status;
}

VirtualFile NAX::GetDecrypted() const {
    return dec_file;
}

std::unique_ptr<NCA> NAX::AsNCA() const {
    if (type == NAXContentType::NCA)
        return std::make_unique<NCA>(GetDecrypted());
    return nullptr;
}

NAXContentType NAX::GetContentType() const {
    return type;
}

std::vector<VirtualFile> NAX::GetFiles() const {
    return {dec_file};
}

std::vector<VirtualDir> NAX::GetSubdirectories() const {
    return {};
}

std::string NAX::GetName() const {
    return file->GetName();
}

VirtualDir NAX::GetParentDirectory() const {
    return file->GetContainingDirectory();
}

} // namespace FileSys
