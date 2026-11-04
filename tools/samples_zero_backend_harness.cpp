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
#include "yuzu_video_core/renderer_vulkan/samples_backend_lab.h"
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
int main(int argc,char** argv) try {
 if(argc!=4)throw std::runtime_error("usage vertex.spv fragment.spv expect-width-defect");
 const bool expected_width_defect=std::stoi(argv[3])!=0;
 videoSettings.current_gpu_accuracy=GpuAccuracy::High;
 auto library=OpenLibrary();if(!library)throw std::runtime_error("Vulkan loader missing");
 vk::InstanceDispatch dispatch;
 ValidationState validation;
 auto instance=CreateInstance(*library,dispatch,VK_API_VERSION_1_1,Core::Frontend::WindowSystemType::Windows,true);
 auto layers=vk::EnumerateInstanceLayerProperties(dispatch);
 if(!layers||std::none_of(layers->begin(),layers->end(),[](const auto& layer){return std::string_view(layer.layerName)=="VK_LAYER_KHRONOS_validation";}))
   throw std::runtime_error("Khronos validation unavailable");
 if(!dispatch.vkCreateDebugUtilsMessengerEXT)throw std::runtime_error("Debug utils unavailable");
 VkDebugUtilsMessengerCreateInfoEXT messenger_ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
 messenger_ci.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
 messenger_ci.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
 messenger_ci.pfnUserCallback=sample_validation;messenger_ci.pUserData=&validation;
 auto messenger=instance.CreateDebugUtilsMessenger(messenger_ci);
 auto physicals=instance.EnumeratePhysicalDevices();if(physicals.empty())throw std::runtime_error("No GPU");
 {
 Device device(*instance,vk::PhysicalDevice(physicals[0],dispatch),nullptr,dispatch);
 std::cerr<<"REAL_DEVICE_READY "<<device.GetModelName()<<"\n";
 StateTracker tracker;Scheduler scheduler(device,tracker);MemoryAllocator allocator(device);
 DescriptorPool pool(device,scheduler);ComputePassDescriptorQueue queue(device,scheduler);
 const auto& logical=device.GetLogical();
 VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image_ci.imageType=VK_IMAGE_TYPE_2D;
 image_ci.format=VK_FORMAT_R8G8B8A8_UNORM;image_ci.extent={16,16,1};image_ci.mipLevels=1;image_ci.arrayLayers=1;
 image_ci.samples=VK_SAMPLE_COUNT_1_BIT;image_ci.tiling=VK_IMAGE_TILING_OPTIMAL;image_ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
 image_ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image_ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
 auto image=allocator.CreateImage(image_ci);
 VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view_ci.image=*image;view_ci.viewType=VK_IMAGE_VIEW_TYPE_2D;
 view_ci.format=image_ci.format;view_ci.subresourceRange=range;auto view=logical.CreateImageView(view_ci);
 VkAttachmentDescription attachment{};attachment.format=image_ci.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
 attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
 attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
 attachment.initialLayout=VK_IMAGE_LAYOUT_GENERAL;attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
 VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_GENERAL};VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
 subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
 VkRenderPassCreateInfo pass_ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};pass_ci.attachmentCount=1;pass_ci.pAttachments=&attachment;
 pass_ci.subpassCount=1;pass_ci.pSubpasses=&subpass;auto pass=logical.CreateRenderPass(pass_ci);
 VkImageView raw_view=*view;VkFramebufferCreateInfo frame_ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};frame_ci.renderPass=*pass;
 frame_ci.attachmentCount=1;frame_ci.pAttachments=&raw_view;frame_ci.width=16;frame_ci.height=16;frame_ci.layers=1;auto frame=logical.CreateFramebuffer(frame_ci);
 auto vert=read_spv(argv[1]),frag=read_spv(argv[2]);
 VkShaderModuleCreateInfo module_ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module_ci.codeSize=vert.size()*4;module_ci.pCode=vert.data();auto vs=logical.CreateShaderModule(module_ci);
 module_ci.codeSize=frag.size()*4;module_ci.pCode=frag.data();auto fs=logical.CreateShaderModule(module_ci);
 VkPipelineLayoutCreateInfo layout_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};auto layout=logical.CreatePipelineLayout(layout_ci);
 std::array<VkPipelineShaderStageCreateInfo,2> stages{};
 for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
 stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=*vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=*fs;
 VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
 VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
 VkViewport viewport{0,0,16,16,0,1};VkRect2D scissor{{0,0},{16,16}};
 VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
 VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
 VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo blend_ci{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend_ci.attachmentCount=1;blend_ci.pAttachments=&blend;
 VkGraphicsPipelineCreateInfo pipeline_ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pipeline_ci.stageCount=2;pipeline_ci.pStages=stages.data();pipeline_ci.pVertexInputState=&vi;
 pipeline_ci.pInputAssemblyState=&ia;pipeline_ci.pViewportState=&vp;pipeline_ci.pRasterizationState=&raster;pipeline_ci.pMultisampleState=&multisample;
 pipeline_ci.pColorBlendState=&blend_ci;pipeline_ci.layout=*layout;pipeline_ci.renderPass=*pass;auto pipeline=logical.CreateGraphicsPipeline(pipeline_ci);

 Memory source;Tegra::MaxwellDeviceMemoryManager memory(source);auto asid=memory.RegisterProcess(&source);memory.Map(0x1000,0,source.data.size(),asid,false);
 CpuRasterizer rasterizer;
 struct Transfer {vk::Buffer buffer;SamplesBackendCopy copy;};std::vector<Transfer> transfers;
 auto sync=[&](std::span<const SamplesBackendCopy> copies,VkBuffer src){
   for(const auto& c:copies){if((c.size!=4&&c.size!=8)||c.address<0x1000||c.address>0x5000-c.size||c.offset>~size_t{}-c.size)throw std::runtime_error("copy range");
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=c.size;ci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    auto buffer=allocator.CreateBuffer(ci,MemoryUsage::Download);auto target=*buffer;
    scheduler.RequestOutsideRenderPassOperationContext();scheduler.Record([src,target,c](vk::CommandBuffer cmd){VkBufferCopy copy{c.offset,0,c.size};cmd.CopyBuffer(src,target,vk::Span(&copy,1));
      VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=target;b.offset=0;b.size=c.size;cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,b);});
    transfers.push_back({std::move(buffer),c});
   }
 };
 auto streamer=CreateSamplesBackendLab(0,sync,&rasterizer,device,scheduler,allocator,queue,pool);
 auto drain=[&]{auto jobs=std::move(rasterizer.deferred);rasterizer.deferred.clear();for(auto& f:jobs)f();};
 auto draw=[&]{std::array<VkImage,1> images{*image};std::array<VkImageSubresourceRange,1> ranges{range};
 scheduler.RequestLabRenderpass(*pass,*frame,{16,16},images,ranges);streamer->NotifyRenderPass(true);
 scheduler.Record([pipeline=*pipeline](vk::CommandBuffer cmd){cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);cmd.Draw(3,1,0,0);});
 streamer->NotifyRenderPass(false);scheduler.RequestOutsideRenderPassOperationContext();};
 auto report=[&](u64 expected,bool timestamp){
  source.write<u32>(0x204,0xa5a5a5a5);auto id=streamer->WriteCounter(0x1200,timestamp,0);
  if(!streamer->HasPendingSync())throw std::runtime_error("expected pending sync");
  streamer->PresyncWrites();streamer->SyncWrites();const bool has_queries=streamer->HasUnsyncedQueries();if(has_queries)streamer->PushUnsyncedQueries();scheduler.Finish();
  if(device.IsLost())throw std::runtime_error("device lost");if(has_queries)streamer->PopUnsyncedQueries();drain();
  for(auto& t:transfers){t.buffer.Invalidate();auto bytes=t.buffer.Mapped();if(bytes.size()<t.copy.size)throw std::runtime_error("unmapped download");std::memcpy(memory.GetPointer<u8>(t.copy.address),bytes.data(),t.copy.size);}transfers.clear();
  auto value=source.read<u32>(0x200);if(value!=expected)throw std::runtime_error("actual SamplesStreamer count mismatch");
  if(streamer->GetQuery(id)->value!=expected)throw std::runtime_error("CPU query readback mismatch");
  if(!timestamp){auto neighbor=source.read<u32>(0x204);if(expected_width_defect){if(neighbor!=0)throw std::runtime_error("expected width defect not reproduced");std::cout<<"NO_TIMESTAMP_WIDTH_DEFECT_CONFIRMED neighbor=0 expected_preserved=2779096485\n";}else {if(neighbor!=0xa5a5a5a5)throw std::runtime_error("no-timestamp neighbor overwritten");std::cout<<"NO_TIMESTAMP_NEIGHBOR_PRESERVED value=2779096485\n";}}
  streamer->Free(id);std::cout<<"ACTUAL_SAMPLES_REPORT_PASS count="<<expected<<" timestamp="<<timestamp<<"\n";
 };
 auto reset=[&]{streamer->ResetCounter();scheduler.Finish();drain();};
 auto cleanup=SCOPE_GUARD {try {
   if(streamer&&scheduler.IsInsideRenderPass())streamer->NotifyRenderPass(false);
   scheduler.RequestOutsideRenderPassOperationContext();scheduler.Finish();
   if(!device.IsLost())drain();
 }catch(...){std::cerr<<"CLEANUP_DRAIN_FAILED\n";std::_Exit(2);}};
 scheduler.Record([image=*image,range](vk::CommandBuffer cmd){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
 b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange=range;cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,nullptr,nullptr,vk::Span(&b,1));});

 unsigned cases=0;
 auto finish_sync=[&]{
  streamer->PresyncWrites();streamer->SyncWrites();
  const bool has_queries=streamer->HasUnsyncedQueries();
  if(has_queries)streamer->PushUnsyncedQueries();scheduler.Finish();
  if(device.IsLost())throw std::runtime_error("device lost");
  if(has_queries)streamer->PopUnsyncedQueries();drain();
  for(auto& t:transfers){t.buffer.Invalidate();auto bytes=t.buffer.Mapped();
   if(bytes.size()<t.copy.size)throw std::runtime_error("unmapped download");
   std::memcpy(memory.GetPointer<u8>(t.copy.address),bytes.data(),t.copy.size);}
  transfers.clear();
 };
 auto seed=[&](u64 address){source.write<u32>(address-0x1000,0xa5a5a5a5);source.write<u32>(address-0x1000+4,0x5a5a5a5a);source.write<u32>(address-0x1000+8,0x12345678);};
 auto check_zero=[&](u64 address,bool timestamp){
  if(source.read<u32>(address-0x1000)!=0)throw std::runtime_error("zero publication absent");
  if(source.read<u32>(address-0x1000+4)!=(timestamp?0u:0x5a5a5a5au))throw std::runtime_error("zero width mismatch");
  if(source.read<u32>(address-0x1000+8)!=0x12345678)throw std::runtime_error("zero next word overwritten");
 };
 auto zero_case=[&](const char* name,u64 address,bool timestamp){
  seed(address);auto id=streamer->WriteCounter(address,timestamp,0);
  if(!streamer->HasPendingSync()||streamer->HasUnsyncedQueries())throw std::runtime_error("bankless pending state");
  if(streamer->GetQuery(id)->value!=0)throw std::runtime_error("bankless query value");
  finish_sync();check_zero(address,timestamp);
  if(streamer->HasPendingSync())throw std::runtime_error("zero queue not cleared");
  seed(address);finish_sync();
  if(source.read<u32>(address-0x1000)!=0xa5a5a5a5)throw std::runtime_error("repeated sync republishes");
  streamer->Free(id);++cases;std::cout<<"ZERO_CASE_PASS "<<name<<" width="<<(timestamp?8:4)<<" repeated_sync=True\n";
 };
 zero_case("construct",0x1400,false);zero_case("construct",0x1420,true);
 reset();zero_case("reset",0x1440,false);zero_case("reset",0x1460,true);
 for(auto flag:{VideoCommon::QueryFlagBits::IsInvalidated,VideoCommon::QueryFlagBits::IsRewritten,VideoCommon::QueryFlagBits::IsOrphan}){
  seed(0x1480);auto id=streamer->WriteCounter(0x1480,true,0);streamer->GetQuery(id)->flags|=flag;
  finish_sync();if(source.read<u32>(0x480)!=0xa5a5a5a5||source.read<u32>(0x484)!=0x5a5a5a5a)throw std::runtime_error("cancelled zero published");
  streamer->Free(id);++cases;std::cout<<"ZERO_FILTER_PASS flag="<<static_cast<u32>(flag)<<"\n";
 }
 // Construction yielded one reusable slot; Free must remove its queued publication.
 seed(0x14a0);auto freed=streamer->WriteCounter(0x14a0,true,0);streamer->Free(freed);
 auto reused=streamer->WriteCounter(0x14c0,false,0);seed(0x14c0);
 if(freed!=reused)throw std::runtime_error("expected FIFO ID reuse");
 finish_sync();if(source.read<u32>(0x4a0)!=0xa5a5a5a5)throw std::runtime_error("freed report published");
 check_zero(0x14c0,false);streamer->Free(reused);++cases;std::cout<<"ZERO_FREE_REUSE_PASS same_id=True\n";
 seed(0x14e0);auto pending=streamer->WriteCounter(0x14e0,true,0);
 reset();if(!streamer->HasPendingSync())throw std::runtime_error("Reset discarded pending zero");
 finish_sync();check_zero(0x14e0,true);streamer->Free(pending);++cases;std::cout<<"ZERO_RESET_PRESERVES_PENDING_PASS\n";
 reset();seed(0x1500);auto mixed_zero=streamer->WriteCounter(0x1500,false,0);
 streamer->StartCounter();draw();source.write<u32>(0x524,0x5a5a5a5a);auto mixed_draw=streamer->WriteCounter(0x1520,false,0);
 finish_sync();check_zero(0x1500,false);
 if(source.read<u32>(0x520)!=256||source.read<u32>(0x524)!=0x5a5a5a5a||streamer->GetQuery(mixed_draw)->value!=256)throw std::runtime_error("mixed draw value");
 streamer->Free(mixed_zero);streamer->Free(mixed_draw);++cases;std::cout<<"ZERO_MIXED_DRAW_PASS zero=0 draw=256\n";
 reset();streamer->StartCounter();draw();report(256,true);++cases;
 reset();streamer->StartCounter();draw();draw();report(512,true);++cases;
 reset();streamer->StartCounter();draw();report(256,false);++cases;
 // The real streamer replication callback already amended the preceding Draw256.
 // No complete QueryCache CPU guest-completion callback is supplied by this lab.
 if(streamer->GetAmendValue()!=256)throw std::runtime_error("historical GPU accumulation not amended");
 seed(0x1560);auto bankless=streamer->WriteCounter(0x1560,false,0);
 if(streamer->HasPendingSync()||streamer->HasUnsyncedQueries())throw std::runtime_error("historical bankless enqueued as zero");
 finish_sync();if(source.read<u32>(0x560)!=0xa5a5a5a5||streamer->GetQuery(bankless)->value+streamer->GetAmendValue()!=256)throw std::runtime_error("historical bankless forced zero");
 streamer->Free(bankless);++cases;
 std::cout<<"ZERO_HISTORY_GATE_PASS amend=256 value_plus_amend=256 actualStreamerCPUamend=True fullCPUcompletion=False\n";
 std::cout<<"SAMPLES_ZERO_BACKEND_PASS cases="<<cases<<" actual_SamplesStreamer=True GPU=True full_QueryCacheRuntime=False\n";
 scheduler.Finish();drain();streamer.reset();scheduler.Finish();
 cleanup.Cancel();
 std::cout<<"SAMPLES_BACKEND_GPU_PASS actual_SamplesStreamer=True actual_Scheduler=True GPU=True full_QueryCacheRuntime=False\n";
 }
 std::cout<<"SAMPLES_VALIDATION errors="<<validation.errors.load()<<" warnings="<<validation.warnings.load()
          <<" loader_errors="<<validation.loader_errors.load()<<"\n";
 if(validation.errors.load()!=0)throw std::runtime_error("Vulkan validation errors");
 return 0;
} catch(const std::exception& e){std::cerr<<"LAB_ERROR "<<e.what()<<"\n";return 1;}
