#include "system_modules.h"
#include "cpu_module.h"
#include "loader_module.h"
#include "module_notification.h"
#include "module_settings.h"
#include "operating_system_module.h"
#include "video_module.h"
#include "notification.h"
#include "settings/core_settings.h"
#include <vector>
#include <sstream>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/settings/identifiers.h>

namespace
{
typedef std::vector<ModuleBase *> BaseModules;

template <typename plugin_type>
std::string LoadModule(const std::string & fileName, std::unique_ptr<plugin_type> & plugin, ModuleNotification * moduleNotification, ModuleSettings * moduleSettings)
{
    Path fullPath((const char *)coreSettings.moduleDir, fileName.c_str());
    plugin = std::make_unique<plugin_type>();
    if (plugin.get() == nullptr)
    {
        return "allocate=failed";
    }
    if (!fullPath.FileExists())
    {
        const std::string result = std::string("path=") + static_cast<const char *>(fullPath) + "\nexists=false";
        plugin = nullptr;
        return result;
    }
    if (!plugin->Load(fullPath, moduleNotification, moduleSettings))
    {
        const std::string result = plugin->LastLoadDiagnostic();
        plugin = nullptr;
        return result;
    }
    return plugin->LastLoadDiagnostic();
}
} // namespace

struct SystemModules::Impl :
    public ISystemModules
{
    explicit Impl(IRenderWindow & window_) :
        window(window_),
        systemLoader(nullptr),
        video(nullptr),
        cpu(nullptr),
        operatingsystem(nullptr),
        valid(false),
        stopping(false)
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.RegisterCallback(NXCoreSetting::EmulationRunning, EmulationRunningChanged, this);
    }

    ~Impl()
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.UnregisterCallback(NXCoreSetting::EmulationRunning, EmulationRunningChanged, this);    
    }
    
    static void EmulationRunningChanged(const char * /*setting*/, void * userData)
    {
        SystemModules::Impl & this_ = *((SystemModules::Impl *)userData);
        SettingsStore & settings = SettingsStore::GetInstance();
        bool emulationRunning = settings.GetBool(NXCoreSetting::EmulationRunning);
        if (emulationRunning)
        {
            this_.StartEmulation();
        }
        else
        {
            this_.StopEmulation(false);
        }
    }

    void StartEmulation() override
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Starting);

        for (BaseModules::iterator itr = baseModules.begin(); itr != baseModules.end(); itr++)
        {
            (*itr)->EmulationStarting();
        }
    }

    void StopEmulation(bool wait) override
    {
        if (stopping)
        {
            return;
        }
        stopping = true;

        SettingsStore & settings = SettingsStore::GetInstance();
        settings.SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopping);

        for (BaseModules::iterator itr = baseModules.begin(); itr != baseModules.end(); itr++)
        {
            (*itr)->EmulationStopping(wait);
        }
        if (settings.GetBool(NXCoreSetting::EmulationRunning))
        {
            settings.SetBool(NXCoreSetting::EmulationRunning, false);
        }

        stopping = false;
    }

    ISystemloader & Systemloader() override
    {
        return *systemLoader;
    }

    IOperatingSystem & OperatingSystem() override
    {
        return *operatingsystem;
    }

    IVideo & Video() override
    {
        return *video;
    }

    ICpu & Cpu() override
    {
        return *cpu;
    }

    IRenderWindow & window;
    BaseModules baseModules;
    ModuleNotification moduleNotification;
    ModuleSettings moduleSettings;
    std::string loaderFile;
    std::string cpuFile;
    std::string videoFile;
    std::string operatingsystemFile;
    std::unique_ptr<LoaderModule> loaderModule;
    std::unique_ptr<CpuModule> cpuModule;
    std::unique_ptr<VideoModule> videoModule;
    std::unique_ptr<OperatingSystemModule> operatingsystemModule;
    ISystemloader * systemLoader;
    IVideo * video;
    ICpu * cpu;
    IOperatingSystem * operatingsystem;
    bool valid;
    bool stopping;
    std::string lastSetupDiagnostic;
};

SystemModules::SystemModules()
{
}

SystemModules::~SystemModules()
{
    ShutDown();
}

void SystemModules::Setup(IRenderWindow & window)
{
    ShutDown();
    impl = std::make_unique<Impl>(window);
    impl->valid = false;
    std::ostringstream diag;
    diag << "SystemModulesSetup=begin\n";

    impl->loaderFile = coreSettings.moduleLoader;
    impl->cpuFile = coreSettings.moduleCpu;
    impl->videoFile = coreSettings.moduleVideo;
    impl->operatingsystemFile = coreSettings.moduleOs;
    diag << "moduleDir=" << (const char *)coreSettings.moduleDir << "\n";
    diag << "loaderFile=" << impl->loaderFile << "\n";
    diag << "cpuFile=" << impl->cpuFile << "\n";
    diag << "videoFile=" << impl->videoFile << "\n";
    diag << "osFile=" << impl->operatingsystemFile << "\n";

    const std::string loaderDiag = LoadModule(impl->loaderFile, impl->loaderModule, &impl->moduleNotification, &impl->moduleSettings);
    diag << "loaderModule=" << (impl->loaderModule ? "ok" : "failed") << "\n";
    diag << loaderDiag << "\n";
    const std::string cpuDiag = LoadModule(impl->cpuFile, impl->cpuModule, &impl->moduleNotification, &impl->moduleSettings);
    diag << "cpuModule=" << (impl->cpuModule ? "ok" : "failed") << "\n";
    diag << cpuDiag << "\n";
    const std::string videoDiag = LoadModule(impl->videoFile, impl->videoModule, &impl->moduleNotification, &impl->moduleSettings);
    diag << "videoModule=" << (impl->videoModule ? "ok" : "failed") << "\n";
    diag << videoDiag << "\n";
    const std::string osDiag = LoadModule(impl->operatingsystemFile, impl->operatingsystemModule, &impl->moduleNotification, &impl->moduleSettings);
    diag << "osModule=" << (impl->operatingsystemModule ? "ok" : "failed") << "\n";
    diag << osDiag << "\n";

    if (impl->loaderModule.get() == nullptr || 
        impl->cpuModule.get() == nullptr || 
        impl->videoModule.get() == nullptr || 
        impl->operatingsystemModule.get() == nullptr)
    {
        diag << "SystemModulesSetup=failed\nreason=module-load";
        impl->lastSetupDiagnostic = diag.str();
        return;
    }
    impl->systemLoader = impl->loaderModule->CreateSystemLoader(*impl);
    if (impl->systemLoader == nullptr)
    {
        diag << "CreateSystemLoader=failed\nSystemModulesSetup=failed";
        impl->lastSetupDiagnostic = diag.str();
        return;
    }
    diag << "CreateSystemLoader=ok\n";
    impl->cpu = impl->cpuModule->CreateCpu(*impl);
    if (impl->cpu == nullptr)
    {
        diag << "CreateCpu=failed\nSystemModulesSetup=failed";
        impl->lastSetupDiagnostic = diag.str();
        return;
    }
    diag << "CreateCpu=ok\n";
    impl->operatingsystem = impl->operatingsystemModule->CreateOS(*impl);
    if (impl->operatingsystem == nullptr)
    {
        diag << "CreateOS=failed\nSystemModulesSetup=failed";
        impl->lastSetupDiagnostic = diag.str();
        return;
    }
    diag << "CreateOS=ok\n";
    impl->video = impl->videoModule->CreateVideo(impl->window, *impl);
    if (impl->video == nullptr)
    {
        diag << "CreateVideo=failed\nSystemModulesSetup=failed";
        impl->lastSetupDiagnostic = diag.str();
        return;
    }
    diag << "CreateVideo=ok\n";

    if (!impl->systemLoader->Initialize())
    {
        diag << "SystemLoaderInitialize=failed\nSystemModulesSetup=failed";
        impl->lastSetupDiagnostic = diag.str();
        return;
    }
    diag << "SystemLoaderInitialize=ok\n";
    if (!impl->cpu->Initialize())
    {
        diag << "CpuInitialize=failed\nSystemModulesSetup=failed";
        impl->lastSetupDiagnostic = diag.str();
        return;
    }
    diag << "CpuInitialize=ok\n";
    if (!impl->video->Initialize())
    {
        diag << "VideoInitialize=failed\nSystemModulesSetup=failed";
        impl->lastSetupDiagnostic = diag.str();
        return;
    }
    diag << "VideoInitialize=ok\n";
    if (!impl->operatingsystem->Initialize())
    {
        diag << "OSInitialize=failed\nSystemModulesSetup=failed";
        impl->lastSetupDiagnostic = diag.str();
        return;
    }
    diag << "OSInitialize=ok\n";

    impl->baseModules.push_back(impl->cpuModule.get());
    impl->baseModules.push_back(impl->videoModule.get());
    impl->baseModules.push_back(impl->operatingsystemModule.get());
    impl->baseModules.push_back(impl->loaderModule.get());
    impl->valid = true;
    diag << "SystemModulesSetup=ok";
    impl->lastSetupDiagnostic = diag.str();
}

void SystemModules::ShutDown()
{
    if (impl == nullptr)
    {
        return;
    }
    impl->StopEmulation(true);
    impl->baseModules.clear();
    if (impl->cpu != nullptr && impl->cpuModule.get() != nullptr)
    {
        impl->cpuModule->DestroyCpu(impl->cpu);
        impl->cpu = nullptr;
    }
    if (impl->video != nullptr && impl->videoModule.get() != nullptr)
    {
        impl->videoModule->DestroyVideo(impl->video);
        impl->video = nullptr;
    }
    if (impl->operatingsystem != nullptr && impl->operatingsystemModule.get() != nullptr)
    {
        impl->operatingsystemModule->DestroyOS(impl->operatingsystem);
        impl->operatingsystem = nullptr;
    }
    if (impl->systemLoader != nullptr && impl->loaderModule.get() != nullptr)
    {
        impl->loaderModule->DestroySystemLoader(impl->systemLoader);
        impl->systemLoader = nullptr;
    }
    for (BaseModules::iterator itr = impl->baseModules.begin(); itr != impl->baseModules.end(); itr++)
    {
        (*itr)->ModuleCleanup();
    }
    impl = nullptr;
}

void SystemModules::FlushSettings(void)
{
    for (BaseModules::iterator itr = impl->baseModules.begin(); itr != impl->baseModules.end(); itr++)
    {
        (*itr)->FlushSettings();
    }
}

bool SystemModules::IsValid() const
{
    return impl.get() != nullptr ? impl->valid : false;
}

const std::string & SystemModules::LastSetupDiagnostic() const
{
    static const std::string empty;
    return impl.get() != nullptr ? impl->lastSetupDiagnostic : empty;
}

ISystemModules & SystemModules::Modules()
{
    return *(impl.get());
}
