// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <iterator>
#include <vector>

#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "core/file_sys/system_archive/mii_model.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys::SystemArchive {

namespace MiiModelData {

constexpr std::array<u8, 0x10> NFTR_STANDARD{'N',  'F',  'T',  'R',  0x01, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr std::array<u8, 0x10> NFSR_STANDARD{'N',  'F',  'S',  'R',  0x01, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

constexpr auto TEXTURE_LOW_LINEAR = NFTR_STANDARD;
constexpr auto TEXTURE_LOW_SRGB = NFTR_STANDARD;
constexpr auto TEXTURE_MID_LINEAR = NFTR_STANDARD;
constexpr auto TEXTURE_MID_SRGB = NFTR_STANDARD;
constexpr auto SHAPE_HIGH = NFSR_STANDARD;
constexpr auto SHAPE_MID = NFSR_STANDARD;

} // namespace MiiModelData

namespace {

VirtualFile LoadExternalMiiModelFile(const std::filesystem::path& directory, const std::string& name) {
    const auto path = directory / name;
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return nullptr;
    }

    std::vector<u8> data{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (data.empty()) {
        LOG_WARNING(Service_FS, "External MiiModel file '{}' is empty", path.string());
        return nullptr;
    }

    LOG_INFO(Service_FS, "Loaded external MiiModel file '{}' ({} bytes)", path.string(), data.size());
    return std::make_shared<VectorVfsFile>(std::move(data), name);
}

VirtualDir LoadExternalMiiModel() {
    const auto base_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir).parent_path() /
                          "sysdata" / "mii_model";

    constexpr std::array<const char*, 6> required_files{
        "NXTextureLowLinear.dat", "NXTextureLowSRGB.dat", "NXTextureMidLinear.dat",
        "NXTextureMidSRGB.dat",   "ShapeHigh.dat",        "ShapeMid.dat",
    };

    auto out = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
                                                    std::vector<VirtualDir>{}, "data");

    for (const auto* name : required_files) {
        auto file = LoadExternalMiiModelFile(base_dir, name);
        if (file == nullptr) {
            LOG_WARNING(Service_FS,
                        "External MiiModel override missing '{}'; falling back to placeholder MiiModel",
                        (base_dir / name).string());
            return nullptr;
        }
        out->AddFile(std::move(file));
    }

    LOG_INFO(Service_FS, "Using external MiiModel override from '{}'", base_dir.string());
    return out;
}

} // Anonymous namespace

VirtualDir MiiModel() {
    if (auto external = LoadExternalMiiModel(); external != nullptr) {
        return external;
    }

    auto out = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
                                                    std::vector<VirtualDir>{}, "data");

    out->AddFile(MakeArrayFile(MiiModelData::TEXTURE_LOW_LINEAR, "NXTextureLowLinear.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::TEXTURE_LOW_SRGB, "NXTextureLowSRGB.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::TEXTURE_MID_LINEAR, "NXTextureMidLinear.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::TEXTURE_MID_SRGB, "NXTextureMidSRGB.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::SHAPE_HIGH, "ShapeHigh.dat"));
    out->AddFile(MakeArrayFile(MiiModelData::SHAPE_MID, "ShapeMid.dat"));

    return out;
}

} // namespace FileSys::SystemArchive
