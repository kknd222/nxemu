// Real production QueryCacheRuntime/BufferCache GPU to guest and condition differential.
#include "query_gpu_fixture.h"
#include "query_cache_os.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_video_core/host1x/host1x.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/engines/maxwell_dma.h"
#include "yuzu_video_core/engines/kepler_memory.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include <iostream>
IModuleSettings* g_settings=nullptr;
struct Modules final : ISystemModules {
 OperatingSystemMock& os;explicit Modules(OperatingSystemMock& o):os(o){}
 void StartEmulation() override {throw std::runtime_error("StartEmulation unused");}
 void StopEmulation(bool) override {throw std::runtime_error("StopEmulation unused");}
 bool IsValid() const override {return true;}
 ISystemloader& Systemloader() override {throw std::runtime_error("loader unused");}
 IOperatingSystem& OperatingSystem() override {return os;}
 IVideo& Video() override {throw std::runtime_error("video unused");}
 ICpu& Cpu() override {throw std::runtime_error("cpu unused");}
};
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
 if(argc!=4&&argc!=6&&argc!=7&&argc!=8)throw std::runtime_error("usage vertex.spv fragment.spv expected-equality-mismatches [capture-predicate expected-no-producer-defect [expected-history-defect [expected-accumulation-defect]]]");
 const bool capture_predicate=argc>=6&&std::stoi(argv[4])!=0;
 const bool expected_no_producer_defect=argc>=6&&std::stoi(argv[5])!=0;
 const bool expected_history_defect=argc>=7&&std::stoi(argv[6])!=0;
 const bool expected_accumulation_defect=argc==8&&std::stoi(argv[7])!=0;
 const unsigned expected_equality_mismatches=std::stoul(argv[3]);if(expected_equality_mismatches>16)throw std::runtime_error("mismatch count bound");
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
 if(!device.IsExtConditionalRendering())throw std::runtime_error("actual conditionalRendering feature not enabled");
 std::cout<<"REAL_CONDITIONAL_FEATURE_ENABLED value=1\n";
 StateTracker tracker;Scheduler scheduler(device,tracker);MemoryAllocator allocator(device);
 DescriptorPool pool(device,scheduler);ComputePassDescriptorQueue queue(device,scheduler);
 const auto& logical=device.GetLogical();
 VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image_ci.imageType=VK_IMAGE_TYPE_2D;
 image_ci.format=VK_FORMAT_R8G8B8A8_UNORM;image_ci.extent={16,16,1};image_ci.mipLevels=1;image_ci.arrayLayers=1;
 image_ci.samples=VK_SAMPLE_COUNT_1_BIT;image_ci.tiling=VK_IMAGE_TILING_OPTIMAL;image_ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
 image_ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image_ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
 auto image=allocator.CreateImage(image_ci);
 VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view_ci.image=*image;view_ci.viewType=VK_IMAGE_VIEW_TYPE_2D;
 view_ci.format=image_ci.format;view_ci.subresourceRange=range;auto view=logical.CreateImageView(view_ci);
 VkAttachmentDescription attachment{};attachment.format=image_ci.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
 attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
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

 Memory source;QueryGpuRasterizer rasterizer;
 Tegra::Host1x::Host1x host(source);auto& memory=host.MemoryManager();auto asid=memory.RegisterProcess(&source);
 memory.Map(0x1000,0,source.data.size(),asid,false);memory.BindInterface(&rasterizer);
 OperatingSystemMock os(source);Modules modules(os);Tegra::GPU gpu(modules,host,false,false);
 Tegra::Control::ChannelState channel(1);
 channel.memory_manager=std::make_shared<Tegra::MemoryManager>(memory,32,1ULL<<24);
 channel.memory_manager->BindRasterizer(&rasterizer);channel.memory_manager->Map(0x20000,0x1000,source.data.size(),Tegra::PTEKind::PITCH,false);
 channel.Init(gpu,0x1234);channel.BindRasterizer(&rasterizer);
 std::unique_ptr<StagingBufferPool> staging;
 std::unique_ptr<GuestDescriptorQueue> guest_queue;
 std::unique_ptr<BufferCacheRuntime> buffer_runtime;
 std::unique_ptr<BufferCache> buffer_cache;
 std::unique_ptr<QueryCacheRuntime> query_runtime;
 std::unique_ptr<QueryCache> cache;
 auto cleanup=SCOPE_GUARD {try {
   if(query_runtime)query_runtime->EndHostConditionalRendering();
   scheduler.RequestOutsideRenderPassOperationContext();if(cache&&!device.IsLost())cache->CommitAsyncFlushes();scheduler.Finish();
   if(cache&&!device.IsLost()){cache->PopAsyncFlushes();rasterizer.Drain();}
 }catch(...){std::cerr<<"CLEANUP_DRAIN_FAILED\n";std::_Exit(2);}};
 staging=std::make_unique<StagingBufferPool>(device,allocator,scheduler);
 guest_queue=std::make_unique<GuestDescriptorQueue>(device,scheduler);
 buffer_runtime=std::make_unique<BufferCacheRuntime>(device,allocator,scheduler,*staging,*guest_queue,queue,pool);
 buffer_cache=std::make_unique<BufferCache>(memory,*buffer_runtime);
 buffer_cache->CreateChannel(channel);buffer_cache->BindToChannel(1);
 query_runtime=std::make_unique<QueryCacheRuntime>(&rasterizer,memory,*buffer_cache,device,allocator,scheduler,*staging,queue,pool);
 cache=std::make_unique<QueryCache>(gpu,rasterizer,memory,*query_runtime);cache->CreateChannel(channel);cache->BindToChannel(1);
 scheduler.SetQueryCache(*cache);
 std::cerr<<"ACTUAL_RUNTIME_READY QueryCacheRuntime=True BufferCache=True SamplesStreamer=True\n";
 scheduler.Record([image=*image,range](vk::CommandBuffer cmd){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
 b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange=range;cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,nullptr,nullptr,vk::Span(&b,1));});
 auto draw=[&](u32 vertices=3){std::array<VkImage,1> images{*image};std::array<VkImageSubresourceRange,1> ranges{range};
 scheduler.RequestLabRenderpass(*pass,*frame,{16,16},images,ranges);
 scheduler.Record([pipeline=*pipeline,vertices](vk::CommandBuffer cmd){cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);cmd.Draw(vertices,1,0,0);});
 scheduler.RequestOutsideRenderPassOperationContext();};
 auto clear=[&]{scheduler.RequestOutsideRenderPassOperationContext();scheduler.Record([image=*image,range](vk::CommandBuffer cmd){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
 cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,b);VkClearColorValue black{};cmd.ClearColorImage(image,VK_IMAGE_LAYOUT_GENERAL,black,range);
 b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,b);});};
 auto read_pixels=[&]{VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=1024;ci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
 auto download=allocator.CreateBuffer(ci,MemoryUsage::Download);scheduler.RequestOutsideRenderPassOperationContext();scheduler.Record([image=*image,target=*download](vk::CommandBuffer cmd){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,b);
 VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={16,16,1};cmd.CopyImageToBuffer(image,VK_IMAGE_LAYOUT_GENERAL,target,vk::Span(&copy,1));
 b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,b);});
 scheduler.Finish();if(device.IsLost())throw std::runtime_error("device lost");download.Invalidate();auto bytes=download.Mapped();unsigned visible=0;for(unsigned i=0;i<256;++i)if(bytes[i*4]||bytes[i*4+1]||bytes[i*4+2])++visible;return visible;};
 auto read_predicate=[&](DAddr address){
  // NoSynchronize observes the buffer already selected by the production HCR
  // path. DoNothing does not upload CPU data or mark a GPU write.
  auto [buffer,offset]=buffer_cache->ObtainCPUBuffer(address,8,VideoCommon::ObtainBufferSynchronize::NoSynchronize,VideoCommon::ObtainBufferOperation::DoNothing);
  const VkBuffer source_buffer=buffer->Handle();const VkDeviceSize source_offset=offset;
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=8;ci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  auto download=allocator.CreateBuffer(ci,MemoryUsage::Download);
  auto finish_before_destroy=SCOPE_GUARD {try{scheduler.Finish();}catch(...){std::cerr<<"PREDICATE_PROBE_DRAIN_FAILED\n";std::_Exit(2);}};
  scheduler.RequestOutsideRenderPassOperationContext();
  scheduler.Record([source_buffer,source_offset,target=*download](vk::CommandBuffer cmd){
   VkBufferMemoryBarrier before{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};before.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
   before.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;before.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;before.buffer=source_buffer;before.offset=source_offset;before.size=8;
   cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,before);
   VkBufferCopy copy{source_offset,0,8};cmd.CopyBuffer(source_buffer,target,vk::Span(&copy,1));
   VkBufferMemoryBarrier after{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
   after.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;after.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;after.buffer=target;after.offset=0;after.size=8;
   cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,after);
  });
  scheduler.Finish();if(device.IsLost())throw std::runtime_error("predicate probe device lost");
  finish_before_destroy.Cancel();download.Invalidate();auto bytes=download.Mapped();if(bytes.size()<8)throw std::runtime_error("predicate download mapping missing");
  std::array<u32,2> words{};std::memcpy(words.data(),bytes.data(),8);
  std::cout<<"QUERY_GPU_PREDICATE_BUFFER address="<<address<<" buffer="<<source_buffer<<" offset="<<source_offset<<" gpu_low="<<words[0]<<" gpu_high="<<words[1]<<" bytes=8 source=actual_BufferCache_NoSynchronize_after_HCR upload=False inserted_Finish=True CPU_callbacks_drained=False\n";
  return words;
 };
 using Q=VideoCommon::QueryType;using F=VideoCommon::QueryPropertiesFlags;using Engine=Tegra::Engines::Maxwell3D;using Mode=Engine::Regs::RenderEnable::Mode;
 unsigned cases=0,conditional_mismatches=0,equality_mismatches=0;
 for(u32 count:{0U,256U})for(u32 mode:{0U,1U}){
  cache->CounterReset(Q::ZPassPixelCount64);scheduler.Finish();rasterizer.Drain();
  const u64 offset=0x200+cases*0x40;const DAddr daddr=0x1000+offset;const GPUVAddr gaddr=0x20000+offset;
  source.write<u32>(offset,0xa5a5a5a5);source.write<u32>(offset+4,mode);source.write<u64>(offset+8,0x8877665544332211ULL);source.write<u64>(offset+16,0x1234567800000000ULL);
  buffer_cache->WriteMemory(daddr,24);
  cache->CounterEnable(Q::ZPassPixelCount64,true);draw(count?3:0);cache->CounterEnable(Q::ZPassPixelCount64,false);
  cache->CounterReport(gaddr,Q::ZPassPixelCount64,F{},0,0);
  if(source.read<u32>(offset)!=0xa5a5a5a5)throw std::runtime_error("premature guest query write");
  cache->NotifyWFI();
  auto& engine=*channel.maxwell_3d;engine.regs.render_enable.address_low=static_cast<u32>(gaddr);engine.regs.render_enable.address_high=0;engine.regs.render_enable.mode=Mode::Conditional;engine.regs.render_enable_override=Engine::Regs::RenderEnable::Override::UseRenderEnable;
  clear();if(!cache->AccelerateHostConditionalRendering())throw std::runtime_error("host condition not accelerated");draw();query_runtime->EndHostConditionalRendering();
  auto visible=read_pixels();
  cache->CommitAsyncFlushes();scheduler.Finish();cache->PopAsyncFlushes();rasterizer.Drain();
  const auto actual_count=source.read<u32>(offset),actual_mode=source.read<u32>(offset+4);
  if(actual_count!=count||actual_mode!=mode)throw std::runtime_error("guest query report/mode mismatch");
  constexpr auto method=(offsetof(Engine::Regs,render_enable)+offsetof(Engine::Regs::RenderEnable,mode))/sizeof(u32);
  engine.CallMethod(static_cast<u32>(method),static_cast<u32>(Mode::Conditional),true);const bool cpu=engine.ShouldExecute();
  if(cpu!=(count!=0&&mode!=0))throw std::runtime_error("actualCPU condition mismatch");
  const bool equal=visible==(cpu?256U:0U);if(!equal)++conditional_mismatches;
  std::cout<<"QUERY_GPU_CONDITIONAL_CASE count="<<count<<" mode="<<mode<<" guest_count="<<actual_count<<" guest_mode="<<actual_mode<<" CPU_should_execute="<<cpu<<" host_visible_pixels="<<visible<<" local_AND_expected="<<cpu<<" independent_Ryujinx_OR_expected="<<(count!=0||mode!=0)<<" equal="<<equal<<"\n";++cases;
 }
 for(unsigned index=0;index<10;++index) {
  const u32 count=index<8?(index/4)*256:0;const u32 high=index<8?(index/2)%2:1;
  const Mode mode=index%2==0?Mode::IfEqual:Mode::IfNotEqual;
  cache->CounterReset(Q::ZPassPixelCount64);scheduler.Finish();rasterizer.Drain();
  const u64 offset=index<8?0x304+index*0x40:(index==8?0xffc:0x1ffc);const DAddr daddr=0x1000+offset;const GPUVAddr gaddr=0x20000+offset;
  source.write<u32>(offset,0xa5a5a5a5);source.write<u32>(offset+4,high);source.write<u64>(offset+8,0x8877665544332211ULL);source.write<u64>(offset+16,0);
  buffer_cache->WriteMemory(daddr,24);
  cache->CounterEnable(Q::ZPassPixelCount64,true);draw(count?3:0);cache->CounterEnable(Q::ZPassPixelCount64,false);
  cache->CounterReport(gaddr,Q::ZPassPixelCount64,F{},0,0);cache->NotifyWFI();
  auto& engine=*channel.maxwell_3d;engine.regs.render_enable.address_low=static_cast<u32>(gaddr);engine.regs.render_enable.address_high=0;engine.regs.render_enable.mode=mode;engine.regs.render_enable_override=Engine::Regs::RenderEnable::Override::UseRenderEnable;
  clear();if(!cache->AccelerateHostConditionalRendering())throw std::runtime_error("zero pair shortcut not accelerated");draw();query_runtime->EndHostConditionalRendering();
  auto visible=read_pixels();cache->CommitAsyncFlushes();scheduler.Finish();cache->PopAsyncFlushes();rasterizer.Drain();
  if(source.read<u32>(offset)!=count||source.read<u32>(offset+4)!=high||source.read<u64>(offset+16)!=0)throw std::runtime_error("equal guest report preservation mismatch");
  constexpr auto method=(offsetof(Engine::Regs,render_enable)+offsetof(Engine::Regs::RenderEnable,mode))/sizeof(u32);
  engine.CallMethod(static_cast<u32>(method),static_cast<u32>(mode),true);const bool cpu=engine.ShouldExecute();
  const bool initial_zero=count==0&&high==0;const bool independent64expected=(mode==Mode::IfEqual)?initial_zero:!initial_zero;
  if(cpu!=independent64expected)throw std::runtime_error("actualCPU 64bit equality mismatch");
  const bool equal=visible==(cpu?256U:0U);if(!equal)++equality_mismatches;
  std::cout<<"QUERY_GPU_EQUALITY_CASE mode="<<(mode==Mode::IfEqual?"IfEqual":"IfNotEqual")<<" initial=("<<count<<","<<high<<") current=(0,0) source_offset="<<offset<<" CPU_should_execute="<<cpu<<" independent64_expected="<<independent64expected<<" host_visible_pixels="<<visible<<" equal="<<equal<<"\n";++cases;
 }
 for(u32 current_high:{1U,2U})for(Mode mode:{Mode::IfEqual,Mode::IfNotEqual}) {
  cache->CounterReset(Q::ZPassPixelCount64);scheduler.Finish();rasterizer.Drain();
  const u64 offset=0x800+(current_high-1)*0x100+(mode==Mode::IfEqual?4:0x44);const DAddr daddr=0x1000+offset;const GPUVAddr gaddr=0x20000+offset;
  source.write<u32>(offset,0xa5a5a5a5);source.write<u32>(offset+4,1);source.write<u64>(offset+8,0x8877665544332211ULL);source.write<u64>(offset+16,0xa5a5a5a5a5a5a5a5ULL);buffer_cache->WriteMemory(daddr,24);
  cache->CounterEnable(Q::ZPassPixelCount64,true);draw(0);cache->CounterEnable(Q::ZPassPixelCount64,false);cache->CounterReport(gaddr,Q::ZPassPixelCount64,F{},0,0);
  auto [current_buffer,current_offset]=buffer_cache->ObtainCPUBuffer(daddr+16,8,VideoCommon::ObtainBufferSynchronize::FullSynchronize,VideoCommon::ObtainBufferOperation::MarkAsWritten);
  buffer_runtime->ClearBuffer(current_buffer->Handle(),current_offset,4,0);buffer_runtime->ClearBuffer(current_buffer->Handle(),current_offset+4,4,current_high);
  cache->NotifyWFI();
  auto& engine=*channel.maxwell_3d;engine.regs.render_enable.address_low=static_cast<u32>(gaddr);engine.regs.render_enable.address_high=0;engine.regs.render_enable.mode=mode;engine.regs.render_enable_override=Engine::Regs::RenderEnable::Override::UseRenderEnable;
  clear();if(!cache->AccelerateHostConditionalRendering())throw std::runtime_error("Equal24 BC route not accelerated");draw();query_runtime->EndHostConditionalRendering();
  auto visible=read_pixels();cache->CommitAsyncFlushes();scheduler.Finish();cache->PopAsyncFlushes();rasterizer.Drain();buffer_cache->DownloadMemory(daddr+16,8);
  if(source.read<u32>(offset)!=0||source.read<u32>(offset+4)!=1||source.read<u32>(offset+16)!=0||source.read<u32>(offset+20)!=current_high)throw std::runtime_error("Equal24 actualGPU current pair download mismatch");
  constexpr auto method=(offsetof(Engine::Regs,render_enable)+offsetof(Engine::Regs::RenderEnable,mode))/sizeof(u32);engine.CallMethod(static_cast<u32>(method),static_cast<u32>(mode),true);const bool cpu=engine.ShouldExecute();
  const bool independent64expected=(mode==Mode::IfEqual)?current_high==1:current_high!=1;if(cpu!=independent64expected)throw std::runtime_error("Equal24 actualCPU mismatch");const bool equal=visible==(cpu?256U:0U);if(!equal)++equality_mismatches;
  std::cout<<"QUERY_GPU_EQUAL24_CASE mode="<<(mode==Mode::IfEqual?"IfEqual":"IfNotEqual")<<" initial=(0,1) current=(0,"<<current_high<<") source_offset="<<offset<<" CPU_should_execute="<<cpu<<" host_visible_pixels="<<visible<<" actualBC_download=True equal="<<equal<<"\n";++cases;
 }

 {
  cache->CounterReset(Q::ZPassPixelCount64);scheduler.Finish();rasterizer.Drain();
  const u64 offset=0x200+cases*0x40;const DAddr daddr=0x1000+offset;const GPUVAddr gaddr=0x20000+offset;
  source.write<u32>(offset,0xa5a5a5a5);source.write<u32>(offset+4,0);source.write<u64>(offset+8,0);source.write<u64>(offset+16,0);buffer_cache->WriteMemory(daddr,24);
  cache->CounterReport(gaddr,Q::ZPassPixelCount64,F{},0,0);
  auto* samples=query_runtime->GetStreamerInterface(Q::ZPassPixelCount64);VideoCommon::QueryBase* query=nullptr;
  for(size_t id=0;id<64;++id){auto* candidate=samples->GetQuery(id);if(!candidate)break;if(candidate->guest_address==daddr){if(query)throw std::runtime_error("ambiguous query slot");query=candidate;}}
  if(!query)throw std::runtime_error("no-producer public query not found");
  const u64 query_value=query->value;const u32 query_flags=static_cast<u32>(query->flags);const bool pending_sync=samples->HasPendingSync();
  cache->NotifyWFI();
  auto& engine=*channel.maxwell_3d;engine.regs.render_enable.address_low=static_cast<u32>(gaddr);engine.regs.render_enable.address_high=0;engine.regs.render_enable.mode=Mode::Conditional;engine.regs.render_enable_override=Engine::Regs::RenderEnable::Override::UseRenderEnable;
  clear();if(!cache->AccelerateHostConditionalRendering())throw std::runtime_error("no-producer condition not accelerated");
  const u32 cpu_pending_low=source.read<u32>(offset),cpu_pending_high=source.read<u32>(offset+4);
  std::array<u32,2> predicate_words{};
  if(capture_predicate){predicate_words=read_predicate(daddr);if(source.read<u32>(offset)!=cpu_pending_low||source.read<u32>(offset+4)!=cpu_pending_high)throw std::runtime_error("predicate probe drained or changed CPU backing");}
  draw();query_runtime->EndHostConditionalRendering();
  auto visible=read_pixels();cache->CommitAsyncFlushes();scheduler.Finish();cache->PopAsyncFlushes();rasterizer.Drain();
  if(source.read<u32>(offset)!=0||source.read<u32>(offset+4)!=0)throw std::runtime_error("no-producer guest report mismatch");
  constexpr auto method=(offsetof(Engine::Regs,render_enable)+offsetof(Engine::Regs::RenderEnable,mode))/sizeof(u32);engine.CallMethod(static_cast<u32>(method),static_cast<u32>(Mode::Conditional),true);
  if(engine.ShouldExecute())throw std::runtime_error("zero-pair actualCPU condition mismatch");
  std::cout<<"QUERY_GPU_NO_PRODUCER_OBSERVATION report_value="<<query_value<<" flags="<<query_flags<<" pending_sync="<<pending_sync<<" cpu_pending_low="<<cpu_pending_low<<" cpu_pending_high="<<cpu_pending_high<<" capture_enabled="<<capture_predicate<<" guest_after_retirement=(0,0) local_AND_expected=0 independent_OR_expected=0 host_visible_pixels="<<visible<<" mismatch="<<(visible!=0)<<" expected_defect="<<expected_no_producer_defect<<" unresolved="<<expected_no_producer_defect<<"\n";
  if(visible!=(expected_no_producer_defect?256U:0U))throw std::runtime_error("unexpected no-producer predicate outcome");
  if(capture_predicate&&(predicate_words[0]!=(expected_no_producer_defect?0xa5a5a5a5U:0U)||predicate_words[1]!=0))throw std::runtime_error("unexpected actualGPU predicate words");
  std::cout<<(expected_no_producer_defect?"QUERY_GPU_NO_PRODUCER_DEFECT_CONFIRMED":"QUERY_GPU_NO_PRODUCER_STAGE_PASS")<<" capture_enabled="<<capture_predicate<<" host_visible_pixels="<<visible<<"\n";++cases;
 }

 for(u32 history_count:{256U,512U})for(bool retire_producer:{false,true}) {
  // Both deferred and retired producers use actual cache callbacks, no injected sum.
  cache->CounterReset(Q::ZPassPixelCount64);scheduler.Finish();rasterizer.Drain();
  const u64 first=0xc00+(history_count==512?0x200:0)+(retire_producer?0x100:0),second=first+0x40;
  source.write<u64>(first,0xa5a5a5a5a5a5a5a5ULL);buffer_cache->WriteMemory(0x1000+first,8);
  cache->CounterEnable(Q::ZPassPixelCount64,true);draw();if(history_count==512)draw();cache->CounterEnable(Q::ZPassPixelCount64,false);
  cache->CounterReport(0x20000+first,Q::ZPassPixelCount64,F{},0,0);
  cache->NotifyWFI();
  if(retire_producer){cache->CommitAsyncFlushes();scheduler.Finish();cache->PopAsyncFlushes();rasterizer.Drain();if(source.read<u32>(first)!=history_count)throw std::runtime_error("history producer report mismatch");}
  source.write<u32>(second,0);source.write<u32>(second+4,1);buffer_cache->WriteMemory(0x1000+second,8);
  cache->CounterReport(0x20000+second,Q::ZPassPixelCount64,F{},0,0);
  auto* samples=query_runtime->GetStreamerInterface(Q::ZPassPixelCount64);
  const bool pending=samples->HasPendingSync();cache->NotifyWFI();
  auto& engine=*channel.maxwell_3d;engine.regs.render_enable.address_low=static_cast<u32>(0x20000+second);engine.regs.render_enable.address_high=0;engine.regs.render_enable.mode=Mode::Conditional;engine.regs.render_enable_override=Engine::Regs::RenderEnable::Override::UseRenderEnable;
  clear();if(!cache->AccelerateHostConditionalRendering())throw std::runtime_error("history condition not accelerated");
  if(source.read<u32>(second)!=0||source.read<u32>(second+4)!=1)throw std::runtime_error("premature history CPU publication");
  std::array<u32,2> words{};if(capture_predicate){words=read_predicate(0x1000+second);if(source.read<u32>(second)!=0)throw std::runtime_error("history probe drained CPU callbacks");}
  draw();query_runtime->EndHostConditionalRendering();auto visible=read_pixels();
  cache->CommitAsyncFlushes();scheduler.Finish();cache->PopAsyncFlushes();rasterizer.Drain();
  if(source.read<u32>(first)!=history_count||source.read<u32>(second)!=history_count||source.read<u32>(second+4)!=1)throw std::runtime_error("historical accumulated report changed");
  constexpr auto method=(offsetof(Engine::Regs,render_enable)+offsetof(Engine::Regs::RenderEnable,mode))/sizeof(u32);engine.CallMethod(static_cast<u32>(method),static_cast<u32>(Mode::Conditional),true);
  if(!engine.ShouldExecute())throw std::runtime_error("history actualCPU consumer failed");
  std::cout<<"QUERY_GPU_HISTORY_OBSERVATION count="<<history_count<<" producer_retired="<<retire_producer<<" pending_sync="<<pending<<" gpu_low="<<(capture_predicate?std::to_string(words[0]):"unobserved")<<" gpu_high="<<(capture_predicate?std::to_string(words[1]):"unobserved")<<" bankless_report="<<source.read<u32>(second)<<" CPU_should_execute=1 host_visible_pixels="<<visible<<" expected_defect="<<expected_history_defect<<"\n";
  if(visible!=(expected_history_defect?0U:256U))throw std::runtime_error("unexpected history consumer pixels");
  if(capture_predicate&&(words[0]!=(expected_history_defect?0U:history_count)||words[1]!=1))throw std::runtime_error("unexpected history GPU report value");
  std::cout<<(expected_history_defect?"QUERY_GPU_HISTORY_DEFECT_CONFIRMED":"QUERY_GPU_HISTORY_STAGE_PASS")<<" count="<<history_count<<" producer_retired="<<retire_producer<<" actual_QueryCache_completion=True\n";++cases;
 }
 for(bool retire_producer:{false,true}) {
  cache->CounterReset(Q::ZPassPixelCount64);scheduler.Finish();rasterizer.Drain();
  const u64 first=retire_producer?0x1100:0x1000,second=first+0x40;
  source.write<u32>(first,0xa5a5a5a5);buffer_cache->WriteMemory(0x1000+first,4);
  cache->CounterEnable(Q::ZPassPixelCount64,true);draw();cache->CounterEnable(Q::ZPassPixelCount64,false);
  cache->CounterReport(0x20000+first,Q::ZPassPixelCount64,F{},0,0);cache->NotifyWFI();
  if(retire_producer){cache->CommitAsyncFlushes();scheduler.Finish();cache->PopAsyncFlushes();rasterizer.Drain();}
  source.write<u32>(second,0);source.write<u32>(second+4,1);source.write<u64>(second+8,0x8877665544332211ULL);source.write<u32>(second+16,512);source.write<u32>(second+20,1);buffer_cache->WriteMemory(0x1000+second,24);
  // A second real draw without Reset must extend the epoch total to512.
  cache->CounterEnable(Q::ZPassPixelCount64,true);draw();cache->CounterEnable(Q::ZPassPixelCount64,false);
  cache->CounterReport(0x20000+second,Q::ZPassPixelCount64,F{},0,0);cache->NotifyWFI();
  // Force the actual Equal24 BC route with an independently GPU-written current
  // pair; otherwise the existing both-QC/CPU route deliberately ends HCR.
  auto [current_buffer,current_offset]=buffer_cache->ObtainCPUBuffer(0x1000+second+16,8,VideoCommon::ObtainBufferSynchronize::FullSynchronize,VideoCommon::ObtainBufferOperation::MarkAsWritten);
  buffer_runtime->ClearBuffer(current_buffer->Handle(),current_offset,4,512);
  buffer_runtime->ClearBuffer(current_buffer->Handle(),current_offset+4,4,1);
  auto& engine=*channel.maxwell_3d;engine.regs.render_enable.address_low=static_cast<u32>(0x20000+second);engine.regs.render_enable.address_high=0;engine.regs.render_enable.mode=Mode::IfEqual;engine.regs.render_enable_override=Engine::Regs::RenderEnable::Override::UseRenderEnable;
  clear();if(!cache->AccelerateHostConditionalRendering())throw std::runtime_error("cross WFI comparison not accelerated");
  std::array<u32,2> words{};if(capture_predicate){words=read_predicate(0x1000+second);if(source.read<u32>(second)!=0)throw std::runtime_error("accumulation probe drained CPU callbacks");}
  draw();query_runtime->EndHostConditionalRendering();auto visible=read_pixels();cache->CommitAsyncFlushes();scheduler.Finish();cache->PopAsyncFlushes();rasterizer.Drain();
  if(source.read<u32>(first)!=256||source.read<u32>(second)!=512||source.read<u32>(second+4)!=1||source.read<u32>(second+16)!=512||source.read<u32>(second+20)!=1)throw std::runtime_error("cross WFI guest total mismatch");
  constexpr auto method=(offsetof(Engine::Regs,render_enable)+offsetof(Engine::Regs::RenderEnable,mode))/sizeof(u32);engine.CallMethod(static_cast<u32>(method),static_cast<u32>(Mode::IfEqual),true);
  if(!engine.ShouldExecute())throw std::runtime_error("cross WFI actualCPU equality failed");
  std::cout<<"QUERY_GPU_ACCUMULATION_OBSERVATION producer_retired="<<retire_producer<<" gpu_low="<<(capture_predicate?std::to_string(words[0]):"unobserved")<<" gpu_high="<<(capture_predicate?std::to_string(words[1]):"unobserved")<<" guest_total=512 comparison=(512,1) CPU_should_execute=1 host_visible_pixels="<<visible<<" expected_defect="<<expected_accumulation_defect<<"\n";
  if(visible!=(expected_accumulation_defect?0U:256U))throw std::runtime_error("unexpected accumulated equality consumer pixels");
  if(capture_predicate&&(words[0]!=(expected_accumulation_defect?256U:512U)||words[1]!=1))throw std::runtime_error("unexpected cross WFI GPU total");
  std::cout<<(expected_accumulation_defect?"QUERY_GPU_ACCUMULATION_DEFECT_CONFIRMED":"QUERY_GPU_ACCUMULATION_STAGE_PASS")<<" producer_retired="<<retire_producer<<" actual_QueryCache_completion=True\n";++cases;
 }
 for(unsigned edge=0;edge<3;++edge) {
  cache->CounterReset(Q::ZPassPixelCount64);scheduler.Finish();rasterizer.Drain();
  const u64 first=0x1400+edge*0x100,second=first+0x40;
  auto produce=[&]{cache->CounterEnable(Q::ZPassPixelCount64,true);draw();cache->CounterEnable(Q::ZPassPixelCount64,false);};
  auto retire=[&]{cache->CommitAsyncFlushes();scheduler.Finish();cache->PopAsyncFlushes();rasterizer.Drain();};
  source.write<u32>(first,0xa5a5a5a5);buffer_cache->WriteMemory(0x1000+first,4);
  if(edge==0){produce();cache->CounterReset(Q::ZPassPixelCount64);produce();}
  else produce();
  cache->CounterReport(0x20000+first,Q::ZPassPixelCount64,F{},0,0);
  if(edge==1)cache->CounterReset(Q::ZPassPixelCount64);
  if(edge==2)produce(); // Newer current query exceeds the guest report's range.
  cache->NotifyWFI();retire();
  if(source.read<u32>(first)!=256)throw std::runtime_error("carry edge first report mismatch");
  const u32 expected=edge==0?512:edge==1?256:768;
  source.write<u32>(second,0);source.write<u32>(second+4,1);source.write<u64>(second+8,0x8877665544332211ULL);source.write<u32>(second+16,expected);source.write<u32>(second+20,1);buffer_cache->WriteMemory(0x1000+second,24);
  produce();cache->CounterReport(0x20000+second,Q::ZPassPixelCount64,F{},0,0);cache->NotifyWFI();
  auto [current_buffer,current_offset]=buffer_cache->ObtainCPUBuffer(0x1000+second+16,8,VideoCommon::ObtainBufferSynchronize::FullSynchronize,VideoCommon::ObtainBufferOperation::MarkAsWritten);
  buffer_runtime->ClearBuffer(current_buffer->Handle(),current_offset,4,expected);buffer_runtime->ClearBuffer(current_buffer->Handle(),current_offset+4,4,1);
  auto& engine=*channel.maxwell_3d;engine.regs.render_enable.address_low=static_cast<u32>(0x20000+second);engine.regs.render_enable.address_high=0;engine.regs.render_enable.mode=Mode::IfEqual;engine.regs.render_enable_override=Engine::Regs::RenderEnable::Override::UseRenderEnable;
  clear();if(!cache->AccelerateHostConditionalRendering())throw std::runtime_error("carry edge comparison not accelerated");
  std::array<u32,2> words{};if(capture_predicate){words=read_predicate(0x1000+second);if(source.read<u32>(second)!=0)throw std::runtime_error("carry edge probe drained callbacks");}
  draw();query_runtime->EndHostConditionalRendering();auto visible=read_pixels();retire();
  if(source.read<u32>(second)!=expected||source.read<u32>(second+4)!=1||visible!=256)throw std::runtime_error("carry edge CPU/GPU consumer mismatch");
  if(capture_predicate&&(words[0]!=expected||words[1]!=1))throw std::runtime_error("carry edge GPU count mismatch");
  std::cout<<"QUERY_GPU_CARRY_EDGE_PASS edge="<<edge<<" guest_total="<<expected<<" host_visible_pixels=256 actual_unreported_draw=True actual_Reset=True\n";++cases;
 }
 scheduler.Finish();rasterizer.Drain();cache->EraseChannel(1);buffer_cache->EraseChannel(1);channel.memory_manager->Unmap(0x20000,source.data.size());cleanup.Cancel();
 if(equality_mismatches!=expected_equality_mismatches)throw std::runtime_error("unexpected 64bit equality mismatch count");
 std::cout<<(expected_equality_mismatches?"QUERY_GPU_EQUALITY_DEFECT_CONFIRMED":"QUERY_GPU_EQUALITY_STAGE_PASS")<<" cases="<<cases<<" equality_mismatches="<<equality_mismatches<<" unresolved_conditional_mismatches="<<conditional_mismatches<<" actual_QueryCacheRuntime=True actual_BufferCache=True actual_SamplesStreamer=True actualCPU=True full_semantics_acceptance=False\n";

 }

 std::cout<<"SAMPLES_VALIDATION errors="<<validation.errors.load()<<" warnings="<<validation.warnings.load()
          <<" loader_errors="<<validation.loader_errors.load()<<"\n";
 if(validation.errors.load()!=0)throw std::runtime_error("Vulkan validation errors");
 return 0;
} catch(const std::exception& e){std::cerr<<"LAB_ERROR "<<e.what()<<"\n";return 1;}
