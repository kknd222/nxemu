// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <yuzu_common/yuzu_assert.h>
#include "arm_dynarmic.h"
#include <atomic>
#include "yuzu_common/debug_document.h"
#include "yuzu_common/logging/log.h"


namespace {
// Failure-only observation: no context, clock, allocation or atomics on mapped paths.
// Shared across directions for this CPU-module lifetime; not a per-game reset budget.
void ReportUnmappedHalt(const char* direction, uint32_t raw, uint32_t mapped,
                        const ICpuCore* core, uint32_t core_index, const char* operation) noexcept {
    try {
        if (Common::DebugDocument::Environment("NXEMU_CPU_HALT_TRACE") == "0") return;
        static std::atomic<uint32_t> remaining{64};
        auto slots = remaining.load(std::memory_order_relaxed);
        while (slots && !remaining.compare_exchange_weak(slots, slots - 1, std::memory_order_relaxed)) {}
        if (!slots) return;
        CpuThreadContext ctx{};
        bool valid{};
        unsigned arch{};
        if (core) {
            try {
                arch = core->GetArchitecture() == ProcessorArchitecture::AArch64 ? 64 : 32;
                core->GetContext(ctx);
                valid = true;
            } catch (...) { ctx = {}; }
        }
        const bool bitmask = direction[0] == 'd';
        const uint32_t unknown_bits = bitmask ? raw & ~0xff000007u : 0;
        LOG_CRITICAL(Core_ARM,
            "NXEMU_CPU_HALT_MAP schema=1 direction={} raw_kind={} raw=0x{:08x} mapped={} classification=unmapped_fallback unknown_bits=0x{:08x} core={} arch={} context_valid={} operation={} pc=0x{:016x} sp=0x{:016x} lr=0x{:016x} pstate=0x{:08x} x0=0x{:016x} x1=0x{:016x} x2=0x{:016x} x3=0x{:016x} x4=0x{:016x} x5=0x{:016x} x6=0x{:016x} x7=0x{:016x} ordinal={} limit=64 budget_scope=cpu_module_load epoch_ms={} run_id={} generation={}",
            direction, bitmask ? "bitmask" : "enum", raw, mapped, unknown_bits, core_index, arch,
            unsigned(valid), operation, ctx.pc, ctx.sp, ctx.lr, ctx.pstate,
            ctx.r[0],ctx.r[1],ctx.r[2],ctx.r[3],ctx.r[4],ctx.r[5],ctx.r[6],ctx.r[7],65-slots,
            Common::DebugDocument::EpochMs(),
            Common::DebugDocument::Quote(Common::DebugDocument::Environment("NXEMU_DEBUG_RUN_ID")),
            Common::DebugDocument::Quote(Common::DebugDocument::Environment("NXEMU_DEBUG_GENERATION")));
    } catch (...) { /* Diagnostic failures must not replace the original halt behavior. */ }
}
}

Dynarmic::HaltReason TranslateDynarmicHaltReason(CpuHaltReason hr)
{
    switch (hr)
    {
    case CpuHaltReason::None: return Dynarmic::HaltReason::None;
    case CpuHaltReason::StepThread: return Dynarmic::HaltReason::Step;
    case CpuHaltReason::CacheInvalidation: return Dynarmic::HaltReason::CacheInvalidation;
    case CpuHaltReason::DataAbort: return Dynarmic::HaltReason::MemoryAbort;
    case CpuHaltReason::BreakLoop: return Dynarmic::HaltReason::UserDefined2;
    case CpuHaltReason::SupervisorCall: return Dynarmic::HaltReason::UserDefined3;
    case CpuHaltReason::SupervisorCallBreakLoop: return (Dynarmic::HaltReason::UserDefined3 | Dynarmic::HaltReason::UserDefined2);
    case CpuHaltReason::InstructionBreakpoint: return Dynarmic::HaltReason::UserDefined4;
    case CpuHaltReason::PrefetchAbort: return Dynarmic::HaltReason::UserDefined6;
    case CpuHaltReason::PrefetchAbortBreakLoop: return (Dynarmic::HaltReason::UserDefined6 | Dynarmic::HaltReason::UserDefined2);
    }
    ReportUnmappedHalt("cpu_to_dynarmic", static_cast<uint32_t>(hr), static_cast<uint32_t>(Dynarmic::HaltReason::Step), nullptr, UINT32_MAX, "conversion");
    UNIMPLEMENTED();
    return Dynarmic::HaltReason::Step;
}

CpuHaltReason TranslateHaltReason(Dynarmic::HaltReason hr, const ICpuCore* core, uint32_t core_index, const char* operation)
{
    switch (hr)
    {
    case Dynarmic::HaltReason::None: return CpuHaltReason::None;
    case Dynarmic::HaltReason::Step: return CpuHaltReason::StepThread;
    case Dynarmic::HaltReason::CacheInvalidation: return CpuHaltReason::CacheInvalidation;
    case Dynarmic::HaltReason::MemoryAbort: return CpuHaltReason::DataAbort;
    case Dynarmic::HaltReason::UserDefined2: return CpuHaltReason::BreakLoop;
    case Dynarmic::HaltReason::UserDefined3: return CpuHaltReason::SupervisorCall;
    case Dynarmic::HaltReason::UserDefined4: return CpuHaltReason::InstructionBreakpoint;
    case Dynarmic::HaltReason::UserDefined6: return CpuHaltReason::PrefetchAbort;
    case Dynarmic::HaltReason::UserDefined2and3: return CpuHaltReason::SupervisorCallBreakLoop;
    case Dynarmic::HaltReason::UserDefined2and6: return CpuHaltReason::PrefetchAbortBreakLoop;
    default:
        // Preserve a data abort if an interrupt or step also fired.
        if (Dynarmic::Has(hr, Dynarmic::HaltReason::MemoryAbort))
        {
            return CpuHaltReason::DataAbort;
        }
        break;
    }
    ReportUnmappedHalt("dynarmic_to_cpu", static_cast<uint32_t>(hr), static_cast<uint32_t>(CpuHaltReason::StepThread), core, core_index, operation);
    UNIMPLEMENTED();
    return CpuHaltReason::StepThread;
}
