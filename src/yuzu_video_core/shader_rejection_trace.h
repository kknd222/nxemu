// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Pure standard-C++ collector; platform/document dependencies stay in the owner .cpp.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

namespace VideoCommon::ShaderRejectionTrace {
bool Enabled();
void Publish();
inline uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct ConstantBuffer { uint64_t address{}; uint32_t size{}; bool enabled{}; };
struct DispatchContext {
    uint64_t token{}, program{};
    std::array<uint32_t,3> grid{}, block{};
    uint32_t cbuf_mask{};
    std::array<ConstantBuffer,8> cbufs{};
};
inline thread_local const DispatchContext* current_dispatch{};
struct ScopedDispatch {
    const DispatchContext* previous{};
    explicit ScopedDispatch(const DispatchContext& context) : previous{current_dispatch} {
        if (Enabled()) current_dispatch=&context;
    }
    ~ScopedDispatch() { current_dispatch=previous; }
};
struct Neighbor { uint64_t offset{}; std::array<uint32_t,4> words{}; };
struct Event {
    uint64_t sequence{}, observed_ms{}, program{}, offset{}, native_bytes{};
    uint32_t stage{}, header_size{}, opcode{}, neighbor_count{};
    bool lowering{};
    std::array<uint32_t,4> words{};
    std::array<char,192> reason{};
    bool reason_truncated{};
    std::array<Neighbor,9> neighbors{};
    DispatchContext dispatch{};
};
struct ReasonCount {
    bool used{}, lowering{};
    std::array<char,192> text{};
    uint64_t count{};
};
struct Registry {
    std::mutex mutex;
    std::array<Event,64> events{};
    std::array<ReasonCount,32> reasons{};
    Event first_lowering{}, first_dispatch{};
    uint64_t cursor{}, publish_ms{}, unclassified{};
    std::atomic<uint64_t> sequence{}, lowering_attempts{}, dispatch_exceptions{}, dispatch_attempts{},
        dispatch_submitted{}, dispatch_not_submitted{}, lock_dropped{}, capture_ns{}, publish_ns{};
};
inline Registry registry;
inline uint64_t NextDispatchToken() {
    return registry.dispatch_attempts.fetch_add(1,std::memory_order_relaxed)+1;
}
inline void Text(std::array<char,192>& out, std::string_view text, bool& truncated) {
    const size_t length=text.size();truncated=length>=out.size();
    std::copy_n(text.data(),std::min(length,out.size()-1),out.begin());
}
inline void Record(Event event, std::string_view reason) {
    if (!Enabled()) return;
    const auto started=std::chrono::steady_clock::now();
    if (event.lowering) ++registry.lowering_attempts; else ++registry.dispatch_exceptions;
    event.sequence=++registry.sequence; event.observed_ms=Now();
    Text(event.reason,reason,event.reason_truncated);
    if (current_dispatch && current_dispatch->program==event.program) event.dispatch=*current_dispatch;
    std::unique_lock lock(registry.mutex,std::try_to_lock);
    if (!lock.owns_lock()) { ++registry.lock_dropped; return; }
    registry.events[registry.cursor++%registry.events.size()]=event;
    auto& first=event.lowering?registry.first_lowering:registry.first_dispatch;
    if (!first.sequence) first=event;
    bool counted=false;
    for (auto& row:registry.reasons) {
        if (row.used && row.lowering==event.lowering && row.text==event.reason) {
            ++row.count;counted=true;break;
        }
    }
    if (!counted) for (auto& row:registry.reasons) if (!row.used) {
        row={true,event.lowering,event.reason,1};counted=true;break;
    }
    if (!counted) ++registry.unclassified;
    registry.capture_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-started).count();
}
inline void DispatchException(uint64_t program,const char* reason) {
    Event event{};event.program=program;event.stage=5;Record(event,reason);
}
inline void DispatchResult(bool submitted) {
    if (!Enabled()) return;
    if (submitted) ++registry.dispatch_submitted; else ++registry.dispatch_not_submitted;
}
} // namespace VideoCommon::ShaderRejectionTrace
