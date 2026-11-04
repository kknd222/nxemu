"""Compile actual request-only capture-context header with fake GPU scalars and real IO.
No renderer/GPU execution or identity authentication is performed.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess,sys
CPP=r"""
#include "yuzu_video_core/frame_capture_context.h"
#include <cassert>
#include <atomic>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
using namespace VideoCommon::FrameCaptureContext;
std::string id(unsigned n){std::ostringstream s;s<<std::hex<<n;return s.str();}
std::filesystem::path file(const std::filesystem::path& root,unsigned n){return root/("frame-capture-"+id(n)+".context.json");}
std::string contents(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
Selection selection(uint64_t n){Selection s;s.valid=true;s.query=0x100+n;s.image_id=n;s.view_id=n+100;s.cpu_owner=0x200+n;s.gpu_owner=0x300+n;s.page_table=7;s.address_space_valid=true;s.address_space=9;s.width=640;s.height=360;s.format=1;s.flags=2;s.modification=11;s.published=12;s.gpu_write=13;s.layer=2;return s;}
Binding binding(){Binding b;b.accelerated=true;b.source_image=1001;b.source_view=1002;b.descriptor_image=2001;b.descriptor_view=2002;b.descriptor_set=3001;b.sampler=4001;b.resource_index=1;b.texture_width=640;b.texture_height=360;b.scaled_width=1280;b.scaled_height=720;b.output_width=1280;b.output_height=720;b.crop={0,0,1,1};return b;}
void record(unsigned n){BeginLayer();Selected(selection(n));Bound(binding());}
void unit(const std::filesystem::path& root){
 static_assert(noexcept(Bound(Binding{})));static_assert(noexcept(Selected(Selection{})));static_assert(noexcept(Path("x",false)));static_assert(std::is_nothrow_destructible_v<Scope>);
 assert(!Active());
 for(unsigned n=0;n<10000;++n){SetRole(Role::Capture);BeginLayer();Selected(selection(n));Bound(binding());Target(n,n+1);Path("off",true);}
 assert(!Active());assert(std::filesystem::is_empty(root));
 auto* before=current;
 for(auto t:{"relative",""}){bool failed=false;try{Scope s(t,"1","run","3",1,true);}catch(const std::exception&){failed=true;}assert(failed&&current==before);}
 for(auto t:{"../bad","XYZ",""}){bool failed=false;try{Scope s(root,t,"run","3",1,true);}catch(const std::exception&){failed=true;}assert(failed&&current==before);}
 for(auto generation:{"","abc","123456789012345678901"}){bool failed=false;try{Scope s(root,"a","run",generation,1,true);}catch(const std::exception&){failed=true;}assert(failed&&current==before);}
 for(auto run:{std::string{},std::string(65,'x')}){bool failed=false;try{Scope s(root,"a",run,"3",1,true);}catch(const std::exception&){failed=true;}assert(failed&&current==before);}
 {
  Scope s(root,id(1),"run","3",77,true);assert(Active()&&current==s.data.get());
  std::array<Input,7> inputs{};for(unsigned n=0;n<inputs.size();++n){auto& i=inputs[n];i.address=0x1234+n;i.offset=64;i.width=640;i.height=360;i.stride=640;i.format=1;i.transform=2;i.blend=3;i.block_height=4;i.crop={1,2,639,359};i.native=true;}
  s.Inputs(inputs);assert(s.data->input_total==7&&s.data->input_count==MaxLayers);
  SetRole(Role::Capture);record(11);SetRole(Role::Swapchain);record(12);Target(9001,9002);Path("present_submitted",true,true,false);
  assert(s.data->source_count==2&&s.data->sources[0].role==Role::Capture&&s.data->sources[1].role==Role::Swapchain);
  const auto serialized=Serialize(*s.data);assert(serialized.size()<=MaxBytes);
 }
 assert(!Active());assert(std::filesystem::is_regular_file(file(root,1)));
 {
  Scope outer(root,id(2),"outer","3",88,true);auto* outer_ptr=current;record(21);
  {
   Scope inner(root,id(3),"inner","5",89,false);record(31);Path("hidden",false);
  }
  assert(current==outer_ptr&&outer.data->source_count==1&&outer.data->sources[0].selected.image_id==21);
  bool failed=false;try{Scope invalid("relative",id(44),"bad","3",1,true);}catch(...){failed=true;}assert(failed&&current==outer_ptr);
  record(22);Path("present_submitted",true);
 }
 assert(!Active());
 {
  Scope s(root,id(4),"bounds","3",90,true);SetRole(Role::Capture);
  for(unsigned n=0;n<7;++n)record(40+n);
  assert(s.data->source_count==4&&s.data->sources_dropped==3);Path("held_incomplete",true,true,true);
 }
 {
  Scope s(root,id(5),"bounds","3",91,true);BeginLayer();Selected(selection(55));
  for(unsigned n=0;n<25;++n)Bound(binding());
  assert(s.data->source_count==MaxSources&&s.data->sources_dropped==9);Path("present_submitted",true);
 }
 {
  Scope s(root,id(6),"nonfinite","3",92,true);BeginLayer();auto a=selection(66);a.address_space_valid=false;Selected(a);auto b=binding();b.crop={std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),1};Bound(b);Path("present_submitted",true);
 }
 try{Scope s(root,id(7),"exception","3",93,true);record(77);throw 42;}catch(int n){assert(n==42);}assert(!Active());
 // Real IO failure must neither throw from destructor nor leave active TLS.
 auto blocker=root/"not-a-directory";{std::ofstream f(blocker);f<<"x";}
 {Scope s(blocker,id(8),"fail","3",94,true);record(88);Path("present_submitted",true);}assert(!Active());assert(!std::filesystem::exists(blocker/file(root,8).filename()));
 // Direct serialization budget rejection, independent of GPU data or filesystem.
 Data over;over.id=std::string(MaxBytes+1,'a');bool rejected=false;try{auto x=Serialize(over);(void)x;}catch(const std::exception&){rejected=true;}assert(rejected);
 {Scope s(root,id(9),"budget","3",95,true);s.data->path=std::string(MaxBytes+1,'a');}assert(!Active());assert(!std::filesystem::exists(file(root,9)));
 // Public counter validation and role clamps are tested without touching production hooks.
 for(unsigned mode=0;mode<2;++mode){Data bad;bad.input_count=mode?0:MaxLayers+1;bad.source_count=mode?MaxSources+1:0;bool failed=false;try{auto x=Serialize(bad);(void)x;}catch(const std::exception&){failed=true;}assert(failed);}
 {Scope s(root,id(11),"invalid-role","3",97,true);SetRole(static_cast<Role>(123));record(111);assert(s.data->sources[0].role==Role::Other);Path("present_submitted",true);}
 // A source pending selection must not leak into next layer when no accelerated selection occurs.
 {Scope s(root,id(10),"selection-reset","3",96,true);record(101);BeginLayer();Bound(binding());assert(!s.data->sources[1].selected.valid);Path("present_submitted",true);}
 std::atomic<unsigned> completed{};std::vector<std::thread> threads;
 for(unsigned n=0;n<8;++n)threads.emplace_back([&,n]{assert(!Active());{Scope s(root,id(100+n),"thread-"+std::to_string(n),"3",1000+n,true);for(unsigned j=0;j<4;++j)record(10000+n*10+j);Path("present_submitted",true);}assert(!Active());++completed;});
 for(auto& t:threads)t.join();assert(completed==8&&!Active());
 std::cout<<"{\"passed\":true,\"actual_header\":true,\"fake_GPU_data\":true,\"GPU_executed\":false,\"off_operations\":10000,\"threads\":8,\"bounded_sources\":16,\"bounded_layers\":4,\"max_bytes\":16384,\"nesting\":true,\"IO_failure_isolated\":true,\"exception_path\":true,\"nonfinite_crop\":true,\"pending_reset\":true,\"counter_validation\":true,\"invalid_role_clamped\":true}\n";
}
int main(int argc,char** argv){assert(argc==2);std::filesystem::path root=argv[1];std::filesystem::create_directories(root);unit(root);}
"""
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source-root',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
 a=ap.parse_args();root=a.source_root.resolve(strict=True)
 if (root/'repo/src').is_dir():root=root/'repo'
 header=root/'src/yuzu_video_core/frame_capture_context.h';assert header.is_file()
 out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);files=out/'files';assert not files.exists(),'Use a new output directory; IO failures intentionally retained'
 cpp=out/'capture-context-test.cpp';cpp.write_text(CPP,encoding='utf-8');exe=out/'capture-context-test.exe';obj=out/'capture-context-test.obj'
 args=['cl','/nologo','/std:c++20','/EHsc','/MT','/O2','/utf-8','/I'+str(root/'src'),str(cpp),'/Fe:'+str(exe),'/Fo:'+str(obj)]
 cmd=out/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(args)+'\n',encoding='utf-8')
 sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();hashes={str(p):sha(p) for p in [header,cpp,Path(__file__).resolve()]}
 report={'command':subprocess.list2cmdline([sys.executable,'-B',Path(__file__).resolve(),*sys.argv[1:]]),'compiler_command':subprocess.list2cmdline(args),'input':'actual production request-context header; fake GPU scalar handles/owners/crop; real IO; no renderer or GPU execution','hashes':hashes}
 b=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=90);(out/'build.log').write_text(b.stdout+b.stderr,encoding='utf-8')
 if b.returncode:
  report.update(stage='compile',literal_stdout=b.stdout,literal_stderr=b.stderr,exit_status=b.returncode);(out/'FAILURE.json').write_text(json.dumps(report,indent=2));print(b.stdout+b.stderr);raise SystemExit(b.returncode)
 native=[str(exe),str(files)];r=subprocess.run(native,capture_output=True,text=True,timeout=30);(out/'run.log').write_text(r.stdout+r.stderr,encoding='utf-8');unchanged=all(sha(Path(p))==h for p,h in hashes.items())
 report.update(native_command=subprocess.list2cmdline(native),literal_stdout=r.stdout,literal_stderr=r.stderr,exit_status=r.returncode,inputs_unchanged=unchanged,executable_sha256=sha(exe))
 if r.returncode or not unchanged:
  report['stage']='run' if r.returncode else 'hash_binding';(out/'FAILURE.json').write_text(json.dumps(report,indent=2));print(r.stdout+r.stderr);raise SystemExit(r.returncode or 1)
 data={p.name:json.loads(p.read_text()) for p in files.glob('*.context.json')}
 # Validate serialization independently with Python's strict JSON reader.
 assert data['frame-capture-1.context.json']['input_total']==7
 one=data['frame-capture-1.context.json'];assert one['input_count']==4 and len(one['inputs'])==4;assert one['composite_ordinal']==77 and one['request_id']=='1' and one['return_path']=='present_submitted'
 assert [x['role'] for x in one['sources']]==['offscreen_recomposition','swapchain_draw']
 assert [x['image_id'] for x in one['sources']]==[11,12] and one['target_image']==9001
 assert data['frame-capture-2.context.json']['run_id']=='outer' and len(data['frame-capture-2.context.json']['sources'])==2
 assert data['frame-capture-3.context.json']['run_id']=='inner' and data['frame-capture-3.context.json']['return_path']=='hidden'
 assert data['frame-capture-4.context.json']['source_count']==4 and data['frame-capture-4.context.json']['sources_dropped']==3
 assert data['frame-capture-4.context.json']['hold_checked'] and data['frame-capture-4.context.json']['held']
 assert data['frame-capture-5.context.json']['source_count']==16 and data['frame-capture-5.context.json']['sources_dropped']==9
 nonfinite=data['frame-capture-6.context.json']['sources'][0];assert nonfinite['crop']==[None,None,None,1] and nonfinite['address_space'] is None
 assert data['frame-capture-7.context.json']['return_path']=='exception_or_unclassified_return'
 assert data['frame-capture-b.context.json']['sources'][0]['role']=='other_draw'
 assert one['snapshot_epoch_ms']>0 and one['publication_at_Composite_scope_exit']
 for n in range(8):
  d=data[f'frame-capture-{100+n:x}.context.json'];assert d['run_id']==f'thread-{n}' and d['composite_ordinal']==1000+n and [x['image_id'] for x in d['sources']]==[10000+n*10+j for j in range(4)]
 assert all(p.stat().st_size<=16384 for p in files.glob('*.context.json'))
 report['result']=json.loads(r.stdout);report['serialization_validated']=True;report['published_files']={str(p):sha(p) for p in files.glob('*.context.json')};report['publication_count']=len(data)
 (out/'RESULT.json').write_text(json.dumps(report,indent=2),encoding='utf-8');print(r.stdout,end='')
if __name__=='__main__':main()
