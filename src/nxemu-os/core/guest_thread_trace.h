// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/guest_debug_trace.h"
#include "yuzu_common/hardware_properties.h"

namespace Core::GuestThreadTrace {
struct State {
    uint64_t tid{}, pid{}, owner_tid{}, wait_generation{}, physical_affinity_mask{}, last_scheduled_tick{};
    uint32_t guest_core_count{};
    int32_t active_core{};
    int32_t state{}, raw_state{}, priority{}, base_priority{}, current_core{}, wait_reason{}, type{};
};
struct Context {
    uint64_t tid{},pc{},sp{},sample_ms{},samples{};
    uint32_t core{},pstate{};
    std::array<uint64_t,31> registers{};
};
struct Registry {
    std::mutex mutex;
    std::array<State,128> states{};
    std::array<Context,128> contexts{};
    size_t state_count{};
    uint64_t state_ms{},omitted{},overflow{},publish_ms{},collector_ns{};
};
inline Registry registry;
inline bool Due(uint64_t& last) {
    if(!GuestDebugTrace::Enabled())return false;
    const auto now=GuestDebugTrace::Now();if(now-last<500)return false;last=now;return true;
}
inline void States(const std::array<State,128>& states,size_t count,uint64_t omitted,uint64_t capture_ns) {
    std::unique_lock lock(registry.mutex,std::try_to_lock);if(!lock.owns_lock())return;
    registry.states=states;registry.state_count=count;registry.omitted=omitted;
    registry.state_ms=GuestDebugTrace::Now();registry.collector_ns+=capture_ns;
}
inline void Saved(Context context) {
    if(!GuestDebugTrace::Enabled())return;
    std::scoped_lock lock(registry.mutex);
    for(auto& row:registry.contexts)if(row.tid==context.tid || !row.tid){
        const auto now=GuestDebugTrace::Now();if(row.tid && now-row.sample_ms<500)return;
        context.sample_ms=now;context.samples=row.samples+1;row=context;return;
    }
    ++registry.overflow;
}
inline void Publish() {
    if(!GuestDebugTrace::Enabled())return;
    const auto start=std::chrono::steady_clock::now();std::string payload;
    {
        std::unique_lock lock(registry.mutex,std::try_to_lock);if(!lock.owns_lock())return;
        if(!Due(registry.publish_ms))return;const auto now=GuestDebugTrace::Now();
        std::ostringstream s;s<<"{\"scope\":\"scheduler_locked_states_and_own_core_saved_contexts\",\"state_age_ms\":";
        if(registry.state_ms)s<<now-registry.state_ms;else s<<"null";
        s<<",\"scheduler_core_capacity\":"<<::Hardware::NUM_CPU_CORES;
        s<<",\"omitted_threads\":"<<registry.omitted<<",\"context_overflow\":"<<registry.overflow
         <<",\"collector_ns\":"<<registry.collector_ns<<",\"threads\":[";
        for(size_t i=0;i<registry.state_count;++i){
            const auto& t=registry.states[i];s<<(i?",":"")<<"{\"tid\":"<<t.tid<<",\"pid\":"<<t.pid
             <<",\"state\":"<<t.state<<",\"raw_state\":"<<t.raw_state<<",\"priority\":"<<t.priority
             <<",\"base_priority\":"<<t.base_priority<<",\"current_core\":"<<t.current_core
             <<",\"wait_reason\":"<<t.wait_reason<<",\"type\":"<<t.type<<",\"owner_tid\":"<<t.owner_tid
             <<",\"wait_generation\":"<<t.wait_generation
             <<",\"guest_core_count\":"<<t.guest_core_count<<",\"active_core\":"<<t.active_core
             <<",\"physical_affinity_mask\":"<<t.physical_affinity_mask
             <<",\"last_scheduled_tick\":"<<t.last_scheduled_tick<<"}";
        }
        s<<"],\"saved_contexts\":[";bool first=true;uint64_t contexts_omitted{};
        for(const auto& c:registry.contexts)if(c.tid){
            std::ostringstream row;row<<"{\"tid\":"<<c.tid<<",\"source\":\"own_core_after_context_save\",\"core\":"<<c.core
             <<",\"sample_age_ms\":"<<now-c.sample_ms<<",\"samples\":"<<c.samples<<",\"pc\":"<<c.pc
             <<",\"sp\":"<<c.sp<<",\"pstate\":"<<c.pstate<<",\"registers\":[";
            for(size_t i=0;i<c.registers.size();++i)row<<(i?",":"")<<c.registers[i];row<<"]}";
            const auto text=row.str();if(static_cast<size_t>(s.tellp())+text.size()>120000){++contexts_omitted;continue;}
            s<<(first?"":",")<<text;first=false;
        }
        s<<"],\"contexts_omitted\":"<<contexts_omitted<<"}";payload=s.str();
    }
    Common::DebugDocument::Publish("guest-threads",payload,false);
    std::scoped_lock lock(registry.mutex);registry.collector_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
}
}
