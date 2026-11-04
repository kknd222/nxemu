// SPDX-License-Identifier: GPL-3.0-or-later
// Calls the actual production lowerer on a bounded CPU-native case. No GPU execution.
#include "yuzu_shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>

static std::string Quote(std::string_view text) {
    std::string result = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') result += '\\';
        if (static_cast<unsigned char>(c) < 32) result += '?'; else result += c;
    }
    return result + '"';
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected one native fixture file");
        const std::filesystem::path path = argv[1];
        const auto bytes = std::filesystem::file_size(path);
        if (!bytes || bytes > 1048576 || bytes % sizeof(Shader::Ampere::Instruction))
            throw std::runtime_error("Native fixture empty, unaligned or over1MiB");
        std::vector<Shader::Ampere::Instruction> native(bytes / 16);
        std::ifstream input(path, std::ios::binary);
        input.read(reinterpret_cast<char*>(native.data()), bytes);
        if (!input || input.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("Native fixture changed while reading");
        const auto start = std::chrono::steady_clock::now();
        const auto result = Shader::Ampere::LowerToMaxwell(native);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        const char* mode = std::getenv("EDEN_NX2_TENSOR");
        std::cout << "{\"schema\":1,\"actual_production_lowerer\":true,\"source_bytes\":" << bytes
                  << ",\"lowered\":" << (result ? "true" : "false")
                  << ",\"error_offset\":" << result.error_offset << ",\"reason\":" << Quote(result.error)
                  << ",\"lowered_instruction_count\":" << result.instructions.size()
                  << ",\"source_instruction_offsets_count\":" << result.source_instruction_offsets.size()
                  << ",\"elapsed_ns\":" << elapsed << ",\"tensor_mode\":" << (mode ? Quote(mode) : "null")
                  << ",\"IR_executed\":false,\"SPIRV_executed\":false,\"GPU_executed\":false}\n";
        return 0; // A correctly reproduced rejection is an observation, not a tool failure.
    } catch (const std::exception& error) {
        std::cout << "{\"schema\":1,\"result\":\"probe_error\",\"reason\":" << Quote(error.what()) << "}\n";
        return 2;
    }
}
