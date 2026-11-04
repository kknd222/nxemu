// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Opt-in CPU-mirror input observation; never a GPU visibility or replay certificate.
#include "yuzu_video_core/native_shader_fixture.h"

namespace VideoCommon::NativeComputeInputs {
inline constexpr std::size_t MaxItems = 4, MaxCbufBytes = 256;
struct Context {
    std::uint64_t program{}, dispatch_program{}, dispatch_token{}, cbuf_address{};
    std::uint32_t cbuf_declared_size{};
    bool cbuf_enabled{};
};
struct Key {
    std::string run, generation;
    std::uint64_t program{}, token{};
};
struct Registry {
    std::mutex mutex;
    std::array<Key, MaxItems> keys;
    std::size_t reserved{}; // Module-load lifetime; deliberately never refunded/reset.
    std::atomic<std::uint64_t> attempts{}, duplicates{}, budget_drops{}, lock_drops{},
        invalid{}, unmapped{}, io_errors{}, exceptions{}, ready{}, reads{}, bytes{};
};
inline Registry registry;

inline bool ValidTarget(std::string_view target) {
    return target.size() == 16 && std::all_of(target.begin(), target.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
inline bool OrdinaryDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    return !ec && std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status);
}
inline std::string CountersJson() {
    std::ostringstream out;
    out << "{\"scope\":\"module_load_no_reload_refund\",\"max_items\":4,\"max_cbuf_bytes\":256"
        << ",\"attempts\":" << registry.attempts.load()
        << ",\"duplicates\":" << registry.duplicates.load()
        << ",\"budget_drops\":" << registry.budget_drops.load()
        << ",\"lock_drops\":" << registry.lock_drops.load()
        << ",\"invalid\":" << registry.invalid.load()
        << ",\"unmapped\":" << registry.unmapped.load()
        << ",\"io_errors\":" << registry.io_errors.load()
        << ",\"exceptions\":" << registry.exceptions.load()
        << ",\"ready\":" << registry.ready.load()
        << ",\"reads\":" << registry.reads.load()
        << ",\"bytes\":" << registry.bytes.load()
        << ",\"GPU_visibility_proven\":false}";
    return out.str();
}

// Mapped(address,size)->bool and Reader(address,span<uint8_t>)->void are injected.
// The caller MUST use a CPU-only no-flush reader (MemoryManager::ReadBlockUnsafe).
// A mapping check is not an atomic snapshot across concurrent guest writes/unmapping.
template <class Mapped, class Reader>
bool Capture(std::span<const std::uint8_t> native, const Context& context, bool enabled,
             std::string_view target_fnv16, Mapped&& mapped, Reader&& read) noexcept {
    namespace N = NativeShaderFixture;
    if (!enabled) return false;
    try {
        if (!N::Enabled() || !ValidTarget(target_fnv16) || native.empty() ||
            native.size() % 16 || native.size() > N::MaxBlobBytes) return false;
        if (N::Hex(N::Hash(native)) != target_fnv16) return false;
        ++registry.attempts;
        const auto run = N::RunId(), generation = N::Generation();
        std::size_t ordinal{};
        {
            std::unique_lock lock(registry.mutex, std::try_to_lock);
            if (!lock.owns_lock()) { ++registry.lock_drops; return false; }
            for (std::size_t i = 0; i < registry.reserved; ++i) {
                const auto& key = registry.keys[i];
                if (key.run == run && key.generation == generation &&
                    key.program == context.program && key.token == context.dispatch_token) {
                    ++registry.duplicates;
                    return false;
                }
            }
            if (registry.reserved == MaxItems) { ++registry.budget_drops; return false; }
            ordinal = registry.reserved++;
            // Store identity before any reader/mapping/file operation; all failures consume it.
            registry.keys[ordinal] = {run, generation, context.program, context.dispatch_token};
        }
        const std::size_t size = std::min<std::size_t>(context.cbuf_declared_size, MaxCbufBytes);
        if (run.empty() || generation.empty() || run.size() > 256 || generation.size() > 256 ||
            !context.program || context.program != context.dispatch_program ||
            !context.dispatch_token || !context.cbuf_enabled || !size || !context.cbuf_address ||
            size - 1 > UINT64_MAX - context.cbuf_address) {
            ++registry.invalid;
            return false;
        }
        const auto root = N::Root();
        if (!root.is_absolute() || !OrdinaryDirectory(root)) {
            ++registry.io_errors;
            return false;
        }
        const auto directory = root / "native-compute-inputs";
        std::error_code ec;
        std::filesystem::create_directory(directory, ec);
        if (ec || !OrdinaryDirectory(directory)) { ++registry.io_errors; return false; }
        if (!mapped(context.cbuf_address, size)) { ++registry.unmapped; return false; }
        std::array<std::uint8_t, MaxCbufBytes> bytes{};
        const auto payload = std::span<std::uint8_t>{bytes.data(), size};
        ++registry.reads;
        read(context.cbuf_address, payload);
        registry.bytes += size;
        const auto content = N::Hex(N::Hash(payload));
        const auto id = "compute-input-" + std::to_string(ordinal);
        const auto blob = id + "-cbuf0.bin";
        std::ostringstream metadata;
        metadata << "{\"schema\":1,\"status\":\"ready\",\"run_id\":" << N::Quote(run)
            << ",\"module_generation\":" << N::Quote(generation)
            << ",\"epoch_ms\":" << N::EpochMs() << ",\"program\":" << context.program
            << ",\"dispatch_program\":" << context.dispatch_program
            << ",\"dispatch_token\":" << context.dispatch_token
            << ",\"native_content_id\":" << N::Quote(target_fnv16)
            << ",\"hash_algorithm\":\"fnv1a64-nonauthenticating\",\"cbuf\":{\"slot\":0"
            << ",\"address\":" << context.cbuf_address
            << ",\"declared_size\":" << context.cbuf_declared_size
            << ",\"captured_size\":" << size << ",\"enabled\":true,\"captured_range_fully_mapped_at_check\":true"
            << ",\"blob\":" << N::Quote(blob) << ",\"content_id\":" << N::Quote(content) << "}"
            << ",\"read_mode\":\"CPU_MIRROR_NO_FLUSH\",\"GPU_visibility_proven\":false"
            << ",\"synchronization_added\":false,\"full_resource_replay\":false"
            << ",\"limitations\":[\"not_atomic_with_guest_write_or_unmap\",\"no_texture_or_ssbo_bytes\","
               "\"no_GPU_buffer_input_or_completion_proof\",\"native_bytes_referenced_not_duplicated\"]}\n";
        // Publish ready metadata only after the exact binary payload exists.
        if (!N::Write(directory / blob, payload) ||
            !N::Text(directory / (id + ".json"), metadata.str())) {
            ++registry.io_errors;
            return false;
        }
        ++registry.ready;
        return true;
    } catch (...) {
        ++registry.exceptions;
        return false; // Optional observation cannot change the original shader rejection.
    }
}
} // namespace VideoCommon::NativeComputeInputs
