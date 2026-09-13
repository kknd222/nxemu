// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "yuzu_common/common_types.h"
#include "core/loader/loader.h"

namespace FileSys {
class ContentProvider;
class NACP;
class NSP;
} // namespace FileSys

class FileSystemController;

namespace Loader {

class AppLoader_NCA;

/// Loads an XCI file
class AppLoader_NSP final : public AppLoader {
public:
    explicit AppLoader_NSP(FileSys::VirtualFile file_,
                           const FileSystemController& fsc,
                           const FileSys::ContentProvider& content_provider, uint64_t program_id,
                           std::size_t program_index);
    ~AppLoader_NSP() override;

    /**
     * Identifies whether or not the given file is an NSP file.
     *
     * @param nsp_file The file to identify.
     *
     * @return FileType::NSP, or FileType::Error if the file is not an NSP.
     */
    static LoaderFileType IdentifyType(const FileSys::VirtualFile & nsp_file);

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
    LoaderResultStatus ReadControlData(FileSys::NACP& nacp) override;
    LoaderResultStatus ReadManualRomFS(FileSys::VirtualFile& out_file) override;

    LoaderResultStatus ReadBanner(std::vector<u8>& buffer) override;
    LoaderResultStatus ReadLogo(std::vector<u8>& buffer) override;

    LoaderResultStatus ReadNSOModules(Modules& modules) override;

private:
    std::unique_ptr<FileSys::NSP> nsp;
    std::unique_ptr<AppLoader> secondary_loader;

    FileSys::VirtualFile icon_file;
    std::unique_ptr<FileSys::NACP> nacp_file;
};

} // namespace Loader
