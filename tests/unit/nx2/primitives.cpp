// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdio>
#include <vector>
#if __has_include("yuzu_video_core/macro_turing.h")
#include "yuzu_video_core/macro_turing.h"
#include "yuzu_video_core/native_qmd.h"
#include "yuzu_video_core/native_gob.h"
#include "yuzu_shader_recompiler/frontend/ampere/program_capture.h"
#include "yuzu_shader_recompiler/frontend/ampere/program_address_map.h"
using Shader::Ampere::Instruction;
static int checks, failures;
static void Check(bool ok, const char* name) { ++checks; if(!ok){++failures;std::printf("FAIL %s\n",name);} }
static std::array<u32,3> Mme(bool end, u32 method=0, u32 value=0, bool load=false) {
    std::array<u32,3> bits{};
    const auto field=[&](u32 pos,u32 n,u32 v){for(u32 i=0;i<n;++i) if(v&(1U<<i)) bits[(pos+i)/32]|=1U<<((pos+i)%32);};
    field(0,1,end); field(15,5,24);field(20,5,24);field(25,5,24);
    field(51,5,24);field(56,5,24);field(61,5,24);
    if(method){field(30,16,method);field(66,16,value);field(82,3,5);field(85,4,load?3:6);}
    return {bits[2],bits[1],bits[0]};
}
int main(){
    std::array<u8,1024> original{}, native{}, restored{};
    for(size_t i=0;i<original.size();++i) original[i]=static_cast<u8>((i/32)*7+i%32);
    Check(Tegra::Texture::NormalizeNativeGobs(native,original),"native GOB conversion");
    restored=original;
    Check(Tegra::Texture::NormalizeNativeGobsInPlace(restored)&&restored==native,"native GOB in-place matches reference");
    Check(Tegra::Texture::NormalizeNativeGobsInPlace(restored)&&restored==original,"native GOB readback restores layout");
    Check(!Tegra::Texture::NormalizeNativeGobsInPlace(std::span<u8>{restored}.first(511)),"native GOB size rejection");
    Tegra::TuringMacroInterpreter mme;
    std::vector<u32> code;
    for(auto ins:{Mme(true,0x123,0x456),Mme(false)})code.insert(code.end(),ins.begin(),ins.end());
    std::vector<u32> fifo;u32 emitted=0;
    auto result=mme.Execute(code,0,fifo,[](u32){return 0U;},[&](u32 a,u32 b){Check(a==0x123&&b==0x456,"MME output");++emitted;return true;});
    Check(result.success&&emitted==1,"MME two-instruction termination");
    code.clear();for(auto ins:{Mme(true,0x125,0,true),Mme(false)})code.insert(code.end(),ins.begin(),ins.end());
    result=mme.Execute(code,0,fifo,[](u32){return 0U;},[](u32,u32){return true;});
    Check(!result.success&&result.error=="parameter FIFO underflow","MME FIFO underflow");
    fifo={0xfeed};result=mme.Execute(code,0,fifo,[](u32){return 0U;},[](u32 a,u32 b){return a==0x125&&b==0xfeed;});
    Check(result.success&&result.consumed==1,"MME FIFO input");
    result=mme.Execute(code,100,fifo,[](u32){return 0U;},[](u32,u32){return true;});
    Check(!result.success&&result.error=="instruction outside uploaded RAM","MME bad entry");
    result=mme.Execute(code,0,fifo,[](u32){return 0U;},[](u32,u32){return false;});
    Check(!result.success&&result.error=="unsupported emitted method","MME backend rejection");
    for(u32 link:{0U,55U}){
      std::array<Instruction,4> native{{{{0x00007802U|(link<<16),0x20,0x00000f00,0}},{{0x00007944,0x10,0x03c00000,0}},{{0x0000794d,0,0x03800000,0}},{{0x00007950U|(link<<24),0xffffffc0,0x03c3ffff,0}}}};
      auto lower=Shader::Ampere::LowerToMaxwell(native);
      Check(bool(lower)&&lower.source_size==sizeof(native)&&lower.instructions.back()==0xe32000000007000fULL,"Ampere RET preserves link");
    }
    std::array<Instruction,5> clobber{{{{0x00007802,0x20,0x00000f00,0}},{{0x00007944,0x10,0x03c00000,0}},{{0x0000794d,0,0x03800000,0}},{{0x00007802,0,0x00000f00,0}},{{0x00007950,0xffffffb0,0x03c3ffff,0}}}};
    auto rejected=Shader::Ampere::LowerToMaxwell(clobber);
    Check(!bool(rejected)&&rejected.error=="native callee overwrites link register"&&rejected.error_offset==0x30,"Ampere clobber rejection");
    std::array<Instruction,1> invalid{{{{0,0,0,0}}}};
    Check(!bool(Shader::Ampere::LowerToMaxwell(invalid)),"Ampere unsupported opcode");
    Shader::Ampere::ProgramCaptureState capture;
    Check(capture.Observe({{0x7947,0xffffffe1,0x3ffff,0}},0)==Shader::Ampere::ProgramCaptureState::Result::InvalidTarget,"Ampere branch alignment");
    std::vector<u64> mapped(30,0);std::array<u32,2> offsets{0,3};
    Shader::Ampere::AppendNativeAddressMap(mapped,offsets);
    Check(Shader::Ampere::NativeProgramAddress(mapped,0)==0x50,"native address map first");
    Check(Shader::Ampere::NativeProgramAddress(mapped,16)==0x70,"native address map relocated");
    Check(!Shader::Ampere::NativeProgramAddress(mapped,1)&&!Shader::Ampere::NativeProgramAddress(mapped,32),"native address map bounds");
    Tegra::NativeQmdScheduler scheduler;Tegra::NativeQmdScheduler::Descriptor qmd{};qmd[18]=0x30;
    qmd[24]=0x2000;qmd[25]=(1U<<23)|(1U<<30);qmd[26]=0xabc;
    u32 releases=0;
    auto read=[&](u64,Tegra::NativeQmdScheduler::Descriptor& out){out=qmd;return true;};
    auto release=[&](u64 a,u32 b){++releases;return a==0x2000&&b==0xabc;};
    Check(scheduler.Signal(0x1000,3,read,release).empty()&&releases==1,"QMD release payload");
    qmd[4]=1U<<14;
    Check(scheduler.Signal(0x1100,3,read,release).empty()&&releases==1,"QMD dependency wait");
    Check(scheduler.Signal(0x1100,7,read,release).empty()&&releases==2,"QMD dependency completion");
    qmd[4]=0;qmd[12]=qmd[13]=qmd[14]=1;
    Check(scheduler.Signal(0x1200,3,read,release)=="native shader dispatch required"&&releases==2,"QMD missing backend rejects dispatch");
    Check(scheduler.Signal(0x1250,3,read,release,[](const auto&){return false;})=="native shader dispatch required"&&releases==2,"QMD rejected backend does not release");
    qmd[18]=0;
    Check(scheduler.Signal(0x1300,3,read,release)=="unsupported QMD version or queue","QMD version validation");
    std::printf("NX2_PRIMITIVES checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
#else
int main(){std::puts("NX2_PRIMITIVES unavailable=1");return 1;}
#endif
