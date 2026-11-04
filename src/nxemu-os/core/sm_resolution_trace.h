// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include "yuzu_common/debug_document.h"
#ifdef CreateEvent
#undef CreateEvent
#endif
namespace Core::SmResolutionTrace {
struct Row { uint64_t seq{},epoch{},tid{}; std::string name,phase; uint32_t result{}; };
inline std::mutex mutex;
inline std::array<Row,64> rows;
inline uint64_t sequence{},last_publish{},collector_ns{};
inline void FlushLocked(uint64_t now) {
 if(!sequence || now-last_publish<500)return;last_publish=now;
 std::ostringstream s;s << "{\"source\":\"SM registration/resolution and chat IPC handlers\",\"history_only\":true,\"limit\":64,\"total\":" << sequence
  << ",\"overwritten\":" << (sequence>64?sequence-64:0) << ",\"collector_ns_previous\":" << collector_ns << ",\"events\":[";
 bool first=true;for(uint64_t i=sequence>64?sequence-64:0;i<sequence;++i) {auto& r=rows[i%64];if(!first)s << ',';first=false;
  s << "{\"sequence\":" << r.seq << ",\"event_epoch_ms\":" << r.epoch << ",\"age_ms\":" << (now>=r.epoch?now-r.epoch:0)
   << ",\"guest_tid\":" << r.tid << ",\"name\":" << Common::DebugDocument::Quote(r.name) << ",\"phase\":" << Common::DebugDocument::Quote(r.phase) << ",\"result\":" << r.result << '}';}
 s << "]}";Common::DebugDocument::Publish("sm-resolution",s.str(),false);
}
inline void Record(uint64_t tid,const std::string& name,const std::string& phase,uint32_t result) noexcept {
 if(!Common::DebugDocument::Enabled())return;
 try {auto start=std::chrono::steady_clock::now();std::lock_guard lock(mutex);auto now=Common::DebugDocument::EpochMs();
  rows[sequence%64]={sequence+1,now,tid,name.substr(0,64),phase.substr(0,48),result};++sequence;FlushLocked(now);
  collector_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();}catch(...){}
}
inline void Flush() noexcept {if(!Common::DebugDocument::Enabled())return;try {std::unique_lock lock(mutex,std::try_to_lock);if(lock.owns_lock())FlushLocked(Common::DebugDocument::EpochMs());}catch(...) {}}
}
