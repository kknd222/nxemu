"""Actual native invocation header and actual DrawManager::State CPU mapping tests."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,sys,traceback
CPP=r'''
#include "yuzu_video_core/native_draw_invocation.h"
#include "yuzu_video_core/native_draw_input_fixture.h"
#include "yuzu_video_core/engines/draw_manager.h"
#include <cassert>
#include <thread>
#include <vector>
#include <iostream>
using namespace VideoCommon::NativeDrawInvocation;
using DM=Tegra::Engines::DrawManager;
DM::State State(){DM::State s{};s.topology=Tegra::Engines::PrimitiveTopology::Triangles;s.draw_mode=DM::DrawMode::General;s.draw_indexed=true;s.instance_count=99;s.vertex_buffer={12,3};s.index_buffer.first=4;s.index_buffer.count=6;s.index_buffer.format=Tegra::Engines::Maxwell3D::Regs::IndexFormat::UnsignedShort;s.index_buffer.start_addr_high=0x12345678;s.index_buffer.start_addr_low=0x87654321;s.base_index=0xfffffff8;s.base_instance=7;return s;}
VideoCommon::NativeDrawInputs::IndexPrefix Index(){VideoCommon::NativeDrawInputs::IndexPrefix i{};i.requested=true;i.range_valid=true;i.mapped=true;i.address=0x1000;i.limit_address=0x1005;i.element_size=2;i.count=3;i.first=0;i.bytes=6;i.prefix={0,0,1,0,2,0};return i;}
DM::IndirectParams Params(){DM::IndirectParams p{};p.is_indexed=true;p.is_byte_count=false;p.include_count=true;p.count_start_address=0x55;p.indirect_start_address=0x66;p.buffer_size=100;p.max_draw_counts=8;p.stride=20;p.instance_mask=0xffff;return p;}
int main(int argc,char** argv){assert(argc==3);const std::string mode=argv[1];auto state=State();auto params=Params();assert(!Current().available);
if(mode=="capture"||mode=="capture_duplicate"||mode=="index_capture"){
 namespace N=VideoCommon::NativeDrawInputs;std::filesystem::path root=argv[2];std::filesystem::create_directories(root);
 _putenv_s("NXEMU_NATIVE_SHADER_DIAG_DIR",root.string().c_str());_putenv_s("NXEMU_DEBUG_RUN_ID","111");_putenv_s("NXEMU_DEBUG_GENERATION","1");
 std::array<uint8_t,32> native{};std::array<uint8_t,128> header{};std::array<uint8_t,80> parsed{};std::array<uint8_t,0x3800> regs{};std::array<uint8_t,16> cbuf{};
 N::Input in{};in.token=1;in.program=0x9999;in.stage=0;in.header_size=128;in.error_offset=16;in.opcode=0x9c3;in.words={0x000479c3,0,0x1e00,0xe7000};
 for(size_t n=0;n<4;++n)for(size_t b=0;b<4;++b)native[16+n*4+b]=uint8_t(in.words[n]>>(8*b));
 in.native=native;in.header=header;in.regs=regs;in.cbuf=cbuf;in.parsed_header=parsed;in.header_fully_mapped=true;in.cbuf_address=0x1000;in.cbuf_declared_size=32;in.cbuf_enabled=true;in.cbuf_fully_mapped=true;in.reason="native special register";
 in.invocation=MakeDirect(state,false,1);auto bad=in;bad.invocation.kind=Kind(99);{Scope scope(bad.invocation);bad.invocation=Current();assert(!N::Valid(bad)&&!N::Capture(bad));}assert(N::registry.reserved==0);
 if(mode=="index_capture"){
  state.index_buffer.first=0;state.index_buffer.count=3;state.base_index=0;state.index_buffer.start_addr_high=0;state.index_buffer.start_addr_low=0x1000;
  in.invocation=MakeDirect(state,true,1);in.index_input=Index();assert(N::Valid(in));auto invalid=in;invalid.index_input.prefix[6]=1;assert(!N::Valid(invalid)&&!N::Capture(invalid));
  assert(N::Capture(in));const auto original=in.index_input;auto changed=original;changed.prefix[4]=3;auto moved=original;moved.address=0x2000;moved.limit_address=0x2005;auto unmapped=original;unmapped.mapped=false;unmapped.prefix={};
  for(auto v:{changed,moved,unmapped}){in.index_input=v;++in.token;assert(N::Valid(in)&&N::Capture(in));assert(!N::Same(N::registry.entries[0],in,N::registry.entries[0].bytes));}
  in.index_input=original;assert(N::Same(N::registry.entries[0],in,N::registry.entries[0].bytes));assert(N::registry.ready==4&&N::registry.duplicates==0&&N::registry.invalid==2);std::cout<<N::CountersJson()<<'\n';return 0;
 }
 assert(N::Capture(in));const auto a=in.invocation;
 if(mode=="capture_duplicate"){in.token=2;assert(!N::Capture(in));assert(N::registry.ready==1&&N::registry.duplicates==1);}
 else{
  const std::array variants{MakeDirect(state,false,2),MakeDirect(state,true,1),MakeIndirect(state,params)};
  for(auto v:variants){in.invocation=v;++in.token;assert(N::Capture(in));assert(!N::Same(N::registry.entries[0],in,N::registry.entries[0].bytes));}
  in.invocation=a;assert(N::Same(N::registry.entries[0],in,N::registry.entries[0].bytes));
  assert(N::registry.ready==4&&N::registry.duplicates==0&&N::registry.reserved==4);
 }
 assert(N::registry.invalid==1);std::cout<<N::CountersJson()<<'\n';return 0;
}else if(mode=="index_bounds"){
 namespace N=VideoCommon::NativeDrawInputs;auto good=Index();assert(N::ValidIndex(good));unsigned rejected{};
 auto bad=[&](auto change){auto i=good;change(i);assert(!N::ValidIndex(i));++rejected;};
 for(unsigned size:{0U,3U,8U})bad([&](auto& i){i.element_size=size;});
 bad([](auto& i){i.bytes=17;});bad([](auto& i){i.bytes=5;});bad([](auto& i){i.prefix[6]=1;});bad([](auto& i){i.range_valid=false;});bad([](auto& i){i.mapped=false;});bad([](auto& i){i.address=0;});bad([](auto& i){i.address=0x1006;});bad([](auto& i){i.limit_address=0x1004;});bad([](auto& i){i.address=UINT64_MAX-2;i.limit_address=UINT64_MAX;});bad([](auto& i){i.address=UINT64_MAX-2;i.limit_address=0;});bad([](auto& i){i.count=UINT32_MAX;i.element_size=4;i.bytes=0;});bad([](auto& i){i.requested=false;});bad([](auto& i){i.count=0;i.bytes=0;i.prefix={};});
 auto unknown=good;unknown.limit_address=0;assert(N::ValidIndex(unknown));assert(N::IndexJson(unknown).find("\"limit_address\":0")!=std::string::npos);auto i=good;i.mapped=false;i.prefix={};assert(N::ValidIndex(i));assert(N::IndexJson(i).find("\"prefix\":[]")!=std::string::npos);i.range_valid=false;assert(N::ValidIndex(i));i=good;i.count=UINT32_MAX;i.element_size=4;i.bytes=16;i.limit_address=i.address+15;assert(N::ValidIndex(i));assert(N::ValidIndex(N::IndexPrefix{}));assert(rejected==16);std::cout<<"{\"passed\":true,\"index_rejections\":"<<rejected<<",\"bounded_bytes\":16,\"GPU_executed\":false}\n";return 0;
}else if(mode=="off"||mode=="fixtures_off"||mode=="default_unset"||mode=="startup_off"){
 assert(!Enabled());if(mode=="startup_off"){_putenv_s("NXEMU_NATIVE_DRAW_INPUTS","1");_putenv_s("NXEMU_NATIVE_SHADER_FIXTURES","1");assert(!Enabled());}auto a=MakeDirect(state,true,2);assert(a==Snapshot{});assert(MakeIndirect(state,params)==Snapshot{});Snapshot raw{};raw.available=true;raw.kind=Kind::Direct;raw.instance_count_available=true;Scope scope(raw);assert(!Current().available);
}else if(mode=="startup"){
 assert(Enabled());_putenv_s("NXEMU_NATIVE_DRAW_INPUTS","0");_putenv_s("NXEMU_NATIVE_SHADER_FIXTURES","0");assert(Enabled());Scope scope(MakeDirect(state,false,2));assert(Current().available);
}else if(mode=="mapping"){
 auto s=MakeDirect(state,false,2);assert(Valid(s)&&s.available&&s.kind==Kind::Direct&&!s.is_indexed&&s.instance_count_available&&s.instance_count==2);assert(s.retained_draw_indexed&&s.retained_instance_count==99&&s.topology==uint32_t(state.topology)&&s.draw_mode==0&&s.vertex_first==12&&s.vertex_count==3&&s.index_first==4&&s.index_count==6&&s.base_index==0xfffffff8&&s.base_instance==7&&s.index_format==1&&s.index_address==0x1234567887654321ULL);Scope scope(s);std::cout<<Json(Current())<<'\n';return 0;
}else if(mode=="nested"){
 auto a=MakeDirect(state,false,1),b=MakeDirect(state,true,2);{Scope outer(a);assert(Current()==a);{Scope inner(b);assert(Current()==b);}assert(Current()==a);{Snapshot bad=b;bad.kind=Kind(3);Scope inner(bad);assert(Current()==bad&&!Valid(Current()));}assert(Current()==a);}assert(!Current().available);
}else if(mode=="exception"){
 auto a=MakeDirect(state,false,1);{Scope outer(a);try{Scope inner(MakeDirect(state,true,2));throw 123;}catch(int){}assert(Current()==a);}assert(!Current().available);
}else if(mode=="copy"){
 auto a=MakeDirect(state,false,1);Scope scope(a);auto copy=Current();copy.vertex_first=99;assert(Current().vertex_first==12);a.vertex_first=55;assert(Current().vertex_first==12);
}else if(mode=="threads"){
 auto a=MakeDirect(state,false,1);{Scope outer(a);std::vector<std::thread> threads;for(unsigned n=0;n<8;++n)threads.emplace_back([&,n]{assert(!Current().available);auto s=MakeDirect(state,n%2,n+1);{Scope scope(s);assert(Current()==s);}assert(!Current().available);});for(auto& t:threads)t.join();assert(Current()==a);}assert(!Current().available);
}else if(mode=="bounds"){
 auto state_bad=state;state_bad.draw_mode=DM::DrawMode(99);assert(!Valid(MakeDirect(state_bad,false,1))&&!Valid(MakeIndirect(state_bad,params)));auto s=MakeDirect(state,false,1);auto bad=[&](auto mutate){auto x=s;mutate(x);assert(!Valid(x));assert(Json(x).find("\"invalid\":true")!=std::string::npos);};bad([](auto& x){x.kind=Kind(3);});bad([](auto& x){x.draw_mode=3;});bad([](auto& x){x.index_format=3;});bad([](auto& x){x.inline_prefix_size=17;});bad([](auto& x){x.inline_bytes_size=1;});bad([](auto& x){x.inline_prefix[15]=1;});bad([](auto& x){x.instance_count_available=false;});bad([](auto& x){x.indirect_address=1;});bad([](auto& x){x.available=false;});assert(Valid(Snapshot{}));auto i=MakeIndirect(state,params);i.instance_count=1;assert(!Valid(i));i=MakeIndirect(state,params);i.instance_count_available=true;assert(!Valid(i));
}else if(mode=="inline"){
 for(unsigned size:{0U,1U,15U,16U,17U,100000U}){state.inline_index_draw_indexes.resize(size);for(unsigned n=0;n<size;++n)state.inline_index_draw_indexes[n]=uint8_t(n+1);state.draw_mode=DM::DrawMode::InlineIndex;auto s=MakeDirect(state,true,3);assert(Valid(s)&&s.inline_bytes_size==size&&s.inline_prefix_size==std::min(size,16U));for(unsigned n=0;n<s.inline_prefix_size;++n)assert(s.inline_prefix[n]==uint8_t(n+1));}
 auto s=MakeDirect(state,true,3);std::cout<<Json(s)<<'\n';return 0;
}else if(mode=="indirect"){
 auto s=MakeIndirect(state,params);assert(Valid(s)&&s.kind==Kind::Indirect&&s.is_indexed&&!s.instance_count_available&&s.instance_count==0&&s.retained_instance_count==99&&s.include_count&&!s.is_byte_count&&s.count_address==0x55&&s.indirect_address==0x66&&s.buffer_size==100&&s.max_draw_count==8&&s.stride==20&&s.instance_mask==0xffff);std::cout<<Json(s)<<'\n';return 0;
}else if(mode=="different"){
 state.inline_index_draw_indexes={1,2,3,4};auto a=MakeDirect(state,false,1);auto b=MakeDirect(state,true,1);auto c=MakeDirect(state,false,2);auto d=MakeIndirect(state,params);state.vertex_buffer.first++;auto e=MakeDirect(state,false,1);assert(a.inline_prefix==b.inline_prefix&&a.inline_prefix==c.inline_prefix&&a.inline_prefix==d.inline_prefix&&a.inline_prefix==e.inline_prefix);assert(a!=b&&a!=c&&a!=d&&a!=e);assert(Json(a)!=Json(b)&&Json(a)!=Json(c)&&Json(a)!=Json(d)&&Json(a)!=Json(e));
}else assert(false);
std::cout<<"{\"passed\":true,\"actual_header\":true,\"actual_DrawManager_State\":true,\"GPU_executed\":false}\n";
}
'''
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--source-root',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();R=a.source_root.resolve();O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
 cpp=O/'invocation-test.cpp';cpp.write_text(CPP);exe=O/'invocation-test.exe'
 compiler=['cl','/nologo','/std:c++20','/EHsc','/MT','/O2','/utf-8','/I'+str(R/'src'),'/I'+str(R/'external/boost'),'/I'+str(R/'external/fmt/include'),str(cpp),'/Fe:'+str(exe),'/Fo:'+str(O/'invocation-test.obj')]
 cmd=O/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(compiler)+'\n')
 bound=[R/'src/yuzu_video_core/native_draw_invocation.h',R/'src/yuzu_video_core/native_draw_input_fixture.h',R/'src/yuzu_common/debug_document.h',R/'src/yuzu_video_core/engines/draw_manager.h',R/'src/yuzu_video_core/engines/maxwell_3d.h',Path(__file__).resolve(),cpp];hashes={str(p):sha(p) for p in bound}
 report={'command':subprocess.list2cmdline([sys.executable,'-B',Path(__file__),*sys.argv[1:]]),'compiler_command':subprocess.list2cmdline(compiler),'input':'actual helper/DrawManager State; synthetic retained CPU state; TLS/threads; no GPU reads','hashes':hashes}
 try:
  b=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=120);(O/'build.log').write_text(b.stdout+b.stderr);report.update(stage='compile',literal_stdout=b.stdout,literal_stderr=b.stderr,exit_status=b.returncode);assert b.returncode==0
  rows=[]
  for mode in ['off','fixtures_off','default_unset','startup_off','startup','mapping','nested','exception','copy','threads','bounds','inline','indirect','different','capture','capture_duplicate','index_bounds','index_capture']:
   env=dict(os.environ);env['NXEMU_NATIVE_DRAW_INPUTS']='0' if mode in ['off','startup_off'] else '1';env['NXEMU_NATIVE_SHADER_FIXTURES']='0' if mode=='fixtures_off' else '1';
   if mode=='default_unset':env.pop('NXEMU_NATIVE_DRAW_INPUTS',None);env.pop('NXEMU_NATIVE_SHADER_FIXTURES',None)
   command=[str(exe),mode,str(O/mode)];r=subprocess.run(command,env=env,capture_output=True,text=True,timeout=30)
   row={'command':subprocess.list2cmdline(command),'input':{'mode':mode,'NXEMU_NATIVE_DRAW_INPUTS':env.get('NXEMU_NATIVE_DRAW_INPUTS'),'NXEMU_NATIVE_SHADER_FIXTURES':env.get('NXEMU_NATIVE_SHADER_FIXTURES')},'literal_stdout':r.stdout,'literal_stderr':r.stderr,'exit_status':r.returncode};rows.append(row);report.update(stage='run',results=rows);assert r.returncode==0,row;row['parsed']=json.loads(r.stdout)
   if mode in ['mapping','inline','indirect']:
    s=row['parsed'];assert s['schema']==1 and s['available'] and isinstance(s['kind'],int) and not s['indexed_vertex_id_decoded'] and not s['indirect_arguments_decoded']
   if mode in ['capture','capture_duplicate','index_capture']:
    manifests=list((O/mode/'native-draw-inputs').glob('*.json'));assert len(manifests)==(4 if mode in ['capture','index_capture'] else 1)
    docs=[json.loads(f.read_text()) for f in manifests];assert all(d['invocation']['available'] and d['invocation']['schema']==1 for d in docs)
    assert all(d['files']==docs[0]['files'] or all(d['files'][role]['fnv1a64']==docs[0]['files'][role]['fnv1a64'] and d['files'][role]['bytes']==docs[0]['files'][role]['bytes'] for role in d['files']) for d in docs)
    if mode=='capture':
     identity={(d['invocation']['kind'],d['invocation']['is_indexed'],d['invocation']['instance_count']) for d in docs};assert identity=={(1,False,1),(1,False,2),(1,True,1),(2,True,0)}
    if mode=='index_capture':
     assert all(d['invocation']['kind']==1 and d['invocation']['is_indexed'] and d['invocation']['index_count']==3 and d['invocation']['base_index']==0 for d in docs)
     indexes=[d['index_input'] for d in docs];assert all(i['requested'] and i['range_valid'] and i['element_size']==2 and i['count']==3 and i['first']==0 and i['bytes_requested']==6 for i in indexes)
     assert len({json.dumps(i,sort_keys=True) for i in indexes})==4
     assert any(i['prefix']==[0,0,1,0,2,0] for i in indexes) and any(i['prefix']==[0,0,1,0,3,0] for i in indexes) and any(not i['mapped'] and i['prefix']==[] for i in indexes)
    row['published_manifest_count']=len(manifests)
  assert all(sha(Path(p))==h for p,h in hashes.items())
  report.update(stage='complete',exit_status=0,results=rows,scenarios=len(rows),inputs_unchanged=True,actual_header=True,actual_DrawManager_State=True,GPU_executed=False,executable_sha256=sha(exe));(O/'RESULT.json').write_text(json.dumps(report,indent=2));print('NATIVE_DRAW_INVOCATION_PASS; actual_header=True; actual_DrawManager_State=True; scenarios=18; TLS_nested_threads=True; bounded_prefix16=True; GPU=False')
 except BaseException:
  report['failure']=traceback.format_exc();(O/'FAILURE.json').write_text(json.dumps(report,indent=2));raise
if __name__=='__main__':main()
