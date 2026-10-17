#include "emulation_session.h"

#include <android/log.h>
#include <android/native_window.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <yuzu_common/fs/path_util.h>
#include <vector>

#include <common/base64.h>
#include <common/json.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/base.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-module-spec/video.h>

namespace
{
    constexpr const char * kLogTag = "NxEmu";
    constexpr uint32_t kMaxArtworkBytes = 16 * 1024 * 1024;

    std::string EncodeRomImage(IRomInfo * rom, LoaderResultStatus (IRomInfo::*reader)(uint8_t *, uint32_t *))
    {
        if (rom == nullptr)
        {
            return {};
        }
        uint32_t size = 0;
        LoaderResultStatus status = (rom->*reader)(nullptr, &size);
        if (status != LoaderResultStatus::Success || size == 0 || size >= kMaxArtworkBytes)
        {
            return {};
        }
        std::vector<uint8_t> data(size);
        status = (rom->*reader)(data.data(), &size);
        if (status != LoaderResultStatus::Success || size == 0)
        {
            return {};
        }
        data.resize(size);
        return base64_encode(data.data(), data.size());
    }

    std::string ReadRomTitle(IRomInfo * rom)
    {
        if (rom == nullptr)
        {
            return {};
        }
        std::string title;
        uint32_t title_sz = 0;
        LoaderResultStatus title_res = rom->ReadTitle(nullptr, &title_sz);
        if (title_res == LoaderResultStatus::Success && title_sz > 0 && title_sz < 1024 * 1024)
        {
            title.resize(title_sz);
            title_res = rom->ReadTitle(title.data(), &title_sz);
            if (title_res != LoaderResultStatus::Success)
            {
                title.clear();
            }
        }
        if (const auto nul = title.find('\0'); nul != std::string::npos)
        {
            title.resize(nul);
        }
        return title;
    }
}

EmulationSession & EmulationSession::GetInstance()
{
    static EmulationSession instance;
    return instance;
}

void EmulationSession::InitializeSystem()
{
    std::lock_guard lock(m_mutex);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    RestoreStubModulesLocked();
    if (m_system_modules && m_system_modules->IsValid())
    {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "SystemModules: setup OK");
    }
    else
    {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "SystemModules: setup incomplete — ROM metadata will be unavailable");
        m_system_modules.reset();
    }
}

void EmulationSession::FlushSettings()
{
    std::lock_guard lock(m_mutex);
    if (m_system_modules && m_system_modules->IsValid())
    {
        m_system_modules->FlushSettings();
    }
}

std::string EmulationSession::SaveDataPaths(const std::string & title_id)
{
    std::lock_guard lock(m_mutex);
    if (title_id.size() != 16 || title_id.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
        !m_system_modules || !m_system_modules->IsValid() || m_native_window != nullptr)
    {
        return {};
    }
    auto & os = m_system_modules->Modules().OperatingSystem();
    HostProfileInfo profile{};
    const auto index = SettingsStore::GetInstance().GetInt("nxos:CurrentUser");
    if (!os.GetProfile(static_cast<uint32_t>(index), &profile))
    {
        return {};
    }
    uint64_t user[2]{};
    std::memcpy(user, profile.uuid, sizeof(user));
    char legacy_user[33]{};
    std::snprintf(legacy_user, sizeof(legacy_user), "%016llX%016llX",
                  static_cast<unsigned long long>(user[1]), static_cast<unsigned long long>(user[0]));
    std::string raw_user;
    for (const uint8_t byte : profile.uuid)
    {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", byte);
        raw_user += hex;
    }
    std::string title = title_id;
    for (char & ch : title) if (ch >= 'a' && ch <= 'f') ch -= 'a' - 'A';
    char base_title[17]{};
    std::snprintf(base_title, sizeof(base_title), "%016llX", std::strtoull(title.c_str(), nullptr, 16) & ~0xFFULL);
    const auto root = Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir) / "user/save";
    const auto account_future = root / "account" / raw_user / base_title / "0";
    const auto device_future = root / "device" / base_title / "0";
    const auto account = std::filesystem::is_directory(account_future) ? account_future :
        root / "0000000000000000" / legacy_user / title;
    const auto device = std::filesystem::is_directory(device_future) ? device_future :
        root / "0000000000000000" / "00000000000000000000000000000000" / title;
    JsonValue result(JsonValueType::Object);
    result["account"] = account.string();
    result["device"] = device.string();
    return JsonStyledWriter().write(result);
}

void EmulationSession::ShutdownSystem()
{
    std::lock_guard lock(m_mutex);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    if (m_system_modules && m_system_modules->IsValid())
    {
        m_system_modules->FlushSettings();
    }
    m_system_modules.reset();
    m_render_window.ClearSurface();
    m_native_window = nullptr;
    m_pixel_ratio = 1.0f;
}

void EmulationSession::RestoreStubModulesLocked()
{
    if (m_system_modules && m_system_modules->IsValid())
    {
        m_system_modules->FlushSettings();
    }
    m_system_modules.reset();
    m_render_window.ClearSurface();
    m_native_window = nullptr;
    m_pixel_ratio = 1.0f;
    m_system_modules = std::make_unique<SystemModules>();
    m_system_modules->Setup(m_render_window);
    if (!m_system_modules->IsValid())
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SystemModules setup failed:\n%s",
                            m_system_modules->LastSetupDiagnostic().c_str());
    }
}

bool EmulationSession::Run(ANativeWindow * native_window, float pixel_ratio, const std::string & rom_path)
{
    std::lock_guard lock(m_mutex);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    if (m_system_modules && m_system_modules->IsValid())
    {
        m_system_modules->FlushSettings();
    }
    m_system_modules.reset();

    if (native_window == nullptr)
    {
        RestoreStubModulesLocked();
        return false;
    }

    m_native_window = native_window;
    m_pixel_ratio = pixel_ratio > 0.f ? pixel_ratio : 1.0f;
    m_render_window.AttachSurface(native_window, m_pixel_ratio);
    m_system_modules = std::make_unique<SystemModules>();
    m_system_modules->Setup(m_render_window);
    if (!m_system_modules->IsValid())
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "SystemModules setup failed:\n%s",
                            m_system_modules->LastSetupDiagnostic().c_str());
    }

    if (!m_system_modules->IsValid())
    {
        RestoreStubModulesLocked();
        return false;
    }

    const auto & settings = SettingsStore::GetInstance();
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "Boot settings: resolution=%d scale=%.2f language=%d region=%d vibration=%d",
                        settings.GetInt("nxvideo:ResolutionSetup"), settings.GetFloat("nxvideo:ResolutionUpFactor"),
                        settings.GetInt("nxos:LanguageIndex"), settings.GetInt("nxos:RegionIndex"),
                        settings.GetBool("nxos:VibrationEnabled"));
    const int native_w = ANativeWindow_getWidth(m_native_window);
    const int native_h = ANativeWindow_getHeight(m_native_window);
    if (native_w > 0 && native_h > 0)
    {
        m_system_modules->Modules().Video().UpdateFramebufferLayout(static_cast<uint32_t>(native_w),
                                                                 static_cast<uint32_t>(native_h));
    }

    ISystemloader & loader = m_system_modules->Modules().Systemloader();
    if (!loader.LoadRom(rom_path.c_str(), 0, 0, -1, ApplicationLaunchType::FrontendInitiated))
    {
        RestoreStubModulesLocked();
        return false;
    }
    return true;
}

void EmulationSession::Stop()
{
    std::lock_guard lock(m_mutex);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    RestoreStubModulesLocked();
}

void EmulationSession::SetPaused(bool paused)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return;
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    if (!os.IsPoweredOn())
    {
        return;
    }
    os.SetEmulationPaused(paused);
}

bool EmulationSession::IsPaused()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return false;
    }
    return m_system_modules->Modules().OperatingSystem().IsEmulationPaused();
}

void EmulationSession::SetNativeWindow(ANativeWindow * native_window)
{
    std::lock_guard lock(m_mutex);
    m_render_window.AttachSurface(native_window, m_pixel_ratio);
    m_native_window = native_window;
}

void EmulationSession::SurfaceDestroyed()
{
    std::lock_guard lock(m_mutex);
    const bool keep_running = m_system_modules && m_system_modules->IsValid() &&
                              SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning);
    m_render_window.SuppressPresentation();
    if (keep_running)
    {
        m_system_modules->Modules().Video().NotifyWindowChanged();
    }
}

void EmulationSession::SurfaceChanged()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid() || m_native_window == nullptr)
    {
        return;
    }
    if (!SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning))
    {
        return;
    }
    IVideo & video = m_system_modules->Modules().Video();
    video.NotifyWindowChanged();
    const int native_w = ANativeWindow_getWidth(m_native_window);
    const int native_h = ANativeWindow_getHeight(m_native_window);
    if (native_w > 0 && native_h > 0)
    {
        video.UpdateFramebufferLayout(static_cast<uint32_t>(native_w), static_cast<uint32_t>(native_h));
    }
}

bool EmulationSession::ReadAppletCapture(std::vector<uint8_t> & out, uint32_t & width, uint32_t & height)
{
    IVideo * video = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_system_modules || !m_system_modules->IsValid())
        {
            return false;
        }
        video = &m_system_modules->Modules().Video();
    }
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
    const uint32_t size = video->GetLinearAppletCaptureBuffer(nullptr, 0, &capture_width, &capture_height);
    if (size == 0 || capture_width == 0 || capture_height == 0)
    {
        return false;
    }
    out.resize(size);
    video->GetLinearAppletCaptureBuffer(out.data(), size, &capture_width, &capture_height);
    width = capture_width;
    height = capture_height;
    return width * height * 4 <= out.size();
}

void EmulationSession::AppletCaptureSize(uint32_t & width, uint32_t & height)
{
    width = 0;
    height = 0;
    IVideo * video = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_system_modules || !m_system_modules->IsValid())
        {
            return;
        }
        video = &m_system_modules->Modules().Video();
    }
    video->GetLinearAppletCaptureBuffer(nullptr, 0, &width, &height);
}

ANativeWindow * EmulationSession::NativeWindow() const
{
    std::lock_guard lock(m_mutex);
    return m_native_window;
}

float EmulationSession::PixelRatio() const
{
    std::lock_guard lock(m_mutex);
    return m_pixel_ratio;
}

std::string EmulationSession::QueryRomMetadata(const std::string & path)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return {};
    }

    ISystemloader & loader = m_system_modules->Modules().Systemloader();
    IRomInfo * const rom = loader.RomInfo(path.c_str(), 0, 0);
    if (rom == nullptr)
    {
        return {};
    }

    const LoaderFileType file_type = rom->GetFileType();

    uint64_t program_id = 0;
    const LoaderResultStatus pid_res = rom->ReadProgramId(program_id);
    const bool have_pid = pid_res == LoaderResultStatus::Success;

    const std::string title = ReadRomTitle(rom);
    const std::string icon_b64 = EncodeRomImage(rom, &IRomInfo::ReadIcon);

    rom->Release();

    char pid_hex[17];
    if (have_pid)
    {
        snprintf(pid_hex, sizeof(pid_hex), "%016llx", (unsigned long long)program_id);
    }
    else
    {
        snprintf(pid_hex, sizeof(pid_hex), "0");
    }

    JsonValue obj(JsonValueType::Object);
    obj["path"] = path;
    obj["title"] = title;
    obj["programId"] = pid_hex;
    obj["fileType"] = static_cast<int>(file_type);
    obj["icon"] = icon_b64;
    obj["error"] = JsonValue();
    return JsonStyledWriter().write(obj);
}

std::string EmulationSession::QueryRomInfo(const std::string & path)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return {};
    }

    ISystemloader & loader = m_system_modules->Modules().Systemloader();
    IRomInfo * rom = nullptr;
    if (!path.empty())
    {
        rom = loader.RomInfo(path.c_str(), 0, 0);
    }
    if (rom == nullptr)
    {
        rom = loader.LoadedRomInfo();
    }
    if (rom == nullptr)
    {
        return {};
    }

    JsonValue obj(JsonValueType::Object);
    obj["title"] = ReadRomTitle(rom);
    obj["icon"] = EncodeRomImage(rom, &IRomInfo::ReadIcon);
    obj["logo"] = EncodeRomImage(rom, &IRomInfo::ReadLogo);
    obj["banner"] = EncodeRomImage(rom, &IRomInfo::ReadBanner);
    rom->Release();
    return JsonStyledWriter().write(obj);
}

std::array<double, 4> EmulationSession::GetPerfStats()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return {0.0, 0.0, 0.0, 0.0};
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    if (!os.IsPoweredOn())
    {
        return {0.0, 0.0, 0.0, 0.0};
    }
    const PerfStatsResults results = os.GetAndResetPerfStats();
    return {results.system_fps, results.average_game_fps, results.frametime, results.emulation_speed};
}

uint32_t EmulationSession::GetShadersBuilding()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return 0;
    }
    return m_system_modules->Modules().Video().ShadersBuilding();
}

std::string EmulationSession::GetFirmwareVersion()
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return "N/A";
    }
    char buffer[32]{};
    const uint32_t length =
        m_system_modules->Modules().Systemloader().GetInstalledFirmwareDisplayVersion(buffer, sizeof(buffer));
    if (length == 0)
    {
        return "N/A";
    }
    return std::string(buffer, length);
}

void EmulationSession::SetOverlayButton(int port, int button_id, bool pressed)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return;
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    if (!os.IsPoweredOn())
    {
        return;
    }
    os.SetPlayerButtonState((uint32_t)port, (uint32_t)button_id, pressed);
}

void EmulationSession::SetOverlayJoystick(int port, int stick_id, float x, float y)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return;
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    if (!os.IsPoweredOn())
    {
        return;
    }
    os.SetPlayerAnalogState((uint32_t)port, (uint32_t)stick_id, x, y);
}

int EmulationSession::GetStyleIndex(int player_index)
{
    std::lock_guard lock(m_mutex);
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return static_cast<int>(NpadStyleIndex::Fullkey);
    }
    IOperatingSystem & os = m_system_modules->Modules().OperatingSystem();
    const NpadIdType npad_id = player_index == 8 ? NpadIdType::Handheld : (NpadIdType)player_index;
    return (int)os.GetEmulatedController(npad_id).GetNpadStyleIndex();
}

IOperatingSystem * EmulationSession::OperatingSystem()
{
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return nullptr;
    }
    return &m_system_modules->Modules().OperatingSystem();
}

SystemModules * EmulationSession::Modules()
{
    if (!m_system_modules || !m_system_modules->IsValid())
    {
        return nullptr;
    }
    return m_system_modules.get();
}
