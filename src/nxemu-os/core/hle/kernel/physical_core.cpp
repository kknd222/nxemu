// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "core/core.h"
#include "core/debugger/debugger.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc.h"

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

    interface->Initialize();

    const auto EnterContext = [&]() {
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

        return true;
    };

    const auto ExitContext = [&]() {
        // Unlock the thread.
        interface->UnlockThread(thread);

        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // On exit, we no longer are running.
        m_cpucore = nullptr;
        m_current_thread = nullptr;
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
            if (system.DebuggerEnabled())
            {
                UNIMPLEMENTED();
            }
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
                hr = interface->RunThread(thread);
            }

            WriteGuestTrace(*thread, *interface, m_core_index, hr);

            ExitContext();
        }

        // Determine why we stopped.
        const bool supervisor_call = hr == CpuHaltReason::SupervisorCall || hr == CpuHaltReason::SupervisorCallBreakLoop;
        const bool prefetch_abort = hr == CpuHaltReason::PrefetchAbort || hr == CpuHaltReason::PrefetchAbortBreakLoop;
        const bool breakpoint = hr == CpuHaltReason::InstructionBreakpoint;
        const bool data_abort = hr == CpuHaltReason::DataAbort;
        const bool interrupt = hr == CpuHaltReason::BreakLoop || hr == CpuHaltReason::SupervisorCallBreakLoop || hr == CpuHaltReason::PrefetchAbortBreakLoop;

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
            if (system.DebuggerEnabled())
            {
                UNIMPLEMENTED();
            }
            else
            {
                UNIMPLEMENTED();
            }
            thread->RequestSuspend(SuspendType::Debug);
            return;
        }

        // Notify the debugger and go to sleep on data abort.
        if (data_abort)
        {
            if (system.DebuggerEnabled())
            {
                UNIMPLEMENTED();
            }
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
        interface->SetContext(thread->GetContext());
        interface->SetTpidrroEl0(GetInteger(thread->GetTlsAddress()));
        interface->SetWatchpointArray(process->GetWatchpoints().data(), (uint32_t)process->GetWatchpoints().size());
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
        UNIMPLEMENTED();
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
