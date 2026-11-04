// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_shader_recompiler/ssa_fixture_capture.h"
#include "yuzu_shader_recompiler/ssa_fixture.h"
#include "yuzu_common/debug_document.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace Shader::SsaFixture {
namespace {
thread_local Scope* current{};
std::atomic<bool> claimed{}; // One pair per module load, reload never resets.
std::uint64_t Now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
std::string Hex(std::uint64_t n){std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<n;return s.str();}
std::string Content(std::span<const std::uint8_t> bytes){std::uint64_t h=14695981039346656037ull;for(auto b:bytes){h^=b;h*=1099511628211ull;}return Hex(h);}
void Write(const std::filesystem::path& path,std::span<const std::uint8_t> bytes){
    auto temp=path;temp+=".tmp";
    {std::ofstream f(temp,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));if(!f)throw std::runtime_error("SSA fixture write failed");}
    std::error_code ec;std::filesystem::rename(temp,path,ec);if(ec)throw std::runtime_error("SSA fixture publish failed");
}
}
struct CaptureState {
    IdentityMap ids;
    std::filesystem::path root;
    std::string id,run,generation,error,pre_content,post_content;
    std::uint64_t key{},unique_hash{},pre_bytes{},post_bytes{},pre_ns{},post_ns{},epoch_ms{};
    std::size_t pre_blocks{},post_blocks{},pre_insts{},post_insts{},pre_sealed{},pre_cache{};
    bool pre_complete{},post_complete{},finished{};
};
namespace {
void Metadata(CaptureState& s,bool pending){
    using Common::DebugDocument::Quote;
    std::ostringstream out;
    out<<"{\"schema\":1,\"id\":"<<Quote(s.id)<<",\"run_id\":"<<Quote(s.run)<<",\"module_generation\":"<<Quote(s.generation)
       <<",\"epoch_ms\":"<<s.epoch_ms<<",\"pipeline_key\":"<<Quote(Hex(s.key))<<",\"shader_unique_hash\":"<<Quote(std::to_string(s.unique_hash))
       <<",\"status\":"<<Quote(pending?"awaiting_postSSA":s.post_complete?"ready":"capture_failed")<<",\"error\":"<<Quote(s.error)
       <<",\"pre_file\":"<<Quote(s.id+".pre.bin")<<",\"post_file\":"<<Quote(s.id+".post.bin")
       <<",\"pre_bytes\":"<<s.pre_bytes<<",\"post_bytes\":"<<s.post_bytes<<",\"pre_content_id\":"<<Quote(s.pre_content)<<",\"post_content_id\":"<<Quote(s.post_content)
       <<",\"hash_algorithm\":\"fnv1a64-nonauthenticating\",\"pre_complete\":"<<(s.pre_complete?"true":"false")<<",\"post_complete\":"<<(s.post_complete?"true":"false")
       <<",\"pre_objects\":{\"blocks\":"<<s.pre_blocks<<",\"instructions\":"<<s.pre_insts<<"},\"post_objects\":{\"blocks\":"<<s.post_blocks<<",\"instructions\":"<<s.post_insts<<"}"
       <<",\"pre_sealed_blocks\":"<<s.pre_sealed<<",\"pre_nonempty_register_cache\":"<<s.pre_cache
       <<",\"pre_capture_ns\":"<<s.pre_ns<<",\"post_capture_ns\":"<<s.post_ns
       <<",\"limits\":{\"pairs\":1,\"phase_bytes\":67108864,\"pair_bytes\":134217728},\"budget_scope\":\"module_load; reload does not reset\""
       <<",\"SSA_only_replay\":true,\"syntax_info_environment_profile_not_captured\":true,\"GPU_resources_not_captured\":true,\"scope\":\"exact paired SSA boundary graph; not full translation or GPU replay\"}";
    const auto text=out.str();if(text.size()>16384)throw std::runtime_error("SSA metadata budget exceeded");
    Write(s.root/(s.id+(pending?".pending.json":".json")),std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
void Fail(CaptureState& s,const char* reason) noexcept {try{s.error=std::string(reason).substr(0,768);s.finished=true;if(!s.root.empty()&&!s.id.empty())Metadata(s,false);}catch(...) {}}
}
Scope::Scope(std::uint64_t key_,std::uint64_t unique_hash_) noexcept:key(key_),unique_hash(unique_hash_),previous(current){current=this;}
Scope::~Scope(){if(state&&!state->finished)Fail(*state,"SSA call or post capture did not complete");current=previous;}
void CapturePre(IR::Program& program) noexcept {
    if(!current)return;
    try {
        if(Common::DebugDocument::Environment("NXEMU_SSA_FIXTURES")!="1")return;
        const auto filter=Common::DebugDocument::Environment("NXEMU_PIPELINE_FIXTURE_KEY");
        if(filter!=Hex(current->key))return; // Explicit single target is mandatory.
        bool expected=false;if(!claimed.compare_exchange_strong(expected,true))return;
        current->state=std::make_unique<CaptureState>();auto& s=*current->state;
        s.key=current->key;s.unique_hash=current->unique_hash;s.run=Common::DebugDocument::Environment("NXEMU_DEBUG_RUN_ID");s.generation=Common::DebugDocument::Environment("NXEMU_DEBUG_GENERATION");
        if(s.run.empty()||s.generation.empty()||s.generation.size()>20||s.generation.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("SSA capture requires a valid bridge session");
        const auto dir=Common::DebugDocument::Environment("NXEMU_NATIVE_SHADER_DIAG_DIR");if(dir.empty())throw std::runtime_error("SSA diagnostic directory missing");
        s.root=std::filesystem::u8path(dir)/"ssa-fixtures";s.id="ssa-"+Hex(s.key)+"-g"+s.generation+"-1";s.epoch_ms=Common::DebugDocument::EpochMs();
        std::filesystem::create_directories(s.root);const auto started=Now();
        auto bytes=Encode(program,s.ids,0);if(bytes.size()>MaxBytes)throw std::runtime_error("SSA pre capture exceeds phase budget");
        s.pre_blocks=s.ids.block_ids.size();s.pre_insts=s.ids.inst_ids.size();
        for(auto* b:s.ids.block_ids){if(b->IsSsaSealed())++s.pre_sealed;for(std::uint32_t r=0;r<IR::NUM_REGS;++r)if(!b->SsaRegValue(static_cast<IR::Reg>(r)).IsEmpty())++s.pre_cache;}
        s.pre_bytes=bytes.size();s.pre_content=Content(bytes);Write(s.root/(s.id+".pre.bin"),bytes);s.pre_complete=true;s.pre_ns=Now()-started;Metadata(s,true);
    }catch(const std::exception& error){if(current->state)Fail(*current->state,error.what());}catch(...){if(current->state)Fail(*current->state,"SSA pre capture exception");}
}
void CapturePost(IR::Program& program) noexcept {
    if(!current||!current->state||!current->state->pre_complete||current->state->finished)return;
    auto& s=*current->state;
    try {
        const auto started=Now();auto bytes=Encode(program,s.ids,1);if(bytes.size()>MaxBytes||s.pre_bytes+bytes.size()>2*MaxBytes)throw std::runtime_error("SSA pair budget exceeded");
        s.post_blocks=s.ids.block_ids.size();s.post_insts=s.ids.inst_ids.size();s.post_bytes=bytes.size();s.post_content=Content(bytes);
        Write(s.root/(s.id+".post.bin"),bytes);s.post_complete=true;s.post_ns=Now()-started;Metadata(s,false);s.finished=true;
    }catch(const std::exception& error){Fail(s,error.what());}catch(...){Fail(s,"SSA post capture exception");}
}
}
