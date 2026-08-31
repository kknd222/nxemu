// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cinttypes>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <span>

#include "core/arm/nce/arm_nce.h"
#include "arm_dynarmic.h"
#include "nce/interpreter_visitor.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/arm/nce/patcher.h"
#include "yuzu_common/logging/log.h"

#include <signal.h>
#include <sys/mman.h>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#include <strings.h>
#endif
#include <sys/syscall.h>
#include <unistd.h>

namespace Core {

namespace {

struct sigaction g_orig_bus_action;
struct sigaction g_orig_segv_action;
bool g_have_orig_bus_action{};
bool g_have_orig_segv_action{};
thread_local void* g_pending_return_signal_tpidr{};

struct ActiveNceSignalThread {
    std::atomic<int> tid{};
    std::atomic<uintptr_t> tpidr{};
};

constexpr std::size_t MaxActiveNceSignalThreads = 32;
std::array<ActiveNceSignalThread, MaxActiveNceSignalThreads> g_active_nce_signal_threads{};

int CurrentTidNoThrow() {
    return static_cast<int>(syscall(SYS_gettid));
}

void RegisterActiveNceSignalThread(int tid, Kernel::KThread::NativeExecutionParameters* tpidr) {
    const auto raw_tpidr = reinterpret_cast<uintptr_t>(tpidr);
    for (auto& slot : g_active_nce_signal_threads) {
        const int slot_tid = slot.tid.load(std::memory_order_acquire);
        if (slot_tid == tid) {
            slot.tpidr.store(raw_tpidr, std::memory_order_release);
            return;
        }
    }

    for (auto& slot : g_active_nce_signal_threads) {
        int expected = 0;
        if (slot.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
            slot.tpidr.store(raw_tpidr, std::memory_order_release);
            return;
        }
    }

    // Extremely unlikely on Android (normally <= 4 guest CPU threads). Prefer preserving the
    // latest thread over silently leaving the handler unable to recover host TLS.
    g_active_nce_signal_threads[static_cast<std::size_t>(tid) % MaxActiveNceSignalThreads].tpidr.store(
        raw_tpidr, std::memory_order_release);
    g_active_nce_signal_threads[static_cast<std::size_t>(tid) % MaxActiveNceSignalThreads].tid.store(
        tid, std::memory_order_release);
}

void UnregisterActiveNceSignalThread(int tid, Kernel::KThread::NativeExecutionParameters* tpidr) {
    const auto raw_tpidr = reinterpret_cast<uintptr_t>(tpidr);
    for (auto& slot : g_active_nce_signal_threads) {
        if (slot.tid.load(std::memory_order_acquire) != tid) {
            continue;
        }
        if (slot.tpidr.load(std::memory_order_acquire) == raw_tpidr) {
            slot.tpidr.store(0, std::memory_order_release);
            slot.tid.store(0, std::memory_order_release);
        }
        return;
    }
}

Kernel::KThread::NativeExecutionParameters* FindActiveNceSignalThread(void* candidate_tpidr, int tid) {
    const auto raw_candidate = reinterpret_cast<uintptr_t>(candidate_tpidr);
    if (raw_candidate != 0) {
        for (auto& slot : g_active_nce_signal_threads) {
            const auto raw_tpidr = slot.tpidr.load(std::memory_order_acquire);
            if (raw_tpidr == raw_candidate) {
                return reinterpret_cast<Kernel::KThread::NativeExecutionParameters*>(raw_tpidr);
            }
        }
    }

    for (auto& slot : g_active_nce_signal_threads) {
        if (slot.tid.load(std::memory_order_acquire) == tid) {
            const auto raw_tpidr = slot.tpidr.load(std::memory_order_acquire);
            if (raw_tpidr != 0) {
                return reinterpret_cast<Kernel::KThread::NativeExecutionParameters*>(raw_tpidr);
            }
        }
    }
    return nullptr;
}

void RestoreHostTpidrEl0(u64 host_tpidr) {
#if defined(__aarch64__)
    asm volatile("msr tpidr_el0, %0" : : "r"(host_tpidr) : "memory");
#else
    (void)host_tpidr;
#endif
}

int LibcSigAction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    static auto* libc_sigaction = [] {
        void* libc = dlopen("libc.so", RTLD_LOCAL | RTLD_LAZY);
        void* sym = libc ? dlsym(libc, "sigaction") : nullptr;
        if (sym == nullptr) {
            sym = dlsym(RTLD_DEFAULT, "sigaction");
        }
        return reinterpret_cast<decltype(&sigaction)>(sym);
    }();
    return libc_sigaction ? libc_sigaction(signum, act, oldact) : sigaction(signum, act, oldact);
}

template <typename Callback>
void InvokeOriginalActionWithMask(int sig, const struct sigaction& original, Callback&& callback) {
    sigset_t callback_mask = original.sa_mask;
    sigset_t previous_mask;

    if ((original.sa_flags & SA_NODEFER) == 0) {
        sigaddset(&callback_mask, sig);
    }

    sigprocmask(SIG_BLOCK, &callback_mask, &previous_mask);

    if ((original.sa_flags & SA_NODEFER) != 0) {
        sigset_t nodefer_mask;
        sigemptyset(&nodefer_mask);
        sigaddset(&nodefer_mask, sig);
        sigprocmask(SIG_UNBLOCK, &nodefer_mask, nullptr);
    }

    callback();
    sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
}

void ForwardSignalToOriginalAction(int sig, siginfo_t* info, void* raw_context,
                                   const struct sigaction& original) {
    if (original.sa_handler == SIG_IGN) {
        return;
    }

    if (original.sa_handler == SIG_DFL) {
        LibcSigAction(sig, &original, nullptr);
        syscall(SYS_tkill, gettid(), sig);
        return;
    }

    if ((original.sa_flags & SA_SIGINFO) != 0 && original.sa_sigaction != nullptr) {
        InvokeOriginalActionWithMask(sig, original,
                                     [&] { original.sa_sigaction(sig, info, raw_context); });
        return;
    }

    if ((original.sa_flags & SA_SIGINFO) == 0 && original.sa_handler != nullptr) {
        InvokeOriginalActionWithMask(sig, original, [&] { original.sa_handler(sig); });
        return;
    }

    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    LibcSigAction(sig, &default_action, nullptr);
    syscall(SYS_tkill, gettid(), sig);
}

using HandlerType = decltype(sigaction::sa_sigaction);

bool UseNceAltSignalStack();

bool IsNceSignalAction(int sig, const struct sigaction& action) {
    if ((action.sa_flags & SA_SIGINFO) == 0) {
        return false;
    }

    const auto handler = reinterpret_cast<HandlerType>(action.sa_sigaction);
    if (sig == ReturnToRunCodeByExceptionLevelChangeSignal) {
        return handler == reinterpret_cast<HandlerType>(
                              &ArmNce::ReturnToRunCodeByExceptionLevelChangeSignalHandler);
    }
    if (sig == BreakFromRunCodeSignal) {
        return handler == reinterpret_cast<HandlerType>(&ArmNce::BreakFromRunCodeSignalHandler);
    }
    if (sig == GuestAlignmentFaultSignal) {
        return handler == reinterpret_cast<HandlerType>(&ArmNce::GuestAlignmentFaultSignalHandler);
    }
    if (sig == GuestAccessFaultSignal) {
        return handler == reinterpret_cast<HandlerType>(&ArmNce::GuestAccessFaultSignalHandler);
    }
    return false;
}

void SaveOriginalSignalActionIfNeeded(int sig, const struct sigaction& old_action) {
    if (IsNceSignalAction(sig, old_action)) {
        return;
    }

    if (sig == GuestAlignmentFaultSignal) {
        g_orig_bus_action = old_action;
        g_have_orig_bus_action = true;
    } else if (sig == GuestAccessFaultSignal) {
        g_orig_segv_action = old_action;
        g_have_orig_segv_action = true;
    }
}

void InstallNceSignalAction(int sig, HandlerType handler, const sigset_t& signal_mask,
                            int extra_flags = 0, bool refresh_original = false) {
    struct sigaction current {};
    LibcSigAction(sig, nullptr, &current);

    const bool already_installed = IsNceSignalAction(sig, current);
    if (already_installed && !refresh_original) {
        return;
    }

    if (!already_installed) {
        SaveOriginalSignalActionIfNeeded(sig, current);
    }

    struct sigaction action {};
    action.sa_flags = SA_SIGINFO | extra_flags;
    if (UseNceAltSignalStack()) {
        action.sa_flags |= SA_ONSTACK;
    }
    action.sa_sigaction = reinterpret_cast<HandlerType>(handler);
    action.sa_mask = signal_mask;

    struct sigaction old_action {};
    LibcSigAction(sig, &action, &old_action);
    SaveOriginalSignalActionIfNeeded(sig, old_action);
}

sigset_t BuildNceSignalMask() {
    sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, ReturnToRunCodeByExceptionLevelChangeSignal);
    sigaddset(&signal_mask, BreakFromRunCodeSignal);
    sigaddset(&signal_mask, GuestAlignmentFaultSignal);
    sigaddset(&signal_mask, GuestAccessFaultSignal);
    return signal_mask;
}

void InstallNceSignalHandlers(bool refresh_fault_handlers) {
    const sigset_t signal_mask = BuildNceSignalMask();

    InstallNceSignalAction(
        ReturnToRunCodeByExceptionLevelChangeSignal,
        reinterpret_cast<HandlerType>(&ArmNce::ReturnToRunCodeByExceptionLevelChangeSignalHandler),
        signal_mask);
    InstallNceSignalAction(BreakFromRunCodeSignal,
                           reinterpret_cast<HandlerType>(&ArmNce::BreakFromRunCodeSignalHandler),
                           signal_mask);

    // Vulkan/driver stacks are allowed to install their own SIGSEGV handlers after CPU init.
    // NCE depends on SIGSEGV/SIGBUS while executing guest code, so re-arm those handlers right
    // before entering guest. If a driver replaced our handler, keep it as the host fallback.
    InstallNceSignalAction(
        GuestAlignmentFaultSignal,
        reinterpret_cast<HandlerType>(&ArmNce::GuestAlignmentFaultSignalHandler), signal_mask, 0,
        refresh_fault_handlers);
    InstallNceSignalAction(GuestAccessFaultSignal,
                           reinterpret_cast<HandlerType>(&ArmNce::GuestAccessFaultSignalHandler),
                           signal_mask, SA_RESTART, refresh_fault_handlers);
}

void InstallNceControlSignalHandlers() {
    const sigset_t signal_mask = BuildNceSignalMask();

    // These signals are generated by NCE itself (tkill SIGUSR2 to enter guest and SIGURG to
    // break out). Installing them during Initialize() makes early/lifecycle races harmless
    // without stealing SIGSEGV/SIGBUS from ART/Vulkan before guest code is actually running.
    InstallNceSignalAction(
        ReturnToRunCodeByExceptionLevelChangeSignal,
        reinterpret_cast<HandlerType>(&ArmNce::ReturnToRunCodeByExceptionLevelChangeSignalHandler),
        signal_mask);
    InstallNceSignalAction(BreakFromRunCodeSignal,
                           reinterpret_cast<HandlerType>(&ArmNce::BreakFromRunCodeSignalHandler),
                           signal_mask);
}

// Verify assembly offsets.
using NativeExecutionParameters = Kernel::KThread::NativeExecutionParameters;
constexpr u32 NativeExecutionParametersMagic = Common::MakeMagic('Y', 'U', 'Z', 'U');
static_assert(offsetof(NativeExecutionParameters, native_context) == TpidrEl0NativeContext);
static_assert(offsetof(NativeExecutionParameters, lock) == TpidrEl0Lock);
static_assert(offsetof(NativeExecutionParameters, magic) == TpidrEl0TlsMagic);

fpsimd_context* GetFloatingPointState(mcontext_t& host_ctx) {
    auto* const begin = reinterpret_cast<char*>(&host_ctx.__reserved);
    auto* const end = begin + sizeof(host_ctx.__reserved);

    for (auto* ptr = begin; ptr + sizeof(_aarch64_ctx) <= end;) {
        auto* header = reinterpret_cast<_aarch64_ctx*>(ptr);
        if (header->magic == FPSIMD_MAGIC) {
            if (header->size >= sizeof(fpsimd_context) &&
                static_cast<size_t>(end - ptr) >= sizeof(fpsimd_context)) {
                return reinterpret_cast<fpsimd_context*>(header);
            }
            LOG_CRITICAL(Core_ARM,
                         "Malformed NCE signal FPSIMD context: size={:#x} remaining={:#x}",
                         header->size, static_cast<size_t>(end - ptr));
            return nullptr;
        }
        if (header->magic == 0 && header->size == 0) {
            break;
        }
        if (header->size < sizeof(_aarch64_ctx) || ptr + header->size > end) {
            LOG_CRITICAL(Core_ARM,
                         "Malformed NCE signal context chain: magic={:#x} size={:#x}",
                         header->magic, header->size);
            return nullptr;
        }
        ptr += header->size;
    }
    LOG_CRITICAL(Core_ARM, "NCE signal frame missing FPSIMD context");
    return nullptr;
}

using namespace Common::Literals;
constexpr u32 StackSize = 128_KiB;

bool ReadAndroidNceBoolProperty(const char* name, bool default_value) {
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {};
    const int len = __system_property_get(name, value);
    if (len <= 0) {
        return default_value;
    }
    return std::strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
           strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0;
#else
    (void)name;
    return default_value;
#endif
}

bool UseNceAltSignalStack() {
#if defined(__ANDROID__)
    // Eden/yuzu install NCE handlers on an alternate signal stack.  On RMX3700/Kirby this is
    // required: running guest signal handlers on the guest stack can die with a bare SIGSEGV
    // before Vulkan presents its first frame. Keep it overridable for A/B probes.
    return ReadAndroidNceBoolProperty("debug.nxemu.nce.altstack", true);
#else
    return true;
#endif
}

bool UseNceNoInitProbe() {
#if defined(__ANDROID__)
    return ReadAndroidNceBoolProperty("debug.nxemu.nce.noinit", false);
#else
    return false;
#endif
}

bool UseNceNoRunProbe() {
#if defined(__ANDROID__)
    return ReadAndroidNceBoolProperty("debug.nxemu.nce.norun", false);
#else
    return false;
#endif
}

bool UseNceControlSignalsOnlyProbe() {
#if defined(__ANDROID__)
    return ReadAndroidNceBoolProperty("debug.nxemu.nce.control_only", false);
#else
    return false;
#endif
}

bool UseNceDirectTrace() {
#if defined(__ANDROID__)
    return ReadAndroidNceBoolProperty("debug.nxemu.nce.trace", false);
#else
    return false;
#endif
}

bool ConsumeNceDispatchLogBudget() {
#if defined(__ANDROID__)
    // This path can be hit hundreds/thousands of times while guest pages are invalidated.
    // Logging each handled fault materially hurts Android performance and can make NCE look
    // slower than Dynarmic. Keep a tiny breadcrumb budget for normal builds; use
    // debug.nxemu.nce.trace=1 when a full signal/dispatch trace is needed.
    static std::atomic<int> budget{16};
    return budget.fetch_sub(1, std::memory_order_relaxed) > 0;
#else
    return true;
#endif
}

bool ConsumeNceDirectLogBudget() {
#if defined(__ANDROID__)
    // Android log printing from a signal path is useful for probes but is not async-signal-safe.
    // Keep it opt-in and bounded; otherwise tight SVC loops can crash inside the logger itself.
    static std::atomic<int> budget{256};
    return budget.fetch_sub(1, std::memory_order_relaxed) > 0;
#else
    return false;
#endif
}

void AndroidNceDirectLog(const char* message) {
#if defined(__ANDROID__)
    if (!UseNceDirectTrace() || !ConsumeNceDirectLogBudget()) {
        return;
    }
    __android_log_print(ANDROID_LOG_ERROR, "NxEmuNCE", "%s", message ? message : "");
#else
    (void)message;
#endif
}

void AndroidNceDirectLogf(const char* fmt, ...) {
#if defined(__ANDROID__)
    if (!UseNceDirectTrace() || !ConsumeNceDirectLogBudget()) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, "NxEmuNCE", fmt ? fmt : "", args);
    va_end(args);
#else
    (void)fmt;
#endif
}

void EnsureNceSignalStackForCurrentThread(const char* reason) {
    if (!UseNceAltSignalStack()) {
        if (UseNceDirectTrace()) {
            LOG_INFO(Core_ARM,
                     "Android NCE per-thread signal stack skipped: reason={} tid={} altstack=disabled",
                     reason ? reason : "", gettid());
        }
        return;
    }

    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) == 0 &&
        current.ss_sp != nullptr && current.ss_size >= StackSize) {
        if (UseNceDirectTrace() && ConsumeNceDirectLogBudget()) {
            LOG_INFO(Core_ARM,
                     "Android NCE existing per-thread signal stack reused: reason={} tid={} sp={} size={:#x}",
                     reason ? reason : "", gettid(), current.ss_sp,
                     static_cast<unsigned int>(current.ss_size));
        }
        return;
    }

    thread_local std::unique_ptr<u8[]> thread_signal_stack;
    if (!thread_signal_stack) {
        thread_signal_stack = std::make_unique<u8[]>(StackSize);
    }

    stack_t ss{};
    ss.ss_sp = thread_signal_stack.get();
    ss.ss_size = StackSize;
    ss.ss_flags = 0;

    if (sigaltstack(&ss, nullptr) != 0) {
        const int saved_errno = errno;
        LOG_ERROR(Core_ARM,
                  "Android NCE failed to install per-thread signal stack: reason={} tid={} errno={} "
                  "({})",
                  reason ? reason : "", gettid(), saved_errno, strerror(saved_errno));
        return;
    }

    LOG_INFO(Core_ARM, "Android NCE installed per-thread signal stack: reason={} tid={} size={:#x}",
             reason ? reason : "", gettid(), StackSize);
}

} // namespace

void* ArmNce::RestoreGuestContext(void* raw_context) {
    // Retrieve the host context.
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    AndroidNceDirectLogf("RestoreGuestContext begin raw=%p pc=0x%llx sp=0x%llx x9=0x%llx",
                         raw_context, static_cast<unsigned long long>(host_ctx.pc),
                         static_cast<unsigned long long>(host_ctx.sp),
                         static_cast<unsigned long long>(host_ctx.regs[9]));

    // Thread-local parameters will be located in x9.
    auto* tpidr = static_cast<NativeExecutionParameters*>(g_pending_return_signal_tpidr);
    if (tpidr == nullptr) {
        tpidr = reinterpret_cast<Kernel::KThread::NativeExecutionParameters*>(host_ctx.regs[9]);
    } else if (reinterpret_cast<uintptr_t>(tpidr) != static_cast<uintptr_t>(host_ctx.regs[9])) {
        LOG_WARNING(Core_ARM,
                    "Android NCE Return signal x9 mismatch: pending_tpidr={} signal_x9={:#018X}",
                    static_cast<void*>(tpidr), host_ctx.regs[9]);
    }
    if (tpidr == nullptr) {
        AndroidNceDirectLog("RestoreGuestContext fail missing tpidr");
        LOG_CRITICAL(Core_ARM,
                     "Android NCE RestoreGuestContext missing tpidr: signal_x9={:#018X} pc={:#018X} "
                     "sp={:#018X}",
                     host_ctx.regs[9], host_ctx.pc, host_ctx.sp);
        return nullptr;
    }
    if (tpidr->magic != NativeExecutionParametersMagic) {
        AndroidNceDirectLogf("RestoreGuestContext fail bad magic tpidr=%p magic=0x%x", tpidr,
                             tpidr->magic);
        LOG_CRITICAL(Core_ARM,
                     "Android NCE RestoreGuestContext bad tpidr magic: tpidr={} magic={:#010X} "
                     "expected={:#010X} signal_x9={:#018X} pc={:#018X} sp={:#018X}",
                     static_cast<void*>(tpidr), tpidr->magic, NativeExecutionParametersMagic,
                     host_ctx.regs[9], host_ctx.pc, host_ctx.sp);
        return nullptr;
    }
    auto* guest_ctx = static_cast<GuestContext*>(tpidr->native_context);
    if (guest_ctx == nullptr) {
        AndroidNceDirectLogf("RestoreGuestContext fail missing native_context tpidr=%p", tpidr);
        LOG_CRITICAL(Core_ARM,
                     "Android NCE RestoreGuestContext missing native_context: tpidr={} "
                     "signal_x9={:#018X} pc={:#018X} sp={:#018X}",
                     static_cast<void*>(tpidr), host_ctx.regs[9], host_ctx.pc, host_ctx.sp);
        return nullptr;
    }

    // Retrieve the host floating point state.
    auto* fpctx = GetFloatingPointState(host_ctx);
    if (fpctx == nullptr) {
        AndroidNceDirectLogf("RestoreGuestContext fail missing FPSIMD tpidr=%p guest=%p", tpidr,
                             guest_ctx);
        LOG_CRITICAL(Core_ARM,
                     "Android NCE RestoreGuestContext missing FPSIMD: tpidr={} guest_ctx={} "
                     "guest_pc={:#018X} guest_sp={:#018X}",
                     static_cast<void*>(tpidr), static_cast<void*>(guest_ctx), guest_ctx->pc,
                     guest_ctx->sp);
        return nullptr;
    }

    // Save host callee-saved registers.
    std::memcpy(guest_ctx->host_ctx.host_saved_vregs.data(), &fpctx->vregs[8],
                sizeof(guest_ctx->host_ctx.host_saved_vregs));
    std::memcpy(guest_ctx->host_ctx.host_saved_regs.data(), &host_ctx.regs[19],
                sizeof(guest_ctx->host_ctx.host_saved_regs));

    // Save stack pointer.
    guest_ctx->host_ctx.host_sp = host_ctx.sp;
    AndroidNceDirectLogf(
        "RestoreGuestContext switching to guest tpidr=%p guest=%p guest_pc=0x%llx guest_sp=0x%llx "
        "host_sp=0x%llx",
        tpidr, guest_ctx, static_cast<unsigned long long>(guest_ctx->pc),
        static_cast<unsigned long long>(guest_ctx->sp),
        static_cast<unsigned long long>(guest_ctx->host_ctx.host_sp));

    // Restore all guest state except tpidr_el0.
    host_ctx.sp = guest_ctx->sp;
    host_ctx.pc = guest_ctx->pc;
    host_ctx.pstate = guest_ctx->pstate;
    fpctx->fpcr = guest_ctx->fpcr;
    fpctx->fpsr = guest_ctx->fpsr;
    std::memcpy(host_ctx.regs, guest_ctx->cpu_registers.data(), sizeof(host_ctx.regs));
    std::memcpy(fpctx->vregs, guest_ctx->vector_registers.data(), sizeof(fpctx->vregs));

    // Return the new thread-local storage pointer.
    AndroidNceDirectLog("RestoreGuestContext end returning tpidr");
    return tpidr;
}

void ArmNce::SaveGuestContext(GuestContext* guest_ctx, void* raw_context) {
    // Retrieve the host context.
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    AndroidNceDirectLogf("SaveGuestContext begin guest=%p pc=0x%llx sp=0x%llx", guest_ctx,
                         static_cast<unsigned long long>(host_ctx.pc),
                         static_cast<unsigned long long>(host_ctx.sp));

    // Retrieve the host floating point state.
    auto* fpctx = GetFloatingPointState(host_ctx);
    if (fpctx == nullptr) {
        AndroidNceDirectLog("SaveGuestContext fail missing FPSIMD");
        return;
    }

    // Save all guest registers except tpidr_el0.
    std::memcpy(guest_ctx->cpu_registers.data(), host_ctx.regs, sizeof(host_ctx.regs));
    std::memcpy(guest_ctx->vector_registers.data(), fpctx->vregs, sizeof(fpctx->vregs));
    guest_ctx->fpsr = fpctx->fpsr;
    guest_ctx->fpcr = fpctx->fpcr;
    guest_ctx->pstate = static_cast<u32>(host_ctx.pstate);
    guest_ctx->pc = host_ctx.pc;
    guest_ctx->sp = host_ctx.sp;

    // Restore stack pointer.
    host_ctx.sp = guest_ctx->host_ctx.host_sp;

    // Restore host callee-saved registers.
    std::memcpy(&host_ctx.regs[19], guest_ctx->host_ctx.host_saved_regs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_regs));
    std::memcpy(&fpctx->vregs[8], guest_ctx->host_ctx.host_saved_vregs.data(),
                sizeof(guest_ctx->host_ctx.host_saved_vregs));

    // Return from the call on exit by setting pc to x30.
    host_ctx.pc = guest_ctx->host_ctx.host_saved_regs[11];

    // Clear esr_el1 and return it.
    host_ctx.regs[0] = guest_ctx->esr_el1.exchange(0);
    AndroidNceDirectLogf("SaveGuestContext end host_pc=0x%llx halt=0x%llx", 
                         static_cast<unsigned long long>(host_ctx.pc),
                         static_cast<unsigned long long>(host_ctx.regs[0]));
}


GuestContext* ArmNce::ResolveGuestSignalContextAndRestoreHostTls(void* candidate_tpidr, int sig,
                                                                 void* info,
                                                                 void* raw_context) {
    const int tid = CurrentTidNoThrow();
    auto* si = static_cast<siginfo_t*>(info);
    auto& signal_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    AndroidNceDirectLogf(
        "ResolveGuestSignal begin sig=%d tid=%d candidate=%p fault=0x%llx pc=0x%llx sp=0x%llx",
        sig, tid, candidate_tpidr,
        static_cast<unsigned long long>(si ? reinterpret_cast<u64>(si->si_addr) : 0),
        static_cast<unsigned long long>(signal_ctx.pc),
        static_cast<unsigned long long>(signal_ctx.sp));
    auto* tpidr = FindActiveNceSignalThread(candidate_tpidr, tid);
    if (tpidr == nullptr) {
        // Do not dereference candidate_tpidr here: this function exists specifically to avoid a
        // second SIGSEGV inside the assembly signal handler when guest TPIDR_EL0 was clobbered.
        AndroidNceDirectLog("ResolveGuestSignal host/no-active-tpidr");
        return nullptr;
    }

    if (tpidr->magic != NativeExecutionParametersMagic || tpidr->native_context == nullptr) {
        AndroidNceDirectLogf("ResolveGuestSignal invalid tpidr=%p magic=0x%x native=%p", tpidr,
                             tpidr->magic, tpidr->native_context);
        return nullptr;
    }

    auto* guest_ctx = static_cast<GuestContext*>(tpidr->native_context);
    const u64 host_tpidr = reinterpret_cast<u64>(guest_ctx->host_ctx.host_tpidr_el0);
    if (host_tpidr != 0) {
        RestoreHostTpidrEl0(host_tpidr);
    }
    AndroidNceDirectLogf("ResolveGuestSignal restored host tls guest=%p host_tpidr=0x%llx",
                         guest_ctx, static_cast<unsigned long long>(host_tpidr));

    if (reinterpret_cast<uintptr_t>(candidate_tpidr) != reinterpret_cast<uintptr_t>(tpidr)) {
        const auto* si = static_cast<siginfo_t*>(info);
        const auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
        LOG_WARNING(Core_ARM,
                    "Android NCE recovered signal context by tid: sig={} tid={} candidate_tpidr={} "
                    "active_tpidr={} fault_addr={:#018X} pc={:#018X}",
                    sig, tid, candidate_tpidr, static_cast<void*>(tpidr),
                    si ? reinterpret_cast<u64>(si->si_addr) : 0, host_ctx.pc);
    }

    return guest_ctx;
}



bool ArmNce::HandleFailedGuestFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    auto* info = static_cast<siginfo_t*>(raw_info);

    // We can't handle the access, so determine why we crashed.
    const bool is_prefetch_abort = host_ctx.pc == reinterpret_cast<u64>(info->si_addr);
    LOG_ERROR(Core_ARM,
              "Android NCE unhandled guest fault: signal={} code={} fault_addr={:#018X} "
              "host_pc={:#018X} guest_pc={:#018X} sp={:#018X} prefetch={}",
              info->si_signo, info->si_code, reinterpret_cast<u64>(info->si_addr), host_ctx.pc,
              guest_ctx ? guest_ctx->pc : 0, host_ctx.sp, is_prefetch_abort);

    // For data aborts, skip the instruction and return to guest code.
    // This will allow games to continue in many scenarios where they would otherwise crash.
    if (!is_prefetch_abort) {
        LOG_WARNING(Core_ARM,
                    "Android NCE data abort fallback: advancing PC by 4 at host_pc={:#018X} "
                    "fault_addr={:#018X}",
                    host_ctx.pc, reinterpret_cast<u64>(info->si_addr));
        host_ctx.pc += 4;
        return true;
    }

    // This is a prefetch abort.
    guest_ctx->esr_el1.fetch_or(static_cast<u64>(CpuHaltReason::PrefetchAbort));

    // Forcibly mark the context as locked. We are still running.
    // We may race with SignalInterrupt here:
    // - If we lose the race, then SignalInterrupt will send us a signal we are masking,
    //   and it will do nothing when it is unmasked, as we have already left guest code.
    // - If we win the race, then SignalInterrupt will wait for us to unlock first.
    auto& thread_params = static_cast<Kernel::KThread*>(guest_ctx->parent->m_running_thread)->GetNativeExecutionParameters();
    thread_params.lock.store(SpinLockLocked);

    // Return to host.
    SaveGuestContext(guest_ctx, raw_context);
    return false;
}

bool ArmNce::HandleGuestAlignmentFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    auto* info = static_cast<siginfo_t*>(raw_info);
    AndroidNceDirectLogf(
        "HandleGuestAlignmentFault begin guest=%p fault=0x%llx pc=0x%llx sp=0x%llx",
        guest_ctx, static_cast<unsigned long long>(info ? reinterpret_cast<u64>(info->si_addr) : 0),
        static_cast<unsigned long long>(host_ctx.pc),
        static_cast<unsigned long long>(host_ctx.sp));
    auto* fpctx = GetFloatingPointState(host_ctx);
    if (fpctx == nullptr) {
        AndroidNceDirectLog("HandleGuestAlignmentFault missing FPSIMD");
        LOG_CRITICAL(Core_ARM, "Android NCE alignment fault without FPSIMD signal state");
        return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
    }
    auto* process = static_cast<Kernel::KThread*>(guest_ctx->parent->m_running_thread)->GetOwnerKProcess();
    auto& memory = process->GetCoreMemory();

    LOG_WARNING(Core_ARM, "Android NCE guest alignment fault: pc={:#018X} sp={:#018X}",
                host_ctx.pc, host_ctx.sp);

    // Match and execute one instruction in the interpreter, mirroring Eden/yuzu NCE.
    auto next_pc = MatchAndExecuteOneInstruction(memory, &host_ctx, fpctx);
    if (next_pc) {
        LOG_INFO(Core_ARM, "Android NCE alignment fallback handled: pc={:#018X} next_pc={:#018X}",
                 host_ctx.pc, *next_pc);
        host_ctx.pc = *next_pc;
        return true;
    }

    // We couldn't handle the access.
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

bool ArmNce::HandleGuestAccessFault(GuestContext* guest_ctx, void* raw_info, void* raw_context) {
    auto* info = static_cast<siginfo_t*>(raw_info);
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    AndroidNceDirectLogf(
        "HandleGuestAccessFault begin guest=%p fault=0x%llx pc=0x%llx sp=0x%llx code=%d",
        guest_ctx, static_cast<unsigned long long>(info ? reinterpret_cast<u64>(info->si_addr) : 0),
        static_cast<unsigned long long>(host_ctx.pc),
        static_cast<unsigned long long>(host_ctx.sp), info ? info->si_code : 0);

    // Try to handle an invalid access.
    // TODO: handle accesses which split a page?
    constexpr u64 PageBits = 12;
    constexpr u64 PageSize = 1ULL << PageBits;
    constexpr u64 PageMask = PageSize - 1;
    const u64 addr = reinterpret_cast<u64>(info->si_addr) & ~PageMask;

#if defined(__ANDROID__)
    // Android NCE direct-maps guest VA into the host address space.  The first executable
    // permission fault may happen on module tail pages / NCE patch trampolines before the
    // heavier Memory::InvalidateNCE path is safe to call from the guest signal context.
    //
    // If the fault is an instruction fetch from the same guest page, make that single page
    // executable directly and return to guest.  This mirrors the practical effect needed by
    // InvalidateNCE for SEGV_ACCERR prefetch faults, but avoids re-entering complex memory/log
    // machinery while the signal handler is still running on guest ucontext state.
    const bool is_exec_fault = info != nullptr && info->si_code == SEGV_ACCERR &&
                               reinterpret_cast<u64>(info->si_addr) == host_ctx.pc;
    if (is_exec_fault) {
        const int prot = PROT_READ | PROT_EXEC;
        const int rc = mprotect(reinterpret_cast<void*>(addr), PageSize, prot);
        AndroidNceDirectLogf(
            "HandleGuestAccessFault direct mprotect exec page=0x%llx rc=%d errno=%d pc=0x%llx",
            static_cast<unsigned long long>(addr), rc, rc == 0 ? 0 : errno,
            static_cast<unsigned long long>(host_ctx.pc));
        if (rc == 0) {
            __builtin___clear_cache(reinterpret_cast<char*>(addr),
                                    reinterpret_cast<char*>(addr + PageSize));
            const u32 inst = *reinterpret_cast<const u32*>(host_ctx.pc);
            AndroidNceDirectLogf(
                "HandleGuestAccessFault direct exec instruction pc=0x%llx inst=0x%08x",
                static_cast<unsigned long long>(host_ctx.pc), inst);
            return true;
        }
    }
#endif

    auto* process = static_cast<Kernel::KThread*>(guest_ctx->parent->m_running_thread)->GetOwnerKProcess();
    auto& memory = process->GetCoreMemory();
    AndroidNceDirectLogf(
        "HandleGuestAccessFault before concrete InvalidateNCE page=0x%llx process=%p memory=%p",
        static_cast<unsigned long long>(addr), static_cast<void*>(process), static_cast<void*>(&memory));
    if (memory.InvalidateNCE(addr, PageSize)) {
        AndroidNceDirectLog("HandleGuestAccessFault handled by InvalidateNCE");
        if (ConsumeNceDispatchLogBudget()) {
            LOG_INFO(Core_ARM,
                     "Android NCE guest access fault handled by InvalidateNCE: fault_addr={:#018X} "
                     "page={:#018X}",
                     reinterpret_cast<u64>(info->si_addr), addr);
        }
        return true;
    }

    LOG_WARNING(Core_ARM,
                "Android NCE guest access fault not handled by InvalidateNCE: fault_addr={:#018X} "
                "page={:#018X}",
                reinterpret_cast<u64>(info->si_addr), addr);
    AndroidNceDirectLog("HandleGuestAccessFault not handled, entering failed fallback");

    // We couldn't handle the access.
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

void ArmNce::HandleHostAlignmentFault(int sig, void* raw_info, void* raw_context) {
    LOG_ERROR(Core_ARM, "Android NCE host alignment fault: sig={} addr={:#018X}", sig,
              reinterpret_cast<u64>(static_cast<siginfo_t*>(raw_info)->si_addr));
    if (g_have_orig_bus_action) {
        ForwardSignalToOriginalAction(sig, static_cast<siginfo_t*>(raw_info), raw_context,
                                      g_orig_bus_action);
    } else {
        struct sigaction default_action {};
        default_action.sa_handler = SIG_DFL;
        LibcSigAction(sig, &default_action, nullptr);
        syscall(SYS_tkill, gettid(), sig);
    }
}

void ArmNce::HandleHostAccessFault(int sig, void* raw_info, void* raw_context) {
    auto& host_ctx = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    LOG_ERROR(Core_ARM,
              "Android NCE host access fault: sig={} code={} addr={:#018X} pc={:#018X} sp={:#018X}",
              sig, static_cast<siginfo_t*>(raw_info)->si_code,
              reinterpret_cast<u64>(static_cast<siginfo_t*>(raw_info)->si_addr), host_ctx.pc,
              host_ctx.sp);
    if (g_have_orig_segv_action) {
        ForwardSignalToOriginalAction(sig, static_cast<siginfo_t*>(raw_info), raw_context,
                                      g_orig_segv_action);
    } else {
        struct sigaction default_action {};
        default_action.sa_handler = SIG_DFL;
        LibcSigAction(sig, &default_action, nullptr);
        syscall(SYS_tkill, gettid(), sig);
    }
}

void ArmNce::LockThread(IKernelThread* thread) {
    auto* thread_params = &static_cast<Kernel::KThread*>(thread)->GetNativeExecutionParameters();
    const bool log_verbose = UseNceDirectTrace() && ConsumeNceDispatchLogBudget();
    if (log_verbose) {
        LOG_INFO(Core_ARM,
                 "Android NCE LockThread: this={} core={} tid={} thread={} params={} magic={:#010X} "
                 "lock={} native_context={} is_running={}",
                 static_cast<void*>(this), m_core_index, gettid(), static_cast<void*>(thread),
                 static_cast<void*>(thread_params), thread_params->magic,
                 thread_params->lock.load(std::memory_order_relaxed), thread_params->native_context,
                 thread_params->is_running);
    }
    LockThreadParameters(thread_params);
    if (log_verbose) {
        LOG_INFO(Core_ARM, "Android NCE LockThread acquired: this={} core={} params={}",
                 static_cast<void*>(this), m_core_index, static_cast<void*>(thread_params));
    }
}

void ArmNce::UnlockThread(IKernelThread* thread) {
    auto* thread_params = &static_cast<Kernel::KThread*>(thread)->GetNativeExecutionParameters();
    if (UseNceDirectTrace() && ConsumeNceDispatchLogBudget()) {
        LOG_INFO(Core_ARM,
                 "Android NCE UnlockThread: this={} core={} tid={} thread={} params={} native_context={} "
                 "is_running={}",
                 static_cast<void*>(this), m_core_index, gettid(), static_cast<void*>(thread),
                 static_cast<void*>(thread_params), thread_params->native_context,
                 thread_params->is_running);
    }
    // Match Eden/yuzu NCE lifecycle more closely: any path that unlocks the native thread
    // parameters also publishes the latest TLS values back to the persistent guest context and
    // clears the transient native_context pointer. RunThread normally does this too, but keeping
    // UnlockThread self-contained avoids stale TLS/native_context after external interrupts or
    // scheduler exits on Android.
    m_guest_ctx.tpidr_el0 = thread_params->tpidr_el0;
    m_guest_ctx.tpidrro_el0 = thread_params->tpidrro_el0;
    thread_params->native_context = nullptr;
    UnlockThreadParameters(thread_params);
}

CpuHaltReason ArmNce::RunThread(IKernelThread* thread) {
    const bool log_verbose_entry = UseNceDirectTrace() && ConsumeNceDispatchLogBudget();
    if (log_verbose_entry) {
        LOG_INFO(Core_ARM,
                 "Android NCE RunThread begin: this={} core={} old_tid={} caller_tid={} pc={:#018X}",
                 static_cast<void*>(this), m_core_index, m_thread_id, gettid(), m_guest_ctx.pc);
    }

    // ReturnToRunCodeByExceptionLevelChange uses tkill to signal the currently-running host
    // thread into the NCE signal trampoline. Initialize() can be called from a setup thread on
    // Android, so refresh this on every RunThread before entering guest code.
    m_thread_id = gettid();
    EnsureNceSignalStackForCurrentThread("run-thread");

    // Check if we're already interrupted.
    // If we are, we can just return immediately.
    const u64 raw_hr = m_guest_ctx.esr_el1.exchange(0);
    CpuHaltReason hr = static_cast<CpuHaltReason>(raw_hr);
    if (raw_hr != 0) {
        if (UseNceDirectTrace() && ConsumeNceDispatchLogBudget()) {
            LOG_INFO(Core_ARM,
                     "Android NCE RunThread early halt: this={} core={} tid={} halt={:#x}",
                     static_cast<void*>(this), m_core_index, m_thread_id, raw_hr);
        }
        return hr;
    }

    // Get the thread context.
    auto* k_thread = static_cast<Kernel::KThread*>(thread);
    auto* thread_params = &k_thread->GetNativeExecutionParameters();
    auto* process = k_thread->GetOwnerKProcess();

    if (UseNceNoRunProbe()) {
        LOG_ERROR(Core_ARM,
                  "Android NCE RunThread no-run probe active: this={} core={} tid={} params={} "
                  "magic={:#010X} pc={:#018X} sp={:#018X}",
                  static_cast<void*>(this), m_core_index, m_thread_id,
                  static_cast<void*>(thread_params), thread_params->magic, m_guest_ctx.pc,
                  m_guest_ctx.sp);
        return CpuHaltReason::BreakLoop;
    }

    // Assign current members. Follow Eden's Android NCE pattern: cache TLS values before
    // publishing native_context/is_running, then use release/acquire fences around guest entry.
    // This makes TLS/native_context visibility deterministic across signal handlers and guest
    // core threads on Android.
    const u64 tpidr_el0_cache = m_guest_ctx.tpidr_el0;
    const u64 tpidrro_el0_cache = m_guest_ctx.tpidrro_el0;
    m_running_thread = thread;
    m_guest_ctx.parent = this;
    thread_params->native_context = &m_guest_ctx;
    thread_params->tpidr_el0 = tpidr_el0_cache;
    thread_params->tpidrro_el0 = tpidrro_el0_cache;
    std::atomic_thread_fence(std::memory_order_release);
    thread_params->is_running = true;
    RegisterActiveNceSignalThread(m_thread_id, thread_params);

    // Reinstall SIGSEGV/SIGBUS handlers immediately before guest entry. Mobile Vulkan drivers
    // sometimes replace process-wide signal handlers during initialization; without this, the
    // first guest fault exits the whole app with a bare "signal 11" and no NCE diagnostics.
    //
    // Keep this after native_context/is_running are set: a signal delivered during/just after
    // handler installation must see a valid NativeExecutionParameters block.
    AndroidNceDirectLog("RunThread before InstallNceSignalHandlers");
    if (UseNceControlSignalsOnlyProbe()) {
        LOG_ERROR(Core_ARM,
                  "Android NCE control-only probe active: installing SIGUSR2/SIGURG only; "
                  "SIGSEGV/SIGBUS fault handlers skipped");
        InstallNceControlSignalHandlers();
    } else {
        InstallNceSignalHandlers(true);
    }
    AndroidNceDirectLog("RunThread after InstallNceSignalHandlers");

#if defined(__ANDROID__)
    // PhysicalCore::LoadContext can race slightly with RunThread on the Android integration path.
    // When direct NCE mappings work, the guest may execute immediately; wait briefly for TPIDRRO
    // (TLS) to be installed instead of entering with a partially populated context.
    for (int i = 0; i < 50 && m_guest_ctx.tpidrro_el0 == 0; ++i) {
        usleep(1000);
    }
    AndroidNceDirectLogf("RunThread context ready check pc=0x%llx sp=0x%llx tpidrro=0x%llx",
                         static_cast<unsigned long long>(m_guest_ctx.pc),
                         static_cast<unsigned long long>(m_guest_ctx.sp),
                         static_cast<unsigned long long>(m_guest_ctx.tpidrro_el0));
#endif

    if (log_verbose_entry) {
        LOG_INFO(Core_ARM,
                 "Android NCE RunThread entering guest: tid={} thread_params={} native_context={} "
                 "magic={:#010X} pc={:#018X} sp={:#018X}",
                 m_thread_id, static_cast<void*>(thread_params), thread_params->native_context,
                 thread_params->magic, m_guest_ctx.pc, m_guest_ctx.sp);
    }

    // TODO: finding and creating the post handler needs to be locked
    // to deal with dynamic loading of NROs.
    const auto& post_handlers = process->GetPostHandlers();
    const auto dispatch_pc = m_guest_ctx.pc;
    const auto post_it = post_handlers.find(dispatch_pc);
    const bool post_hit = post_it != post_handlers.end();
    const u64 post_target = post_hit ? post_it->second : 0;
    const bool should_log_dispatch = UseNceDirectTrace() && ConsumeNceDispatchLogBudget();
    if (should_log_dispatch) {
        LOG_INFO(Core_ARM,
                 "Android NCE dispatch: tid={} pc={:#018X} sp={:#018X} post_handlers={} "
                 "post_hit={} trampoline={:#018X} svc_before={:#x}",
                 m_thread_id, dispatch_pc, m_guest_ctx.sp, post_handlers.size(), post_hit,
                 post_target, m_guest_ctx.svc);
    }
    if (post_hit) {
        hr = ReturnToRunCodeByTrampoline(thread_params, &m_guest_ctx, post_target);
    } else {
        g_pending_return_signal_tpidr = thread_params;
        hr = ReturnToRunCodeByExceptionLevelChange(m_thread_id, thread_params);
        g_pending_return_signal_tpidr = nullptr;
    }

    // Unload members.
    // The thread does not change, so we can persist the old reference.
    std::atomic_thread_fence(std::memory_order_acquire);
    const u64 final_tpidr_el0 = thread_params->tpidr_el0;
    const u64 final_tpidrro_el0 = thread_params->tpidrro_el0;
    m_running_thread = nullptr;
    m_guest_ctx.tpidr_el0 = final_tpidr_el0;
    m_guest_ctx.tpidrro_el0 = final_tpidrro_el0;
    UnregisterActiveNceSignalThread(m_thread_id, thread_params);
    thread_params->native_context = nullptr;
    thread_params->is_running = false;

    // Return the halt reason.
    if (should_log_dispatch) {
        LOG_INFO(Core_ARM,
                 "Android NCE RunThread returned: tid={} halt={:#x} dispatch_pc={:#018X} "
                 "pc={:#018X} sp={:#018X} svc={:#x} post_hit={} trampoline={:#018X}",
                 m_thread_id, static_cast<u64>(hr), dispatch_pc, m_guest_ctx.pc, m_guest_ctx.sp,
                 m_guest_ctx.svc, post_hit, post_target);
    }
    return hr;
}

CpuHaltReason ArmNce::StepThread(IKernelThread* thread) {
    return CpuHaltReason::StepThread;
}

u32 ArmNce::GetSvcNumber() const {
    return m_guest_ctx.svc;
}

void ArmNce::GetSvcArguments(uint64_t (&args)[8]) const {
    for (size_t i = 0; i < 8; i++) {
        args[i] = m_guest_ctx.cpu_registers[i];
    }
}

void ArmNce::SetSvcArguments(const uint64_t (&args)[8]) {
    for (size_t i = 0; i < 8; i++) {
        m_guest_ctx.cpu_registers[i] = args[i];
    }
}

ArmNce::ArmNce(ICoreSystem& system, bool uses_wall_clock, IKernelProcess& process, std::size_t core_index)
    : m_system{system}, m_process{process}, m_core_index{core_index} {
    m_guest_ctx.system = &m_system;
    LOG_INFO(Core_ARM, "Android NCE ArmNce constructed: this={} core={} uses_wall_clock={}",
             static_cast<void*>(this), core_index, uses_wall_clock);
}

ArmNce::~ArmNce() = default;

void ArmNce::Initialize() {
    const bool log_verbose = UseNceDirectTrace() && ConsumeNceDispatchLogBudget();
    if (log_verbose) {
        LOG_INFO(Core_ARM, "Android NCE Initialize begin: this={} old_tid={}",
                 static_cast<void*>(this), m_thread_id);
    }
    if (UseNceNoInitProbe()) {
        LOG_ERROR(Core_ARM, "Android NCE Initialize no-init probe active: this={} old_tid={}",
                  static_cast<void*>(this), m_thread_id);
        return;
    }
    if (m_thread_id == -1) {
        m_thread_id = gettid();
    }

    // Do not install an alternate signal stack from Initialize() on Android. In this port
    // Initialize() can run on setup/loader threads while Vulkan and ART are also installing
    // process-wide signal machinery; on RMX3700 this crashed immediately after sigaltstack().
    // Install the per-thread alt stack in RunThread(), immediately before guest entry.
    if (log_verbose) {
        LOG_INFO(Core_ARM, "Android NCE Initialize end: this={} tid={} altstack=deferred",
                 static_cast<void*>(this), m_thread_id);
    }

    // Android: delay even the NCE-owned control signals until RunThread(). On RMX3700 the
    // process can die immediately after Initialize begin when installing signal actions from
    // the loader/setup thread. RunThread() calls InstallNceSignalHandlers(true) immediately
    // before tkill/guest entry, which is late enough and has valid thread_params/native_context.

    // Set up fault signals.
    // Android note: do not install process-wide SIGSEGV/SIGBUS handlers during CPU
    // construction. ART/Vulkan/driver threads may raise or chain host signals before the
    // guest is actually running. Install/re-arm NCE handlers immediately before guest entry
    // in RunThread() instead, when tpidr/native_context are valid for the running CPU thread.
}

void ArmNce::SetTpidrroEl0(u64 value) {
    if (UseNceDirectTrace() && ConsumeNceDispatchLogBudget()) {
        LOG_INFO(Core_ARM, "Android NCE SetTpidrroEl0: this={} core={} tid={} value={:#018X}",
                 static_cast<void*>(this), m_core_index, gettid(), value);
    }
    m_guest_ctx.tpidrro_el0 = value;
}

void ArmNce::GetContext(CpuThreadContext& ctx) const {
    for (size_t i = 0; i < 29; i++) {
        ctx.r[i] = m_guest_ctx.cpu_registers[i];
    }
    ctx.fp = m_guest_ctx.cpu_registers[29];
    ctx.lr = m_guest_ctx.cpu_registers[30];
    ctx.sp = m_guest_ctx.sp;
    ctx.pc = m_guest_ctx.pc;
    ctx.pstate = m_guest_ctx.pstate;
    std::memcpy(ctx.v, m_guest_ctx.vector_registers.data(), sizeof(ctx.v));
    ctx.fpcr = m_guest_ctx.fpcr;
    ctx.fpsr = m_guest_ctx.fpsr;
    ctx.tpidr = m_guest_ctx.tpidr_el0;
    if (UseNceDirectTrace() && ConsumeNceDispatchLogBudget()) {
        LOG_INFO(Core_ARM,
                 "Android NCE GetContext: this={} core={} tid={} pc={:#018X} sp={:#018X} tpidr={:#018X}",
                 static_cast<const void*>(this), m_core_index, gettid(), ctx.pc, ctx.sp, ctx.tpidr);
    }
}

void ArmNce::SetContext(const CpuThreadContext& ctx) {
    const bool log_verbose = UseNceDirectTrace() && ConsumeNceDispatchLogBudget();
    if (log_verbose) {
        LOG_INFO(Core_ARM,
                 "Android NCE SetContext begin: this={} core={} tid={} pc={:#018X} sp={:#018X} tpidr={:#018X}",
                 static_cast<void*>(this), m_core_index, gettid(), ctx.pc, ctx.sp, ctx.tpidr);
    }
    for (size_t i = 0; i < 29; i++) {
        m_guest_ctx.cpu_registers[i] = ctx.r[i];
    }
    m_guest_ctx.cpu_registers[29] = ctx.fp;
    m_guest_ctx.cpu_registers[30] = ctx.lr;
    m_guest_ctx.sp = ctx.sp;
    m_guest_ctx.pc = ctx.pc;
    m_guest_ctx.pstate = ctx.pstate;
    std::memcpy(m_guest_ctx.vector_registers.data(), ctx.v, sizeof(ctx.v));
    m_guest_ctx.fpcr = ctx.fpcr;
    m_guest_ctx.fpsr = ctx.fpsr;
    m_guest_ctx.tpidr_el0 = ctx.tpidr;
    if (log_verbose) {
        LOG_INFO(Core_ARM, "Android NCE SetContext end: this={} core={} pc={:#018X}",
                 static_cast<void*>(this), m_core_index, m_guest_ctx.pc);
    }
}

void ArmNce::SignalInterrupt(IKernelThread* thread) {
    const bool log_verbose = UseNceDirectTrace() && ConsumeNceDispatchLogBudget();
    if (log_verbose) {
        LOG_INFO(Core_ARM,
                 "Android NCE SignalInterrupt begin: this={} core={} target_tid={} caller_tid={}",
                 static_cast<void*>(this), m_core_index, m_thread_id, gettid());
    }
    // Add break loop condition.
    m_guest_ctx.esr_el1.fetch_or(static_cast<u64>(CpuHaltReason::BreakLoop));

    // Lock the thread context.
    auto* params = &static_cast<Kernel::KThread*>(thread)->GetNativeExecutionParameters();
    LockThreadParameters(params);

    if (params->is_running) {
        // We should signal to the running thread.
        // The running thread will unlock the thread context.
        if (log_verbose) {
            LOG_INFO(Core_ARM, "Android NCE SignalInterrupt tkill: this={} core={} target_tid={}",
                     static_cast<void*>(this), m_core_index, m_thread_id);
        }
        syscall(SYS_tkill, m_thread_id, BreakFromRunCodeSignal);
    } else {
        // If the thread is no longer running, we have nothing to do.
        if (log_verbose) {
            LOG_INFO(Core_ARM, "Android NCE SignalInterrupt unlock-not-running: this={} core={}",
                     static_cast<void*>(this), m_core_index);
        }
        UnlockThreadParameters(params);
    }
}

void ArmNce::ClearInstructionCache() {
    // TODO: This is not possible to implement correctly on Linux because
    // we do not have any access to ic iallu.

    // Require accesses to complete.
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void ArmNce::InvalidateCacheRange(u64 addr, std::size_t size) {
    this->ClearInstructionCache();
}


void ArmNce::SetWatchpointArray(const CpuDebugWatchpoint* watchpoints, uint32_t count) {
    if (UseNceDirectTrace() && ConsumeNceDispatchLogBudget()) {
        LOG_INFO(Core_ARM,
                 "Android NCE SetWatchpointArray: this={} core={} tid={} watchpoints={} count={}",
                 static_cast<void*>(this), m_core_index, gettid(),
                 static_cast<const void*>(watchpoints), count);
    }
}

void ArmNce::Release() {
    delete this;
}
} // namespace Core













