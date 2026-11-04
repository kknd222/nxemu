// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/hle/kernel/handle_debug_trace.h"
#include <sstream>
#include <string>
#include "yuzu_common/debug_document.h"
namespace Kernel::HandleDebugTrace {
bool Enabled() {
    static const bool enabled = [] {
        const auto value = Common::DebugDocument::Environment("NXEMU_HANDLE_TRACE");
        return value.empty() ? Common::DebugDocument::Enabled() : value == "1";
    }();
    return enabled;
}
void Json(std::ostringstream& s,const Event& e,uint64_t now) {
    using Common::DebugDocument::Quote;
    s<<"{\"sequence\":"<<e.sequence<<",\"age_ms\":"<<(now-e.observed_ms)
     <<",\"table_id\":"<<e.table<<",\"object_id\":"<<e.object<<",\"handle\":"<<e.handle
     <<",\"index\":"<<e.index<<",\"requested_generation\":"<<e.requested_generation
     <<",\"stored_generation\":"<<e.stored_generation<<",\"table_size\":"<<e.table_size
     <<",\"result\":"<<e.result<<",\"guest_tid\":"<<e.guest_tid<<",\"process_id\":"<<e.process_id
     <<",\"operation\":"<<Quote(e.operation.data())<<",\"reason\":"<<Quote(e.reason.data())
     <<",\"actual_type\":"<<Quote(e.actual_type.data())<<",\"expected_type\":"<<Quote(e.expected_type.data())<<"}";
}
void Publish() {
    if(!Enabled() || !Common::DebugDocument::PublicationEnabled("handle-lifecycle"))return;
    const auto start=std::chrono::steady_clock::now();std::string payload;
    { std::unique_lock lock(registry.mutex,std::try_to_lock);if(!lock.owns_lock())return;
      const auto now=Now();if(now-registry.publish_ms<500)return;registry.publish_ms=now;
      std::ostringstream s;s<<"{\"source\":\"handle_table_locked_boundaries_and_svc\",\"enabled\":true,\"capacity\":256,\"failure_capacity\":64,\"sequence\":"<<registry.sequence
       <<",\"overwritten\":"<<(registry.cursor>256?registry.cursor-256:0)
       <<",\"failure_overwritten\":"<<(registry.failure_cursor>64?registry.failure_cursor-64:0)
       <<",\"lock_dropped\":"<<registry.lock_dropped.load()<<",\"rate_dropped\":"<<registry.rate_dropped.load()
       <<",\"capture_ns\":"<<registry.capture_ns.load()<<",\"publish_ns\":"<<registry.publish_ns.load()
       <<",\"first_failure\":";
      if(registry.first_failure.sequence)Json(s,registry.first_failure,now);else s<<"null";
      s<<",\"first_send_sync_failure\":";
      if(registry.first_send_sync_failure.sequence)Json(s,registry.first_send_sync_failure,now);else s<<"null";
      s<<",\"recent\":[";bool comma=false;
      uint64_t recent_omitted=0,failure_omitted=0;
      for(uint64_t i=registry.cursor>256?registry.cursor-256:0;i<registry.cursor;++i){if(s.tellp()>60000){recent_omitted=registry.cursor-i;break;}if(comma)s<<",";comma=true;Json(s,registry.recent[i%256],now);}
      s<<"],\"failures\":[";comma=false;
      for(uint64_t i=registry.failure_cursor>64?registry.failure_cursor-64:0;i<registry.failure_cursor;++i){if(s.tellp()>100000){failure_omitted=registry.failure_cursor-i;break;}if(comma)s<<",";comma=true;Json(s,registry.failures[i%64],now);}
      s<<"],\"recent_budget_omitted\":"<<recent_omitted<<",\"failure_budget_omitted\":"<<failure_omitted
       <<",\"limitations\":[\"bounded_history_not_live_table\",\"generation_is_linear_id_not_total_reuse_count\",\"capture_cost_excludes_disabled_and_dropped_calls\",\"table_and_object_ids_may_be_reused_after_lifetime\",\"tid_pid_zero_for_table_boundary\",\"stored_generation_zero_means_empty_or_unknown_at_svc_boundary\"]}";payload=s.str(); }
    Common::DebugDocument::Publish("handle-lifecycle",payload,false);
    registry.publish_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
}
} // namespace Kernel::HandleDebugTrace
