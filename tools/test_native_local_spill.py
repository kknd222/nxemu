"""Bounded actual native local-memory lowering, IR/SPIRV and Vulkan numeric regression.

Synthetic PT cases use production STL/LDL translators and a synthetic SSBO consumer.
Predicate encoding is checked separately; direct visitor cases do not claim execution
of predication. An independent production CFG/TranslateProgram probe is also run.
No emulator launch, game input, ROM, keys, firmware, save or driver modification.
"""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, sys, traceback

CPP = r'''
#include "lower_override.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate_program.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "nxemu-video/video_settings.h"
#include <vulkan/vulkan.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <atomic>
using namespace Shader; using namespace Shader::IR;
VideoSettings videoSettings{};
struct Env final:Environment {
 std::map<u32,u64> code;
 Env(){stage=Stage::Compute;start_address=0;}
 u64 ReadInstruction(u32 address) override {return code.at(address);}
 u32 ReadCbufValue(u32,u32) override {throw std::runtime_error("unexpected cbuf");}
 TextureType ReadTextureType(u32) override {throw std::runtime_error("unexpected texture");}
 TexturePixelFormat ReadTexturePixelFormat(u32) override {throw std::runtime_error("unexpected texture");}
 bool IsTexturePixelFormatInteger(u32) override {throw std::runtime_error("unexpected texture");}
 u32 ReadViewportTransformState() override{return 1;}
 u32 TextureBoundBuffer()const override{return 17;}
 u32 LocalMemorySize()const override{return 512;}
 u32 SharedMemorySize()const override{return 0;}
 std::array<u32,3> WorkgroupSize()const override{return {1,1,1};}
 bool HasHLEMacroState()const override{return false;}
 std::optional<ReplaceConstant> GetReplaceConstBuffer(u32,u32)override{return {};}
 void Dump(u64,u64)override{}
};
struct Fixture{
 ObjectPool<Inst> insts;ObjectPool<Block> blocks;Program program;Block* block;
 Fixture(){block=blocks.Create(insts);block->SetOrder(0);program.blocks={block};program.post_order_blocks={block};program.stage=Stage::Compute;program.local_memory_size=512;program.workgroup_size={1,1,1};}
};
void bits(Ampere::Instruction& i,unsigned offset,unsigned n,u32 value){for(unsigned b=0;b<n;++b){auto& w=i.words[(offset+b)/32];const auto mask=1U<<((offset+b)%32);w=(w&~mask)|(((value>>b)&1)?mask:0);}}
Ampere::Instruction spill(bool store,unsigned size,unsigned value,unsigned offset,unsigned pred=7,unsigned address=255){
 Ampere::Instruction i{};bits(i,0,12,store?0x387:0x983);bits(i,12,4,pred);bits(i,24,8,address);bits(i,store?32:16,8,value);bits(i,40,24,offset);bits(i,73,3,size);bits(i,84,1,1);if(!store)bits(i,85,1,1);return i;
}
Ampere::Instruction exit_insn{{0x0000794d,0,0x03800000,0x000fea00}};
Ampere::LoweredProgram one(const Ampere::Instruction& ins){const std::array<Ampere::Instruction,2>x{ins,exit_insn};return Ampere::LowerToMaxwell(x);}
unsigned lowering_accept{},lowering_reject{},numeric_cases{},cfg_cases{},predicate_encoding_cases{},negative_cases{};
std::vector<u32> cfg_probe(const Ampere::LoweredProgram& lower,const std::filesystem::path& out,const std::string& name){
 Env env;for(unsigned n=0;n<lower.instructions.size();++n)env.code.emplace(8+n*8+(n/3)*8,lower.instructions[n]);
 ObjectPool<Maxwell::Flow::Block> flows;Maxwell::Flow::CFG cfg(env,flows,Maxwell::Location{0});ObjectPool<Inst> insts;ObjectPool<Block> blocks;
 auto program=Maxwell::TranslateProgram(insts,blocks,env,cfg,HostTranslateInfo{});assert(program.stage==Stage::Compute&&program.local_memory_size==512);
 // Insert only controlled input initialization and an output consumer. The
 // predicate/control-flow translation itself is the actual production CFG/ASL.
 IREmitter seed(*program.blocks.front(),program.blocks.front()->begin());for(unsigned n=0;n<5;++n)seed.WriteLocal(seed.Imm32(n),seed.Imm32(0xccccccccU));
 auto* tail=program.blocks.back();IREmitter ir(*tail);for(unsigned n=0;n<5;++n)tail->AppendNewInst(Opcode::WriteStorage32,{Value(0U),Value(n*4),Value(ir.LoadLocal(ir.Imm32(n)))});
 Optimization::CollectShaderInfoPass(env,program);program.info.storage_buffers_descriptors.push_back({static_cast<u32>(Info::MAX_CBUFS),0,1,true});
 Profile profile{};profile.supported_spirv=0x00010300;profile.unified_descriptor_binding=true;Backend::Bindings bindings{};auto spv=Backend::SPIRV::EmitSPIRV(profile,{},program,bindings);assert(bindings.unified==1);
 std::ofstream binary(out/(name+".comp.spv"),std::ios::binary);binary.write(reinterpret_cast<const char*>(spv.data()),spv.size()*4);std::ofstream irout(out/(name+".ir.txt"));irout<<DumpProgram(program);++cfg_cases;return spv;
}
Ampere::Instruction predicate_init(bool value){Ampere::Instruction i{};bits(i,0,12,0x20c);bits(i,12,4,7);bits(i,24,8,255);bits(i,32,8,255);bits(i,76,3,value?2:1);bits(i,81,3,0);bits(i,84,3,7);bits(i,87,3,7);return i;}
std::vector<u32> numeric(unsigned size,unsigned offset,bool zero,const std::filesystem::path& out,const std::string& name,bool dynamic=false){
 const unsigned words=size==6?4:size==5?2:1,effective=offset+(dynamic?32:0);auto st=spill(true,size,zero?255:0,effective);auto ld=spill(false,size,8,offset,7,dynamic?18:255);
 const std::array<Ampere::Instruction,3> native{st,ld,exit_insn};const auto lower=Ampere::LowerToMaxwell(native);assert(lower&&lower.instructions.size()==words*2+1);
 Fixture f;Env env;IREmitter ir(*f.block);const std::array<u32,4> values{0x12345678U,0xa5a5a5a5U,0xffffffffU,0x80000001U};
 for(unsigned n=0;n<4;++n)ir.SetReg(Reg(n),ir.Imm32(values[n]));
 if(dynamic)ir.SetReg(Reg::R18,ir.Imm32(32U));
 // Seed target and neighbours with nonzero bytes, so a zero store cannot pass
 // merely because private storage happened to start with zero.
 for(unsigned n=0;n<8;++n)ir.WriteLocal(ir.Imm32(effective/4+n),ir.Imm32(0xccccccccU));
 Maxwell::TranslatorVisitor v(env,*f.block);
 for(unsigned n=0;n<words;++n)v.STL(lower.instructions[n]);
 for(unsigned n=0;n<words;++n)v.LDL(lower.instructions[words+n]);
 for(unsigned n=0;n<words;++n)f.block->AppendNewInst(Opcode::WriteStorage32,{Value(0U),Value(n*4),Value(ir.GetReg(Reg(8+n)))});
 f.block->AppendNewInst(Opcode::WriteStorage32,{Value(0U),Value(words*4),Value(ir.LoadLocal(ir.Imm32(effective/4+words)))});
 Optimization::SsaRewritePass(f.program);Optimization::ConstantPropagationPass(env,f.program);Optimization::DeadCodeEliminationPass(f.program);Optimization::IdentityRemovalPass(f.program);Optimization::CollectShaderInfoPass(env,f.program);Optimization::VerificationPass(f.program);
 f.program.info.storage_buffers_descriptors.push_back({static_cast<u32>(Info::MAX_CBUFS),0,1,true});
 AbstractSyntaxNode block{};block.type=AbstractSyntaxNode::Type::Block;block.data.block=f.block;AbstractSyntaxNode ret{};ret.type=AbstractSyntaxNode::Type::Return;f.program.syntax_list={block,ret};
 Profile profile{};profile.supported_spirv=0x00010300;profile.unified_descriptor_binding=true;Backend::Bindings bindings{};
 auto spv=Backend::SPIRV::EmitSPIRV(profile,{},f.program,bindings);assert(bindings.unified==1&&spv[0]==0x07230203);
 std::ofstream binary(out/(name+".comp.spv"),std::ios::binary);binary.write(reinterpret_cast<const char*>(spv.data()),spv.size()*4);binary.close();
 std::ofstream irout(out/(name+".ir.txt"));irout<<DumpProgram(f.program);
 std::vector<u32> expected;for(unsigned n=0;n<words;++n)expected.push_back(zero?0:values[n]);expected.push_back(0xccccccccU);
 std::ofstream exp(out/(name+".expected.txt"));for(u32 value:expected)exp<<std::hex<<value<<'\n';++numeric_cases;return spv;
}
std::atomic<unsigned> validation_errors{},validation_warnings{},validation_messages{};
VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*){if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++validation_errors;if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)++validation_warnings;if(validation_messages.fetch_add(1)<32&&data&&data->pMessage)std::cerr<<"VULKAN_DIAGNOSTIC "<<std::string_view(data->pMessage).substr(0,512)<<'\n';return VK_FALSE;}
struct Gpu {
 VkInstance instance{};VkDebugUtilsMessengerEXT messenger{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};u32 family{};VkBuffer buffer{};VkDeviceMemory memory{};VkDescriptorSetLayout set_layout{};VkPipelineLayout pipeline_layout{};VkDescriptorPool descriptor_pool{};VkDescriptorSet descriptor{};VkCommandPool command_pool{};std::string name;
 static void ok(VkResult r){if(r!=VK_SUCCESS)throw std::runtime_error("Vulkan result="+std::to_string(r));}
 Gpu(){
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="NXEmu local spill regression";app.apiVersion=VK_API_VERSION_1_1;const char* layer="VK_LAYER_KHRONOS_validation";const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.enabledExtensionCount=1;ic.ppEnabledExtensionNames=&extension;ok(vkCreateInstance(&ic,nullptr,&instance));
  VkDebugUtilsMessengerCreateInfoEXT mc{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};mc.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;mc.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;mc.pfnUserCallback=diagnostic;auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));assert(create);ok(create(instance,&mc,nullptr,&messenger));
  u32 count=0;ok(vkEnumeratePhysicalDevices(instance,&count,nullptr));std::vector<VkPhysicalDevice> devices(count);ok(vkEnumeratePhysicalDevices(instance,&count,devices.data()));for(u32 n=0;n<count;++n){VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(devices[n],&p);std::cout<<"ADAPTER_CANDIDATE "<<n<<" "<<p.deviceName<<'\n';}const char* selected=std::getenv("NXEMU_LOCAL_SPILL_ADAPTER_INDEX");if(!count||(!selected&&count!=1))throw std::runtime_error("ambiguous or absent Vulkan adapter; explicit --adapter-index required");const u32 index=selected?std::stoul(selected):0;if(index>=count)throw std::runtime_error("adapter index out of range");physical=devices[index];VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);name=props.deviceName;std::cout<<"SELECTED_ADAPTER_INDEX "<<index<<'\n';
  vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues.data());family=~0U;for(u32 n=0;n<count;++n)if(queues[n].queueFlags&VK_QUEUE_COMPUTE_BIT){family=n;break;}assert(family!=~0U);
  VkPhysicalDeviceFeatures available{};vkGetPhysicalDeviceFeatures(physical,&available);if(!available.shaderImageGatherExtended)throw std::runtime_error("production SPIRV requires shaderImageGatherExtended");VkPhysicalDeviceFeatures enabled{};enabled.shaderImageGatherExtended=VK_TRUE;
  float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.pEnabledFeatures=&enabled;ok(vkCreateDevice(physical,&dc,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
  VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=256;bc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;ok(vkCreateBuffer(device,&bc,nullptr,&buffer));VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(physical,&mp);u32 type=~0U;for(u32 n=0;n<mp.memoryTypeCount;++n)if((req.memoryTypeBits&(1U<<n))&&(mp.memoryTypes[n].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=n;break;}assert(type!=~0U);VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=req.size;ma.memoryTypeIndex=type;ok(vkAllocateMemory(device,&ma,nullptr,&memory));ok(vkBindBufferMemory(device,buffer,memory,0));
  VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=1;sl.pBindings=&binding;ok(vkCreateDescriptorSetLayout(device,&sl,nullptr,&set_layout));VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&set_layout;ok(vkCreatePipelineLayout(device,&pl,nullptr,&pipeline_layout));
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc.maxSets=1;pc.poolSizeCount=1;pc.pPoolSizes=&ps;ok(vkCreateDescriptorPool(device,&pc,nullptr,&descriptor_pool));VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=descriptor_pool;da.descriptorSetCount=1;da.pSetLayouts=&set_layout;ok(vkAllocateDescriptorSets(device,&da,&descriptor));VkDescriptorBufferInfo bi{buffer,0,256};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=descriptor;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.descriptorCount=1;write.pBufferInfo=&bi;vkUpdateDescriptorSets(device,1,&write,0,nullptr);
  VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;ok(vkCreateCommandPool(device,&cp,nullptr,&command_pool));
 }
 void run(const std::vector<u32>& spv,const std::vector<u32>& expected){
  void* mapped{};ok(vkMapMemory(device,memory,0,256,0,&mapped));std::fill_n(static_cast<u32*>(mapped),64,0xdeadbeefU);vkUnmapMemory(device,memory);
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=spv.size()*4;sm.pCode=spv.data();VkShaderModule module{};ok(vkCreateShaderModule(device,&sm,nullptr,&module));VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;stage.module=module;stage.pName="main";VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage=stage;ci.layout=pipeline_layout;VkPipeline pipeline{};ok(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline));
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=command_pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer command{};ok(vkAllocateCommandBuffers(device,&ca,&command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ok(vkBeginCommandBuffer(command,&begin));vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&descriptor,0,nullptr);vkCmdDispatch(command,1,1,1);VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);ok(vkEndCommandBuffer(command));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;ok(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));ok(vkQueueWaitIdle(queue));
  ok(vkMapMemory(device,memory,0,256,0,&mapped));const auto* got=static_cast<const u32*>(mapped);for(unsigned n=0;n<expected.size();++n)if(got[n]!=expected[n]){std::cerr<<"GPU_MISMATCH lane="<<n<<" actual="<<got[n]<<" expected="<<expected[n]<<'\n';throw std::runtime_error("local-memory GPU mismatch");}assert(got[expected.size()]==0xdeadbeefU);vkUnmapMemory(device,memory);vkFreeCommandBuffers(device,command_pool,1,&command);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,module,nullptr);
 }
 ~Gpu(){if(device){vkDeviceWaitIdle(device);vkDestroyCommandPool(device,command_pool,nullptr);vkDestroyDescriptorPool(device,descriptor_pool,nullptr);vkDestroyPipelineLayout(device,pipeline_layout,nullptr);vkDestroyDescriptorSetLayout(device,set_layout,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,memory,nullptr);vkDestroyDevice(device,nullptr);}if(messenger){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));destroy(instance,messenger,nullptr);}if(instance)vkDestroyInstance(instance,nullptr);}
};
int main(int argc,char**argv)try{
 assert(argc==3);const bool baseline=std::string(argv[1])=="baseline";const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
 for(unsigned size:{4U,5U,6U})for(bool store:{false,true})for(unsigned value:{0U,8U,255U}){
  auto r=one(spill(store,size,value,0));const bool expected=baseline?(size==4&&value!=255):(store||value!=255);assert(bool(r)==expected);if(r)++lowering_accept;else ++lowering_reject;
 }
 for(unsigned pred:{0U,7U,8U,15U}){auto r=one(spill(true,4,0,16,pred));assert(r);if(pred==15){assert(r.instructions.size()==1);}else{assert(((r.instructions[0]>>16)&15)==pred);}++predicate_encoding_cases;}
 auto reject=[&](Ampere::Instruction i){auto r=one(i);assert(!r);++negative_cases;};
 for(bool store:{false,true}){reject(spill(store,3,0,0));reject(spill(store,7,0,0));reject(spill(store,4,0,1));reject(spill(store,4,0,0x800000));auto i=spill(store,4,0,0);bits(i,24,8,246);reject(i);i=spill(store,4,0,0);bits(i,76,1,1);reject(i);}
 {Ampere::Instruction captured{{0xff007387,0x000000ff,0x00100c00,0x000fe800}};auto r=one(captured);assert(bool(r)==!baseline);if(r){assert(r.instructions.size()==5);for(unsigned n=0;n<4;++n)assert(r.instructions[n]==(0xef5400000007ffffULL|(u64{n*4}<<20)));}}
 {Ampere::Instruction captured{{0x12107983,0x00000000,0x00100800,0x000ea200}};auto r=one(captured);assert(bool(r)==!baseline);if(r){assert(r.instructions.size()==2);assert(r.instructions[0]==0xef44000000071210ULL);}}
 if(!baseline){
  reject(spill(false,4,255,0));reject(spill(true,6,4,0x7ffff4));reject(spill(false,6,8,0x7ffff4));reject(spill(true,6,244,0));reject(spill(false,6,244,0));reject(spill(true,6,2,0));reject(spill(false,6,2,0));
  Ampere::Instruction uniform{};bits(uniform,0,12,0x9c3);bits(uniform,12,4,15);const std::array<Ampere::Instruction,3>x{uniform,spill(true,6,216,0),exit_insn};assert(!Ampere::LowerToMaxwell(x));++negative_cases;
  for(unsigned size:{4U,5U,6U}){const unsigned words=size==6?4:size==5?2:1;assert(one(spill(true,size,0,0x800000-4*words)));assert(one(spill(false,size,8,0x800000-4*words)));}
  for(unsigned size:{5U,6U})for(unsigned address=16;address<16+(size==6?4:2);++address)reject(spill(false,size,16,0,7,address));
  for(unsigned size:{4U,5U,6U})for(unsigned value:{0U,255U})reject(spill(true,size,value,0,7,18));
  Gpu gpu;unsigned id{};for(unsigned size:{4U,5U,6U})for(bool zero:{false,true})for(unsigned offset:{0U,16U})for(bool dynamic:{false,true}){
   const unsigned words=size==6?4:size==5?2:1;auto spv=numeric(size,offset,zero,out,std::to_string(id++),dynamic);std::vector<u32> expected;const std::array<u32,4> values{0x12345678U,0xa5a5a5a5U,0xffffffffU,0x80000001U};for(unsigned n=0;n<words;++n)expected.push_back(zero?0:values[n]);expected.push_back(0xccccccccU);gpu.run(spv,expected);
  }
  for(bool state:{false,true})for(unsigned predicate:{0U,8U}){const std::array<Ampere::Instruction,3> native{predicate_init(state),spill(true,6,255,0,predicate),exit_insn};auto lowered=Ampere::LowerToMaxwell(native);assert(lowered);auto spv=cfg_probe(lowered,out,"cfg-"+std::to_string(state)+"-"+std::to_string(predicate));const bool execute=state!=(predicate==8);std::vector<u32> expected(4,execute?0:0xccccccccU);expected.push_back(0xccccccccU);gpu.run(spv,expected);}
  std::cout<<"GPU_ADAPTER "<<gpu.name<<'\n';
 }
 assert(validation_errors==0&&validation_warnings==0);std::cout<<"RESULT {\"passed\":true,\"baseline\":"<<(baseline?"true":"false")<<",\"actual_lower\":true,\"native_accept\":"<<lowering_accept<<",\"native_rejected\":"<<lowering_reject<<",\"negative_cases\":"<<negative_cases<<",\"predicate_encoding_cases\":"<<predicate_encoding_cases<<",\"actual_local_translator_SSA_SPIRV_GPU_cases\":"<<numeric_cases<<",\"actual_CFG_TranslateProgram_cases\":"<<cfg_cases<<",\"predicate_GPU_execution\":"<<(!baseline?"true":"false")<<",\"Khronos_errors\":"<<validation_errors<<",\"Khronos_warnings\":"<<validation_warnings<<",\"full_ROM_replay\":false}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}
'''

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--lower-source', type=Path)
    ap.add_argument('--mode', choices=['baseline', 'modified'], default='modified')
    ap.add_argument('--adapter-index', type=int)
    a = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    build = repo / 'build/windows-ninja'
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    lower = (a.lower_source or repo / 'src/yuzu_shader_recompiler/frontend/ampere/lower_to_maxwell.h').resolve(strict=True)
    override = out / 'lower_override.h'; shutil.copy2(lower, override)
    cpp = out / 'native-local-spill.cpp'; cpp.write_text(CPP, encoding='utf-8')
    exe = out / 'native-local-spill.exe'; objects = out / 'objects'; objects.mkdir()
    sdk = Path(r'C:\VulkanSDK\1.4.357.0')
    local = repo / 'src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/load_store_local_shared.cpp'
    libs = [build / p for p in ['src/yuzu_shader_recompiler/Release/shader_recompiler.lib', 'src/common/Release/common.lib', 'src/yuzu_common/Release/yuzu_common.lib', 'external/fmt/Release/fmt.lib', 'external/sirit/src/Release/sirit.lib']]
    frozen = out / 'frozen'; frozen.mkdir(); frozen_libs = []
    for n, p in enumerate(libs):
        q = frozen / (str(n) + '-' + p.name); shutil.copy2(p, q); frozen_libs.append(q)
    command = ['cl', '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/MT', '/O2', '/I' + str(out), '/I' + str(repo / 'src'), '/I' + str(repo / 'external/boost'), '/I' + str(repo / 'external/fmt/include'), '/I' + str(build / 'src'), '/I' + str(sdk / 'Include'), str(cpp), str(local), '/Fe:' + str(exe), '/Fo:' + str(objects) + '\\', *map(str, frozen_libs), str(sdk / 'Lib/vulkan-1.lib'), '/link', '/OPT:REF', 'ws2_32.lib', 'winmm.lib', 'bcrypt.lib', 'crypt32.lib', 'iphlpapi.lib', 'psapi.lib', 'ole32.lib', 'shell32.lib']
    cmd = out / 'build.cmd'; cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n' + subprocess.list2cmdline(command) + '\n')
    paths = [Path(__file__), lower, override, cpp, local, *frozen_libs]
    hashes = {str(p): sha(p) for p in paths}
    report = dict(command=subprocess.list2cmdline([sys.executable, '-B', Path(__file__), *sys.argv[1:]]), compiler_command=subprocess.list2cmdline(command), mode=a.mode, input='synthetic native local spill 32/64/128; captured STL128 RZ form; synthetic SSBO consumer; actual lower/local translator/IR/SPIRV; GPU PT only', hashes=hashes)
    try:
        b = subprocess.run(['cmd', '/c', str(cmd)], capture_output=True, text=True, timeout=150)
        (out / 'build.log').write_text(b.stdout + b.stderr); report.update(stage='compile', exit_status=b.returncode, literal_stdout=b.stdout, literal_stderr=b.stderr); assert b.returncode == 0
        native = [str(exe), a.mode, str(out / 'cases')]
        import os
        env = os.environ.copy()
        if a.adapter_index is not None:
            assert a.adapter_index >= 0
            env['NXEMU_LOCAL_SPILL_ADAPTER_INDEX'] = str(a.adapter_index)
        report['explicit_adapter_index'] = a.adapter_index
        r = subprocess.run(native, capture_output=True, text=True, timeout=60, env=env)
        (out / 'run.log').write_text(r.stdout + r.stderr); report.update(stage='run', native_command=subprocess.list2cmdline(native), exit_status=r.returncode, literal_stdout=r.stdout, literal_stderr=r.stderr); assert r.returncode == 0
        result = json.loads(next(x[7:] for x in r.stdout.splitlines() if x.startswith('RESULT '))); report['result'] = result
        validator = sdk / 'Bin/spirv-val.exe'; validation = []
        for spv in sorted((out / 'cases').glob('*.spv')):
            v = subprocess.run([str(validator), '--target-env', 'vulkan1.1', str(spv)], capture_output=True, text=True, timeout=15)
            validation.append(dict(path=str(spv), sha256=sha(spv), exit_status=v.returncode, literal_stdout=v.stdout, literal_stderr=v.stderr)); assert v.returncode == 0
        report['SPIRV_validation'] = validation
        assert all(sha(Path(p)) == h for p, h in hashes.items()), 'Frozen input changed during test'
        report.update(stage='complete', inputs_unchanged=True, executable_sha256=sha(exe))
        (out / 'RESULT.json').write_text(json.dumps(report, indent=2))
        print('NATIVE_LOCAL_SPILL_PASS; mode=' + a.mode + '; actual_backend=True; GPU_cases=' + str(result['actual_local_translator_SSA_SPIRV_GPU_cases']))
    except BaseException:
        report['failure'] = traceback.format_exc(); (out / 'FAILURE.json').write_text(json.dumps(report, indent=2)); raise

if __name__ == '__main__': main()
