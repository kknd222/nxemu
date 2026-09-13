#include "module_base.h"
#include <yuzu_common/logging/backend.h>
#include <sstream>
#ifdef ANDROID
#include <yuzu_common/android/java_bridge.h>
#include <dlfcn.h>
#endif

namespace
{
const char * ModuleTypeName(uint16_t type)
{
    switch (type)
    {
    case MODULE_TYPE_LOADER:
        return "loader";
    case MODULE_TYPE_VIDEO:
        return "video";
    case MODULE_TYPE_CPU:
        return "cpu";
    case MODULE_TYPE_OPERATING_SYSTEM:
        return "os";
    default:
        return "unknown";
    }
}

std::string LastDynamicLibraryError()
{
#ifdef _WIN32
    return "";
#else
    const char * error = dlerror();
    return error != nullptr ? error : "";
#endif
}
} // namespace

ModuleBase::ModuleBase() :
    m_lib(nullptr),
    m_moduleInfo({0}),
    EmulationStarting(nullptr),
    EmulationStopping(nullptr),
    ModuleCleanup(nullptr),
    FlushSettings(nullptr)
{
}

ModuleBase::~ModuleBase()
{
    ModuleDone(false);
}

bool ModuleBase::Load(const char * fileName, IModuleNotification * notification, IModuleSettings * settings)
{
    ModuleDone();
    m_lastLoadDiagnostic.clear();
    std::ostringstream diag;
    diag << "path=" << (fileName != nullptr ? fileName : "<null>") << "\n";
    diag << "expectedType=" << ModuleTypeName(ModuleType()) << "(" << static_cast<int>(ModuleType()) << ")\n";
    m_lib = DynamicLibraryOpen(fileName);
    if (m_lib == nullptr)
    {
        diag << "open=failed";
        const std::string error = LastDynamicLibraryError();
        if (!error.empty())
        {
            diag << "\ndlerror=" << error;
        }
        m_lastLoadDiagnostic = diag.str();
        return false;
    }
    diag << "open=ok\n";

    ModuleBase::tyGetModuleInfo GetModuleInfo = (ModuleBase::tyGetModuleInfo)DynamicLibraryGetProc(m_lib, "GetModuleInfo");
    if (GetModuleInfo == nullptr)
    {
        diag << "GetModuleInfo=missing";
        const std::string error = LastDynamicLibraryError();
        if (!error.empty())
        {
            diag << "\ndlerror=" << error;
        }
        m_lastLoadDiagnostic = diag.str();
        return false;
    }
    diag << "GetModuleInfo=ok\n";

    GetModuleInfo(&m_moduleInfo);
    diag << "moduleName=" << m_moduleInfo.name << "\n";
    diag << "moduleType=" << ModuleTypeName(m_moduleInfo.type) << "(" << m_moduleInfo.type << ")\n";
    diag << "moduleVersion=" << m_moduleInfo.version << "\n";
    if (!ValidVersion(m_moduleInfo))
    {
        diag << "validVersion=false";
        m_lastLoadDiagnostic = diag.str();
        return false;
    }
    diag << "validVersion=true\n";
    if (m_moduleInfo.type != ModuleType())
    {
        diag << "typeMatch=false";
        m_lastLoadDiagnostic = diag.str();
        return false;
    }
    diag << "typeMatch=true\n";

    ModuleBase::tyModuleInitialize ModuleInitialize = (ModuleBase::tyModuleInitialize)DynamicLibraryGetProc(m_lib, "ModuleInitialize");
    ModuleCleanup = (ModuleBase::tyModuleCleanup)DynamicLibraryGetProc(m_lib, "ModuleCleanup");
    EmulationStarting = (ModuleBase::tyEmulationStarting)DynamicLibraryGetProc(m_lib, "EmulationStarting");
    EmulationStopping = (ModuleBase::tyEmulationStopping)DynamicLibraryGetProc(m_lib, "EmulationStopping");
    FlushSettings = (ModuleBase::tyFlushSettings)DynamicLibraryGetProc(m_lib, "FlushSettings");

    if (ModuleInitialize == nullptr ||
        ModuleCleanup == nullptr ||
        EmulationStarting == nullptr ||
        EmulationStopping == nullptr ||
        FlushSettings == nullptr)
    {
        diag << "exports=failed\n";
        diag << "ModuleInitialize=" << (ModuleInitialize != nullptr ? "ok" : "missing") << "\n";
        diag << "ModuleCleanup=" << (ModuleCleanup != nullptr ? "ok" : "missing") << "\n";
        diag << "EmulationStarting=" << (EmulationStarting != nullptr ? "ok" : "missing") << "\n";
        diag << "EmulationStopping=" << (EmulationStopping != nullptr ? "ok" : "missing") << "\n";
        diag << "FlushSettings=" << (FlushSettings != nullptr ? "ok" : "missing");
        const std::string error = LastDynamicLibraryError();
        if (!error.empty())
        {
            diag << "\ndlerror=" << error;
        }
        m_lastLoadDiagnostic = diag.str();
        return false;
    }
    diag << "exports=ok\n";

    if (!LoadFunctions())
    {
        diag << "moduleSpecificExports=failed";
        const std::string error = LastDynamicLibraryError();
        if (!error.empty())
        {
            diag << "\ndlerror=" << error;
        }
        m_lastLoadDiagnostic = diag.str();
        return false;
    }
    diag << "moduleSpecificExports=ok\n";

    ModuleInterfaces interfaces = {0};
    interfaces.notification = notification;
    interfaces.settings = settings;
    interfaces.logger = Common::Log::ModuleLogger();
#ifdef ANDROID
    interfaces.java_vm = GetJavaVM();
    interfaces.native_library_class = GetNativeLibraryGlobalRef();
#else
    interfaces.java_vm = nullptr;
    interfaces.native_library_class = nullptr;
#endif
    const int init_result = ModuleInitialize(interfaces);
    diag << "ModuleInitializeResult=" << init_result << "\n";
    if (init_result != 0)
    {
        diag << "load=failed";
        m_lastLoadDiagnostic = diag.str();
        return false;
    }
    diag << "load=ok";
    m_lastLoadDiagnostic = diag.str();
    return true;
}

const std::string & ModuleBase::LastLoadDiagnostic() const
{
    return m_lastLoadDiagnostic;
}

bool ModuleBase::ValidVersion(MODULE_INFO & info)
{
    if (info.type == MODULE_TYPE_LOADER && info.version == MODULE_LOADER_SPECS_VERSION)
    {
        return true;
    }
    if (info.type == MODULE_TYPE_CPU && info.version == MODULE_CPU_SPECS_VERSION)
    {
        return true;
    }
    if (info.type == MODULE_TYPE_VIDEO && info.version == MODULE_VIDEO_SPECS_VERSION)
    {
        return true;
    }
    if (info.type == MODULE_TYPE_OPERATING_SYSTEM && info.version == MODULE_OPERATING_SYSTEM_SPECS_VERSION)
    {
        return true;
    }
    return false;
}

void ModuleBase::ModuleDone(bool callUnloadModule)
{
    if (m_lib == nullptr)
    {
        return;
    }
    if (ModuleCleanup != nullptr)
    {
        ModuleCleanup();    
    }
    if (callUnloadModule)
    {
        UnloadModule();
    }
    DynamicLibraryClose(m_lib);
    m_lib = nullptr;
    ModuleCleanup = nullptr;
    EmulationStarting = nullptr;
    EmulationStopping = nullptr;
    FlushSettings = nullptr;
}
