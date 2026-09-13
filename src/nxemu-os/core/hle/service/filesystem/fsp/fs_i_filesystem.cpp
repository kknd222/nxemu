// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/string_util.h"
#include "core/file_sys/fssrv/fssrv_sf_path.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_directory.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/filesystem/fsp/fs_i_filesystem.h"

namespace Service::FileSystem {

IFileSystem::IFileSystem(Core::System & system_, IVirtualDirectoryPtr && dir_, SizeGetter size_getter_) : 
    ServiceFramework{system_, "IFileSystem"},
    backend{std::make_unique<FileSys::Fsa::IFileSystem>(std::move(dir_))},
    size_getter{std::move(size_getter_)} 
{
    static const FunctionInfo functions[] = {
        {0, D<&IFileSystem::CreateFile>, "CreateFile"},
        {1, D<&IFileSystem::DeleteFile>, "DeleteFile"},
        {2, D<&IFileSystem::CreateDirectory>, "CreateDirectory"},
        {3, D<&IFileSystem::DeleteDirectory>, "DeleteDirectory"},
        {4, D<&IFileSystem::DeleteDirectoryRecursively>, "DeleteDirectoryRecursively"},
        {5, D<&IFileSystem::RenameFile>, "RenameFile"},
        {6, nullptr, "RenameDirectory"},
        {7, D<&IFileSystem::GetEntryType>, "GetEntryType"},
        {8, D<&IFileSystem::OpenFile>, "OpenFile"},
        {9, D<&IFileSystem::OpenDirectory>, "OpenDirectory"},
        {10, D<&IFileSystem::Commit>, "Commit"},
        {11, D<&IFileSystem::GetFreeSpaceSize>, "GetFreeSpaceSize"},
        {12, D<&IFileSystem::GetTotalSpaceSize>, "GetTotalSpaceSize"},
        {13, D<&IFileSystem::CleanDirectoryRecursively>, "CleanDirectoryRecursively"},
        {14, nullptr, "GetFileTimeStampRaw"},
        {15, nullptr, "QueryEntry"},
        {16, nullptr, "GetFileSystemAttribute"},
    };
    RegisterHandlers(functions);
}

Result IFileSystem::CreateFile(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path, s32 option, s64 size)
{
    const Result result = backend->CreateFile(FileSys::Path(path->str), size);
    LOG_INFO(Service_FS, "SaveTrace CreateFile path='{}' option=0x{:X} size={} result=0x{:08X}",
             path->str, option, size, result.raw);
    R_RETURN(result);
}

Result IFileSystem::DeleteFile(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path)
{
    LOG_DEBUG(Service_FS, "called. file={}", path->str);

    R_RETURN(backend->DeleteFile(FileSys::Path(path->str)));
}

Result IFileSystem::CreateDirectory(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path)
{
    const Result result = backend->CreateDirectory(FileSys::Path(path->str));
    LOG_INFO(Service_FS, "SaveTrace CreateDirectory path='{}' result=0x{:08X}", path->str,
             result.raw);
    R_RETURN(result);
}

Result IFileSystem::DeleteDirectory(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path)
{
    LOG_DEBUG(Service_FS, "called. directory={}", path->str);

    UNIMPLEMENTED();
    R_SUCCEED();
}

Result IFileSystem::DeleteDirectoryRecursively(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path)
{
    LOG_DEBUG(Service_FS, "called. directory={}", path->str);

    UNIMPLEMENTED();
    R_SUCCEED();
}

Result IFileSystem::CleanDirectoryRecursively(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path)
{
    LOG_DEBUG(Service_FS, "called. Directory: {}", path->str);

    UNIMPLEMENTED();
    R_SUCCEED();
}

Result IFileSystem::RenameFile(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> old_path, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> new_path)
{
    LOG_DEBUG(Service_FS, "called. file '{}' to file '{}'", old_path->str, new_path->str);

    R_RETURN(backend->RenameFile(FileSys::Path(old_path->str), FileSys::Path(new_path->str)));
}

Result IFileSystem::OpenFile(OutInterface<IFile> out_interface, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path, u32 mode)
{
    IVirtualFilePtr vfs_file;
    const Result result = backend->OpenFile(vfs_file.GetAddressForSet(), FileSys::Path(path->str).GetString(), static_cast<VirtualFileOpenMode>(mode));
    LOG_INFO(Service_FS, "SaveTrace OpenFile path='{}' mode=0x{:X} result=0x{:08X}", path->str,
             mode, result.raw);
    R_TRY(result);

    *out_interface = std::make_shared<IFile>(system, std::move(vfs_file), path->str);
    R_SUCCEED();
}

Result IFileSystem::OpenDirectory(OutInterface<IDirectory> out_interface, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path, u32 mode)
{
    IVirtualDirectoryPtr vfs_dir;
    const Result result = backend->OpenDirectory(vfs_dir.GetAddressForSet(), FileSys::Path(path->str), static_cast<FileSys::OpenDirectoryMode>(mode));
    LOG_INFO(Service_FS, "SaveTrace OpenDirectory path='{}' mode=0x{:X} result=0x{:08X}",
             path->str, mode, result.raw);
    R_TRY(result);

    *out_interface = std::make_shared<IDirectory>(system, std::move(vfs_dir), static_cast<FileSys::OpenDirectoryMode>(mode));
    R_SUCCEED();
}

Result IFileSystem::GetEntryType(Out<u32> out_type, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path)
{
    FileSys::DirectoryEntryType vfs_entry_type{};
    const Result result = backend->GetEntryType(&vfs_entry_type, FileSys::Path(path->str));
    LOG_INFO(Service_FS, "SaveTrace GetEntryType path='{}' result=0x{:08X} type={}", path->str,
             result.raw, static_cast<u32>(vfs_entry_type));
    R_TRY(result);

    *out_type = static_cast<u32>(vfs_entry_type);
    R_SUCCEED();
}

Result IFileSystem::Commit()
{
    // The current VFS writes directly to the host file. Commit is therefore a
    // durability boundary rather than a delayed data copy.
    LOG_INFO(Service_FS, "SaveTrace Commit filesystem={}", static_cast<const void*>(this));

    R_SUCCEED();
}

Result IFileSystem::GetFreeSpaceSize(Out<s64> out_size, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path)
{
    *out_size = static_cast<s64>(size_getter.get_free_size());
    LOG_INFO(Service_FS, "SaveTrace GetFreeSpaceSize path='{}' size={}", path->str, *out_size);
    R_SUCCEED();
}

Result IFileSystem::GetTotalSpaceSize(Out<s64> out_size, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path)
{
    *out_size = static_cast<s64>(size_getter.get_total_size());
    LOG_INFO(Service_FS, "SaveTrace GetTotalSpaceSize path='{}' size={}", path->str, *out_size);
    R_SUCCEED();
}

} // namespace Service::FileSystem
