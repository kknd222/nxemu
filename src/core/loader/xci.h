// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "yuzu_common/common_types.h"
#include "core/loader/loader.h"

namespace FileSys {
class ContentProvider;
class NACP;
class XCI;
} // namespace FileSys

class FileSystemController;

namespace Loader {

class AppLoader_NCA;

/// Loads an XCI file
class AppLoader_XCI final : public AppLoader {
public:
    explicit AppLoader_XCI(FileSys::VirtualFile file_,
                           const FileSystemController& fsc,
                           const FileSys::ContentProvider& content_provider, uint64_t program_id,
                           std::size_t program_index);
    ~AppLoader_XCI() override;

    /**
     * Identifies whether or not the given file is an XCI file.
     *
     * @param xci_file The file to identify.
     *
     * @return FileType::XCI, or FileType::Error if the file is not an XCI file.
     */
    static LoaderFileType IdentifyType(const FileSys::VirtualFile & xci_file);

    LoaderFileType GetFileType() const override
    {
        return IdentifyType(file);
    }

    LoadResult Load(Systemloader & loader, ISystemModules & systemModules) override;

    LoaderResultStatus VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback) override;

    LoaderResultStatus ReadRomFS(FileSys::VirtualFile& out_file) override;
    LoaderResultStatus ReadUpdateRaw(FileSys::VirtualFile& out_file) override;
    LoaderResultStatus ReadProgramId(uint64_t& out_program_id) override;
    LoaderResultStatus ReadProgramIds(uint64_t * buffer, uint32_t * count) override;
    LoaderResultStatus ReadIcon(uint8_t * buffer, uint32_t * bufferSize) override;
    LoaderResultStatus ReadTitle(char * buffer, uint32_t * bufferSize) override;
    LoaderResultStatus ReadControlData(FileSys::NACP& control) override;
    LoaderResultStatus ReadManualRomFS(FileSys::VirtualFile& out_file) override;

    LoaderResultStatus ReadBanner(std::vector<u8>& buffer) override;
    LoaderResultStatus ReadLogo(std::vector<u8>& buffer) override;

    LoaderResultStatus ReadNSOModules(Modules& modules) override;

private:
    std::unique_ptr<FileSys::XCI> xci;
    std::unique_ptr<AppLoader_NCA> nca_loader;

    FileSys::VirtualFile icon_file;
    std::unique_ptr<FileSys::NACP> nacp_file;
};

} // namespace Loader
