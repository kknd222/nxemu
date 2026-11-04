// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Explicit-request, bounded filesystem publication. No Vulkan calls or platform dependencies.
#include "yuzu_video_core/diagnostic_frame_capture.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace VideoCommon::PairedPixelCapture {
inline constexpr uint64_t MaxImageBytes = 8 * 1024 * 1024;
struct Image {
    uint32_t width{}, height{}, format{}, mip{}, layer{}, samples{1};
    uint64_t image{}, view{};
    bool swizzle_known{}, identity_swizzle{};
    std::array<uint32_t,4> swizzle{};
    bool gpu_completed{};
    std::span<const uint8_t> pixels;
    std::string reason="not_reached";
};
inline bool Bgra(uint32_t f) { return f==44 || f==50; }
inline bool Supported(uint32_t f) { return f==37 || f==43 || f==44 || f==50 || f==51 || f==57; }
inline uint64_t Bytes(uint32_t w,uint32_t h) {
    if (!w || !h || uint64_t(w)*h > MaxImageBytes/4) return 0;
    return uint64_t(w)*h*4;
}
inline bool AtomicBytes(const std::filesystem::path& path,std::span<const uint8_t> bytes) {
    using namespace DiagnosticFrameCapture;
    const auto tmp=std::filesystem::path(path.string()+".tmp");
    if (!PlainFileOrAbsent(path) || !PlainFileOrAbsent(tmp)) return false;
    { std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
      out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!out)return false; }
    std::error_code ec;std::filesystem::remove(path,ec);ec.clear();
    std::filesystem::rename(tmp,path,ec);return !ec;
}
inline std::string ImageJson(const std::filesystem::path& root,std::string_view id,
                             std::string_view role,const Image& image) {
    using namespace DiagnosticFrameCapture;
    if(!SafeId(id) || id.size()!=32 || (role!="source" && role!="target"))
        throw std::invalid_argument("Invalid paired pixel id or role");
    const auto bytes=Bytes(image.width,image.height);
    std::string failure=image.reason.substr(0,256);
    if (!image.gpu_completed) { if(failure.empty())failure="gpu_not_completed"; }
    else if (!Supported(image.format)) failure="unsupported_format";
    else if (image.samples!=1) failure="unsupported_samples";
    else if (image.swizzle_known && std::ranges::any_of(image.swizzle,[](uint32_t s){return s>6;})) failure="unsupported_component_swizzle";
    else if (!bytes) failure="invalid_extent_or_byte_budget";
    else if (image.pixels.size()!=bytes) failure="pixel_size_mismatch";
    else failure.clear();
    const auto prefix="frame-capture-"+std::string(id)+"."+std::string(role);
    uint64_t black{},alpha_zero{},alpha_full{};uint8_t alpha_min=255,alpha_max=0;
    bool rgba_zero=true;
    uint64_t view_black{},view_alpha_zero{},view_alpha_full{};
    uint8_t view_alpha_min=255,view_alpha_max=0;
    if(failure.empty()) {
        std::ostringstream header;header<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
        const auto h=header.str();std::vector<uint8_t> ppm(h.begin(),h.end());
        ppm.reserve(h.size()+size_t(bytes/4)*3);
        for(size_t p=0;p<image.pixels.size();p+=4){
            const auto r=image.pixels[p+(Bgra(image.format)?2:0)];
            const auto g=image.pixels[p+1];
            const auto b=image.pixels[p+(Bgra(image.format)?0:2)];
            const auto a=image.pixels[p+3];
            ppm.push_back(r);ppm.push_back(g);ppm.push_back(b);
            black+=!(r|g|b);alpha_zero+=a==0;alpha_full+=a==255;
            alpha_min=std::min(alpha_min,a);alpha_max=std::max(alpha_max,a);
            rgba_zero=rgba_zero&&!(r|g|b|a);
            if(image.swizzle_known){
                const std::array<uint8_t,4> stored{r,g,b,a};
                std::array<uint8_t,4> sampled{};
                for(size_t c=0;c<4;++c){
                    const auto sw=image.swizzle[c];
                    sampled[c]=sw==0?stored[c]:sw==1?0:sw==2?255:stored[sw-3];
                }
                view_black+=!(sampled[0]|sampled[1]|sampled[2]);
                view_alpha_zero+=sampled[3]==0;view_alpha_full+=sampled[3]==255;
                view_alpha_min=std::min(view_alpha_min,sampled[3]);view_alpha_max=std::max(view_alpha_max,sampled[3]);
            }
        }
        if(!AtomicBytes(root/(prefix+".rgba"),image.pixels) ||
           !AtomicBytes(root/(prefix+".ppm"),ppm))failure="pixel_file_io_failed";
    }
    std::ostringstream s;s<<"{\"status\":"<<Quote(failure.empty()?"captured":"unavailable")
      <<",\"reason\":"<<Quote(failure)<<",\"width\":"<<image.width<<",\"height\":"<<image.height
      <<",\"vk_format\":"<<image.format<<",\"raw_order\":"<<Quote(Bgra(image.format)?"BGRA8":"RGBA8")
      <<",\"mip\":"<<image.mip<<",\"layer\":"<<image.layer<<",\"samples\":"<<image.samples
      <<",\"image\":"<<image.image<<",\"view\":"<<image.view
      <<",\"swizzle_known\":"<<(image.swizzle_known?"true":"false")
      <<",\"identity_swizzle\":"<<(image.identity_swizzle?"true":"false")
      <<",\"component_swizzle\":";
    if(image.swizzle_known)s<<'['<<image.swizzle[0]<<','<<image.swizzle[1]<<','<<image.swizzle[2]<<','<<image.swizzle[3]<<']';
    else s<<"null";
    const bool view_known=failure.empty()&&image.swizzle_known;
    s<<",\"view_all_rgb_zero\":"<<(view_known?(view_black==bytes/4?"true":"false"):"null")
      <<",\"view_alpha_min_encoded\":"<<(view_known?std::to_string(view_alpha_min):"null")
      <<",\"view_alpha_max_encoded\":"<<(view_known?std::to_string(view_alpha_max):"null")
      <<",\"view_alpha_zero_pixels\":"<<(view_known?std::to_string(view_alpha_zero):"null")
      <<",\"view_alpha_full_pixels\":"<<(view_known?std::to_string(view_alpha_full):"null")
      <<",\"gpu_readback_completed\":"<<(image.gpu_completed?"true":"false")
      <<",\"bytes\":"<<(failure.empty()?bytes:0)
      <<",\"rgba_file\":"<<(failure.empty()?Quote(prefix+".rgba"):"null")
      <<",\"ppm_file\":"<<(failure.empty()?Quote(prefix+".ppm"):"null")
      <<",\"exact_black_pixels\":"<<(failure.empty()?std::to_string(black):"null")
      <<",\"all_rgb_zero\":"<<(failure.empty()?(black==bytes/4?"true":"false"):"null")
      <<",\"all_rgba_zero\":"<<(failure.empty()?(rgba_zero?"true":"false"):"null")
      <<",\"alpha_min\":"<<(failure.empty()?std::to_string(alpha_min):"null")
      <<",\"alpha_max\":"<<(failure.empty()?std::to_string(alpha_max):"null")
      <<",\"alpha_zero_pixels\":"<<(failure.empty()?std::to_string(alpha_zero):"null")
      <<",\"alpha_full_pixels\":"<<(failure.empty()?std::to_string(alpha_full):"null")<<'}';
    return s.str();
}
inline bool Publish(const DiagnosticFrameCapture::Ticket& ticket,uint64_t composite,
                    const Image& source,const Image& target,uint64_t readback_ns) {
    using namespace DiagnosticFrameCapture;
    if (!ticket.root.is_absolute() || !SafeId(ticket.request.id) || ticket.request.id.size()!=32 ||
        ticket.request.run_id.empty() || ticket.request.run_id.size()>64 ||
        ticket.request.generation.empty() || ticket.request.generation.size()>64)
        return false;
    std::error_code ec;
    if (!std::filesystem::is_directory(ticket.root,ec) ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(ticket.root,ec)))return false;
    const auto a=ImageJson(ticket.root,ticket.request.id,"source",source);
    const auto b=ImageJson(ticket.root,ticket.request.id,"target",target);
    std::ostringstream s;s<<"{\"schema\":1,\"request_id\":"<<Quote(ticket.request.id)
     <<",\"run_id\":"<<Quote(ticket.request.run_id)<<",\"generation\":"<<Quote(ticket.request.generation)
     <<",\"composite_ordinal\":"<<composite<<",\"snapshot_epoch_ms\":"<<EpochMs()
     <<",\"readback_ns\":"<<readback_ns<<",\"max_image_bytes\":"<<MaxImageBytes
     <<",\"source\":"<<a<<",\"target\":"<<b
     <<",\"scope\":\"source before AA; renderer-owned display target before Present; not OS pixels\""
     <<",\"raw_pixels_before_component_swizzle\":true,\"view_statistics\":\"encoded channel classification; not shader output or linear SRGB values\""
     <<",\"limitations\":[\"explicit GPU Finish changes timing; timeout does not cancel request\","
       "\"first selected layer only; unsupported format/swizzle/sample marked unavailable\","
       "\"no writer shader identity or slot generation; handles process-local\"]}";
    // Metadata last: readers never treat partial binary publication as a completed pair.
    return AtomicText(ticket.root/("frame-capture-"+ticket.request.id+".pair.json"),s.str());
}
} // namespace VideoCommon::PairedPixelCapture
