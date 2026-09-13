// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "debug.h"
#include "core/hle/kernel/k_process.h"

namespace Core {

namespace {
void SymbolicateBacktrace(Kernel::KProcess * process, std::vector<BacktraceEntry> & out)
{
    // Android PoC/NCE path needs backtraces for diagnostics, but full symbolication
    // is not wired in this split nxemu build yet. Keep raw PC/LR frame addresses
    // instead of spamming "Unimplemented code!" assertions every time a guest thread
    // hits a debug/prefetch/data-abort suspend path.
    (void)process;
    (void)out;
}

std::vector<BacktraceEntry> GetAArch64Backtrace(Kernel::KProcess * process, const CpuThreadContext & ctx)
{
    std::vector<BacktraceEntry> out;
    auto & memory = process->GetMemory();
    auto pc = ctx.pc, lr = ctx.lr, fp = ctx.fp;

    out.push_back({"", 0, pc, 0, ""});

    // fp (= x29) points to the previous frame record.
    // Frame records are two words long:
    // fp+0 : pointer to previous frame record
    // fp+8 : value of lr for frame
    for (size_t i = 0; i < 256; i++)
    {
        out.push_back({"", 0, lr, 0, ""});
        if (!fp || (fp % 4 != 0) || !memory.IsValidVirtualAddressRange(fp, 16))
        {
            break;
        }
        lr = memory.Read64(fp + 8);
        fp = memory.Read64(fp);
    }

    SymbolicateBacktrace(process, out);

    return out;
}

std::vector<BacktraceEntry> GetAArch32Backtrace(Kernel::KProcess * process, const CpuThreadContext & ctx)
{
    std::vector<BacktraceEntry> out;
    auto & memory = process->GetMemory();
    auto pc = ctx.pc, lr = ctx.lr, fp = ctx.fp;

    out.push_back({"", 0, pc, 0, ""});

    // fp (= r11) points to the last frame record.
    // Frame records are two words long:
    // fp+0 : pointer to previous frame record
    // fp+4 : value of lr for frame
    for (size_t i = 0; i < 256; i++)
    {
        out.push_back({"", 0, lr, 0, ""});
        if (!fp || (fp % 4 != 0) || !memory.IsValidVirtualAddressRange(fp, 8))
        {
            break;
        }
        lr = memory.Read32(fp + 4);
        fp = memory.Read32(fp);
    }

    SymbolicateBacktrace(process, out);

    return out;
}

} // namespace

std::vector<BacktraceEntry> GetBacktraceFromContext(Kernel::KProcess * process, const CpuThreadContext & ctx)
{
    if (process->Is64Bit())
    {
        return GetAArch64Backtrace(process, ctx);
    }
    else
    {
        return GetAArch32Backtrace(process, ctx);
    }
}

} // namespace Core
