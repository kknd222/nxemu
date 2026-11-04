from pathlib import Path
import json,subprocess,sys,hashlib
import argparse
parser=argparse.ArgumentParser(description="Actual frontend generation methods and shader-registry lifecycle regression; synthetic workers, no GPU")
parser.add_argument('--source-root',type=Path,required=True)
parser.add_argument('--out',type=Path,required=True)
parser.add_argument('--expect',choices=['baseline','modified'],required=True)
parser.add_argument('--label',default='test')
a=parser.parse_args()
P=Path(__file__).resolve().parents[2];E=a.out.resolve();E.mkdir(parents=True,exist_ok=True)
root=a.source_root.resolve();mode=a.label
s=(root/'repo/src/nxemu/ui/emulation/emulator_session.cpp').read_text()
def function(name):
 start=s.index('void EmulatorSession::'+name+'()');left=s.index('{',start);depth=1;i=left+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]
modified='void EmulatorSession::AdvanceModuleGeneration()' in s
assert modified==(a.expect=='modified'), 'Source does not match expected generation behavior'
if modified:
 for name,end in [('Initialise','FlushSettings'),('LoadProgram','PrepareLaunch'),('ResetModules','AdvanceModuleGeneration')]:
  start=s.index('EmulatorSession::'+name+'(');body=s[start:s.index('EmulatorSession::'+end+'(',start)]
  target='m_modules.StartLaunch(' if name=='LoadProgram' else 'm_modules.Setup('
  assert body.index('AdvanceModuleGeneration();')<body.index(target)
 assert 'AdvanceModuleGeneration' not in s[s.index('void EmulatorSession::FinishLaunch()'):s.index('void EmulatorSession::AbortLaunch(')]
 assert '++m_moduleGeneration' not in function('RegisterSystemCallbacks')
body=r'''
#include "yuzu_video_core/shader_progress_trace.h"
#include "yuzu_common/debug_document.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
using namespace VideoCommon::ShaderProgress;
namespace VideoCommon::ShaderProgress {
bool Enabled(){return true;}
std::string RunId(){return Common::DebugDocument::Environment("NXEMU_DEBUG_RUN_ID");}
std::string Generation(){return Common::DebugDocument::Environment("NXEMU_DEBUG_GENERATION");}
void Publish() noexcept {}
}
struct IOperatingSystem {
 void* execute{};void* exit{};
 void RegisterExecuteProgramCallback(void(*)(unsigned,void*),void* p){execute=p;}
 void RegisterExitCallback(void(*)(void*),void* p){exit=p;}
};
struct Modules {IOperatingSystem os;Modules& ModulesRef(){return *this;}IOperatingSystem& OperatingSystem(){return os;}};
struct ModuleOwner {::Modules value;::Modules& Modules(){return value;}};
struct EmulatorSession {
 struct CallbackContext {EmulatorSession* session;uint64_t generation;};
 uint64_t m_moduleGeneration=2;ModuleOwner m_modules;std::unique_ptr<CallbackContext> m_callbackContext;
 static void ExecuteProgramCallback(unsigned,void*){}static void ExitProgramCallback(void*){}
 void RegisterSystemCallbacks();
 ADVANCE_DECL
};
FUNCTIONS
int main(){
 _putenv_s("NXEMU_DEBUG_RUN_ID","generation-test");_putenv_s("NXEMU_DEBUG_GENERATION","2");
 EmulatorSession session;
 BEFORE_WORKER
 std::array<Token,10> tokens;std::vector<std::thread> workers;
 for(unsigned i=0;i<10;++i)workers.emplace_back([&,i]{tokens[i]=Begin(100+i);});
 for(auto& w:workers)w.join();
 auto initial=Take();assert(initial&&initial->begun==10);const auto epoch=initial->epoch;
 session.RegisterSystemCallbacks();assert(session.m_moduleGeneration==3);
 assert(session.m_modules.value.os.execute==session.m_callbackContext.get());
 assert(session.m_modules.value.os.exit==session.m_callbackContext.get());
 _putenv_s("NXEMU_DEBUG_GENERATION",std::to_string(session.m_moduleGeneration).c_str());
 auto after=Take();assert(after);
 for(auto& token:tokens)Finish(token,State::Completed);
 after=Take();assert(after);
 VERIFY
}
'''
if modified:
 body=body.replace('ADVANCE_DECL','void AdvanceModuleGeneration();').replace('FUNCTIONS',function('AdvanceModuleGeneration')+'\n'+function('RegisterSystemCallbacks')).replace('BEFORE_WORKER','session.AdvanceModuleGeneration();')
 body=body.replace('VERIFY',r'''
 assert(after->epoch==epoch&&after->begun==10&&after->completed==10&&after->mixed_session_drops==0&&after->session_discarded==0);
 // Same source publication with bridge disabled never creates an environment entry.
 _putenv_s("NXEMU_DEBUG_RUN_ID","");_putenv_s("NXEMU_DEBUG_GENERATION","");session.AdvanceModuleGeneration();assert(Generation().empty());
 // Lifecycle retirement remains intentional: new identity cannot accept old token updates.
 _putenv_s("NXEMU_DEBUG_RUN_ID","generation-test");session.AdvanceModuleGeneration();auto fresh=Begin(200);assert(fresh);auto p=Take();assert(p&&p->epoch!=epoch&&p->begun==1);Finish(tokens[0],State::Exception);Finish(fresh,State::Completed);p=Take();assert(p->completed==1&&p->mixed_session_drops==1);
 std::cout<<"MODIFIED_PASS; prepare_running_same_epoch=True; begun10_completed10=True; mixed0=True; callback_identity=True; bridge_off=True; stale_token_rejected=True\n";
''')
else:
 body=body.replace('ADVANCE_DECL','').replace('FUNCTIONS',function('RegisterSystemCallbacks')).replace('BEFORE_WORKER','')
 body=body.replace('VERIFY',r'''
 assert(after->epoch!=epoch&&after->begun==0&&after->completed==0&&after->mixed_session_drops==10&&after->session_discarded==10);
 std::cout<<"BASELINE_REPRODUCED; epoch_changed=True; discarded10=True; mixed10=True; begun0_completed0=True\n";
''')
cpp=E/(mode+'-test.cpp');cpp.write_text(body);exe=E/(mode+'-test.exe')
cmd=E/(mode+'-compile.cmd');cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\ncl /nologo /std:c++20 /EHsc /MD /O2 /I"'+str(P/'repo/src')+'" "'+str(cpp)+'" /Fe:"'+str(exe)+'" /Fo:"'+str(E/(mode+'-test.obj'))+'"\n')
r=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True);assert r.returncode==0,r.stdout+r.stderr
r=subprocess.run([str(exe)],capture_output=True,text=True);assert r.returncode==0,r.stdout+r.stderr
result=dict(command=subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),input='actual extracted frontend generation/callback methods + production shader registry; synthetic10 concurrent worker tasks, noGPU',literal_stdout=r.stdout,literal_stderr=r.stderr,exit_status=r.returncode,source_sha256=hashlib.sha256(s.encode()).hexdigest(),compiler_command=str(cmd),compiler_stdout='compiled successfully')
(E/(mode.upper()+'_RESULT.json')).write_text(json.dumps(result,indent=2));print(r.stdout,end='')
