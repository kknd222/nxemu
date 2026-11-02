// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bitset>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_shader_recompiler/exception.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/memory_manager.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include "yuzu_video_core/textures/decoders.h"

namespace Tegra::Engines {

KeplerCompute::KeplerCompute(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_}, upload_state{memory_manager, regs.upload} {
    execution_mask.reset();
    execution_mask[KEPLER_COMPUTE_REG_INDEX(exec_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(data_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(launch)] = true;
}

KeplerCompute::~KeplerCompute() = default;

void KeplerCompute::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer);
}


void KeplerCompute::SetNativeTuring(bool enabled, Maxwell3D* graphics) {
    native_mode = enabled;
    native_graphics = graphics;
    execution_mask.reset();
    if (enabled) {
        // Native MME parameter boundaries and side effects must bypass the plain register sink.
        execution_mask.set();
    } else {
        execution_mask[KEPLER_COMPUTE_REG_INDEX(exec_upload)] = true;
        execution_mask[KEPLER_COMPUTE_REG_INDEX(data_upload)] = true;
        execution_mask[KEPLER_COMPUTE_REG_INDEX(launch)] = true;
    }
}

bool KeplerCompute::ProcessNativeMethod(u32 method, u32 argument, bool is_last_call) {
    if (method >= 0x1000) {
        LOG_ERROR(HW_GPU, "NX2_COMPUTE register out of range: {:#x}", method);
        return true;
    }
    if (method >= 0xe00) {
        if (!native_executing_macro) {
            if (method & 1) {
                LOG_ERROR(HW_GPU, "NX2_COMPUTE macro starts on argument method: {:#x}", method);
                return true;
            }
            native_executing_macro = method;
        }
        if (native_parameters.size() >= 0x10000) {
            native_parameters.clear(); native_executing_macro.reset();
            LOG_ERROR(HW_GPU, "NX2_COMPUTE macro parameter limit exceeded");
            return true;
        }
        native_parameters.push_back(argument);
        if (!is_last_call) return true;
        const auto positions = native_compute_uploaded || !native_graphics
            ? std::span<const u32>{native_positions} : native_graphics->NativeMacroPositions();
        const auto code = native_compute_uploaded || !native_graphics
            ? std::span<const u32>{native_code} : native_graphics->NativeMacroCode();
        const u32 macro = *native_executing_macro;
        const u32 start = positions[((macro - 0xe00) / 2) % positions.size()];
        native_executing_macro.reset();
        if (native_graphics) native_interpreter.active_dram = native_graphics->NativeMacroDataRam();
        const auto result = native_interpreter.Execute(code, start, native_parameters,
            [&](u32 address) { return address < native_registers.size() ? native_registers[address] : 0U; },
            [&](u32 address, u32 value) {
                if (address >= 0xe00) return false;
                CallMethod(address, value, true);
                return true;
            });
        ++native_calls;
        if (!result.success) ++native_failures;
        if (native_calls <= 16 || (!result.success && native_failures <= 16))
            LOG_INFO(HW_GPU, "NX2_COMPUTE macro={:#x} start={:#x} consumed={}/{} success={} pc={:#x} error={}",
                     macro, start, result.consumed, native_parameters.size(), result.success, result.pc, result.error);
        native_parameters.clear();
        return true;
    }
    native_registers[method] = argument;
    switch (method) {
    case 0x45:
        native_compute_uploaded = true;
        native_write_offset = argument < 0x1000 ? argument * 3 : static_cast<u32>(native_code.size());
        return true;
    case 0x46:
        if (native_write_offset < native_code.size()) native_code[native_write_offset++] = argument;
        else LOG_ERROR(HW_GPU, "NX2_COMPUTE macro code upload out of bounds");
        return true;
    case 0x47: native_compute_uploaded = true; native_bind_pointer = argument; return true;
    case 0x48:
        if (native_bind_pointer < native_positions.size()) native_positions[native_bind_pointer++] = argument;
        else LOG_ERROR(HW_GPU, "NX2_COMPUTE macro bind out of bounds");
        return true;
    case 0xae: case 0xaf: case 0xb0: case 0x107: {
        CaptureNativeCompute(method, argument);
        ++native_qmd_requests;
        GPUVAddr address = GPUVAddr{native_registers[0xad]} << 8;
        u32 control = argument;
        if (method == 0x107) {
            address = ((GPUVAddr{native_registers[0xc6]} << 32) | native_registers[0xc7]) << 8;
            if (!memory_manager.IsFullyMappedRange(address, 256)) {
                LOG_ERROR(HW_GPU, "NX2_QMD unmapped inline destination={:#x}", address);
                return true;
            }
            memory_manager.WriteBlock(address, native_registers.data() + 0xc8, 256);
            control = 3;
        } else if (method == 0xae || (method == 0xaf && !(argument & 2)) ||
                   (method == 0xb0 && ((argument & 15) == 0 || (argument & 15) == 1))) {
            return true;
        }
        const auto error = native_qmd.Signal(address, control,
            [&](u64 source, NativeQmdScheduler::Descriptor& descriptor) {
                if (!memory_manager.IsFullyMappedRange(source, sizeof(descriptor))) return false;
                memory_manager.ReadBlock(source, descriptor.data(), sizeof(descriptor));
                return true;
            },
            [&](u64 destination, u32 payload, u32 flags) {
                if (!memory_manager.IsFullyMappedRange(destination, sizeof(payload))) return false;
                if (flags & (1U << 26)) {
                    rasterizer->SignalFence([this, destination, payload] {
                        memory_manager.Write<u32>(destination, memory_manager.Read<u32>(destination) + payload);
                    });
                } else if (flags & (1U << 19)) {
                    rasterizer->SignalFence([this, destination, payload] {
                        memory_manager.Write<u32>(destination, (std::max)(memory_manager.Read<u32>(destination), payload));
                    });
                } else {
                    rasterizer->Query(destination, VideoCommon::QueryType::Payload,
                        VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
                }
                return true;
            }, [this](const auto& descriptor) { return DispatchNativeCompute(descriptor); });
        if (native_qmd_requests <= 32 || (!error.empty() && native_qmd_requests % 1024 == 0))
            LOG_INFO(HW_GPU, "NX2_QMD method={:#x} control={:#x} address={:#x} result={} requests={}",
                     method, control, address, error.empty() ? "scheduled_or_completed" : error, native_qmd_requests);
        return true;
    }
    case 0x6c3: {
        const GPUVAddr address = (GPUVAddr{native_registers[0x6c0] & 0x1ffffff} << 32) | native_registers[0x6c1];
        const u32 operation = argument & 3;
        if ((operation == 0 || operation == 2) && !(argument & 8) &&
            memory_manager.IsFullyMappedRange(address, (argument & (1U << 28)) ? 4 : 16)) {
            rasterizer->Query(address, VideoCommon::QueryType::Payload,
                (argument & (1U << 28)) ? VideoCommon::QueryPropertiesFlags::IsAFence
                                       : VideoCommon::QueryPropertiesFlags::HasTimeout,
                native_registers[0x6c2], 0);
        } else LOG_ERROR(HW_GPU, "NX2_COMPUTE unsupported semaphore address={:#x} control={:#x}", address, argument);
        return true;
    }
    default:
        if (method < Regs::NUM_REGS) regs.reg_array[method] = argument;
        return method != KEPLER_COMPUTE_REG_INDEX(exec_upload) && method != KEPLER_COMPUTE_REG_INDEX(data_upload);
    }
}


bool KeplerCompute::DispatchNativeCompute(const NativeQmdScheduler::Descriptor& qmd) {
    launch_description = {};
    launch_description.grid_dim_x.Assign(qmd[12]);
    launch_description.grid_dim_y.Assign(qmd[13] & 0xffff);
    launch_description.grid_dim_z.Assign(qmd[14] & 0xffff);
    launch_description.shared_alloc.Assign(qmd[17] & 0x3ffff);
    launch_description.block_dim_x.Assign(qmd[18] >> 16);
    launch_description.block_dim_y.Assign(qmd[19] & 0xffff);
    launch_description.block_dim_z.Assign(qmd[19] >> 16);
    launch_description.const_buffer_enable_mask.Assign(qmd[20] & 0xff);
    if ((qmd[23] & 0xffffff) > 0xfffff || (qmd[50] & 0xffffff) > 0xfffff) return false;
    launch_description.local_pos_alloc.Assign(qmd[23] & 0xffffff);
    launch_description.local_crs_alloc.Assign(qmd[50] & 0xffffff);
    regs.code_loc.address_low = qmd[48];
    regs.code_loc.address_high = qmd[49] & 0x1ffff;
    for (u32 slot = 0; slot < NumConstBuffers; ++slot) {
        const u32 high = qmd[33 + slot * 2];
        if (high & 0x1ff00) return false;
        auto& buffer = launch_description.const_buffer_config[slot];
        buffer.address_low = qmd[32 + slot * 2];
        buffer.address_high.Assign(high & 0xff);
        buffer.size.Assign(((high >> 19) & 0x1fff) << 4);
    }
    if (!launch_description.block_dim_x || !launch_description.block_dim_y || !launch_description.block_dim_z) return false;
    try {
        const bool submitted = rasterizer->DispatchNativeCompute();
        static u32 logged{};
        if (logged++ < 16)
            LOG_INFO(HW_GPU, "NX2_COMPUTE_DISPATCH program={:#x} grid={}/{}/{} submitted={}",
                     regs.code_loc.Address(), qmd[12], qmd[13] & 0xffff, qmd[14] & 0xffff, submitted);
        return submitted;
    } catch (const Shader::Exception& error) {
        static u32 rejected{};
        if (rejected++ < 16) LOG_ERROR(HW_GPU, "NX2_COMPUTE_REJECT program={:#x} reason={}", regs.code_loc.Address(), error.what());
        return false;
    }
}

void KeplerCompute::CaptureNativeCompute(u32 method, u32 argument) {
    const char* directory = std::getenv("NXEMU_NATIVE_SHADER_DIAG_DIR");
    if (!directory || (method != 0x107 && native_qmd_requests != 0)) return;
    static std::atomic<u32> captures{};
    static std::atomic<bool> synchronizationCaptured{};
    u32 index{};
    if (method != 0x107) {
        if (synchronizationCaptured.exchange(true, std::memory_order_relaxed)) return;
    } else {
        index = 1 + captures.fetch_add(1, std::memory_order_relaxed);
        if (index > 4) return;
    }
    const auto root = std::filesystem::u8path(directory);
    std::error_code ec; std::filesystem::create_directories(root, ec);
    if (ec) return;
    const auto name = fmt::format("compute-{}", index);
    const auto write = [&](std::string_view suffix, const void* data, size_t size) {
        std::ofstream file(root / (name + std::string(suffix)), std::ios::binary);
        file.write(static_cast<const char*>(data), size);
    };
    std::array<u32, 64> qmd{};
    GPUVAddr address = GPUVAddr{native_registers[0xad]} << 8;
    bool mapped{};
    if (method == 0x107) {
        std::copy_n(native_registers.begin() + 0xc8, qmd.size(), qmd.begin());
        address = ((GPUVAddr{native_registers[0xc6]} << 32) | native_registers[0xc7]) << 8;
        mapped = true; // Descriptor is inline, destination mapping is checked separately.
    } else if (memory_manager.IsFullyMappedRange(address, sizeof(qmd))) {
        memory_manager.ReadBlock(address, qmd.data(), sizeof(qmd)); mapped = true;
    }
    write("-registers.bin", native_registers.data(), sizeof(native_registers));
    if (mapped) write("-qmd.bin", qmd.data(), sizeof(qmd));
    const auto code = native_compute_uploaded || !native_graphics
        ? std::span<const u32>{native_code} : native_graphics->NativeMacroCode();
    write("-mme.bin", code.data(), code.size_bytes());
    write("-dram.bin", native_interpreter.active_dram->data(), sizeof(TuringMacroInterpreter::DataRam));
    const GPUVAddr program = (GPUVAddr{qmd[49] & 0x1ffff} << 32) | qmd[48];
    if (mapped && memory_manager.IsFullyMappedRange(program, 65536)) {
        std::array<u8, 65536> shader{};
        memory_manager.ReadBlock(program, shader.data(), shader.size());
        write("-shader.bin", shader.data(), shader.size());
    }
    std::ofstream meta(root / (name + ".txt"));
    meta << fmt::format("method={:#x} argument={:#x} address={:#x} mapped={} program={:#x} version={:#x} grid={}/{}/{} header_size=0\n",
        method, argument, address, mapped, program, qmd[18] & 0xff, qmd[12], qmd[13] & 0xffff, qmd[14] & 0xffff);
    LOG_INFO(HW_GPU, "NX2_COMPUTE_CAPTURE file={}", (root / (name + ".txt")).string());
}

void KeplerCompute::ConsumeSinkImpl() {
    for (auto [method, value] : method_sink) {
        if (native_mode) {
            CallMethod(method, value, true);
        } else if (method < Regs::NUM_REGS) {
            regs.reg_array[method] = value;
        } else {
            LOG_ERROR(HW_GPU, "Compute sink register out of range: {:#x}", method);
        }
    }
    method_sink.clear();
}

void KeplerCompute::CallMethod(u32 method, u32 method_argument, bool is_last_call) {
    if (native_mode && ProcessNativeMethod(method, method_argument, is_last_call)) return;
    if (method >= Regs::NUM_REGS) {
        LOG_ERROR(HW_GPU, "Compute register out of range: {:#x}", method);
        return;
    }

    regs.reg_array[method] = method_argument;

    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(exec_upload): {
        UploadInfo info{.upload_address = upload_address,
                        .exec_address = upload_state.ExecTargetAddress(),
                        .copy_size = upload_state.GetUploadSize(),
                        .was_dirty = upload_dirty};
        uploads.push_back(info);
        upload_state.ProcessExec(regs.exec_upload.linear != 0);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(data_upload): {
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(method_argument, is_last_call);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(launch): {
        const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();

        for (auto& data : uploads) {
            const GPUVAddr offset = data.exec_address - launch_desc_loc;
            if (offset / sizeof(u32) == LAUNCH_REG_INDEX(grid_dim_x)) {
                const bool source_dirty =
                    memory_manager.IsMemoryDirty(data.upload_address, data.copy_size);
                if (data.was_dirty || source_dirty) {
                    indirect_compute = {data.upload_address};
                }
            }
        }
        uploads.clear();
        ProcessLaunch();
        indirect_compute = std::nullopt;
        break;
    }
    default:
        break;
    }
}

void KeplerCompute::CallMultiMethod(u32 method, const u32* base_start, u32 amount,
                                    u32 methods_pending) {
    if (native_mode) {
        for (u32 i = 0; i < amount; ++i)
            CallMethod(method, base_start[i], methods_pending - i <= 1);
        return;
    }
    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(data_upload):
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(base_start, amount);
        return;
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

void KeplerCompute::ProcessLaunch() {
    const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();
    memory_manager.ReadBlockUnsafe(launch_desc_loc, &launch_description,
                                   LaunchParams::NUM_LAUNCH_PARAMETERS * sizeof(u32));
    rasterizer->DispatchCompute();
}

Texture::TICEntry KeplerCompute::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tic.Address() + tic_index * sizeof(Texture::TICEntry)};

    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry KeplerCompute::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tsc.Address() + tsc_index * sizeof(Texture::TSCEntry)};

    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

} // namespace Tegra::Engines
