"""Actual GuestDebugTrace header regression; CPU diagnostic registry, not guest execution."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess, sys

CPP = r'''
#include "nxemu-os/core/guest_debug_trace.h"
#include <cassert>
#include <iostream>
namespace T=Core::GuestDebugTrace;
int main(int argc,char** argv) {
 bool on=std::string(argv[1])=="on";
 assert(T::Enabled()==on);
 std::array<uint32_t,12> words{0x83,2,2,0,0x1501d9};
 uint64_t first=0;
 for(unsigned i=0;i<12;++i) {
  T::Event e;e.kind="ipc";e.port="nx2:gsv";e.command=115;e.tipc=true;e.words=words;
  auto token=T::Begin(e);if(!i)first=token;
  T::End(token,words.data());
#ifdef HAS_PUBLICATION
  assert(T::NeedsPublishedTls(token)==(on&&i<8));
  std::array<uint32_t,12> built{0,1,0},tls{0,1,0,0,0x1501d9};
  T::PublishedTls(token,built.data(),3,tls.data(),i!=1);
  assert(!T::NeedsPublishedTls(token));
#endif
 }
 for(unsigned i=0;i<500;++i) {T::Event e;e.kind="ipc";e.port="hid";e.command=213;auto token=T::Begin(e);T::End(token);}
 if(!on)assert(T::registry.completed==0);
#ifdef HAS_PUBLICATION
 if(on) {
  assert(T::registry.gsv115_query_count==8&&T::registry.gsv115_query_omitted==4);
  auto& e=T::registry.gsv115_queries[0];assert(e.token==first&&e.reply_published&&e.published_word_count==3);
  assert(e.reply[4]==0&&e.published_tls[4]==0x1501d9&&e.published_tls_readable);
  assert(!T::registry.gsv115_queries[1].published_tls_readable&&T::registry.gsv115_queries[1].published_tls[4]==0);
  std::array<uint32_t,12> replacement{};T::PublishedTls(first,replacement.data(),12,replacement.data(),true);
  assert(e.published_word_count==3&&e.published_tls[4]==0x1501d9);
  assert(!T::NeedsPublishedTls(0)&&!T::NeedsPublishedTls(~uint64_t{}));
 }
#endif
 T::Publish();
#ifdef HAS_PUBLICATION
 std::cout<<"REPLY_RETENTION_PASS enabled="<<on<<" retained="<<(on?8:0)<<" omitted="<<(on?4:0)<<" published_words=3 stale_tls_preserved=1 CPU_ONLY=1\n";
#else
 std::cout<<"BASELINE_RETENTION_ABSENT enabled="<<on<<" recent_capacity=32 CPU_ONLY=1\n";
#endif
}
'''

def main():
 p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 root=a.source_root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
 header=root/'src/nxemu-os/core/guest_debug_trace.h';new='NeedsPublishedTls' in header.read_text()
 cpp=out/'test.cpp';cpp.write_text(CPP);exe=out/'test.exe'
 cmd=['cl','/nologo','/std:c++20','/EHsc','/MT','/O2','/utf-8','/I'+str(root/'src'),str(cpp),'/Fe:'+str(exe),'/Fo:'+str(out/'test.obj')]
 if new:cmd.insert(1,'/DHAS_PUBLICATION')
 build=out/'build.cmd';build.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'+subprocess.list2cmdline(cmd)+'\n')
 b=subprocess.run(['cmd','/c',str(build)],capture_output=True,text=True,timeout=120);(out/'build.log').write_text(b.stdout+b.stderr);assert b.returncode==0,b.stdout+b.stderr
 results=[]
 for mode in ['off','on']:
  env=dict(os.environ);env['NXEMU_DEBUG_DETAIL']='1' if mode=='on' else '0';env['NXEMU_NATIVE_SHADER_DIAG_DIR']=str(out/mode)
  r=subprocess.run([str(exe),mode],env=env,capture_output=True,text=True,timeout=15)
  results.append(dict(command=subprocess.list2cmdline([str(exe),mode]),input={'NXEMU_DEBUG_DETAIL':env['NXEMU_DEBUG_DETAIL'],'NXEMU_NATIVE_SHADER_DIAG_DIR':env['NXEMU_NATIVE_SHADER_DIAG_DIR']},stdout=r.stdout,stderr=r.stderr,exit_status=r.returncode));assert r.returncode==0,results[-1]
  if mode=='off':assert not (out/mode).exists()
  else:
   docs=list((out/mode).glob('*.json'));assert docs
   d=json.loads(docs[0].read_text());payload=d.get('data',d)
   if isinstance(payload,str):payload=json.loads(payload)
   if new:
    assert len(payload['gsv115_query_first'])==8 and payload['gsv115_query_omitted']==4
    assert payload['gsv115_query_first'][0]['published_tls_words'][4]==0x1501d9
 (out/'RESULT.json').write_text(json.dumps(dict(actual_header=str(header),sha256=hashlib.sha256(header.read_bytes()).hexdigest(),retention_present=new,GPU=False,guest_execution=False,results=results),indent=2))
 for r in results:print(r['stdout'],end='')

if __name__=='__main__':main()
