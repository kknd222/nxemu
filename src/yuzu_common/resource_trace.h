// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include "yuzu_common/debug_document.h"
namespace Common::ResourceTrace {
// A DLL-local bounded event history, not a continuous trace or guest memory snapshot.
template <bool Loader> struct Registry {
    struct Row { uint64_t seq{}, epoch{}, title{}, size{}; int storage{}, type{}, status{}; const char* stage{}; bool present{}; };
    inline static std::mutex mutex;
    inline static std::array<Row,64> rows{};
    inline static uint64_t sequence{}, last_publish{}, elapsed_ns{}, publications{};
    static void FlushLocked(uint64_t now) {
        if (now-last_publish<500 || !sequence) return;
        last_publish=now;
        std::ostringstream s;
        s << "{\"source\":\"" << (Loader?"loader catalog/NCA/RomFS calls":"FSP OpenDataStorageByDataId branches")
          << "\",\"history_only\":true,\"coverage\":\"open resolution only; content reads/decryption completeness not established\",\"limit\":64,\"total\":" << sequence
          << ",\"overwritten\":" << (sequence>64?sequence-64:0) << ",\"capture_publish_ns_previous\":" << elapsed_ns
          << ",\"publications_previous\":" << publications << ",\"events\":[";
        bool first=true;
        for(uint64_t i=sequence>64?sequence-64:0;i<sequence;++i) {
            auto& r=rows[i%64];if(!first)s << ',';first=false;
            s << "{\"sequence\":" << r.seq << ",\"event_epoch_ms\":" << r.epoch << ",\"age_ms\":" << (now>=r.epoch?now-r.epoch:0)
              << ",\"title_id\":" << r.title << ",\"storage\":" << r.storage << ",\"content_type\":" << r.type
              << ",\"stage\":" << DebugDocument::Quote(r.stage) << ",\"status\":" << r.status
              << ",\"present\":" << (r.present?"true":"false") << ",\"bytes\":" << r.size << '}';
        }
        s << "]}";DebugDocument::Publish(Loader?"loader-resources":"system-resources",s.str(),false);++publications;
    }
    static void Record(uint64_t title,int storage,int type,const char* stage,int status,bool present,uint64_t size=0) noexcept {
        if(!DebugDocument::Enabled())return;
        try {
            auto start=std::chrono::steady_clock::now();std::lock_guard lock(mutex);auto now=DebugDocument::EpochMs();
            rows[sequence%64]={sequence+1,now,title,size,storage,type,status,stage,present};++sequence;FlushLocked(now);
            elapsed_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        } catch(...) {}
    }
    static void Flush() noexcept {
        if(!DebugDocument::Enabled())return;
        try {auto start=std::chrono::steady_clock::now();std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock())return;
            FlushLocked(DebugDocument::EpochMs());elapsed_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();}catch(...){}
    }
};
using Loader=Registry<true>;using System=Registry<false>;
}
