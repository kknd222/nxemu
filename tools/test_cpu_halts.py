"""Compile actual halt conversion source; capture assertion/log sinks, not emulate an ISA."""
from pathlib import Path
import argparse, hashlib, json, subprocess, sys

CPP = r"""
#include <atomic>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "arm_dynarmic.h"
std::atomic<unsigned> assertions{}, contexts{};
std::mutex log_mutex;
std::vector<std::string> logs;
bool trace_disabled{};
void HaltTestAssert(){++assertions;}
void HaltTestLog(std::string s){std::lock_guard lock(log_mutex);logs.push_back(std::move(s));}
struct FakeCore final:ICpuCore{
 bool throw_context{};
 void Initialize()override{}
 ProcessorArchitecture GetArchitecture()const override{return ProcessorArchitecture::AArch64;}
 uint32_t GetSvcNumber()const override{return 0;}
 void GetContext(CpuThreadContext& c)const override{++contexts;if(throw_context)throw std::runtime_error("probe context failure");c={};c.pc=0x12345678;c.sp=0xabcdef00;c.lr=0x11223344;c.pstate=0x60000000;for(unsigned i=0;i<8;++i)c.r[i]=100+i;}
 void SetContext(const CpuThreadContext&)override{}
 void GetSvcArguments(uint64_t (&)[8])const override{}
 void SetSvcArguments(const uint64_t (&)[8])override{}
 void SetTpidrroEl0(uint64_t)override{}
 CpuHaltReason RunThread(IKernelThread*)override{return CpuHaltReason::None;}
 CpuHaltReason StepThread(IKernelThread*)override{return CpuHaltReason::None;}
 void LockThread(IKernelThread*)override{}
 void UnlockThread(IKernelThread*)override{}
 void ClearInstructionCache()override{}
 void InvalidateCacheRange(uint64_t,uint64_t)override{}
 const CpuDebugWatchpoint* HaltedWatchpoint()const override{return nullptr;}
 void RewindBreakpointInstruction()override{}
 void SetWatchpointArray(const CpuDebugWatchpoint*,uint32_t)override{}
 void SignalInterrupt(IKernelThread*)override{}
 void Release()override{}
};
int main(int argc,char** argv){
 FakeCore core;
 auto map=[&](uint32_t mask){
#if HALT_CONTEXT
   return static_cast<unsigned>(TranslateHaltReason(static_cast<Dynarmic::HaltReason>(mask),&core,2,"unit_run"));
#else
   return static_cast<unsigned>(TranslateHaltReason(static_cast<Dynarmic::HaltReason>(mask)));
#endif
 };
 std::vector<unsigned> values,forward;
 const bool stress=argc>1&&std::string(argv[1])=="stress";
 if(stress){
   std::vector<std::thread> workers;
   for(unsigned n=0;n<8;++n)workers.emplace_back([&]{for(unsigned i=0;i<1024;++i)assert(map(0x02000002)==static_cast<unsigned>(CpuHaltReason::StepThread));});
   for(auto& t:workers)t.join();
 }else{
#if HALT_CONTEXT
   trace_disabled=true;assert(map(3)==static_cast<unsigned>(CpuHaltReason::StepThread));assert(contexts==0&&logs.empty());trace_disabled=false;
   core.throw_context=true;assert(map(3)==static_cast<unsigned>(CpuHaltReason::StepThread));core.throw_context=false;
#endif
   for(unsigned i=0;i<=10;++i)forward.push_back(static_cast<unsigned>(TranslateDynarmicHaltReason(static_cast<CpuHaltReason>(i==10?31:i))));
   const unsigned bit[]={0,1,2,24,25,26,27,28,29,30,31};
   for(unsigned n=0;n<2048;++n){uint32_t mask=0;for(unsigned b=0;b<11;++b)if(n&(1u<<b))mask|=1u<<bit[b];values.push_back(map(mask));}
   for(uint32_t mask:{8u,0x10001u,0x12345678u,0xffffffffu})values.push_back(map(mask));
 }
#if HALT_CONTEXT
 assert(logs.size()==64);
 if(stress)assert(contexts==64);
#else
 assert(logs.empty()&&contexts==0);
#endif
 std::cout<<"{\"actual_mapping_source_compiled\":true,\"ISA_emulated\":false,\"assert_log_sinks_intercepted\":true,\"synthetic_context\":true,\"stress\":"<<(stress?"true":"false")<<",\"assertions\":"<<assertions<<",\"context_reads\":"<<contexts<<",\"mapping\":[";
 for(unsigned i=0;i<values.size();++i){if(i)std::cout<<',';std::cout<<values[i];}
 std::cout<<"],\"forward\":[";
 for(unsigned i=0;i<forward.size();++i){if(i)std::cout<<',';std::cout<<forward[i];}
 std::cout<<"],\"logs\":[";
 for(unsigned i=0;i<logs.size();++i){if(i)std::cout<<',';std::cout<<std::quoted(logs[i]);}
 std::cout<<"]}\n";
}
"""

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source-root", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--modified", action="store_true")
    a=ap.parse_args()
    p=Path(__file__).resolve().parents[2]
    root=a.source_root.resolve()
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    shadow=out/"sink-headers"
    for name,body in {
        "yuzu_common/yuzu_assert.h":"#pragma once\nvoid HaltTestAssert();\n#define UNIMPLEMENTED() HaltTestAssert()\n",
        "yuzu_common/logging/log.h":'#pragma once\n#include <fmt/format.h>\n#include <string>\nvoid HaltTestLog(std::string);\n#define LOG_CRITICAL(category, ...) HaltTestLog(fmt::format(__VA_ARGS__))\n',
        "yuzu_common/debug_document.h":'#pragma once\n#include <string>\nextern bool trace_disabled;\nnamespace Common::DebugDocument {inline std::string Environment(const char* n){std::string s(n);if(s=="NXEMU_CPU_HALT_TRACE")return trace_disabled?"0":"1";if(s=="NXEMU_DEBUG_RUN_ID")return "unit";if(s=="NXEMU_DEBUG_GENERATION")return "7";return "";}inline std::string Quote(const std::string& s){return "\\\""+s+"\\\"";}inline unsigned long long EpochMs(){return 42;}}\n',
    }.items():
        q=shadow/name;q.parent.mkdir(parents=True,exist_ok=True);q.write_text(body)
    cpp=out/"halt-test.cpp";cpp.write_text(CPP)
    source=root/"repo/src/nxemu-cpu/arm_dynarmic.cpp"
    header=root/"repo/src/nxemu-cpu/arm_dynarmic.h"
    exe=out/"halt-test.exe";objects=out/"objects";objects.mkdir(exist_ok=True)
    args=["cl","/nologo","/std:c++20","/EHsc","/MT","/O2","/utf-8",f"/DHALT_CONTEXT={int(a.modified)}",
          "/I"+str(shadow),"/I"+str(header.parent),"/I"+str(p/"repo/src"),"/I"+str(p/"repo/src/nxemu-cpu"),
          "/I"+str(p/"repo/external/fmt/include"),str(cpp),str(source),"/Fe:"+str(exe),
          "/Fo:"+str(objects)+"\\",str(p/"repo/build/windows-ninja/external/fmt/Release/fmt.lib")]
    cmd=out/"build.cmd";cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'+subprocess.list2cmdline(args)+"\n")
    hashes={str(q):hashlib.sha256(q.read_bytes()).hexdigest() for q in [source,header,cpp,p/"repo/src/nxemu-cpu/dynarmic/interface/halt_reason.h",p/"repo/src/nxemu-module-spec/cpu.h"]}
    r=subprocess.run(["cmd","/c",str(cmd)],capture_output=True,text=True)
    (out/"build.log").write_text(r.stdout+r.stderr)
    assert r.returncode==0,r.stdout+r.stderr
    results=[]
    for mode in ["unit","stress"]:
        r=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=30)
        assert r.returncode==0,r.stdout+r.stderr
        results.append(dict(mode=mode,command=subprocess.list2cmdline([str(exe),mode]),input="real enum/source conversion; synthetic masks/context, intercepted sinks",literal_stdout=r.stdout,literal_stderr=r.stderr,exit_status=r.returncode,result=json.loads(r.stdout)))
    parser_verification=None
    if a.modified:
        from debug_guest_faults import parse_text, guest_faults, LIMIT
        unit=parse_text("\n".join(results[0]['result']['logs']))
        stress=parse_text("\n".join(results[1]['result']['logs']))
        assert len(unit['cpu_halts'])==len(stress['cpu_halts'])==64 and unit['cpu_halt_malformed']==0
        assert len(set(e['ordinal'] for e in stress['cpu_halts']))==64
        assert all(e['metadata_consistent'] for e in unit['cpu_halts'])
        assert any(e['context_valid'] and e['pc']==0x12345678 and e['gpr_x0_x7']==list(range(100,108)) for e in unit['cpu_halts'])
        assert any(not e['context_valid'] and e['pc'] is None for e in unit['cpu_halts'])
        assert parse_text('NXEMU_CPU_HALT_MAP schema=1 incomplete=1')['cpu_halt_malformed']==1
        sample=results[0]['result']['logs'][3].encode();cut=len(sample)//2;half=LIMIT//2
        gap_file=out/'noncontiguous-log-test.txt'
        gap_file.write_bytes(b'A'*(half-cut-1)+b'\n'+sample[:cut]+b'G'*(1024*1024)+sample[cut:]+b'\n'+b'B'*(half-len(sample[cut:])-1))
        gap=guest_faults(gap_file);assert gap['bytes_omitted']==1024*1024 and gap['cpu_halts']==[]
        parser_verification=dict(rows64=True, stress_unique_ordinals=True, raw_bits_consistent=True, typed_context=True, missing_context_explicit=True, malformed_count=True, noncontiguous_slices_not_stitched=True)
    assert all(hashlib.sha256(Path(q).read_bytes()).hexdigest()==h for q,h in hashes.items())
    (out/"RESULT.json").write_text(json.dumps(dict(command=subprocess.list2cmdline([sys.executable,"-B",str(Path(__file__).resolve()),*sys.argv[1:]]),hashes=hashes,results=results,parser_verification=parser_verification),indent=2))
    print(f"HALT_TEST_PASS; actual_mapping_source=True; masks2052=True; forward11=True; stress8192=True; modified={a.modified}; ISA_emulated=False")

if __name__=="__main__":
    main()
