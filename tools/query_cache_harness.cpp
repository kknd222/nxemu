// Entire production QueryCacheBase with real GPU/channel/memory; CPU-controlled streamers.
#include "query_cache_fixture.h"
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
    OperatingSystemMock& os;
    explicit Modules(OperatingSystemMock& o):os(o){}
    void StartEmulation() override {throw std::runtime_error("StartEmulation unused");}
    void StopEmulation(bool) override {throw std::runtime_error("StopEmulation unused");}
    bool IsValid() const override {return true;}
    ISystemloader& Systemloader() override {throw std::runtime_error("loader unused");}
    IOperatingSystem& OperatingSystem() override {return os;}
    IVideo& Video() override {throw std::runtime_error("video unused");}
    ICpu& Cpu() override {throw std::runtime_error("cpu unused");}
};
struct CacheRuntime;
struct CacheTraits {using RuntimeType=CacheRuntime;};
struct CacheRuntime {
    Tegra::MaxwellDeviceMemoryManager& memory;
    VideoCommon::GuestStreamer<CacheTraits> payload;
    VideoCommon::GuestStreamer<CacheTraits> controlled_samples;
    unsigned barriers{},binds{},compares{},sync_writes{};
    VideoCommon::LookupData last{};
    explicit CacheRuntime(Tegra::MaxwellDeviceMemoryManager& m):memory(m),payload(0,*this),controlled_samples(21,*this){}
    VideoCommon::StreamerInterface* GetStreamerInterface(VideoCommon::QueryType t){
        if(t==VideoCommon::QueryType::Payload)return &payload;
        if(t==VideoCommon::QueryType::ZPassPixelCount64)return &controlled_samples;
        return nullptr;
    }
    template<class T>void SyncValues(std::span<T> values){for(auto& v:values){assert(v.size==4||v.size==8);auto* p=memory.GetPointer<u8>(v.address);assert(p);std::memcpy(p,&v.value,v.size);++sync_writes;}}
    template<class T>void SyncValues(std::vector<T>& v){SyncValues<T>(std::span<T>(v));}
    void Barriers(bool){++barriers;}
    void Bind3DEngine(Tegra::Engines::Maxwell3D* p){assert(p);++binds;}
    void ResumeHostConditionalRendering(){}
    void PauseHostConditionalRendering(){}
    void EndHostConditionalRendering(){}
    bool HostConditionalRenderingCompareValue(VideoCommon::LookupData x,bool dirty){assert(!dirty);last=x;++compares;return false;}
    bool HostConditionalRenderingCompareValues(VideoCommon::LookupData x,VideoCommon::LookupData,bool dirty,bool){return HostConditionalRenderingCompareValue(x,dirty);}
};
int main() try {
    using Q=VideoCommon::QueryType;using F=VideoCommon::QueryPropertiesFlags;
    Memory source;CacheRasterizer rasterizer;
    std::cerr<<"MILESTONE memory_ready\n";
    Tegra::Host1x::Host1x host(source);
    std::cerr<<"MILESTONE host_ready\n";
    auto& memory=host.MemoryManager();auto asid=memory.RegisterProcess(&source);
    memory.Map(0x1000,0,source.data.size(),asid,false);memory.BindInterface(&rasterizer);
    OperatingSystemMock os(source);Modules modules(os);
    Tegra::GPU gpu(modules,host,false,false);
    std::cerr<<"MILESTONE gpu_object_ready\n";
    assert(os.tick_calls==0); // genuine GPU construction does not enter OS
    Tegra::Control::ChannelState channel(1);
    channel.memory_manager=std::make_shared<Tegra::MemoryManager>(memory,32,1ULL<<24);
    channel.memory_manager->BindRasterizer(&rasterizer);
    channel.memory_manager->Map(0x20000,0x1000,source.data.size(),Tegra::PTEKind::PITCH,false);
    channel.Init(gpu,0x1234);channel.BindRasterizer(&rasterizer);
    std::cerr<<"MILESTONE channel_ready\n";
    CacheRuntime runtime(memory);
    VideoCommon::QueryCacheBase<CacheTraits> cache(gpu,rasterizer,memory,runtime);
    cache.CreateChannel(channel);cache.BindToChannel(1);assert(runtime.binds==1);
    std::cerr<<"MILESTONE cache_ready\n";
    unsigned cases=0;
    for(u32 value:{256U,0U,224U,0xffffffffU}){
        source.write<u64>(0x200,0xa5a5a5a5a5a5a5a5ULL);
        cache.CounterEnable(Q::ZPassPixelCount64,true);cache.CounterReport(0x20200,Q::ZPassPixelCount64,F{},value,0);
        assert(source.read<u32>(0x200)==0xa5a5a5a5&&rasterizer.operations.size()==1);++cases;
        cache.NotifyWFI();assert(source.read<u32>(0x200)==value&&source.read<u32>(0x204)==0xa5a5a5a5);++cases;
        cache.FlushRegion(0x1200,4);cache.CommitAsyncFlushes();assert(!cache.ShouldWaitAsyncFlushes());rasterizer.Drain();cache.PopAsyncFlushes();
        assert(source.read<u32>(0x200)==value&&!cache.HasUncommittedFlushes());++cases;
        std::cout<<"CACHE_ORDER_PASS value="<<value<<" order=CounterReport,NotifyWFI,FlushRegion,CommitAsyncFlushes,Drain,PopAsyncFlushes CPU_callbacks=True\n";
        cache.CounterEnable(Q::ZPassPixelCount64,false);cache.CounterReset(Q::ZPassPixelCount64);
    }
    source.data.fill(0xa5);cache.CounterReport(0x20200,Q::ZPassPixelCount64,F::HasTimeout|F::IsAFence,0x11223344,0);
    auto* long_query=static_cast<VideoCommon::StreamerInterface*>(&runtime.controlled_samples)->GetQuery(0);
    assert(long_query&&True(long_query->flags&VideoCommon::QueryFlagBits::HasTimestamp)&&True(long_query->flags&VideoCommon::QueryFlagBits::IsFence)&&os.tick_calls==0);
    cache.NotifyWFI();cache.CommitAsyncFlushes();rasterizer.Drain();cache.PopAsyncFlushes();
    assert(source.read<u64>(0x200)==0x11223344ULL&&source.read<u64>(0x208)==OperatingSystemMock::ticks&&source.read<u32>(0x210)==0xa5a5a5a5);++cases;
    assert(os.tick_calls==1);
    source.data.fill(0xa5);cache.CounterReport(0x20300,Q::Payload,F{},7,0);
    assert(source.read<u32>(0x300)==7);rasterizer.Drain();++cases; // existing fast payload path
    source.write<u32>(0x200,0xa5a5a5a5);cache.CounterReport(0x20200,Q::ZPassPixelCount64,F{},99,0);
    cache.InvalidateRegion(0x1200,4);cache.NotifyWFI();cache.CommitAsyncFlushes();rasterizer.Drain();cache.PopAsyncFlushes();
    assert(source.read<u32>(0x200)==0xa5a5a5a5);++cases;
    cache.CounterReport(0x20200,Q::ZPassPixelCount64,F{},1,0);
    auto* replaced=static_cast<VideoCommon::StreamerInterface*>(&runtime.controlled_samples)->GetQuery(0);
    cache.CounterReport(0x20200,Q::ZPassPixelCount64,F{},2,0);
    assert(replaced&&True(replaced->flags&VideoCommon::QueryFlagBits::IsRewritten));
    cache.NotifyWFI();cache.CommitAsyncFlushes();rasterizer.Drain();cache.PopAsyncFlushes();
    assert(source.read<u32>(0x200)==2);++cases;
    auto& engine=*channel.maxwell_3d;
    cache.CounterReport(0x20204,Q::ZPassPixelCount64,F{},9,0);
    engine.regs.render_enable.address_low=0x20200;engine.regs.render_enable.address_high=0;
    engine.regs.render_enable.mode=Tegra::Engines::Maxwell3D::Regs::RenderEnable::Mode::Conditional;
    engine.regs.render_enable_override=Tegra::Engines::Maxwell3D::Regs::RenderEnable::Override::UseRenderEnable;
    assert(!cache.AccelerateHostConditionalRendering()&&runtime.last.address==0x1200&&runtime.last.found_query);++cases;
    cache.NotifyWFI();cache.CommitAsyncFlushes();rasterizer.Drain();cache.PopAsyncFlushes();
    cache.EraseChannel(1);channel.memory_manager->Unmap(0x20000,source.data.size());
    std::cout<<"QUERY_CACHE_FULL_TEMPLATE_PASS cases="<<cases<<" actual_GPU_object=True actual_QueryCacheBase=True timestamp_calls="<<os.tick_calls<<" SamplesStreamer=False Vulkan=False\n";
} catch(const std::exception& error){std::cerr<<"HARNESS_ERROR "<<error.what()<<"\n";return 1;}
