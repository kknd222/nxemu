#include "ui_application.h"
#include "settings/ui_settings.h"
#include "ui/emulation/emulator_session.h"
#include "ui/host/ui_dialog_window.h"
#include "ui/host/ui_notification.h"
#include "ui/host/ui_pack.h"
#include "ui/host/ui_window.h"
#include "ui/i18n/translation.h"
#include "ui/library/game_library.h"
#include "ui/library/interface_language.h"
#include "ui/views/main_window.h"
#include "ui/emulation/debug_bridge.h"
#include "ui/views/message_dialog.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <iostream>
#include <cstdlib>
#include <nxemu-core/app_init.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <string>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/logging/log.h>

namespace
{
constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 720;
constexpr uint64_t kStillAfterMs = 250;
constexpr int kStillFrameMs = 100;

bool KeepGraphicsBusy(const UiWindow & window, EmulatorSession & session)
{
#if defined(__APPLE__)
    return session.IsRunning() && !session.IsPaused() && window.IsFullscreen() &&
           !SettingsStore::GetInstance().GetBool(NXOsSetting::UseSpeedLimit);
#else
    (void)window;
    (void)session;
    return false;
#endif
}

std::filesystem::path FindAssetDirectory()
{
    const std::filesystem::path exeDir = Common::FS::GetExeDirectory();
    for (const std::filesystem::path & packDir : {exeDir, exeDir.parent_path() / "Resources"})
    {
        std::error_code packError;
        if (std::filesystem::is_regular_file(packDir / "nxemu.lang", packError) &&
            UiPack::Open(packDir / "nxemu.lang", packDir / "ui"))
        {
            return packDir / "ui";
        }
    }
    const std::filesystem::path candidates[] = {
        exeDir / "ui",
        // macOS application bundle: Contents/MacOS/../Resources/ui
        exeDir.parent_path() / "Resources" / "ui",
        exeDir.parent_path().parent_path().parent_path() / "src" / "nxemu" / "ui" / "assets",
    };

    for (const std::filesystem::path & dir : candidates)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(dir / "main_window.rml", error))
        {
            return dir;
        }
    }
    return {};
}

struct LiveResize
{
    UiWindow & window;
    MainWindow & mainWindow;
    SDL_ThreadID thread = SDL_GetCurrentThreadID();
    bool busy = false;
};

bool SDLCALL DrawWhileResizing(void * userdata, SDL_Event * event)
{
    LiveResize & resize = *static_cast<LiveResize *>(userdata);
    UiDialogWindow * dialogWindow = resize.window.DialogWindow();
    if (SDL_GetCurrentThreadID() != resize.thread || resize.busy || event->type != SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        (event->window.windowID != SDL_GetWindowID(resize.window.Handle()) && (dialogWindow == nullptr || !dialogWindow->Owns(*event))))
    {
        return true;
    }
    resize.busy = true;
    resize.mainWindow.HandleEvent(*event);
    resize.mainWindow.Update();
    resize.window.PaceFrame();
    resize.window.Render(resize.mainWindow.GameArea());
    resize.busy = false;
    return true;
}

int Run(UiNotification & notification, const std::string & gamePath, const std::string & firmwarePath, const std::string & debugPath)
{
    const std::filesystem::path assetDir = FindAssetDirectory();
    if (assetDir.empty())
    {
        std::cerr << "NXEmu: the interface files are missing\n";
        return 1;
    }

    UiWindow window;
    const std::string title = std::string("NXEmu ") + VER_FILE_VERSION_STR;
    if (!window.Create(title.c_str(), kInitialWidth, kInitialHeight, assetDir, SettingsStore::GetInstance().GetInt(NXVideoSetting::VulkanDevice)))
    {
        std::cerr << "NXEmu: " << window.Error() << '\n';
        notification.DisplayError(window.Error().c_str(), "NXEmu");
        return 1;
    }
    LOG_INFO(Frontend, "Startup: window created");
    window.SetTheme(uiSettings.interfaceTheme);
    InterfaceLanguage::AdoptSystemLanguage();
    InterfaceLanguage::Use(InterfaceLanguage::Code(), assetDir);

    Rml::ElementDocument * startup = Translation::LoadDocument(*window.Context(), assetDir / "startup.rml");
    if (startup != nullptr)
    {
        startup->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        window.Context()->Update();
        window.Render(nullptr);
        LOG_INFO(Frontend, "Startup: startup screen shown");
    }

    EmulatorSession session(window, notification, window.Handle());
    if (!session.Initialise())
    {
        notification.DisplayError("Required modules failed to load.", "NXEmu");
        return 1;
    }
    LOG_INFO(Frontend, "Startup: modules ready");
    if (startup != nullptr)
    {
        startup->Close();
    }
    const auto closesMainWindow = [&window](const SDL_Event & event) {
        return event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window.Handle());
    };
    GameLibrary library;
    MainWindow mainWindow(window, session, library, assetDir);
    if (!mainWindow.Create())
    {
        std::cerr << "NXEmu: unable to load the main window document\n";
        return 1;
    }
    LOG_INFO(Frontend, "Startup: main window created");

    LiveResize liveResize{window, mainWindow};
    notification.SetPresenter([&](bool question, const std::string & title, const std::string & text) {
        const bool wasBusy = liveResize.busy;
        liveResize.busy = true;
        mainWindow.RevealMouseCursor();
        // The main window takes no input while the dialog window is open.
        MessageDialog dialog(window.FrontContext(), assetDir, question, title, text);
        UiDialogWindow * dialogWindow = window.DialogWindow();
        dialog.Show();
        SDL_Event deferred{};
        bool quitting = false;
        while (dialog.IsVisible())
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT || closesMainWindow(event))
                {
                    deferred = event;
                    quitting = true;
                    dialog.Dismiss();
                    break;
                }
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                {
                    dialog.Dismiss();
                    continue;
                }
                window.HandleEvent(event);
            }
            dialog.Update();
            window.SyncNativeState();
            window.Context()->Update();
            if (dialogWindow != nullptr)
            {
                dialogWindow->Update();
            }
            window.PaceFrame();
            if (!window.Render(mainWindow.GameArea()) && !question)
            {
                // The dialog cannot be seen, such as after the GPU stopped responding.
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), window.Handle());
                dialog.Dismiss();
            }
        }
        mainWindow.RevealMouseCursor();
        liveResize.busy = wasBusy;
        if (quitting)
        {
            SDL_PushEvent(&deferred);
        }
        return dialog.Response();
    });
    SDL_AddEventWatch(&DrawWhileResizing, &liveResize);

    if (!firmwarePath.empty())
    {
        mainWindow.OpenFirmwareSource(firmwarePath);
    }
    else if (!gamePath.empty())
    {
        mainWindow.OpenGame(gamePath);
    }

    DebugBridge debugBridge(debugPath);
    debugBridge.Tick(session, mainWindow);
    uint64_t lastChange = SDL_GetTicks();
    uint64_t lastRender = 0;
    while (!mainWindow.QuitRequested())
    {
        // Input polling keeps its normal cadence while a game runs, even
        // when the interface image can be reused for several game frames.
        window.PaceFrame();
        SDL_Event event;
        bool changed = false;
        bool pending = !session.IsRunning() && SDL_GetTicks() - lastChange >= kStillAfterMs ? SDL_WaitEventTimeout(&event, kStillFrameMs) != 0 : SDL_PollEvent(&event) != 0;
        for (; pending; pending = SDL_PollEvent(&event) != 0)
        {
            liveResize.busy = true;
            changed = true;
            if (event.type == SDL_EVENT_QUIT || closesMainWindow(event))
            {
                mainWindow.RequestQuit();
            }
            else
            {
                mainWindow.HandleEvent(event);
            }
            liveResize.busy = false;
        }
        liveResize.busy = true;
        window.SyncNativeState();
        mainWindow.Update();
        debugBridge.Tick(session, mainWindow);
        const uint64_t now = SDL_GetTicks();
        if (changed || !mainWindow.Still())
        {
            lastChange = now;
        }
        if (now - lastChange < kStillAfterMs || now - lastRender >= kStillFrameMs || KeepGraphicsBusy(window, session))
        {
            if (window.Render(mainWindow.GameArea()) && lastRender == 0)
            {
                LOG_INFO(Frontend, "Startup: first frame shown");
            }
            lastRender = SDL_GetTicks();
        }
        liveResize.busy = false;
    }
    debugBridge.Tick(session, mainWindow, true);
    SDL_RemoveEventWatch(&DrawWhileResizing, &liveResize);
    notification.SetPresenter({});
    return 0;
}
} // namespace

bool StartupChecks(const char * arg0, bool * has_broken_vulkan, bool perform_vulkan_check);

bool probeGraphics(const char * executable)
{
    bool brokenVulkan = false;
    StartupChecks(executable, &brokenVulkan, true);
    return !brokenVulkan;
}

int RunUiApplication(int argc, char ** argv)
{
    UiNotification notification;
    SetNotification(&notification);

    std::string gamePath;
    std::string firmwarePath;
    std::string debugPath;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        const char * environment = nullptr;
        if (argument == "--nxshader-import") environment = "NXEMU_IMPORT_NXSHADER";
        else if (argument == "--nxshader-game-root") environment = "NXEMU_SHADER_GAME_ROOT";
        else if (argument == "--nxshader-record") environment = "NXEMU_NXSHADER_RECORD";
        else if (argument == "--native-sparse-readback") environment = "NXEMU_NATIVE_SPARSE_READBACK";
        else if (argument == "--native-dirty-readback") environment = "NXEMU_NATIVE_DIRTY_READBACK";
        else if (argument == "--buffer-readback-segments") environment = "NXEMU_BUFFER_READBACK_SEGMENTS";
        else if (argument == "--software-keyboard-name") environment = "NXEMU_SOFTWARE_KEYBOARD_NAME";
        else if (argument == "--nxshader-timing") environment = "NXEMU_NXSHADER_TIMING";
        else if (argument == "--nxshader-timing-file") environment = "NXEMU_NXSHADER_TIMING_FILE";
        else if (argument == "--nxshader-runtime-report") environment = "NXEMU_NXSHADER_RUNTIME_REPORT";
        else if (argument == "--debug-detail") environment = "NXEMU_DEBUG_DETAIL";
        else if (argument == "--handle-trace") environment = "NXEMU_HANDLE_TRACE";
        else if (argument == "--shader-rejection-trace") environment = "NXEMU_SHADER_REJECTION_TRACE";
        else if (argument == "--shader-fixtures") environment = "NXEMU_NATIVE_SHADER_FIXTURES";
        else if (argument == "--frame-capture") environment = "NXEMU_FRAME_CAPTURE";
        else if (argument == "--paired-pixels") environment = "NXEMU_PAIRED_PIXELS";
        else if (argument == "--image-writers") environment = "NXEMU_IMAGE_WRITERS";
        else if (argument == "--native-draw-inputs") environment = "NXEMU_NATIVE_DRAW_INPUTS";
        else if (argument == "--native-compute-inputs") environment = "NXEMU_NATIVE_COMPUTE_INPUTS";
        else if (argument == "--native-compute-input-hash") environment = "NXEMU_NATIVE_COMPUTE_INPUT_HASH";
        else if (argument == "--shader-progress") environment = "NXEMU_SHADER_PROGRESS";
        else if (argument == "--ssa-fixtures") environment = "NXEMU_SSA_FIXTURES";
        else if (argument == "--pipeline-fixtures") environment = "NXEMU_PIPELINE_FIXTURES";
        else if (argument == "--pipeline-fixture-key") environment = "NXEMU_PIPELINE_FIXTURE_KEY";
        if (environment)
        {
            if (++i >= argc)
            {
                notification.DisplayError("Missing value for NXShader argument.", "NXEmu");
                SetNotification(nullptr);
                return 1;
            }
            if ((argument == "--native-sparse-readback" || argument == "--native-dirty-readback" || argument == "--buffer-readback-segments" ||
                 argument == "--debug-detail" || argument == "--handle-trace" ||
                 argument == "--shader-rejection-trace" || argument == "--shader-fixtures" || argument == "--frame-capture" || argument == "--paired-pixels" || argument == "--image-writers" || argument == "--native-draw-inputs" ||
                  argument == "--shader-progress" || argument == "--pipeline-fixtures" || argument == "--ssa-fixtures" || argument == "--native-compute-inputs") &&
                std::string(argv[i]) != "0" && std::string(argv[i]) != "1") {
                notification.DisplayError("Native readback option expects 0 or 1.", "NXEmu");
                SetNotification(nullptr);
                return 1;
            }
              if(argument=="--pipeline-fixture-key" || argument=="--native-compute-input-hash") {
                  const std::string key=argv[i];
                  if(key.size()!=16 || key.find_first_not_of("0123456789abcdef")!=std::string::npos) {
                      notification.DisplayError("Pipeline key expects 16 lowercase hex digits.","NXEmu");
                      SetNotification(nullptr);return 1;
                  }
              }
#ifdef _WIN32
            _putenv_s(environment, argv[i]);
#else
            setenv(environment, argv[i], 1);
#endif
        }
        else if (argument == "--debug-bridge") {
            if (++i >= argc) { SetNotification(nullptr); return 1; }
            debugPath = argv[i];
        }
        else if (argument == "--load" || argument == "-l" || argument == "--install-firmware")
        {
            if (++i >= argc)
            {
                notification.DisplayError("Missing game path for --load.", "NXEmu");
                SetNotification(nullptr);
                return 1;
            }
            if (argument == "--install-firmware") firmwarePath = argv[i];
            else gamePath = argv[i];
        }
        else if (!argument.empty() && argument[0] != '-' && gamePath.empty()) gamePath = argument;
        else
        {
            notification.DisplayError("Unknown command-line argument.", "NXEmu");
            SetNotification(nullptr);
            return 1;
        }
    }

    int result = 1;
    LOG_INFO(Frontend, "Startup: checking Vulkan");
    if (argc > 0 && uiSettings.performVulkanCheck && !probeGraphics(argv[0]))
    {
        notification.DisplayError("Vulkan could not be initialised. Update the graphics driver and try again.", "NXEmu");
    }
    else
    {
        LOG_INFO(Frontend, "Startup: Vulkan checked");
        result = Run(notification, gamePath, firmwarePath, debugPath);
    }
    SetNotification(nullptr);
    return result;
}
