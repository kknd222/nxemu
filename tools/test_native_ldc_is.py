"""Actual native Lower -> production LDC translator/Slot -> real SSA/constant propagation.

Synthetic register values and native instructions. No SPIRV, Vulkan, GPU or ROM replay.
The prebuilt production library supplies IR and pass implementations; load_constant.cpp
is compiled explicitly so the addressing function under test is the actual current source.
"""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,sys,traceback

CPP=r'''
#include "lower_override.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate/impl/load_constant.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <filesystem>
using namespace Shader;
using namespace Shader::IR;
namespace L=Shader::Maxwell::LDC;
struct Env final:Environment{
 u64 ReadInstruction(u32)override{throw std::runtime_error("no shader instruction fetch expected");}
 u32 ReadCbufValue(u32,u32)override{throw std::runtime_error("numeric cbuf read is outside this address test");}
 TextureType ReadTextureType(u32)override{throw std::runtime_error("no texture");}
 TexturePixelFormat ReadTexturePixelFormat(u32)override{throw std::runtime_error("no texture");}
 bool IsTexturePixelFormatInteger(u32)override{throw std::runtime_error("no texture");}
 u32 ReadViewportTransformState()override{return 1;}
 u32 TextureBoundBuffer()const override{return 17;}
 u32 LocalMemorySize()const override{return 0;}
 u32 SharedMemorySize()const override{return 0;}
 std::array<u32,3> WorkgroupSize()const override{return {};}
 bool HasHLEMacroState()const override{return false;}
 std::optional<ReplaceConstant> GetReplaceConstBuffer(u32,u32)override{return {};}
 void Dump(u64,u64)override{}
};
struct Fixture {
 ObjectPool<Inst> instructions;ObjectPool<Block> blocks;Program program;Block* block;
 Fixture(){block=blocks.Create(instructions);block->SetOrder(0);program.blocks={block};program.post_order_blocks={block};}
};
void bits(Ampere::Instruction& i,unsigned offset,unsigned count,u32 value){
 for(unsigned b=0;b<count;++b){const u32 mask=1U<<((offset+b)%32);auto& word=i.words[(offset+b)/32];word=(word&~mask)|(((value>>b)&1)?mask:0);}
}
Ampere::Instruction native(unsigned size,unsigned mode,unsigned source,unsigned dest,unsigned bank,int offset,unsigned pred=7){
 Ampere::Instruction i{};bits(i,0,12,0xb82);bits(i,12,4,pred);bits(i,16,8,dest);bits(i,24,8,source);bits(i,38,16,u32(offset));bits(i,54,5,bank);bits(i,73,3,size);bits(i,78,2,mode);return i;
}
Ampere::LoweredProgram lower_one(const Ampere::Instruction& i){
 Ampere::Instruction exit{{0x0000794d,0,0x03800000,0x000fea00}};
 const std::array<Ampere::Instruction,2> program{i,exit};return Ampere::LowerToMaxwell(program);
}
u64 expected_word(const Ampere::Instruction& i,unsigned mode){
 const unsigned bank=i.Bits(54,5)==24?17:i.Bits(54,5);
 return 0xef90000000000000ULL|(u64{i.Bits(12,4)}<<16)|i.Bits(16,8)|(u64{i.Bits(24,8)}<<8)|
        (u64{i.Bits(38,16)}<<20)|(u64{bank}<<36)|(u64{mode}<<44)|(u64{i.Bits(73,3)}<<48);
}
std::vector<u64> default_words;unsigned native_ok{},native_rejected{},downstream_cases{},invalid_cases{},native_false_skipped{};
void address(u64 word,u32 value,unsigned size,unsigned mode,unsigned rawbank,int signed_offset,const std::filesystem::path& out,const std::string& name){
 const L::Encoding enc{word};Fixture f;Env env;IREmitter ir(*f.block);
 // The lowerer reserves R216 for emulated UR0 in uniform-bank programs.
 // Seed it differently: this ordinary LDC source R0 must not be remapped to UR0.
 ir.SetReg(Reg::R216,ir.Imm32(0x20000U));if(enc.src_reg!=Reg::RZ)ir.SetReg(enc.src_reg,ir.Imm32(value));
 Maxwell::TranslatorVisitor visitor(env,*f.block);visitor.LDC(word);
 unsigned get_reg{};for(auto& inst:f.block->Instructions())if(inst.GetOpcode()==Opcode::GetRegister){++get_reg;assert(inst.Arg(0).Reg()==Reg(enc.src_reg));}
 assert(get_reg==1); // Actual emitter creates GetRegister RZ; SSA resolves it to zero.
 Optimization::SsaRewritePass(f.program);Optimization::ConstantPropagationPass(env,f.program);
 const u32 effective=enc.src_reg==Reg::RZ?0U:value;
 const u32 sum=effective+u32(signed_offset);const u32 base=rawbank==24?17:rawbank;
 const u32 expect_index=mode==2?base+(sum>>16):base;const u32 expect_offset=mode==2?sum&0xffffU:sum;
 unsigned loads{},sets{},extractions{};Inst* load_inst{};std::array<Inst*,2> final_dest{};
 for(auto& inst:f.block->Instructions()){
  if(inst.GetOpcode()==Opcode::GetCbufU32||inst.GetOpcode()==Opcode::GetCbufU32x2){
   ++loads;load_inst=&inst;assert(inst.GetOpcode()==(size==4?Opcode::GetCbufU32:Opcode::GetCbufU32x2));
   const auto index=inst.Arg(0).Resolve(),offset=inst.Arg(1).Resolve();assert(index.IsImmediate()&&offset.IsImmediate());
   assert(index.U32()==expect_index&&offset.U32()==expect_offset);
  }
  if(inst.GetOpcode()==Opcode::SetRegister){++sets;for(unsigned lane=0;lane<(size==5?2U:1U);++lane)if(inst.Arg(0).Reg()==enc.dest_reg+lane)final_dest[lane]=&inst;}
  if(inst.GetOpcode()==Opcode::CompositeExtractU32x2)++extractions;
 }
 assert(loads==1);
 // SSA retains SetRegister statements until a later elimination pass; that is
 // not an addressing error. Their arguments are checked through actual cbuf IR.
 assert(sets>0);assert(extractions==(size==5?2U:0U));
 // Actual post-SSA final destination writes must consume this very cbuf load.
 for(unsigned lane=0;lane<(size==5?2U:1U);++lane){
  assert(final_dest[lane]);const Value v=final_dest[lane]->Arg(1).Resolve();assert(!v.IsImmediate());
  if(size==4){assert(v.Inst()==load_inst);}else{const Inst* x=v.Inst();assert(x->GetOpcode()==Opcode::CompositeExtractU32x2);assert(x->Arg(0).Resolve().Inst()==load_inst);assert(x->Arg(1).Resolve().U32()==lane);}
 }
 std::ofstream file(out/(name+".ir.txt"));file<<DumpProgram(f.program);assert(file);++downstream_cases;
}
int main(int argc,char** argv){
 assert(argc==3);const bool baseline=std::string(argv[1])=="baseline";const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
 unsigned id{};
 for(unsigned size:{4U,5U})for(unsigned mode:{0U,2U})for(unsigned bank:{0U,1U,17U})
 for(int offset:{0,8,16,-8,-32768,32767})for(u32 value:{0x10000U,0x10020U,0x10040U,0xfffcU,0xfffffff8U,0U}){
  auto i=native(size,mode,0,6,bank,offset);const auto lower=lower_one(i);
  const bool expected=!baseline||mode==0;
  if(bool(lower)!=expected){std::cerr<<"MISMATCH size="<<size<<" mode="<<mode<<" bank="<<bank<<" offset="<<offset<<" value="<<value<<" opcode="<<std::hex<<i.Bits(0,12)<<" reason="<<lower.error<<"\n";assert(false);}
  const u64 word=expected_word(i,mode);
  if(expected){assert(lower.instructions.size()==2&&lower.instructions[0]==word);++native_ok;}else{assert(lower.error=="native indexed constant load mode"&&lower.error_offset==0);++native_rejected;}
  if(mode==0)default_words.push_back(word);
  // Baseline IS still tests the existing downstream Slot, but NOT a successful native chain.
  address(expected?lower.instructions[0]:word,value,size,mode,bank,offset,out,std::to_string(id++));
 }
 // Source/destination/predicate encoding, including RZ and overlapping source/dest.
 for(unsigned source:{0U,1U,4U,128U,255U})for(unsigned pred:{0U,7U,8U,15U}){
  auto i=native(5,0,source,4,0,8,pred);auto lower=lower_one(i);
  // Native @!PT is statically false: actual whole-program lowering removes LDC.
  // Test this skip policy without invoking a downstream visitor for this instruction.
  const bool skipped=pred==15;assert(lower&&lower.instructions.size()==(skipped?1U:2U));
  const u64 word=expected_word(i,0);
  if(skipped){assert(lower.instructions[0]==0xe30000000007000fULL);++native_false_skipped;continue;}else{assert(lower.instructions[0]==word);++native_ok;}
  const L::Encoding e{word};assert(e.src_reg==Reg(source)&&e.dest_reg==Reg::R4&&e.mode==L::Mode::Default&&e.offset==8);
  const Maxwell::Instruction instruction{word};assert(instruction.Pred()==Maxwell::Predicate(u64{pred}));
  // Direct visitor call does not execute predication; it validates actual address IR only.
  address(word,0x10020U,5,0,0,8,out,std::to_string(id++));
 }
 // Existing default bank24->17 policy remains unchanged; IS may not silently remap it.
 {auto i=native(4,0,0,4,24,16);auto r=lower_one(i);assert(r&&r.instructions[0]==expected_word(i,0));default_words.push_back(r.instructions[0]);++native_ok;}
 auto reject=[&](Ampere::Instruction i){auto r=lower_one(i);assert(!r&&r.error_offset==0);++invalid_cases;};
 for(unsigned mode:{1U,3U})for(unsigned size:{4U,5U})reject(native(size,mode,0,6,0,0));
 reject(native(5,0,0,5,0,0));reject(native(5,2,0,5,0,0));reject(native(5,0,0,254,0,0));reject(native(5,2,0,254,0,0));
 reject(native(3,0,0,4,0,0));reject(native(6,0,0,4,0,0));reject(native(4,0,0,4,18,0));reject(native(4,2,0,4,18,0));reject(native(4,2,0,4,24,0));
 {auto i=native(4,0,0,4,0,0);bits(i,91,1,1);reject(i);i=native(4,2,0,4,0,0);bits(i,91,1,1);reject(i);}
 // Real Slot rejects downstream IL and ISL independently, not just the native guard.
 for(unsigned mode:{1U,3U}){auto i=native(4,mode,0,4,0,0);Fixture f;Env env;Maxwell::TranslatorVisitor visitor(env,*f.block);bool rejected=false;try{visitor.LDC(expected_word(i,mode));}catch(const NotImplementedException&){rejected=true;}assert(rejected);++invalid_cases;}
 std::ofstream words(out/"default-words.txt");for(u64 word:default_words)words<<std::hex<<word<<'\n';assert(words);
 std::cout<<"{\"passed\":true,\"baseline\":"<<(baseline?"true":"false")<<",\"actual_native_lower\":true,\"actual_LDC_translator\":true,\"actual_Slot\":true,\"actual_SSA_constprop\":true,\"native_ok\":"<<native_ok<<",\"native_IS_rejected\":"<<native_rejected<<",\"native_false_skipped\":"<<native_false_skipped<<",\"IR_address_cases\":"<<downstream_cases<<",\"invalid_cases\":"<<invalid_cases<<",\"default_words\":"<<default_words.size()<<",\"actual_destination_consumers_tested\":true,\"predicate_execution_tested\":false,\"GPU_executed\":false}\n";
}
'''

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--source-root',type=Path);ap.add_argument('--library',type=Path);ap.add_argument('--lower-source',type=Path);ap.add_argument('--mode',choices=['baseline','modified'],default='modified');a=ap.parse_args()
 R=(a.source_root or Path(__file__).resolve().parents[1]).resolve();B=R/'build/windows-ninja';O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
 source=(a.lower_source or R/'src/yuzu_shader_recompiler/frontend/ampere/lower_to_maxwell.h').resolve(strict=True);override=O/'lower_override.h';shutil.copy2(source,override)
 lib=(a.library or B/'src/yuzu_shader_recompiler/Release/shader_recompiler.lib').resolve(strict=True)
 ldc=R/'src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/load_constant.cpp';cpp=O/'ldc-is-test.cpp';cpp.write_text(CPP,encoding='utf-8');exe=O/'ldc-is-test.exe';objects=O/'objects';objects.mkdir()
 libs=[lib,B/'src/common/Release/common.lib',B/'src/yuzu_common/Release/yuzu_common.lib',B/'external/fmt/Release/fmt.lib']
 command=['cl','/nologo','/std:c++20','/utf-8','/EHsc','/MT','/O2','/I'+str(O),'/I'+str(R/'src'),'/I'+str(R/'external/boost'),'/I'+str(R/'external/fmt/include'),'/I'+str(B/'src'),str(cpp),str(ldc),'/Fe:'+str(exe),'/Fo:'+str(objects)+'\\',*[str(p.resolve(strict=True)) for p in libs],'/link','/OPT:REF','ws2_32.lib','winmm.lib','bcrypt.lib','crypt32.lib','iphlpapi.lib','psapi.lib','ole32.lib','shell32.lib']
 cmd=O/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(command)+'\n')
 bound=[source,override,ldc,cpp,*libs,R/'src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/load_constant.h',R/'src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/impl.h',R/'src/yuzu_shader_recompiler/frontend/ir/ir_emitter.h']
 hashes={str(p):sha(p) for p in bound};report=dict(command=subprocess.list2cmdline([sys.executable,'-B',Path(__file__),*sys.argv[1:]]),compiler_command=subprocess.list2cmdline(command),input='native16-byte LDC32/64 plus actual16-byte EXIT for whole-program guard; synthetic registers; actual Lower, LDC/Slot source, production IR/pass library',hashes=hashes,mode=a.mode)
 try:
  b=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=120);(O/'build.log').write_text(b.stdout+b.stderr,encoding='utf-8');report.update(stage='compile',literal_stdout=b.stdout,literal_stderr=b.stderr,exit_status=b.returncode);assert b.returncode==0
  native=[str(exe),a.mode,str(O/'IR')];r=subprocess.run(native,capture_output=True,text=True,timeout=90);(O/'run.log').write_text(r.stdout+r.stderr,encoding='utf-8');report.update(stage='run',native_command=subprocess.list2cmdline(native),literal_stdout=r.stdout,literal_stderr=r.stderr,exit_status=r.returncode);assert r.returncode==0
  result=json.loads(r.stdout);assert result['passed'];assert all(sha(Path(f))==h for f,h in hashes.items()),'Input changed during test'
  report.update(stage='complete',result=result,inputs_unchanged=True,default_words_sha256=sha(O/'IR/default-words.txt'),executable_sha256=sha(exe));(O/'RESULT.json').write_text(json.dumps(report,indent=2));print('NATIVE_LDC_IS_PASS; mode='+a.mode+'; actual_Lower_LDC_Slot_SSA_ConstProp=True; GPU=False')
 except BaseException:
  report['failure']=traceback.format_exc();(O/'FAILURE.json').write_text(json.dumps(report,indent=2));raise
if __name__=='__main__':main()
