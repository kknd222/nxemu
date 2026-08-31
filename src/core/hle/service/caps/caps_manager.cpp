// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <sstream>

#include "yuzu_common/fs/file.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/service/caps/caps_manager.h"
#include "core/hle/service/caps/caps_result.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"

namespace Service::Capture {

AlbumManager::AlbumManager(Core::System& system_) : system{system_} {}

AlbumManager::~AlbumManager() = default;

Result AlbumManager::DeleteAlbumFile(const AlbumFileId& file_id) {
    if (file_id.storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    std::filesystem::path path;
    const auto result = GetFile(path, file_id);

    if (result.IsError()) {
        return result;
    }

    if (!Common::FS::RemoveFile(path)) {
        return ResultFileNotFound;
    }

    return ResultSuccess;
}

Result AlbumManager::IsAlbumMounted(AlbumStorage storage) {
    if (storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    is_mounted = true;

    if (storage == AlbumStorage::Sd) {
        FindScreenshots();
    }

    return is_mounted ? ResultSuccess : ResultIsNotMounted;
}

Result AlbumManager::GetAlbumFileList(std::span<AlbumEntry> out_entries, u64& out_entries_count,
                                      AlbumStorage storage, u8 flags) const {
    if (storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    for (auto& [file_id, path] : album_files) {
        if (file_id.storage != storage) {
            continue;
        }
        if (out_entries_count >= SdAlbumFileLimit) {
            break;
        }
        if (out_entries_count >= out_entries.size()) {
            break;
        }

        const auto entry_size = Common::FS::GetSize(path);
        out_entries[out_entries_count++] = {
            .entry_size = entry_size,
            .file_id = file_id,
        };
    }

    return ResultSuccess;
}

Result AlbumManager::GetAlbumFileList(std::span<ApplicationAlbumFileEntry> out_entries,
                                      u64& out_entries_count, ContentType content_type,
                                      s64 start_posix_time, s64 end_posix_time, u64 aruid) const {
    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    std::vector<ApplicationAlbumEntry> album_entries(out_entries.size());
    const auto start_date = ConvertToAlbumDateTime(start_posix_time);
    const auto end_date = ConvertToAlbumDateTime(end_posix_time);
    const auto result = GetAlbumFileList(album_entries, out_entries_count, content_type, start_date,
                                         end_date, aruid);

    if (result.IsError()) {
        return result;
    }

    for (std::size_t i = 0; i < out_entries_count; i++) {
        out_entries[i] = {
            .entry = album_entries[i],
            .datetime = album_entries[i].datetime,
            .unknown = {},
        };
    }

    return ResultSuccess;
}

Result AlbumManager::GetAlbumFileList(std::span<ApplicationAlbumEntry> out_entries,
                                      u64& out_entries_count, ContentType content_type,
                                      AlbumFileDateTime start_date, AlbumFileDateTime end_date,
                                      u64 aruid) const {
    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    for (auto& [file_id, path] : album_files) {
        if (file_id.type != content_type) {
            continue;
        }
        if (file_id.date > start_date) {
            continue;
        }
        if (file_id.date < end_date) {
            continue;
        }
        if (out_entries_count >= SdAlbumFileLimit) {
            break;
        }
        if (out_entries_count >= out_entries.size()) {
            break;
        }

        const auto entry_size = Common::FS::GetSize(path);
        out_entries[out_entries_count++] = {
            .size = entry_size,
            .hash{},
            .datetime = file_id.date,
            .storage = file_id.storage,
            .content = content_type,
            .unknown = 1,
        };
    }

    return ResultSuccess;
}

Result AlbumManager::GetAutoSavingStorage(bool& out_is_autosaving) const {
    out_is_autosaving = false;
    return ResultSuccess;
}

Result AlbumManager::LoadAlbumScreenShotImage(LoadAlbumScreenShotImageOutput& out_image_output,
                                              std::span<u8> out_image, const AlbumFileId& file_id,
                                              const ScreenShotDecodeOption& decoder_options) const {
    if (file_id.storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    out_image_output = {
        .width = 1280,
        .height = 720,
        .attribute =
            {
                .unknown_0{},
                .orientation = AlbumImageOrientation::None,
                .unknown_1{},
                .unknown_2{},
                .pad163{},
            },
        .pad179{},
    };

    std::filesystem::path path;
    const auto result = GetFile(path, file_id);

    if (result.IsError()) {
        return result;
    }

    return LoadImage(out_image, path, static_cast<int>(out_image_output.width),
                     +static_cast<int>(out_image_output.height), decoder_options.flags);
}

Result AlbumManager::LoadAlbumScreenShotThumbnail(
    LoadAlbumScreenShotImageOutput& out_image_output, std::span<u8> out_image,
    const AlbumFileId& file_id, const ScreenShotDecodeOption& decoder_options) const {
    if (file_id.storage > AlbumStorage::Sd) {
        return ResultInvalidStorage;
    }

    if (!is_mounted) {
        return ResultIsNotMounted;
    }

    out_image_output = {
        .width = 320,
        .height = 180,
        .attribute =
            {
                .unknown_0{},
                .orientation = AlbumImageOrientation::None,
                .unknown_1{},
                .unknown_2{},
                .pad163{},
            },
        .pad179{},
    };

    std::filesystem::path path;
    const auto result = GetFile(path, file_id);

    if (result.IsError()) {
        return result;
    }

    return LoadImage(out_image, path, static_cast<int>(out_image_output.width),
                     +static_cast<int>(out_image_output.height), decoder_options.flags);
}

Result AlbumManager::SaveScreenShot(ApplicationAlbumEntry& out_entry,
                                    const ScreenShotAttribute& attribute,
                                    AlbumReportOption report_option, std::span<const u8> image_data,
                                    u64 aruid) {
    return SaveScreenShot(out_entry, attribute, report_option, {}, image_data, aruid);
}

Result AlbumManager::SaveScreenShot(ApplicationAlbumEntry& out_entry,
                                    const ScreenShotAttribute& attribute,
                                    AlbumReportOption report_option,
                                    const ApplicationData& app_data, std::span<const u8> image_data,
                                    u64 aruid) {
    UNIMPLEMENTED();
    R_SUCCEED();
}

Result AlbumManager::SaveEditedScreenShot(ApplicationAlbumEntry& out_entry,
                                          const ScreenShotAttribute& attribute,
                                          const AlbumFileId& file_id,
                                          std::span<const u8> image_data) {
    UNIMPLEMENTED();
    R_SUCCEED();
}

Result AlbumManager::GetFile(std::filesystem::path& out_path, const AlbumFileId& file_id) const {
    const auto file = album_files.find(file_id);

    if (file == album_files.end()) {
        return ResultFileNotFound;
    }

    out_path = file->second;
    return ResultSuccess;
}

void AlbumManager::FindScreenshots() {
    is_mounted = false;
    album_files.clear();

    // TODO: Swap this with a blocking operation.
    const auto screenshots_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ScreenshotsDir);
    Common::FS::IterateDirEntries(
        screenshots_dir,
        [this](const std::filesystem::path& full_path) {
            AlbumEntry entry;
            if (GetAlbumEntry(entry, full_path).IsError()) {
                return true;
            }
            while (album_files.contains(entry.file_id)) {
                if (++entry.file_id.date.unique_id == 0) {
                    break;
                }
            }
            album_files[entry.file_id] = full_path;
            return true;
        },
        Common::FS::DirEntryFilter::File);

    is_mounted = true;
}

Result AlbumManager::GetAlbumEntry(AlbumEntry& out_entry, const std::filesystem::path& path) const {
    std::istringstream line_stream(path.filename().string());
    std::string date;
    std::string application;
    std::string time;

    // Parse filename to obtain entry properties
    std::getline(line_stream, application, '_');
    std::getline(line_stream, date, '_');
    std::getline(line_stream, time, '_');

    std::istringstream date_stream(date);
    std::istringstream time_stream(time);
    std::string year;
    std::string month;
    std::string day;
    std::string hour;
    std::string minute;
    std::string second;

    std::getline(date_stream, year, '-');
    std::getline(date_stream, month, '-');
    std::getline(date_stream, day, '-');

    std::getline(time_stream, hour, '-');
    std::getline(time_stream, minute, '-');
    std::getline(time_stream, second, '-');

    try {
        out_entry = {
            .entry_size = 1,
            .file_id{
                .application_id = static_cast<u64>(std::stoll(application, 0, 16)),
                .date =
                    {
                        .year = static_cast<s16>(std::stoi(year)),
                        .month = static_cast<s8>(std::stoi(month)),
                        .day = static_cast<s8>(std::stoi(day)),
                        .hour = static_cast<s8>(std::stoi(hour)),
                        .minute = static_cast<s8>(std::stoi(minute)),
                        .second = static_cast<s8>(std::stoi(second)),
                        .unique_id = 0,
                    },
                .storage = AlbumStorage::Sd,
                .type = ContentType::Screenshot,
                .unknown = 1,
            },
        };
    } catch (const std::invalid_argument&) {
        return ResultUnknown;
    } catch (const std::out_of_range&) {
        return ResultUnknown;
    } catch (const std::exception&) {
        return ResultUnknown;
    }

    return ResultSuccess;
}

Result AlbumManager::LoadImage(std::span<u8> out_image, const std::filesystem::path& path,
                               int width, int height, ScreenShotDecoderFlag flag) const {
    UNIMPLEMENTED();
    R_SUCCEED();
}

void AlbumManager::FlipVerticallyOnWrite(bool flip) {
    UNIMPLEMENTED();
}

static void PNGToMemory(void* context, void* data, int len) {
    std::vector<u8>* png_image = static_cast<std::vector<u8>*>(context);
    unsigned char* png = static_cast<unsigned char*>(data);
    png_image->insert(png_image->end(), png, png + len);
}

Result AlbumManager::SaveImage(ApplicationAlbumEntry& out_entry, std::span<const u8> image,
                               u64 title_id, const AlbumFileDateTime& date) const {
    UNIMPLEMENTED();
    R_SUCCEED();
}

AlbumFileDateTime AlbumManager::ConvertToAlbumDateTime(u64 posix_time) const {
    UNIMPLEMENTED();
    return {};
}

} // namespace Service::Capture
