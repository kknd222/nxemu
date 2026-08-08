#include "os_manager.h"
#include "profile_image_writer.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "yuzu_common/hex_util.h"
#include "core/memory/cheat_engine.h"
#include "core/perf_stats.h"
#include "os_settings.h"
#include "os_settings_identifiers.h"
#include "yuzu_audio_core/sink/sink_details.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/string_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_hid_core/frontend/emulated_controller.h"
#include "yuzu_hid_core/hid_core.h"
#include "yuzu_input_common/drivers/keyboard.h"
#include "yuzu_input_common/drivers/virtual_gamepad.h"
#include "yuzu_input_common/main.h"
#include <nxemu-core/settings/identifiers.h>
#include <filesystem>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <array>
#include <vector>
#include <string>
#include <unordered_set>

namespace
{
    constexpr char ACC_SAVE_AVATORS_BASE_PATH[] = "system/save/8000000000000010/su/avators";

    std::string TitleIdToHex(uint64_t title_id)
    {
        char buffer[17]{};
        std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(title_id));
        return buffer;
    }

    bool EqualsIgnoreCase(std::string a, std::string b)
    {
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return a == b;
    }

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return {};
        }
        std::ostringstream out;
        out << file.rdbuf();
        return out.str();
    }

    std::string TrimAsciiWhitespace(std::string text)
    {
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
        {
            text.erase(0, 3);
        }

        const auto is_space = [](unsigned char c) {
            return std::isspace(c) != 0;
        };

        while (!text.empty() && is_space(static_cast<unsigned char>(text.front())))
        {
            text.erase(text.begin());
        }
        while (!text.empty() && is_space(static_cast<unsigned char>(text.back())))
        {
            text.pop_back();
        }
        return text;
    }

    std::string NormalizeCheatName(std::string text)
    {
        text = TrimAsciiWhitespace(std::move(text));
        if (text.size() >= 2 && text.front() == '[' && text.back() == ']')
        {
            text = text.substr(1, text.size() - 2);
            text = TrimAsciiWhitespace(std::move(text));
        }

        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return text;
    }

    std::string CheatEntryName(const Core::Memory::CheatEntry& entry)
    {
        const auto& name = entry.definition.readable_name;
        const auto end = std::find(name.begin(), name.end(), '\0');
        return std::string(name.begin(), end);
    }

    std::unordered_set<std::string> LoadCheatNameSet(const std::filesystem::path& path, bool& exists)
    {
        exists = false;
        std::unordered_set<std::string> out;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
        {
            return out;
        }

        exists = true;
        std::istringstream input(ReadTextFile(path));
        std::string line;
        while (std::getline(input, line))
        {
            line = TrimAsciiWhitespace(std::move(line));
            if (line.empty() || line.front() == '#' || line.front() == ';')
            {
                continue;
            }
            if (line.rfind("//", 0) == 0)
            {
                continue;
            }

            const auto comment_pos = line.find('#');
            if (comment_pos != std::string::npos)
            {
                line.resize(comment_pos);
            }
            line = NormalizeCheatName(std::move(line));
            if (!line.empty())
            {
                out.insert(std::move(line));
            }
        }
        return out;
    }

    void ApplyCheatSelection(std::vector<Core::Memory::CheatEntry>& cheats,
                             const std::filesystem::path& cheat_file)
    {
        const auto cheat_dir = cheat_file.parent_path();
        bool enabled_exists = false;
        bool disabled_exists = false;
        const auto enabled_names = LoadCheatNameSet(cheat_dir / "enabled.txt", enabled_exists);
        const auto disabled_names = LoadCheatNameSet(cheat_dir / "disabled.txt", disabled_exists);

        if (!enabled_exists && !disabled_exists)
        {
            return;
        }

        const bool enable_all = enabled_names.contains("*") || enabled_names.contains("all");
        uint32_t enabled_count = 0;
        for (auto& cheat : cheats)
        {
            const auto name = NormalizeCheatName(CheatEntryName(cheat));
            const bool has_opcodes = cheat.definition.num_opcodes > 0;

            if (enabled_exists)
            {
                cheat.enabled = has_opcodes && (enable_all || enabled_names.contains(name));
            }
            else
            {
                cheat.enabled = has_opcodes;
            }

            if (disabled_exists && disabled_names.contains(name))
            {
                cheat.enabled = false;
            }

            if (cheat.enabled)
            {
                ++enabled_count;
            }
        }

        LOG_INFO(CheatEngine,
                 "Applied cheat selection for {}: enabled.txt={}, disabled.txt={}, enabled {}/{} entries",
                 cheat_file.string(), enabled_exists, disabled_exists, enabled_count, cheats.size());
    }

    std::vector<std::filesystem::path> FindCheatFiles(uint64_t title_id, const std::array<uint8_t, 0x20>& build_id)
    {
        std::vector<std::filesystem::path> out;
        const auto load_root = Common::FS::GetYuzuPath(Common::FS::YuzuPath::LoadDir);
        const auto title_root = load_root / TitleIdToHex(title_id);
        std::error_code ec;
        if (!std::filesystem::is_directory(title_root, ec))
        {
            return out;
        }

        const auto build_id_full = Common::HexToString(build_id);
        std::string build_id_trimmed = build_id_full;
        const auto last_non_zero = build_id_trimmed.find_last_not_of('0');
        if (last_non_zero != std::string::npos)
        {
            build_id_trimmed.resize(last_non_zero + 1);
        }

        std::vector<std::string> valid_names;
        if (build_id_full.size() >= 16)
        {
            valid_names.push_back(build_id_full.substr(0, 16));
        }
        if (build_id_trimmed.size() >= 16)
        {
            valid_names.push_back(build_id_trimmed.substr(0, 16));
        }
        if (!build_id_trimmed.empty())
        {
            valid_names.push_back(build_id_trimmed);
        }
        valid_names.push_back(build_id_full);

        const auto scan_cheat_dir = [&](const std::filesystem::path& cheat_dir) {
            std::error_code scan_ec;
            if (!std::filesystem::is_directory(cheat_dir, scan_ec))
            {
                return;
            }
            for (const auto& entry : std::filesystem::directory_iterator(cheat_dir, scan_ec))
            {
                if (scan_ec || !entry.is_regular_file(scan_ec) || !EqualsIgnoreCase(entry.path().extension().string(), ".txt"))
                {
                    continue;
                }
                const auto stem = entry.path().stem().string();
                for (const auto& name : valid_names)
                {
                    if (EqualsIgnoreCase(stem, name))
                    {
                        out.push_back(entry.path());
                        break;
                    }
                }
            }
        };

        // yuzu/Ryujinx style directly under load/<title_id>/cheats
        scan_cheat_dir(title_root / "cheats");

        // NXEmu add-ons style: load/<title_id>/<mod name>/cheats
        std::error_code iter_ec;
        for (const auto& entry : std::filesystem::directory_iterator(title_root, iter_ec))
        {
            if (!iter_ec && entry.is_directory(iter_ec) && !EqualsIgnoreCase(entry.path().filename().string(), "cheats"))
            {
                scan_cheat_dir(entry.path() / "cheats");
            }
        }

        return out;
    }

    class IButtonMappingListImpl : public IButtonMappingList
    {
    public:
        explicit IButtonMappingListImpl(const std::unordered_map<NativeAnalogValues, Common::ParamPackage>& mappings)
        {
            m_indices.reserve(mappings.size());
            m_params.reserve(mappings.size());

            for (const auto& [index, param] : mappings)
            {
                m_indices.push_back(static_cast<uint32_t>(index));
                m_params.emplace_back(new IParamPackageImpl(param));
            }
        }
        explicit IButtonMappingListImpl(const std::unordered_map<NativeButtonValues, Common::ParamPackage>& mappings)
        {
            m_indices.reserve(mappings.size());
            m_params.reserve(mappings.size());

            for (const auto& [index, param] : mappings)
            {
                m_indices.push_back(static_cast<uint32_t>(index));
                m_params.emplace_back(new IParamPackageImpl(param));
            }
        }
        explicit IButtonMappingListImpl(const std::unordered_map<NativeMotionValues, Common::ParamPackage>& mappings)
        {
            m_indices.reserve(mappings.size());
            m_params.reserve(mappings.size());

            for (const auto& [index, param] : mappings)
            {
                m_indices.push_back(static_cast<uint32_t>(index));
                m_params.emplace_back(new IParamPackageImpl(param));
            }
        }

        ~IButtonMappingListImpl()
        {
            for (IParamPackageImpl* item : m_params)
            {
                item->Release();
            }
        }

        uint32_t GetCount() const override
        {
            return static_cast<uint32_t>(m_indices.size());
        }

        uint32_t GetIndex(uint32_t position) const override
        {
            return m_indices[position];
        }

        IParamPackage& GetParamPackage(uint32_t position) const override
        {
            return *m_params[position];
        }

        void Release() override
        {
            delete this;
        }

    private:
        std::vector<uint32_t> m_indices;
        std::vector<IParamPackageImpl*> m_params;
    };

    bool FillHostProfileInfo(const Service::Account::ProfileManager & manager, std::size_t index, HostProfileInfo * out_profile)
    {
        if (out_profile == nullptr)
        {
            return false;
        }

        const auto uuid = manager.GetUser(index);
        if (!uuid)
        {
            return false;
        }

        Service::Account::ProfileBase profile{};
        if (!manager.GetProfileBase(*uuid, profile))
        {
            return false;
        }

        std::memcpy(out_profile->uuid, profile.user_uuid.uuid.data(), HOST_PROFILE_UUID_SIZE);
        const std::string username = Common::StringFromFixedZeroTerminatedBuffer((const char *)profile.username.data(), profile.username.size());
        std::memset(out_profile->username, 0, sizeof(out_profile->username));
        std::strncpy(out_profile->username, username.c_str(), HOST_PROFILE_USERNAME_SIZE);
        return true;
    }

    std::filesystem::path ProfileImageFilesystemPath(const Common::UUID & uuid)
    {
        return Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir) / ACC_SAVE_AVATORS_BASE_PATH / (uuid.FormattedString() + ".jpg");
    }
}

extern IModuleSettings * g_settings;

OSManager::OSManager(ISystemModules & modules) :
    m_modules(modules),
    m_coreSystem(modules),
    m_process(nullptr)
{
}

OSManager::~OSManager()
{
    if (m_process != nullptr)
    {
        m_process->Close();
        m_process = nullptr;
    }
}

void OSManager::EmulationStarting()
{
    g_settings->SetBool(NXOsSetting::UseSpeedLimit, true);

    m_emuThread = std::make_unique<EmuThread>(m_coreSystem, m_process);
    m_emuThread->Start();
}

void OSManager::EmulationStopping(bool wait)
{
    g_settings->SetBool(NXOsSetting::UseSpeedLimit, true);

    if (m_emuThread)
    {
        m_emuThread->Stop();
        if (wait)
        {
            m_emuThread.reset();
        }
    }
}

bool OSManager::Initialize(void)
{
    SetupOsSetting();
    m_coreSystem.Initialize();
    m_coreSystem.HIDCore().ReloadInputDevices();
    return true;
}

void OSManager::ShutDown()
{
    m_coreSystem.SetShuttingDown(true);
    if (m_coreSystem.IsPoweredOn())
    {
        m_coreSystem.SetExitRequested(true);
        m_coreSystem.GetAppletManager().RequestExit();
    }
    m_emuThread->SetRunning(true);
}

bool OSManager::IsShuttingDown() const
{
    return m_coreSystem.IsShuttingDown();
}

bool OSManager::IsPoweredOn() const
{
    return m_coreSystem.IsPoweredOn();
}

void OSManager::ShutdownMainProcess()
{
    m_coreSystem.ShutdownMainProcess();
}

bool OSManager::CreateApplicationProcess(uint64_t codeSize, const IProgramMetadata & metaData, uint64_t & baseAddress, uint64_t & processID, bool is_hbl)
{
    if (m_process != nullptr)
    {
        return false;
    }
    m_coreSystem.InitializeKernel(metaData.GetTitleID());
    Kernel::KernelCore & kernel = m_coreSystem.Kernel();
    m_process = Kernel::KProcess::Create(kernel);
    if (m_process == nullptr)
    {
        return false;
    }
    Kernel::KProcess::Register(kernel, m_process);
    kernel.AppendNewProcess(m_process);
    kernel.MakeApplicationProcess(m_process);

    if (m_process->LoadFromMetadata(metaData, codeSize, 0, false).IsError())
    {
        return false;
    }
    
    auto params = Service::AM::FrontendAppletParameters{
        .applet_id = Service::AM::AppletId::Application,
        .applet_type = Service::AM::AppletType::Application,
        .launch_type = Service::AM::LaunchType::FrontendInitiated,
    };
    params.program_id = metaData.GetTitleID();
    m_coreSystem.GetAppletManager().CreateAndInsertByFrontendAppletParameters(m_process->GetProcessId(), params);

    processID = m_process->GetProcessId();
    baseAddress = GetInteger(m_process->GetEntryPoint());
    return true;
}

void OSManager::StartApplicationProcess(int32_t priority, int64_t stackSize, uint32_t version, StorageId baseGameStorageId, StorageId updateStorageId, uint8_t * nacpData, uint32_t nacpDataLen)
{
    m_coreSystem.AddGlueRegistrationForProcess(*m_process, version, baseGameStorageId, updateStorageId, nacpData, nacpDataLen);
    m_process->Run(priority, stackSize);
}

bool OSManager::LoadModule(const IModuleInfo & module, uint64_t baseAddress)
{
    if (m_process == nullptr)
    {
        return false;
    }
    m_process->LoadModule(module, baseAddress);
    return true;
}

void OSManager::RegisterCheatMetadata(const uint8_t build_id_raw[32], uint64_t main_region_begin, uint64_t main_region_size)
{
    std::array<uint8_t, 0x20> build_id{};
    std::memcpy(build_id.data(), build_id_raw, build_id.size());

    const uint64_t title_id = m_coreSystem.GetApplicationProcessProgramID();
    const auto cheat_files = FindCheatFiles(title_id, build_id);
    if (cheat_files.empty())
    {
        LOG_INFO(CheatEngine, "No cheat file found for title_id={:016X}, build_id={}", title_id, Common::HexToString(build_id));
        return;
    }

    Core::Memory::TextCheatParser parser;
    std::vector<Core::Memory::CheatEntry> merged_cheats;
    for (const auto& cheat_file : cheat_files)
    {
        const auto text = ReadTextFile(cheat_file);
        if (text.empty())
        {
            LOG_WARNING(CheatEngine, "Cheat file is empty or unreadable: {}", cheat_file.string());
            continue;
        }

        auto parsed = parser.Parse(text);
        if (parsed.empty())
        {
            LOG_WARNING(CheatEngine, "Failed to parse cheat file: {}", cheat_file.string());
            continue;
        }

        ApplyCheatSelection(parsed, cheat_file);

        const auto enabled_count =
            std::count_if(parsed.begin(), parsed.end(), [](const Core::Memory::CheatEntry& entry) {
                return entry.enabled;
            });
        LOG_INFO(CheatEngine, "Loaded {} cheat entries ({} enabled) from {}", parsed.size(),
                 enabled_count, cheat_file.string());
        merged_cheats.insert(merged_cheats.end(), parsed.begin(), parsed.end());
    }

    if (merged_cheats.empty())
    {
        LOG_WARNING(CheatEngine, "All matching cheat files were empty/unparseable for title_id={:016X}", title_id);
        return;
    }

    for (uint32_t i = 0; i < merged_cheats.size(); ++i)
    {
        merged_cheats[i].cheat_id = i;
    }

    // Some cheats use large Main NSO-relative offsets that land in the application code/ASLR
    // mapping beyond the decompressed main NSO image size. Keep the final IsValidVirtualAddress
    // guard in StandardVmCallbacks, but avoid rejecting these addresses only because the image
    // size passed by the NSO loader was too small.
    constexpr uint64_t MinimumMainCheatRegionSize = 0x10000000ULL;
    const uint64_t effective_main_region_size = std::max(main_region_size, MinimumMainCheatRegionSize);
    if (effective_main_region_size != main_region_size)
    {
        LOG_INFO(CheatEngine, "Expanding cheat main region size from {:#X} to {:#X}",
                 main_region_size, effective_main_region_size);
    }

    m_coreSystem.RegisterCheatList(merged_cheats, build_id, main_region_begin, effective_main_region_size);
}

IDeviceMemory & OSManager::DeviceMemory(void)
{
    return m_coreSystem.DeviceMemory();
}

void OSManager::KeyboardKeyPress(int modifier, int keyIndex, int keyCode)
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->GetKeyboard()->SetKeyboardModifiers(modifier);
    input_subsystem->GetKeyboard()->PressKeyboardKey(keyIndex);
    input_subsystem->GetKeyboard()->PressKey(keyCode);
    input_subsystem->PumpEvents();
}

void OSManager::KeyboardKeyRelease(int modifier, int keyIndex, int keyCode)
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->GetKeyboard()->SetKeyboardModifiers(modifier);
    input_subsystem->GetKeyboard()->ReleaseKeyboardKey(keyIndex);
    input_subsystem->GetKeyboard()->ReleaseKey(keyCode);
    input_subsystem->PumpEvents();
}

void OSManager::GatherGPUDirtyMemory(ICacheInvalidator * invalidator)
{
    m_coreSystem.GatherGPUDirtyMemory(invalidator);
}

uint64_t OSManager::GetGPUTicks()
{
    return m_coreSystem.CoreTiming().GetGPUTicks();
}

uint64_t OSManager::GetProgramId()
{
    return m_coreSystem.ApplicationProcess()->GetProgramId();
}

bool OSManager::GetExitLocked() const
{
    return m_coreSystem.GetExitLocked();
}

void OSManager::GameFrameEnd()
{
    m_coreSystem.GetPerfStats().EndGameFrame();
}

void OSManager::AudioGetSyncIDs(uint32_t * ids, uint32_t maxCount, uint32_t* actualCount)
{
    std::vector<AudioCore::Sink::AudioEngine> sinkIds = AudioCore::Sink::GetSinkIDs();
    if (actualCount)
    {
        *actualCount = (uint32_t)sinkIds.size();
    }

    if (ids != nullptr && maxCount > 0 && sinkIds.size() > 0)
    {
        memcpy(ids, sinkIds.data(), std::min(maxCount, (uint32_t)sinkIds.size()) * sizeof(uint32_t));
    }
}

void OSManager::AudioGetDeviceListForSink(uint32_t sinkId, bool capture, DeviceEnumCallback callback, void * userData)
{
    std::vector<std::string> devices = AudioCore::Sink::GetDeviceListForSink((AudioCore::Sink::AudioEngine)sinkId, capture);
    for (size_t i = 0, n = devices.size(); i < n; i++)
    {
        callback(devices[i].c_str(), userData);
    }
}

void OSManager::RegisterHostThread()
{
    m_coreSystem.RegisterHostThread();
}

IParamPackageList * OSManager::GetInputDevices() const
{
    return new IParamPackageListImpl(m_coreSystem.InputSubsystem()->GetInputDevices());
}

IEmulatedController & OSManager::GetEmulatedController(NpadIdType index)
{
    return *m_coreSystem.HIDCore().GetEmulatedController(index);
}

ButtonNames OSManager::GetButtonName(const IParamPackage& param) const
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    return input_subsystem->GetButtonName(param);
}

bool OSManager::IsController(const IParamPackage & params) const
{
    std::shared_ptr<InputCommon::InputSubsystem>& input_subsystem = m_coreSystem.InputSubsystem();
    return input_subsystem->IsController(params);
}

NpadStyleSet OSManager::GetSupportedStyleTag() const
{
    return m_coreSystem.HIDCore().GetSupportedStyleTag().raw;
}

IButtonMappingList * OSManager::GetButtonMappingForDevice(const IParamPackage & param) const
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    return new IButtonMappingListImpl(input_subsystem->GetButtonMappingForDevice(param));
}

IButtonMappingList * OSManager::GetAnalogMappingForDevice(const IParamPackage & param) const
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    return new IButtonMappingListImpl(input_subsystem->GetAnalogMappingForDevice(param));
}

IButtonMappingList * OSManager::GetMotionMappingForDevice(const IParamPackage & param) const
{
    std::shared_ptr<InputCommon::InputSubsystem>& input_subsystem = m_coreSystem.InputSubsystem();
    return new IButtonMappingListImpl(input_subsystem->GetMotionMappingForDevice(param));
}

void OSManager::BeginMapping(PollingInputType type)
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->BeginMapping(type);
}

void OSManager::StopMapping()
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->StopMapping();
}

IParamPackage * OSManager::GetNextInput() const
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    return new IParamPackageImpl(input_subsystem->GetNextInput());
}

void OSManager::PumpInputEvents() const
{
    std::shared_ptr<InputCommon::InputSubsystem>& input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->PumpEvents();
}

PerfStatsResults OSManager::GetAndResetPerfStats()
{
    return m_coreSystem.GetAndResetPerfStats();
}

void OSManager::SetEmulationPaused(bool paused)
{
    if (!m_emuThread)
    {
        return;
    }
    m_emuThread->SetRunning(!paused);
}

bool OSManager::IsEmulationPaused() const
{
    if (!m_emuThread)
    {
        return false;
    }
    return !m_emuThread->IsRunning();
}

void OSManager::SetFrontendApplets(ICabinetApplet * cabinet, IControllerApplet * controller, IErrorApplet * error, IMiiEditApplet * mii_edit, IParentalControlsApplet * parental_controls, IPhotoViewerApplet * photo_viewer, IProfileSelectApplet * profile_select, ISoftwareKeyboardApplet * software_keyboard, IWebBrowserApplet * web_browser)
{
    Service::AM::Frontend::FrontendAppletSet applets{};
    applets.cabinet = cabinet;
    applets.controller = controller;
    applets.error = error;
    applets.mii_edit = mii_edit;
    applets.parental_controls = parental_controls;
    applets.photo_viewer = photo_viewer;
    applets.profile_select = profile_select;
    applets.software_keyboard = software_keyboard;
    applets.web_browser = web_browser;
    m_coreSystem.SetFrontendAppletSet(std::move(applets));
}

void OSManager::SetPlayerButtonState(uint32_t player_index, uint32_t button_ordinal, bool pressed)
{
    InputCommon::VirtualGamepad* const virtual_gamepad = m_coreSystem.InputSubsystem()->GetVirtualGamepad();
    if (virtual_gamepad == nullptr)
    {
        return;
    }
    virtual_gamepad->SetButtonState(player_index, static_cast<int>(button_ordinal), pressed);
}

void OSManager::SetPlayerAnalogState(uint32_t player_index, uint32_t stick_index, float x, float y)
{
    InputCommon::VirtualGamepad* const virtual_gamepad = m_coreSystem.InputSubsystem()->GetVirtualGamepad();
    if (virtual_gamepad == nullptr)
    {
        return;
    }
    virtual_gamepad->SetStickPosition(player_index, static_cast<int>(stick_index), x, y);
}

uint32_t OSManager::GetProfileCount() const
{
    Service::Account::ProfileManager manager;
    return (uint32_t)manager.GetUserCount();
}

bool OSManager::GetProfile(uint32_t index, HostProfileInfo * out_profile) const
{
    Service::Account::ProfileManager manager;
    return FillHostProfileInfo(manager, index, out_profile);
}

bool OSManager::CreateProfile(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE], const char * username_utf8, HostProfileInfo * out_profile)
{
    if (uuid_bytes == nullptr || username_utf8 == nullptr || username_utf8[0] == '\0')
    {
        return false;
    }

    Service::Account::ProfileManager manager;
    if (manager.GetUserCount() >= Service::Account::MAX_USERS)
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};
    if (uuid.IsInvalid() || manager.UserExists(uuid))
    {
        return false;
    }

    if (manager.CreateNewUser(uuid, std::string(username_utf8)).IsError())
    {
        return false;
    }

    manager.WriteUserSaveFile();
    if (out_profile == nullptr)
    {
        return true;
    }

    const auto index = manager.GetUserIndex(uuid);
    return index && FillHostProfileInfo(manager, *index, out_profile);
}

bool OSManager::RenameProfile(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE], const char * username_utf8)
{
    if (uuid_bytes == nullptr || username_utf8 == nullptr || username_utf8[0] == '\0')
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};

    Service::Account::ProfileManager manager;
    Service::Account::ProfileBase profile{};
    if (!manager.GetProfileBase(uuid, profile))
    {
        return false;
    }

    const std::string username(username_utf8);
    profile.username.fill(0);
    std::copy_n(username.begin(), std::min(username.size(), profile.username.size()), profile.username.begin());

    if (!manager.SetProfileBase(uuid, profile))
    {
        return false;
    }

    manager.WriteUserSaveFile();
    return true;
}

bool OSManager::RemoveProfile(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE])
{
    if (uuid_bytes == nullptr)
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};

    Service::Account::ProfileManager manager;
    if (manager.GetUserCount() < 2 || !manager.RemoveUser(uuid))
    {
        return false;
    }

    manager.WriteUserSaveFile();
    return true;
}

bool OSManager::SetProfileImage(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE], const uint8_t * image_data, uint32_t image_size)
{
    if (uuid_bytes == nullptr || image_data == nullptr || image_size == 0)
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};

    Service::Account::ProfileManager manager;
    if (!manager.UserExists(uuid))
    {
        return false;
    }

    return WriteProfileJpegFromMemory(image_data, image_size, ProfileImageFilesystemPath(uuid));
}

bool OSManager::GetProfileImagePath(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE], char * out_path, uint32_t out_path_size) const
{
    if (uuid_bytes == nullptr || out_path == nullptr || out_path_size == 0)
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};

    Service::Account::ProfileManager manager;
    if (!manager.UserExists(uuid))
    {
        return false;
    }

    const std::filesystem::path imagePath = ProfileImageFilesystemPath(uuid);
    std::error_code ec;
    if (!std::filesystem::exists(imagePath, ec) || ec)
    {
        out_path[0] = '\0';
        return true;
    }

    const std::string path = Common::FS::PathToUTF8String(imagePath);
    if (path.size() + 1 > out_path_size)
    {
        return false;
    }

    std::memcpy(out_path, path.c_str(), path.size() + 1);
    return true;
}
