// Actual production MemoryManager/Maxwell3D consumer; CPU adapter, not host conditional Vulkan.
#include "query_guest_fixture.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include <iostream>
IModuleSettings* g_settings=nullptr; // settings storage is not exercised by this CPU fixture
struct Traits { using RuntimeType = Runtime; };
struct CpuRasterizer final : VideoCore::RasterizerInterface {
    unsigned flushes{},mapping_notifications{},invalidations{};
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
    void SyncOperation(std::function<void()>&&) override {unused();}
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
int main(int argc,char** argv) try {
    assert(argc==4);
    using Engine=Tegra::Engines::Maxwell3D;
    using Mode=Engine::Regs::RenderEnable::Mode;
    using Override=Engine::Regs::RenderEnable::Override;
    using Compare=Engine::Regs::ReportSemaphore::Compare;
    static_assert(sizeof(Compare)==24 && offsetof(Compare,current_sequence)==16);
    Memory source;
    std::cerr<<"MILESTONE backing_ready\n";
    Tegra::MaxwellDeviceMemoryManager memory(source);
    auto asid=memory.RegisterProcess(&source);
    memory.Map(0x1000,0,source.data.size(),asid,false);
    std::cerr<<"MILESTONE device_map_ready\n";
    Tegra::MemoryManager gpu_memory(memory,32,1ULL<<24);
    std::cerr<<"MILESTONE gpu_memory_ready\n";
    CpuRasterizer rasterizer;
    gpu_memory.BindRasterizer(&rasterizer);
    assert(gpu_memory.Map(0x20000,0x1000,source.data.size(),Tegra::PTEKind::PITCH,false)==0x20000);
    std::cerr<<"MILESTONE gpu_map_ready\n";
    Engine engine(gpu_memory);
    std::cerr<<"MILESTONE engine_ready\n";
    engine.BindRasterizer(&rasterizer);
    Runtime runtime{memory};
    VideoCommon::GuestStreamer<Traits> streamer(0,runtime);
    constexpr GPUVAddr report=0x20200;
    engine.regs.render_enable.address_low=static_cast<u32>(report);
    engine.regs.render_enable.address_high=0;
    engine.regs.render_enable_override=Override::UseRenderEnable;
    unsigned cases=0;
    auto check=[&](Mode mode,bool expected) {
        constexpr auto method=(offsetof(Engine::Regs,render_enable)+
                               offsetof(Engine::Regs::RenderEnable,mode))/sizeof(u32);
        engine.CallMethod(static_cast<u32>(method),static_cast<u32>(mode),true);
        if(engine.ShouldExecute()!=expected)throw std::runtime_error("actual CPU condition mismatch");
        ++cases;
    };
    for(int i=1;i<argc;++i) {
        auto parsed=std::stoull(argv[i]);assert(parsed<=0xffffffffULL);u32 sample=static_cast<u32>(parsed);
        Compare cmp{0,1,0xaaaa5555,0x5555aaaa,sample,1};
        assert(source.WriteBlock(0x200,&cmp,sizeof(cmp)));
        check(Mode::Conditional,false); // pending query has not written report yet
        const auto id=streamer.WriteCounter(0x1200,false,sample);
        assert(streamer.HasPendingSync());
        check(Mode::Conditional,false);
        streamer.SyncWrites();
        assert(source.read<u32>(0x200)==sample && source.read<u32>(0x204)==1);
        check(Mode::Conditional,sample!=0);
        check(Mode::IfEqual,true);
        check(Mode::IfNotEqual,false);
        source.write<u32>(0x214,2); // current mode differs, sequence stays equal
        check(Mode::IfEqual,false);
        check(Mode::IfNotEqual,true);
        static_cast<VideoCommon::StreamerInterface*>(&streamer)->Free(id);
        std::cout<<"GUEST_TO_ACTUAL_CPU_CONDITION_PASS sample="<<sample<<"\n";
    }
    Compare cmp{9,0,0,0,9,0};source.WriteBlock(0x200,&cmp,sizeof(cmp));
    check(Mode::Conditional,false); // a nonzero u64 report alone is insufficient
    check(Mode::IfEqual,true);
    check(Mode::True,true);check(Mode::False,false);
    engine.regs.render_enable_override=Override::AlwaysRender;check(Mode::False,true);
    engine.regs.render_enable_override=Override::NeverRender;check(Mode::True,false);
    engine.regs.render_enable_override=Override::UseRenderEnable;
    engine.regs.render_enable.address_low=0x20ff8; // 24-byte comparison crosses a 4 KiB GPU page
    cmp=Compare{0,1,0xaaaa5555,0x5555aaaa,5,1};source.WriteBlock(0xff8,&cmp,sizeof(cmp));
    check(Mode::Conditional,false);
    const auto crossing=streamer.WriteCounter(0x1ff8,false,5);
    streamer.SyncWrites();
    check(Mode::Conditional,true);check(Mode::IfEqual,true);check(Mode::IfNotEqual,false);
    assert(source.read<u32>(0xffc)==1 && source.read<u32>(0x1000)==0xaaaa5555 &&
           source.read<u32>(0x1004)==0x5555aaaa);
    static_cast<VideoCommon::StreamerInterface*>(&streamer)->Free(crossing);
    assert(runtime.writes==4 && rasterizer.flushes>0);
    gpu_memory.Unmap(0x20000,source.data.size());
    std::cout<<"QUERY_ACTUAL_CPU_CONSUMER_PASS cases="<<cases
             <<" actual_MemoryManager=True actual_Maxwell3D=True CPU_SyncValues=True GPU=False full_QueryCache=False\n";
} catch(const std::exception& error) {
    std::cerr<<"HARNESS_ERROR "<<error.what()<<"\n";return 1;
}
