#pragma once
#include <array>
#include <cassert>
#include <cstring>
#include <span>
#include <stdexcept>
#include "yuzu_video_core/query_cache/query_cache.h"
struct Memory final : IDeviceMemory, IMemory {
    alignas(4096) std::array<uint8_t,16384> data{};
    const uint8_t* BackingBasePointer() const override {return data.data();}
    uint64_t BackingSize() const override {return data.size();}
    uint8_t* VirtualBasePointer() override {return data.data();}
    void EnableDirectMappedAddress() override {throw std::runtime_error("direct mapping not used");}
    bool IsValidVirtualAddressRange(uint64_t a,uint64_t n) const override {return a<=data.size()&&n<=data.size()-a;}
    void RasterizerMarkRegionCached(uint64_t,uint64_t,bool) override {}
    uint8_t* GetPointerSilent(uint64_t a) override {return a<data.size()?data.data()+a:nullptr;}
    bool InvalidateNCE(uint64_t,uint64_t) override {return false;}
    bool NceAccessAllowed(uint64_t,bool) override {return false;}
    bool NceTrackWrites(uint64_t) override {return false;}
    bool NcePrepareCanonicalWrite(uint64_t) override {return false;}
    void RasterizerMarkRegionReadCached(uint64_t,uint64_t,int32_t) override {}
    bool NceTrackReads(uint64_t) override {return false;}
    void RasterizerSetNativeReadAccess(uint64_t,uint64_t,bool) override {}
    template<class T>T read(uint64_t a){assert(IsValidVirtualAddressRange(a,sizeof(T)));T v;std::memcpy(&v,data.data()+a,sizeof(T));return v;}
    template<class T>void write(uint64_t a,T v){assert(IsValidVirtualAddressRange(a,sizeof(T)));std::memcpy(data.data()+a,&v,sizeof(T));}
    bool ReadBlock(uint64_t a,void* p,uint64_t n) override {if(!IsValidVirtualAddressRange(a,n))return false;std::memcpy(p,data.data()+a,n);return true;}
    bool WriteBlock(uint64_t a,const void* p,uint64_t n) override {if(!IsValidVirtualAddressRange(a,n))return false;std::memcpy(data.data()+a,p,n);return true;}
#define RW(N,T) T Read##N(uint64_t a)override{return read<T>(a);} void Write##N(uint64_t a,T v)override{write<T>(a,v);} bool WriteExclusive##N(uint64_t a,T v,T e)override{if(read<T>(a)!=e)return false;write<T>(a,v);return true;}
    RW(8,uint8_t) RW(16,uint16_t) RW(32,uint32_t) RW(64,uint64_t)
#undef RW
    bool WriteExclusive128(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)override{throw std::runtime_error("exclusive128 unused");}
};
struct Runtime {
    Tegra::MaxwellDeviceMemoryManager& memory;
    unsigned calls{},writes{};
    template<class T>void SyncValues(std::span<T> values){
        ++calls;
        for(auto& v:values){assert(v.size==4||v.size==8);auto* ptr=memory.GetPointer<uint8_t>(v.address);assert(ptr);std::memcpy(ptr,&v.value,v.size);++writes;}
    }
    // SyncValues adapter is deliberately CPU controlled, not Vulkan Runtime.
    template<class T>void SyncValues(std::vector<T>& values){SyncValues<T>(std::span<T>(values));}
};
