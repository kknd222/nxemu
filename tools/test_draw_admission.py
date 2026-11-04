"""Compile actual admission/owner registry headers. Synthetic CPU events, no GPU execution."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,sys,traceback
CPP=r'''
#include "yuzu_video_core/draw_admission_trace.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
using namespace VideoCommon;namespace A=DrawAdmission;namespace W=ImageWriters;const std::array<uint64_t,6> zero_hashes{};
void save(const std::filesystem::path& out,const char* name,const A::Snapshot& x){auto text=A::Serialize(x);assert(text.size()<=A::MaxBytes);std::ofstream f(out/name);f<<text;assert(f);}
A::Target target(uint64_t owner,uint32_t slot=0){return {owner,slot,1280,720,51,2,3,9,7,0x1111,slot};}
int main(int argc,char**argv){
 if(argc==3&&std::string(argv[1])=="--global"){
  const bool on=std::string(argv[2])=="on";assert(A::Enabled()==on);SetEnvironmentVariableA("NXEMU_IMAGE_WRITERS",on?"0":"1");assert(A::Enabled()==on);
  W::Birth(0x9980,1,0x8880,0,2,2);{A::Scoped s;s.BeginTarget(target(0x8880));A::MarkRefresh(A::Refresh::CachedInvalid);s.Finish(A::Outcome::PipelineNull);}
  assert(A::Read(W::Read(0x9980)).available==on);std::cout<<"STARTUP_PASS enabled="<<on<<" cached=1\n";return 0;
 }
 assert(argc==2);std::filesystem::path out=argv[1];std::filesystem::create_directories(out);
 auto w=std::make_unique<W::Registry>(true);auto a=std::make_unique<A::Registry>(true);
 const auto birth=w->Birth(0x1000,1,0x3000,0,1280,720);assert(birth);const auto owner=w->OwnerBindings(0x3000);assert(owner.count==1&&owner.bindings[0].birth==birth&&!owner.overflow&&!owner.lockmiss);
 assert(!w->OwnerBindings(0).count&&!w->OwnerBindings(0x9999).count);
 {auto off=std::make_unique<W::Registry>(false);assert(!off->OwnerBindings(0x3000).count);}
 {A::Scoped s(false,a.get(),w.get());s.BeginTarget(target(0x3000));A::MarkKey(999,{1,2,3,4,5,6});s.Finish(A::Outcome::PipelineReady);}assert(!a->Read(w->Read(0x1000)).available);
 {A::Scoped s(true,a.get(),w.get());s.BeginTarget(target(0x3000));A::MarkStage(4,0x1234);A::MarkStageHash(4,0x777);A::Reject(0x1200,0x10,3,0x9c3,{0x000479c3,0,0x1e00,0xe7000},"native special register");A::MarkRefresh(A::Refresh::Exception);s.Finish(A::Outcome::PipelineNull);}
 auto rejected=a->Read(w->Read(0x1000));assert(rejected.count==1);const auto& p=rejected.packets[0];assert(!p.key_valid&&!p.key&&p.hashes==zero_hashes);assert(p.rejection&&p.opcode==0x9c3&&p.failed_stage==4&&p.stage_hash_valid_mask==16&&p.bindings.bindings[0].birth==birth);save(out,"rejected.json",rejected);
 {A::Scoped s(true,a.get(),w.get());s.BeginTarget(target(0x3000));A::MarkRefresh(A::Refresh::CachedInvalid);s.Finish(A::Outcome::PipelineNull);}
 auto cached=a->Read(w->Read(0x1000));const auto& c=cached.packets[cached.count-1];assert(c.refresh==A::Refresh::CachedInvalid&&!c.rejection&&!c.key_valid&&c.failed_stage==-1&&c.stage_visited_mask==0&&c.programs==zero_hashes);save(out,"cached_invalid.json",cached);
 {A::Scoped s(true,a.get(),w.get());s.BeginTarget(target(0x3000));A::MarkRefresh(A::Refresh::CachedValid);A::MarkKey(123,{1,2,3,4,5,6});s.Finish(A::Outcome::PipelineReady);}
 auto ready=a->Read(w->Read(0x1000));assert(ready.packets[ready.count-1].key_valid&&ready.packets[ready.count-1].key==123);save(out,"key.json",ready);
 // Birth is captured BEFORE call and cannot be rebound to a reused VkImage afterwards.
 {A::Scoped s(true,a.get(),w.get());s.BeginTarget(target(0x3000));assert(w->Birth(0x1000,2,0x3000,0,2,2)>birth);s.Finish(A::Outcome::PipelineNull);}
 auto reused=a->Read(w->Read(0x1000));assert(!reused.available&&reused.nonmatching_packets==4);save(out,"reused.json",reused);
 {A::Scoped s(true,a.get(),w.get());s.BeginTarget(target(0x9999));s.Finish(A::Outcome::PipelineNull);}auto wrong=a->Read(w->Read(0x1000));assert(!wrong.available&&wrong.unbound==1);save(out,"wrong_owner.json",wrong);
 // Exceptions terminate only this scope and TLS is restored.
 try{A::Scoped s(true,a.get(),w.get());s.BeginTarget(target(0x3000));A::MarkStage(0,42);throw 1;}catch(int){}
 assert(A::current==nullptr);auto exception=a->Read(w->Read(0x1000));assert(exception.available&&exception.packets[0].outcome==A::Outcome::ExceptionAborted);save(out,"exception.json",exception);
 const auto second=w->Birth(0x2000,3,0x4000,0,2,2);assert(second);
 {A::Scoped outer(true,a.get(),w.get());outer.BeginTarget(target(0x3000));A::MarkStage(0,100);{A::Scoped disabled(false,a.get(),w.get());A::MarkStage(2,999);A::Reject(999,0,2,7,{9,9,9,9},"disabled nested");}
  {A::Scoped inner(true,a.get(),w.get());inner.BeginTarget(target(0x4000));A::MarkStage(1,200);A::Reject(200,0,1,9,{1,2,3,4},"inner");inner.Finish(A::Outcome::PipelineNull);}
  A::MarkStageHash(0,99);outer.Finish(A::Outcome::PipelineReady);
 }assert(A::current==nullptr);auto inner=a->Read(w->Read(0x2000));assert(inner.count==1&&inner.packets[0].programs[1]==200&&inner.packets[0].rejection);auto outer=a->Read(w->Read(0x1000));assert(outer.packets[outer.count-1].programs[0]==100&&!outer.packets[outer.count-1].rejection);save(out,"nested_inner.json",inner);save(out,"nested_outer.json",outer);
 for(int n=0;n<12;++n){A::Scoped s(true,a.get(),w.get());s.BeginTarget(target(0x3000));A::MarkKey(n,{1,2,3,4,5,6});s.Finish(A::Outcome::PipelineReady);}auto ring=a->Read(w->Read(0x1000));assert(ring.count==4&&ring.packets[0].key==8&&ring.packets[3].key==11&&ring.packets[3].owner_total==18);save(out,"ring.json",ring);

 // A rejected admission is retained separately after cached-invalid repetitions erase its ring entry.
 auto wf=std::make_unique<W::Registry>(true);auto af=std::make_unique<A::Registry>(true);const auto first_birth=wf->Birth(0xd000,1,0xe000,0,2,2);
 {A::Scoped s(true,af.get(),wf.get());s.BeginTarget(target(0xe000));A::Reject(77,16,3,0x9c3,{1,2,3,4},"first\"quoted\\reason");s.Finish(A::Outcome::PipelineNull);}
 for(int n=0;n<10;++n){A::Scoped s(true,af.get(),wf.get());s.BeginTarget(target(0xe000));A::MarkRefresh(A::Refresh::CachedInvalid);s.Finish(A::Outcome::PipelineNull);}auto first=af->Read(wf->Read(0xd000));assert(first.count==5&&first.packets[0].rejection&&first.packets[0].retained_first_rejection&&first.packets[0].owner_total==11);save(out,"first_rejection.json",first);
 wf->Birth(0xd000,1,0xe000,0,2,2);auto old_first=af->Read(wf->Read(0xd000));assert(!old_first.available&&old_first.nonmatching_packets==5);save(out,"first_rejection_reused.json",old_first);
 {A::Scoped s(true,af.get(),wf.get());s.BeginTarget(target(0xe000));A::Reject(88,32,3,99,{5,6,7,8},"new birth");s.Finish(A::Outcome::PipelineNull);}auto fresh_first=af->Read(wf->Read(0xd000));assert(fresh_first.first_rejection_replaced==1&&fresh_first.count==1&&fresh_first.packets[0].rejection_program==88&&fresh_first.packets[0].bindings.bindings[0].birth>first_birth);save(out,"first_rejection_replaced.json",fresh_first);
 // Nine same-owner images exceed eight fixed binding slots; no silent completeness claim.
 auto wb=std::make_unique<W::Registry>(true);auto ab=std::make_unique<A::Registry>(true);for(int n=0;n<9;++n)assert(wb->Birth(0x7000+n*0x100,1,0x9000,0,2,2));auto many=wb->OwnerBindings(0x9000);assert(many.count==8&&many.overflow);
 uint64_t selected=many.bindings[0].image;{A::Scoped s(true,ab.get(),wb.get());s.BeginTarget(target(0x9000));s.Finish(A::Outcome::PipelineNull);}auto bound=ab->Read(wb->Read(selected));assert(bound.count==1&&bound.binding_overflow==1&&bound.packets[0].bindings.overflow&&bound.packets[0].bindings.count==1&&bound.packets[0].non_selected_bindings==7);save(out,"binding_overflow.json",bound);
 // Concurrent independent TLS scopes; bounded try_lock omissions observed, not hidden.
 auto ws=std::make_unique<W::Registry>(true);auto as=std::make_unique<A::Registry>(true);for(unsigned t=0;t<8;++t)ws->Birth(0xa000+t*0x100,1,0xb000+t*0x100,0,2,2);
 std::atomic<unsigned> go{},ready_threads{},finished{};std::vector<std::thread> threads;for(unsigned t=0;t<8;++t)threads.emplace_back([&,t]{++ready_threads;while(!go.load())std::this_thread::yield();for(unsigned n=0;n<2000;++n){A::Scoped s(true,as.get(),ws.get());s.BeginTarget(target(0xb000+t*0x100,t));A::MarkStage(t%6,0xc000+t);A::MarkRefresh(A::Refresh::CachedInvalid);s.Finish(A::Outcome::PipelineNull);}assert(A::current==nullptr);++finished;});while(ready_threads!=8)std::this_thread::yield();go=1;for(auto& t:threads)t.join();assert(finished==8);
 {A::Scoped s(true,as.get(),ws.get());s.BeginTarget(target(0xb000));s.Finish(A::Outcome::PipelineReady);}auto concurrent=as->Read(ws->Read(0xa000));assert(concurrent.available&&concurrent.lock_drops>0&&concurrent.count<=4);save(out,"concurrent.json",concurrent);
 {A::Snapshot invalid;invalid.count=A::MaxPackets+1;bool rejected=false;try{A::Serialize(invalid);}catch(const std::length_error&){rejected=true;}assert(rejected);invalid.count=1;invalid.packets[0].bindings.count=9;rejected=false;try{A::Serialize(invalid);}catch(const std::length_error&){rejected=true;}assert(rejected);}
 // Maximum selected-chain packet count and long/quoted reason are bounded at publication.
 A::Snapshot max;max.available=true;max.count=20;for(auto& p:max.packets){p.token=~uint64_t{};p.record_seq=~uint64_t{};p.key_valid=true;p.key=~uint64_t{};p.programs.fill(~uint64_t{});p.hashes.fill(~uint64_t{});p.stage_hashes.fill(~uint64_t{});p.bindings.count=4;for(auto& b:p.bindings.bindings){b={~uint64_t{},~uint64_t{},~uint32_t{},~uint32_t{}};}p.reason.fill('x');p.reason.back()='\0';}bool max_refused=false;try{save(out,"maximum.json",max);}catch(const std::length_error&){max_refused=true;}assert(max_refused);max.count=20;for(auto& p:max.packets){p.bindings.count=1;p.programs={};p.hashes={};p.stage_hashes={};p.reason={};}save(out,"maximum.json",max);
 std::cout<<"{\"passed\":true,\"actual_header\":true,\"CPU_events_only\":true,\"GPU_executed\":false,\"threads\":8,\"scopes\":16000,\"max_packets\":20,\"max_bytes\":32768,\"startup_tested_separately\":true}\n";
}
'''
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,required=True);p.add_argument('--out',type=Path,required=True);args=p.parse_args();root=args.source_root.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False);files=out/'files';cpp=out/'admission-test.cpp';cpp.write_text(CPP,encoding='utf-8');exe=out/'admission-test.exe'
 compiler=['cl','/nologo','/std:c++20','/EHsc','/MT','/O2','/utf-8','/I'+str(root/'src'),str(cpp),'/Fe:'+str(exe),'/Fo:'+str(out/'admission-test.obj')]
 cmd=out/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(compiler)+'\n',encoding='utf-8')
 bound=[root/'src/yuzu_video_core/draw_admission_trace.h',root/'src/yuzu_video_core/image_writer_trace.h',root/'src/yuzu_common/debug_document.h',Path(__file__).resolve(),cpp];hashes={str(p):sha(p) for p in bound};report={'command':subprocess.list2cmdline([sys.executable,'-B',Path(__file__),*sys.argv[1:]]),'input':'actual production headers; synthetic cached image births, owner registers and shader admission; CPU threads/filesystem, no GPU','hashes':hashes,'compiler_command':subprocess.list2cmdline(compiler)}
 try:
  b=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=120);(out/'build.log').write_text(b.stdout+b.stderr);report.update(stage='compile',literal_stdout=b.stdout,literal_stderr=b.stderr,exit_status=b.returncode);assert b.returncode==0,report
  native=[str(exe),str(files)];r=subprocess.run(native,capture_output=True,text=True,timeout=90);(out/'run.log').write_text(r.stdout+r.stderr);report.update(stage='run',native_command=subprocess.list2cmdline(native),literal_stdout=r.stdout,literal_stderr=r.stderr,exit_status=r.returncode);assert r.returncode==0,report
  data={p.name:json.loads(p.read_text()) for p in files.glob('*.json')};assert len(data)==15,data.keys();assert data['rejected.json']['packets'][0]['opcode']==0x9c3;assert not data['cached_invalid.json']['packets'][-1]['key_valid'];assert not data['reused.json']['available'];assert all(p.stat().st_size<=32768 for p in files.glob('*.json'))
  globals=[]
  for mode in ['off','on']:
   env=dict(os.environ);env['NXEMU_IMAGE_WRITERS']='1' if mode=='on' else '0';g=subprocess.run([str(exe),'--global',mode],capture_output=True,text=True,env=env,timeout=15);assert g.returncode==0,(g.stdout,g.stderr);globals.append({'command':subprocess.list2cmdline([str(exe),'--global',mode]),'input':'NXEMU_IMAGE_WRITERS='+env['NXEMU_IMAGE_WRITERS'],'literal_stdout':g.stdout,'literal_stderr':g.stderr,'exit_status':g.returncode})
  assert all(sha(Path(p))==h for p,h in hashes.items()),'Source changed during test';report.update(stage='complete',result=json.loads(r.stdout),serialization_validated=True,publication_count=len(data),published_files={str(p):sha(p) for p in files.glob('*.json')},startup_switch_tests=globals,inputs_unchanged=True);(out/'RESULT.json').write_text(json.dumps(report,indent=2));print('ADMISSION_TEST_PASS; actual_headers=True; TLS=True; birth_reuse=True; ring4=True; threads8=True; GPU=False')
 except BaseException:
  report['failure']=traceback.format_exc();(out/'FAILURE.json').write_text(json.dumps(report,indent=2));raise
if __name__=='__main__':main()
