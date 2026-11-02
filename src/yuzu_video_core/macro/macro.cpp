// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <fstream>
#include <optional>
#include <span>

#include "yuzu_common/container_hash.h"

#include <fstream>
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/settings.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/macro/macro.h"
#include "yuzu_video_core/macro/macro_hle.h"
#include "yuzu_video_core/macro/macro_interpreter.h"

#if defined(_M_X64) || defined(ARCHITECTURE_x86_64)
#include "yuzu_video_core/macro/macro_jit_x64.h"
#endif

namespace Tegra {

static void Dump(u64 hash, std::span<const u32> code, bool decompiled = false) {
    const auto base_dir{Common::FS::GetYuzuPath(Common::FS::YuzuPath::DumpDir)};
    const auto macro_dir{base_dir / "macros"};
    if (!Common::FS::CreateDir(base_dir) || !Common::FS::CreateDir(macro_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create macro dump directories");
        return;
    }
    auto name{macro_dir / fmt::format("{:016x}.macro", hash)};

    if (decompiled) {
        auto new_name{macro_dir / fmt::format("decompiled_{:016x}.macro", hash)};
        if (Common::FS::Exists(name)) {
            (void)Common::FS::RenameFile(name, new_name);
            return;
        }
        name = new_name;
    }

    std::fstream macro_file(name, std::ios::out | std::ios::binary);
    if (!macro_file) {
        LOG_ERROR(Common_Filesystem, "Unable to open or create file at {}",
                  Common::FS::PathToUTF8String(name));
        return;
    }
    macro_file.write(reinterpret_cast<const char*>(code.data()), code.size_bytes());
}

MacroEngine::MacroEngine(Engines::Maxwell3D& maxwell3d_)
    : hle_macros{std::make_unique<Tegra::HLEMacro>(maxwell3d_)}, maxwell3d{maxwell3d_} {}

MacroEngine::~MacroEngine() = default;

void MacroEngine::AddCode(u32 method, u32 data) {
    if (native_turing) {
        if (native_write_offset >= native_code.size()) {
            LOG_ERROR(HW_GPU, "Native MME upload outside RAM at {:#x}", native_write_offset);
            return;
        }
        native_code[native_write_offset++] = data;
        return;
    }
    uploaded_macro_code[method].push_back(data);
}

void MacroEngine::ClearCode(u32 method) {
    if (native_turing) {
        native_write_offset = method < 0x1000 ? method * 3 : static_cast<u32>(native_code.size());
        return;
    }
    macro_cache.erase(method);
    uploaded_macro_code.erase(method);
}

void MacroEngine::Execute(u32 method, const std::vector<u32>& parameters) {
    if (native_turing) {
        maxwell3d.RefreshParameters();
        native_fifo.assign(parameters.begin(), parameters.end());
        const auto result = native_interpreter.Execute(native_code, method, native_fifo,
            [&](u32 address) { return address < Engines::Maxwell3D::Regs::NUM_REGS
                ? maxwell3d.GetRegisterValue(address) : 0U; },
            [&](u32 address, u32 value) {
                // Native MME must not recursively launch another macro.
                if (address >= 0xe00 ||
                    address >= Engines::Maxwell3D::Regs::NUM_REGS) return false;
                if (address == 0x157 || address == 0x158) {
                    auto& memory = maxwell3d.GetMemoryManager();
                    const GPUVAddr source = (GPUVAddr{maxwell3d.GetRegisterValue(0x154) & 0x1ffffff} << 32) |
                                             maxwell3d.GetRegisterValue(0x155);
                    const u32 ram = maxwell3d.GetRegisterValue(0x156);
                    const size_t bytes = size_t{value} * sizeof(u32);
                    if (value > 0x1000 || !memory.IsFullyMappedRange(source, bytes) ||
                        (address == 0x157 && (ram >= native_interpreter.active_dram->size() ||
                         value > native_interpreter.active_dram->size() - ram))) return false;
                    if (memory.IsMemoryDirty(source, bytes, VideoCommon::CacheType::BufferCache))
                        memory.FlushRegion(source, bytes, VideoCommon::CacheType::BufferCache);
                    for (u32 i = 0; i < value; ++i) {
                        const u32 word = memory.Read<u32>(source + u64{i} * sizeof(u32));
                        if (address == 0x158) native_fifo.push_back(word);
                        else (*native_interpreter.active_dram)[ram + i] = word;
                    }
                    return true;
                }
                maxwell3d.CallMethod(address, value, true);
                return true;
            });
        if (!result.success) LOG_ERROR(HW_GPU,
            "Native MME failed: start={:#x}, pc={:#x}, consumed={}/{}, error={}",
            method, result.pc, result.consumed, native_fifo.size(), result.error);
        return;
    }
    auto compiled_macro = macro_cache.find(method);
    if (compiled_macro != macro_cache.end()) {
        const auto& cache_info = compiled_macro->second;
        if (cache_info.has_hle_program) {
            cache_info.hle_program->Execute(parameters, method);
        } else {
            maxwell3d.RefreshParameters();
            cache_info.lle_program->Execute(parameters, method);
        }
    } else {
        // Macro not compiled, check if it's uploaded and if so, compile it
        std::optional<u32> mid_method;
        const auto macro_code = uploaded_macro_code.find(method);
        if (macro_code == uploaded_macro_code.end()) {
            for (const auto& [method_base, code] : uploaded_macro_code) {
                if (method >= method_base && (method - method_base) < code.size()) {
                    mid_method = method_base;
                    break;
                }
            }
            if (!mid_method.has_value()) {
                ASSERT_MSG(false, "Macro 0x{0:x} was not uploaded", method);
                return;
            }
        }
        auto& cache_info = macro_cache[method];

        if (!mid_method.has_value()) {
            cache_info.lle_program = Compile(macro_code->second);
            cache_info.hash = Common::HashValue(macro_code->second);
        } else {
            const auto& macro_cached = uploaded_macro_code[mid_method.value()];
            const auto rebased_method = method - mid_method.value();
            auto& code = uploaded_macro_code[method];
            code.resize(macro_cached.size() - rebased_method);
            std::memcpy(code.data(), macro_cached.data() + rebased_method,
                        code.size() * sizeof(u32));
            cache_info.hash = Common::HashValue(code);
            cache_info.lle_program = Compile(code);
        }

        auto hle_program = hle_macros->GetHLEProgram(cache_info.hash);
        if (!hle_program || Settings::values.disable_macro_hle) {
            maxwell3d.RefreshParameters();
            cache_info.lle_program->Execute(parameters, method);
        } else {
            cache_info.has_hle_program = true;
            cache_info.hle_program = std::move(hle_program);
            cache_info.hle_program->Execute(parameters, method);
        }

        if (Settings::values.dump_macros) {
            Dump(cache_info.hash, macro_code->second, cache_info.has_hle_program);
        }
    }
}

std::unique_ptr<MacroEngine> GetMacroEngine(Engines::Maxwell3D& maxwell3d) {
    if (Settings::values.disable_macro_jit) {
        return std::make_unique<MacroInterpreter>(maxwell3d);
    }
#if defined(_M_X64) || defined(ARCHITECTURE_x86_64)
    return std::make_unique<MacroJITx64>(maxwell3d);
#else
    return std::make_unique<MacroInterpreter>(maxwell3d);
#endif
}

} // namespace Tegra
