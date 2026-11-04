"""Actual native subset Lower -> production translators/passes -> SPIRV; not a game shader fix."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,sys,traceback
from native_draw_inputs import read_case
import test_native_ldc_is as T
PREFIX=T.CPP.split('std::vector<u64> default_words;')[0]
PREFIX=PREFIX.replace('struct Env final:Environment{','struct Env final:Environment{\n Env(const std::filesystem::path& path){stage=Stage::VertexB;std::ifstream f(path,std::ios::binary);f.read(reinterpret_cast<char*>(&sph),sizeof(sph));assert(f.gcount()==sizeof(sph));}')
CPP=PREFIX+r'''
#include "nxemu-video/video_settings.h"
VideoSettings videoSettings{};
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
int main(int argc,char** argv){
 assert(argc==5);const std::filesystem::path out=argv[3];std::filesystem::create_directories(out);
 std::array<Ampere::Instruction,6> native;std::ifstream input(argv[1],std::ios::binary);input.read(reinterpret_cast<char*>(native.data()),sizeof(native));assert(input.gcount()==sizeof(native));
 const auto lowered=Ampere::LowerToMaxwell(native);
 if(std::string(argv[4])=="baseline"){
  assert(!lowered&&lowered.error_offset==32&&lowered.error=="native indexed constant load mode");
  std::cout<<"{\"passed\":true,\"baseline_IS_rejected\":true,\"error_offset\":32,\"SPIRV_emitted\":false}\n";return 0;
 }
 if(!lowered){std::cerr<<lowered.error<<'\n';assert(false);}assert(lowered.instructions.size()==7);
 Fixture f;Env env(argv[2]);f.program.stage=Stage::VertexB;Maxwell::TranslatorVisitor v(env,*f.block);
 // Dispatch the exact supported lowering expansion of this fixed six-instruction synthetic entry.
 v.ALD(lowered.instructions[0]);v.MOV32I(lowered.instructions[1]);v.ISCADD_reg(lowered.instructions[2]);
 v.LDC(lowered.instructions[3]);v.LDC(lowered.instructions[4]);v.AST(lowered.instructions[5]);
 std::ofstream pre(out/"pre.ir.txt");pre<<DumpProgram(f.program);pre.close();
 Optimization::SsaRewritePass(f.program);Optimization::ConstantPropagationPass(env,f.program);
 Optimization::DeadCodeEliminationPass(f.program);Optimization::IdentityRemovalPass(f.program);
 Optimization::CollectShaderInfoPass(env,f.program);Optimization::VerificationPass(f.program);
 unsigned cbufloads{},positionstores{},vertexloads{};
 for(auto& inst:f.block->Instructions()){
  if(inst.GetOpcode()==Opcode::GetCbufU32x2){assert(!inst.Arg(0).IsImmediate());++cbufloads;}
  if(inst.GetOpcode()==Opcode::SetAttribute){auto attr=inst.Arg(0).Attribute();if(attr>=Attribute::PositionX&&attr<=Attribute::PositionW)++positionstores;}
  if(inst.GetOpcode()==Opcode::GetAttributeU32&&inst.Arg(0).Attribute()==Attribute::VertexId)++vertexloads;
 }
 assert(cbufloads==2&&positionstores==4&&vertexloads==1&&f.program.info.uses_cbuf_indirect);
 assert(f.program.info.constant_buffer_descriptors.size()==14);
 for(const auto& desc:f.program.info.constant_buffer_descriptors)assert(f.program.info.constant_buffer_used_sizes[desc.index]==65536);
 AbstractSyntaxNode block{};block.type=AbstractSyntaxNode::Type::Block;block.data.block=f.block;
 AbstractSyntaxNode ret{};ret.type=AbstractSyntaxNode::Type::Return;f.program.syntax_list={block,ret};
 std::ofstream post(out/"post.ir.txt");post<<DumpProgram(f.program);post.close();
 Profile profile{};profile.supported_spirv=0x00010300;profile.unified_descriptor_binding=true;profile.support_vertex_instance_id=false;
 Backend::Bindings bindings{};const auto spv=Backend::SPIRV::EmitSPIRV(profile,{},f.program,bindings);
 assert(bindings.unified==14&&spv.size()>5&&spv[0]==0x07230203);
 std::ofstream binary(out/"native-ldc-is.vert.spv",std::ios::binary);binary.write(reinterpret_cast<const char*>(spv.data()),spv.size()*4);binary.close();assert(binary);
 std::cout<<"{\"passed\":true,\"actual_Lower_ALD_MOV32I_ISCADD_LDC_AST\":true,\"actual_SSA_constprop_DCE_collectinfo_SPIRV\":true,\"dynamic_cbuf_loads\":2,\"position_stores\":4,\"VertexId_loads\":1,\"UBO_bindings\":14,\"SPIRV_words\":"<<spv.size()<<",\"predicate_execution_tested\":false,\"GPU_executed\":false}\n";
}
'''
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--capture',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--lower-source',type=Path);ap.add_argument('--expect-is-rejection',action='store_true');a=ap.parse_args()
 R=Path(__file__).resolve().parents[1];B=R/'build/windows-ninja';O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
 manifest=json.loads(a.capture.read_text());case=read_case(a.capture,{'run_id':manifest['run_id'],'module_generation':int(manifest['generation'])});assert case['index_input']['index_prefix_values']==[0,1,2]
 native=Path(case['files']['native']['path']).read_bytes();assert len(native)==352
 selected=b''.join(native[n:n+16] for n in [0,0x30,0x40,0x50,0x130,0x150]);minimal=O/'minimal.native.bin';minimal.write_bytes(selected)
 header=Path(case['files']['parsed_header']['path']);lower=(a.lower_source or R/'src/yuzu_shader_recompiler/frontend/ampere/lower_to_maxwell.h').resolve();shutil.copy2(lower,O/'lower_override.h')
 cpp=O/'native-ldc-spv.cpp';cpp.write_text(CPP);obj=O/'objects';obj.mkdir();exe=O/'native-ldc-spv.exe'
 libs=[B/'src/yuzu_shader_recompiler/Release/shader_recompiler.lib',B/'src/common/Release/common.lib',B/'src/yuzu_common/Release/yuzu_common.lib',B/'external/fmt/Release/fmt.lib',B/'external/sirit/src/Release/sirit.lib']
 frozen=O/'frozen';frozen.mkdir();frozen_libs=[]
 for n,p in enumerate(libs):q=frozen/(str(n)+'-'+p.name);shutil.copy2(p,q);frozen_libs.append(q)
 ldc=R/'src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/load_constant.cpp'
 command=['cl','/nologo','/std:c++20','/utf-8','/EHsc','/MT','/O2','/I'+str(O),'/I'+str(R/'src'),'/I'+str(R/'external/boost'),'/I'+str(R/'external/fmt/include'),'/I'+str(B/'src'),str(cpp),str(ldc),'/Fe:'+str(exe),'/Fo:'+str(obj)+'\\',*map(str,frozen_libs),'/link','/OPT:REF','ws2_32.lib','winmm.lib','bcrypt.lib','crypt32.lib','iphlpapi.lib','psapi.lib','ole32.lib','shell32.lib']
 cmd=O/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(command)+'\n')
 paths=[Path(__file__),Path(T.__file__),lower,O/'lower_override.h',cpp,ldc,minimal,header,a.capture,*frozen_libs]
 hashes={str(p.resolve()):sha(p) for p in paths};report=dict(command=subprocess.list2cmdline([sys.executable,'-B',Path(__file__),*sys.argv[1:]]),compiler_command=subprocess.list2cmdline(command),input='six-instruction synthetic entry using captured ALD/LEA/LDC/AST/EXIT bytes; no original SR/CSM block bypass in emulator',hashes=hashes)
 try:
  b=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=150);(O/'build.log').write_text(b.stdout+b.stderr);report.update(stage='compile',literal_stdout=b.stdout,literal_stderr=b.stderr,exit_status=b.returncode);assert b.returncode==0
  mode='baseline' if a.expect_is_rejection else 'modified';r=subprocess.run([str(exe),str(minimal),str(header),str(O),mode],capture_output=True,text=True,timeout=30);report.update(stage='run',literal_stdout=r.stdout,literal_stderr=r.stderr,exit_status=r.returncode);assert r.returncode==0;report['result']=json.loads(r.stdout)
  if not a.expect_is_rejection:
   spv=O/'native-ldc-is.vert.spv';validator=Path(r'C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe');dis=validator.with_name('spirv-dis.exe');assert validator.is_file()
   v=subprocess.run([str(validator),'--target-env','vulkan1.1',str(spv)],capture_output=True,text=True,timeout=15);report['validation']=dict(command=subprocess.list2cmdline([str(validator),'--target-env','vulkan1.1',str(spv)]),literal_stdout=v.stdout,literal_stderr=v.stderr,exit_status=v.returncode);assert v.returncode==0,report['validation']
   q=subprocess.run([str(dis),str(spv),'-o',str(O/'native-ldc-is.vert.spvasm')],capture_output=True,text=True,timeout=15);assert q.returncode==0;report['SPIRV_sha256']=sha(spv)
  assert all(sha(Path(p))==h for p,h in hashes.items());report['inputs_unchanged']=True;report['stage']='complete';(O/'RESULT.json').write_text(json.dumps(report,indent=2));print('NATIVE_LDC_SPIRV_PASS; mode='+mode+'; actual_backend=True; GPU=False')
 except BaseException:
  report['failure']=traceback.format_exc();(O/'FAILURE.json').write_text(json.dumps(report,indent=2));raise
if __name__=='__main__':main()
