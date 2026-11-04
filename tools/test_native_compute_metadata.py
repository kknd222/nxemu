"""Actual native fixture header+reader tests; CPU metadata only, no GPU visibility claim."""
from pathlib import Path
import argparse, copy, hashlib, importlib.util, json, os, subprocess, sys

CPP=r'''
#include "yuzu_video_core/native_shader_fixture.h"
#include <cassert>
#include <iostream>
#include <cstring>
#include <cstdlib>
namespace N=VideoCommon::NativeShaderFixture;
std::filesystem::path root;
namespace VideoCommon::NativeShaderFixture {
bool Enabled(){return std::string(std::getenv("NXEMU_NATIVE_SHADER_FIXTURES"))=="1";}
std::filesystem::path Root(){return root;}
std::string RunId(){return "metadata-test";}
std::string Generation(){return "3";}
bool Replace(const std::filesystem::path& a,const std::filesystem::path& b) noexcept {
 std::error_code ec;std::filesystem::remove(b,ec);ec.clear();std::filesystem::rename(a,b,ec);return !ec;
}
}
int main(int argc,char**argv){
 assert(argc==2);root=argv[1];std::filesystem::create_directories(root);
 std::array<uint32_t,8> words{1,2,3,4,0x0830723c,12,0x8ff,0x041ff000};
 N::Metadata m{0x10000,16,5,0,0x23c,{0x0830723c,12,0x8ff,0x041ff000},"matrix","complete"};
#ifdef HAS_CONTEXT
 m.compute.available=m.compute.dispatch_available=true;m.compute.local_memory_size=512;
 m.compute.shared_memory_size=4096;m.compute.texture_bound_buffer=2;m.compute.workgroup_size={128,1,1};
 m.compute.grid={64,24,1};m.compute.dispatch_token=9;m.compute.dispatch_program=m.program;m.compute.cbuf_mask=1;
 m.compute.cbufs[0]={0x20000,256,true};
#endif
 const auto blob=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(words.data()),sizeof(words));
 N::Capture(blob,m,1000);
 if(!N::Enabled()){assert(N::registry.entries.empty());std::cout<<"COMPUTE_METADATA_OFF_PASS CPU_ONLY=1\n";return 0;}
 assert(N::registry.entries.size()==1);
#ifdef HAS_CONTEXT
 m.compute.shared_memory_size=9999;
#endif
 N::Capture(blob,m,1010);assert(N::registry.duplicates==1&&N::registry.entries.size()==1);
 for(unsigned i=0;i<8;++i){words[0]=100+i;N::Capture(blob,m,2000+i*1000);}
 assert(N::registry.entries.size()==8&&N::registry.quota_drops==1);
 std::cout<<"COMPUTE_METADATA_ON_PASS retained=8 duplicates=1 quota_drops=1 context="
#ifdef HAS_CONTEXT
 <<1
#else
 <<0
#endif
 <<" GPU_RESOURCE_BYTES=0 GPU_VISIBILITY=0\n";
}
'''

def main():
 p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();r=a.source_root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
 header=r/'src/yuzu_video_core/native_shader_fixture.h';has='ComputeSnapshot' in header.read_text();cpp=out/'test.cpp';cpp.write_text(CPP);exe=out/'test.exe';cmd=['cl','/nologo','/std:c++20','/EHsc','/MT','/O2','/utf-8','/I'+str(r/'src'),str(cpp),'/Fe:'+str(exe),'/Fo:'+str(out/'test.obj')]
 if has:cmd.insert(1,'/DHAS_CONTEXT')
 build=out/'build.cmd';build.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'+subprocess.list2cmdline(cmd)+'\n');b=subprocess.run(['cmd','/c',str(build)],capture_output=True,text=True,timeout=120);(out/'build.log').write_text(b.stdout+b.stderr);assert b.returncode==0,b.stdout+b.stderr
 spec=importlib.util.spec_from_file_location('reader',r/'tools/nx2_lab.py');reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader);rows=[]
 for mode in ['off','on']:
  dest=out/mode/'diagnostics';env=dict(os.environ,NXEMU_NATIVE_SHADER_FIXTURES='1' if mode=='on' else '0');run=subprocess.run([str(exe),str(dest)],capture_output=True,text=True,env=env,timeout=20);assert run.returncode==0,(run.stdout,run.stderr);rows.append(dict(command=subprocess.list2cmdline([str(exe),str(dest)]),input={'NXEMU_NATIVE_SHADER_FIXTURES':env['NXEMU_NATIVE_SHADER_FIXTURES']},stdout=run.stdout,stderr=run.stderr,exit_status=run.returncode));print(run.stdout,end='')
  if mode=='off':assert not (dest/'native-fixtures').exists()
  else:
   d=reader.inspect_cases(out/mode);assert len(d['cases'])==8
   if has:
    c=d['cases'][0]['compute_environment'];assert c['shared_memory_size']==4096 and c['grid']==[64,24,1] and c['constant_buffers'][0]['size']==256;assert d['cases'][0]['replay_ready'] is False
    index=json.loads((dest/'native-fixtures/index.json').read_text());meta=json.loads((dest/'native-fixtures'/index['entries'][0]['metadata']).read_text());bad=[]
    for key,value in [('available',1),('local_memory_size',-1),('shared_memory_size',1<<32),('texture_bound_buffer',True),('dispatch_available',1),('dispatch_token',-1),('dispatch_program',2),('workgroup_size',[1,2]),('grid',[1,2,True]),('constant_buffers',[]),('resource_bytes_captured',True),('GPU_visibility_proven',True)]:
     x=copy.deepcopy(meta);x['compute_environment'][key]=value;bad.append(x)
    x=copy.deepcopy(meta);x['stage']=0;bad.append(x)
    for key,value in [('slot',1),('enabled',1),('address',-1),('size',1<<32)]:x=copy.deepcopy(meta);x['compute_environment']['constant_buffers'][0][key]=value;bad.append(x)
    for value in [True,1.0]:x=copy.deepcopy(meta);x['compute_environment']['constant_buffers'][1]['slot']=value;bad.append(x)
    for key,value in [('dispatch_token',0),('cbuf_mask',0x101)]:x=copy.deepcopy(meta);x['compute_environment'][key]=value;bad.append(x)
    for key,value in [('stage',5.0),('program',float(meta['program']))]:x=copy.deepcopy(meta);x[key]=value;bad.append(x)
    x=copy.deepcopy(meta);x['compute_environment']['dispatch_available']=False;bad.append(x)
    for x in bad:
     try:reader.compute_context(x)
     except ValueError:pass
     else:raise AssertionError('malformed metadata accepted')
    legacy=copy.deepcopy(meta);del legacy['compute_environment'];assert reader.compute_context(legacy)['available'] is False
    print('COMPUTE_READER_PASS negative_cases='+str(len(bad))+' legacy=True GPU=False')
 result=dict(command=subprocess.list2cmdline([sys.executable,'-B',Path(__file__),*sys.argv[1:]]),source_root=str(r),header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),compute_metadata_present=has,CPU_only=True,GPU=False,results=rows);(out/'RESULT.json').write_text(json.dumps(result,indent=2))
if __name__=='__main__':main()
