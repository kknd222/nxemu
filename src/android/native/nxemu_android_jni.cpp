#include <jni.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <mutex>
#include <memory>
#include <vector>
#include <csignal>
#include <sys/ucontext.h>
#include <sys/system_properties.h>

#include <android/log.h>
#include <android/api-level.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <strings.h>
#include <unistd.h>

#include "nxemu-core/app_init.h"
#include "nxemu-core/modules/system_modules.h"
#include "nxemu-core/notification.h"
#include "nxemu-core/settings/identifiers.h"
#include "nxemu-core/settings/settings.h"
#include "nxemu-cpu/cpu_settings_identifiers.h"
#include "nxemu-loader/loader_settings_identifiers.h"
#include "nxemu-video/video_settings_identifiers.h"
#include "nxemu-module-spec/system_loader.h"
#include "nxemu-module-spec/operating_system.h"
#include "yuzu_common/android/java_bridge.h"
#include "yuzu_common/fs/fs_android.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"

namespace Service::NxemuAndroidDiagnostics {
void SetFullTrace(bool enabled);
bool IsFullTraceEnabled();
}

extern IModuleSettings* g_settings;

namespace {

constexpr jint JniVersion = JNI_VERSION_1_6;
constexpr const char* LogTag = "NxEmuAndroid";
// NCE is now controlled by the in-app preference. Keep the debug properties below as an
// emergency override path for older builds/devices, but do not silently force Dynarmic on
// normal Android runs; otherwise the UI can say "NCE requested" while the core stays slow.
constexpr bool kAndroidNceStabilityGuardDefault = false;

jobject g_native_library_class = nullptr;
ANativeWindow* g_native_window = nullptr;
std::unique_ptr<SystemModules> g_system_modules;
std::string g_files_dir;
std::string g_native_library_dir;
std::string g_app_dir;
std::string g_gpu_hook_lib_dir;
std::string g_gpu_custom_driver_dir;
std::string g_gpu_custom_driver_name;
std::string g_gpu_file_redirect_dir;
bool g_runtime_initialized = false;
bool g_last_boot_attempted = false;
bool g_last_boot_loaded = false;
bool g_shutdown_requested = false;
int g_last_input_player = -1;
int g_last_input_button = -1;
bool g_last_input_pressed = false;
int g_last_analog_player = -1;
int g_last_analog_stick = -1;
float g_last_analog_x = 0.0f;
float g_last_analog_y = 0.0f;
std::string g_last_boot_path;
std::string g_last_status = "status=created";
int g_android_frame_skip = 0;
int g_android_resolution_setup = 0;
int g_android_aspect_ratio = 4; // AspectRatio::Stretch, yuzu/Eden/Citron-style phone fullscreen
bool g_android_graphics_compat = false;
bool g_android_prefer_nce = false;
bool g_last_logged_nce_state_valid = false;
int g_last_logged_nce_backend = -1;
bool g_last_logged_nce_has_39bit = false;
bool g_last_logged_nce_fastmem = false;
bool g_last_logged_nce_enabled = false;
std::mutex g_runtime_mutex;
std::mutex g_progress_mutex;
std::vector<std::pair<std::string, long long>> g_load_stages;
std::string g_load_current_stage = "created";
long long g_load_start_ms = 0;
long long g_load_last_stage_ms = 0;
struct sigaction g_prev_segv_action {};
struct sigaction g_prev_bus_action {};
bool g_have_prev_segv_action = false;
bool g_have_prev_bus_action = false;

bool ReadAndroidBoolProperty(const char* name, bool default_value) {
    char value[PROP_VALUE_MAX] = {};
    const int len = __system_property_get(name, value);
    if (len <= 0) {
        return default_value;
    }
    return std::strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
           strcasecmp(value, "y") == 0 || strcasecmp(value, "yes") == 0 ||
           strcasecmp(value, "on") == 0;
}

int ReadAndroidIntProperty(const char* name, int default_value) {
    char value[PROP_VALUE_MAX] = {};
    const int len = __system_property_get(name, value);
    if (len <= 0) {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

bool IsAndroidNceExperimentEnabled() {
    return ReadAndroidBoolProperty("debug.nxemu.nce", false) ||
           ReadAndroidBoolProperty("persist.debug.nxemu.nce", false);
}

bool IsAndroidNceGuardActive() {
    return kAndroidNceStabilityGuardDefault && !IsAndroidNceExperimentEnabled();
}

const char* AspectRatioLabel(int aspect_ratio) {
    switch (aspect_ratio) {
    case 0:
        return "16:9";
    case 1:
        return "4:3";
    case 2:
        return "21:9";
    case 3:
        return "16:10";
    case 4:
        return "Stretch";
    default:
        return "Unknown";
    }
}

class AndroidNotification final : public INotification {
public:
    void DisplayError(const char* message, const char* title) const override {
        last_error = std::string(title ? title : "Error") + ": " + (message ? message : "");
    }

    NotificationResponse Query(const char*, const char*) const override {
        return NotificationResponse::No;
    }

    void BreakPoint(const char* fileName, uint32_t lineNumber) override {
        last_error = std::string("Breakpoint: ") + (fileName ? fileName : "") + ":" +
                     std::to_string(lineNumber);
    }

    void AppInitDone() override {}

    mutable std::string last_error;
};

class AndroidRenderWindow final : public IRenderWindow {
public:
    void* RenderSurface() const override {
        return g_native_window;
    }

    float PixelRatio() const override {
        return 1.0f;
    }
};

AndroidNotification g_android_notification;
AndroidRenderWindow g_android_render_window;

void ReplaceNativeLibraryClass(JNIEnv* env, jclass clazz) {
    if (g_native_library_class != nullptr) {
        ClearNativeLibraryGlobalRef(env);
        g_native_library_class = nullptr;
    }

    g_native_library_class = env->NewGlobalRef(clazz);
    SetNativeLibraryGlobalRef(g_native_library_class);
    Common::FS::Android::RegisterCallbacks(env, static_cast<jclass>(g_native_library_class));
}

std::array<char, 4> ReadMagicAt(std::ifstream& file, std::streamoff offset) {
    std::array<char, 4> magic{};
    file.seekg(offset, std::ios::beg);
    file.read(magic.data(), magic.size());
    return magic;
}

void ForwardFatalSignal(int sig, siginfo_t* info, void* context) {
    const auto& action = sig == SIGBUS ? g_prev_bus_action : g_prev_segv_action;
    const bool have_action = sig == SIGBUS ? g_have_prev_bus_action : g_have_prev_segv_action;
    if (have_action) {
        if ((action.sa_flags & SA_SIGINFO) != 0 && action.sa_sigaction != nullptr) {
            action.sa_sigaction(sig, info, context);
            return;
        }
        if (action.sa_handler == SIG_IGN) {
            return;
        }
        if (action.sa_handler != nullptr && action.sa_handler != SIG_DFL) {
            action.sa_handler(sig);
            return;
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void AndroidFatalSignalHandler(int sig, siginfo_t* info, void* context) {
#if defined(__aarch64__)
    const auto& host_ctx = static_cast<ucontext_t*>(context)->uc_mcontext;
    const unsigned long long pc = static_cast<unsigned long long>(host_ctx.pc);
    const unsigned long long sp = static_cast<unsigned long long>(host_ctx.sp);
#else
    const unsigned long long pc = 0;
    const unsigned long long sp = 0;
#endif
    Dl_info dl_info {};
    const char* symbol = "unknown";
    const char* object = "unknown";
    unsigned long long object_base = 0;
    unsigned long long object_offset = 0;
    if (dladdr(reinterpret_cast<void*>(pc), &dl_info) != 0) {
        if (dl_info.dli_sname != nullptr) {
            symbol = dl_info.dli_sname;
        }
        if (dl_info.dli_fname != nullptr) {
            object = dl_info.dli_fname;
        }
        object_base = reinterpret_cast<unsigned long long>(dl_info.dli_fbase);
        object_offset = object_base != 0 ? pc - object_base : 0;
    }
    __android_log_print(ANDROID_LOG_FATAL, LogTag,
                        "fatalSignal sig=%d code=%d addr=%p pc=0x%llx sp=0x%llx object=%s "
                        "objectBase=0x%llx objectOffset=0x%llx symbol=%s stage=%s lastBoot=%s "
                        "nceRequested=%d",
                        sig, info != nullptr ? info->si_code : 0,
                        info != nullptr ? info->si_addr : nullptr, pc, sp, object, object_base,
                        object_offset, symbol, g_load_current_stage.c_str(),
                        g_last_boot_path.c_str(), g_android_prefer_nce ? 1 : 0);
    ForwardFatalSignal(sig, info, context);
}

void InstallAndroidFatalSignalDiagnostics() {
    auto install_one = [](int sig, struct sigaction& previous, bool& have_previous) {
        struct sigaction current {};
        sigaction(sig, nullptr, &current);

        struct sigaction action {};
        action.sa_sigaction = AndroidFatalSignalHandler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_RESTART;

        struct sigaction old_action {};
        if (sigaction(sig, &action, &old_action) != 0) {
            return;
        }

        // This function is intentionally called multiple times around Vulkan/custom-driver
        // initialization. Do not save our own handler as the previous handler, otherwise a crash
        // would recurse inside ForwardFatalSignal instead of re-raising to debuggerd.
        const bool old_is_self =
            (old_action.sa_flags & SA_SIGINFO) != 0 &&
            old_action.sa_sigaction == AndroidFatalSignalHandler;
        const bool current_is_self =
            (current.sa_flags & SA_SIGINFO) != 0 &&
            current.sa_sigaction == AndroidFatalSignalHandler;
        if (!old_is_self && !current_is_self) {
            previous = old_action;
            have_previous = true;
        }
    };

    install_one(SIGSEGV, g_prev_segv_action, g_have_prev_segv_action);
    install_one(SIGBUS, g_prev_bus_action, g_have_prev_bus_action);
    __android_log_print(ANDROID_LOG_INFO, LogTag, "fatal signal diagnostics installed stage=%s",
                        g_load_current_stage.c_str());
}

void RefreshNceEnabledLocked(const char* reason) {
    if (!g_runtime_initialized || g_settings == nullptr) {
        return;
    }
    const bool nce_guard_active = IsAndroidNceGuardActive();
    if (nce_guard_active) {
        g_settings->SetInt(NXCpuSetting::CpuBackend, 0);
        g_settings->SetBool(NXCpuSetting::NceEnabled, false);
        const bool is_status = reason != nullptr && std::strcmp(reason, "status") == 0;
        if (!is_status || !g_last_logged_nce_state_valid || g_last_logged_nce_backend != 0 ||
            g_last_logged_nce_enabled) {
            __android_log_print(ANDROID_LOG_INFO, LogTag,
                                "refreshNceEnabled: reason=%s nativeGuard=1 nceExperiment=0 backend=0 nce=0",
                                reason != nullptr ? reason : "");
        }
        g_last_logged_nce_state_valid = true;
        g_last_logged_nce_backend = 0;
        g_last_logged_nce_has_39bit = false;
        g_last_logged_nce_fastmem = false;
        g_last_logged_nce_enabled = false;
        return;
    }
    const bool is_nce = g_settings->GetInt(NXCpuSetting::CpuBackend) == 1;
    const bool has_39bit = g_settings->GetBool(NXLoaderSetting::Has39BitAddressSpace);
    const bool fastmem = g_settings->GetBool(NXCpuSetting::CpuoptFastmem);
    const bool nce_enabled = is_nce && has_39bit && fastmem;
    const int backend = g_settings->GetInt(NXCpuSetting::CpuBackend);
    g_settings->SetBool(NXCpuSetting::NceEnabled, nce_enabled);

    const bool is_status = reason != nullptr && std::strcmp(reason, "status") == 0;
    const bool state_changed = !g_last_logged_nce_state_valid ||
                               g_last_logged_nce_backend != backend ||
                               g_last_logged_nce_has_39bit != has_39bit ||
                               g_last_logged_nce_fastmem != fastmem ||
                               g_last_logged_nce_enabled != nce_enabled;
    if (!is_status || state_changed) {
        __android_log_print(ANDROID_LOG_INFO, LogTag,
                            "refreshNceEnabled: reason=%s backend=%d has39=%d fastmem=%d nce=%d",
                            reason != nullptr ? reason : "", backend, has_39bit ? 1 : 0,
                            fastmem ? 1 : 0, nce_enabled ? 1 : 0);
    }
    g_last_logged_nce_state_valid = true;
    g_last_logged_nce_backend = backend;
    g_last_logged_nce_has_39bit = has_39bit;
    g_last_logged_nce_fastmem = fastmem;
    g_last_logged_nce_enabled = nce_enabled;
}

long long SteadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void ResetLoadProgress(const std::string& stage) {
    std::lock_guard lk{g_progress_mutex};
    g_load_stages.clear();
    g_load_start_ms = SteadyNowMs();
    g_load_last_stage_ms = g_load_start_ms;
    g_load_current_stage = stage;
    g_load_stages.emplace_back(stage, 0);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, LogTag, "loadProgress reset stage=%s", stage.c_str());
#endif
}

void MarkLoadStage(const std::string& stage) {
    std::lock_guard lk{g_progress_mutex};
    const long long now = SteadyNowMs();
    if (g_load_start_ms == 0) {
        g_load_start_ms = now;
        g_load_last_stage_ms = now;
    }
    g_load_current_stage = stage;
    g_load_stages.emplace_back(stage, now - g_load_start_ms);
    g_load_last_stage_ms = now;
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, LogTag, "loadProgress stage=%s elapsedMs=%lld",
                        stage.c_str(), now - g_load_start_ms);
#endif
}

std::string BuildLoadProgress() {
    std::lock_guard lk{g_progress_mutex};
    const long long now = SteadyNowMs();
    const long long elapsed = g_load_start_ms != 0 ? now - g_load_start_ms : 0;
    const long long stage_elapsed = g_load_last_stage_ms != 0 ? now - g_load_last_stage_ms : 0;
    std::ostringstream out;
    out << "loadProgress=ok\n";
    out << "currentStage=" << g_load_current_stage << "\n";
    out << "elapsedMs=" << elapsed << "\n";
    out << "currentStageElapsedMs=" << stage_elapsed << "\n";
    out << "stageCount=" << g_load_stages.size() << "\n";
    out << "stages:\n";
    const std::size_t start = g_load_stages.size() > 18 ? g_load_stages.size() - 18 : 0;
    for (std::size_t i = start; i < g_load_stages.size(); ++i) {
        out << "  " << g_load_stages[i].second << "ms " << g_load_stages[i].first << "\n";
    }
    return out.str();
}

std::array<char, 4> ReadMagicAt(FILE* file, long offset) {
    std::array<char, 4> magic{};
    if (file == nullptr) {
        return magic;
    }
    if (std::fseek(file, offset, SEEK_SET) != 0) {
        return magic;
    }
    std::fread(magic.data(), 1, magic.size(), file);
    return magic;
}

std::string MagicToString(const std::array<char, 4>& magic) {
    std::string text;
    text.reserve(magic.size());
    for (char c : magic) {
        text.push_back(c >= 0x20 && c <= 0x7e ? c : '.');
    }
    return text;
}

std::string ExtensionOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return "";
    }

    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

std::string ProbeGameFileFromMagic(const std::string& path, long long size, const std::string& source,
                                   const std::array<char, 4>& magic0,
                                   const std::array<char, 4>& magic10,
                                   const std::array<char, 4>& magic100,
                                   const std::array<char, 4>& magic200) {
    const std::string extension = ExtensionOf(path);
    std::ostringstream out;
    out << "probe=ok\n";
    out << "path=" << path << "\n";
    out << "size=" << size << "\n";
    out << "extension=" << extension << "\n";
    out << "source=" << source << "\n";

    if (magic10 == std::array<char, 4>{'N', 'R', 'O', '0'}) {
        out << "format=NRO homebrew\n";
        out << "nxemuPreferredExtension=nro\n";
        out << "targetLoader=NRO\n";
    } else if (magic0 == std::array<char, 4>{'P', 'F', 'S', '0'}) {
        out << "format=DNSP/NSP PFS0 container\n";
        out << "nxemuPreferredExtension=dnsp\n";
        out << "targetLoader=NSP\n";
    } else if (magic100 == std::array<char, 4>{'H', 'E', 'A', 'D'}) {
        out << "format=DXCI/XCI gamecard image\n";
        out << "nxemuPreferredExtension=dxci\n";
        out << "targetLoader=XCI\n";
    } else if (magic200 == std::array<char, 4>{'N', 'C', 'A', '3'} ||
               magic200 == std::array<char, 4>{'N', 'C', 'A', '2'}) {
        out << "format=NCA content archive\n";
        out << "nxemuPreferredExtension=nca\n";
        out << "targetLoader=NCA\n";
    } else {
        out << "format=unknown\n";
        if (extension == "dxci" || extension == "xci") {
            out << "targetLoader=XCI-by-extension\n";
        } else if (extension == "dnsp" || extension == "nsp") {
            out << "targetLoader=NSP-by-extension\n";
        } else if (extension == "nro") {
            out << "targetLoader=NRO-by-extension\n";
        } else if (extension == "nca") {
            out << "targetLoader=NCA-by-extension\n";
        } else {
            out << "targetLoader=unknown\n";
        }
        out << "magic@0x0=" << MagicToString(magic0) << "\n";
        out << "magic@0x10=" << MagicToString(magic10) << "\n";
        out << "magic@0x100=" << MagicToString(magic100) << "\n";
        out << "magic@0x200=" << MagicToString(magic200) << "\n";
    }

    return out.str();
}

std::string ProbeContentUriGameFile(const std::string& path) {
    MarkLoadStage("probe content-uri begin");
    const auto size = static_cast<long long>(Common::FS::Android::GetSize(path));
    if (size < 0x20) {
        return "probe=failed\nreason=content uri file too small or size unavailable\npath=" + path +
               "\nsize=" + std::to_string(size);
    }

    const int fd = Common::FS::Android::OpenContentUri(path, Common::FS::Android::OpenMode::Read);
    if (fd < 0) {
        return "probe=failed\nreason=open content uri failed\npath=" + path;
    }

    FILE* file = fdopen(fd, "rb");
    if (file == nullptr) {
        close(fd);
        return "probe=failed\nreason=fdopen content uri failed\npath=" + path;
    }

    const auto magic0 = ReadMagicAt(file, 0x0);
    const auto magic10 = ReadMagicAt(file, 0x10);
    const auto magic100 = ReadMagicAt(file, 0x100);
    const auto magic200 = ReadMagicAt(file, 0x200);
    std::fclose(file);

    auto result = ProbeGameFileFromMagic(path, size, "content-uri", magic0, magic10, magic100, magic200);
    MarkLoadStage("probe content-uri end");
    return result;
}

std::string ProbeHostGameFile(const std::string& path) {
    MarkLoadStage("probe host-file begin");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return "probe=failed\nreason=open failed\npath=" + path;
    }

    const auto size = file.tellg();
    if (size < 0x20) {
        return "probe=failed\nreason=file too small\npath=" + path;
    }

    const auto magic0 = ReadMagicAt(file, 0x0);
    const auto magic10 = ReadMagicAt(file, 0x10);
    const auto magic100 = ReadMagicAt(file, 0x100);
    const auto magic200 = ReadMagicAt(file, 0x200);

    auto result = ProbeGameFileFromMagic(path, static_cast<long long>(size), "host-file", magic0, magic10,
                                         magic100, magic200);
    MarkLoadStage("probe host-file end");
    return result;
}

std::string ProbeGameFile(const std::string& path) {
    if (Common::FS::Android::IsContentUri(path)) {
        return ProbeContentUriGameFile(path);
    }
    return ProbeHostGameFile(path);
}

void ApplyGpuDriverSettingsLocked() {
    if (!g_runtime_initialized) {
        return;
    }

    SettingsStore& settings = SettingsStore::GetInstance();
    if (!g_gpu_hook_lib_dir.empty()) {
        settings.SetString(NXCoreSetting::GpuHookLibDir, g_gpu_hook_lib_dir.c_str());
    }
    settings.SetString(NXCoreSetting::GpuCustomDriverDir, g_gpu_custom_driver_dir.c_str());
    settings.SetString(NXCoreSetting::GpuCustomDriverName, g_gpu_custom_driver_name.c_str());
    settings.SetString(NXCoreSetting::GpuFileRedirectDir, g_gpu_file_redirect_dir.c_str());
}

void ApplyAndroidPerformanceSettingsLocked(const char* reason) {
    if (!g_runtime_initialized) {
        return;
    }

    SettingsStore& settings = SettingsStore::GetInstance();

    // Android default performance profile. These are deliberately conservative enough
    // to keep correctness while reducing mobile GPU load and shader stalls.
    settings.SetInt(NXCoreSetting::AndroidFrameSkip, g_android_frame_skip);
    const bool effective_nce_request = g_android_prefer_nce && !IsAndroidNceGuardActive();
    settings.SetInt(NXCpuSetting::CpuBackend, effective_nce_request ? 1 : 0);
    settings.SetInt(NXVideoSetting::ResolutionSetup, g_android_resolution_setup);
    const int astc_mode = std::clamp(ReadAndroidIntProperty("debug.nxemu.astc_mode", 0), 0, 2);
    settings.SetInt(NXVideoSetting::AstcDecodeMode, astc_mode);
    settings.SetInt(NXVideoSetting::ASTCRecompressionMethod, 0);
    // Follow yuzu/Eden/Citron Android's "Stretch to window" route for phone fullscreen:
    // keep the Java SurfaceView MATCH_PARENT and let the renderer framebuffer layout fill
    // the actual window. Do not add extra Activity fullscreen flags here; the previous
    // black-screen regression was reproduced with those flags still active even after
    // reverting aspect ratio.
    settings.SetInt(NXVideoSetting::AspectRatio, std::clamp(g_android_aspect_ratio, 0, 4));
    settings.SetBool(NXVideoSetting::ForceMaximumClocks, true);

    // Do not disable every async GPU path just because NCE is requested. That made Android NCE
    // technically start, but it serialized shader build/presentation work and kept real games
    // (Metal Dogs etc.) CPU/GPU-bound at very low speed. Short RMX3700 probes are stable with the
    // async GPU worker enabled, so keep all async paths on by default; a debug property remains for
    // quick fallback on driver/device combinations that regress.
    const bool allow_async_shader_building =
        ReadAndroidBoolProperty("debug.nxemu.force_async_shaders",
                                !g_android_graphics_compat);
    const bool allow_async_presentation = true;
    const bool allow_async_gpu_emulation =
        !ReadAndroidBoolProperty("debug.nxemu.disable_async_gpu_with_nce", false);
    settings.SetBool(NXVideoSetting::UseAsynchronousShaderBuilding, allow_async_shader_building);
    settings.SetBool(NXVideoSetting::UseAsynchronousGPUEmulation, allow_async_gpu_emulation);
    settings.SetBool(NXVideoSetting::EnableAsynchronousPresentation, allow_async_presentation);
    settings.SetBool(NXVideoSetting::FastGPUTime, true);
    settings.SetBool(NXVideoSetting::UseVulkanPipelineCache,
                     ReadAndroidBoolProperty("debug.nxemu.pipeline_cache",
                                             !g_android_graphics_compat));
    settings.SetBool(NXVideoSetting::EnableReactiveFlushing,
                     ReadAndroidBoolProperty("debug.nxemu.reactive_flushing",
                                             g_android_graphics_compat));
    settings.SetInt(NXVideoSetting::AnisotropicFiltering, 0);          // Default/no forced AF
    settings.SetInt(NXVideoSetting::VRAMUsageMode, 0);                 // Conservative
    // Compatibility mode is meant to trade a bit of speed for fewer title/driver-specific
    // rendering stalls.  Metal Dogs on RMX3700 + Turnip 26.2 stays alive with the same
    // profile that the probe script used: compat=true, reactive=true, async shaders=false,
    // and GPU accuracy=High(1).  Keep the debug property as an explicit override for A/B.
    const int default_gpu_accuracy = g_android_graphics_compat ? 1 : 0;
    settings.SetInt(NXVideoSetting::AccuracyLevel,
                    std::clamp(ReadAndroidIntProperty("debug.nxemu.gpu_accuracy",
                                                      default_gpu_accuracy),
                               0, 2));

    // NceEnabled is derived by cpu_settings.cpp from backend + fastmem + loader 39-bit eligibility.
    // Do not force-clear it here: boot-game/profile changes happen both before and after the loader
    // learns whether the title uses the 39-bit address space. Clearing it after the loader enables it
    // makes the CPU manager silently fall back to Dynarmic.
    settings.SetInt(NXCpuSetting::CpuAccuracy, 0);                     // Auto
    settings.SetBool(NXCpuSetting::CpuDebugMode, false);
    settings.SetBool(NXCpuSetting::CpuoptFastmem, true);
    settings.SetBool(NXCpuSetting::CpuoptFastmemExclusives, false);
    settings.SetBool(NXCpuSetting::CpuoptRecompileExclusives, true);
    settings.SetBool(NXCpuSetting::CpuoptIgnoreMemoryAborts, true);
    settings.SetBool(NXCpuSetting::CpuoptUnsafeFastmemCheck, true);
    settings.SetBool(NXCpuSetting::CpuoptUnsafeIgnoreGlobalMonitor, true);
    RefreshNceEnabledLocked(reason);

    __android_log_print(ANDROID_LOG_INFO, LogTag,
                        "applyAndroidPerformanceSettings: reason=%s res=%d astc=%d accuracy=%d reactive=%d pipelineCache=%d aspect=%s(%d) graphicsCompat=%d asyncShader=%d asyncPresent=1 asyncGpu=%d maxClock=1 nceRequest=%d",
                        reason != nullptr ? reason : "", g_android_resolution_setup,
                        astc_mode, settings.GetInt(NXVideoSetting::AccuracyLevel),
                        settings.GetBool(NXVideoSetting::EnableReactiveFlushing) ? 1 : 0,
                        settings.GetBool(NXVideoSetting::UseVulkanPipelineCache) ? 1 : 0,
                        AspectRatioLabel(g_android_aspect_ratio), g_android_aspect_ratio,
                        g_android_graphics_compat ? 1 : 0, allow_async_shader_building ? 1 : 0,
                        allow_async_gpu_emulation ? 1 : 0, effective_nce_request ? 1 : 0);
}

std::string BuildPerformanceStatusLocked() {
    std::ostringstream out;
    SettingsStore& settings = SettingsStore::GetInstance();
    RefreshNceEnabledLocked("status");
    out << "performanceProfile=android-default\n";
    out << "frameSkip=" << g_android_frame_skip << "\n";
    out << "resolutionSetup=" << settings.GetInt(NXVideoSetting::ResolutionSetup) << "\n";
    out << "astcDecodeMode=" << settings.GetInt(NXVideoSetting::AstcDecodeMode) << "\n";
    out << "astcRecompression=" << settings.GetInt(NXVideoSetting::ASTCRecompressionMethod) << "\n";
    // Before runtime-init, ApplyAndroidPerformanceSettingsLocked intentionally returns early and
    // SettingsStore still has the PC/default 16:9 value. Report the Android-requested value here
    // so the Java diagnostics do not briefly claim 16:9 while the UI/default is Stretch.
    const int aspect_ratio = g_runtime_initialized
                                 ? settings.GetInt(NXVideoSetting::AspectRatio)
                                 : std::clamp(g_android_aspect_ratio, 0, 4);
    out << "aspectRatio=" << aspect_ratio << "\n";
    out << "aspectRatioLabel=" << AspectRatioLabel(aspect_ratio) << "\n";
    out << "forceMaxClock=" << (settings.GetBool(NXVideoSetting::ForceMaximumClocks) ? "true" : "false") << "\n";
    out << "asyncShaders=" << (settings.GetBool(NXVideoSetting::UseAsynchronousShaderBuilding) ? "true" : "false") << "\n";
    out << "asyncGpu=" << (settings.GetBool(NXVideoSetting::UseAsynchronousGPUEmulation) ? "true" : "false") << "\n";
    out << "asyncPresentation=" << (settings.GetBool(NXVideoSetting::EnableAsynchronousPresentation) ? "true" : "false") << "\n";
    out << "fastGpuTime=" << (settings.GetBool(NXVideoSetting::FastGPUTime) ? "true" : "false") << "\n";
    out << "graphicsCompat=" << (g_android_graphics_compat ? "true" : "false") << "\n";
    out << "vulkanPipelineCache=" << (settings.GetBool(NXVideoSetting::UseVulkanPipelineCache) ? "true" : "false") << "\n";
    out << "reactiveFlushing=" << (settings.GetBool(NXVideoSetting::EnableReactiveFlushing) ? "true" : "false") << "\n";
    out << "gpuAccuracy=" << settings.GetInt(NXVideoSetting::AccuracyLevel) << "\n";
    out << "nceRequested=" << (g_android_prefer_nce ? "true" : "false") << "\n";
    out << "nceNativeGuard=" << (IsAndroidNceGuardActive() ? "true" : "false") << "\n";
    out << "nceExperimentProperty=" << (IsAndroidNceExperimentEnabled() ? "true" : "false") << "\n";
    out << "cpuBackendSetting=" << settings.GetInt(NXCpuSetting::CpuBackend) << "\n";
    out << "nceEligible39Bit=" << (settings.GetBool(NXLoaderSetting::Has39BitAddressSpace) ? "true" : "false") << "\n";
    out << "nceFastmem=" << (settings.GetBool(NXCpuSetting::CpuoptFastmem) ? "true" : "false") << "\n";
    const bool nce_enabled = settings.GetBool(NXCpuSetting::NceEnabled);
    out << "nceEnabled=" << (nce_enabled ? "true" : "false") << "\n";
    out << "cpuBackendActual=" << (nce_enabled ? "NCE" : "Dynarmic") << "\n";
    out << "cpuAccuracy=" << settings.GetInt(NXCpuSetting::CpuAccuracy) << "\n";
    out << "nativeBuildOptimized="
#ifdef NDEBUG
        << "true\n";
#else
        << "false\n";
#endif
    out << "androidFullTrace="
        << (Service::NxemuAndroidDiagnostics::IsFullTraceEnabled() ? "true" : "false");
    return out.str();
}

std::string BuildGpuDriverStatusLocked() {
    std::ostringstream out;
    out << "gpuHookLibDir=" << g_gpu_hook_lib_dir << "\n";
    out << "gpuCustomDriverDir=" << g_gpu_custom_driver_dir << "\n";
    out << "gpuCustomDriverName=" << g_gpu_custom_driver_name << "\n";
    out << "gpuFileRedirectDir=" << g_gpu_file_redirect_dir << "\n";
#if defined(__ANDROID__) && (defined(_M_ARM64) || defined(ARCHITECTURE_arm64))
    out << "gpuCustomDriverSupported="
        << ((android_get_device_api_level() >= 28 && access("/dev/kgsl-3d0", F_OK) == 0) ? "true" : "false");
#else
    out << "gpuCustomDriverSupported=false";
#endif
    return out.str();
}

void ApplyAndroidDefaultLogFilterLocked() {
#if defined(__ANDROID__)
    // Keep Android playable builds quiet by default: these classes can emit thousands of
    // STUBBED/HLE/NVDRV/VI lines per minute and the formatting + logcat I/O is visible as FPS loss.
    // Error/Critical still pass through; detailed service history remains available through
    // NxemuAndroidDiagnostics sampling and can be expanded with NXEMU_ANDROID_FULL_DIAG builds.
    auto set_level = [](LogClass log_class, LogLevel level) {
        Common::Log::g_classLevel[static_cast<uint8_t>(log_class)] = level;
    };
    set_level(LogClass::Service, LogLevel::Error);
    set_level(LogClass::Service_AM, LogLevel::Error);
    set_level(LogClass::Service_SM, LogLevel::Error);
    set_level(LogClass::Service_NVDRV, LogLevel::Error);
    set_level(LogClass::Service_Nvnflinger, LogLevel::Error);
    set_level(LogClass::Service_VI, LogLevel::Error);
    set_level(LogClass::HW_Memory, LogLevel::Error);
#endif
}
std::string EnsureRuntimeInitialized() {
    if (g_runtime_initialized) {
        return "runtime=already initialized";
    }
    if (g_files_dir.empty() || g_app_dir.empty()) {
        return "runtime=failed\nreason=files/app dir not set";
    }

    if (!AppInit(&g_android_notification, g_files_dir.c_str(), g_app_dir.c_str())) {
        return "runtime=failed\nreason=AppInit failed\nlast_error=" + g_android_notification.last_error;
    }

    g_runtime_initialized = true;
    ApplyAndroidDefaultLogFilterLocked();
    InstallAndroidFatalSignalDiagnostics();

    SettingsStore& settings = SettingsStore::GetInstance();
    if (!g_native_library_dir.empty()) {
        settings.SetString(NXCoreSetting::ModuleDirectory, g_native_library_dir.c_str());
    }
    Settings::values.use_auto_stub.SetValue(true);
    ApplyGpuDriverSettingsLocked();
    ApplyAndroidPerformanceSettingsLocked("runtime-init");
    MarkLoadStage("runtime initialized");

    std::ostringstream out;
    out << "runtime=initialized\n";
    out << "baseDir=" << g_files_dir << "\n";
    out << "appDir=" << g_app_dir << "\n";
    out << "moduleDir=" << g_native_library_dir << "\n";
    out << "gpuDriver:\n" << BuildGpuDriverStatusLocked() << "\n";
    out << "performance:\n" << BuildPerformanceStatusLocked();
    return out.str();
}


const char* GetDynamicDiagnosticSymbol(const char* symbol, const char* fallback) {
    using Fn = const char* (*)();
    void* raw = dlsym(RTLD_DEFAULT, symbol);
    if (raw == nullptr) {
        return fallback;
    }
    auto fn = reinterpret_cast<Fn>(raw);
    return fn != nullptr ? fn() : fallback;
}

const char* GetLoaderHblDiagnostics() {
    return GetDynamicDiagnosticSymbol(
        "NxemuGetLastHblEnvDiagnostics", "hblEnvDiagnostics=unavailable-loader-symbol");
}

const char* GetMainThreadStartupDiagnostics() {
    return GetDynamicDiagnosticSymbol(
        "NxemuGetLastMainThreadStartupDiagnostics", "mainThreadStartupDiagnostics=unavailable-os-symbol");
}

const char* GetLoadRomDiagnostics() {
    return GetDynamicDiagnosticSymbol(
        "NxemuGetLastLoadRomDiagnostics", "loadRomDiagnostics=unavailable-loader-symbol");
}

const char* GetNceLoaderDiagnostics() {
    return GetDynamicDiagnosticSymbol(
        "NxemuGetLastNceLoaderDiagnostics", "nceLoaderDiagnostics=unavailable-loader-symbol");
}

const char* GetOsServiceDiagnostics() {
    return GetDynamicDiagnosticSymbol(
        "NxemuGetLastOsServiceDiagnostics", "osServiceDiagnostics=unavailable-os-symbol");
}

std::string RequestGuestCpuSampleDynamic() {
    using Fn = void (*)();
    void* raw = dlsym(RTLD_DEFAULT, "NxemuRequestGuestCpuSample");
    if (raw == nullptr) {
        return "cpuSample=failed\nreason=unavailable-os-symbol";
    }
    auto fn = reinterpret_cast<Fn>(raw);
    if (fn == nullptr) {
        return "cpuSample=failed\nreason=null-symbol";
    }
    fn();
    return "cpuSample=requested";
}

std::string EnsureSystemModulesReady();

void* ResolveOptionalSymbol(const char* symbol) {
    void* raw = dlsym(RTLD_DEFAULT, symbol);
    if (raw != nullptr) {
        return raw;
    }
    // libnxemu-android.so is intentionally not linked against libnxemu-video.so.
    // Renderer diagnostics live there, so resolve them lazily if that SO is already loaded.
    void* video = dlopen("libnxemu-video.so", RTLD_NOW | RTLD_NOLOAD);
    if (video != nullptr) {
        raw = dlsym(video, symbol);
        dlclose(video);
        if (raw != nullptr) {
            return raw;
        }
    }
    return nullptr;
}

uint64_t GetDynamicU64Symbol(const char* symbol) {
    using Fn = uint64_t (*)();
    void* raw = ResolveOptionalSymbol(symbol);
    if (raw == nullptr) {
        return 0;
    }
    auto fn = reinterpret_cast<Fn>(raw);
    return fn != nullptr ? fn() : 0;
}

uint32_t GetDynamicU32Symbol(const char* symbol) {
    using Fn = uint32_t (*)();
    void* raw = ResolveOptionalSymbol(symbol);
    if (raw == nullptr) {
        return 0;
    }
    auto fn = reinterpret_cast<Fn>(raw);
    return fn != nullptr ? fn() : 0;
}

bool HasDynamicSymbol(const char* symbol) {
    return ResolveOptionalSymbol(symbol) != nullptr;
}

uint64_t GetHblNextLoadPathAddress() {
    return GetDynamicU64Symbol("NxemuGetLastHblNextLoadPathAddress");
}

uint64_t GetHblNextLoadArgvAddress() {
    return GetDynamicU64Symbol("NxemuGetLastHblNextLoadArgvAddress");
}

std::string ReadGuestCStringLocked(uint64_t address, uint64_t max_size = 0x1000) {
    if (!g_system_modules || !g_system_modules->IsValid() || address == 0 || max_size == 0) {
        return {};
    }
    std::vector<char> buffer(static_cast<size_t>(max_size), 0);
    if (!g_system_modules->Modules().OperatingSystem().ReadApplicationMemory(
            address, buffer.data(), static_cast<uint64_t>(buffer.size()))) {
        return {};
    }
    const auto end = std::find(buffer.begin(), buffer.end(), '\0');
    return std::string(buffer.begin(), end);
}

std::string ResolveGuestPathToHostLocked(const std::string& guest_path) {
    constexpr const char* SdmcPrefix = "sdmc:/";
    if (guest_path.rfind(SdmcPrefix, 0) == 0) {
        return g_files_dir + "/user/sdmc/" + guest_path.substr(std::strlen(SdmcPrefix));
    }
    return guest_path;
}

bool HostFileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return static_cast<bool>(file);
}

std::string BuildNextLoadStatusLocked() {
    const uint64_t path_addr = GetHblNextLoadPathAddress();
    const uint64_t argv_addr = GetHblNextLoadArgvAddress();
    const std::string guest_path = ReadGuestCStringLocked(path_addr);
    const std::string guest_argv = ReadGuestCStringLocked(argv_addr);
    const std::string host_path = guest_path.empty() ? std::string{} : ResolveGuestPathToHostLocked(guest_path);

    std::ostringstream out;
    out << "nextLoadPathAddr=0x" << std::hex << path_addr << "\n";
    out << "nextLoadArgvAddr=0x" << std::hex << argv_addr << "\n";
    out << std::dec;
    out << "nextLoadPending=" << (!guest_path.empty() ? "true" : "false") << "\n";
    out << "nextLoadGuestPath=" << guest_path << "\n";
    out << "nextLoadHostPath=" << host_path << "\n";
    out << "nextLoadHostExists=" << (!host_path.empty() && HostFileExists(host_path) ? "true" : "false") << "\n";
    out << "nextLoadArgvSize=" << guest_argv.size();
    return out.str();
}

std::string LaunchPathFreshLocked(const std::string& host_path, const char* reason) {
    std::ostringstream out;
    out << "launchFreshReason=" << (reason ? reason : "") << "\n";
    out << "launchFreshPath=" << host_path << "\n";
    out << ProbeGameFile(host_path) << "\n";
    if (!HostFileExists(host_path)) {
        out << "launchFresh=failed\nreason=host file missing";
        return out.str();
    }
    if (g_system_modules) {
        g_system_modules->ShutDown();
        g_system_modules.reset();
        out << "previousModules=shutdown\n";
    }
    g_last_boot_attempted = true;
    g_last_boot_loaded = false;
    g_last_boot_path = host_path;
    out << EnsureSystemModulesReady() << "\n";
    if (!g_system_modules || !g_system_modules->IsValid()) {
        out << "launchFresh=failed\nreason=SystemModules invalid";
        return out.str();
    }

    __android_log_print(ANDROID_LOG_INFO, LogTag, "launchFresh LoadRom begin: %s", host_path.c_str());
    const bool loaded = g_system_modules->Modules().Systemloader().LoadRom(host_path.c_str(), 0, -1, ApplicationLaunchType::FrontendInitiated);
    g_last_boot_loaded = loaded;
    out << "launchFresh=" << (loaded ? "LoadRom accepted" : "LoadRom failed");
    return out.str();
}

void AppendAndroidHidControllerState(std::ostringstream& out, const char* label, NpadIdType id);

std::string BuildRuntimeStatusLocked() {
    std::ostringstream out;
    out << "runtimeInitialized=" << (g_runtime_initialized ? "true" : "false") << "\n";
    out << "shutdownRequested=" << (g_shutdown_requested ? "true" : "false") << "\n";
    out << "surface=" << (g_native_window != nullptr ? "attached" : "none") << "\n";
    out << "modules=" << (g_system_modules && g_system_modules->IsValid() ? "valid" : "none_or_invalid") << "\n";
    out << "lastBootAttempted=" << (g_last_boot_attempted ? "true" : "false") << "\n";
    out << "lastBootLoaded=" << (g_last_boot_loaded ? "true" : "false") << "\n";
    out << "lastBootPath=" << g_last_boot_path << "\n";
    out << "filesDir=" << g_files_dir << "\n";
    out << "moduleDir=" << g_native_library_dir << "\n";
    out << "gpuDriver:\n" << BuildGpuDriverStatusLocked() << "\n";
    out << "performance:\n" << BuildPerformanceStatusLocked() << "\n";
    if (g_system_modules && g_system_modules->IsValid()) {
        auto& controller =
            g_system_modules->Modules().OperatingSystem().GetEmulatedController(NpadIdType::Player1);
        out << "hidPlayer1Connected=" << (controller.IsConnected() ? "true" : "false") << "\n";
        out << "hidPlayer1Style=" << static_cast<int>(controller.GetNpadStyleIndex()) << "\n";
        AppendAndroidHidControllerState(out, "hidPlayer1", NpadIdType::Player1);
        AppendAndroidHidControllerState(out, "hidHandheld", NpadIdType::Handheld);
        if (g_last_input_button >= 0 &&
            g_last_input_button < static_cast<int>(NativeButtonValues::NumButtons)) {
            button_status_t buttons[static_cast<size_t>(NativeButtonValues::NumButtons)]{};
            controller.GetButtonsStatus(buttons, static_cast<size_t>(NativeButtonValues::NumButtons));
            out << "lastInputPlayer=" << g_last_input_player << "\n";
            out << "lastInputButton=" << g_last_input_button << "\n";
            out << "lastInputPressed=" << (g_last_input_pressed ? "true" : "false") << "\n";
            out << "hidLastButtonValue="
                << (buttons[static_cast<size_t>(g_last_input_button)].value ? "true" : "false")
                << "\n";
        }
    }
    if (const char* hbl = GetLoaderHblDiagnostics(); hbl != nullptr) {
        out << "hblDiagnostics:\n" << hbl << "\n";
    }
    if (const char* startup = GetMainThreadStartupDiagnostics(); startup != nullptr) {
        out << "mainThreadStartupDiagnostics:\n" << startup << "\n";
    }
    if (const char* load_rom = GetLoadRomDiagnostics(); load_rom != nullptr) {
        out << "loadRomDiagnostics:\n" << load_rom << "\n";
    }
    if (const char* nce_loader = GetNceLoaderDiagnostics(); nce_loader != nullptr) {
        out << "nceLoaderDiagnostics:\n" << nce_loader << "\n";
    }
    if (const char* os_services = GetOsServiceDiagnostics(); os_services != nullptr) {
        out << "osServiceDiagnostics:\n" << os_services << "\n";
    }
    out << "nextLoadStatus:\n" << BuildNextLoadStatusLocked() << "\n";
    out << "lastStatus=" << g_last_status;
    return out.str();
}

constexpr uint32_t kAndroidPlayer1VirtualPort = 0;
constexpr uint32_t kAndroidHandheldVirtualPort = 8;

void EnsureAndroidControllerConnected(NpadIdType id, NpadStyleIndex style) {
    auto& controller = g_system_modules->Modules().OperatingSystem().GetEmulatedController(id);
    controller.SetNpadStyleIndex(style);
    if (!controller.IsConnected()) {
        controller.Connect();
    }
}

void AppendAndroidHidControllerState(std::ostringstream& out, const char* label, NpadIdType id) {
    auto& controller = g_system_modules->Modules().OperatingSystem().GetEmulatedController(id);
    const auto sticks = controller.GetSticksValues();
    out << label << "Connected=" << (controller.IsConnected() ? "true" : "false") << "\n";
    out << label << "Style=" << static_cast<int>(controller.GetNpadStyleIndex()) << "\n";
    out << label << "StickL=" << sticks.status[static_cast<size_t>(NativeAnalogValues::LStick)].x.value
        << "," << sticks.status[static_cast<size_t>(NativeAnalogValues::LStick)].y.value
        << " dirs="
        << (sticks.status[static_cast<size_t>(NativeAnalogValues::LStick)].left ? "L" : "")
        << (sticks.status[static_cast<size_t>(NativeAnalogValues::LStick)].right ? "R" : "")
        << (sticks.status[static_cast<size_t>(NativeAnalogValues::LStick)].up ? "U" : "")
        << (sticks.status[static_cast<size_t>(NativeAnalogValues::LStick)].down ? "D" : "") << "\n";
    out << label << "StickR=" << sticks.status[static_cast<size_t>(NativeAnalogValues::RStick)].x.value
        << "," << sticks.status[static_cast<size_t>(NativeAnalogValues::RStick)].y.value
        << " dirs="
        << (sticks.status[static_cast<size_t>(NativeAnalogValues::RStick)].left ? "L" : "")
        << (sticks.status[static_cast<size_t>(NativeAnalogValues::RStick)].right ? "R" : "")
        << (sticks.status[static_cast<size_t>(NativeAnalogValues::RStick)].up ? "U" : "")
        << (sticks.status[static_cast<size_t>(NativeAnalogValues::RStick)].down ? "D" : "") << "\n";
}

void ForceAndroidPlayer1InputLocked(std::ostringstream* out) {
    if (!g_system_modules || !g_system_modules->IsValid()) {
        if (out) {
            *out << "androidInput=skipped\nreason=modules not ready\n";
        }
        return;
    }

    auto& player1 =
        g_system_modules->Modules().OperatingSystem().GetEmulatedController(NpadIdType::Player1);
    auto& handheld =
        g_system_modules->Modules().OperatingSystem().GetEmulatedController(NpadIdType::Handheld);
    const bool player1_was_connected = player1.IsConnected();
    const auto player1_old_style = player1.GetNpadStyleIndex();
    const bool handheld_was_connected = handheld.IsConnected();
    const auto handheld_old_style = handheld.GetNpadStyleIndex();

    // Android phone/tablet runs often advertise handheld mode while some games still read
    // Player1/fullkey.  Mirror the touch frontend to both virtual ports:
    // - Player1 port 0: pro/fullkey path used by most yuzu/eden style overlays.
    // - Handheld port 8: required by titles that poll handheld npad while docked_mode=Handheld.
    EnsureAndroidControllerConnected(NpadIdType::Player1, NpadStyleIndex::Fullkey);
    EnsureAndroidControllerConnected(NpadIdType::Handheld, NpadStyleIndex::Handheld);

    if (out) {
        *out << "androidInput=forced-player1-and-handheld\n";
        *out << "hidPlayer1WasConnected=" << (player1_was_connected ? "true" : "false") << "\n";
        *out << "hidPlayer1OldStyle=" << static_cast<int>(player1_old_style) << "\n";
        *out << "hidPlayer1Connected=" << (player1.IsConnected() ? "true" : "false") << "\n";
        *out << "hidPlayer1Style=" << static_cast<int>(player1.GetNpadStyleIndex()) << "\n";
        *out << "hidHandheldWasConnected=" << (handheld_was_connected ? "true" : "false") << "\n";
        *out << "hidHandheldOldStyle=" << static_cast<int>(handheld_old_style) << "\n";
        *out << "hidHandheldConnected=" << (handheld.IsConnected() ? "true" : "false") << "\n";
        *out << "hidHandheldStyle=" << static_cast<int>(handheld.GetNpadStyleIndex()) << "\n";
    }
}

std::string EnsureSystemModulesReady() {
    const std::string runtime = EnsureRuntimeInitialized();
    if (!g_runtime_initialized) {
        return runtime;
    }

    if (!g_system_modules) {
        g_system_modules = std::make_unique<SystemModules>();
    }

    if (!g_system_modules->IsValid()) {
        g_system_modules->Setup(g_android_render_window);
        MarkLoadStage(g_system_modules->IsValid() ? "SystemModules setup valid" : "SystemModules setup invalid");
    }

    std::ostringstream out;
    out << runtime << "\n";
    out << "modules=" << (g_system_modules->IsValid() ? "valid" : "invalid");
    if (g_system_modules->IsValid()) {
        out << "\n";
        ForceAndroidPlayer1InputLocked(&out);
        // Remove trailing newline noise for prettier Java overlay.
        const auto text = out.str();
        if (!text.empty() && text.back() == '\n') {
            out.str(text.substr(0, text.size() - 1));
            out.clear();
        }
    }
    if (!g_android_notification.last_error.empty()) {
        out << "\nlast_error=" << g_android_notification.last_error;
    }
    return out.str();
}

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    SetJavaVM(vm);
    __android_log_print(ANDROID_LOG_INFO, LogTag, "JNI_OnLoad");
    return JniVersion;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_nxemu_app_NativeLibrary_initialize(JNIEnv* env, jclass, jclass native_library_class) {
    if (native_library_class == nullptr) {
        return JNI_FALSE;
    }

    ReplaceNativeLibraryClass(env, native_library_class);
    return GetJavaVM() != nullptr ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_version(JNIEnv* env, jclass) {
    const std::string version = "nxemu-android-poc";
    return env->NewStringUTF(version.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_initializeRuntime(JNIEnv* env, jclass, jstring files_dir,
                                                   jstring native_library_dir) {
    if (files_dir == nullptr || native_library_dir == nullptr) {
        return env->NewStringUTF("runtime=failed\nreason=null Java path");
    }

    const char* raw_files_dir = env->GetStringUTFChars(files_dir, nullptr);
    const char* raw_native_library_dir = env->GetStringUTFChars(native_library_dir, nullptr);
    if (raw_files_dir == nullptr || raw_native_library_dir == nullptr) {
        if (raw_files_dir != nullptr) {
            env->ReleaseStringUTFChars(files_dir, raw_files_dir);
        }
        if (raw_native_library_dir != nullptr) {
            env->ReleaseStringUTFChars(native_library_dir, raw_native_library_dir);
        }
        return env->NewStringUTF("runtime=failed\nreason=GetStringUTFChars failed");
    }

    std::lock_guard lk{g_runtime_mutex};
    g_files_dir = raw_files_dir;
    g_native_library_dir = raw_native_library_dir;
    g_app_dir = g_files_dir + "/user";
    g_shutdown_requested = false;
    MarkLoadStage("initializeRuntime begin");

    env->ReleaseStringUTFChars(files_dir, raw_files_dir);
    env->ReleaseStringUTFChars(native_library_dir, raw_native_library_dir);

    const std::string result = EnsureRuntimeInitialized();
    g_last_status = result;
    __android_log_print(ANDROID_LOG_INFO, LogTag, "initializeRuntime: %s", result.c_str());
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_initializeGpuDriver(JNIEnv* env, jclass, jstring hook_lib_dir,
                                                     jstring custom_driver_dir,
                                                     jstring custom_driver_name,
                                                     jstring file_redirect_dir) {
    const auto read = [env](jstring value) -> std::string {
        if (value == nullptr) {
            return {};
        }
        const char* raw = env->GetStringUTFChars(value, nullptr);
        if (raw == nullptr) {
            return {};
        }
        std::string result = raw;
        env->ReleaseStringUTFChars(value, raw);
        return result;
    };

    std::lock_guard lk{g_runtime_mutex};
    g_gpu_hook_lib_dir = read(hook_lib_dir);
    g_gpu_custom_driver_dir = read(custom_driver_dir);
    g_gpu_custom_driver_name = read(custom_driver_name);
    g_gpu_file_redirect_dir = read(file_redirect_dir);
    ApplyGpuDriverSettingsLocked();
    ApplyAndroidPerformanceSettingsLocked("gpu-driver-change");
    MarkLoadStage("gpu driver settings applied");

    std::ostringstream out;
    out << "initializeGpuDriver=ok\n" << BuildGpuDriverStatusLocked() << "\n" << BuildPerformanceStatusLocked();
    g_last_status = out.str();
    __android_log_print(ANDROID_LOG_INFO, LogTag, "initializeGpuDriver: %s", out.str().c_str());
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_nxemu_app_NativeLibrary_supportsCustomDriverLoading(JNIEnv*, jclass) {
#if defined(__ANDROID__) && (defined(_M_ARM64) || defined(ARCHITECTURE_arm64))
    return (android_get_device_api_level() >= 28 && access("/dev/kgsl-3d0", F_OK) == 0) ? JNI_TRUE : JNI_FALSE;
#else
    return JNI_FALSE;
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_setPerformanceProfile(JNIEnv* env, jclass, jint frame_skip,
                                                       jint resolution_setup, jint aspect_ratio,
                                                       jboolean graphics_compat, jboolean prefer_nce) {
    std::lock_guard lk{g_runtime_mutex};
    g_android_frame_skip = std::clamp(static_cast<int>(frame_skip), 0, 4);
    g_android_resolution_setup = std::clamp(static_cast<int>(resolution_setup), 0, 2);
    g_android_aspect_ratio = std::clamp(static_cast<int>(aspect_ratio), 0, 4);
    g_android_graphics_compat = (graphics_compat == JNI_TRUE);
    g_android_prefer_nce = (prefer_nce == JNI_TRUE);
    ApplyAndroidPerformanceSettingsLocked("java-performance-profile");
    std::ostringstream out;
    out << "setPerformanceProfile=ok\n";
    out << BuildPerformanceStatusLocked();
    g_last_status = out.str();
    __android_log_print(ANDROID_LOG_INFO, LogTag, "setPerformanceProfile: %s", out.str().c_str());
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_getPerformanceStats(JNIEnv* env, jclass) {
    std::ostringstream out;
    const bool has_composite_counter = HasDynamicSymbol("NxemuAndroidGetVulkanCompositeCount");
    const bool has_present_counter = HasDynamicSymbol("NxemuAndroidGetVulkanPresentCount");
    const bool has_fb_counter = HasDynamicSymbol("NxemuAndroidGetVulkanLastFbWidth");
    const uint64_t composite_count = GetDynamicU64Symbol("NxemuAndroidGetVulkanCompositeCount");
    const uint64_t present_count = GetDynamicU64Symbol("NxemuAndroidGetVulkanPresentCount");
    const uint32_t fb_width = GetDynamicU32Symbol("NxemuAndroidGetVulkanLastFbWidth");
    const uint32_t fb_height = GetDynamicU32Symbol("NxemuAndroidGetVulkanLastFbHeight");
    const uint32_t fb_stride = GetDynamicU32Symbol("NxemuAndroidGetVulkanLastFbStride");
    const uint32_t fb_format = GetDynamicU32Symbol("NxemuAndroidGetVulkanLastFbFormat");

    if (g_runtime_mutex.try_lock()) {
        std::unique_lock<std::mutex> lk{g_runtime_mutex, std::adopt_lock};
        out << BuildPerformanceStatusLocked() << "\n";
        out << "runtimePerfLock=acquired\n";
    } else {
        out << "runtimePerfLock=busy\n";
        out << "performanceProfile=busy\n";
        out << "frameSkip=" << g_android_frame_skip << "\n";
        out << "resolutionSetup=" << g_android_resolution_setup << "\n";
        out << "aspectRatio=" << g_android_aspect_ratio << "\n";
        out << "aspectRatioLabel=" << AspectRatioLabel(g_android_aspect_ratio) << "\n";
        out << "graphicsCompat=" << (g_android_graphics_compat ? "true" : "false") << "\n";
        out << "nceRequested=" << (g_android_prefer_nce ? "true" : "false") << "\n";
    }

    out << "vulkanCounterSymbols=present:" << (has_present_counter ? "1" : "0")
        << ",composite:" << (has_composite_counter ? "1" : "0")
        << ",fb:" << (has_fb_counter ? "1" : "0") << "\n";
    out << "vulkanCompositeCount=" << composite_count << "\n";
    out << "vulkanPresentCount=" << present_count << "\n";
    out << "vulkanLastFbWidth=" << fb_width << "\n";
    out << "vulkanLastFbHeight=" << fb_height << "\n";
    out << "vulkanLastFbStride=" << fb_stride << "\n";
    out << "vulkanLastFbFormat=" << fb_format << "\n";

    static uint64_t last_stats_present = 0;
    static uint64_t last_stats_composite = 0;
    static long long last_stats_ms = 0;
    static double last_safe_present_fps = 0.0;
    static double last_safe_composite_fps = 0.0;
    const long long now_ms = SteadyNowMs();
    if (last_stats_ms > 0 && now_ms > last_stats_ms) {
        const double seconds = static_cast<double>(now_ms - last_stats_ms) / 1000.0;
        if (present_count >= last_stats_present && seconds > 0.0) {
            last_safe_present_fps =
                static_cast<double>(present_count - last_stats_present) / seconds;
        }
        if (composite_count >= last_stats_composite && seconds > 0.0) {
            last_safe_composite_fps =
                static_cast<double>(composite_count - last_stats_composite) / seconds;
        }
    }
    last_stats_present = present_count;
    last_stats_composite = composite_count;
    last_stats_ms = now_ms;
    const double derived_fps = last_safe_present_fps > 0.0 ? last_safe_present_fps
                                                           : last_safe_composite_fps;
    // Do not assume every title targets 60 FPS. Some Switch games are 30 FPS; showing
    // 30/60 as 50% speed is misleading. This is an overlay/session heuristic until the
    // core exposes title timing directly.
    static double inferred_target_fps = 60.0;
    static int target30_samples = 0;
    static int target60_samples = 0;
    if (derived_fps >= 26.0 && derived_fps <= 34.5) {
        target30_samples++;
        target60_samples = 0;
    } else if (derived_fps >= 45.0) {
        target60_samples++;
        target30_samples = 0;
    }
    if (target30_samples >= 2) {
        inferred_target_fps = 30.0;
    } else if (target60_samples >= 2) {
        inferred_target_fps = 60.0;
    }
    const double derived_speed_percent =
        derived_fps > 0.0 ? (derived_fps / inferred_target_fps) * 100.0 : 0.0;

    // Android guard:
    // On the current PoC the Java sampler can race loader/emulation teardown or early boot. Tombstones
    // showed Core::PerfStats::GetAndResetStats() crashing in pthread_mutex_lock(NULL) from the
    // NxEmuPerfSample thread. Until the core exposes a lifecycle-safe perf API, never let diagnostics
    // kill the emulator process. Report conservative zero stats unless a game was accepted and shutdown
    // has not started.
    out << "performanceStats=android-safe-vulkan-counters\n";
    out << "reason=core perf stats disabled on Android; using Vulkan present/composite deltas\n";
    out << "derivedPresentFps=" << last_safe_present_fps << "\n";
    out << "derivedCompositeFps=" << last_safe_composite_fps << "\n";
    out << "targetGameFpsAuto=" << inferred_target_fps << "\n";
    out << "systemFps=" << derived_fps << "\n";
    out << "gameFps=" << derived_fps << "\n";
    out << "frametimeMs=" << (derived_fps > 0.0 ? 1000.0 / derived_fps : 0.0) << "\n";
    out << "speedPercent=" << derived_speed_percent;
    return env->NewStringUTF(out.str().c_str());

#if 0
    try {
        const PerfStatsResults stats =
            g_system_modules->Modules().OperatingSystem().GetAndResetPerfStats();
        out << "systemFps=" << stats.system_fps << "\n";
        out << "gameFps=" << stats.average_game_fps << "\n";
        out << "frametimeMs=" << (stats.frametime * 1000.0) << "\n";
        out << "speedPercent=" << (stats.emulation_speed * 100.0);
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "getPerformanceStats guarded exception: %s",
                            e.what());
        out << "performanceStats=failed\n";
        out << "reason=" << e.what() << "\n";
        out << "systemFps=0\n";
        out << "gameFps=0\n";
        out << "frametimeMs=0\n";
        out << "speedPercent=0";
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "getPerformanceStats guarded unknown exception");
        out << "performanceStats=failed\n";
        out << "reason=unknown exception\n";
        out << "systemFps=0\n";
        out << "gameFps=0\n";
        out << "frametimeMs=0\n";
        out << "speedPercent=0";
    }
    return env->NewStringUTF(out.str().c_str());
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_getLoadProgress(JNIEnv* env, jclass) {
    const std::string result = BuildLoadProgress();
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_probeGame(JNIEnv* env, jclass, jstring path) {
    if (path == nullptr) {
        return env->NewStringUTF("probe=failed\nreason=null path");
    }

    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    if (raw_path == nullptr) {
        return env->NewStringUTF("probe=failed\nreason=GetStringUTFChars failed");
    }

    ResetLoadProgress(std::string("probeGame begin: ") + raw_path);
    const std::string result = ProbeGameFile(raw_path);
    MarkLoadStage("probeGame end");
    __android_log_print(ANDROID_LOG_INFO, LogTag, "probeGame: %s", result.c_str());
    env->ReleaseStringUTFChars(path, raw_path);
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_setSurface(JNIEnv* env, jclass, jobject surface) {
    std::lock_guard lk{g_runtime_mutex};
    if (g_native_window != nullptr) {
        ANativeWindow_release(g_native_window);
        g_native_window = nullptr;
    }

    if (surface == nullptr) {
        g_last_status = "surface=cleared";
        return env->NewStringUTF("surface=cleared");
    }

    g_native_window = ANativeWindow_fromSurface(env, surface);
    if (g_native_window == nullptr) {
        g_last_status = "surface=failed";
        return env->NewStringUTF("surface=failed");
    }

    std::ostringstream out;
    out << "surface=ok\n";
    out << "width=" << ANativeWindow_getWidth(g_native_window) << "\n";
    out << "height=" << ANativeWindow_getHeight(g_native_window);
    g_last_status = out.str();
    MarkLoadStage("surface attached");
    __android_log_print(ANDROID_LOG_INFO, LogTag, "setSurface: %s", out.str().c_str());
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_bootGame(JNIEnv* env, jclass, jstring path) {
    if (path == nullptr) {
        return env->NewStringUTF("boot=failed\nreason=null path");
    }

    const char* raw_path = env->GetStringUTFChars(path, nullptr);
    if (raw_path == nullptr) {
        return env->NewStringUTF("boot=failed\nreason=GetStringUTFChars failed");
    }

    ResetLoadProgress(std::string("bootGame begin: ") + raw_path);
    std::lock_guard lk{g_runtime_mutex};
    g_last_boot_attempted = true;
    g_last_boot_loaded = false;
    g_last_boot_path = raw_path;

    std::ostringstream out;
    MarkLoadStage("boot probe begin");
    out << ProbeGameFile(raw_path) << "\n";
    MarkLoadStage("boot probe end");
    out << "surface=" << (g_native_window != nullptr ? "attached" : "missing") << "\n";
    if (g_native_window == nullptr) {
        MarkLoadStage("boot blocked: surface missing");
        out << "boot=failed\n";
        out << "reason=surface missing\n";
        out << "note=Surface was destroyed before renderer startup; wait for surface reattach and retry.";
        g_last_boot_loaded = false;
        g_last_status = out.str();
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "bootGame blocked: surface missing");
        env->ReleaseStringUTFChars(path, raw_path);
        return env->NewStringUTF(out.str().c_str());
    }
    MarkLoadStage("EnsureSystemModulesReady begin");
    out << EnsureSystemModulesReady() << "\n";
    MarkLoadStage("EnsureSystemModulesReady end");

    if (g_system_modules && g_system_modules->IsValid()) {
        ApplyAndroidPerformanceSettingsLocked("boot-game");
        out << "performance:\n" << BuildPerformanceStatusLocked() << "\n";
        __android_log_print(ANDROID_LOG_INFO, LogTag, "bootGame LoadRom begin: %s", raw_path);
        InstallAndroidFatalSignalDiagnostics();
        MarkLoadStage("LoadRom running");
        const bool loaded = g_system_modules->Modules().Systemloader().LoadRom(raw_path, 0, -1, ApplicationLaunchType::FrontendInitiated);
        InstallAndroidFatalSignalDiagnostics();
        RefreshNceEnabledLocked("after-loadrom");
        MarkLoadStage(loaded ? "LoadRom accepted" : "LoadRom failed");
        g_last_boot_loaded = loaded;
        out << "boot=" << (loaded ? "LoadRom accepted" : "LoadRom failed") << "\n";
        if (!g_android_notification.last_error.empty()) {
            out << "last_error=" << g_android_notification.last_error << "\n";
        }
        if (const char* load_rom = GetLoadRomDiagnostics(); load_rom != nullptr) {
            MarkLoadStage("LoadRom diagnostics collected");
            out << "loadRomDiagnostics:\n" << load_rom << "\n";
        }
        if (const char* nce_loader = GetNceLoaderDiagnostics(); nce_loader != nullptr) {
            out << "nceLoaderDiagnostics:\n" << nce_loader << "\n";
        }
        out << "performanceAfterLoad:\n" << BuildPerformanceStatusLocked() << "\n";
        out << "note=renderer/process may still be incomplete on Android PoC";
    } else {
        out << "boot=failed\nreason=SystemModules invalid";
    }

    g_last_status = out.str();
    MarkLoadStage("bootGame result ready");
    __android_log_print(ANDROID_LOG_INFO, LogTag, "bootGame result: %s", out.str().c_str());
    env->ReleaseStringUTFChars(path, raw_path);
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_shutdownRuntime(JNIEnv* env, jclass) {
    std::lock_guard lk{g_runtime_mutex};
    std::ostringstream out;
    out << "shutdown:";
    g_shutdown_requested = true;

    if (g_system_modules) {
        // 保持 Surface 到 modules 完全停止后再释放，避免 compositor/VI 仍引用已 abandoned 的 BufferQueue。
        g_system_modules->ShutDown();
        g_system_modules.reset();
        out << "\nmodules=shutdown";
    } else {
        out << "\nmodules=none";
    }

    if (g_native_window != nullptr) {
        ANativeWindow_release(g_native_window);
        g_native_window = nullptr;
        out << "\nsurface=released";
    } else {
        out << "\nsurface=none";
    }

    g_last_status = out.str();
    __android_log_print(ANDROID_LOG_INFO, LogTag, "shutdownRuntime: %s", out.str().c_str());
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_runtimeStatus(JNIEnv* env, jclass) {
    std::lock_guard lk{g_runtime_mutex};
    const std::string result = BuildRuntimeStatusLocked();
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_nextLoadStatus(JNIEnv* env, jclass) {
    std::lock_guard lk{g_runtime_mutex};
    const std::string result = BuildNextLoadStatusLocked();
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_launchPendingNextLoad(JNIEnv* env, jclass) {
    std::lock_guard lk{g_runtime_mutex};
    const uint64_t path_addr = GetHblNextLoadPathAddress();
    const std::string guest_path = ReadGuestCStringLocked(path_addr);
    std::ostringstream out;
    out << BuildNextLoadStatusLocked() << "\n";
    if (guest_path.empty()) {
        out << "launchPendingNextLoad=skipped\nreason=no pending next-load path";
        g_last_status = out.str();
        return env->NewStringUTF(out.str().c_str());
    }

    const std::string host_path = ResolveGuestPathToHostLocked(guest_path);
    out << LaunchPathFreshLocked(host_path, "hbl-next-load");
    g_last_status = out.str();
    __android_log_print(ANDROID_LOG_INFO, LogTag, "launchPendingNextLoad: %s", out.str().c_str());
    return env->NewStringUTF(out.str().c_str());
}


extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_setPlayerButton(JNIEnv* env, jclass, jint player_index,
                                                 jint button_ordinal, jboolean pressed) {
    std::lock_guard lk{g_runtime_mutex};
    if (!g_system_modules || !g_system_modules->IsValid()) {
        return env->NewStringUTF("input=failed\nreason=modules not ready");
    }
    if (player_index < 0 || button_ordinal < 0) {
        return env->NewStringUTF("input=failed\nreason=invalid argument");
    }

    auto& operating_system = g_system_modules->Modules().OperatingSystem();
    const uint32_t requested_player = static_cast<uint32_t>(player_index);
    const uint32_t button = static_cast<uint32_t>(button_ordinal);
    const bool is_pressed = pressed == JNI_TRUE;
    operating_system.SetPlayerButtonState(requested_player, button, is_pressed);
    if (requested_player == kAndroidPlayer1VirtualPort) {
        operating_system.SetPlayerButtonState(kAndroidHandheldVirtualPort, button, is_pressed);
    }
    g_last_input_player = static_cast<int>(player_index);
    g_last_input_button = static_cast<int>(button_ordinal);
    g_last_input_pressed = is_pressed;

    EnsureAndroidControllerConnected(NpadIdType::Player1, NpadStyleIndex::Fullkey);
    EnsureAndroidControllerConnected(NpadIdType::Handheld, NpadStyleIndex::Handheld);
    auto& controller = operating_system.GetEmulatedController(NpadIdType::Player1);
    auto& handheld = operating_system.GetEmulatedController(NpadIdType::Handheld);

    std::ostringstream out;
    out << "input=ok\nplayer=" << player_index << "\nbutton=" << button_ordinal
        << "\npressed=" << (is_pressed ? "true" : "false")
        << "\nmirroredHandheld=" << (requested_player == kAndroidPlayer1VirtualPort ? "true" : "false")
        << "\nhidPlayer1Connected=" << (controller.IsConnected() ? "true" : "false")
        << "\nhidPlayer1Style=" << static_cast<int>(controller.GetNpadStyleIndex())
        << "\nhidHandheldConnected=" << (handheld.IsConnected() ? "true" : "false")
        << "\nhidHandheldStyle=" << static_cast<int>(handheld.GetNpadStyleIndex());

    if (button_ordinal >= 0 &&
        button_ordinal < static_cast<jint>(NativeButtonValues::NumButtons)) {
        button_status_t buttons[static_cast<size_t>(NativeButtonValues::NumButtons)]{};
        controller.GetButtonsStatus(buttons, static_cast<size_t>(NativeButtonValues::NumButtons));
        button_status_t handheld_buttons[static_cast<size_t>(NativeButtonValues::NumButtons)]{};
        handheld.GetButtonsStatus(handheld_buttons, static_cast<size_t>(NativeButtonValues::NumButtons));
        out << "\nhidButtonValue="
            << (buttons[static_cast<size_t>(button_ordinal)].value ? "true" : "false")
            << "\nhidHandheldButtonValue="
            << (handheld_buttons[static_cast<size_t>(button_ordinal)].value ? "true" : "false");
    } else {
        out << "\nhidButtonValue=out_of_range";
    }
    g_last_status = out.str();
    // Keep the latest input result available through runtimeStatus/session diagnostics,
    // but do not mirror every touch/ADB button into logcat. Automated probes can generate
    // hundreds of button events and logcat I/O becomes the dominant noise source.
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_setPlayerAnalog(JNIEnv* env, jclass, jint player_index,
                                                 jint stick_index, jfloat x, jfloat y) {
    std::lock_guard lk{g_runtime_mutex};
    if (!g_system_modules || !g_system_modules->IsValid()) {
        return env->NewStringUTF("analog=failed\nreason=modules not ready");
    }
    if (player_index < 0 || stick_index < 0 ||
        stick_index >= static_cast<jint>(NativeAnalogValues::NumAnalogs)) {
        return env->NewStringUTF("analog=failed\nreason=invalid argument");
    }

    const float clamped_x = std::clamp(static_cast<float>(x), -1.0f, 1.0f);
    const float clamped_y = std::clamp(static_cast<float>(y), -1.0f, 1.0f);
    auto& operating_system = g_system_modules->Modules().OperatingSystem();
    const uint32_t requested_player = static_cast<uint32_t>(player_index);
    const uint32_t stick = static_cast<uint32_t>(stick_index);
    operating_system.SetPlayerAnalogState(requested_player, stick, clamped_x, clamped_y);
    if (requested_player == kAndroidPlayer1VirtualPort) {
        operating_system.SetPlayerAnalogState(kAndroidHandheldVirtualPort, stick, clamped_x,
                                              clamped_y);
    }
    g_last_analog_player = static_cast<int>(player_index);
    g_last_analog_stick = static_cast<int>(stick_index);
    g_last_analog_x = clamped_x;
    g_last_analog_y = clamped_y;

    EnsureAndroidControllerConnected(NpadIdType::Player1, NpadStyleIndex::Fullkey);
    EnsureAndroidControllerConnected(NpadIdType::Handheld, NpadStyleIndex::Handheld);
    auto& controller = operating_system.GetEmulatedController(NpadIdType::Player1);
    auto& handheld = operating_system.GetEmulatedController(NpadIdType::Handheld);
    const auto player1_sticks = controller.GetSticksValues();
    const auto handheld_sticks = handheld.GetSticksValues();
    const auto left_stick = static_cast<size_t>(NativeAnalogValues::LStick);
    const auto right_stick = static_cast<size_t>(NativeAnalogValues::RStick);

    std::ostringstream out;
    out << "analog=ok\nplayer=" << player_index << "\nstick=" << stick_index
        << "\nx=" << clamped_x << "\ny=" << clamped_y
        << "\nmirroredHandheld=" << (requested_player == kAndroidPlayer1VirtualPort ? "true" : "false")
        << "\nhidPlayer1Connected="
        << (controller.IsConnected() ? "true" : "false") << "\nhidPlayer1Style="
        << static_cast<int>(controller.GetNpadStyleIndex())
        << "\nhidHandheldConnected=" << (handheld.IsConnected() ? "true" : "false")
        << "\nhidHandheldStyle=" << static_cast<int>(handheld.GetNpadStyleIndex())
        << "\nhidPlayer1StickL=" << player1_sticks.status[left_stick].x.value << ","
        << player1_sticks.status[left_stick].y.value
        << "\nhidPlayer1StickR=" << player1_sticks.status[right_stick].x.value << ","
        << player1_sticks.status[right_stick].y.value
        << "\nhidHandheldStickL=" << handheld_sticks.status[left_stick].x.value << ","
        << handheld_sticks.status[left_stick].y.value
        << "\nhidHandheldStickR=" << handheld_sticks.status[right_stick].x.value << ","
        << handheld_sticks.status[right_stick].y.value;
    g_last_status = out.str();
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_app_NativeLibrary_requestGuestCpuSample(JNIEnv* env, jclass) {
    // Do not leave full HLE tracing enabled after a stall sample. Permanent full trace makes
    // hot SVC/VI/NVDRV paths flood logcat and can itself cause severe slowdowns on Android.
    // OS.Thread/CPU.Core samples are recorded as non-hot diagnostics, so they remain available
    // in the session log without enabling full trace globally.
    Service::NxemuAndroidDiagnostics::SetFullTrace(false);
    const std::string result = RequestGuestCpuSampleDynamic();
    std::string output = result + "\nandroidFullTrace=disabled";
    __android_log_print(ANDROID_LOG_INFO, LogTag, "requestGuestCpuSample: %s", output.c_str());
    return env->NewStringUTF(output.c_str());
}




