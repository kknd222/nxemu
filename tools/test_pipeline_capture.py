"""Compile actual pipeline_fixture header, exercise bounded filesystem/concurrency paths.

Environment sink is synthetic; no GPU/driver/ISA execution or whole-game claim.
"""
from pathlib import Path
import argparse, hashlib, json, subprocess, sys

CPP = r'''
#include "yuzu_video_core/pipeline_fixture.h"
#include "yuzu_common/debug_document.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <vector>
using namespace VideoCommon::PipelineFixture;
std::unordered_map<std::string,std::string> env;
namespace VideoCommon::PipelineFixture {
std::string Environment(std::string_view n){auto it=env.find(std::string(n));return it==env.end()?"":it->second;}
std::mutex hook_mutex;std::condition_variable hook_cv;std::string hook_prefix;unsigned hook_entered{};bool hook_released{},hook_throw{};
void PipelineCaptureTestWriteHook(const std::filesystem::path& p){
 std::unique_lock lock(hook_mutex);if(p.extension()!=".spv"||hook_prefix.empty()||!p.filename().string().starts_with(hook_prefix))return;
 ++hook_entered;hook_cv.notify_all();hook_cv.wait(lock,[]{return hook_released;});if(hook_throw)throw std::runtime_error("controlled blob failure");
}
}
void gate(const std::string& prefix,bool fail=false){std::scoped_lock lock(hook_mutex);hook_prefix=prefix;hook_entered=0;hook_released=false;hook_throw=fail;}
void await(unsigned n){std::unique_lock lock(hook_mutex);assert(hook_cv.wait_for(lock,std::chrono::seconds(10),[&]{return hook_entered>=n;}));}
void release(){std::scoped_lock lock(hook_mutex);hook_released=true;hook_cv.notify_all();}
#if CAPTURE_AUDIT
std::uint64_t value(Outcome o);
#endif
void controlled(const std::filesystem::path& base,const std::vector<std::uint32_t>& code){
 env["NXEMU_DEBUG_RUN_ID"]="controlled-io";env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=(base/"controlled-io").string();
 gate("pipeline-"+Hex(70));std::shared_ptr<Handle> first;std::thread t([&]{first=Capture(70,2,code);});await(1);
 auto second=Capture(71,2,code);
#if CAPTURE_RESERVATION
 assert(second);
#else
 assert(!second);
#endif
 release();t.join();assert(first);Recipe(first,"{}");Recipe(second,"{}");first.reset();second.reset();
 // Keep four real4MiB writes in flight: fifth request must not oversubscribe.
 env["NXEMU_DEBUG_RUN_ID"]="controlled-bytes";env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=(base/"controlled-bytes").string();gate("pipeline-");
 std::vector<std::uint32_t> large(MaxBlobBytes/4,0);std::copy(code.begin(),code.end(),large.begin());
 std::array<std::shared_ptr<Handle>,4> held;std::vector<std::thread> threads;
 for(unsigned i=0;i<4;++i){
#if CAPTURE_RESERVATION
  threads.emplace_back([&,i]{held[i]=Capture(80+i,2,large);});await(i+1);
#else
  if(i==0){threads.emplace_back([&]{held[0]=Capture(80,2,large);});await(1);}else assert(!Capture(80+i,2,large));
#endif
 }
#if CAPTURE_RESERVATION
 const auto bytes=value(Outcome::ByteBudget);assert(!Capture(84,2,code));assert(value(Outcome::ByteBudget)==bytes+1);
 assert(current_budget->charged_bytes==MaxTotalBytes);
#else
 assert(!Capture(84,2,code));
#endif
 release();for(auto& thread:threads)thread.join();for(auto& h:held)Recipe(h,"{}");
 // Sixteen tiny blobs in flight independently enforce the case ceiling.
 env["NXEMU_DEBUG_RUN_ID"]="controlled-cases";env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=(base/"controlled-cases").string();gate("pipeline-");
 std::array<std::shared_ptr<Handle>,16> cases;std::vector<std::thread> case_threads;
 for(unsigned i=0;i<16;++i){
#if CAPTURE_RESERVATION
  case_threads.emplace_back([&,i]{cases[i]=Capture(100+i,2,code);});await(i+1);
#else
  if(i==0){case_threads.emplace_back([&]{cases[0]=Capture(100,2,code);});await(1);}else assert(!Capture(100+i,2,code));
#endif
 }
#if CAPTURE_RESERVATION
 const auto case_rejections=value(Outcome::CaseBudget);assert(!Capture(116,2,code));assert(value(Outcome::CaseBudget)==case_rejections+1);
#else
 assert(!Capture(116,2,code));
#endif
 release();for(auto& thread:case_threads)thread.join();for(auto& h:cases)Recipe(h,"{}");
 // Old run's failing blob must refund its OWN reservation, never the new run.
 env["NXEMU_DEBUG_RUN_ID"]="old-run";env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=(base/"old-run").string();gate("pipeline-"+Hex(90),true);
 std::shared_ptr<Handle> old;std::thread old_thread([&]{old=Capture(90,2,code);});await(1);
 env["NXEMU_DEBUG_RUN_ID"]="new-run";env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=(base/"new-run").string();auto newer=Capture(91,2,code);
#if CAPTURE_RESERVATION
 assert(newer);auto new_budget=current_budget;assert(new_budget->charged_bytes==code.size()*4);
#else
 assert(!newer);
#endif
 release();old_thread.join();assert(!old);
#if CAPTURE_RESERVATION
 assert(current_budget==new_budget&&new_budget->charged_bytes==code.size()*4);
 // Same identity reload retains chargedbytes; pending failure retains realblob.
 env["NXEMU_DEBUG_GENERATION"]="2";auto again=Capture(92,2,code);assert(again);assert(current_budget==new_budget&&new_budget->charged_bytes==2*code.size()*4);Recipe(again,"{}");env["NXEMU_DEBUG_GENERATION"]="1";
#else
 newer=Capture(91,2,code);assert(newer);
#endif
 Recipe(newer,"{}");
 gate("");
}
void verify(bool passed){assert(passed);}
#if CAPTURE_AUDIT
std::uint64_t value(Outcome o){return outcomes[static_cast<std::size_t>(o)].load();}
#define REASON(o, stmt) {auto before=value(Outcome::o);stmt;assert(value(Outcome::o)==before+1);}
#else
#define REASON(o, stmt) {stmt;}
#endif
int main(int argc,char** argv){
 assert(argc==2);const auto base=std::filesystem::path(argv[1]);std::filesystem::create_directories(base);
 env["NXEMU_DEBUG_RUN_ID"]="unit";env["NXEMU_DEBUG_GENERATION"]="1";
 std::vector<std::uint32_t> code{0x07230203,0x00010300,0,1,0};
 env["NXEMU_PIPELINE_FIXTURES"]="0";assert(!Capture(1,2,code));
#if CAPTURE_AUDIT
 assert(value(Outcome::Attempted)==0);
#endif
 env["NXEMU_PIPELINE_FIXTURES"]="1";
 REASON(MissingDirectory, assert(!Capture(1,2,code)));
 auto root=base/"normal";env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=root.string();
 env["NXEMU_PIPELINE_FIXTURE_KEY"]=Hex(99);REASON(Filtered, assert(!Capture(1,2,code)));env["NXEMU_PIPELINE_FIXTURE_KEY"]="";
 std::vector<std::uint32_t> invalid{1};REASON(InvalidCode, assert(!Capture(1,2,invalid)));
 env["NXEMU_DEBUG_GENERATION"]="bad";REASON(InvalidSession, assert(!Capture(1,2,code)));env["NXEMU_DEBUG_GENERATION"]="1";
 {
  std::unique_lock held(mutex);std::thread t([&]{REASON(LockContention, assert(!Capture(1,2,code)));});t.join();
 }
 auto ready=Capture(1,2,code);assert(ready);Recipe(ready,"{}");assert(std::filesystem::exists(ready->root/(ready->id+".json")));
 {
  std::ifstream f(ready->root/(ready->id+".spv"),std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(f)),{});
  assert(bytes.size()==code.size()*4&&std::memcmp(bytes.data(),code.data(),bytes.size())==0);
 }
 ready.reset();
 {auto h=Capture(2,3,code);assert(h);REASON(RecipeInvalid, Recipe(h,""));}
 {auto h=Capture(3,4,code);assert(h);auto q=h->root/(h->id+".json.tmp");std::filesystem::create_directory(q);REASON(RecipeWriteFailure, Recipe(h,"{}"));}
 {auto h=Capture(4,5,code);assert(h);assert(std::filesystem::exists(h->root/(h->id+".pending.json")));}
 auto bad=base/"directory-as-file";{std::ofstream f(bad);f<<'x';}env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=bad.string();
 REASON(DirectoryFailure, assert(!Capture(5,6,code)));
 env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=root.string();
 const auto expected=[&](std::uint64_t key,const char* suffix){return root/"pipeline-fixtures"/("pipeline-"+Hex(key)+"-"+Hex(ContentId(code))+"-g1-"+std::to_string(count+1)+suffix);};
 std::filesystem::create_directory(expected(6,".spv.tmp"));REASON(BlobFailure, assert(!Capture(6,7,code)));
 std::filesystem::create_directory(expected(7,".pending.json.tmp"));REASON(PendingFailure, assert(!Capture(7,8,code)));
#if CAPTURE_RESERVATION
 assert(current_budget->charged_bytes==5*code.size()*4); // Failed blobs refunded, pending failure kept its real blob.
#endif
 env["NXEMU_DEBUG_RUN_ID"]="case-budget";env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=(base/"case-budget").string();
 for(unsigned i=0;i<16;++i){auto h=Capture(10+i,2,code);assert(h);Recipe(h,"{}");}
 REASON(CaseBudget, assert(!Capture(26,2,code)));
 // Same run, new generation retains the original case budget.
 env["NXEMU_DEBUG_GENERATION"]="2";REASON(CaseBudget, assert(!Capture(27,2,code)));env["NXEMU_DEBUG_GENERATION"]="1";
 env["NXEMU_DEBUG_RUN_ID"]="byte-budget";env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=(base/"byte-budget").string();
 std::vector<std::uint32_t> large(MaxBlobBytes/4,0);std::copy(code.begin(),code.end(),large.begin());
 for(unsigned i=0;i<4;++i){auto h=Capture(30+i,2,large);assert(h);Recipe(h,"{}");}
 REASON(ByteBudget, assert(!Capture(34,2,code)));
 env["NXEMU_DEBUG_RUN_ID"]="stress";env["NXEMU_NATIVE_SHADER_DIAG_DIR"]=(base/"stress").string();
 std::atomic<unsigned> accepted{};const auto before_drops=drops.load();
 std::vector<std::thread> workers;for(unsigned i=0;i<8;++i)workers.emplace_back([&,i]{auto h=Capture(40+i,2,code);if(h){++accepted;Recipe(h,"{}");}});
 for(auto& t:workers)t.join();assert(accepted+drops.load()-before_drops==8);
#if CAPTURE_AUDIT
 assert(capture_active==0&&recipe_active==0);
 const auto rejected=value(Outcome::MissingDirectory)+value(Outcome::Filtered)+value(Outcome::InvalidCode)+value(Outcome::InvalidSession)+value(Outcome::LockContention)+value(Outcome::CaseBudget)+value(Outcome::ByteBudget)+value(Outcome::DirectoryFailure)+value(Outcome::BlobFailure)+value(Outcome::PendingFailure)+value(Outcome::CaptureException);
 assert(value(Outcome::Attempted)==rejected+value(Outcome::ReturnedHandle));
 assert(value(Outcome::ReleasedWithoutRecipe)==3);
#endif
 controlled(base,code);
 _putenv_s("NXEMU_NATIVE_SHADER_DIAG_DIR",(base/"publication").string().c_str());_putenv_s("NXEMU_DEBUG_DETAIL","0");_putenv_s("NXEMU_PIPELINE_FIXTURES","1");
#if CAPTURE_AUDIT
 assert(Common::DebugDocument::PublicationEnabled("pipeline-capture"));
 assert(!Common::DebugDocument::PublicationEnabled("texture-cache"));
 const auto payload=CountersJson();assert(payload.size()<8192);Common::DebugDocument::Publish("pipeline-capture",payload,false);
 assert(std::filesystem::exists(base/"publication/pipeline-capture.json"));
 _putenv_s("NXEMU_PIPELINE_FIXTURES","0");assert(!Common::DebugDocument::PublicationEnabled("pipeline-capture"));
 std::cout<<payload<<'\n';
#else
 assert(!Common::DebugDocument::PublicationEnabled("pipeline-capture"));std::cout<<"{\"baseline_no_classification\":true,\"stress_accepted\":"<<accepted<<"}\n";
#endif
}
'''

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--modified',action='store_true')
    ap.add_argument('--reservation',action='store_true')
    a=ap.parse_args();p=Path(__file__).resolve().parents[2];root=a.source_root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    cpp=out/'capture-test.cpp';cpp.write_text('#include <cstring>\n'+CPP)
    exe=out/'capture-test.exe';obj=out/'capture-test.obj'
    # Insert a test-only deterministic IO gate; production Write body remains intact.
    # This is explicitly instrumented source, not an unmodified-header timing claim.
    header=root/'repo/src/yuzu_video_core/pipeline_fixture.h'
    text=header.read_text(encoding='utf-8-sig');marker='inline void Write(const std::filesystem::path& path,const char* bytes,std::size_t size){'
    assert text.count(marker)==1
    shadow=out/'instrumented/yuzu_video_core/pipeline_fixture.h';shadow.parent.mkdir(parents=True,exist_ok=True)
    shadow.write_text(text.replace(marker,'void PipelineCaptureTestWriteHook(const std::filesystem::path&);\n'+marker+'\n PipelineCaptureTestWriteHook(path);'),encoding='utf-8')
    args=['cl','/nologo','/std:c++20','/EHsc','/MT','/O2',f'/DCAPTURE_AUDIT={int(a.modified)}',f'/DCAPTURE_RESERVATION={int(a.reservation)}','/I'+str(out/'instrumented'),'/I'+str(root/'repo/src'),'/I'+str(p/'repo/src'),str(cpp),'/Fe:'+str(exe),'/Fo:'+str(obj)]
    cmd=out/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'+subprocess.list2cmdline(args)+'\n')
    common=root/'repo/src/yuzu_common/debug_document.h'
    source=[header,common if common.exists() else p/'repo/src/yuzu_common/debug_document.h',shadow]
    hashes={str(q):hashlib.sha256(q.read_bytes()).hexdigest() for q in source}
    r=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True);(out/'build.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
    native=[str(exe),str(out/'files')];r=subprocess.run(native,capture_output=True,text=True,timeout=60)
    assert r.returncode==0,r.stdout+r.stderr
    snapshot=json.loads(r.stdout)
    parser=None
    if a.modified:
        from pipeline_cases import inspect_cases
        pub=out/'files/publication/pipeline-capture.json'
        bridge=out/'bridge';diag=bridge/'diagnostics';diag.mkdir(parents=True,exist_ok=True)
        (diag/'pipeline-capture.json').write_bytes(pub.read_bytes())
        view=inspect_cases(bridge)
        assert view['available'] is False and view['capture_diagnostics']['available'] is True
        assert view['capture_diagnostics']['data']['counters']==snapshot['counters']
        malformed=json.loads(pub.read_text());malformed['data']['counters']['attempted']=-1
        (diag/'pipeline-capture.json').write_text(json.dumps(malformed))
        assert inspect_cases(bridge)['capture_diagnostics']['available'] is False
        (diag/'pipeline-capture.json').write_bytes(pub.read_bytes())
        from debug_inspect import document
        class SDK:
            root=bridge
        envelope=json.loads(pub.read_text());status={'run_id':envelope['run_id'],'module_generation':envelope['module_generation'],'fresh':True,'video_snapshot_available':True}
        doc=document(SDK(),'pipeline-capture.json',status)
        assert doc['available'] and doc['counts_are_not_live_task_or_gpu_success']
        mismatch=dict(status,run_id='other');assert document(SDK(),'pipeline-capture.json',mismatch)['fresh'] is False
        parser=dict(audit_without_archive=True,malformed_counter_rejected=True,SDK_document_typed=True,session_mismatch_not_fresh=True)
    assert all(hashlib.sha256(Path(q).read_bytes()).hexdigest()==h for q,h in hashes.items())
    result=dict(command=subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),input='Actual production reservation/Write body with test-only IO gate insertion, realfilesystem/8thread normal stress and controlled4large/16tiny inflight writes; syntheticenvironment/dummySPV, noGPU/ISA',hashes=hashes,native_command=subprocess.list2cmdline(native),literal_stdout=r.stdout,literal_stderr=r.stderr,exit_status=r.returncode,snapshot=snapshot,parser=parser,controlled_IO_not_unmodified_timing=True,reservation=a.reservation,controlled_invariants=dict(other_capture_while_IO_blocked=a.reservation,byte_budget_includes4_inflight=a.reservation,case_budget_includes16_inflight=a.reservation,old_run_refund_isolated=a.reservation,legacy_IO_lock_observed=not a.reservation))
    (out/'RESULT.json').write_text(json.dumps(result,indent=2))
    print(f'PIPELINE_CAPTURE_TEST_PASS; instrumented_production=True; filesystem=True; controlled_IO=True; budgets=True; reload_preserved=True; reservation={a.reservation}; GPU_executed=False')

if __name__=='__main__':main()
