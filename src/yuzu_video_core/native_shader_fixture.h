// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// CPU-native rejection corpus only. No resource reads, GPU synchronization, or replay claims.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace VideoCommon::NativeShaderFixture {
constexpr std::size_t MaxItems=8, MaxBlobBytes=1024*1024, MaxTotalBytes=4*1024*1024;
constexpr std::uint64_t WriteIntervalMs=500;
bool Enabled();
std::filesystem::path Root();
std::string RunId();
std::string Generation();
bool Replace(const std::filesystem::path& from,const std::filesystem::path& to) noexcept;
struct ConstantBufferSnapshot {
    std::uint64_t address{};
    std::uint32_t size{};
    bool enabled{};
};
struct ComputeSnapshot {
    bool available{}, dispatch_available{};
    std::uint32_t local_memory_size{}, shared_memory_size{}, texture_bound_buffer{};
    std::array<std::uint32_t,3> workgroup_size{}, grid{};
    std::uint64_t dispatch_token{}, dispatch_program{};
    std::uint32_t cbuf_mask{};
    std::array<ConstantBufferSnapshot,8> cbufs{};
};
struct Metadata {
    std::uint64_t program{},error_offset{};
    std::uint32_t stage{},header_size{},opcode{};
    std::array<std::uint32_t,4> words{};
    std::string_view reason,termination;
    ComputeSnapshot compute{};
};
inline std::uint64_t Hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t value=14695981039346656037ULL;
    for(const auto byte:bytes) {value^=byte;value*=1099511628211ULL;}
    return value;
}
inline std::string Hex(std::uint64_t n) {
    std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<n;return s.str();
}
inline std::string Quote(std::string_view value,std::size_t limit=256) {
    std::ostringstream s;s<<'"';
    for(const unsigned char c:value.substr(0,limit)) {
        if(c=='"'||c=='\\') s<<'\\'<<char(c);
        else if(c<32||c>=128) s<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;
        else s<<char(c);
    }
    s<<'"';return s.str();
}
inline std::uint64_t EpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
inline bool Ordinary(const std::filesystem::path& path,bool allow_absent) {
    std::error_code ec;const auto status=std::filesystem::symlink_status(path,ec);
    if(ec==std::errc::no_such_file_or_directory) return allow_absent;
    if(ec) return false;
    return (allow_absent&&!std::filesystem::exists(status))||std::filesystem::is_regular_file(status);
}
inline bool Write(const std::filesystem::path& path,std::span<const std::uint8_t> bytes,bool replace=false) {
    const auto temp=std::filesystem::path(path.string()+".tmp");
    if(!Ordinary(path,true)||!Ordinary(temp,true)||(!replace&&std::filesystem::exists(path))) return false;
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out.close();if(!out)return false;}
    return Replace(temp,path);
}
inline bool Text(const std::filesystem::path& path,const std::string& text,bool replace=false) {
    return Write(path,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()},replace);
}
struct Entry {std::string id,blob,metadata;std::vector<std::uint8_t> bytes;};
struct Registry {
    std::mutex mutex;
    std::string run,generation,last_error;
    std::uint64_t attempts{},duplicates{},quota_drops{},rate_drops{},io_errors{},stored_bytes{},capture_ns{},last_write_ms{},last_publish_ms{};
    std::vector<Entry> entries;
    std::atomic<std::uint64_t> contention_drops{},exception_drops{};
};
inline Registry registry;
inline void Index(const std::filesystem::path& directory,std::uint64_t now,bool force=false) {
    auto& r=registry;if(!force&&r.last_publish_ms&&now-r.last_publish_ms<WriteIntervalMs)return;
    r.last_publish_ms=now;
    std::ostringstream s;s<<"{\"schema\":1,\"enabled\":true,\"run_id\":"<<Quote(r.run)<<",\"module_generation\":"<<Quote(r.generation)
    <<",\"epoch_ms\":"<<now<<",\"attempts\":"<<r.attempts<<",\"duplicates\":"<<r.duplicates<<",\"quota_drops\":"<<r.quota_drops
    <<",\"rate_drops\":"<<r.rate_drops<<",\"contention_drops\":"<<r.contention_drops.load()<<",\"exception_drops\":"<<r.exception_drops.load()<<",\"io_errors\":"<<r.io_errors
    <<",\"stored_bytes\":"<<r.stored_bytes<<",\"capture_ns\":"<<r.capture_ns<<",\"last_error\":"<<Quote(r.last_error)
    <<",\"limits\":{\"max_items\":8,\"max_blob_bytes\":1048576,\"max_total_bytes\":4194304,\"new_write_interval_ms\":500},\"entries\":[";
    for(std::size_t i=0;i<r.entries.size();++i){if(i)s<<',';const auto& e=r.entries[i];s<<"{\"id\":"<<Quote(e.id)<<",\"blob\":"<<Quote(e.blob)<<",\"metadata\":"<<Quote(e.metadata)<<",\"stored_bytes\":"<<e.bytes.size()<<'}';}
    s<<"],\"omissions\":[\"SPH_header_bytes_not_captured\",\"constant_buffers_textures_and_resources_not_captured\",\"not_full_resource_replay\",\"duplicate_metadata_is_first_occurrence_only\",\"index_publication_only_at_rejection_boundaries\",\"byte_budget_not_IO_wallclock_deadline\",\"capture_ns_excludes_index_write\",\"configured_root_ancestors_are_trusted\"]}\n";
    if(!Text(directory/"index.json",s.str(),true)){++r.io_errors;r.last_error="index_publish_failed";}
}
inline void Capture(std::span<const std::uint8_t> bytes,const Metadata& m,std::uint64_t now=EpochMs()) noexcept {
    if(!Enabled())return;
    const auto started=std::chrono::steady_clock::now();
    try {
        auto& r=registry;std::unique_lock lock(r.mutex,std::try_to_lock);
        if(!lock.owns_lock()){++r.contention_drops;return;}
        const auto run=RunId(),gen=Generation();
        if(run.empty()||gen.empty()||run.size()>256||gen.size()>256)return;
        if(r.run!=run||r.generation!=gen) {
            r.run=run;r.generation=gen;r.entries.clear();r.attempts=r.duplicates=r.quota_drops=r.rate_drops=r.io_errors=r.stored_bytes=r.capture_ns=r.last_write_ms=r.last_publish_ms=0;r.last_error.clear();r.contention_drops=0;r.exception_drops=0;
        }
        ++r.attempts;
        const auto root=Root(),directory=root/"native-fixtures";
        std::error_code ec;
        const auto root_status=std::filesystem::symlink_status(root,ec);
        if(ec||!std::filesystem::is_directory(root_status)||std::filesystem::is_symlink(root_status)){++r.io_errors;r.last_error="root_not_ordinary_directory";return;}
        std::filesystem::create_directory(directory,ec);
        const auto status=std::filesystem::symlink_status(directory,ec);
        if(ec||!std::filesystem::is_directory(status)||std::filesystem::is_symlink(status)){++r.io_errors;r.last_error="fixture_directory_not_ordinary";return;}
        const auto finish=[&](bool force=false){r.capture_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();Index(directory,now,force);};
        for(const auto& e:r.entries)if(e.bytes.size()==bytes.size()&&std::equal(e.bytes.begin(),e.bytes.end(),bytes.begin())){++r.duplicates;finish();return;}
        if(bytes.empty()||bytes.size()%16||bytes.size()>MaxBlobBytes||r.entries.size()>=MaxItems||bytes.size()>MaxTotalBytes-r.stored_bytes){++r.quota_drops;r.last_error="empty_unaligned_or_quota";finish();return;}
        if(r.last_write_ms&&now-r.last_write_ms<WriteIntervalMs){++r.rate_drops;finish();return;}
        r.last_write_ms=now;
        const auto context=r.run+"/"+r.generation;
        const auto id=Hex(Hash({reinterpret_cast<const std::uint8_t*>(context.data()),context.size()}))+"-"+std::to_string(r.entries.size())+"-"+Hex(Hash(bytes));
        Entry e{id,"native-"+id+".bin","native-"+id+".json",{bytes.begin(),bytes.end()}};
        std::ostringstream s;s<<"{\"schema\":1,\"status\":\"ready\",\"id\":"<<Quote(id)<<",\"run_id\":"<<Quote(run)<<",\"module_generation\":"<<Quote(gen)
        <<",\"epoch_ms\":"<<now<<",\"blob\":"<<Quote(e.blob)<<",\"program\":"<<m.program<<",\"stage\":"<<m.stage<<",\"header_size\":"<<m.header_size
        <<",\"error_offset\":"<<m.error_offset<<",\"opcode\":"<<m.opcode<<",\"words\":["<<m.words[0]<<','<<m.words[1]<<','<<m.words[2]<<','<<m.words[3]
        <<"],\"reason\":"<<Quote(m.reason)<<",\"reason_truncated\":"<<(m.reason.size()>256?"true":"false")<<",\"capture_termination\":"<<Quote(m.termination)
        <<",\"captured_instruction_count\":"<<bytes.size()/16<<",\"source_bytes\":"<<bytes.size()<<",\"stored_bytes\":"<<bytes.size()
        <<",\"truncated\":false,\"capture_state_complete\":"<<(m.termination=="complete"?"true":"false")
        <<",\"compute_environment\":{\"available\":"<<(m.compute.available?"true":"false")
        <<",\"source\":\"GPU_owner_environment_before_native_lower_rejection\""
        <<",\"local_memory_size\":"<<m.compute.local_memory_size
        <<",\"shared_memory_size\":"<<m.compute.shared_memory_size
        <<",\"texture_bound_buffer\":"<<m.compute.texture_bound_buffer<<",\"workgroup_size\":[";
        for(size_t i=0;i<3;++i)s<<(i?",":"")<<m.compute.workgroup_size[i];
        s<<"],\"dispatch_available\":"<<(m.compute.dispatch_available?"true":"false")
         <<",\"dispatch_token\":"<<m.compute.dispatch_token<<",\"dispatch_program\":"<<m.compute.dispatch_program
         <<",\"grid\":[";
        for(size_t i=0;i<3;++i)s<<(i?",":"")<<m.compute.grid[i];
        s<<"],\"cbuf_mask\":"<<m.compute.cbuf_mask<<",\"constant_buffers\":[";
        for(size_t i=0;i<m.compute.cbufs.size();++i){const auto& b=m.compute.cbufs[i];
            s<<(i?",":"")<<"{\"slot\":"<<i<<",\"enabled\":"<<(b.enabled?"true":"false")
             <<",\"address\":"<<b.address<<",\"size\":"<<b.size<<"}";}
        s<<"],\"resource_bytes_captured\":false,\"GPU_visibility_proven\":false}"
        <<",\"encoding\":\"little_endian_u32x4_native_instructions\",\"hash_algorithm\":\"fnv1a64-nonauthenticating\",\"content_id\":"<<Quote(Hex(Hash(bytes)))
        <<",\"omissions\":[\"SPH_header_bytes_not_captured\",\"constant_buffers_textures_and_resources_not_captured\",\"CPU_capture_complete_does_not_prove_full_guest_program\",\"not_full_resource_replay\"]}\n";
        if(!Write(directory/e.blob,bytes)||!Text(directory/e.metadata,s.str())){++r.io_errors;r.last_error="blob_or_metadata_publish_failed";finish(true);return;}
        r.stored_bytes+=bytes.size();r.entries.emplace_back(std::move(e));r.last_error.clear();finish(true);
    }catch(...){++registry.exception_drops;/* Diagnostics never change shader rejection semantics. */}
}
} // namespace VideoCommon::NativeShaderFixture
