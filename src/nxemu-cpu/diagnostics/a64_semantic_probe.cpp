// SPDX-License-Identifier: GPL-2.0-or-later
// Offline, explicitly called CPU semantic probe. Never invoked by game execution.
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include <array>
#include <cstring>
#include <stdexcept>

namespace {
struct Memory final : Dynarmic::A64::UserCallbacks {
    std::array<unsigned char,8192> bytes{};
    Dynarmic::A64::Jit* jit{};
    uint64_t ticks{512}, writes{}, failures{}, fallback{};
    bool fail_once{};
    template<class T> T Read(uint64_t a) {
        if(a<0x1000 || a-0x1000>bytes.size()-sizeof(T))throw std::out_of_range("probe read");
        T v;std::memcpy(&v,bytes.data()+a-0x1000,sizeof(v));return v;
    }
    template<class T> void Write(uint64_t a,T v) {
        if(a<0x1000 || a-0x1000>bytes.size()-sizeof(T))throw std::out_of_range("probe write");
        std::memcpy(bytes.data()+a-0x1000,&v,sizeof(v));
    }
    template<class T> bool Exclusive(uint64_t a,T v,T expected) {
        ++writes;if(fail_once){fail_once=false;++failures;return false;}
        const T old=Read<T>(a);if(old!=expected){++failures;return false;}Write(a,v);return true;
    }
    uint8_t MemoryRead8(uint64_t a) override{return Read<uint8_t>(a);}
    uint16_t MemoryRead16(uint64_t a) override{return Read<uint16_t>(a);}
    uint32_t MemoryRead32(uint64_t a) override{return Read<uint32_t>(a);}
    uint64_t MemoryRead64(uint64_t a) override{return Read<uint64_t>(a);}
    Dynarmic::A64::Vector MemoryRead128(uint64_t a) override{return Read<Dynarmic::A64::Vector>(a);}
    void MemoryWrite8(uint64_t a,uint8_t v) override{Write(a,v);}
    void MemoryWrite16(uint64_t a,uint16_t v) override{Write(a,v);}
    void MemoryWrite32(uint64_t a,uint32_t v) override{Write(a,v);}
    void MemoryWrite64(uint64_t a,uint64_t v) override{Write(a,v);}
    void MemoryWrite128(uint64_t a,Dynarmic::A64::Vector v) override{Write(a,v);}
    bool MemoryWriteExclusive8(uint64_t a,uint8_t v,uint8_t e) override{return Exclusive(a,v,e);}
    bool MemoryWriteExclusive16(uint64_t a,uint16_t v,uint16_t e) override{return Exclusive(a,v,e);}
    bool MemoryWriteExclusive32(uint64_t a,uint32_t v,uint32_t e) override{return Exclusive(a,v,e);}
    bool MemoryWriteExclusive64(uint64_t a,uint64_t v,uint64_t e) override{return Exclusive(a,v,e);}
    bool MemoryWriteExclusive128(uint64_t a,Dynarmic::A64::Vector v,Dynarmic::A64::Vector e) override{return Exclusive(a,v,e);}
    void InterpreterFallback(uint64_t,size_t) override{++fallback;jit->HaltExecution();}
    void CallSVC(uint32_t) override{jit->HaltExecution();}
    void ExceptionRaised(uint64_t,Dynarmic::A64::Exception) override{++fallback;jit->HaltExecution();}
    void AddTicks(uint64_t n) override{ticks=n>=ticks?0:ticks-n;}
    uint64_t GetTicksRemaining() override{return ticks;}
    uint64_t GetCNTPCT() override{return 0;}
};
}

// ABI v1: 31 initial/final registers; 64-bit memory at 0x1800; output
// [memory, PC, PSTATE, exclusive attempts, failures, fallback, ticks_remaining].
// 0=successful SVC termination; 2=unsupported/exception; 3=budget exhausted; 4=host error.
extern "C" __declspec(dllexport) int NxemuA64SemanticProbeV1(
    uint32_t opcode,const uint64_t* initial,uint64_t memory,int fail_once,
    uint64_t* final,uint64_t* output) noexcept {
    if(!initial||!final||!output)return 4;
    try {
        Memory m;m.fail_once=fail_once!=0;m.Write<uint32_t>(0x1000,opcode);
        m.Write<uint32_t>(0x1004,0xd4000001);m.Write<uint64_t>(0x1800,memory);
        Dynarmic::ExclusiveMonitor monitor(1);
        Dynarmic::A64::UserConfig c{};c.callbacks=&m;c.global_monitor=&monitor;c.code_cache_size=16*1024*1024;
        Dynarmic::A64::Jit jit(c);m.jit=&jit;jit.SetPC(0x1000);
        for(size_t i=0;i<31;++i)jit.SetRegister(i,initial[i]);
        jit.Run();auto regs=jit.GetRegisters();std::copy(regs.begin(),regs.end(),final);
        output[0]=m.Read<uint64_t>(0x1800);output[1]=jit.GetPC();output[2]=jit.GetPstate();
        output[3]=m.writes;output[4]=m.failures;output[5]=m.fallback;output[6]=m.ticks;
        return m.fallback?2:!m.ticks?3:0;
    }catch(...){return 4;}
}


#include "yuzu_common/hardware_properties.h"
#include <atomic>
#include <thread>
#include <vector>

// Explicit OFFLINE probe, not a live emulator control or fastmem test.
// Uses the exact linked Dynarmic monitor backend and the CPU factory's shared capacity.
// output: [capacity, independent_commits, parallel_commits, stale_rejections,
//          clear_processor_isolation, parallel_core_count]. No persistent state.
extern "C" __declspec(dllexport) int NxemuSixCoreMonitorProbeV1(uint64_t* output) noexcept {
    if (!output) return 4;
    std::fill_n(output,6,0);
    try {
        const size_t count=Hardware::NUM_CPU_CORES;
        Dynarmic::ExclusiveMonitor monitor(count);
        output[0]=monitor.GetProcessorCount();
        std::vector<uint64_t> words(count,0);
        for(size_t i=0;i<count;++i) {
            const uint64_t address=0x1800+i*8;
            monitor.ReadAndMark<uint64_t>(i,address,[&]{return words[i];});
            if(monitor.DoExclusiveOperation<uint64_t>(i,address,[&](uint64_t old){
                if(words[i]!=old)return false;words[i]=old+1;return true;
            }))++output[1];
        }
        std::atomic<uint64_t> value{0};
        for(size_t i=0;i<count;++i)
            monitor.ReadAndMark<uint64_t>(i,0x2000,[&]{return value.load();});
        if(!monitor.DoExclusiveOperation<uint64_t>(0,0x2000,[&](uint64_t old){
            return value.compare_exchange_strong(old,old+1);
        }))return 2;
        for(size_t i=1;i<count;++i)
            if(!monitor.DoExclusiveOperation<uint64_t>(i,0x2000,[&](uint64_t){return true;}))++output[3];
        monitor.ReadAndMark<uint64_t>(0,0x3000,[]{return uint64_t{3};});
        monitor.ReadAndMark<uint64_t>(count-1,0x3008,[]{return uint64_t{4};});
        monitor.ClearProcessor(count-1);
        output[4]=monitor.DoExclusiveOperation<uint64_t>(0,0x3000,[](uint64_t old){return old==3;}) &&
            !monitor.DoExclusiveOperation<uint64_t>(count-1,0x3008,[](uint64_t){return true;});
        value.store(0);
        std::vector<std::jthread> workers;
        for(size_t i=0;i<count;++i) workers.emplace_back([&,i] {
            for(size_t n=0;n<1000;++n) {
                bool done=false;
                while(!done) {
                    monitor.ReadAndMark<uint64_t>(i,0x2000,[&]{return value.load();});
                    done=monitor.DoExclusiveOperation<uint64_t>(i,0x2000,[&](uint64_t old){
                        return value.compare_exchange_strong(old,old+1);
                    });
                    if(!done)std::this_thread::yield();
                }
            }
        });
        for(auto& w:workers)w.join();
        output[2]=value.load();output[5]=count;
        monitor.Clear();
        return output[0]==count && output[1]==count && output[2]==count*1000 &&
               output[3]==count-1 && output[4] ? 0 : 2;
    } catch (...) { return 4; }
}
