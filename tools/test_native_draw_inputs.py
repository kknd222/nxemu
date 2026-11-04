"""Compile and execute the actual CPU draw-input capture header; no GPU execution."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,sys,traceback

CPP=r'''
#include "yuzu_video_core/native_draw_input_fixture.h"
#include <cassert>
#include <iostream>
#include <thread>
using namespace VideoCommon::NativeDrawInputs;
namespace fs=std::filesystem;
struct Data {
 std::array<uint8_t,32> native{};std::array<uint8_t,128> header{};
 std::array<uint8_t,80> parsed_header{};std::array<uint8_t,0x3800> regs{};std::array<uint8_t,16> cbuf{};
 Input in;
 Data(){in.token=1;in.program=0x9999;in.stage=0;in.header_size=128;in.error_offset=16;in.opcode=0x9c3;in.words={0x000479c3,0,0x1e00,0xe7000};for(size_t n=0;n<4;++n)for(size_t b=0;b<4;++b)native[16+n*4+b]=uint8_t(in.words[n]>>(8*b));
  in.native=native;in.header=header;in.regs=regs;in.cbuf=cbuf;in.cbuf_address=0x1000;in.cbuf_declared_size=32;in.cbuf_enabled=true;in.cbuf_fully_mapped=true;in.reason="native special register";in.parsed_header=parsed_header;in.header_fully_mapped=true;}
};
void set(const char* k,const std::string& v){assert(SetEnvironmentVariableA(k,v.c_str()));}
int main(int argc,char** argv){
 assert(argc==3);std::string mode=argv[1];fs::path root=argv[2];fs::create_directories(root);
 set("NXEMU_NATIVE_SHADER_DIAG_DIR",root.string());set("NXEMU_DEBUG_RUN_ID","111");set("NXEMU_DEBUG_GENERATION","1");
 Data d;
 if(mode=="off"||mode=="fixtures_off"){
  assert(!Enabled());assert(!Capture(d.in));assert(registry.attempts==0);assert(!fs::exists(root/"native-draw-inputs"));
 }else if(mode=="startup"){
  assert(Enabled());set("NXEMU_NATIVE_DRAW_INPUTS","0");set("NXEMU_NATIVE_SHADER_FIXTURES","0");assert(Enabled());assert(Capture(d.in));
 }else if(mode=="valid"){
  assert(Capture(d.in));assert(registry.ready==1);assert(registry.charged_raw_bytes==32+128+80+0x3800+16);
 }else if(mode=="dedup"){
  assert(Capture(d.in));d.in.token=2;assert(!Capture(d.in));assert(registry.duplicates==1&&registry.ready==1);
  d.in.stage=4;assert(Capture(d.in));assert(registry.ready==2); // stage is part of input identity
 }else if(mode=="budget"){
  for(unsigned i=0;i<4;++i){d.regs[0]=i;d.in.token=i+1;assert(Capture(d.in));}
  d.regs[0]=7;d.in.token=7;set("NXEMU_DEBUG_GENERATION","2");set("NXEMU_DEBUG_RUN_ID","222");assert(!Capture(d.in));
  assert(registry.ready==4&&registry.budget_drops==1&&registry.reserved==4);
 }else if(mode=="unmapped"){
  d.in.cbuf={};d.in.cbuf_fully_mapped=false;assert(Capture(d.in));assert(registry.ready==1);
 }else if(mode=="disabled_cbuf"){
  d.in.cbuf={};d.in.cbuf_enabled=false;d.in.cbuf_fully_mapped=false;d.in.cbuf_address=0;d.in.cbuf_declared_size=0;assert(Capture(d.in));
 }else if(mode=="header_unmapped"){
  d.in.header={};d.in.header_fully_mapped=false;assert(Capture(d.in));
 }else if(mode=="header_changed"){
  d.header[0]=1;assert(Capture(d.in));
 }else if(mode=="reservation"){
  auto first=Reserve();assert(first==1);auto wrong=d.in;wrong.token=0;assert(!Capture(wrong,first));assert(!Capture(d.in,first));
  assert(!Capture(d.in,0)&&!Capture(d.in,5));auto second=Reserve();assert(second==2);assert(Capture(d.in,second));
  assert(registry.invalid_reservation==3&&registry.claimed_count==2&&registry.reserved_count==2&&registry.ready==1);
 }else if(mode=="bounded_reads"){
  unsigned reads{};for(unsigned n=0;n<100;++n){const auto reservation=Reserve();if(!reservation)continue;++reads;Capture(d.in,reservation);}
  assert(reads==4&&registry.budget_drops==96&&registry.ready==1&&registry.duplicates==3);
 }else if(mode=="reservation_reload"){
  auto first=Reserve();assert(first==1);set("NXEMU_DEBUG_GENERATION","2");assert(!Capture(d.in,first));set("NXEMU_DEBUG_GENERATION","1");
  auto second=Reserve();assert(second==2);assert(Capture(d.in,second));assert(registry.invalid==1&&registry.ready==1);
 }else if(mode=="invalid"){
  auto invalid=[&](auto change){auto in=d.in;change(in);assert(!Capture(in));};
  invalid([](Input& i){i.token=0;});invalid([](Input& i){i.program=0;});invalid([](Input& i){i.stage=3;});invalid([](Input& i){i.header_size=64;});
  invalid([](Input& i){i.native={};});invalid([](Input& i){i.native=i.native.first(31);});invalid([](Input& i){i.header=i.header.first(127);});invalid([](Input& i){i.regs=i.regs.first(0x37ff);});invalid([](Input& i){i.parsed_header=i.parsed_header.first(79);});
  invalid([](Input& i){i.error_offset=1;});invalid([](Input& i){i.error_offset=32;});invalid([](Input& i){i.words[0]^=1;});invalid([](Input& i){i.opcode=0x9c2;});
  invalid([](Input& i){i.cbuf_bank=18;});invalid([](Input& i){i.cbuf_declared_size=15;});invalid([](Input& i){i.cbuf_offset=33;});invalid([](Input& i){i.cbuf_enabled=false;});
  invalid([](Input& i){i.cbuf_fully_mapped=false;});invalid([](Input& i){i.cbuf_address=0;});
  std::array<uint8_t,1025> big{};invalid([&](Input& i){i.cbuf=big;i.cbuf_declared_size=2000;});
  std::array<uint8_t,4112> big_native{};invalid([&](Input& i){i.native=big_native;});
  set("NXEMU_DEBUG_RUN_ID","..");assert(!Capture(d.in));set("NXEMU_DEBUG_RUN_ID","111");set("NXEMU_DEBUG_GENERATION","");assert(!Capture(d.in));set("NXEMU_DEBUG_GENERATION","1");
  set("NXEMU_NATIVE_SHADER_DIAG_DIR","relative-path");assert(!Capture(d.in));set("NXEMU_NATIVE_SHADER_DIAG_DIR",(root/"absent").string());assert(!Capture(d.in));
  assert(registry.invalid==25&&registry.ready==0&&registry.reserved==0);
 }else if(mode=="io"){
  auto directory=root/"native-draw-inputs";fs::create_directory(directory);
  auto blocked=directory/"draw-input-111-1-1-0.native.bin";std::ofstream f(blocked,std::ios::binary);f<<"original";f.close();
  assert(!Capture(d.in));assert(registry.io_errors==1&&registry.reserved==1&&registry.ready==0);
  assert(!fs::exists(directory/"draw-input-111-1-1-0.json"));assert(fs::file_size(blocked)==8);
  d.regs[0]=1;d.in.token=2;assert(Capture(d.in));assert(registry.ready==1&&registry.reserved==2);
 }else if(mode=="directory_io"){
  std::ofstream(root/"native-draw-inputs")<<"blocked";assert(!Capture(d.in));assert(registry.io_errors==1&&registry.ready==0);
 }else if(mode=="threads"){
  std::atomic<unsigned> ready{},go{};std::vector<std::thread> threads;
  for(unsigned i=0;i<8;++i)threads.emplace_back([&,i]{Data x;x.in.token=i+1;x.regs[0]=i;++ready;while(!go.load())std::this_thread::yield();for(unsigned j=0;j<20;++j)Capture(x.in);});
  while(ready!=8)std::this_thread::yield();go=1;for(auto& t:threads)t.join();
  assert(registry.ready<=4&&registry.ready>0&&registry.reserved<=4&&registry.attempts==160);assert(registry.charged_raw_bytes<=4*MaxRawBytes);
 }else assert(false);
 std::cout<<CountersJson()<<"\n";
}
'''

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def fnv(data):
 h=14695981039346656037
 for b in data:h=((h^b)*1099511628211)&0xffffffffffffffff
 return f'{h:016x}'
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--source-root',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();root=a.source_root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
 cpp=out/'native-input-test.cpp';cpp.write_text(CPP);exe=out/'native-input-test.exe'
 compiler=['cl','/nologo','/std:c++20','/EHsc','/MT','/O2','/utf-8','/I'+str(root/'src'),str(cpp),'/Fe:'+str(exe),'/Fo:'+str(out/'native-input-test.obj')]
 cmd=out/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(compiler)+'\n')
 bound=[root/'src/yuzu_video_core/native_draw_input_fixture.h',root/'src/yuzu_common/debug_document.h',Path(__file__).resolve(),cpp];hashes={str(f):sha(f) for f in bound}
 report=dict(command=subprocess.list2cmdline([sys.executable,'-B',Path(__file__),*sys.argv[1:]]),input='actual CPU capture header; synthetic native/header/regs/cbuf; filesystem and threads; no GPU',hashes=hashes,compiler_command=subprocess.list2cmdline(compiler))
 try:
  b=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=120);(out/'build.log').write_text(b.stdout+b.stderr);report.update(stage='compile',literal_stdout=b.stdout,literal_stderr=b.stderr,exit_status=b.returncode);assert b.returncode==0
  results=[]
  for mode in ['off','fixtures_off','startup','valid','dedup','budget','unmapped','disabled_cbuf','header_unmapped','header_changed','reservation','bounded_reads','reservation_reload','invalid','io','directory_io','threads']:
   env=dict(os.environ);env['NXEMU_NATIVE_DRAW_INPUTS']='0' if mode=='off' else '1';env['NXEMU_NATIVE_SHADER_FIXTURES']='0' if mode=='fixtures_off' else '1'
   command=[str(exe),mode,str(out/mode)];r=subprocess.run(command,capture_output=True,text=True,env=env,timeout=60)
   row=dict(command=subprocess.list2cmdline(command),input={'mode':mode,'NXEMU_NATIVE_DRAW_INPUTS':env['NXEMU_NATIVE_DRAW_INPUTS'],'NXEMU_NATIVE_SHADER_FIXTURES':env['NXEMU_NATIVE_SHADER_FIXTURES']},literal_stdout=r.stdout,literal_stderr=r.stderr,exit_status=r.returncode);results.append(row);assert r.returncode==0,row
   row['counters']=json.loads(r.stdout)
   for f in (out/mode/'native-draw-inputs').glob('*.json'):
    d=json.loads(f.read_text());assert d['schema']==1 and d['status']=='ready' and d['token']>0
    assert d['run_id']=='111' and d['generation']=='1'
    for role,x in d['files'].items():
     raw=(f.parent/x['file']).read_bytes();assert len(raw)==x['bytes'] and fnv(raw)==x['fnv1a64']
    if mode in ['unmapped','disabled_cbuf']:assert d['files']['cbuf']['bytes']==0 and not d['cbuf']['fully_mapped']
    assert d['files']['parsed_header']['bytes']==80
    if mode=='header_unmapped':assert d['files']['header']['bytes']==0 and d['parsed_header_matches_raw'] is None
    elif mode=='header_changed':assert d['parsed_header_matches_raw'] is False
    else:assert d['parsed_header_matches_raw'] is True
  assert all(sha(Path(f))==h for f,h in hashes.items()),'Source changed during test'
  report.update(stage='complete',exit_status=0,results=results,inputs_unchanged=True,actual_header=True,GPU_executed=False,scenarios=len(results),executable_sha256=sha(exe));(out/'RESULT.json').write_text(json.dumps(report,indent=2));print('NATIVE_DRAW_INPUTS_PASS; actual_header=True; scenarios17=True; threads8=True; bounds=True; immutable_files=True; GPU=False')
 except BaseException:
  report['failure']=traceback.format_exc();(out/'FAILURE.json').write_text(json.dumps(report,indent=2));raise
if __name__=='__main__':main()
