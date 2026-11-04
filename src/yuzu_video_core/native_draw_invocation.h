// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// CPU retained draw state, not decoded GPU arguments or actual indexed VertexId.
#include "yuzu_common/debug_document.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
namespace VideoCommon::NativeDrawInvocation {
enum class Kind : uint32_t { Unavailable=0, Direct=1, Indirect=2 };
struct Snapshot {
    bool available{},is_indexed{},instance_count_available{},retained_draw_indexed{};
    Kind kind{};
    uint32_t instance_count{},retained_instance_count{},topology{},draw_mode{},vertex_first{},vertex_count{},
        index_first{},index_count{},base_index{},base_instance{},index_format{},inline_prefix_size{};
    uint64_t index_address{},inline_bytes_size{};
    std::array<uint8_t,16> inline_prefix{};
    bool is_byte_count{},include_count{};
    uint64_t count_address{},indirect_address{},buffer_size{},max_draw_count{},stride{};
    uint32_t instance_mask{};
    bool operator==(const Snapshot&) const = default;
};
static_assert(std::is_trivially_copyable_v<Snapshot> && std::is_standard_layout_v<Snapshot>);
inline bool Enabled() {
    static const bool enabled=Common::DebugDocument::Environment("NXEMU_NATIVE_DRAW_INPUTS")=="1" &&
        Common::DebugDocument::Environment("NXEMU_NATIVE_SHADER_FIXTURES")=="1";
    return enabled;
}
inline bool Valid(const Snapshot& s) noexcept {
    if(!s.available)return s==Snapshot{};
    if(s.kind!=Kind::Direct && s.kind!=Kind::Indirect)return false;
    if(s.draw_mode>2 || s.index_format>2 || s.inline_prefix_size>16 ||
       s.inline_prefix_size!=std::min<uint64_t>(s.inline_bytes_size,16))return false;
    for(size_t n=s.inline_prefix_size;n<16;++n)if(s.inline_prefix[n])return false;
    if(s.kind==Kind::Direct)return s.instance_count_available && !s.is_byte_count && !s.include_count &&
        !s.count_address && !s.indirect_address && !s.buffer_size && !s.max_draw_count && !s.stride && !s.instance_mask;
    return !s.instance_count_available && !s.instance_count;
}
template<class State> inline Snapshot Retained(const State& state) noexcept {
    Snapshot s{};s.available=true;s.topology=static_cast<uint32_t>(state.topology);
    s.draw_mode=static_cast<uint32_t>(state.draw_mode);s.retained_draw_indexed=state.draw_indexed;
    s.retained_instance_count=state.instance_count;s.vertex_first=state.vertex_buffer.first;
    s.vertex_count=state.vertex_buffer.count;s.index_first=state.index_buffer.first;
    s.index_count=state.index_buffer.count;s.base_index=state.base_index;s.base_instance=state.base_instance;
    s.index_format=static_cast<uint32_t>(state.index_buffer.format);
    s.index_address=(uint64_t{state.index_buffer.start_addr_high}<<32)|state.index_buffer.start_addr_low;
    s.inline_bytes_size=state.inline_index_draw_indexes.size();
    s.inline_prefix_size=static_cast<uint32_t>(std::min<uint64_t>(s.inline_bytes_size,16));
    std::copy_n(state.inline_index_draw_indexes.begin(),s.inline_prefix_size,s.inline_prefix.begin());return s;
}
template<class State> inline Snapshot MakeDirect(const State& state,bool is_indexed,uint32_t instance_count) noexcept {
    if(!Enabled())return {};auto s=Retained(state);s.kind=Kind::Direct;s.is_indexed=is_indexed;
    s.instance_count_available=true;s.instance_count=instance_count;return s;
}
template<class State,class Params> inline Snapshot MakeIndirect(const State& state,const Params& params) noexcept {
    if(!Enabled())return {};auto s=Retained(state);s.kind=Kind::Indirect;s.is_indexed=params.is_indexed;
    s.is_byte_count=params.is_byte_count;s.include_count=params.include_count;
    s.count_address=params.count_start_address;s.indirect_address=params.indirect_start_address;
    s.buffer_size=params.buffer_size;s.max_draw_count=params.max_draw_counts;s.stride=params.stride;
    s.instance_mask=params.instance_mask;return s;
}
inline thread_local Snapshot current{};
inline Snapshot Current() noexcept {return Enabled()?current:Snapshot{};}
class Scope {
    Snapshot previous{};bool active{};
public:
    explicit Scope(const Snapshot& snapshot) noexcept :active(Enabled()) {
        if(active){previous=current;current=snapshot;}
    }
    ~Scope(){if(active)current=previous;}
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    Scope(Scope&&)=delete;Scope& operator=(Scope&&)=delete;
};
inline std::string Json(const Snapshot& s) {
    if(!Valid(s))return "{\"schema\":1,\"available\":false,\"invalid\":true}";
    if(!s.available)return "{\"schema\":1,\"available\":false,\"kind\":0}";
    std::ostringstream o;o<<"{\"schema\":1,\"available\":true,\"kind\":"<<static_cast<uint32_t>(s.kind);
#define NDI_NUM(field) o<<",\"" #field "\":"<<s.field
#define NDI_BOOL(field) o<<",\"" #field "\":"<<(s.field?"true":"false")
    NDI_BOOL(is_indexed);NDI_BOOL(instance_count_available);NDI_NUM(instance_count);
    NDI_BOOL(retained_draw_indexed);NDI_NUM(retained_instance_count);NDI_NUM(topology);NDI_NUM(draw_mode);
    NDI_NUM(vertex_first);NDI_NUM(vertex_count);NDI_NUM(index_first);NDI_NUM(index_count);
    NDI_NUM(base_index);NDI_NUM(base_instance);NDI_NUM(index_format);NDI_NUM(index_address);
    NDI_NUM(inline_bytes_size);NDI_NUM(inline_prefix_size);o<<",\"inline_prefix\":[";
    for(size_t n=0;n<s.inline_prefix_size;++n){if(n)o<<',';o<<unsigned(s.inline_prefix[n]);}o<<']';
    NDI_BOOL(is_byte_count);NDI_BOOL(include_count);NDI_NUM(count_address);NDI_NUM(indirect_address);
    NDI_NUM(buffer_size);NDI_NUM(max_draw_count);NDI_NUM(stride);NDI_NUM(instance_mask);
#undef NDI_NUM
#undef NDI_BOOL
    o<<",\"indexed_vertex_id_decoded\":false,\"indirect_arguments_decoded\":false,\"scope\":\"cpu_retained_state_and_caller_arguments\"}";return o.str();
}
} // namespace VideoCommon::NativeDrawInvocation
