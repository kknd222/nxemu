// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Explicit CPU-side draw input snapshots. No shader execution or numerical GPU oracle.
#include "yuzu_common/debug_document.h"
#include "yuzu_video_core/native_draw_invocation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

namespace VideoCommon::NativeDrawInputs {
inline constexpr size_t MaxItems=4,MaxNativeBytes=4096,HeaderBytes=128,RegisterBytes=0x3800,
                        MaxCbufBytes=1024,ParsedHeaderBytes=80,MaxRawBytes=MaxNativeBytes+HeaderBytes+ParsedHeaderBytes+RegisterBytes+MaxCbufBytes;
inline bool Enabled(){
    static const bool on=Common::DebugDocument::Environment("NXEMU_NATIVE_DRAW_INPUTS")=="1" &&
                         Common::DebugDocument::Environment("NXEMU_NATIVE_SHADER_FIXTURES")=="1";
    return on;
}
struct IndexPrefix {
    bool requested{},range_valid{},mapped{};
    uint64_t address{},limit_address{};
    uint32_t element_size{},count{},first{},bytes{};
    std::array<uint8_t,16> prefix{};
    bool operator==(const IndexPrefix&) const = default;
};
inline bool ValidIndex(const IndexPrefix& i){
    if(!i.requested)return i==IndexPrefix{};
    if(i.element_size!=1&&i.element_size!=2&&i.element_size!=4)return false;
    if(i.bytes!=std::min<uint64_t>(uint64_t{i.count}*i.element_size,16))return false;
    if(i.mapped&&!i.range_valid)return false;
    // A zero retained limit means unavailable; CPU capture still requires count/address overflow checks and mapping.
    if(i.range_valid&&(!i.bytes||!i.address||i.bytes-1>UINT64_MAX-i.address||
       (i.limit_address&&(i.address>i.limit_address||i.bytes-1>i.limit_address-i.address))))return false;
    for(size_t n=i.mapped?i.bytes:0;n<16;++n)if(i.prefix[n])return false;
    return true;
}
inline std::string IndexJson(const IndexPrefix& i){
    std::ostringstream o;o<<"{\"requested\":"<<(i.requested?"true":"false")<<",\"range_valid\":"<<(i.range_valid?"true":"false")<<",\"mapped\":"<<(i.mapped?"true":"false")
      <<",\"address\":"<<i.address<<",\"limit_address\":"<<i.limit_address<<",\"element_size\":"<<i.element_size<<",\"count\":"<<i.count<<",\"first\":"<<i.first<<",\"bytes_requested\":"<<i.bytes<<",\"prefix\":[";
    for(size_t n=0;n<(i.mapped?i.bytes:0);++n){if(n)o<<',';o<<unsigned(i.prefix[n]);}o<<"]}";return o.str();
}
struct Input {
    IndexPrefix index_input{};
    NativeDrawInvocation::Snapshot invocation{};
    uint64_t token{},program{};
    uint32_t stage{},header_size{},error_offset{},opcode{};
    std::array<uint32_t,4> words{};
    std::span<const uint8_t> native,header,regs,cbuf;
    uint64_t cbuf_address{};
    uint32_t cbuf_declared_size{};
    bool cbuf_enabled{},cbuf_fully_mapped{};
    std::string_view reason;
    uint32_t cbuf_bank{},cbuf_offset{};
    std::span<const uint8_t> parsed_header;bool header_fully_mapped{};
};
inline uint64_t Hash(std::span<const uint8_t> bytes){
    uint64_t hash=14695981039346656037ULL;for(uint8_t byte:bytes){hash^=byte;hash*=1099511628211ULL;}return hash;
}
inline std::string Hex(uint64_t number){std::ostringstream out;out<<std::hex<<std::setw(16)<<std::setfill('0')<<number;return out.str();}
inline bool Digits(std::string_view value){return !value.empty()&&value.size()<=64&&std::all_of(value.begin(),value.end(),[](char c){return c>='0'&&c<='9';});}
inline bool Valid(const Input& in){
    if(!ValidIndex(in.index_input)||!NativeDrawInvocation::Valid(in.invocation))return false;
    if(!in.token||!in.program||(in.stage!=0&&in.stage!=4)||in.header_size!=HeaderBytes ||
       in.native.empty()||in.native.size()>MaxNativeBytes||in.native.size()%16 ||
       in.parsed_header.size()!=ParsedHeaderBytes||
       (in.header_fully_mapped ? in.header.size()!=HeaderBytes : !in.header.empty())||
       in.regs.size()!=RegisterBytes||in.cbuf.size()>MaxCbufBytes||
       in.error_offset%16||in.error_offset>in.native.size()-16||in.opcode>0xfff||in.cbuf_bank>=18)
        return false;
    if((!in.cbuf_enabled||!in.cbuf_fully_mapped)&&!in.cbuf.empty())return false;
    if(!in.cbuf_enabled&&in.cbuf_fully_mapped)return false;
    if(in.cbuf.size()>in.cbuf_declared_size||in.cbuf_offset>in.cbuf_declared_size||
       in.cbuf.size()>in.cbuf_declared_size-in.cbuf_offset)return false;
    if(in.cbuf_enabled&&in.cbuf_fully_mapped&&!in.cbuf_address)return false;
    // Tie the reported rejected words to the exact captured native instruction.
    for(size_t n=0;n<4;++n){uint32_t word{};for(size_t b=0;b<4;++b)word|=uint32_t(in.native[in.error_offset+n*4+b])<<(8*b);if(word!=in.words[n])return false;}
    return (in.words[0]&0xfff)==in.opcode;
}
struct Entry {
    bool occupied{},ready{};std::vector<uint8_t> bytes;std::string run,generation,root,invocation_json,index_json;
    uint32_t stage{},cbuf_bank{},cbuf_offset{},cbuf_declared_size{};
    uint64_t cbuf_address{};bool cbuf_enabled{},cbuf_fully_mapped{};
    std::array<size_t,5> sizes{};bool header_fully_mapped{};
};
struct Registry {
    std::mutex mutex;std::array<Entry,MaxItems> entries{};size_t reserved{};
    std::atomic<uint64_t> attempts{},duplicates{},invalid{},budget_drops{},lock_drops{},io_errors{},
                          exceptions{},ready{},charged_raw_bytes{},reserved_count{},claimed_count{},invalid_reservation{},claimed_mask{};
};
inline Registry registry;
inline bool OrdinaryDirectory(const std::filesystem::path& path){
    std::error_code ec;const auto status=std::filesystem::symlink_status(path,ec);
    return !ec&&std::filesystem::is_directory(status)&&!std::filesystem::is_symlink(status);
}
inline bool Absent(const std::filesystem::path& path){
    std::error_code ec;const auto status=std::filesystem::symlink_status(path,ec);
    return ec==std::errc::no_such_file_or_directory||(!ec&&!std::filesystem::exists(status));
}
inline bool WriteNew(const std::filesystem::path& path,std::span<const uint8_t> bytes){
    auto temp=path;temp+=".tmp";
    if(!Absent(path)||!Absent(temp))return false;
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);
     if(!out)return false;if(!bytes.empty())out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
     out.close();if(!out)return false;}
    // Exclusive target creation; unlike rename this never replaces an existing target.
    std::error_code ec;std::filesystem::create_hard_link(temp,path,ec);if(ec)return false;
    std::filesystem::remove(temp,ec);return true;
}
inline bool Same(const Entry& entry,const Input& in,const std::vector<uint8_t>& bytes){
    return entry.occupied&&entry.index_json==IndexJson(in.index_input)&&entry.invocation_json==NativeDrawInvocation::Json(in.invocation)&&entry.stage==in.stage&&entry.cbuf_bank==in.cbuf_bank&&
           entry.cbuf_offset==in.cbuf_offset&&entry.cbuf_declared_size==in.cbuf_declared_size&&
           entry.cbuf_address==in.cbuf_address&&entry.cbuf_enabled==in.cbuf_enabled&&
           entry.cbuf_fully_mapped==in.cbuf_fully_mapped&&
           entry.header_fully_mapped==in.header_fully_mapped&&
           entry.sizes==std::array<size_t,5>{in.native.size(),in.header.size(),in.parsed_header.size(),in.regs.size(),in.cbuf.size()}&&entry.bytes==bytes;
}
inline std::string CountersJson(){
    const auto claimed=registry.claimed_count.load(),reserved=registry.reserved_count.load();
    std::ostringstream out;out<<"{\"schema\":1,\"scope\":\"module_load_lifetime_no_reload_reset\",\"attempts\":"<<registry.attempts.load()
        <<",\"duplicates\":"<<registry.duplicates.load()<<",\"invalid\":"<<registry.invalid.load()<<",\"budget_drops\":"<<registry.budget_drops.load()
        <<",\"lock_drops\":"<<registry.lock_drops.load()<<",\"io_errors\":"<<registry.io_errors.load()<<",\"exceptions\":"<<registry.exceptions.load()
        <<",\"ready\":"<<registry.ready.load()<<",\"charged_raw_bytes\":"<<registry.charged_raw_bytes.load()
        <<",\"reserved\":"<<reserved<<",\"claimed\":"<<claimed
        <<",\"invalid_reservation\":"<<registry.invalid_reservation.load()<<",\"unclaimed_reservations\":"<<(reserved-claimed)
        <<",\"max_items\":4,\"max_raw_bytes_per_item\":"<<MaxRawBytes<<"}";return out.str();
}
// Claim a bounded attempt BEFORE the caller makes additional CPU memory reads.
// Slots are never refunded or reset by a reload; duplicate/failed captures consume an attempt.
inline uint64_t Reserve()noexcept{
    if(!Enabled())return 0;++registry.attempts;
    try{
        const auto run=Common::DebugDocument::Environment("NXEMU_DEBUG_RUN_ID");
        const auto generation=Common::DebugDocument::Environment("NXEMU_DEBUG_GENERATION");
        const auto root_text=Common::DebugDocument::Environment("NXEMU_NATIVE_SHADER_DIAG_DIR");
        if(!Digits(run)||!Digits(generation)||root_text.empty()){++registry.invalid;return 0;}
        const auto root=std::filesystem::u8path(root_text);
        if(!root.is_absolute()||!OrdinaryDirectory(root)){++registry.invalid;return 0;}
        std::unique_lock lock(registry.mutex,std::try_to_lock);if(!lock.owns_lock()){++registry.lock_drops;return 0;}
        if(registry.reserved==MaxItems){++registry.budget_drops;return 0;}
        const size_t ordinal=registry.reserved++;++registry.reserved_count;auto& entry=registry.entries[ordinal];
        entry.occupied=true;entry.run=run;entry.generation=generation;entry.root=root_text;
        return ordinal+1;
    }catch(...){++registry.exceptions;return 0;}
}
inline bool Capture(const Input& in,uint64_t reservation)noexcept{
    if(!Enabled())return false;
    if(!reservation||reservation>MaxItems||reservation>registry.reserved_count.load()){
        ++registry.invalid_reservation;return false;
    }
    const uint64_t bit=uint64_t{1}<<(reservation-1);
    if(registry.claimed_mask.fetch_or(bit)&bit){++registry.invalid_reservation;return false;}
    ++registry.claimed_count;
    try{
        const size_t ordinal=size_t(reservation-1);auto& reserved=registry.entries[ordinal];
        const auto run=Common::DebugDocument::Environment("NXEMU_DEBUG_RUN_ID");
        const auto generation=Common::DebugDocument::Environment("NXEMU_DEBUG_GENERATION");
        const auto root_text=Common::DebugDocument::Environment("NXEMU_NATIVE_SHADER_DIAG_DIR");
        if(!Valid(in)||run!=reserved.run||generation!=reserved.generation||root_text!=reserved.root){++registry.invalid;return false;}
        const auto root=std::filesystem::u8path(root_text);
        if(!root.is_absolute()||!OrdinaryDirectory(root)){++registry.invalid;return false;}
        std::vector<uint8_t> bytes;bytes.reserve(in.native.size()+in.header.size()+in.parsed_header.size()+in.regs.size()+in.cbuf.size());
        for(auto data:{in.native,in.header,in.parsed_header,in.regs,in.cbuf})bytes.insert(bytes.end(),data.begin(),data.end());
        {
            std::unique_lock lock(registry.mutex,std::try_to_lock);if(!lock.owns_lock()){++registry.lock_drops;return false;}
            for(const auto& entry:registry.entries)if(Same(entry,in,bytes)){++registry.duplicates;return false;}
            auto& entry=registry.entries[ordinal];entry.bytes=std::move(bytes);
            entry.invocation_json=NativeDrawInvocation::Json(in.invocation);entry.index_json=IndexJson(in.index_input);
            entry.stage=in.stage;entry.cbuf_bank=in.cbuf_bank;entry.cbuf_offset=in.cbuf_offset;entry.cbuf_declared_size=in.cbuf_declared_size;
            entry.cbuf_address=in.cbuf_address;entry.cbuf_enabled=in.cbuf_enabled;entry.cbuf_fully_mapped=in.cbuf_fully_mapped;entry.header_fully_mapped=in.header_fully_mapped;
            entry.sizes={in.native.size(),in.header.size(),in.parsed_header.size(),in.regs.size(),in.cbuf.size()};
            registry.charged_raw_bytes+=entry.bytes.size();
        }
        // Reservations and failed publication attempts keep their module-lifetime charge.
        // IO happens outside the registry lock; no more than four writers can be admitted.
        const auto directory=root/"native-draw-inputs";std::error_code ec;
        std::filesystem::create_directory(directory,ec);
        if(!OrdinaryDirectory(directory)){++registry.io_errors;return false;}
        const auto id="draw-input-"+run+"-"+generation+"-"+std::to_string(in.token)+"-"+std::to_string(ordinal);
        std::ostringstream manifest;manifest<<"{\"schema\":1,\"status\":\"ready\",\"id\":"<<Common::DebugDocument::Quote(id)
          <<",\"run_id\":"<<Common::DebugDocument::Quote(run)<<",\"generation\":"<<Common::DebugDocument::Quote(generation)
          <<",\"epoch_ms\":"<<Common::DebugDocument::EpochMs()<<",\"token\":"<<in.token<<",\"program\":"<<in.program
          <<",\"stage\":"<<in.stage<<",\"header_size\":"<<in.header_size<<",\"error_offset\":"<<in.error_offset<<",\"opcode\":"<<in.opcode
          <<",\"words\":["<<in.words[0]<<','<<in.words[1]<<','<<in.words[2]<<','<<in.words[3]<<"]"
          <<",\"reason\":"<<Common::DebugDocument::Quote(std::string(in.reason.substr(0,192)))
          <<",\"header_fully_mapped\":"<<(in.header_fully_mapped?"true":"false")
          <<",\"parsed_header_matches_raw\":"<<(in.header_fully_mapped? (std::equal(in.parsed_header.begin(),in.parsed_header.end(),in.header.begin())?"true":"false"):"null")
          <<",\"reason_truncated\":"<<(in.reason.size()>192?"true":"false")<<",\"cbuf\":{\"bank\":"<<in.cbuf_bank
          <<",\"offset\":"<<in.cbuf_offset<<",\"address\":"<<in.cbuf_address<<",\"declared_size\":"<<in.cbuf_declared_size
          <<",\"enabled\":"<<(in.cbuf_enabled?"true":"false")<<",\"fully_mapped\":"<<(in.cbuf_fully_mapped?"true":"false")<<"},\"files\":{";
        size_t field{};const std::array<std::string_view,5> names{"native","header","parsed_header","regs","cbuf"};
        for(auto data:{in.native,in.header,in.parsed_header,in.regs,in.cbuf}){
            const auto name=std::string(names[field]);const auto filename=id+"."+name+".bin";
            if(!WriteNew(directory/filename,data)){++registry.io_errors;return false;}
            if(field)manifest<<',';manifest<<Common::DebugDocument::Quote(name)<<":{\"file\":"<<Common::DebugDocument::Quote(filename)
                <<",\"bytes\":"<<data.size()<<",\"fnv1a64\":"<<Common::DebugDocument::Quote(Hex(Hash(data)))<<"}";++field;
        }
        manifest<<"},\"invocation\":"<<NativeDrawInvocation::Json(in.invocation)<<",\"index_input\":"<<IndexJson(in.index_input)<<",\"limits\":{\"module_load_items\":4,\"native\":4096,\"header\":128,\"parsed_header\":80,\"regs\":14336,\"cbuf\":1024},\"scope\":\"CPU-side input snapshots linked by draw admission token; not executed shader inputs or a GPU numerical oracle\",\"omissions\":[\"GPU_visibility_not_verified\",\"vertex_streams_and_textures_not_captured\",\"cbuf_is_bounded_slice_not_full_binding\",\"native_program_capture_may_be_incomplete\",\"no_wscale_or_csmtest_numeric_oracle\",\"configured_root_ancestors_are_trusted\",\"filesystem_IO_has_no_in_call_deadline\"]}";
        const auto text=manifest.str();
        if(!WriteNew(directory/(id+".json"),{reinterpret_cast<const uint8_t*>(text.data()),text.size()})){++registry.io_errors;return false;}
        ++registry.ready;return true;
    }catch(...){++registry.exceptions;return false;}
}
inline bool Capture(const Input& in)noexcept{
    if(!Enabled())return false;
    if(!Valid(in)){++registry.attempts;++registry.invalid;return false;}
    const auto reservation=Reserve();return reservation&&Capture(in,reservation);
}
} // namespace VideoCommon::NativeDrawInputs
