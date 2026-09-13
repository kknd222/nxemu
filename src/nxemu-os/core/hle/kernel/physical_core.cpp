// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/scope_exit.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "core/core.h"
#include "core/debugger/debugger.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/service/nxemu_android_diagnostics.h"
#include "core/hle/kernel/svc.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <type_traits>
#include <fmt/format.h>
#if ANDROID
#include <cstring>
#include <strings.h>
#include <sys/system_properties.h>
#endif

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>

#include <fmt/format.h>

namespace {

// Lightweight guest-side sampling used while diagnosing games which keep the
// host CPU busy without issuing any further HLE calls.  This deliberately
// bypasses the normal asynchronous logger so it remains useful when the last
// visible log line itself is where execution appears to stall.
void WriteGuestTrace(const Kernel::KThread& thread, ICpuCore& cpu, u32 core_index,
                     CpuHaltReason halt_reason) {
    const char* path = std::getenv("NXEMU_GUEST_TRACE_FILE");
    if (path == nullptr || *path == '\0') {
        return;
    }

    thread_local auto last_sample = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (last_sample.time_since_epoch().count() != 0 &&
        now - last_sample < std::chrono::seconds{1}) {
        return;
    }
    last_sample = now;

    CpuThreadContext context{};
    cpu.GetContext(context);

    u64 previous_fp{};
    u64 caller_lr{};
    if (auto* process = thread.GetOwnerKProcess(); process != nullptr && context.fp != 0 &&
        process->GetMemory().IsValidVirtualAddressRange(context.fp, 16)) {
        previous_fp = process->GetMemory().Read64(context.fp);
        caller_lr = process->GetMemory().Read64(context.fp + 8);
    }

    const auto line = fmt::format(
        "core={} tid={:016X} halt={} pc={:016X} lr={:016X} sp={:016X} fp={:016X} "
        "caller_lr={:016X} prev_fp={:016X} x0={:016X} x1={:016X} x2={:016X} x3={:016X} "
        "x19={:016X} x20={:016X} x21={:016X} x22={:016X} x23={:016X} x24={:016X}\n",
        core_index, thread.GetThreadId(), static_cast<u32>(halt_reason), context.pc, context.lr,
        context.sp, context.fp, caller_lr, previous_fp, context.r[0], context.r[1], context.r[2],
        context.r[3], context.r[19], context.r[20], context.r[21], context.r[22], context.r[23],
        context.r[24]);

    static std::mutex file_mutex;
    std::scoped_lock lock{file_mutex};
    std::ofstream output{path, std::ios::app | std::ios::binary};
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
}

} // namespace

namespace Kernel {

namespace {

#if ANDROID
bool ReadAndroidBoolProperty(const char* name, bool default_value) {
    char value[PROP_VALUE_MAX] = {};
    const int len = __system_property_get(name, value);
    if (len <= 0) {
        return default_value;
    }
    return std::strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
           strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0;
}

bool ShouldTraceAndroidNceCpuCore() {
    static std::atomic<int> cached{-1};
    const int value = cached.load(std::memory_order_relaxed);
    if (value >= 0) {
        return value != 0;
    }
    const bool enabled = ReadAndroidBoolProperty("debug.nxemu.nce.trace", false);
    cached.store(enabled ? 1 : 0, std::memory_order_relaxed);
    return enabled;
}

#define NXEMU_ANDROID_CPU_TRACE(...) \
    do {                             \
        if (ShouldTraceAndroidNceCpuCore()) { \
            LOG_INFO(__VA_ARGS__);   \
        }                            \
    } while (false)
#else
#define NXEMU_ANDROID_CPU_TRACE(...) ((void)0)
#endif

std::atomic<int> g_cpu_snapshot_budget{0};

bool ShouldLogCpuCoreSnapshot(u32 core_index) {
    using namespace std::chrono;
    const int budget = g_cpu_snapshot_budget.load(std::memory_order_relaxed);
    if (budget <= 0) {
        return false;
    }
    static std::array<steady_clock::time_point, 4> last{};
    const auto now = steady_clock::now();
    if (core_index >= last.size() || now - last[core_index] < 1s) {
        return false;
    }
    last[core_index] = now;
    g_cpu_snapshot_budget.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

std::string HexBytes(const std::array<u8, 32>& bytes, size_t count) {
    std::string out;
    out.reserve(count * 3);
    for (size_t i = 0; i < count && i < bytes.size(); ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        out += fmt::format("{:02x}", bytes[i]);
    }
    return out;
}

} // namespace

extern "C" void NxemuRequestCpuCoreSnapshotBudget(int count) {
    g_cpu_snapshot_budget.store(std::clamp(count, 0, 64), std::memory_order_relaxed);
}

PhysicalCore::PhysicalCore(KernelCore & kernel, uint32_t core_index) :
    m_kernel{kernel}, 
    m_core_index{core_index}
{
    m_is_single_core = !kernel.IsMulticore();
}
PhysicalCore::~PhysicalCore() = default;

void PhysicalCore::RunThread(Kernel::KThread * thread)
{
    auto * process = thread->GetOwnerKProcess();
    auto & system = m_kernel.System();
    auto * interface = process->GetCpuCore(m_core_index);

    NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore RunThread begin: core={} thread={} interface={}",
             m_core_index, static_cast<void*>(thread), static_cast<void*>(interface));
    interface->Initialize();
    NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore after Initialize: core={} thread={} interface={}",
             m_core_index, static_cast<void*>(thread), static_cast<void*>(interface));

    const auto EnterContext = [&]() {
        NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore EnterContext begin: core={} interrupted={}",
                 m_core_index, m_is_interrupted);

        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // Check if we are already interrupted. If we are, we can just stop immediately.
        if (m_is_interrupted)
        {
            return false;
        }

        // Mark that we are running.
        m_cpucore = interface;
        m_current_thread = thread;

        // Acquire the lock on the thread parameters.
        // This allows us to force synchronization with Interrupt.
        interface->LockThread(thread);
        NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore EnterContext end: core={} thread={}",
                 m_core_index, static_cast<void*>(thread));

        return true;
    };

    const auto ExitContext = [&]() {
        NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore ExitContext begin: core={} thread={}",
                 m_core_index, static_cast<void*>(thread));
        // Unlock the thread.
        interface->UnlockThread(thread);

        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // On exit, we no longer are running.
        m_cpucore = nullptr;
        m_current_thread = nullptr;
        NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore ExitContext end: core={}", m_core_index);
    };

    while (true)
    {
        // If the thread is scheduled for termination, exit.
        if (thread->HasDpc() && thread->IsTerminationRequested())
        {
            thread->Exit();
        }

        // Notify the debugger and go to sleep if a step was performed
        // and this thread has been scheduled again.
        if (thread->GetStepState() == StepState::StepPerformed)
        {
            process->LogBacktrace(*interface);
            thread->RequestSuspend(SuspendType::Debug);
            return;
        }

        // Otherwise, run the thread.
        CpuHaltReason hr{};
        {
            // If we were interrupted, exit immediately.
            if (!EnterContext())
            {
                return;
            }

            if (thread->GetStepState() == StepState::StepPending)
            {
                hr = interface->StepThread(thread);

                if (hr == CpuHaltReason::StepThread)
                {
                    thread->SetStepState(StepState::StepPerformed);
                }
            }
            else
            {
                NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore before interface RunThread: core={}",
                         m_core_index);
                hr = interface->RunThread(thread);
                NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore after interface RunThread: core={} halt={}",
                         m_core_index, static_cast<int>(hr));
            }

            WriteGuestTrace(*thread, *interface, m_core_index, hr);

            ExitContext();
        }

        if (ShouldLogCpuCoreSnapshot(m_core_index)) {
            CpuThreadContext ctx{};
            interface->GetContext(ctx);
            std::array<u8, 32> pc_bytes{};
            const bool pc_bytes_ok =
                process->GetCoreMemory().ReadBlock(ctx.pc, pc_bytes.data(), 16);
            Service::NxemuAndroidDiagnostics::RecordEvent(
                "CPU.Core",
                fmt::format("core={} thread_id={} halt={} pc={:#x} lr={:#x} sp={:#x} bytes={}",
                            m_core_index, thread->GetThreadId(), static_cast<int>(hr), ctx.pc,
                            ctx.lr, ctx.sp,
                            pc_bytes_ok ? HexBytes(pc_bytes, 16) : "unreadable"));
        }

        // Determine why we stopped.
        //
        // NCE returns CpuHaltReason values as a bit field.  In particular the Android
        // SVC trampoline ORs SupervisorCall with any pending asynchronous esr_el1 bits
        // (for example BreakLoop from SignalInterrupt).  Treating halt reasons as exact
        // enum values makes combined states easy to miss and can leave a guest thread
        // parked after an SVC/fence transition.  Match Eden/yuzu's PhysicalCore logic
        // and test the individual bits instead.
        const auto HasHalt = [](CpuHaltReason value, CpuHaltReason flag) {
            using U = std::underlying_type_t<CpuHaltReason>;
            return (static_cast<U>(value) & static_cast<U>(flag)) != 0;
        };
        const bool step_completed =
            HasHalt(hr, CpuHaltReason::StepThread) &&
            thread->GetStepState() == StepState::StepPerformed;
        const bool supervisor_call =
            !step_completed && HasHalt(hr, CpuHaltReason::SupervisorCall);
        const bool prefetch_abort =
            !step_completed && HasHalt(hr, CpuHaltReason::PrefetchAbort);
        const bool breakpoint =
            !step_completed && HasHalt(hr, CpuHaltReason::InstructionBreakpoint);
        const bool data_abort =
            !step_completed && HasHalt(hr, CpuHaltReason::DataAbort);
        const bool interrupt =
            !step_completed && HasHalt(hr, CpuHaltReason::BreakLoop);

        // Since scheduling may occur here, we cannot use any cached
        // state after returning from calls we make.

        // Notify the debugger and go to sleep if a breakpoint was hit,
        // or if the thread is unable to continue for any reason.
        if (breakpoint || prefetch_abort)
        {
            if (breakpoint)
            {
                interface->RewindBreakpointInstruction();
            }
            process->LogBacktrace(*interface);
            thread->RequestSuspend(SuspendType::Debug);
            return;
        }

        // Notify the debugger and go to sleep on data abort.
        if (data_abort)
        {
            process->LogBacktrace(*interface);
            thread->RequestSuspend(SuspendType::Debug);
            return;
        }

        // Handle system calls.
        if (supervisor_call)
        {
            // Perform call.
            Svc::Call(system, interface->GetSvcNumber());
            return;
        }

        // Handle external interrupt sources.
        if (interrupt || m_is_single_core)
        {
            return;
        }
    }
}

void PhysicalCore::LoadContext(const KThread * thread)
{
    auto * const process = thread->GetOwnerKProcess();
    if (!process)
    {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto * interface = process->GetCpuCore(m_core_index);
    if (interface)
    {
        NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore LoadContext begin: core={} thread={} interface={}",
                 m_core_index, static_cast<const void*>(thread), static_cast<void*>(interface));
        interface->SetContext(thread->GetContext());
        NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore LoadContext after SetContext: core={}",
                 m_core_index);
        interface->SetTpidrroEl0(thread->GetTlsAddress().GetValue());
        NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore LoadContext after SetTpidrro: core={} tls={:#x}",
                 m_core_index, thread->GetTlsAddress().GetValue());
        interface->SetWatchpointArray(process->GetWatchpoints().data(), (uint32_t)process->GetWatchpoints().size());
        NXEMU_ANDROID_CPU_TRACE(Core_ARM, "Android NCE PhysicalCore LoadContext end: core={}", m_core_index);
    }
}

void PhysicalCore::LoadSvcArguments(const KProcess & process, const uint64_t (&args)[8])
{
    process.GetCpuCore(m_core_index)->SetSvcArguments(args);
}

void PhysicalCore::SaveContext(KThread * thread) const
{
    auto * const process = thread->GetOwnerKProcess();
    if (!process)
    {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto * interface = process->GetCpuCore(m_core_index);
    if (interface)
    {
        interface->GetContext(thread->GetContext());
    }
}

void PhysicalCore::SaveSvcArguments(KProcess & process, uint64_t (&args)[8]) const
{
    process.GetCpuCore(m_core_index)->GetSvcArguments(args);
}

void PhysicalCore::CloneFpuStatus(KThread * dst) const
{
    auto * process = dst->GetOwnerKProcess();

    CpuThreadContext ctx{};
    process->GetCpuCore(m_core_index)->GetContext(ctx);

    dst->GetContext().fpcr = ctx.fpcr;
    dst->GetContext().fpsr = ctx.fpsr;
}

void PhysicalCore::LogBacktrace()
{
    auto * process = GetCurrentProcessPointer(m_kernel);
    if (!process)
    {
        return;
    }

    auto * interface = process->GetCpuCore(m_core_index);
    if (interface)
    {
        process->LogBacktrace(*interface);
    }
}

void PhysicalCore::Idle()
{
    std::unique_lock lk{m_guard};
    m_on_interrupt.wait(lk, [this] { return m_is_interrupted; });
}

bool PhysicalCore::IsInterrupted() const
{
    return m_is_interrupted;
}

void PhysicalCore::Interrupt()
{
    // Lock core context.
    std::scoped_lock lk{m_guard};

    // Load members.
    auto * cpucore = m_cpucore;
    auto * thread = m_current_thread;

    // Add interrupt flag.
    m_is_interrupted = true;

    // Interrupt ourselves.
    m_on_interrupt.notify_one();

    // If there is no thread running, we are done.
    if (cpucore == nullptr)
    {
        return;
    }

    // Interrupt the CPU.
    cpucore->SignalInterrupt(thread);
}

void PhysicalCore::ClearInterrupt()
{
    std::scoped_lock lk{m_guard};
    m_is_interrupted = false;
}

} // namespace Kernel

