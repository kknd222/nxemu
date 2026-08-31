#include "cpu_manager.h"
#include "cpu_settings.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-module-spec/base.h>
#include <yuzu_common/logging/log.h>
#include "arm_dynarmic_64.h"
#include "arm_dynarmic_32.h"
#include "patch/patch_collection.h"
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
#include "nce/arm_nce.h"
#include "arm_dynarmic.h"
#endif
#if defined(_M_X64) || defined(ARCHITECTURE_x86_64) || defined(_M_ARM64) || defined(ARCHITECTURE_arm64)
#include "exclusive_monitor_interface.h"
#endif

extern IModuleSettings * g_settings;

CpuInterface::CpuInterface(ISystemModules & modules, uint32_t processorCount) :
    m_modules(modules),
    m_monitor(processorCount)
{
}

CpuInterface::~CpuInterface()
{
}

bool CpuInterface::Initialize(void)
{
    return true;
}

IExclusiveMonitor * CpuInterface::CreateExclusiveMonitor(IMemory & memory)
{
#if defined(_M_X64) || defined(ARCHITECTURE_x86_64) || defined(_M_ARM64) || defined(ARCHITECTURE_arm64)
    return new ExclusiveMonitor(memory, m_monitor);
#else
    // TODO(merry): Passthrough exclusive monitor
    return nullptr;
#endif
}

IPatchCollection * CpuInterface::CreatePatchCollection(bool is_application)
{
    return new PatchCollection(m_modules, is_application);
}

ICpuCore * CpuInterface::CreateCpuCore(ICoreSystem & system, bool is64Bit, bool usesWallClock, IKernelProcess & process, uint32_t coreIndex)
{
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    LOG_INFO(Core_ARM,
             "Android NCE CreateCpuCore begin: core={} is64Bit={} nceEnabled={} backend={} fastmem={}",
             coreIndex, is64Bit, g_settings->GetBool(NXCpuSetting::NceEnabled),
             g_settings->GetInt(NXCpuSetting::CpuBackend),
             g_settings->GetBool(NXCpuSetting::CpuoptFastmem));
    if (is64Bit && g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        // Register the scoped JIT handler before creating any NCE instances
        // so that its signal handler will appear first in the signal chain.
        LOG_INFO(Core_ARM, "Android NCE CreateCpuCore registering scoped JIT handler: core={}", coreIndex);
        ScopedJitExecution::RegisterHandler();
        LOG_INFO(Core_ARM, "Android NCE CreateCpuCore creating ArmNce: core={}", coreIndex);

        auto* core = new Core::ArmNce(system, usesWallClock, process, coreIndex);
        LOG_INFO(Core_ARM, "Android NCE CreateCpuCore created ArmNce: core={} ptr={}", coreIndex,
                 static_cast<void*>(core));
        return core;
    }
    else
#endif
    if (is64Bit)
    {
        LOG_INFO(Core_ARM, "Android NCE CreateCpuCore using Dynarmic64: core={} is64Bit={}", coreIndex,
                 is64Bit);
        return new ArmDynarmic64(system, usesWallClock, process, m_monitor, coreIndex);
    }
    LOG_INFO(Core_ARM, "Android NCE CreateCpuCore using Dynarmic32: core={}", coreIndex);
    return new ArmDynarmic32(system, usesWallClock, process, m_monitor, coreIndex);
}

