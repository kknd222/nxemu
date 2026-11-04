// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Fixed-slot admission is independent of snapshot locks. No GPU calls/threads/waits.
#include "yuzu_shader_recompiler/translation_trace.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace VideoCommon::ShaderProgress {
constexpr std::size_t ActiveLimit=64,HistoryLimit=32;
constexpr std::uint64_t PublishIntervalNs=500000000;
inline std::uint64_t Now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
bool Enabled();std::string RunId();std::string Generation();void Publish() noexcept;
struct Session {
 const std::string run,generation;const std::uint64_t epoch;
 std::atomic<std::uint64_t> begun{},completed{},exceptions{},skipped{},history_overwrites{};
 Session(std::string r,std::string g,std::uint64_t e):run(std::move(r)),generation(std::move(g)),epoch(e){}
};
struct Token {std::uint64_t epoch{},id{},slot_generation{};std::uint32_t slot_index{UINT32_MAX};std::shared_ptr<Session> session;explicit operator bool()const{return id!=0;}};
enum class Stage:std::uint8_t {CFG,TranslateProgram,IRPostprocess,EmitSPIRV,ShaderModule,Constructor,Queued,DescriptorLayout,DriverCreateComputePipeline,Statistics,Count};
inline const char* Name(Stage s){constexpr const char* names[]={"CFG","TranslateProgram","IRPostprocess","EmitSPIRV","ShaderModule","Constructor","queued","descriptorLayout","driverCreateComputePipeline","pipelineStatistics"};return static_cast<std::size_t>(s)<std::size(names)?names[static_cast<std::size_t>(s)]:"unknown";}
enum class State:std::uint8_t {Running,Pending,Completed,Exception,Skipped};
inline const char* Name(State s){constexpr const char* names[]={"running","pending","completed","exception","skipped"};return names[static_cast<std::size_t>(s)];}
struct Metrics {std::uint64_t source_bytes{},ir_blocks{},ir_instructions{},spirv_words{},ssbo_count{};bool source_known{},ir_known{},ir_truncated{},spirv_known{},ssbo_known{};std::uint64_t create_flags{},required_subgroup_size{};bool native_parameters_known{},subgroup_pnext_present{};};
struct Row {
 Token token{};std::uint64_t hash{},started_ns{},changed_ns{},ended_ns{},wait_started_ns{},wait_ended_ns{},wait_calls{},waiters{};
 Stage stage{Stage::CFG};State state{State::Running};Metrics metrics{};
 Shader::TranslationTrace::Record translation{};bool translation_known{};
 std::array<std::uint64_t,static_cast<std::size_t>(Stage::Count)> stage_ns{};
 std::array<char,97> reason{};std::array<char,49> device{};bool reason_truncated{},device_truncated{};
};

enum class SlotPhase:std::uint8_t {Free,Initializing,Ready};
struct Slot {std::atomic<SlotPhase> phase{SlotPhase::Free};std::atomic<std::uint64_t> generation{};std::mutex mutex;Row row{};};
// Portable C++ shared_ptr atomic operations; no lock-freedom claim.
struct AtomicSession {
 std::shared_ptr<Session> value;
 std::shared_ptr<Session> load(std::memory_order order=std::memory_order_seq_cst)const{return std::atomic_load_explicit(&value,order);}
 bool compare_exchange_strong(std::shared_ptr<Session>& expected,std::shared_ptr<Session> desired,std::memory_order order){return std::atomic_compare_exchange_strong_explicit(&value,&expected,std::move(desired),order,std::memory_order_acquire);}
};
struct Registry {
 AtomicSession session;
 std::atomic<std::uint64_t> next_epoch{},next_id{};
 std::array<Slot,ActiveLimit> slots;
 std::mutex history_mutex;std::array<Row,HistoryLimit> history{};std::size_t history_next{},history_count{};std::uint64_t history_epoch{};
 std::atomic<std::uint64_t> begin_attempts{},begin_lock_drops{},begin_exception_drops{},begin_session_invalid{},begin_session_races{},active_overflow{},
 update_drops{},update_lock_drops{},update_exception_drops{},terminal_drops{},terminal_lock_drops{},terminal_exception_drops{},
 mixed_session_drops{},unknown_events{},session_discarded{},retired_terminal_slots{},archive_deferrals{},snapshot_slot_drops{},snapshot_history_drops{},
 publish_drops{},publish_ns{},capture_ns{},last_publish_ns{};
};
inline Registry registry;
// Synchronous source hint only; token identity never depends on worker TLS.
inline thread_local std::optional<std::uint64_t> source_hint;
class SourceScope {std::optional<std::uint64_t> previous;public:explicit SourceScope(std::uint64_t bytes):previous(source_hint){source_hint=bytes;}~SourceScope(){source_hint=previous;}};
template<std::size_t N> inline bool Copy(std::array<char,N>& out,std::string_view text){out.fill(0);const auto n=std::min(text.size(),N-1);std::copy_n(text.begin(),n,out.begin());return n<text.size();}
inline std::shared_ptr<Session> CurrentSession() {
 const auto run=RunId(),gen=Generation();if(run.empty()||gen.empty()||run.size()>256||gen.size()>256)return {};
 auto& r=registry;auto current=r.session.load(std::memory_order_acquire);
 for(unsigned attempt=0;attempt<4;++attempt){
  if(current&&current->run==run&&current->generation==gen)return current;
  auto candidate=std::make_shared<Session>(run,gen,r.next_epoch.fetch_add(1)+1);
  if(r.session.compare_exchange_strong(current,candidate,std::memory_order_acq_rel))return candidate;
 }
 return {};
}
inline bool Current(const Token& t){const auto current=registry.session.load(std::memory_order_acquire);return t.session&&current&&t.epoch==current->epoch&&t.session.get()==current.get();}
inline bool Match(const Row& row,const Token& t){return row.token.id==t.id&&row.token.epoch==t.epoch&&row.token.slot_generation==t.slot_generation&&row.token.slot_index==t.slot_index;}
// Must hold slot mutex. Publication of Free occurs only after the last row access.
inline void RetireLocked(Slot& slot) {
 const auto current=registry.session.load(std::memory_order_acquire);
 if(slot.phase.load(std::memory_order_acquire)==SlotPhase::Ready&&current&&slot.row.token.epoch!=current->epoch){
  if(slot.row.ended_ns)++registry.retired_terminal_slots;else ++registry.session_discarded;
  slot.row={};slot.phase.store(SlotPhase::Free,std::memory_order_release);
 }
}
inline void Retire(Slot& slot) {
 if(slot.phase.load(std::memory_order_acquire)!=SlotPhase::Ready)return;
 std::unique_lock lock(slot.mutex,std::try_to_lock);if(lock.owns_lock())RetireLocked(slot);
}
// Reservation owns Initializing exclusively; abort returns the slot without publishing a row.
class Reservation {
 Slot* slot{};
public:
 explicit Reservation(Slot& s):slot(&s){}
 ~Reservation(){if(slot){slot->row={};slot->phase.store(SlotPhase::Free,std::memory_order_release);}}
 void Commit(){slot->phase.store(SlotPhase::Ready,std::memory_order_release);slot=nullptr;}
};
inline Token Begin(std::uint64_t hash,std::string_view device={}) noexcept {
 try {
  if(!Enabled())return {};auto& r=registry;++r.begin_attempts;const auto started=Now();const auto context=CurrentSession();
  if(!context){++r.begin_session_invalid;return {};}
  for(std::uint32_t index=0;index<ActiveLimit;++index){
   auto& slot=r.slots[index];Retire(slot);
   auto free=SlotPhase::Free;
   if(!slot.phase.compare_exchange_strong(free,SlotPhase::Initializing,std::memory_order_acq_rel))continue;
   Reservation reservation(slot);
   if(r.session.load(std::memory_order_acquire).get()!=context.get()){++r.begin_session_races;return {};}
   const Token token{context->epoch,r.next_id.fetch_add(1)+1,slot.generation.fetch_add(1)+1,index,context};
   // No readers/writers inspect row while Initializing; this copy needs no slot/global lock.
   slot.row={};slot.row.token=token;slot.row.hash=hash;slot.row.started_ns=slot.row.changed_ns=Now();slot.row.device_truncated=Copy(slot.row.device,device);
   if(source_hint){slot.row.metrics.source_bytes=*source_hint;slot.row.metrics.source_known=true;}
   ++context->begun;reservation.Commit();r.capture_ns+=Now()-started;return token;
  }
  ++r.active_overflow;return {};
 }catch(...){++registry.begin_exception_drops;++registry.update_drops;return {};}
}
// Must hold slot mutex. Keep terminal row occupying capacity if history is contended.
inline void ArchiveLocked(Slot& slot) {
 if(!slot.row.ended_ns||slot.phase.load(std::memory_order_acquire)!=SlotPhase::Ready)return;
 auto& r=registry;std::unique_lock history_lock(r.history_mutex,std::try_to_lock);
 if(!history_lock.owns_lock()){++r.archive_deferrals;return;}
 if(!Current(slot.row.token)){RetireLocked(slot);return;}
 const auto context=slot.row.token.session;
 if(r.history_epoch!=context->epoch){r.history={};r.history_next=r.history_count=0;r.history_epoch=context->epoch;}
 if(r.history_count==HistoryLimit)++context->history_overwrites;else ++r.history_count;
 r.history[r.history_next]=slot.row;r.history_next=(r.history_next+1)%HistoryLimit;
 slot.row={};slot.phase.store(SlotPhase::Free,std::memory_order_release);
}
inline void Drop(bool terminal,bool exception=false){auto& r=registry;++r.update_drops;if(terminal){++r.terminal_drops;if(exception)++r.terminal_exception_drops;else ++r.terminal_lock_drops;}else{if(exception)++r.update_exception_drops;else ++r.update_lock_drops;}}
template<class F> inline void WithRow(Token token,F fn,bool include_terminal,bool terminal) noexcept {
 if(!token)return;
 try {
  const auto started=Now();auto& r=registry;
  if(!Current(token)){++r.mixed_session_drops;return;}
  if(token.slot_index<ActiveLimit){auto& slot=r.slots[token.slot_index];
   std::unique_lock lock(slot.mutex,std::try_to_lock);if(!lock.owns_lock()){Drop(terminal);return;}
   if(slot.phase.load(std::memory_order_acquire)==SlotPhase::Ready&&Match(slot.row,token)){
    if(!slot.row.ended_ns||include_terminal)fn(slot.row);
    if(slot.row.ended_ns)ArchiveLocked(slot);r.capture_ns+=Now()-started;return;
   }
  }
  if(include_terminal){std::unique_lock lock(r.history_mutex,std::try_to_lock);if(!lock.owns_lock()){Drop(terminal);return;}
   if(r.history_epoch==token.epoch)for(auto& row:r.history)if(Match(row,token)){fn(row);r.capture_ns+=Now()-started;return;}
  }
  ++r.unknown_events;
 }catch(...){Drop(terminal,true);}
}
template<class F> inline void Update(Token token,F fn,bool include_terminal=false) noexcept {WithRow(token,fn,include_terminal,false);}
inline void Transition(Token t,Stage stage){Update(t,[&](Row& row){const auto now=Now();row.stage_ns[static_cast<std::size_t>(row.stage)]+=now-row.changed_ns;row.changed_ns=now;row.stage=stage;row.state=stage==Stage::Queued?State::Pending:State::Running;});}
inline void SetTranslationTrace(Token t,const Shader::TranslationTrace::Record& record){Update(t,[&](Row& row){row.translation=record;row.translation_known=true;});}
inline void SetMetrics(Token t,const Metrics& metrics){Update(t,[&](Row& row){row.metrics=metrics;});}
inline void SetNativeParameters(Token t,std::uint64_t flags,bool subgroup_present,std::uint64_t required_size){Update(t,[&](Row& row){row.metrics.create_flags=flags;row.metrics.required_subgroup_size=required_size;row.metrics.subgroup_pnext_present=subgroup_present;row.metrics.native_parameters_known=true;});}
inline void SetReason(Token t,std::string_view reason){Update(t,[&](Row& row){row.reason_truncated=Copy(row.reason,reason);});}
inline void WaitBegin(Token t){Update(t,[](Row& row){++row.wait_calls;++row.waiters;row.wait_started_ns=Now();},true);}
inline void WaitEnd(Token t){Update(t,[](Row& row){if(row.waiters)--row.waiters;row.wait_ended_ns=Now();},true);}
inline void Finish(Token t,State state,std::string_view reason={}) noexcept {
 WithRow(t,[&](Row& row){if(row.ended_ns)return;const auto now=Now();row.stage_ns[static_cast<std::size_t>(row.stage)]+=now-row.changed_ns;row.ended_ns=now;row.state=state;if(!reason.empty())row.reason_truncated=Copy(row.reason,reason);
  const auto& context=row.token.session;if(state==State::Completed)++context->completed;else if(state==State::Exception)++context->exceptions;else if(state==State::Skipped)++context->skipped;
 },true,true);
}
class Guard {Token token;int exceptions;bool armed{true};public:explicit Guard(Token t):token(t),exceptions(std::uncaught_exceptions()){}void HandOff(){armed=false;}~Guard(){if(armed&&token)Finish(token,State::Exception,std::uncaught_exceptions()>exceptions?"exception_unwinding":"scope_left_without_native_handoff");}};
class WaitGuard {Token token;public:explicit WaitGuard(Token t):token(t){WaitBegin(token);}~WaitGuard(){WaitEnd(token);}};
struct Snapshot {
 std::string run,generation;std::array<Row,ActiveLimit> active{};std::array<Row,HistoryLimit> history{};
 std::uint64_t epoch{},sampled_ns{},begun{},completed{},exceptions{},skipped{},history_overwrites{},session_discarded{},unknown_events{},update_drops{},active_overflow{},mixed_session_drops{},publish_drops{},publish_ns{},capture_ns{},
 begin_attempts{},begin_lock_drops{},begin_exception_drops{},begin_session_invalid{},begin_session_races{},update_lock_drops{},update_exception_drops{},terminal_drops{},terminal_lock_drops{},terminal_exception_drops{},archive_deferrals{},retired_terminal_slots{},snapshot_slot_drops{},snapshot_history_drops{};
 std::size_t snapshot_omitted_slots{};bool snapshot_history_omitted{};
};
inline std::optional<Snapshot> Take(std::uint64_t =Now()) noexcept {
 try {
  auto& r=registry;const auto context=CurrentSession();if(!context)return std::nullopt;Snapshot s;s.run=context->run;s.generation=context->generation;s.epoch=context->epoch;
  for(std::size_t index=0;index<ActiveLimit;++index){auto& slot=r.slots[index];std::unique_lock lock(slot.mutex,std::try_to_lock);
   if(!lock.owns_lock()){++r.snapshot_slot_drops;++s.snapshot_omitted_slots;continue;}
   RetireLocked(slot);
   if(slot.phase.load(std::memory_order_acquire)==SlotPhase::Initializing)++s.snapshot_omitted_slots;
   if(slot.phase.load(std::memory_order_acquire)==SlotPhase::Ready&&slot.row.token.epoch==s.epoch){ArchiveLocked(slot);if(slot.phase.load(std::memory_order_acquire)==SlotPhase::Ready)s.active[index]=slot.row;}
  }
  {std::unique_lock lock(r.history_mutex,std::try_to_lock);if(!lock.owns_lock()){++r.snapshot_history_drops;s.snapshot_history_omitted=true;}else if(r.history_epoch==s.epoch)s.history=r.history;}
  s.sampled_ns=Now();s.begun=context->begun;s.completed=context->completed;s.exceptions=context->exceptions;s.skipped=context->skipped;s.history_overwrites=context->history_overwrites;
#define NX2_COPY_COUNTER(field) s.field=r.field.load()
  NX2_COPY_COUNTER(session_discarded);NX2_COPY_COUNTER(unknown_events);NX2_COPY_COUNTER(update_drops);NX2_COPY_COUNTER(active_overflow);NX2_COPY_COUNTER(mixed_session_drops);NX2_COPY_COUNTER(publish_drops);NX2_COPY_COUNTER(publish_ns);NX2_COPY_COUNTER(capture_ns);
  NX2_COPY_COUNTER(begin_attempts);NX2_COPY_COUNTER(begin_lock_drops);NX2_COPY_COUNTER(begin_exception_drops);NX2_COPY_COUNTER(begin_session_invalid);NX2_COPY_COUNTER(begin_session_races);NX2_COPY_COUNTER(update_lock_drops);NX2_COPY_COUNTER(update_exception_drops);NX2_COPY_COUNTER(terminal_drops);NX2_COPY_COUNTER(terminal_lock_drops);NX2_COPY_COUNTER(terminal_exception_drops);NX2_COPY_COUNTER(archive_deferrals);NX2_COPY_COUNTER(retired_terminal_slots);NX2_COPY_COUNTER(snapshot_slot_drops);NX2_COPY_COUNTER(snapshot_history_drops);
#undef NX2_COPY_COUNTER
  return s;
 }catch(...){++registry.publish_drops;return std::nullopt;}
}
inline std::string Quote(std::string_view v){std::ostringstream s;s<<'"';for(const unsigned char c:v){if(c=='"'||c=='\\')s<<'\\'<<char(c);else if(c<32||c>=128)s<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;else s<<char(c);}s<<'"';return s.str();}
inline std::string RowJson(const Row& r,std::uint64_t now){
 std::ostringstream s;const auto number=[&](std::uint64_t value,bool known){return known?std::to_string(value):"null";};
 s<<"{\"token\":"<<r.token.id<<",\"session_epoch\":"<<r.token.epoch<<",\"slot_index\":"<<r.token.slot_index<<",\"slot_generation\":"<<r.token.slot_generation<<",\"pipeline_hash\":"<<r.hash<<",\"stage\":"<<Quote(Name(r.stage))<<",\"state\":"<<Quote(Name(r.state))
 <<",\"started_ns\":"<<r.started_ns<<",\"changed_ns\":"<<r.changed_ns<<",\"ended_ns\":"<<r.ended_ns<<",\"age_ms\":"<<(now-r.changed_ns)/1000000<<",\"total_ms\":"<<((r.ended_ns?r.ended_ns:now)-r.started_ns)/1000000
 <<",\"waiters\":"<<r.waiters<<",\"wait_calls\":"<<r.wait_calls<<",\"wait_started_ns\":"<<r.wait_started_ns<<",\"wait_ended_ns\":"<<r.wait_ended_ns
 <<",\"source_bytes\":"<<number(r.metrics.source_bytes,r.metrics.source_known)<<",\"ir_blocks\":"<<number(r.metrics.ir_blocks,r.metrics.ir_known)<<",\"ir_instructions\":"<<number(r.metrics.ir_instructions,r.metrics.ir_known)
 <<",\"ir_count_truncated\":"<<(r.metrics.ir_truncated?"true":"false")<<",\"spirv_words\":"<<number(r.metrics.spirv_words,r.metrics.spirv_known)<<",\"ssbo_count\":"<<number(r.metrics.ssbo_count,r.metrics.ssbo_known)
 <<",\"create_flags\":"<<number(r.metrics.create_flags,r.metrics.native_parameters_known)<<",\"required_subgroup_size\":"<<number(r.metrics.required_subgroup_size,r.metrics.native_parameters_known)<<",\"native_parameters_known\":"<<(r.metrics.native_parameters_known?"true":"false")<<",\"subgroup_pnext_present\":"<<(r.metrics.native_parameters_known?(r.metrics.subgroup_pnext_present?"true":"false"):"null")
 <<",\"device\":"<<Quote(r.device.data())<<",\"device_truncated\":"<<(r.device_truncated?"true":"false")<<",\"reason\":"<<Quote(r.reason.data())<<",\"reason_truncated\":"<<(r.reason_truncated?"true":"false")
 <<",\"not_running_proof\":"<<Quote(r.ended_ns?"terminal_scope_observed":(r.state==State::Pending?"queue_submission_observed_only":"none"))<<",\"stage_ns\":[";
 for(std::size_t i=0;i<r.stage_ns.size();++i){if(i)s<<',';s<<r.stage_ns[i];}s<<"],\"translation_passes\":";
 if(!r.translation_known)s<<"null";else {
  s<<'[';bool comma=false;
  for(std::size_t i=0;i<Shader::TranslationTrace::Count;++i){const auto& p=r.translation.passes[i];if(!p.calls)continue;
   if(comma)s<<',';comma=true;s<<"{\"name\":"<<Quote(Shader::TranslationTrace::Names[i])<<",\"wall_ns\":"<<p.wall_ns<<",\"calls\":"<<p.calls<<",\"completed\":"<<p.completed<<",\"exceptions\":"<<p.exceptions<<'}';}
  s<<']';
 }
 s<<",\"translation_subpasses\":";
 if(!r.translation_known)s<<"null";else {
  s<<'[';bool comma=false;
  for(std::size_t i=0;i<Shader::TranslationTrace::SubCount;++i){const auto& p=r.translation.subpasses[i];if(!p.calls)continue;
   if(comma)s<<',';comma=true;s<<"{\"name\":"<<Quote(Shader::TranslationTrace::SubNames[i])<<",\"wall_ns\":"<<p.wall_ns<<",\"calls\":"<<p.calls<<",\"completed\":"<<p.completed<<",\"exceptions\":"<<p.exceptions<<'}';}
  s<<']';
 }
 s<<",\"translation_work\":";
 if(!r.translation_known)s<<"null";else {
  s<<'{';for(std::size_t i=0;i<Shader::TranslationTrace::WorkCount;++i){if(i)s<<',';s<<Quote(Shader::TranslationTrace::WorkNames[i])<<':'<<r.translation.work[i];}
  s<<",\"overflow\":"<<(r.translation.work_overflow?"true":"false")<<'}';
 }
 s<<'}';return s.str();
}
inline std::string Json(const Snapshot& snapshot){
 const auto& r=snapshot;std::ostringstream s;s<<"{\"source\":\"compute_hooks_and_frontend_DebugSnapshot\",\"session_epoch\":"<<r.epoch<<",\"sampled_ns\":"<<r.sampled_ns<<",\"snapshot_run_id\":"<<Quote(r.run)<<",\"snapshot_generation\":"<<Quote(r.generation)
 <<",\"begun\":"<<r.begun<<",\"completed\":"<<r.completed<<",\"exceptions\":"<<r.exceptions<<",\"skipped\":"<<r.skipped<<",\"history_overwrites\":"<<r.history_overwrites<<",\"session_discarded_active\":"<<r.session_discarded
 <<",\"unknown_events\":"<<r.unknown_events<<",\"update_drops\":"<<r.update_drops<<",\"active_overflow\":"<<r.active_overflow<<",\"mixed_session_drops\":"<<r.mixed_session_drops<<",\"publish_drops\":"<<r.publish_drops<<",\"publish_ns\":"<<r.publish_ns<<",\"capture_ns\":"<<r.capture_ns
 <<",\"begin_attempts\":"<<r.begin_attempts<<",\"begin_lock_drops\":"<<r.begin_lock_drops<<",\"begin_exception_drops\":"<<r.begin_exception_drops<<",\"begin_session_invalid\":"<<r.begin_session_invalid<<",\"begin_session_races\":"<<r.begin_session_races
 <<",\"update_lock_drops\":"<<r.update_lock_drops<<",\"update_exception_drops\":"<<r.update_exception_drops<<",\"terminal_drops\":"<<r.terminal_drops<<",\"terminal_lock_drops\":"<<r.terminal_lock_drops<<",\"terminal_exception_drops\":"<<r.terminal_exception_drops
 <<",\"archive_deferrals\":"<<r.archive_deferrals<<",\"retired_terminal_slots\":"<<r.retired_terminal_slots<<",\"snapshot_slot_drops\":"<<r.snapshot_slot_drops<<",\"snapshot_history_drops\":"<<r.snapshot_history_drops<<",\"snapshot_omitted_slots\":"<<r.snapshot_omitted_slots<<",\"snapshot_history_omitted\":"<<(r.snapshot_history_omitted?"true":"false")
 <<",\"limits\":{\"active\":64,\"history\":32,\"publish_ms\":500,\"payload_soft_bytes\":98304},\"active\":[";
 std::size_t omitted{},active_count{},history_count{},pending_terminal_count{},terminal_waiters{};bool comma=false;
 for(const auto& row:r.active)if(row.token&&!row.ended_ns){++active_count;const auto text=RowJson(row,r.sampled_ns);if(s.tellp()+std::streamoff(text.size())>96000){++omitted;continue;}if(comma)s<<',';s<<text;comma=true;}
 s<<"],\"pending_terminal\":[";comma=false;
 for(const auto& row:r.active)if(row.token&&row.ended_ns){++pending_terminal_count;terminal_waiters+=row.waiters;const auto text=RowJson(row,r.sampled_ns);if(s.tellp()+std::streamoff(text.size())>95000){++omitted;continue;}if(comma)s<<',';s<<text;comma=true;}
 s<<"],\"history\":[";comma=false;
 for(const auto& row:r.history)if(row.token){++history_count;terminal_waiters+=row.waiters;const auto text=RowJson(row,r.sampled_ns);if(s.tellp()+std::streamoff(text.size())>96000){++omitted;continue;}if(comma)s<<',';s<<text;comma=true;}
 s<<"],\"active_count\":"<<active_count<<",\"history_count\":"<<history_count<<",\"pending_terminal_count\":"<<pending_terminal_count<<",\"reserved_slot_count\":"<<active_count+pending_terminal_count<<",\"snapshot_omitted_rows\":"<<omitted<<",\"known_compute_not_running\":"<<(active_count==0&&pending_terminal_count==0&&r.snapshot_omitted_slots==0&&!r.snapshot_history_omitted&&omitted==0&&terminal_waiters==0&&r.begun>0&&r.completed+r.exceptions+r.skipped==r.begun&&r.begin_exception_drops==0&&r.begin_session_invalid==0&&r.begin_session_races==0&&r.update_drops==0&&r.active_overflow==0&&r.unknown_events==0&&r.session_discarded==0?"true":"false")
 <<",\"omissions\":[\"graphics_lower_cachehits_uncovered\",\"no_global_shader_not_running_proof\",\"pending_is_queue_boundary_not_driver_start_proof\",\"update_drop_may_leave_stale_stage_or_terminal\",\"source_size_hint_only_online_synchronous_path\",\"stage_times_exclude_current_inflight_phase_until_transition\",\"queue_cancellation_and_pipeline_destruction_uncovered\",\"drop_counters_cumulative_process_except_session_lifecycle\",\"per_slot_snapshot_not_atomic_across_all_operations\",\"pending_terminal_occupies_slot_until_history_archive\",\"shared_ptr_session_CAS_not_claimed_lock_free\",\"publication_cost_previous_samples_only\",\"no_timeout_cancellation_or_success_fabrication\"]}";return s.str();
}
} // namespace VideoCommon::ShaderProgress
