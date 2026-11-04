// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace Kernel::HandleDebugTrace {
// Only copied scalars and static type-name bytes. Never retains/opens emulation objects.
struct Event {
    uint64_t sequence{}, observed_ms{}, table{}, object{}, guest_tid{}, process_id{};
    uint32_t handle{}, index{}, requested_generation{}, stored_generation{}, table_size{}, result{};
    std::array<char,48> operation{}, reason{}, actual_type{}, expected_type{};
};
bool Enabled();
inline uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
template<size_t N> inline void Text(std::array<char,N>& out,const char* in) {
    if(in) { const auto count=std::min(N-1,std::strlen(in)); std::copy_n(in,count,out.begin()); }
}
struct Registry {
    std::mutex mutex;
    std::array<Event,256> recent{};
    std::array<Event,64> failures{};
    Event first_failure{}, first_send_sync_failure{};
    uint64_t sequence{}, cursor{}, failure_cursor{}, window_ms{}, window_count{}, publish_ms{};
    std::atomic<uint64_t> lock_dropped{}, rate_dropped{}, capture_ns{}, publish_ns{};
};
inline Registry registry;
inline void Record(uint64_t table,const char* operation,uint32_t handle,uint32_t stored_generation,
                   uint32_t table_size,uint64_t object,const char* actual_type,const char* expected_type,
                   const char* reason,uint32_t result=0,uint64_t tid=0,uint64_t pid=0) {
    if(!Enabled())return;
    const auto start=std::chrono::steady_clock::now();
    std::unique_lock lock(registry.mutex,std::try_to_lock);
    if(!lock.owns_lock()){++registry.lock_dropped;return;}
    const auto now=Now();
    if(now-registry.window_ms>=1000){registry.window_ms=now;registry.window_count=0;}
    // Successful traffic is capped; failures keep their own bounded history and first latch.
    if(result==0 && ++registry.window_count>4096){++registry.rate_dropped;return;}
    Event e{};e.sequence=++registry.sequence;e.observed_ms=now;e.table=table;e.handle=handle;
    e.index=handle&0x7fff;e.requested_generation=(handle>>15)&0x7fff;
    e.stored_generation=stored_generation;e.table_size=table_size;e.object=object;
    e.result=result;e.guest_tid=tid;e.process_id=pid;
    Text(e.operation,operation);Text(e.reason,reason);Text(e.actual_type,actual_type);Text(e.expected_type,expected_type);
    registry.recent[registry.cursor++%registry.recent.size()]=e;
    if(result){registry.failures[registry.failure_cursor++%registry.failures.size()]=e;
        if(!registry.first_failure.sequence)registry.first_failure=e;
        if(operation && std::strcmp(operation,"SendSyncRequest")==0 && !registry.first_send_sync_failure.sequence)
            registry.first_send_sync_failure=e;}
    registry.capture_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count();
}
void Publish();
} // namespace Kernel::HandleDebugTrace
