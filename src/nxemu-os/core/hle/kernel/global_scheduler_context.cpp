// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <algorithm>
#include <limits>
#include "core/hle_execution_time.h"
#include "core/core_timing.h"


#include "yuzu_common/yuzu_assert.h"
#include "core/core.h"
#include "core/hle/kernel/global_scheduler_context.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/k_process.h"
#include "core/guest_thread_trace.h"


namespace Kernel {

GlobalSchedulerContext::GlobalSchedulerContext(KernelCore& kernel)
    : m_kernel{kernel}, m_scheduler_lock{kernel} {

}

GlobalSchedulerContext::~GlobalSchedulerContext() = default;

GlobalSchedulerContext::HleWork GlobalSchedulerContext::BeginHle() {
    auto& timing = m_kernel.System().CoreTiming();
    if (!timing.CpuHleSynchronizationEnabled()) return {};
    KScopedSchedulerLock lock{m_kernel};
    auto& thread = GetCurrentThread(m_kernel);
    // A nested service call is already charged to its enclosing handler.
    if (thread.hle_execution.IsCollecting()) return {};
    const auto start = std::max(thread.hle_available_time,
        static_cast<u64>(timing.GetGlobalTimeNs().count()));
    ASSERT(!thread.hle_execution.IsCollecting());
    thread.hle_execution.Begin(Core::Timing::ReadNativeThreadTimeNs());
    return {start, &thread};
}

u64 GlobalSchedulerContext::EndHle(const HleWork& work) {
    if (!work.executing_thread) return 0;
    const auto elapsed = work.executing_thread->hle_execution.Finish(Core::Timing::ReadNativeThreadTimeNs());
    KScopedSchedulerLock lock{m_kernel};
    auto& timing = m_kernel.System().CoreTiming();
    if (!timing.CpuHleSynchronizationEnabled()) return 0;
    if (!elapsed) {
        // A missing CPU-time sample cannot become free work or blocked wall time.
        timing.DisableCpuHleSynchronization();
        return 0;
    }
    constexpr auto limit = static_cast<u64>(std::numeric_limits<s64>::max());
    const auto start = std::min(work.virtual_start, limit);
    auto& available = work.executing_thread->hle_available_time;
    available = std::max(available, start + std::min(*elapsed, limit - start));
    return available;
}

void GlobalSchedulerContext::AddThread(KThread* thread) {
    std::scoped_lock lock{m_global_list_guard};
    m_thread_list.push_back(thread);
}

void GlobalSchedulerContext::RemoveThread(KThread* thread) {
    std::scoped_lock lock{m_global_list_guard};
    std::erase(m_thread_list, thread);
}

void GlobalSchedulerContext::PreemptThreads() {
    // The priority levels at which the global scheduler preempts threads every 10 ms. They are
    // ordered from Core 0 to Core 5; core3 is the shared system core.
    static constexpr std::array<u32, Hardware::NUM_CPU_CORES> preemption_priorities{
        59,
        59,
        59,
        63,
        59,
        59,
    };

    ASSERT(KScheduler::IsSchedulerLockedByCurrentThread(m_kernel));
    for (u32 core_id = 0; core_id < Hardware::NUM_CPU_CORES; core_id++) {
        const u32 priority = preemption_priorities[core_id];
        KScheduler::RotateScheduledQueue(m_kernel, core_id, priority);
    }
    static uint64_t last_snapshot_ms{};
    if(Core::GuestThreadTrace::Due(last_snapshot_ms)) {
        const auto start=std::chrono::steady_clock::now();
        std::unique_lock list_lock(m_global_list_guard,std::try_to_lock);
        if(list_lock.owns_lock()) {
            std::array<Core::GuestThreadTrace::State,128> rows{};size_t count{};uint64_t omitted{};
            for(int pass=0;pass<2;++pass)for(const auto* t:m_thread_list) {
                const auto* p=t->GetOwnerKProcess();if((p!=nullptr)!=(pass==0))continue;
                if(count==rows.size()){++omitted;continue;}
                auto& row=rows[count++];row.tid=t->GetThreadId();row.pid=p?p->GetProcessId():0;
                row.state=static_cast<int32_t>(t->GetState());row.raw_state=static_cast<int32_t>(t->GetRawState());
                row.priority=t->GetPriority();row.base_priority=t->GetBasePriority();row.current_core=t->GetCurrentCore();
                row.wait_reason=static_cast<int32_t>(t->GetWaitReasonForDebugging());row.type=static_cast<int32_t>(t->GetThreadType());
                const auto* owner=t->GetLockOwner();row.owner_tid=owner?owner->GetThreadId():0;row.wait_generation=t->GetWaitGeneration();
                row.guest_core_count=p?p->GetGuestCoreCount():0;row.active_core=t->GetActiveCore();
                row.physical_affinity_mask=t->GetAffinityMask().GetAffinityMask();row.last_scheduled_tick=t->GetLastScheduledTick();
            }
            Core::GuestThreadTrace::States(rows,count,omitted,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
        }
    }
}

bool GlobalSchedulerContext::IsLocked() const {
    return m_scheduler_lock.IsLockedByCurrentThread();
}

void GlobalSchedulerContext::RegisterDummyThreadForWakeup(KThread* thread) {
    ASSERT(this->IsLocked());

    m_woken_dummy_threads.insert(thread);
}

void GlobalSchedulerContext::UnregisterDummyThreadForWakeup(KThread* thread) {
    ASSERT(this->IsLocked());

    m_woken_dummy_threads.erase(thread);
}

void GlobalSchedulerContext::WakeupWaitingDummyThreads() {
    ASSERT(this->IsLocked());

    for (auto* thread : m_woken_dummy_threads) {
        thread->DummyThreadEndWait();
    }

    m_woken_dummy_threads.clear();
}

} // namespace Kernel
