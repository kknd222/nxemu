// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Fixed-root explicit frame requests only; this header has no platform/renderer/GPU dependencies.
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace VideoCommon::DiagnosticFrameCapture {
bool Enabled();
std::filesystem::path Root();
std::string RunId();
std::string Generation();
constexpr uint32_t Width=1280,Height=720;
constexpr size_t RequestBudget=512;
constexpr uint64_t TtlMs=20000;
struct Request { std::string run_id,generation,id; uint64_t epoch_ms{}; };
struct Ticket {
    Request request;std::filesystem::path root;
    uint64_t started_ms{},started_ns{};bool window_is_shown{};
};
inline uint64_t EpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline uint64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::string Quote(std::string_view text) {
    std::ostringstream out;out<<'"';
    for(const unsigned char c:text){
        if(c=='"'||c=='\\')out<<'\\'<<char(c);
        else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;
        else out<<char(c);
    }
    out<<'"';return out.str();
}
inline bool SafeId(std::string_view id) {
    if(id.empty()||id.size()>32)return false;
    for(char c:id)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;
    return true;
}
// Strict small JSON subset: five exact fields, ASCII strings without escapes, positive integers.
inline std::optional<Request> Parse(std::string_view input) {
    if(input.empty()||input.size()>RequestBudget)return std::nullopt;
    size_t p=0;const auto ws=[&]{while(p<input.size()&&(input[p]==' '||input[p]=='\n'||input[p]=='\r'||input[p]=='\t'))++p;};
    const auto character=[&](char c){ws();if(p>=input.size()||input[p]!=c)return false;++p;return true;};
    const auto string=[&]() -> std::optional<std::string>{
        if(!character('"'))return std::nullopt;const size_t start=p;
        while(p<input.size()&&input[p]!='"'){
            const auto c=static_cast<unsigned char>(input[p]);if(c<32||c>=127||c=='\\')return std::nullopt;++p;
        }
        if(p>=input.size())return std::nullopt;std::string out{input.substr(start,p-start)};++p;return out;
    };
    const auto number=[&]() -> std::optional<uint64_t>{
        ws();const size_t start=p;while(p<input.size()&&input[p]>='0'&&input[p]<='9')++p;
        if(p==start||(p-start>1&&input[start]=='0'))return std::nullopt;
        uint64_t out{};const auto result=std::from_chars(input.data()+start,input.data()+p,out);
        if(result.ec!=std::errc{})return std::nullopt;return out;
    };
    if(!character('{'))return std::nullopt;Request result;unsigned seen=0;
    for(unsigned field=0;field<5;++field){
        if(field&&!character(','))return std::nullopt;const auto key=string();if(!key||!character(':'))return std::nullopt;
        unsigned bit=0;
        if(*key=="schema"){
            bit=1;const auto value=number();if(!value||*value!=1)return std::nullopt;
        }else if(*key=="epoch_ms"){
            bit=2;const auto value=number();if(!value)return std::nullopt;result.epoch_ms=*value;
        }else if(*key=="run_id"||*key=="generation"||*key=="id"){
            bit=*key=="run_id"?4:*key=="generation"?8:16;const auto value=string();if(!value||value->empty()||value->size()>64)return std::nullopt;
            if(*key=="run_id")result.run_id=*value;else if(*key=="generation")result.generation=*value;else result.id=*value;
        }else return std::nullopt;
        if(seen&bit)return std::nullopt;seen|=bit;
    }
    if(seen!=31||!character('}'))return std::nullopt;ws();if(p!=input.size()||!SafeId(result.id))return std::nullopt;
    return result;
}
inline bool PlainFileOrAbsent(const std::filesystem::path& file) {
    std::error_code ec;const auto status=std::filesystem::symlink_status(file,ec);
    if(ec==std::errc::no_such_file_or_directory)return true;
    return !ec&&(status.type()==std::filesystem::file_type::not_found||status.type()==std::filesystem::file_type::regular);
}
inline bool AtomicText(const std::filesystem::path& file,std::string_view text) {
    const auto tmp=std::filesystem::path(file.string()+".tmp");
    if(!PlainFileOrAbsent(file)||!PlainFileOrAbsent(tmp))return false;
    {std::ofstream out(tmp,std::ios::binary|std::ios::trunc);out.write(text.data(),text.size());if(!out)return false;}
    std::error_code ec;std::filesystem::remove(file,ec);ec.clear();std::filesystem::rename(tmp,file,ec);return !ec;
}
inline std::string Status(const Request& request,std::string_view status,std::string_view reason,
                          uint64_t start=0,uint64_t readback_ns=0,uint64_t cpu_ns=0,
                          uint64_t black=0,bool shown=false) {
    std::ostringstream s;s<<"{\"schema\":1,\"request_id\":"<<Quote(request.id)
     <<",\"run_id\":"<<Quote(request.run_id)<<",\"generation\":"<<Quote(request.generation)
     <<",\"status\":"<<Quote(status)<<",\"reason\":"<<Quote(reason)
     <<",\"requested_epoch_ms\":"<<request.epoch_ms<<",\"started_epoch_ms\":"<<start
     <<",\"completed_epoch_ms\":"<<EpochMs()<<",\"width\":"<<Width<<",\"height\":"<<Height
     <<",\"format\":\"P6_RGB8\",\"source_format\":\"VK_FORMAT_B8G8R8A8_UNORM\""
     <<",\"window_is_shown\":"<<(shown?"true":"false")<<",\"readback_ns\":"<<readback_ns<<",\"cpu_write_ns\":"<<cpu_ns
     <<",\"exact_black_pixels\":"<<black<<",\"black_ratio\":"<<std::setprecision(17)<<(double(black)/double(Width*Height))
     <<",\"file\":"<<(status=="completed"?Quote("frame-capture-"+request.id+".ppm"):"null")
     <<",\"source\":\"explicit_offscreen_current_composite\",\"gpu_readback_completed\":"<<(status=="completed"?"true":"false")
     <<",\"limitations\":[\"one_explicit_gpu_finish_changes_timing\",\"observer_timeout_does_not_cancel_gpu_finish_or_free_active_buffer\",\"capture_requires_nonempty_composite_even_when_minimized\",\"PPM_is_game_composition_not_OS_UI\"]}";return s.str();
}
struct State {
    std::mutex mutex;std::string run_id;unsigned accepted{};uint64_t last_poll_ms{};
    std::array<std::string,4> ids{};bool in_flight{};
};
inline State state;
inline void Respond(const std::filesystem::path& root,const Request& r,std::string_view data) {
    AtomicText(root/"frame-capture-status.json",data);
    if(SafeId(r.id))AtomicText(root/("frame-capture-"+r.id+".json"),data);
}
inline std::optional<Ticket> Poll(const std::filesystem::path& root,std::string_view run,
                                 std::string_view generation,bool shown,uint64_t now=EpochMs()) {
    if(!root.is_absolute()||run.empty()||generation.empty())return std::nullopt;
    std::unique_lock lock(state.mutex,std::try_to_lock);if(!lock.owns_lock()||state.in_flight)return std::nullopt;
    if(now>=state.last_poll_ms&&now-state.last_poll_ms<500)return std::nullopt;state.last_poll_ms=now;
    if(state.run_id!=run){state.run_id=run;state.accepted=0;state.ids={};}
    const auto path=root/"frame-capture-request.json";
    std::error_code ec;if(!std::filesystem::exists(path,ec))return std::nullopt;
    Request request;std::string reason;
    const auto claimed=root/"frame-capture-request.processing";
    if(!PlainFileOrAbsent(claimed)||std::filesystem::exists(claimed,ec)){Respond(root,request,Status(request,"rejected","claim_busy"));return std::nullopt;}
    ec.clear();std::filesystem::rename(path,claimed,ec);if(ec)return std::nullopt;
    // Read the atomically claimed file, not a request path another writer can replace.
    if(!PlainFileOrAbsent(claimed))reason="request_not_plain_file";
    else {
        std::ifstream in(claimed,std::ios::binary);std::array<char,RequestBudget+1> bytes{};in.read(bytes.data(),bytes.size());
        const auto parsed=Parse(std::string_view(bytes.data(),static_cast<size_t>(in.gcount())));
        if(!parsed)reason="invalid_request_schema";else request=*parsed;
    }
    if(reason.empty()) {
        if(request.run_id!=run||request.generation!=generation)reason="session_mismatch";
        else if(request.epoch_ms>now||now-request.epoch_ms>TtlMs)reason="request_expired_or_future";
        else {
            for(unsigned i=0;i<state.accepted;++i)if(state.ids[i]==request.id)reason="duplicate_id";
            if(reason.empty()&&state.accepted>=4)reason="run_quota_exhausted";
        }
    }
    if(!reason.empty()){Respond(root,request,Status(request,"rejected",reason));std::filesystem::remove(claimed,ec);return std::nullopt;}
    state.ids[state.accepted++]=request.id;state.in_flight=true;
    Ticket ticket{request,root,now,SteadyNs(),shown};Respond(root,request,Status(request,"started","",now,0,0,0,shown));return ticket;
}
inline void Release(const Ticket& ticket) {
    std::error_code ec;std::filesystem::remove(ticket.root/"frame-capture-request.processing",ec);
    std::scoped_lock lock(state.mutex);state.in_flight=false;
}
inline void Failure(const Ticket& ticket,std::string_view reason) {
    Respond(ticket.root,ticket.request,Status(ticket.request,"error",reason,ticket.started_ms,0,0,0,ticket.window_is_shown));Release(ticket);
}
inline bool Complete(const Ticket& ticket,std::span<const uint8_t> bgra,uint64_t readback_ns) {
    if(bgra.size()!=size_t(Width)*Height*4){Failure(ticket,"unexpected_readback_size");return false;}
    const auto start=SteadyNs();const auto file=ticket.root/("frame-capture-"+ticket.request.id+".ppm");
    const auto tmp=std::filesystem::path(file.string()+".tmp");
    if(!PlainFileOrAbsent(file)||!PlainFileOrAbsent(tmp)){Failure(ticket,"output_not_plain_file");return false;}
    std::ofstream out(tmp,std::ios::binary|std::ios::trunc);out<<"P6\n"<<Width<<" "<<Height<<"\n255\n";
    uint64_t black=0;std::array<char,Width*3> row{};
    for(uint32_t y=0;y<Height;++y){
        for(uint32_t x=0;x<Width;++x){const auto pos=(size_t(y)*Width+x)*4;
            row[x*3]=char(bgra[pos+2]);row[x*3+1]=char(bgra[pos+1]);row[x*3+2]=char(bgra[pos]);
            if((bgra[pos]|bgra[pos+1]|bgra[pos+2])==0)++black;
        }
        out.write(row.data(),row.size());
    }
    out.close();if(!out){Failure(ticket,"ppm_write_failed");return false;}
    std::error_code ec;std::filesystem::remove(file,ec);ec.clear();std::filesystem::rename(tmp,file,ec);
    if(ec){Failure(ticket,"ppm_publish_failed");return false;}
    Respond(ticket.root,ticket.request,Status(ticket.request,"completed","",ticket.started_ms,readback_ns,SteadyNs()-start,black,ticket.window_is_shown));Release(ticket);return true;
}
inline void CancelQueued(const std::filesystem::path& root,std::string_view run,std::string_view generation) {
    std::unique_lock lock(state.mutex,std::try_to_lock);if(!lock.owns_lock()||state.in_flight)return;
    const auto path=root/"frame-capture-request.json";if(!PlainFileOrAbsent(path))return;
    std::ifstream in(path,std::ios::binary);std::array<char,RequestBudget+1> bytes{};in.read(bytes.data(),bytes.size());
    const auto r=Parse(std::string_view(bytes.data(),static_cast<size_t>(in.gcount())));
    in.close(); // Windows denies deletion while the input stream still owns the file handle.
    if(r&&r->run_id==run&&r->generation==generation){Respond(root,*r,Status(*r,"cancelled","renderer_stopped_before_capture"));std::error_code ec;std::filesystem::remove(path,ec);}
}
} // namespace VideoCommon::DiagnosticFrameCapture
