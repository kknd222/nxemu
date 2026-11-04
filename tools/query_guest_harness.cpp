// Actual GuestStreamer template + production MaxwellDeviceMemoryManager.
// This is not SamplesStreamer, Vulkan execution, or complete QueryCacheBase.
#include <cassert>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <array>
#include "nxemu-video/video_settings.h"
#include "yuzu_video_core/query_cache/query_cache.h"
VideoSettings videoSettings{};
#include "query_guest_fixture.h"
struct Traits{using RuntimeType=Runtime;};
int main(int argc,char** argv){
    Memory source;Tegra::MaxwellDeviceMemoryManager memory(source);
    auto asid=memory.RegisterProcess(&source);memory.Map(0x1000,0,source.data.size(),asid,false);
    assert(memory.GetPointer<uint8_t>(0x1000)==source.data.data());
    assert(memory.GetPointer<uint8_t>(0x2100)==source.data.data()+0x1100);
    Runtime runtime{memory};VideoCommon::GuestStreamer<Traits> stream(0,runtime);
    auto* api=static_cast<VideoCommon::StreamerInterface*>(&stream);
    source.data.fill(0xa5);
    assert(!stream.HasPendingSync());stream.SyncWrites();assert(runtime.calls==0);
    auto a=stream.WriteCounter(0x1100,false,0x12345678);
    assert(stream.HasPendingSync()&&source.read<uint32_t>(0x100)==0xa5a5a5a5);
    assert(api->GetQuery(a)->value==0x12345678&&VideoCommon::True(api->GetQuery(a)->flags&VideoCommon::QueryFlagBits::IsFinalValueSynced));
    std::cout<<"BASELINE_PENDING_UNWRITTEN_PASS; value=0xa5a5a5a5\n";
    stream.SyncWrites();assert(!stream.HasPendingSync()&&runtime.calls==1&&runtime.writes==1);
    assert(source.read<uint32_t>(0x100)==0x12345678&&source.read<uint32_t>(0x104)==0xa5a5a5a5);
    assert(VideoCommon::True(api->GetQuery(a)->flags&VideoCommon::QueryFlagBits::IsHostSynced));
    stream.SyncWrites();assert(runtime.calls==1);
    auto b=stream.WriteCounter(0x1200,true,0x89abcdef);stream.SyncWrites();
    assert(source.read<uint64_t>(0x200)==0x89abcdef&&source.read<uint64_t>(0x208)==0xa5a5a5a5a5a5a5a5ULL);
    auto c=stream.WriteCounter(0x1300,false,7);api->GetQuery(c)->flags|=VideoCommon::QueryFlagBits::IsRewritten;
    auto d=stream.WriteCounter(0x1400,false,9);api->GetQuery(d)->flags|=VideoCommon::QueryFlagBits::IsInvalidated;
    unsigned calls=runtime.calls;stream.SyncWrites();assert(runtime.calls==calls&&!stream.HasPendingSync());
    assert(source.read<uint32_t>(0x300)==0xa5a5a5a5&&source.read<uint32_t>(0x400)==0xa5a5a5a5);
    api->Free(a);auto e=stream.WriteCounter(0x1500,false,42);assert(e==a);
    assert(VideoCommon::False(api->GetQuery(e)->flags&VideoCommon::QueryFlagBits::IsHostSynced));
    stream.SyncWrites();assert(source.read<uint32_t>(0x500)==42);
    auto z=stream.WriteCounter(0x1600,false,0);stream.SyncWrites();assert(source.read<uint32_t>(0x600)==0);
    auto max=stream.WriteCounter(0x1700,true,0xffffffff);stream.SyncWrites();assert(source.read<uint64_t>(0x700)==0xffffffff);
    auto narrow=stream.WriteCounter(0x1800,false,0);api->GetQuery(narrow)->value=0x11223344aabbccddULL;
    auto wide=stream.WriteCounter(0x1900,true,0);api->GetQuery(wide)->value=0x11223344aabbccddULL;
    stream.SyncWrites();assert(source.read<uint32_t>(0x800)==0xaabbccdd&&source.read<uint32_t>(0x804)==0xa5a5a5a5);
    assert(source.read<uint64_t>(0x900)==0x11223344aabbccddULL&&source.read<uint64_t>(0x908)==0xa5a5a5a5a5a5a5a5ULL);
    if(argc==4){
        for(unsigned i=0;i<3;++i){auto value=static_cast<uint32_t>(std::stoul(argv[i+1]));auto addr=0x1a00+0x100*i;stream.WriteCounter(addr,false,value);stream.SyncWrites();assert(source.read<uint32_t>(addr-0x1000)==value);}
        std::cout<<"IMPORTED_HOST_SAMPLES_GUEST_WRITE_PASS; samples="<<argv[1]<<','<<argv[2]<<','<<argv[3]<<"; SamplesStreamer_executed=False\n";
    }else assert(argc==1);
    (void)z;(void)max;memory.UnregisterProcess(asid);
    std::cout<<"GUEST_STREAMER_DEVICE_MEMORY_PASS; actual_template=True; actual_device_manager=True; SyncValues_adapter=CPU; GPU=False; full_QueryCache=False; timestamp_generated=False\n";
}
