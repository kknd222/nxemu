// Actual production MemoryManager/Maxwell3D consumer; CPU adapter, not host conditional Vulkan.
#include "query_guest_fixture.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include <iostream>
IModuleSettings* g_settings=nullptr; // settings storage is not exercised by this CPU fixture
struct Traits { using RuntimeType = Runtime; };
struct CpuRasterizer final : VideoCore::RasterizerInterface {
    unsigned flushes{},mapping_notifications{},invalidations{}; std::vector<std::function<void()>> deferred;
    [[noreturn]] static void unused() { throw std::runtime_error("unexpected rasterizer operation"); }
    void Draw(bool,u32) override {unused();}
    void DrawTexture() override {unused();}
    void Clear(u32) override {unused();}
    void DispatchCompute() override {unused();}
    void ResetCounter(VideoCommon::QueryType) override {unused();}
    void Query(GPUVAddr,VideoCommon::QueryType,VideoCommon::QueryPropertiesFlags,u32,u32) override {unused();}
    void BindGraphicsUniformBuffer(size_t,u32,GPUVAddr,u32) override {unused();}
    void DisableGraphicsUniformBuffer(size_t,u32) override {unused();}
    void SignalFence(std::function<void()>&&) override {unused();}
    void SignalMemoryOperationFence(std::function<void()>&&) override {unused();}
    void WaitForFence() override {unused();}
    void SyncOperation(std::function<void()>&& f) override {deferred.push_back(std::move(f));}
    void SignalSyncPoint(u32) override {unused();}
    void SignalReference() override {unused();}
    void ReleaseFences(bool) override {unused();}
    void FlushAll() override {unused();}
    void FlushRegion(DAddr,u64,VideoCommon::CacheType) override {++flushes;}
    bool MustFlushRegion(DAddr,u64,VideoCommon::CacheType) override {return false;}
    RasterizerDownloadArea GetFlushArea(DAddr,u64) override {unused();}
    void InvalidateRegion(DAddr,u64,VideoCommon::CacheType) override {++invalidations;}
    void OnCacheInvalidation(PAddr,u64) override {++invalidations;}
    bool OnCPUWrite(PAddr,u64) override {unused();}
    void InvalidateGPUCache() override {unused();}
    void UnmapMemory(DAddr,u64) override {++invalidations;}
    void ModifyGPUMemory(size_t,GPUVAddr,u64,bool) override {++mapping_notifications;}
    void FlushAndInvalidateRegion(DAddr,u64,VideoCommon::CacheType) override {unused();}
    void WaitForIdle() override {unused();}
    void FragmentBarrier() override {unused();}
    void TiledCacheBarrier() override {unused();}
    void FlushCommands() override {unused();}
    void TickFrame() override {unused();}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {unused();}
    void AccelerateInlineToMemory(GPUVAddr,size_t,std::span<const u8>) override {unused();}
};
#include "yuzu_video_core/renderer_vulkan/vk_compute_pass.h"
#include "yuzu_video_core/renderer_vulkan/vk_query_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/vulkan_common/vulkan_library.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include <fstream>
#include <atomic>
#include <algorithm>
#include "yuzu_common/debug_document.h"
IModuleNotification* g_notify=nullptr;
namespace VideoCommon::PipelineFixture {
std::string Environment(std::string_view name) {return Common::DebugDocument::Environment(std::string(name).c_str());}
}
namespace VideoCommon::ShaderProgress {
bool Enabled(){const auto value=Common::DebugDocument::Environment("NXEMU_SHADER_PROGRESS");return value=="1"||(value.empty()&&Common::DebugDocument::Enabled());}
std::string RunId(){return Common::DebugDocument::Environment("NXEMU_DEBUG_RUN_ID");}
std::string Generation(){return Common::DebugDocument::Environment("NXEMU_DEBUG_GENERATION");}
}
using namespace Vulkan;
struct ValidationState { std::atomic<unsigned> errors{},warnings{},loader_errors{},messages{}; };
VKAPI_ATTR VkBool32 VKAPI_CALL sample_validation(
 VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,
 const VkDebugUtilsMessengerCallbackDataEXT* data,void* opaque) {
 auto& state=*static_cast<ValidationState*>(opaque);
 const bool loader=data&&data->pMessageIdName&&std::string_view(data->pMessageIdName)=="Loader Message";
 if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {if(loader)++state.loader_errors;else ++state.errors;}
 if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)++state.warnings;
 if(state.messages.fetch_add(1)<32&&data&&data->pMessage)
   std::cerr<<"VULKAN_DIAGNOSTIC "<<std::string_view(data->pMessage).substr(0,512)<<"\n";
 return VK_FALSE;
}
std::vector<u32> read_spv(const char* path) {
 std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("SPV input missing");
 auto n=f.tellg();if(n<=0||n>1048576||n%4)throw std::runtime_error("SPV size");
 std::vector<u32> x(static_cast<size_t>(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(x.data()),n);return x;
}
#include <limits>

struct Case {const char* name;std::vector<u64> values;u64 initial;size_t min,max;};
int main(int argc,char** argv) try {
 if(argc!=3)throw std::runtime_error("usage expected-defects small|all");
 const unsigned expected=std::stoul(argv[1]);const bool small=std::string_view(argv[2])=="small";
 if(!small&&std::string_view(argv[2])!="all")throw std::runtime_error("case scope");
 auto library=OpenLibrary();if(!library)throw std::runtime_error("Vulkan loader missing");
 vk::InstanceDispatch dispatch;ValidationState validation;
 auto instance=CreateInstance(*library,dispatch,VK_API_VERSION_1_1,Core::Frontend::WindowSystemType::Windows,true);
 auto layers=vk::EnumerateInstanceLayerProperties(dispatch);
 if(!layers||std::none_of(layers->begin(),layers->end(),[](const auto& l){return std::string_view(l.layerName)=="VK_LAYER_KHRONOS_validation";}))throw std::runtime_error("Khronos validation unavailable");
 if(!dispatch.vkCreateDebugUtilsMessengerEXT)throw std::runtime_error("debug utils unavailable");
 VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};ci.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;ci.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;ci.pfnUserCallback=sample_validation;ci.pUserData=&validation;auto messenger=instance.CreateDebugUtilsMessenger(ci);
 auto physicals=instance.EnumeratePhysicalDevices();if(physicals.empty())throw std::runtime_error("no GPU");
 unsigned cases=0,defects=0;
 {
 Device device(*instance,vk::PhysicalDevice(physicals[0],dispatch),nullptr,dispatch);std::cerr<<"REAL_DEVICE_READY "<<device.GetModelName()<<"\n";
 StateTracker tracker;Scheduler scheduler(device,tracker);MemoryAllocator allocator(device);DescriptorPool pool(device,scheduler);ComputePassDescriptorQueue queue(device,scheduler);
 QueriesPrefixScanPass scan(device,scheduler,pool,queue);
 const size_t unlimited=std::numeric_limits<size_t>::max();
 std::vector<Case> tests{{"reset-at-zero",{256,256},999,0,0},{"no-reset-carry",{256},256,unlimited,unlimited},{"u32-carry",{0xffffffffULL,1,3},0x100000000ULL,unlimited,unlimited},{"mid-reset",{2,3,5},10,1,1}};
 if(!small){tests.push_back({"dispatch-2048",std::vector<u64>(2048,1),17,unlimited,unlimited});tests.push_back({"dispatch-2049",std::vector<u64>(2049,1),17,unlimited,unlimited});tests.push_back({"dispatch-4097",std::vector<u64>(4097,1),17,unlimited,unlimited});tests.push_back({"reset-at-dispatch-boundary",std::vector<u64>(2049,1),17,2048,2048});}
 for(const auto& c:tests){
  const size_t n=c.values.size(),physical=(n+2047)/2048*2048+8;const VkDeviceSize bytes=physical*8;
  auto make=[&](VkDeviceSize size,MemoryUsage usage){VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.size=size;b.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;return allocator.CreateBuffer(b,usage);};
  auto input=make(bytes,MemoryUsage::DeviceLocal),output=make(bytes,MemoryUsage::DeviceLocal),accum=make(8,MemoryUsage::DeviceLocal),upload=make(bytes+8,MemoryUsage::Upload),download=make(bytes+8,MemoryUsage::Download);
  auto mapped=upload.Mapped();std::fill(mapped.begin(),mapped.end(),u8{});std::memcpy(mapped.data(),c.values.data(),n*8);std::memcpy(mapped.data()+bytes,&c.initial,8);upload.Flush();
  auto drain=SCOPE_GUARD {try{scheduler.Finish();}catch(...){std::cerr<<"PREFIX_DRAIN_FAILED\n";std::_Exit(2);}};
  scheduler.RequestOutsideRenderPassOperationContext();scheduler.Record([src=*upload,input=*input,output=*output,accum=*accum,bytes](vk::CommandBuffer cmd){VkBufferCopy a{0,0,bytes},b{bytes,0,8};cmd.CopyBuffer(src,input,vk::Span(&a,1));cmd.CopyBuffer(src,accum,vk::Span(&b,1));cmd.FillBuffer(output,0,bytes,0x5a5a5a5a);});
  scan.Run(*accum,*output,*input,n,c.min,c.max);
  scheduler.RequestOutsideRenderPassOperationContext();scheduler.Record([output=*output,accum=*accum,target=*download,bytes](vk::CommandBuffer cmd){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,b);VkBufferCopy a{0,0,bytes},d{0,bytes,8};cmd.CopyBuffer(output,target,vk::Span(&a,1));cmd.CopyBuffer(accum,target,vk::Span(&d,1));b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,b);});
  scheduler.Finish();if(device.IsLost())throw std::runtime_error("device lost");drain.Cancel();download.Invalidate();auto data=download.Mapped();std::vector<u64> actual(n);std::memcpy(actual.data(),data.data(),n*8);u64 actual_accum{};std::memcpy(&actual_accum,data.data()+bytes,8);
  // This controlled oracle specifies the production scan contract: prefix output
  // adds inherited carry before the first Reset marker; final carry excludes
  // values before the last Reset marker. It is not a replacement GPU algorithm.
  std::vector<u64> want(n);u64 sum=0,final=0;for(size_t i=0;i<n;++i){sum+=c.values[i];want[i]=sum+(i<c.min?c.initial:0);if(c.max==unlimited||i>=c.max)final+=c.values[i];}if(c.max==unlimited)final+=c.initial;
  size_t first_bad=n;for(size_t i=0;i<n;++i)if(actual[i]!=want[i]){first_bad=i;break;}const bool mismatch=first_bad!=n||actual_accum!=final;if(mismatch)++defects;
  u64 guard{};std::memcpy(&guard,data.data()+((physical-1)*8),8);if(guard!=0x5a5a5a5a5a5a5a5aULL)throw std::runtime_error("prefix wrote physical guard");
  std::cout<<"PREFIX_GPU_CASE name="<<c.name<<" n="<<n<<" initial="<<c.initial<<" min="<<c.min<<" max="<<c.max<<" prefix_last="<<actual.back()<<" prefix_expected="<<want.back()<<" accumulator="<<actual_accum<<" accumulator_expected="<<final<<" first_bad="<<(first_bad==n?-1:static_cast<long long>(first_bad))<<" mismatch="<<mismatch<<" guard_preserved=True actual_QueriesPrefixScanPass=True\n";
  ++cases;
 }
 scheduler.Finish();
 }
 std::cout<<"PREFIX_VALIDATION errors="<<validation.errors.load()<<" warnings="<<validation.warnings.load()<<" loader_errors="<<validation.loader_errors.load()<<"\n";
 if(validation.errors.load()||validation.warnings.load()||validation.loader_errors.load())throw std::runtime_error("Vulkan validation diagnostics");
 std::cout<<(defects==expected?"PREFIX_GPU_EXPECTATIONS_PASS":"PREFIX_GPU_EXPECTATIONS_FAILED")<<" cases="<<cases<<" defects="<<defects<<" expected_defects="<<expected<<" actual_QueriesPrefixScanPass=True GPU=True full_QueryCacheRuntime=False\n";
 return defects==expected?0:1;
} catch(const std::exception& e){std::cerr<<"PREFIX_ERROR "<<e.what()<<"\n";return 1;}
