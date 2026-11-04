#pragma once
#include "query_guest_fixture.h"
#include "yuzu_video_core/rasterizer_interface.h"
#include <deque>
struct CacheRasterizer final : VideoCore::RasterizerInterface {
    unsigned flushes{},mapping_notifications{},invalidations{},references{}; std::deque<std::function<void()>> operations; void Drain(){while(!operations.empty()){auto f=std::move(operations.front());operations.pop_front();f();}}
    [[noreturn]] static void unused() { throw std::runtime_error("unexpected rasterizer operation"); }
    void Draw(bool,u32) override {unused();}
    void DrawTexture() override {unused();}
    void Clear(u32) override {unused();}
    void DispatchCompute() override {unused();}
    void ResetCounter(VideoCommon::QueryType) override {unused();}
    void Query(GPUVAddr,VideoCommon::QueryType,VideoCommon::QueryPropertiesFlags,u32,u32) override {unused();}
    void BindGraphicsUniformBuffer(size_t,u32,GPUVAddr,u32) override {unused();}
    void DisableGraphicsUniformBuffer(size_t,u32) override {unused();}
    void SignalFence(std::function<void()>&& f) override {operations.push_back(std::move(f));}
    void SignalMemoryOperationFence(std::function<void()>&&) override {unused();}
    void WaitForFence() override {unused();}
    void SyncOperation(std::function<void()>&& f) override {operations.push_back(std::move(f));}
    void SignalSyncPoint(u32) override {unused();}
    void SignalReference() override {++references;}
    void ReleaseFences(bool) override {Drain();}
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
