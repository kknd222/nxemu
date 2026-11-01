// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <sstream>
#include <mutex>
#include <utility>
#include <vector>

#include "system_loader.h"
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/system_loader.h>
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/logging/log.h"
#include "loader_settings_identifiers.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include "yuzu_common/settings.h"
#include "yuzu_common/swap.h"
#include "core/core.h"
#include "core/file_sys/filesystem.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nro.h"
#include "core/loader/nso.h"
#include "core/memory.h"

#if 0 && (defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__))
#include "core/arm/nce/patcher.h"
#endif

extern IModuleSettings * g_settings;

namespace {
std::mutex g_hbl_diagnostic_mutex;
std::string g_hbl_diagnostic = "hblEnvConfigured=false";
u64 g_hbl_next_load_path_addr = 0;
u64 g_hbl_next_load_argv_addr = 0;

void SetHblDiagnostic(std::string text)
{
    std::lock_guard lock{g_hbl_diagnostic_mutex};
    g_hbl_diagnostic = std::move(text);
}
} // namespace

extern "C" const char * NxemuGetLastHblEnvDiagnostics()
{
    std::lock_guard lock{g_hbl_diagnostic_mutex};
    return g_hbl_diagnostic.c_str();
}

extern "C" uint64_t NxemuGetLastHblNextLoadPathAddress()
{
    std::lock_guard lock{g_hbl_diagnostic_mutex};
    return g_hbl_next_load_path_addr;
}

extern "C" uint64_t NxemuGetLastHblNextLoadArgvAddress()
{
    std::lock_guard lock{g_hbl_diagnostic_mutex};
    return g_hbl_next_load_argv_addr;
}

namespace Loader {

struct NroSegmentHeader {
    u32_le offset;
    u32_le size;
};
static_assert(sizeof(NroSegmentHeader) == 0x8, "NroSegmentHeader has incorrect size.");

struct NroHeader {
    INSERT_PADDING_BYTES(0x4);
    u32_le module_header_offset;
    u32 magic_ext1;
    u32 magic_ext2;
    u32_le magic;
    INSERT_PADDING_BYTES(0x4);
    u32_le file_size;
    INSERT_PADDING_BYTES(0x4);
    std::array<NroSegmentHeader, 3> segments; // Text, RoData, Data (in that order)
    u32_le bss_size;
    INSERT_PADDING_BYTES(0x44);
};
static_assert(sizeof(NroHeader) == 0x80, "NroHeader has incorrect size.");

struct ModHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le unwind_start_offset;
    u32_le unwind_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(ModHeader) == 0x1c, "ModHeader has incorrect size.");

struct AssetSection {
    u64_le offset;
    u64_le size;
};
static_assert(sizeof(AssetSection) == 0x10, "AssetSection has incorrect size.");

struct AssetHeader {
    u32_le magic;
    u32_le format_version;
    AssetSection icon;
    AssetSection nacp;
    AssetSection romfs;
};
static_assert(sizeof(AssetHeader) == 0x38, "AssetHeader has incorrect size.");

struct HblConfigEntry {
    u32_le key;
    u32_le flags;
    u64_le value[2];
};
static_assert(sizeof(HblConfigEntry) == 0x18, "HblConfigEntry has incorrect size.");

static constexpr u32 HblEnvAllocationSize = 0x6000;
static constexpr u32 HblEnvConfigOffset = 0x0000;
static constexpr u32 HblEnvNextLoadPathOffset = 0x1000;
static constexpr u32 HblEnvNextLoadArgvOffset = 0x2000;
static constexpr u32 HblEnvArgvOffset = 0x3000;
static constexpr u32 HblEnvLoaderInfoOffset = 0x4000;
static constexpr u32 HblEnvUserIdOffset = 0x5000;

static void WriteAscii(Kernel::PhysicalMemory & image, std::size_t offset, const char * text)
{
    const std::size_t max_len = image.size() > offset ? image.size() - offset : 0;
    if (max_len == 0)
    {
        return;
    }
    const std::size_t len = std::min<std::size_t>(std::strlen(text), max_len - 1);
    std::memcpy(image.data() + offset, text, len);
    image[offset + len] = 0;
}

static u64 AppendHomebrewEnvironment(Kernel::PhysicalMemory & image, u64 load_base,
                                      const char * argv0, u64 * main_thread_handle_write_address)
{
    const auto env_offset = static_cast<u32>(image.size());
    image.resize(image.size() + HblEnvAllocationSize);
    std::memset(image.data() + env_offset, 0, HblEnvAllocationSize);

    const u64 env_base = load_base + env_offset;
    const u64 config_addr = env_base + HblEnvConfigOffset;
    const u64 next_load_path_addr = env_base + HblEnvNextLoadPathOffset;
    const u64 next_load_argv_addr = env_base + HblEnvNextLoadArgvOffset;
    const u64 argv_addr = env_base + HblEnvArgvOffset;
    const u64 loader_info_addr = env_base + HblEnvLoaderInfoOffset;
    const u64 user_id_addr = env_base + HblEnvUserIdOffset;

    WriteAscii(image, env_offset + HblEnvArgvOffset, argv0);
    WriteAscii(image, env_offset + HblEnvLoaderInfoOffset, "nxemu android hbl environment");

    {
        std::lock_guard lock{g_hbl_diagnostic_mutex};
        g_hbl_next_load_path_addr = next_load_path_addr;
        g_hbl_next_load_argv_addr = next_load_argv_addr;
    }

    auto * entries = reinterpret_cast<HblConfigEntry *>(image.data() + env_offset + HblEnvConfigOffset);
    std::size_t i = 0;
    const auto add = [&](u32 key, u64 value0, u64 value1 = 0) {
        entries[i].key = key;
        entries[i].flags = 0;
        entries[i].value[0] = value0;
        entries[i].value[1] = value1;
        ++i;
    };

    // libnx homebrew ABI entry types. Keep this intentionally minimal; hbmenu only needs
    // NextLoadPath for launchInit(), while libnx init needs a main-thread handle value.
    add(1, 0);                                      // EntryType_MainThreadHandle, patched after handle creation.
    *main_thread_handle_write_address = config_addr + offsetof(HblConfigEntry, value);
    add(2, next_load_path_addr, next_load_argv_addr); // EntryType_NextLoadPath.
    add(5, 0, argv_addr);                          // EntryType_Argv.
    add(6, UINT64_MAX, UINT64_MAX);                 // EntryType_SyscallAvailableHint.
    add(7, 0, 1);                                  // EntryType_AppletType=Application, ApplicationOverride.
    add(14, 0x6e78656d752d616eULL, 0x64726f69642d706fULL); // EntryType_RandomSeed.
    add(15, user_id_addr);                         // EntryType_UserIdStorage.
    add(16, (12u << 16) | (1u << 8) | 0u);         // EntryType_HosVersion 12.1.0.
    add(17, UINT64_MAX);                           // EntryType_SyscallAvailableHint2.
    entries[i].key = 0;                            // EntryType_EndOfList.
    entries[i].flags = 0;
    entries[i].value[0] = loader_info_addr;
    entries[i].value[1] = std::strlen("nxemu android hbl environment") + 1;

    std::ostringstream diag;
    diag << "hblEnvConfigured=true\n";
    diag << "hblAbi=minimal-v2\n";
    diag << "hblConfigAddr=0x" << std::hex << config_addr << "\n";
    diag << "hblEnvBase=0x" << std::hex << env_base << "\n";
    diag << "hblEnvOffset=0x" << std::hex << env_offset << "\n";
    diag << "hblNextLoadPathAddr=0x" << std::hex << next_load_path_addr << "\n";
    diag << "hblNextLoadArgvAddr=0x" << std::hex << next_load_argv_addr << "\n";
    diag << "hblArgvAddr=0x" << std::hex << argv_addr << "\n";
    diag << "hblMainThreadHandleWriteAddress=0x" << std::hex << *main_thread_handle_write_address << "\n";
    diag << "hblMainThreadArg0=0x" << std::hex << config_addr << "\n";
    diag << "hblMainThreadArg1=0xffffffffffffffff\n";
    diag << "hblNextLoadPathProvided=true\n";
    diag << "hblConfigEntryCount=" << std::dec << (i + 1) << "\n";
    diag << "hblArgv0=" << argv0;
    SetHblDiagnostic(diag.str());

    return config_addr;
}


AppLoader_NRO::AppLoader_NRO(FileSys::VirtualFile file_) : AppLoader(std::move(file_))
{
    NroHeader nro_header{};
    if (file->ReadObject(&nro_header) != sizeof(NroHeader))
    {
        return;
    }

    if (file->GetSize() >= nro_header.file_size + sizeof(AssetHeader))
    {
        const uint64_t offset = nro_header.file_size;
        AssetHeader asset_header{};
        if (file->ReadObject(&asset_header, offset) != sizeof(AssetHeader))
        {
            return;
        }

        if (asset_header.format_version != 0) 
        {
            LOG_WARNING(Loader, "NRO Asset Header has format {}, currently supported format is 0. If strange glitches occur with metadata, check NRO assets.", asset_header.format_version);
        }

        if (asset_header.magic != Common::MakeMagic('A', 'S', 'E', 'T'))
        {
            return;
        }

        if (asset_header.nacp.size > 0)
        {
            nacp = std::make_unique<FileSys::NACP>(std::make_shared<FileSys::OffsetVfsFile>(file, asset_header.nacp.size, offset + asset_header.nacp.offset, "Control.nacp"));
        }

        if (asset_header.romfs.size > 0)
        {
            romfs = std::make_shared<FileSys::OffsetVfsFile>(file, asset_header.romfs.size, offset + asset_header.romfs.offset, "game.romfs");
        }

        if (asset_header.icon.size > 0)
        {
            icon_data = file->ReadBytes(asset_header.icon.size, offset + asset_header.icon.offset);
        }
    }
}

AppLoader_NRO::~AppLoader_NRO() = default;

LoaderFileType AppLoader_NRO::IdentifyType(const FileSys::VirtualFile & nro_file)
{
    // Read NSO header
    NroHeader nro_header{};
    if (sizeof(NroHeader) != nro_file->ReadObject(&nro_header))
    {
        return LoaderFileType::Error;
    }
    if (nro_header.magic == Common::MakeMagic('N', 'R', 'O', '0'))
    {
        return LoaderFileType::NRO;
    }
    return LoaderFileType::Error;
}

bool AppLoader_NRO::IsHomebrew()
{
    // Read NSO header
    NroHeader nro_header{};
    if (sizeof(NroHeader) != file->ReadObject(&nro_header)) 
    {
        return false;
    }
    return nro_header.magic_ext1 == Common::MakeMagic('H', 'O', 'M', 'E') && nro_header.magic_ext2 == Common::MakeMagic('B', 'R', 'E', 'W');
}

static constexpr u32 PageAlignSize(u32 size)
{
    constexpr std::size_t YUZU_PAGEBITS = 12;
    constexpr uint64_t YUZU_PAGESIZE = 1ULL << YUZU_PAGEBITS;
    constexpr uint64_t YUZU_PAGEMASK = YUZU_PAGESIZE - 1;
    return static_cast<u32>((size + YUZU_PAGEMASK) & ~YUZU_PAGEMASK);
}

static bool LoadNroImpl(Systemloader & loader, ISystemModules & modules, const std::vector<u8> & data, uint64_t & baseAddress, uint64_t & processID)
{
    if (data.size() < sizeof(NroHeader))
    {
        return false;
    }

    // Read NSO header
    NroHeader nro_header{};
    std::memcpy(&nro_header, data.data(), sizeof(NroHeader));
    if (nro_header.magic != Common::MakeMagic('N', 'R', 'O', '0'))
    {
        return false;
    }

    // Build program image
    Kernel::CodeSet codeset;
    codeset.memory.resize(PageAlignSize(nro_header.file_size));
    Kernel::PhysicalMemory & program_image(codeset.memory);
    program_image.resize(PageAlignSize(nro_header.file_size));
    std::memcpy(program_image.data(), data.data(), program_image.size());
    if (program_image.size() != PageAlignSize(nro_header.file_size))
    {
        return false;
    }

    for (std::size_t i = 0; i < nro_header.segments.size(); ++i)
    {
        codeset.segments[i].addr = nro_header.segments[i].offset;
        codeset.segments[i].offset = nro_header.segments[i].offset;
        codeset.segments[i].size = PageAlignSize(nro_header.segments[i].size);
    }

    if (!Settings::values.program_args.GetValue().empty())
    {
        const auto arg_data = Settings::values.program_args.GetValue();
        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = program_image.size();
        program_image.resize(static_cast<u32>(program_image.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(program_image.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(program_image.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    // Default .bss to NRO header bss size if MOD0 section doesn't exist
    u32 bss_size{PageAlignSize(nro_header.bss_size)};

    // Read MOD header
    ModHeader mod_header{};
    std::memcpy(&mod_header, program_image.data() + nro_header.module_header_offset, sizeof(ModHeader));

    const bool has_mod_header{mod_header.magic == Common::MakeMagic('M', 'O', 'D', '0')};
    if (has_mod_header) 
    {
        // Resize program image to include .bss section and page align each section
        bss_size = PageAlignSize(mod_header.bss_end_offset - mod_header.bss_start_offset);
    }

    codeset.DataSegment().size += bss_size;
    program_image.resize(static_cast<u32>(program_image.size()) + bss_size);
    size_t image_size = program_image.size();

#if 0 && (defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__))
    const auto& code = codeset.CodeSegment();

    // NROs always have a 39-bit address space.
    g_settings->SetBool(NXLoaderSetting::Has39BitAddressSpace, true);

    // Create NCE patcher
    Core::NCE::Patcher patch{};

    if (g_settings->GetBool(NXCpuSetting::NceEnabled)) {
        // Patch SVCs and MRS calls in the guest code
        patch.PatchText(program_image.data(), program_image.size(), code.offset, code.size);

        // We only support PostData patching for NROs.
        ASSERT(patch.GetPatchMode() == Core::NCE::PatchMode::PostData);

        // Update patch section.
        auto& patch_segment = codeset.PatchSegment();
        patch_segment.addr = image_size;
        patch_segment.size = static_cast<u32>(patch.GetSectionSize());

        // Add patch section size to the module size.
        image_size += patch_segment.size;
    }
#endif

    // Enable direct memory mapping in case of NCE.
    const uint64_t fastmem_base = [&]() -> size_t {
        if (g_settings->GetBool(NXCpuSetting::NceEnabled)) {
            LOG_WARNING(Loader, "NRO NCE patching is not fully ported yet; using Dynarmic for homebrew NRO");
            g_settings->SetBool(NXCpuSetting::NceEnabled, false);
            return 0;
        }
        return 0;
    }();

    // Setup the process code layout
    IOperatingSystem & operatingSystem = modules.OperatingSystem();
    baseAddress = fastmem_base;
    if (!operatingSystem.CreateApplicationProcess(image_size + HblEnvAllocationSize, FileSys::ProgramMetadata::GetDefault(), baseAddress, processID, true))
    {
        return false;
    }

    uint64_t main_thread_handle_write_address = 0;
    const uint64_t hbl_env_context = AppendHomebrewEnvironment(program_image, baseAddress, "sdmc:/hbmenu.nro", &main_thread_handle_write_address);
    codeset.DataSegment().size += HblEnvAllocationSize;
    image_size = program_image.size();
    operatingSystem.SetMainThreadStartupArguments(hbl_env_context, UINT64_MAX, main_thread_handle_write_address);

    // Relocate code patch and copy to the program_image if running under NCE.
    // This needs to be after LoadFromMetadata so we can use the process entry point.
#if 0 && (defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__))
    if (g_settings->GetBool(NXCpuSetting::NceEnabled)) {
        uint32_t relocated_size = static_cast<uint32_t>(image_size);
        patch.RelocateAndCopy(baseAddress, code.offset, code.size, program_image.data(),
                              &relocated_size, nullptr);
        image_size = relocated_size;
    }
#endif

    // Load codeset for current process
    if (!operatingSystem.LoadModule(codeset, baseAddress))
    {
        return false;
    }
    return true;
}

bool AppLoader_NRO::LoadNro(Systemloader & loader, ISystemModules & modules, const FileSys::VfsFile & nro_file, uint64_t & baseAddress, uint64_t & processID)
{
    return LoadNroImpl(loader, modules, nro_file.ReadAllBytes(), baseAddress, processID);
}

AppLoader_NRO::LoadResult AppLoader_NRO::Load(Systemloader & loader, ISystemModules & systemModules)
{
    if (is_loaded) {
        return {LoaderResultStatus::ErrorAlreadyLoaded, {}};
    }

    uint64_t processID{};
    uint64_t baseAddress{};
    if (!LoadNro(loader, systemModules, *file, baseAddress, processID)) {
        return {LoaderResultStatus::ErrorLoadingNRO, {}};
    }

    FileSys::VirtualFile romFS;
    if (ReadRomFS(romFS) != LoaderResultStatus::Success) {
        LOG_WARNING(Service_FS, "Unable to read base RomFS");
    }
    uint64_t program_id{};
    ReadProgramId(program_id);
    loader.GetFileSystemController().RegisterProcess(
        processID, program_id,
        std::make_unique<FileSys::RomFSFactory>(romFS, IsRomFSUpdatable(), loader.GetContentProvider(),
                                                loader.GetFileSystemController()));

    is_loaded = true;
    return {LoaderResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE, baseAddress}};
}

LoaderResultStatus AppLoader_NRO::ReadIcon(uint8_t * buffer, uint32_t * bufferSize)
{
    if (icon_data.empty())
    {
        return LoaderResultStatus::ErrorNoIcon;
    }

    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    if (buffer != nullptr && *bufferSize < (uint32_t)icon_data.size())
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = (uint32_t)icon_data.size();
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, icon_data.data(), icon_data.size());
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NRO::ReadProgramId(uint64_t& out_program_id) {
    if (nacp == nullptr) {
        return LoaderResultStatus::ErrorNoControl;
    }

    out_program_id = nacp->GetTitleId();
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NRO::ReadRomFS(FileSys::VirtualFile& dir) {
    if (romfs == nullptr) {
        return LoaderResultStatus::ErrorNoRomFS;
    }

    dir = romfs;
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NRO::ReadTitle(char * buffer, uint32_t * bufferSize)
{
    if (nacp == nullptr)
    {
        return LoaderResultStatus::ErrorNoControl;
    }
    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    std::string title = nacp->GetApplicationName();
    if (buffer != nullptr && *bufferSize < (uint32_t)title.size())
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = (uint32_t)title.size();
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, title.data(), title.size());
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NRO::ReadControlData(FileSys::NACP& control) {
    if (nacp == nullptr) {
        return LoaderResultStatus::ErrorNoControl;
    }

    control = *nacp;
    return LoaderResultStatus::Success;
}

bool AppLoader_NRO::IsRomFSUpdatable() const {
    return false;
}

} // namespace Loader



