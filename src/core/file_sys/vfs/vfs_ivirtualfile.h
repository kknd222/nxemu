#pragma once
#include "vfs.h"

class VfsVirtualFile : 
    public FileSys::VfsFile
{
public:
    VfsVirtualFile();
    VfsVirtualFile(IVirtualFile *);
    VfsVirtualFile(VfsVirtualFile && other) noexcept;
    ~VfsVirtualFile();

    VfsVirtualFile & operator=(IVirtualFile * ptr);
    VfsVirtualFile & operator=(VfsVirtualFile && other) noexcept;

    // VfsFile
    std::string GetName() const override;
    std::string GetExtension() const override;
    std::size_t GetSize() const override;
    bool Resize(std::size_t new_size) override;
    FileSys::VirtualDir GetContainingDirectory() const override;    

    bool IsWritable() const override;
    bool IsReadable() const override;

    std::size_t Read(u8 * data, std::size_t length, std::size_t offset = 0) const override;
    std::size_t Write(const u8 * data, std::size_t length, std::size_t offset = 0) override;
    bool Rename(std::string_view name) override;

protected:
    VfsVirtualFile(const VfsVirtualFile &) = delete;
    VfsVirtualFile & operator=(const VfsVirtualFile &) = delete;

    IVirtualFile * m_file;
};