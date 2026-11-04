"""Actual native LEA low-word carry regression: CFG/SSA/SPIRV/Vulkan and IADD3.X consumer.
Uses the permanent local-spill harness infrastructure; no emulator/game input.
"""
import test_native_local_spill as support
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, sys, traceback

EXTRA = r'''
Ampere::Instruction lea(bool uniform,unsigned shift,unsigned carry,unsigned pred=7){
 Ampere::Instruction i{{0x04047291,0x00000006,0x0f80103f,0x001fc800}};
 bits(i,0,12,uniform?0x291:0x211);bits(i,64,8,uniform?63:255);bits(i,75,5,shift);bits(i,81,3,carry);bits(i,12,4,pred);return i;
}
Ampere::Instruction consumer(bool uniform,unsigned carry,unsigned pred=7){
 Ampere::Instruction i{};bits(i,0,12,uniform?0x290:0x210);bits(i,12,4,pred);bits(i,16,8,8);bits(i,24,8,9);bits(i,32,8,10);bits(i,64,8,uniform?63:255);bits(i,74,1,1);bits(i,77,4,15);bits(i,81,3,7);bits(i,84,3,7);bits(i,87,3,carry);return i;
}
u64 mov(unsigned r,u32 value){return 0x010000000007f000ULL|r|(u64{value}<<20);}
u64 store(unsigned r,unsigned word){return 0xef54000000070000ULL|r|(u64{255}<<8)|(u64{word*4}<<20);}
std::vector<u32> exercise(bool uniform,unsigned shift,unsigned cp,u32 a,u32 b,u32 pseed,u32 upseed,unsigned pred,bool carry_enabled,const std::filesystem::path& out,const std::string& name,std::vector<u32>& expected){
 const std::array<Ampere::Instruction,3> native{lea(uniform,shift,carry_enabled?cp:7,pred),consumer(uniform,cp,pred),exit_insn};
 auto low=carry_enabled?Ampere::LowerToMaxwell(native):one(native[0]);assert(low);
 const unsigned ra=uniform?220:4,rb=uniform?222:6,rd=ra,r9=uniform?225:9,r10=uniform?226:10,r8=uniform?224:8;
 std::vector<u64> setup{mov(ra,a),mov(rb,b),mov(r9,0x11223344),mov(r10,0x55667788),mov(r8,0xcafebabe),mov(239,upseed),mov(252,pseed),0x38f0000000070000ULL|(u64{252}<<8)|(u64{127}<<20)};
 low.instructions.insert(low.instructions.begin(),setup.begin(),setup.end());
 const std::array<u64,6> output{store(rd,0),store(r8,1),0x38e8000000070000ULL|252|(u64{255}<<8)|(u64{127}<<20),store(252,2),store(239,3),store(rb,4)};
 low.instructions.insert(low.instructions.end()-1,output.begin(),output.end());
 const bool execute=pred==7 || (bool((pseed>>(pred&7))&1)!=(bool(pred&8)));
 const u32 sum=u32(u32(a<<shift)+b),carry=sum<b;const u32 mask=1U<<(uniform?6-cp:cp);
 expected={execute?sum:a,carry_enabled&&execute?u32(0x11223344U+0x55667788U+carry):0xcafebabeU,
 uniform||!carry_enabled||!execute?pseed:(pseed&~mask)|(carry?mask:0),uniform&&carry_enabled?(upseed&~mask)|(carry?mask:0):upseed,b};
 return cfg_probe(low,out,name);
}

Ampere::Instruction mixed(unsigned shift=6,unsigned src=4,bool negate=false,unsigned rd=6,unsigned ra=4,unsigned pred=7){
 Ampere::Instruction i{{0x04067c11,0x00000004,0x0f8e30ff,0x020fc800}};
 bits(i,75,5,shift);bits(i,32,8,src);bits(i,63,1,negate);bits(i,16,8,rd);bits(i,24,8,ra);bits(i,12,4,pred);return i;
}
unsigned ur(unsigned n){return n<22?216+n:218+n;}
std::vector<u32> mixed_exercise(unsigned shift,unsigned src,bool negate,u32 a,u32 b,unsigned rd,unsigned ra,unsigned pred,u32 ps,const std::filesystem::path& out,const std::string& name,std::vector<u32>& expected){
 auto low=one(mixed(shift,src,negate,rd,ra,pred));assert(low);const u32 ups=0xdeadbea5,guard=0xa5b6c7d8;
 std::vector<u64> setup{mov(215,guard),mov(4,0x24681357)};std::vector<u32> bank;
 for(unsigned n=0;n<28;++n){u32 value=0x13579bdfU+n*0x1020304U;if(n==src)value=b;bank.push_back(value);setup.push_back(mov(ur(n),value));}
 if(ra!=255)setup.push_back(mov(ra,a));if(rd!=ra&&rd!=255)setup.push_back(mov(rd,0xcafebabe));
 setup.push_back(mov(239,ups));setup.push_back(mov(252,ps));setup.push_back(0x38f0000000070000ULL|(u64{252}<<8)|(u64{127}<<20));
 low.instructions.insert(low.instructions.begin(),setup.begin(),setup.end());
 std::vector<u64> output{store(rd,0),store(4,1),0x38e8000000070000ULL|252|(u64{255}<<8)|(u64{127}<<20),store(252,2),store(239,3)};
 for(unsigned n=0;n<28;++n)output.push_back(store(ur(n),4+n));output.push_back(store(ra,32));output.push_back(store(215,33));
 low.instructions.insert(low.instructions.end()-1,output.begin(),output.end());
 const bool execute=pred==7||(bool((ps>>(pred&7))&1)!=bool(pred&8));const u32 av=ra==255?0:a,bv=src==63?0:b,sum=u32(u32(av<<shift)+(negate?u32(0-bv):bv));
 const u32 initial=rd==ra?av:0xcafebabe,result=rd==255?0:execute?sum:initial;
 expected={result,rd==4?result:ra==4?a:0x24681357,ps,ups};expected.insert(expected.end(),bank.begin(),bank.end());expected.push_back(rd==ra?result:av);expected.push_back(rd==215?result:ra==215?a:guard);
 return cfg_probe_uniform(low,out,name);
}
int main(int argc,char**argv)try{
 assert(argc==3||argc==4);const std::string uniform_mode=argc==4?argv[3]:"none";const bool baseline=std::string(argv[1])=="baseline";const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
 const auto captured=lea(true,2,0);assert(captured.words[0]==0x04047291&&captured.words[1]==6&&captured.words[2]==0x0f80103f&&captured.words[3]==0x001fc800);
 assert(bool(one(captured))==!baseline);std::cout<<"CAPTURED_LOW_CARRY "<<(baseline?"rejected":"accepted")<<'\n';
 auto reject=[&](Ampere::Instruction i){assert(!one(i));++negative_cases;};
 for(unsigned flag:{72U,74U,80U,62U,63U}){auto i=lea(false,2,0);bits(i,flag,1,1);reject(i);}
 for(unsigned cp:{2U,3U,6U})reject(lea(true,2,cp));
 for(unsigned pr:{0U,8U})reject(lea(true,2,0,pr));
 {auto i=lea(true,2,0);bits(i,0,12,0x891);reject(i);}
 const bool uniform_check=uniform_mode!="none",uniform_baseline=uniform_mode=="baseline";
 unsigned uniform_gpu_cases{},uniform_negative_cases{},uniform_accept_cases{};
 if(uniform_check){const auto cap=mixed();assert(cap.words[0]==0x04067c11&&cap.words[1]==4&&cap.words[2]==0x0f8e30ff&&cap.words[3]==0x020fc800);assert(bool(one(cap))==!uniform_baseline);std::cout<<"CAPTURED_UNIFORM_SOURCE "<<(uniform_baseline?"rejected":"accepted")<<'\n';
 auto bad=[&](Ampere::Instruction i){assert(!one(i));++uniform_negative_cases;};
 for(unsigned flag:{72U,74U,80U,62U}){auto i=mixed();bits(i,flag,1,1);bad(i);}
 for(unsigned cp:{0U,1U,6U}){auto i=mixed();bits(i,81,3,cp);bad(i);}
 for(unsigned reg:{28U,32U,62U,64U,255U})bad(mixed(6,reg));
 for(unsigned slot:{16U,24U,64U})for(unsigned reg:{216U,239U,245U,246U,254U}){auto i=mixed();bits(i,slot,8,reg);bad(i);}
 {auto i=mixed();bits(i,64,8,4);bad(i);}
 if(!uniform_baseline)for(unsigned rd:{0U,215U,255U})for(unsigned ra:{0U,215U,255U}){assert(one(mixed(6,4,false,rd,ra)));++uniform_accept_cases;}
 }
 unsigned id{};{
 Gpu gpu;std::cout<<"GPU_ADAPTER "<<gpu.name<<'\n';
 for(bool uniform:{false,true})for(unsigned shift:{0U,2U,31U})for(auto pair:std::array<std::array<u32,2>,5>{{{0,0},{0xffffffff,1},{0x40000000,0xffffffff},{1,0x80000000},{0x80000001,0x7fffffff}}})for(unsigned cp:uniform?std::vector<unsigned>{0,1}:std::vector<unsigned>{0,6}){
  std::vector<u32> expected;auto spv=exercise(uniform,shift,cp,pair[0],pair[1],0x55,0xdeadbea5,7,!baseline,out,"case-"+std::to_string(id++),expected);gpu.run(spv,expected);
 }
 if(!baseline)for(bool state:{false,true})for(unsigned pred:{0U,8U}){std::vector<u32> expected;auto spv=exercise(false,2,6,0xffffffff,9,state?0x55:0x54,0xdeadbea5,pred,true,out,"predicate-"+std::to_string(id++),expected);gpu.run(spv,expected);}
 if(uniform_check&&!uniform_baseline){
  for(unsigned shift:{0U,6U,31U})for(bool negate:{false,true})for(auto pair:std::array<std::array<u32,2>,5>{{{0,0},{0xffffffff,1},{0x40000000,0xffffffff},{1,0x80000000},{0x80000001,0x7fffffff}}}){std::vector<u32> exp;auto spv=mixed_exercise(shift,4,negate,pair[0],pair[1],6,4,7,0x55,out,"mixed-"+std::to_string(id++),exp);gpu.run(spv,exp);++uniform_gpu_cases;}
  for(unsigned src:{0U,21U,22U,27U,63U})for(unsigned ra:{4U,255U})for(unsigned rd:{4U,6U,215U,255U}){std::vector<u32> exp;auto spv=mixed_exercise(6,src,false,0x12345678,0xfedcba98,rd,ra,7,0x55,out,"bank-"+std::to_string(id++),exp);gpu.run(spv,exp);++uniform_gpu_cases;}
  for(bool state:{false,true})for(unsigned pred:{0U,8U}){std::vector<u32> exp;auto spv=mixed_exercise(6,4,true,0x12345678,0x76543210,6,4,pred,state?0x55:0x54,out,"mixed-pred-"+std::to_string(id++),exp);gpu.run(spv,exp);++uniform_gpu_cases;}
  {std::vector<u32> exp;auto spv=mixed_exercise(6,4,false,0x12345678,0x76543210,214,215,7,0x55,out,"scratch-boundary",exp);gpu.run(spv,exp);++uniform_gpu_cases;}
 }
 }
 assert(validation_errors==0&&validation_warnings==0);std::cout<<"RESULT {\"passed\":true,\"baseline\":"<<(baseline?"true":"false")<<",\"actual_lower\":true,\"actual_local_translator_SSA_SPIRV_GPU_cases\":"<<cfg_cases<<",\"actual_CFG_TranslateProgram_cases\":"<<cfg_cases<<",\"IADD3_X_consumer\":"<<(!baseline?"true":"false")<<",\"negative_cases\":"<<negative_cases<<",\"uniform_source_GPU_cases\":"<<uniform_gpu_cases<<",\"uniform_source_negative_cases\":"<<uniform_negative_cases<<",\"uniform_source_boundary_accept_cases\":"<<uniform_accept_cases<<",\"Khronos_errors\":"<<validation_errors<<",\"Khronos_warnings\":"<<validation_warnings<<",\"full_ROM_replay\":false}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}
'''

uniform_cfg = support.CPP[support.CPP.index('std::vector<u32> cfg_probe('):support.CPP.index('Ampere::Instruction predicate_init')].replace('cfg_probe(', 'cfg_probe_uniform(').replace('n<5', 'n<34')
CPP = support.CPP[:support.CPP.index('int main(')] + uniform_cfg + EXTRA

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--lower-source', type=Path)
    ap.add_argument('--mode', choices=['baseline', 'modified'], default='modified')
    ap.add_argument('--adapter-index', type=int)
    ap.add_argument('--uniform-source-mode', choices=['none', 'baseline', 'modified'], default='none')
    a = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    build = repo / 'build/windows-ninja'
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    lower = (a.lower_source or repo / 'src/yuzu_shader_recompiler/frontend/ampere/lower_to_maxwell.h').resolve(strict=True)
    override = out / 'lower_override.h'; shutil.copy2(lower, override)
    cpp = out / 'native-lea-carry.cpp'; cpp.write_text(CPP, encoding='utf-8')
    exe = out / 'native-lea-carry.exe'; objects = out / 'objects'; objects.mkdir()
    sdk = Path(r'C:\VulkanSDK\1.4.357.0')
    local = repo / 'src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/load_store_local_shared.cpp'
    libs = [build / p for p in ['src/yuzu_shader_recompiler/Release/shader_recompiler.lib', 'src/common/Release/common.lib', 'src/yuzu_common/Release/yuzu_common.lib', 'external/fmt/Release/fmt.lib', 'external/sirit/src/Release/sirit.lib']]
    frozen = out / 'frozen'; frozen.mkdir(); frozen_libs = []
    for n, p in enumerate(libs):
        q = frozen / (str(n) + '-' + p.name); shutil.copy2(p, q); frozen_libs.append(q)
    command = ['cl', '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/MT', '/O2', '/I' + str(out), '/I' + str(repo / 'src'), '/I' + str(repo / 'external/boost'), '/I' + str(repo / 'external/fmt/include'), '/I' + str(build / 'src'), '/I' + str(sdk / 'Include'), str(cpp), str(local), '/Fe:' + str(exe), '/Fo:' + str(objects) + '\\', *map(str, frozen_libs), str(sdk / 'Lib/vulkan-1.lib'), '/link', '/OPT:REF', 'ws2_32.lib', 'winmm.lib', 'bcrypt.lib', 'crypt32.lib', 'iphlpapi.lib', 'psapi.lib', 'ole32.lib', 'shell32.lib']
    cmd = out / 'build.cmd'; cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n' + subprocess.list2cmdline(command) + '\n')
    paths = [Path(__file__), Path(support.__file__), lower, override, cpp, local, *frozen_libs]
    hashes = {str(p): sha(p) for p in paths}
    report = dict(command=subprocess.list2cmdline([sys.executable, '-B', Path(__file__), *sys.argv[1:]]), compiler_command=subprocess.list2cmdline(command), mode=a.mode, uniform_source_mode=a.uniform_source_mode, input='captured 04047291 00000006 0f80103f 001fc800 and 04067c11 00000004 0f8e30ff 020fc800; native LEA low-word/UP carry plus native IADD3.X and mixed uniform source; actual CFG/SSA/SPIRV/Vulkan; explicit adapter; no game replay', hashes=hashes)
    try:
        b = subprocess.run(['cmd', '/c', str(cmd)], capture_output=True, text=True, timeout=150)
        (out / 'build.log').write_text(b.stdout + b.stderr); report.update(stage='compile', exit_status=b.returncode, literal_stdout=b.stdout, literal_stderr=b.stderr); assert b.returncode == 0
        native = [str(exe), a.mode, str(out / 'cases'), a.uniform_source_mode]
        import os
        env = os.environ.copy()
        if a.adapter_index is not None:
            assert a.adapter_index >= 0
            env['NXEMU_LOCAL_SPILL_ADAPTER_INDEX'] = str(a.adapter_index)
        report['explicit_adapter_index'] = a.adapter_index
        r = subprocess.run(native, capture_output=True, text=True, timeout=60, env=env)
        (out / 'run.log').write_text(r.stdout + r.stderr); report.update(stage='run', native_command=subprocess.list2cmdline(native), exit_status=r.returncode, literal_stdout=r.stdout, literal_stderr=r.stderr); assert r.returncode == 0
        result = json.loads(next(x[7:] for x in r.stdout.splitlines() if x.startswith('RESULT '))); report['result'] = result
        validator = sdk / 'Bin/spirv-val.exe'; validation = []
        for spv in sorted((out / 'cases').glob('*.spv')):
            v = subprocess.run([str(validator), '--target-env', 'vulkan1.1', str(spv)], capture_output=True, text=True, timeout=15)
            validation.append(dict(path=str(spv), sha256=sha(spv), exit_status=v.returncode, literal_stdout=v.stdout, literal_stderr=v.stderr)); assert v.returncode == 0
        report['SPIRV_validation'] = validation
        assert all(sha(Path(p)) == h for p, h in hashes.items()), 'Frozen input changed during test'
        report.update(stage='complete', inputs_unchanged=True, executable_sha256=sha(exe))
        (out / 'RESULT.json').write_text(json.dumps(report, indent=2))
        print('NATIVE_LEA_CARRY_PASS; mode=' + a.mode + '; actual_backend=True; GPU_cases=' + str(result['actual_local_translator_SSA_SPIRV_GPU_cases']))
    except BaseException:
        report['failure'] = traceback.format_exc(); (out / 'FAILURE.json').write_text(json.dumps(report, indent=2)); raise

if __name__ == '__main__': main()
