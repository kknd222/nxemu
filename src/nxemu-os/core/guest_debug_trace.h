// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "yuzu_common/debug_document.h"

namespace Core::GuestDebugTrace {
// Observed HLE boundaries, not a live scheduler walk. No emulation-object pointers retained.
struct Event {
    uint64_t token{}, guest_tid{}, pc{}, started_ms{}, ended_ms{};
    std::string kind, port, phase;
    uint32_t command{}, dispatch_result{};
    bool tipc{};
    std::array<uint32_t,12> words{}, reply{};
    bool reply_published{}, published_tls_readable{};
    uint32_t published_word_count{};
    std::array<uint32_t,12> published_tls{};
    int64_t timeout_ns{};
    uint32_t object_count{};
    std::array<uint64_t,8> objects{};
    std::array<std::string,8> object_types{};
    bool cpu_context_valid{};
    std::array<uint64_t,31> registers{};
    uint32_t pstate{};
    // Bounded exploratory reads, not owner inference or an atomic cross-core snapshot.
    std::array<uint64_t,8> probe_addresses{}, probe_values{};
    std::array<bool,8> probe_readable{};
    uint64_t capture_ns{};
};
inline bool Enabled() {
    static const bool enabled=Common::DebugDocument::Enabled();
    return enabled;
}
inline uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Registry {
    std::mutex mutex;
    std::array<Event,128> active{}, recent{};
    std::array<Event,8> cpu{};
    std::array<Event,32> ipc_recent{};
    uint64_t ipc_cursor{};
    // First eight gsv115 boundaries survive high-frequency HID traffic.
    // Debug-detail only; this does not change any IPC reply or service semantics.
    std::array<Event,8> gsv115_queries{};
    uint64_t gsv115_query_count{}, gsv115_query_omitted{};
    std::array<Event,32> transport_recent{};
    uint64_t transport_cursor{};
    std::array<uint64_t,8> cpu_sample_ms{}, cpu_samples{};
    uint64_t next{}, cursor{}, overflow{}, completed{}, publish_ms{}, collector_ns{};
};
inline Registry registry;
inline bool CpuSampleDue(uint32_t core) {
    if (!Enabled() || core >= registry.cpu.size()) return false;
    thread_local std::array<uint64_t,8> local_sample_ms{};
    const auto now = Now();
    if (now - local_sample_ms[core] < 500) return false;
    local_sample_ms[core] = now;
    std::scoped_lock lock(registry.mutex);
    if (now - registry.cpu_sample_ms[core] < 500) return false;
    registry.cpu_sample_ms[core] = now;
    return true;
}
inline void CpuSample(uint32_t core, Event event) {
    if (!Enabled() || core >= registry.cpu.size()) return;
    std::scoped_lock lock(registry.mutex);
    event.token = ++registry.cpu_samples[core];
    event.started_ms = event.ended_ms = Now();
    registry.cpu[core] = std::move(event);
}
inline uint64_t Begin(Event event) {
    if(!Enabled())return 0;
    std::scoped_lock lock(registry.mutex);
    event.token=++registry.next;event.started_ms=Now();
    for(auto& slot:registry.active)if(!slot.token){slot=std::move(event);return slot.token;}
    ++registry.overflow;return 0;
}
inline void Dispatching(uint64_t token) {
    if(!token)return;
    std::scoped_lock lock(registry.mutex);
    for(auto& slot:registry.active)if(slot.token==token){slot.phase="dispatching";break;}
}
inline void Phase(uint64_t token,const char* phase) {
    if(!token)return;
    std::scoped_lock lock(registry.mutex);
    for(auto& slot:registry.active)if(slot.token==token){slot.phase=phase;break;}
}
inline void Command(uint64_t token,uint32_t command,bool tipc) {
    if(!token)return;
    std::scoped_lock lock(registry.mutex);
    for(auto& slot:registry.active)if(slot.token==token){slot.command=command;slot.tipc=tipc;break;}
}
inline void End(uint64_t token,const uint32_t* reply=nullptr,uint32_t result=0,const char* phase="returned") {
    if(!token)return;
    std::scoped_lock lock(registry.mutex);
    for(auto& slot:registry.active)if(slot.token==token){
        slot.phase=phase;slot.ended_ms=Now();slot.dispatch_result=result;
        if(reply)std::copy_n(reply,slot.reply.size(),slot.reply.begin());
        if(slot.kind=="ipc" && slot.port=="nx2:gsv" && slot.command==115) {
            if(registry.gsv115_query_count<registry.gsv115_queries.size())
                registry.gsv115_queries[registry.gsv115_query_count++]=slot;
            else ++registry.gsv115_query_omitted;
        }
        if(slot.kind=="ipc" || slot.kind=="ipc_reply_pending")
            registry.ipc_recent[registry.ipc_cursor++%registry.ipc_recent.size()]=slot;
        if(slot.kind=="ipc_transport")
            registry.transport_recent[registry.transport_cursor++%registry.transport_recent.size()]=slot;
        registry.recent[registry.cursor++%registry.recent.size()]=std::move(slot);
        slot={};++registry.completed;break;
    }
}
inline bool NeedsPublishedTls(uint64_t token) {
    if(!token)return false;
    std::scoped_lock lock(registry.mutex);
    for(const auto& e:registry.gsv115_queries)
        if(e.token==token)return !e.reply_published;
    return false;
}
inline void PublishedTls(uint64_t token,const uint32_t* built,uint32_t count,
                         const uint32_t* tls,bool readable) {
    if(!token)return;
    std::scoped_lock lock(registry.mutex);
    for(auto& e:registry.gsv115_queries)if(e.token==token && !e.reply_published) {
        e.reply_published=true;e.published_word_count=count;
        e.published_tls_readable=readable;e.reply.fill(0);
        if(built)std::copy_n(built,std::min<size_t>(count,e.reply.size()),e.reply.begin());
        if(readable && tls)std::copy_n(tls,e.published_tls.size(),e.published_tls.begin());
        break;
    }
}
inline std::string Json(const Event& e,uint64_t now) {
    std::ostringstream s;
    s<<"{\"token\":"<<e.token<<",\"kind\":"<<Common::DebugDocument::Quote(e.kind)
     <<",\"phase\":"<<Common::DebugDocument::Quote(e.phase)<<",\"guest_tid\":"<<e.guest_tid
     <<",\"saved_context_pc\":"<<e.pc<<",\"port\":"<<Common::DebugDocument::Quote(e.port)
     <<",\"command\":"<<e.command<<",\"tipc\":"<<(e.tipc?"true":"false")
     <<",\"duration_ms\":"<<((e.ended_ms?e.ended_ms:now)-e.started_ms)
     <<",\"dispatch_result\":"<<e.dispatch_result<<",\"request_words\":[";
    for(size_t i=0;i<e.words.size();++i)s<<(i?",":"")<<e.words[i];
    s<<"],\"reply_words\":[";
    for(size_t i=0;i<e.reply.size();++i)s<<(i?",":"")<<e.reply[i];
    s<<"],\"reply_published\":"<<(e.reply_published?"true":"false")
     <<",\"published_word_count\":"<<e.published_word_count
     <<",\"published_tls_readable\":"<<(e.published_tls_readable?"true":"false")
     <<",\"published_tls_words\":[";
    for(size_t i=0;i<e.published_tls.size();++i)s<<(i?",":"")<<e.published_tls[i];
    s<<"],\"timeout_ns\":"<<e.timeout_ns<<",\"object_count\":"<<e.object_count<<",\"objects\":[";
    for(size_t i=0;i<std::min<size_t>(e.object_count,e.objects.size());++i)
        s<<(i?",":"")<<"{\"address\":"<<e.objects[i]<<",\"type\":"<<Common::DebugDocument::Quote(e.object_types[i])<<"}";
    s<<"]";
    if(e.cpu_context_valid) {
        s<<",\"context_source\":\"executing_core_after_run_before_unlock\",\"capture_ns\":"<<e.capture_ns
         <<",\"pstate\":"<<e.pstate<<",\"registers\":[";
        for(size_t i=0;i<e.registers.size();++i)s<<(i?",":"")<<e.registers[i];
        s<<"],\"memory_probes\":[";
        for(size_t i=0;i<e.probe_addresses.size();++i) {
            s<<(i?",":"")<<"{\"address\":"<<e.probe_addresses[i]<<",\"readable\":"
             <<(e.probe_readable[i]?"true":"false")<<",\"value\":";
            if(e.probe_readable[i])s<<e.probe_values[i];else s<<"null";
            s<<"}";
        }
        s<<"]";
    }
    s<<"}";return s.str();
}
inline void Publish() {
    if(!Enabled())return;
    const auto start=std::chrono::steady_clock::now();
    std::string payload;
    {
        std::unique_lock lock(registry.mutex,std::try_to_lock);
        if(!lock.owns_lock())return;
        const auto now=Now();if(now-registry.publish_ms<500)return;registry.publish_ms=now;
        std::ostringstream s;
        s<<"{\"scope\":\"observed_hle_boundaries_not_live_scheduler\",\"completed\":"<<registry.completed
         <<",\"overflow\":"<<registry.overflow<<",\"collector_ns\":"<<registry.collector_ns;
        s<<",\"cpu_boundary_samples\":["; bool first=true;
        for(size_t core=0;core<registry.cpu.size();++core) if(registry.cpu[core].token) {
            s<<(first?"":",")<<"{\"core\":"<<core<<",\"sample_age_ms\":"
             <<now-registry.cpu[core].ended_ms<<",\"sample\":"<<Json(registry.cpu[core],now)<<"}";first=false;
        }
        s<<"],\"gsv115_query_first\":[";first=true;
        for(const auto& e:registry.gsv115_queries)if(e.token) {
            s<<(first?"":",")<<Json(e,now);first=false;
        }
        s<<"],\"gsv115_query_omitted\":"<<registry.gsv115_query_omitted
         <<",\"ipc_recent\":[";first=true;
        const auto ipc_count=std::min<uint64_t>(registry.ipc_cursor,registry.ipc_recent.size());
        for(uint64_t i=0;i<ipc_count;++i){
            s<<(first?"":",")<<Json(registry.ipc_recent[(registry.ipc_cursor-ipc_count+i)%registry.ipc_recent.size()],now);
            first=false;
        }
        s<<"],\"transport_recent\":[";first=true;
        const auto transport_count=std::min<uint64_t>(registry.transport_cursor,registry.transport_recent.size());
        for(uint64_t i=0;i<transport_count;++i){
            s<<(first?"":",")<<Json(registry.transport_recent[(registry.transport_cursor-transport_count+i)%registry.transport_recent.size()],now);
            first=false;
        }
        s<<"],\"active\":[";first=true;
        uint64_t active_omitted=0;
        // Bound serialized payload too, not just the in-memory ring. Never emit truncated JSON.
        for(const auto& e:registry.active)if(e.token){
            const auto row=Json(e,now);
            if(static_cast<size_t>(s.tellp())+row.size()>116000){++active_omitted;continue;}
            s<<(first?"":",")<<row;first=false;
        }
        s<<"],\"recent\":[";first=true;
        const auto count=std::min<uint64_t>(registry.cursor,registry.recent.size());
        size_t bytes=static_cast<size_t>(s.tellp());
        std::vector<std::string> selected;
        for(uint64_t n=0;n<count;++n){
            const auto row=Json(registry.recent[(registry.cursor-1-n)%registry.recent.size()],now);
            if(bytes+row.size()+1>120000)break;
            selected.push_back(row);bytes+=row.size()+1;
        }
        for(auto i=selected.rbegin();i!=selected.rend();++i){s<<(first?"":",")<<*i;first=false;}
        s<<"],\"active_omitted\":"<<active_omitted<<",\"recent_omitted\":"<<count-selected.size()<<"}";
        payload=s.str();
    }
    Common::DebugDocument::Publish("guest-activity",payload,false);
    std::scoped_lock lock(registry.mutex);
    registry.collector_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
}
} // namespace Core::GuestDebugTrace
