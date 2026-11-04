// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Request-key and CPU admission observations, NOT GPU writes or completed framebuffer draws.
#include "yuzu_video_core/image_writer_trace.h"
#include <algorithm>
#include <string_view>

namespace VideoCommon::DrawAdmission {
inline constexpr size_t Slots=256,ProbeSlots=4,History=4,MaxTargets=8,MaxPackets=20,MaxBytes=32768;
inline bool Enabled(){return ImageWriters::Enabled();}
enum class Refresh {Unknown,CachedValid,CachedInvalid,Refreshed,InvalidAddress,Exception};
enum class Outcome {PipelineReady,PipelineNull,AsyncSkipped,WaitFailed,ExceptionAborted};
inline const char* Name(Refresh r){switch(r){case Refresh::CachedValid:return "cached_valid";case Refresh::CachedInvalid:return "cached_invalid";case Refresh::Refreshed:return "refreshed";case Refresh::InvalidAddress:return "invalid_address";case Refresh::Exception:return "exception";default:return "unknown";}}
inline const char* Name(Outcome r){switch(r){case Outcome::PipelineReady:return "pipeline_ready";case Outcome::PipelineNull:return "pipeline_null";case Outcome::AsyncSkipped:return "async_skipped";case Outcome::WaitFailed:return "wait_failed";default:return "exception_aborted";}}
struct Target {uint64_t owner{};uint32_t slot{},width{},height{},format{},base_layer{},depth{},array_pitch{},tile_mode{},color_mask{},output{};};
struct Packet {
 uint64_t token{},record_seq{};Target target{};ImageWriters::OwnerBindingSet bindings{};
 Outcome outcome{Outcome::ExceptionAborted};Refresh refresh{};bool key_valid{};uint64_t key{};
 std::array<uint64_t,6> hashes{},programs{},stage_hashes{};uint32_t stage_visited_mask{},stage_hash_valid_mask{};int failed_stage{-1};
 bool rejection{},reason_truncated{};uint64_t rejection_program{},rejection_offset{};uint32_t rejection_stage{},opcode{};
 std::array<uint32_t,4> words{};std::array<char,192> reason{};
 uint64_t owner_total{};size_t owner_retained{},non_selected_bindings{};bool retained_first_rejection{};
};
struct Snapshot {
 bool available{};uint64_t selected{},serial{},lock_drops{},unbound{},binding_overflow{},binding_lockmiss{},evictions{},overflow{},nonmatching_packets{},first_rejection_replaced{};
 std::array<Packet,MaxPackets> packets{};size_t count{};const char* reason="no_matching_retained_admission";
};
class Registry {
 struct Row{uint64_t owner{},last{},total{};size_t count{},next{};std::array<Packet,History> packets{};Packet first_rejection{};bool has_first_rejection{};};
 bool enabled;std::mutex mutex;std::array<Row,Slots> rows{};uint64_t serial{},evictions{},overflow{},first_rejection_replaced{};
 std::atomic<uint64_t> lock_drops{},unbound{},binding_overflow{},binding_lockmiss{};
 static size_t Start(uint64_t owner){return size_t((owner>>4)^(owner>>19)^owner)%Slots;}
 Row* Find(uint64_t owner){const auto start=Start(owner);for(size_t n=0;n<ProbeSlots;++n){auto& r=rows[(start+n)%Slots];if(r.owner==owner)return &r;}return nullptr;}
public:
 explicit Registry(bool active):enabled(active){}
 bool Record(Packet packet)noexcept{
  if(!enabled||!packet.target.owner)return false;
  if(!packet.bindings.count)++unbound;if(packet.bindings.overflow)++binding_overflow;if(packet.bindings.lockmiss)++binding_lockmiss;
  std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock()){++lock_drops;return false;}
  if(serial==std::numeric_limits<uint64_t>::max()){++overflow;return false;}
  packet.record_seq=++serial;auto* row=Find(packet.target.owner);
  if(!row){const auto start=Start(packet.target.owner);Row* oldest=nullptr;for(size_t n=0;n<ProbeSlots;++n){auto& r=rows[(start+n)%Slots];if(!r.owner){oldest=&r;break;}if(!oldest||r.last<oldest->last)oldest=&r;}row=oldest;if(row->owner)++evictions;*row={};row->owner=packet.target.owner;}
  
  if(packet.rejection){bool same_lifetime=false;if(row->has_first_rejection){for(size_t b=0;b<std::min(packet.bindings.count,size_t{8});++b)for(size_t old=0;old<std::min(row->first_rejection.bindings.count,size_t{8});++old){const auto& x=packet.bindings.bindings[b];const auto& y=row->first_rejection.bindings.bindings[old];same_lifetime|=x.image==y.image&&x.birth==y.birth;}}
   if(!row->has_first_rejection||(!same_lifetime&&packet.bindings.count)){if(row->has_first_rejection)++first_rejection_replaced;row->first_rejection=packet;row->has_first_rejection=true;}
  }
  row->packets[row->next]=packet;row->next=(row->next+1)%History;if(row->count<History)++row->count;
  if(row->total!=std::numeric_limits<uint64_t>::max())++row->total;row->last=serial;return true;
 }
 Snapshot Read(const ImageWriters::Snapshot& writer)noexcept{
  Snapshot out;out.selected=writer.selected;if(!enabled){out.reason="disabled";return out;}
  if(writer.count>ImageWriters::MaxNodes){out.reason="invalid_writer_snapshot";return out;}
  std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock()){++lock_drops;out.reason="snapshot_lock_contention";return out;}
  out.serial=serial;out.first_rejection_replaced=first_rejection_replaced;out.evictions=evictions;out.overflow=overflow;out.lock_drops=lock_drops.load();out.unbound=unbound.load();out.binding_overflow=binding_overflow.load();out.binding_lockmiss=binding_lockmiss.load();
  std::array<uint64_t,ImageWriters::MaxNodes> owners{};size_t owner_count{};
  for(size_t node=0;node<writer.count;++node){const auto owner=writer.nodes[node].gpu_owner;if(!owner||std::find(owners.begin(),owners.begin()+owner_count,owner)!=owners.begin()+owner_count)continue;owners[owner_count++]=owner;
   const auto* row=Find(owner);if(!row)continue;
   for(size_t n=0;n<row->count+(row->has_first_rejection?1:0);++n){auto packet=n<row->count?row->packets[(row->next+History-row->count+n)%History]:row->first_rejection;
    if(n==row->count){packet.retained_first_rejection=true;bool duplicate=false;for(size_t old=0;old<row->count;++old)duplicate|=row->packets[old].record_seq==packet.record_seq;if(duplicate)continue;}
    ImageWriters::OwnerBindingSet selected{};selected.overflow=packet.bindings.overflow;selected.lockmiss=packet.bindings.lockmiss;
    for(size_t b=0;b<std::min(packet.bindings.count,packet.bindings.bindings.size());++b){const auto& binding=packet.bindings.bindings[b];bool matches=false;for(size_t w=0;w<writer.count;++w){const auto& image=writer.nodes[w];matches|=image.image==binding.image&&image.birth==binding.birth&&image.gpu_owner==owner;}
     if(matches)selected.bindings[selected.count++]=binding;else ++packet.non_selected_bindings;
    }
    if(!selected.count){++out.nonmatching_packets;continue;}packet.bindings=selected;packet.owner_total=row->total;packet.owner_retained=row->count;
    if(out.count==MaxPackets){out.available=out.count!=0;out.reason="packet_budget";return out;}out.packets[out.count++]=packet;
   }
  }
  std::sort(out.packets.begin(),out.packets.begin()+out.count,[](const Packet& a,const Packet& b){return a.record_seq<b.record_seq;});out.available=out.count!=0;if(out.available)out.reason="exact_owner_and_before_call_birth_match";return out;
 }
};
inline Registry& Global(){static Registry registry(true);return registry;}
inline std::atomic<uint64_t> tokens{};
class Scoped;
inline thread_local Scoped* current{};
class Scoped {
 bool active{},finished{};Scoped* previous{};Registry* destination{};ImageWriters::Registry* writers{};
 Packet common{};std::array<Target,MaxTargets> targets{};std::array<ImageWriters::OwnerBindingSet,MaxTargets> bindings{};size_t target_count{};
public:
 explicit Scoped(bool on=Enabled(),Registry* dest=nullptr,ImageWriters::Registry* owner_registry=nullptr)noexcept:active(on),previous(current),destination(dest),writers(owner_registry){
  if(active){if(!destination)destination=&Global();if(!writers)writers=&ImageWriters::Global();common.token=tokens.fetch_add(1,std::memory_order_relaxed)+1;current=this;}else current=nullptr;
 }
 Scoped(const Scoped&)=delete;Scoped& operator=(const Scoped&)=delete;
 ~Scoped(){if(active&&!finished)Finish(Outcome::ExceptionAborted);else if(!active)current=previous;}
 uint64_t Token() const noexcept {return active&&!finished?common.token:0;}
 void BeginTarget(const Target& target)noexcept{
  if(!active||finished||target_count==MaxTargets||!target.owner)return;
  targets[target_count]=target;bindings[target_count]=writers->OwnerBindings(target.owner);++target_count;
 }
 void Finish(Outcome outcome)noexcept{
  if(!active||finished)return;finished=true;common.outcome=outcome;for(size_t n=0;n<target_count;++n){auto packet=common;packet.target=targets[n];packet.bindings=bindings[n];destination->Record(packet);}current=previous;
 }
 void Stage(size_t index,uint64_t address)noexcept{if(index<6){common.programs[index]=address;common.stage_visited_mask|=1U<<index;common.failed_stage=static_cast<int>(index);}}
 void StageHash(size_t index,uint64_t hash)noexcept{if(index<6){common.stage_hashes[index]=hash;common.stage_hash_valid_mask|=1U<<index;}}
 void RefreshResult(Refresh result,int failed_stage)noexcept{common.refresh=result;if(failed_stage>=0)common.failed_stage=failed_stage;else if(result==Refresh::Refreshed||result==Refresh::CachedValid||result==Refresh::CachedInvalid)common.failed_stage=-1;}
 void Key(uint64_t key,const std::array<uint64_t,6>& hashes)noexcept{common.key_valid=true;common.key=key;common.hashes=hashes;}
 void Rejected(uint64_t program,uint64_t offset,uint32_t stage,uint32_t opcode,const std::array<uint32_t,4>& words,std::string_view reason)noexcept{
  if(common.rejection)return;common.rejection=true;common.rejection_program=program;common.rejection_offset=offset;common.rejection_stage=stage;common.opcode=opcode;common.words=words;
  const auto count=std::min(reason.size(),common.reason.size()-1);std::copy_n(reason.data(),count,common.reason.data());common.reason_truncated=reason.size()>count;
 }
};
inline uint64_t CurrentToken() noexcept {return current?current->Token():0;}
inline void MarkStage(size_t index,uint64_t address)noexcept{if(current)current->Stage(index,address);}
inline void MarkStageHash(size_t index,uint64_t hash)noexcept{if(current)current->StageHash(index,hash);}
inline void MarkRefresh(Refresh result,int failed_stage=-1)noexcept{if(current)current->RefreshResult(result,failed_stage);}
inline void MarkKey(uint64_t key,const std::array<uint64_t,6>& hashes)noexcept{if(current)current->Key(key,hashes);}
inline void Reject(uint64_t program,uint64_t offset,uint32_t stage,uint32_t opcode,const std::array<uint32_t,4>& words,std::string_view reason)noexcept{if(current)current->Rejected(program,offset,stage,opcode,words,reason);}
inline Snapshot Read(const ImageWriters::Snapshot& writer)noexcept{return Enabled()?Global().Read(writer):Snapshot{};}
inline std::string Serialize(const Snapshot& x){
 if(x.count>MaxPackets)throw std::length_error("admission packet count exceeds bound");
 std::ostringstream o;o<<"{\"schema\":1,\"available\":"<<(x.available?"true":"false")<<",\"selected_image\":"<<x.selected<<",\"reason\":"<<Common::DebugDocument::Quote(x.reason?x.reason:"missing")<<",\"serial\":"<<x.serial<<",\"lock_drops\":"<<x.lock_drops<<",\"unbound\":"<<x.unbound<<",\"binding_overflow\":"<<x.binding_overflow<<",\"binding_lockmiss\":"<<x.binding_lockmiss<<",\"evictions\":"<<x.evictions<<",\"overflow\":"<<x.overflow<<",\"first_rejection_replaced\":"<<x.first_rejection_replaced<<",\"nonmatching_packets\":"<<x.nonmatching_packets<<",\"packets\":[";
 for(size_t n=0;n<x.count;++n){if(n)o<<',';const auto& p=x.packets[n];const auto& t=p.target;if(p.bindings.count>8)throw std::length_error("admission binding count exceeds bound");
  o<<"{\"token\":"<<p.token<<",\"record_seq\":"<<p.record_seq<<",\"outcome\":\""<<Name(p.outcome)<<"\",\"refresh\":\""<<Name(p.refresh)<<"\",\"key_valid\":"<<(p.key_valid?"true":"false")<<",\"requested_pre_lookup_key\":"<<p.key<<",\"stage_visited_mask\":"<<p.stage_visited_mask<<",\"stage_hash_valid_mask\":"<<p.stage_hash_valid_mask<<",\"failed_stage\":"<<p.failed_stage<<",\"hashes\":[";
  for(size_t k=0;k<6;++k){if(k)o<<',';o<<p.hashes[k];}o<<"],\"programs\":[";for(size_t k=0;k<6;++k){if(k)o<<',';o<<p.programs[k];}o<<"],\"stage_hashes\":[";for(size_t k=0;k<6;++k){if(k)o<<',';o<<p.stage_hashes[k];}
  o<<"],\"target\":{\"owner\":"<<t.owner<<",\"slot\":"<<t.slot<<",\"output\":"<<t.output<<",\"raw_width\":"<<t.width<<",\"raw_height\":"<<t.height<<",\"format\":"<<t.format<<",\"base_layer\":"<<t.base_layer<<",\"depth\":"<<t.depth<<",\"array_pitch\":"<<t.array_pitch<<",\"tile_mode\":"<<t.tile_mode<<",\"color_mask\":"<<t.color_mask<<"},\"bindings_before_call\":[";
  for(size_t b=0;b<p.bindings.count;++b){if(b)o<<',';const auto& v=p.bindings.bindings[b];o<<"{\"image\":"<<v.image<<",\"birth\":"<<v.birth<<",\"width\":"<<v.width<<",\"height\":"<<v.height<<"}";}
  o<<"],\"bindings_overflow\":"<<(p.bindings.overflow?"true":"false")<<",\"bindings_lockmiss\":"<<(p.bindings.lockmiss?"true":"false")<<",\"non_selected_bindings\":"<<p.non_selected_bindings<<",\"retained_first_rejection\":"<<(p.retained_first_rejection?"true":"false")<<",\"owner_total\":"<<p.owner_total<<",\"owner_retained\":"<<p.owner_retained<<",\"rejection\":"<<(p.rejection?"true":"false")<<",\"rejection_program\":"<<p.rejection_program<<",\"rejection_offset\":"<<p.rejection_offset<<",\"rejection_stage\":"<<p.rejection_stage<<",\"opcode\":"<<p.opcode<<",\"words\":[";
  for(size_t k=0;k<4;++k){if(k)o<<',';o<<p.words[k];}o<<"],\"reason\":"<<Common::DebugDocument::Quote(std::string(p.reason.data(),std::find(p.reason.begin(),p.reason.end(),'\0')-p.reason.begin()))<<",\"reason_truncated\":"<<(p.reason_truncated?"true":"false")<<"}";
 }
 o<<"],\"limits\":{\"owner_slots\":"<<Slots<<",\"owner_probe_slots\":"<<ProbeSlots<<",\"events_per_owner\":"<<History<<",\"packets\":"<<MaxPackets<<",\"bindings_before_call\":8},\"scope\":\"exact owner plus before-call cached image birth association; CPU admission only, not GPU write proof; raw width may be pitch; async lower failures outside TLS unknown\"}";
 auto result=o.str();if(result.size()>MaxBytes)throw std::length_error("admission metadata exceeds bound");return result;
}
} // namespace VideoCommon::DrawAdmission
