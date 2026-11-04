// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Common::DebugDocument {
inline std::string Environment(const char* name) {
#ifdef _WIN32
    char buffer[4096]{}; const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return n && n < sizeof(buffer) ? std::string(buffer, n) : std::string{};
#else
    const char* value = std::getenv(name); return value ? value : "";
#endif
}
inline bool Enabled() {
    return !Environment("NXEMU_NATIVE_SHADER_DIAG_DIR").empty() && Environment("NXEMU_DEBUG_DETAIL") == "1";
}
inline bool PublicationEnabled(const std::string& category) {
    if (Environment("NXEMU_NATIVE_SHADER_DIAG_DIR").empty()) return false;
    if (Enabled()) return true;
    // Explicit narrow traces can publish while all other detail collectors stay off.
    return (category == "handle-lifecycle" && Environment("NXEMU_HANDLE_TRACE") == "1") ||
           (category == "shader-rejections" && Environment("NXEMU_SHADER_REJECTION_TRACE") == "1") ||
           (category == "shader-progress" && Environment("NXEMU_SHADER_PROGRESS") == "1") ||
           (category == "pipeline-capture" && Environment("NXEMU_PIPELINE_FIXTURES") == "1");
}
inline std::string Quote(const std::string& input) {
    std::ostringstream s; s << '"';
    for (const unsigned char c : input) {
        if (c == '"' || c == '\\') s << '\\' << char(c);
        else if (c < 0x20) s << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c) << std::dec;
        else s << char(c);
    }
    s << '"'; return s.str();
}
inline std::uint64_t EpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
// Read on the GPU owner thread; no extra emulator-state lock or queued GPU work.
// Publication is opt-in and bounded. A contended diagnostic writer drops a sample.
inline void Publish(const std::string& category, const std::string& payload, bool history = true) noexcept {
    if (!PublicationEnabled(category) || payload.size() > 128 * 1024 || category.size() > 48 ||
        category.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) return;
    try {
        static std::mutex writer_mutex;
        std::unique_lock lock(writer_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        struct Counter { std::uint64_t sequence{}; };
        static std::unordered_map<std::string, Counter> counters;
        if (!counters.contains(category) && counters.size() >= 512) return;
        auto& counter = counters[category];
        const auto sequence = ++counter.sequence;
        const auto root = std::filesystem::u8path(Environment("NXEMU_NATIVE_SHADER_DIAG_DIR"));
        std::error_code ec; std::filesystem::create_directories(root, ec);
        if (ec) return;
        const auto run = Environment("NXEMU_DEBUG_RUN_ID");
        const auto generation = Environment("NXEMU_DEBUG_GENERATION");
        #ifdef _WIN32
        const auto tid = GetCurrentThreadId();
#else
        const std::uint64_t tid = 0;
#endif
        const std::string text = "{\"schema\":1,\"thread_id\":" + std::to_string(tid) + ",\"category\":" + Quote(category) +
            ",\"sequence\":" + std::to_string(sequence) + ",\"epoch_ms\":" + std::to_string(EpochMs()) +
            ",\"run_id\":" + Quote(run) + ",\"module_generation\":" + Quote(generation) +
            ",\"data\":" + payload + "}\n";
        const auto write = [&](const std::filesystem::path& destination) {
            const auto temporary = std::filesystem::path(destination.string() + ".tmp");
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << text; if (!out) return; }
#ifdef _WIN32
            MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
            std::filesystem::rename(temporary, destination, ec);
#endif
        };
        write(root / (category + ".json"));
        if (history) {
            const auto ring = root / "events"; std::filesystem::create_directories(ring, ec);
            if (!ec) write(ring / (category + "-" + std::to_string(sequence % 128) + ".json"));
        }
    } catch (...) { /* Diagnostic IO never aborts the emulator. */ }
}
} // namespace Common::DebugDocument
