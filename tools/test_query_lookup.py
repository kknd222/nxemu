"""Execute the exact production gen_lookup lambda with bounded fake cache/query state.

This is a production-lambda regression, not a full QueryCache/Vulkan/guest-memory
integration test. Each scenario runs in its own process; MSVC checked iterators
turn the baseline end-iterator defect into a deterministic negative result.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys


CPP = r'''
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
using u64=std::uint64_t; using u32=std::uint32_t;
using VAddr=u64; using GPUVAddr=u64;
namespace Core { constexpr u64 DEVICE_PAGEBITS=12, DEVICE_PAGEMASK=4095; }
enum class QueryFlagBits:u32 { IsHostManaged=1, IsGuestSynced=2 };
u32 operator&(u32 flags, QueryFlagBits bit){return flags & static_cast<u32>(bit);}
bool True(u32 n){return n!=0;} bool False(u32 n){return n==0;}
struct QueryBase{u32 flags{};};
namespace VideoCommon { struct LookupData{VAddr address; QueryBase* found_query;}; }
struct Memory { bool mapped=true; std::optional<u64> GpuToCpuAddress(u64 x){return mapped?std::optional<u64>(x):std::nullopt;} };
struct Impl { std::unordered_map<u32,QueryBase*> queries; u32 last_id=~0U;
 QueryBase* ObtainQuery(u32 id){last_id=id;auto it=queries.find(id);return it==queries.end()?nullptr:it->second;}
};
struct Fixture {
 Memory memory; Memory* gpu_memory=&memory; Impl storage; Impl* impl=&storage;
 std::mutex cache_mutex; std::unordered_map<u64,std::unordered_map<u32,u32>> cached_queries;
 VideoCommon::LookupData Lookup(u64 address,bool& qc_dirty) {
__PRODUCTION_LAMBDA__
  return gen_lookup(address);
 }
};
void require(bool value,const char* message){if(!value){std::cerr<<"CHECK_FAIL "<<message<<std::endl;std::exit(3);}}
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
 _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
 _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
 require(argc==2,"scenario argument");const std::string mode=argv[1];
 Fixture f;QueryBase base{},adjacent{};bool dirty=false;u64 address=0x2040;
 const u64 page=address>>Core::DEVICE_PAGEBITS;const u32 offset=address&Core::DEVICE_PAGEMASK;
 QueryBase* expected=nullptr;u64 expected_address=address;bool expected_dirty=false;
 auto put_base=[&]{f.cached_queries[page][offset]=7;f.storage.queries[7]=&base;expected=&base;};
 auto put_adjacent=[&]{f.cached_queries[page][offset+4]=8;f.storage.queries[8]=&adjacent;expected=&adjacent;};
 if(mode=="unmapped"){f.memory.mapped=false;expected_address=0;}
 else if(mode=="no-page"){}
 else if(mode=="no-entry"){f.cached_queries[page][offset+8]=7;}
 else if(mode=="base-clean"){put_base();}
 else if(mode=="base-host-dirty"){put_base();base.flags=1;expected_dirty=true;}
 else if(mode=="base-host-guest-synced"){put_base();base.flags=3;}
 else if(mode=="base-guest-synced-only"){put_base();base.flags=2;}
 else if(mode=="adjacent-clean"){put_adjacent();}
 else if(mode=="adjacent-host-dirty"){put_adjacent();adjacent.flags=1;expected_dirty=true;}
 else if(mode=="both-prefer-base"){put_adjacent();put_base();adjacent.flags=1;}
 else if(mode=="base-null"){f.cached_queries[page][offset]=9;}
 else if(mode=="adjacent-null"){f.cached_queries[page][offset+4]=9;}
 else if(mode=="dirty-sticky"){dirty=true;expected_dirty=true;}
 else if(mode=="page-end-no-wrap"){
  address=0x2ffc;f.cached_queries[2][100]=7;f.cached_queries[3][0]=8;f.storage.queries[8]=&adjacent;expected_address=address;
 }
 else{require(false,"unknown scenario");}
 std::cout<<"BEGIN "<<mode<<std::endl;
 const auto result=f.Lookup(address,dirty);
 require(result.address==expected_address,"original condition base address retained");
 require(result.found_query==expected,"lookup result identity");
 require(dirty==expected_dirty,"dirty state");
 if(expected==&base)require(f.storage.last_id==7,"base entry precedence");
 if(expected==&adjacent)require(f.storage.last_id==8,"adjacent entry selected");
 std::cout<<"QUERY_LOOKUP_CASE_PASS "<<mode<<std::endl;return 0;
}
'''

CASES = ["unmapped", "no-page", "no-entry", "base-clean", "base-host-dirty",
         "base-host-guest-synced", "base-guest-synced-only", "adjacent-clean",
         "adjacent-host-dirty", "both-prefer-base", "base-null", "adjacent-null",
         "dirty-sticky", "page-end-no-wrap"]
BASELINE_DEFECTS = {"adjacent-clean", "adjacent-host-dirty", "base-null", "adjacent-null"}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_lambda(source):
    marker = "const auto gen_lookup = [this, &qc_dirty]"
    if source.count(marker) != 1:
        raise ValueError("production lookup marker must be unique")
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                if source[end:end+1] != ";":
                    raise ValueError("lambda terminator changed")
                return source[start:end+1]
    raise ValueError("unterminated production lambda")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--expect-defects", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    header = (args.header or repo / "src/yuzu_video_core/query_cache/query_cache.h").resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = header.read_text(encoding="utf-8-sig")
    production = extract_lambda(source)
    (out / "production_lambda.txt").write_text(production, encoding="utf-8")
    cpp = out / "query_lookup.cpp"
    cpp.write_text(CPP.replace("__PRODUCTION_LAMBDA__", production), encoding="utf-8")
    exe = out / "query_lookup.exe"
    obj = out / "query_lookup.obj"
    command = ["cl", "/nologo", "/std:c++20", "/utf-8", "/EHsc", "/MDd", "/Od",
               "/D_ITERATOR_DEBUG_LEVEL=2", str(cpp), "/Fe:"+str(exe), "/Fo:"+str(obj)]
    build = out / "build.cmd"
    build.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
                     'if errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(command)+'\n')
    before = {str(header):sha(header), str(Path(__file__).resolve()):sha(Path(__file__))}
    report = dict(schema=1, scope="exact production lambda plus fake cache/query/address translation; not full QueryCache/Vulkan/guest integration",
                  command=subprocess.list2cmdline([sys.executable,"-B",str(Path(__file__).resolve()),*sys.argv[1:]]),
                  compiler_command=subprocess.list2cmdline(command), source_hashes=before,
                  production_lambda_sha256=hashlib.sha256(production.encode()).hexdigest(),
                  checked_iterators=True, actual_GPU=False, actual_guest_memory=False)
    compile_result = subprocess.run(["cmd","/c",str(build)],capture_output=True,text=True,timeout=90)
    (out / "build.log").write_text(compile_result.stdout+compile_result.stderr)
    report["compile"] = dict(exit_status=compile_result.returncode,literal_stdout=compile_result.stdout,literal_stderr=compile_result.stderr)
    if compile_result.returncode:
        (out / "FAILURE.json").write_text(json.dumps(report,indent=2))
        raise RuntimeError("compiler failure; see build.log")
    results=[]
    for case in CASES:
        run = subprocess.run([str(exe),case],capture_output=True,text=True,timeout=10)
        expected_defect=args.expect_defects and case in BASELINE_DEFECTS
        if expected_defect:
            passed=(run.returncode != 0 and "BEGIN "+case in run.stdout and
                    ("cannot dereference end" in run.stderr or
                     case=="base-null" and run.returncode in (3221225477,-1073741819)))
        else:
            passed=(run.returncode==0 and run.stdout.endswith("QUERY_LOOKUP_CASE_PASS "+case+"\n"))
        results.append(dict(case=case,command=subprocess.list2cmdline([str(exe),case]),
                            input=case,expected_defect=expected_defect,passed=passed,
                            exit_status=run.returncode,literal_stdout=run.stdout,literal_stderr=run.stderr))
    report["cases"]=results
    report["source_hashes_unchanged"]=all(sha(Path(p))==h for p,h in before.items())
    report["passed"]=all(r["passed"] for r in results) and report["source_hashes_unchanged"]
    negatives=sum(r["expected_defect"] for r in results)
    mode="baseline" if args.expect_defects else "modified"
    literal=f"QUERY_LOOKUP_PASS; mode={mode}; cases={len(results)}; expected_defects={negatives}; production_lambda=True; GPU=False; guest_integration=False"
    report["literal_output"]=literal if report["passed"] else "QUERY_LOOKUP_FAILED"
    report["exit_status"]=0 if report["passed"] else 1
    report["artifacts"]={p.name:sha(p) for p in [cpp,exe,out/"production_lambda.txt",build,out/"build.log"]}
    (out / "RESULT.json").write_text(json.dumps(report,indent=2))
    print(report["literal_output"])
    return report["exit_status"]


if __name__=="__main__":
    raise SystemExit(main())
