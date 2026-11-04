// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Bounded command-record history. NOT a GPU completion, shader-result or full dependency trace.
#include "yuzu_common/debug_document.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <limits>
#include <type_traits>
#include <stdexcept>

namespace VideoCommon::ImageWriters {
inline constexpr size_t Slots=512,ProbeSlots=4,History=8,MaxNodes=4,MaxBytes=65536;
enum class Op {DrawRecorded,DrawSkipped,ClearRecorded,CopyRecorded,ScaleUpRecorded,
 ScaleDownRecorded,ScaleDiscard,UploadRecorded,ScaleHelperQueued,CopyQueued,UploadQueued};
inline const char* Name(Op op){switch(op){
case Op::DrawRecorded:return "draw_recorded";case Op::DrawSkipped:return "draw_skipped_unbuilt";
case Op::ClearRecorded:return "clear_recorded";case Op::CopyRecorded:return "copy_recorded";
case Op::ScaleUpRecorded:return "scale_up_recorded";case Op::ScaleDownRecorded:return "scale_down_recorded";
case Op::ScaleDiscard:return "scale_select_without_copy";case Op::UploadRecorded:return "upload_recorded";
case Op::ScaleHelperQueued:return "scale_helper_queued_not_record_proof";
case Op::CopyQueued:return "copy_queued_not_record_proof";default:return "upload_queued_not_record_proof";}}
template<class T> inline uint64_t Handle(T h)noexcept{
 if constexpr(std::is_pointer_v<T>)return reinterpret_cast<uintptr_t>(h);else return uint64_t(h);
}
struct Event {
 bool valid{};Op op{};uint64_t seq{},dst{},dst_birth{},src{},src_birth{},shader_key{};
 std::array<uint64_t,6> hashes{};std::array<uint64_t,4> aux{};
 bool clear_region_known{};std::array<uint64_t,4> clear_rect{};std::array<uint64_t,2> clear_layers{};
};
struct Node {
 uint64_t image{},birth{},cpu_owner{},gpu_owner{},parent{},parent_birth{},last{},total{};
 uint32_t width{},height{};std::array<Event,History> events{};size_t count{},next{};
};
struct Snapshot {
 bool available{};uint64_t selected{},serial{},lock_drops{},stale_drops{},missing_drops{},evictions{},overflow{};
 std::array<Node,MaxNodes> nodes{};size_t count{};
 const char* reason="not_found";
};
struct OwnerBinding {uint64_t image{},birth{};uint32_t width{},height{};};
struct OwnerBindingSet {std::array<OwnerBinding,8> bindings{};size_t count{};bool overflow{},lockmiss{};};
class Registry {
 bool enabled;std::mutex mutex;std::array<Node,Slots> slots{};uint64_t serial{},evictions{},overflow{};
 std::atomic<uint64_t> lock_drops{},stale_drops{},missing_drops{};
 static size_t Start(uint64_t image){return size_t((image>>4)^(image>>19)^image)%Slots;}
 Node* Find(uint64_t image){if(!image)return nullptr;const auto base=Start(image);for(size_t n=0;n<ProbeSlots;++n){auto& s=slots[(base+n)%Slots];if(s.image==image)return &s;}return nullptr;}
 Node* Insert(uint64_t image){const auto base=Start(image);Node* oldest=nullptr;for(size_t n=0;n<ProbeSlots;++n){auto& s=slots[(base+n)%Slots];if(!s.image)return &s;if(!oldest||s.last<oldest->last)oldest=&s;}++evictions;return oldest;}
 uint64_t Next(){if(serial==std::numeric_limits<uint64_t>::max()){++overflow;return 0;}return ++serial;}
public:
 explicit Registry(bool active):enabled(active){}
 // Exact GPU base owner only; no range/alias/framebuffer lookup or mutation of image data.
 OwnerBindingSet OwnerBindings(uint64_t owner)noexcept{
  OwnerBindingSet out;if(!enabled||!owner)return out;
  std::unique_lock l(mutex,std::try_to_lock);if(!l.owns_lock()){out.lockmiss=true;return out;}
  for(const auto& node:slots)if(node.image&&node.gpu_owner==owner){
   if(out.count==out.bindings.size()){out.overflow=true;continue;}
   out.bindings[out.count++]={node.image,node.birth,node.width,node.height};
  }return out;
 }

 uint64_t Birth(uint64_t image,uint64_t cpu,uint64_t gpu,uint64_t parent,uint32_t width,uint32_t height)noexcept{
  if(!enabled||!image)return 0;std::unique_lock l(mutex,std::try_to_lock);if(!l.owns_lock()){++lock_drops;return 0;}
  const auto epoch=Next();if(!epoch)return 0;auto* p=Find(parent);const auto parent_epoch=p?p->birth:0;
  auto* s=Find(image);if(!s)s=Insert(image);*s={};s->image=image;s->birth=epoch;s->last=epoch;s->cpu_owner=cpu;s->gpu_owner=gpu;s->parent=parent;s->parent_birth=parent_epoch;s->width=width;s->height=height;return epoch;
 }
 Event Stamp(uint64_t dst,Op op,uint64_t src=0)noexcept{
  Event e;e.op=op;e.dst=dst;e.src=src;if(!enabled)return e;
  std::unique_lock l(mutex,std::try_to_lock);if(!l.owns_lock()){++lock_drops;return e;}
  const auto* d=Find(dst);if(!d){++missing_drops;return e;}e.valid=true;e.dst_birth=d->birth;
  const auto* s=Find(src);e.src_birth=s?s->birth:0;return e;
 }
 bool Record(Event e)noexcept{
  if(!enabled||!e.valid)return false;std::unique_lock l(mutex,std::try_to_lock);if(!l.owns_lock()){++lock_drops;return false;}
  auto* d=Find(e.dst);if(!d||d->birth!=e.dst_birth){++stale_drops;return false;}
  const auto seq=Next();if(!seq)return false;e.seq=seq;d->events[d->next]=e;d->next=(d->next+1)%History;
  if(d->count<History)++d->count;if(d->total!=std::numeric_limits<uint64_t>::max())++d->total;d->last=seq;return true;
 }
 Snapshot Read(uint64_t image)noexcept{
  Snapshot out;out.selected=image;out.reason=enabled?"not_found":"disabled";if(!enabled)return out;
  std::unique_lock l(mutex,std::try_to_lock);if(!l.owns_lock()){++lock_drops;out.reason="snapshot_lock_contention";return out;}
  out.serial=serial;out.lock_drops=lock_drops.load();out.stale_drops=stale_drops.load();out.missing_drops=missing_drops.load();out.evictions=evictions;out.overflow=overflow;
  auto* node=Find(image);uint64_t expected=node?node->birth:0;
  while(node&&out.count<MaxNodes){
   if(expected&&node->birth!=expected){out.reason="parent_birth_changed";break;}
   bool cycle=false;for(size_t n=0;n<out.count;++n)cycle|=out.nodes[n].image==node->image&&out.nodes[n].birth==node->birth;
   if(cycle){out.reason="parent_cycle";break;}
   out.nodes[out.count++]=*node;out.available=true;out.reason="bounded_history";
   uint64_t parent=node->parent;expected=node->parent_birth;
   for(size_t n=0;n<node->count;++n){const auto& e=node->events[(node->next+History-1-n)%History];if(e.src){parent=e.src;expected=e.src_birth;break;}}
   if(!parent)break;if(!expected){out.reason="parent_birth_unknown";break;}node=Find(parent);
   if(!node)out.reason="parent_evicted_or_untracked";
  }
  if(node&&out.count==MaxNodes)out.reason="node_budget";return out;
 }
};
inline bool Enabled(){static const bool on=Common::DebugDocument::Environment("NXEMU_IMAGE_WRITERS")=="1";return on;}
inline Registry& Global(){static Registry registry(true);return registry;}
inline uint64_t Birth(uint64_t image,uint64_t cpu,uint64_t gpu,uint64_t parent,uint32_t w,uint32_t h)noexcept{return Enabled()?Global().Birth(image,cpu,gpu,parent,w,h):0;}
inline Event Stamp(uint64_t image,Op op,uint64_t parent=0)noexcept{return Enabled()?Global().Stamp(image,op,parent):Event{};}
inline bool Record(Event e)noexcept{return e.valid&&Global().Record(e);}
inline Snapshot Read(uint64_t image)noexcept{return Enabled()?Global().Read(image):Snapshot{};}
inline std::string Serialize(const Snapshot& x){
 if(x.count>MaxNodes)throw std::length_error("writer node count exceeds bound");
 for(size_t n=0;n<x.count;++n)if(x.nodes[n].count>History||x.nodes[n].next>=History)
  throw std::length_error("writer event count exceeds bound");
 std::ostringstream o;o<<"{\"schema\":1,\"available\":"<<(x.available?"true":"false")<<",\"selected_image\":"<<x.selected
 <<",\"reason\":"<<Common::DebugDocument::Quote(x.reason?x.reason:"missing")<<",\"serial\":"<<x.serial<<",\"lock_drops\":"<<x.lock_drops
 <<",\"stale_drops\":"<<x.stale_drops<<",\"missing_drops\":"<<x.missing_drops<<",\"evictions\":"<<x.evictions<<",\"overflow\":"<<x.overflow<<",\"nodes\":[";
 for(size_t n=0;n<x.count;++n){if(n)o<<',';const auto& d=x.nodes[n];
  o<<"{\"image\":"<<d.image<<",\"birth\":"<<d.birth<<",\"cpu_owner\":"<<d.cpu_owner<<",\"gpu_owner\":"<<d.gpu_owner
   <<",\"width\":"<<d.width<<",\"height\":"<<d.height<<",\"parent\":"<<d.parent<<",\"parent_birth\":"<<d.parent_birth
   <<",\"events_total\":"<<d.total<<",\"events_retained\":"<<d.count<<",\"events\":[";
  for(size_t j=0;j<d.count;++j){if(j)o<<',';const auto& e=d.events[(d.next+History-d.count+j)%History];
   o<<"{\"seq\":"<<e.seq<<",\"op\":\""<<Name(e.op)<<"\",\"dst\":"<<e.dst<<",\"dst_birth\":"<<e.dst_birth
    <<",\"src\":"<<e.src<<",\"src_birth\":"<<e.src_birth<<",\"shader_key\":"<<e.shader_key<<",\"hashes\":[";
   for(size_t k=0;k<6;++k){if(k)o<<',';o<<e.hashes[k];}o<<"],\"aux\":[";
   for(size_t k=0;k<4;++k){if(k)o<<',';o<<e.aux[k];}
   o<<"],\"clear_region_known\":"<<(e.clear_region_known?"true":"false")<<",\"clear_rect\":[";
   for(size_t k=0;k<4;++k){if(k)o<<',';o<<e.clear_rect[k];}o<<"],\"clear_layers\":[";
   for(size_t k=0;k<2;++k){if(k)o<<',';o<<e.clear_layers[k];}o<<"]}";
  }o<<"]}";
 }o<<"],\"limits\":{\"slots\":"<<Slots<<",\"probe_slots\":"<<ProbeSlots<<",\"events_per_slot\":"<<History<<",\"chain_nodes\":"<<MaxNodes<<"},\"scope\":\"bounded CPU command-record history, not GPU results; missing/uninstrumented writers remain unknown\"}";
 auto text=o.str();if(text.size()>MaxBytes)throw std::length_error("writer snapshot exceeds bound");return text;
}
} // namespace VideoCommon::ImageWriters
