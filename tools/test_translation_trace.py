from pathlib import Path
import argparse,json,subprocess,sys
ap=argparse.ArgumentParser(description='Production optional translation trace regression; synthetic tasks, no GPU')
ap.add_argument('--out',type=Path,required=True);ap.add_argument('--mode',choices=['baseline','modified','rollback'],default='modified');ap.add_argument('--source-root',type=Path);ap.add_argument('--original-root',type=Path)
ap.add_argument('--layer',choices=['passes','substages'],default='passes')
a=ap.parse_args();P=Path(__file__).resolve().parents[2];E=a.out.resolve();E.mkdir(parents=True,exist_ok=True);root=(a.source_root or P).resolve()
translation=root/'repo/src/yuzu_shader_recompiler/frontend/maxwell/translate_program.cpp';s=translation.read_text() if translation.exists() else '';progress=(root/'repo/src/yuzu_video_core/shader_progress_trace.h').read_text()
if a.mode!='modified':
 if a.layer=='substages':
  assert 'translation_subpasses' not in progress and 'SubTimer' not in (root/'repo/src/yuzu_shader_recompiler/ir_opt/ssa_rewrite_pass.cpp').read_text()
  print('BASELINE_PASS; substages_absent=True; top_level_passes_preserved=True');raise SystemExit(0)
 assert 'TranslationTrace::Measure' not in s and 'translation_passes' not in progress
 print('BASELINE_PASS; per_pass_trace_absent=True; original_pipeline_unchanged=True');raise SystemExit(0)
assert 'translation_passes' in progress and 'SetTranslationTrace' in progress
if a.original_root and a.layer=='passes':
 transforms=json.loads((E/'STATEMENT_TRANSFORMS.json').read_text())
 restored=s
 for before,after in transforms:assert restored.count(after)==1;restored=restored.replace(after,before)
 restored=restored.replace('#include "yuzu_shader_recompiler/translation_trace.h"\n','').replace('    { TranslationTrace::Timer stage_setup(TranslationTrace::Pass::StageSetup);\n','')
 restored=restored.replace('    }\n    RemoveUnreachableBlocks(program);','    RemoveUnreachableBlocks(program);')
 assert restored==(a.original_root/'repo/src/yuzu_shader_recompiler/frontend/maxwell/translate_program.cpp').read_text(), 'Pass order/guards or original expressions changed'
cpp=E/'trace-test.cpp';cpp.write_text(r'''
#include "yuzu_video_core/shader_progress_trace.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <thread>
using namespace Shader::TranslationTrace;
namespace VideoCommon::ShaderProgress {
bool Enabled(){return true;}std::string RunId(){return "trace-test";}std::string Generation(){return "3";}void Publish() noexcept {}
}
int main(){
 static_assert(Count==22);Record outer,inner;assert(current==nullptr);
 int value=4;int calls=0;
 {Capture capture(&outer);
  auto p=Measure(Pass::BuildASL,[&]{++calls;return std::make_unique<int>(8);});assert(*p==8&&calls==1);
  int& ref=Measure(Pass::Position,[&]() -> int& {return value;});ref=9;assert(value==9);
  try {Measure(Pass::Texture,[]{throw 7;});assert(false);}catch(int e){assert(e==7);}
  {Capture nested(&inner);Measure(Pass::SsaRewrite,[]{});}assert(current==&outer);
  {Capture disabled(nullptr);Measure(Pass::BuildASL,[]{});}assert(current==&outer);
  std::thread isolated([&]{assert(current==nullptr);Capture capture(&inner);Measure(Pass::GlobalMemory,[]{});});isolated.join();
 }
 assert(current==nullptr);assert(outer.passes[0].calls==1&&outer.passes[0].completed==1);
 assert(outer.passes[static_cast<unsigned>(Pass::Texture)].exceptions==1);
 assert(inner.passes[static_cast<unsigned>(Pass::SsaRewrite)].calls==1&&inner.passes[static_cast<unsigned>(Pass::GlobalMemory)].calls==1);
 using namespace VideoCommon::ShaderProgress;
 auto token=Begin(233304474743316087);assert(token);SetTranslationTrace(token,outer);Finish(token,State::Completed);auto snapshot=Take();assert(snapshot);auto text=Json(*snapshot);assert(text.find("translation_passes")!=std::string::npos);assert(text.find("BuildASL")!=std::string::npos);assert(text.size()<98304);
 Snapshot full;full.run="trace-test";full.generation="3";
 for(unsigned i=0;i<64;++i){auto& row=full.active[i];row.token=token;row.token.id=i+1;row.translation_known=true;for(auto& p:row.translation.passes){p.calls=1;p.completed=1;p.wall_ns=100;}}
 for(unsigned i=0;i<32;++i){full.history[i]=full.active[i];full.history[i].ended_ns=1;}
 auto bounded=Json(full);assert(bounded.size()<98304);assert(bounded.find("\"snapshot_omitted_rows\":0")==std::string::npos);
 std::cout<<"MODIFIED_PASS; passes22=True; call_once=True; move_reference_preserved=True; exception_rethrown=True; TLS_nested_threads=True; disabled_restores=True; bounded_JSON=True; capacity_omissions_explicit=True\n";
}
''')
if a.layer=='substages':
 text=cpp.read_text();code=r''' static_assert(SubCount==4 && WorkCount==23);Record child;
 {Capture capture(&child);{SubTimer timer(Subpass::SSAVisit);Add(Work::SSABlocks,2);Max(Work::SSAPrefixMaxChecks,17);}
 try {SubTimer timer(Subpass::SSAFinalize);throw 8;}catch(int){}
 Add(Work::SSAPrefixChecks,UINT64_MAX);Add(Work::SSAPrefixChecks,1);}
 assert(child.subpasses[2].completed==1&&child.subpasses[3].exceptions==1);
 assert(child.work[0]==2&&child.work[8]==17&&child.work[7]==UINT64_MAX&&child.work_overflow);
 {Capture disabled(nullptr);Add(Work::SSABlocks);SubTimer timer(Subpass::BuildGoto);}assert(current==nullptr);
 std::cout<<"SUBSTAGE_PASS; timers4=True; work23=True; saturated_overflow=True; nested_parent_not_doublecounted=True; disabled_no_record=True\n";
'''
 text=text.replace(' std::cout<<"MODIFIED_PASS;',code+' std::cout<<"MODIFIED_PASS;');cpp.write_text(text)
exe=E/'trace-test.exe';cmd=E/'compile-test.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\ncl /nologo /std:c++20 /EHsc /MD /O2 /I"'+str(root/'repo/src')+'" /I"'+str(P/'repo/src')+'" "'+str(cpp)+'" /Fe:"'+str(exe)+'" /Fo:"'+str(E/'trace-test.obj')+'"\n')
r=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True);assert r.returncode==0,r.stdout+r.stderr
r=subprocess.run([str(exe)],capture_output=True,text=True);assert r.returncode==0,r.stdout+r.stderr
print(r.stdout,end='')
