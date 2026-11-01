#pragma once

#include <string_view>
#include <vector>

#include "vfs.h"

// Bridges IVirtualDirectory (module boundary) to FileSys::VfsDirectory, mirroring VfsVirtualFile.
class VfsVirtualDirectory : public FileSys::VfsDirectory {
public:
    explicit VfsVirtualDirectory(IVirtualDirectory * directory);
    ~VfsVirtualDirectory() override;

    std::vector<FileSys::VirtualFile> GetFiles() const override;
    std::vector<FileSys::VirtualDir> GetSubdirectories() const override;
    bool IsWritable() const override;
    bool IsReadable() const override;
    std::string GetName() const override;
    FileSys::VirtualDir GetParentDirectory() const override;
    FileSys::VirtualDir CreateSubdirectory(std::string_view name) override;
    FileSys::VirtualFile CreateFile(std::string_view name) override;
    bool DeleteSubdirectory(std::string_view name) override;
    bool DeleteFile(std::string_view name) override;
    bool Rename(std::string_view name) override;

    FileSys::VirtualFile GetFile(std::string_view name) const override;
    FileSys::VirtualDir GetSubdirectory(std::string_view name) const override;
    FileSys::VirtualFile GetFileRelative(std::string_view path) const override;
    FileSys::VirtualDir GetDirectoryRelative(std::string_view path) const override;

private:
    IVirtualDirectory * m_directory;
};
