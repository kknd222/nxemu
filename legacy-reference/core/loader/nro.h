// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>
#include "yuzu_common/common_types.h"
#include "core/loader/loader.h"

namespace Core {
class System;
}

namespace FileSys {
class NACP;
}

namespace Kernel {
class KProcess;
}

namespace Loader {

/// Loads an NRO file
class AppLoader_NRO final : public AppLoader {
public:
    explicit AppLoader_NRO(FileSys::VirtualFile file_);
    ~AppLoader_NRO() override;

    /**
     * Identifies whether or not the given file is an NRO file.
     *
     * @param nro_file The file to identify.
     *
     * @return FileType::NRO, or FileType::Error if the file is not an NRO file.
     */
    static LoaderFileType IdentifyType(const FileSys::VirtualFile & nro_file);

    bool IsHomebrew();

    LoaderFileType GetFileType() const override
    {
        return IdentifyType(file);
    }

    LoadResult Load(Systemloader & loader, ISystemModules & modules) override;

    LoaderResultStatus ReadIcon(uint8_t * buffer, uint32_t * bufferSize) override;
    LoaderResultStatus ReadProgramId(uint64_t& out_program_id) override;
    LoaderResultStatus ReadRomFS(FileSys::VirtualFile& dir) override;
    LoaderResultStatus ReadTitle(char * buffer, uint32_t * bufferSize) override;
    LoaderResultStatus ReadControlData(FileSys::NACP& control) override;
    bool IsRomFSUpdatable() const override;

private:
    bool LoadNro(Systemloader & loader, ISystemModules & systemModules, const FileSys::VfsFile & nro_file, uint64_t & baseAddress, uint64_t & processID);

    std::vector<u8> icon_data;
    std::unique_ptr<FileSys::NACP> nacp;
    FileSys::VirtualFile romfs;
};

} // namespace Loader
