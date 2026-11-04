// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Request-only metadata on the renderer owner thread; no GPU calls or platform dependencies.
#include <array>
#include <algorithm>
#include <span>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
namespace VideoCommon::FrameCaptureContext {
inline constexpr size_t MaxBytes=16384,MaxLayers=4,MaxSources=16;
enum class Role { Capture, Swapchain, Other };
struct Input {
 uint64_t address{},offset{};uint32_t width{},height{},stride{},format{},transform{},blend{},block_height{};
 std::array<int32_t,4> crop{};bool native{};
};
struct Selection {
 bool valid{},address_space_valid{},scaled{};uint64_t query{},image_id{},view_id{},cpu_owner{},gpu_owner{},page_table{},address_space{},modification{},published{},gpu_write{};
 uint32_t width{},height{},format{},flags{};int32_t layer{};
};
struct Binding {
 bool accelerated{},fsr{};uint64_t source_image{},source_view{},descriptor_image{},descriptor_view{},descriptor_set{},sampler{},resource_index{};
 uint32_t texture_width{},texture_height{},scaled_width{},scaled_height{},output_width{},output_height{};
 std::array<float,4> crop{};
};
struct Source {Role role{};uint32_t layer{};Selection selected;Binding binding;};
struct Data {
 std::string id,run,generation,path="exception_or_unclassified_return";uint64_t composite{},snapshot_epoch_ms{};
 bool shown{},hold_checked{},held{};uint64_t target_image{},target_view{};
 std::array<Input,MaxLayers> inputs{};size_t input_count{},input_total{};
 std::array<Source,MaxSources> sources{};size_t source_count{},sources_dropped{};
 Role role=Role::Capture;std::array<uint32_t,3> next_layer{};uint32_t current_layer{};Selection pending{};
};
inline thread_local Data* current{};
inline bool Active() noexcept{return current!=nullptr;}
template<class T> uint64_t Handle(T v) noexcept {if constexpr(std::is_pointer_v<T>)return reinterpret_cast<uintptr_t>(v);else return static_cast<uint64_t>(v);}
inline const char* RoleName(Role r) noexcept {switch(r){case Role::Capture:return "offscreen_recomposition";case Role::Swapchain:return "swapchain_draw";default:return "other_draw";}}
inline std::string Quote(std::string_view t){std::ostringstream s;s<<'"';for(unsigned char c:t){if(c=='"'||c=='\\')s<<'\\'<<char(c);else if(c<32)s<<'?';else s<<char(c);}s<<'"';return s.str();}
inline bool Safe(std::string_view id) noexcept{return !id.empty()&&id.size()<=64&&id.find_first_not_of("0123456789abcdef")==std::string_view::npos;}
inline void SetRole(Role r) noexcept{if(current)current->role=static_cast<size_t>(r)<3?r:Role::Other;}
inline void BeginLayer() noexcept {if(current){current->pending={};current->current_layer=current->next_layer[static_cast<size_t>(current->role)]++;}}
inline void Selected(const Selection& s) noexcept{if(current&&current->current_layer<MaxLayers)current->pending=s;}
inline void Bound(const Binding& b) noexcept{
 if(!current)return;auto& d=*current;
 if(d.current_layer>=MaxLayers||d.source_count>=MaxSources){++d.sources_dropped;return;}
 d.sources[d.source_count++]={d.role,d.current_layer,d.pending,b};
}
inline void Path(const char* path,bool shown,bool hold_checked=false,bool held=false) noexcept {
 if(!current)return;try{current->path=path;current->shown=shown;current->hold_checked=hold_checked;current->held=held;}catch(...){}
}
inline void Target(uint64_t image,uint64_t view) noexcept{if(current){current->target_image=image;current->target_view=view;}}
inline std::string Serialize(const Data& d){
 if(d.input_count>MaxLayers||d.source_count>MaxSources)throw std::runtime_error("context record budget exceeded");
 std::ostringstream s;s<<std::setprecision(17)<<"{\"schema\":1,\"request_id\":"<<Quote(d.id)<<",\"run_id\":"<<Quote(d.run)<<",\"generation\":"<<Quote(d.generation)<<",\"snapshot_epoch_ms\":"<<d.snapshot_epoch_ms<<",\"composite_ordinal\":"<<d.composite
 <<",\"return_path\":"<<Quote(d.path)<<",\"window_shown\":"<<(d.shown?"true":"false")<<",\"hold_checked\":"<<(d.hold_checked?"true":"false")<<",\"held\":"<<(d.held?"true":"false")
 <<",\"target_image\":"<<d.target_image<<",\"target_view\":"<<d.target_view<<",\"input_total\":"<<d.input_total<<",\"input_count\":"<<d.input_count<<",\"inputs\":[";
 for(size_t n=0;n<d.input_count;++n){if(n)s<<',';auto& i=d.inputs[n];s<<"{\"layer\":"<<n<<",\"address\":"<<i.address<<",\"offset\":"<<i.offset<<",\"width\":"<<i.width<<",\"height\":"<<i.height<<",\"stride\":"<<i.stride<<",\"format\":"<<i.format<<",\"transform\":"<<i.transform<<",\"blend\":"<<i.blend<<",\"native\":"<<(i.native?"true":"false")<<",\"block_height_log2\":"<<i.block_height<<",\"crop\":["<<i.crop[0]<<','<<i.crop[1]<<','<<i.crop[2]<<','<<i.crop[3]<<"]}";}
 s<<"],\"source_count\":"<<d.source_count<<",\"sources_dropped\":"<<d.sources_dropped<<",\"sources\":[";
 for(size_t n=0;n<d.source_count;++n){if(n)s<<',';auto& a=d.sources[n];auto& t=a.selected;auto& b=a.binding;
 s<<"{\"role\":"<<Quote(RoleName(a.role))<<",\"layer\":"<<a.layer<<",\"accelerated\":"<<(b.accelerated?"true":"false")<<",\"selection_valid\":"<<(t.valid?"true":"false")
 <<",\"query_address\":"<<t.query<<",\"image_id\":"<<t.image_id<<",\"view_id\":"<<t.view_id<<",\"cpu_owner\":"<<t.cpu_owner<<",\"gpu_owner\":"<<t.gpu_owner<<",\"page_table\":"<<t.page_table
 <<",\"address_space\":"<<(t.address_space_valid?std::to_string(t.address_space):"null")<<",\"selected_layer\":"<<t.layer<<",\"selected_format\":"<<t.format<<",\"selected_width\":"<<t.width<<",\"selected_height\":"<<t.height<<",\"flags\":"<<t.flags
 <<",\"modification_tick\":"<<t.modification<<",\"published_tick\":"<<t.published<<",\"gpu_write_tick\":"<<t.gpu_write<<",\"selected_scaled\":"<<(t.scaled?"true":"false")
 <<",\"source_image\":"<<b.source_image<<",\"source_view\":"<<b.source_view<<",\"descriptor_image_after_AA\":"<<b.descriptor_image<<",\"descriptor_view\":"<<b.descriptor_view<<",\"descriptor_set\":"<<b.descriptor_set<<",\"sampler\":"<<b.sampler<<",\"resource_index\":"<<b.resource_index
 <<",\"texture_width\":"<<b.texture_width<<",\"texture_height\":"<<b.texture_height<<",\"scaled_width\":"<<b.scaled_width<<",\"scaled_height\":"<<b.scaled_height<<",\"output_width\":"<<b.output_width<<",\"output_height\":"<<b.output_height<<",\"FSR\":"<<(b.fsr?"true":"false")<<",\"crop\":[";
 for(size_t k=0;k<4;++k){if(k)s<<',';if(std::isfinite(b.crop[k]))s<<b.crop[k];else s<<"null";}s<<"]}";
 }
 s<<"],\"limits\":{\"layers_per_role\":4,\"sources\":16,\"bytes\":16384},\"slot_generation_not_captured\":true,\"handles_only_same_process\":true,\"ticks_are_not_GPU_completion\":true,\"applet_before_request_not_captured\":true,\"no_shader_writer_causality\":true,\"no_actual_target_pixel_readback\":true,\"publication_at_Composite_scope_exit\":true}";
 auto out=s.str();if(out.size()>MaxBytes)throw std::runtime_error("capture context byte budget exceeded");return out;
}
class Scope {
 std::filesystem::path root;Data* previous{};bool installed{};
 public:std::unique_ptr<Data> data;
 Scope(std::filesystem::path root_,std::string id,std::string run,std::string generation,uint64_t composite,bool shown):root(std::move(root_)),previous(current),data(std::make_unique<Data>()) {
  if(!root.is_absolute()||!Safe(id)||run.empty()||run.size()>64||generation.empty()||generation.size()>20||generation.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("invalid context session/root");
  data->id=std::move(id);data->run=std::move(run);data->generation=std::move(generation);data->composite=composite;data->shown=shown;data->snapshot_epoch_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();current=data.get();installed=true;
 }
 void Inputs(std::span<const Input> inputs) noexcept {data->input_total=inputs.size();data->input_count=std::min(inputs.size(),MaxLayers);for(size_t n=0;n<data->input_count;++n)data->inputs[n]=inputs[n];}
 ~Scope() noexcept {
  if(installed)current=previous;
  try{auto text=Serialize(*data);auto p=root/("frame-capture-"+data->id+".context.json"),tmp=p;tmp+=".tmp";
   {std::ofstream f(tmp,std::ios::binary|std::ios::trunc);f<<text;if(!f)return;}
   std::error_code ec;std::filesystem::rename(tmp,p,ec);
  }catch(...){}
 }
 Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
} // namespace VideoCommon::FrameCaptureContext
