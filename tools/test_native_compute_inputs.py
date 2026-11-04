"""Small actual-header CPU tests; no shader library, emulator, GPU, or game input."""
from pathlib import Path
import argparse, hashlib, json, subprocess, sys

CPP = r'''
#include "yuzu_video_core/native_shader_fixture.h"
#ifdef INPUTS_PRESENT
#include "yuzu_video_core/native_compute_input_fixture.h"
#endif
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace N=VideoCommon::NativeShaderFixture;
std::filesystem::path root;
std::string run="1",generation="3";
bool fixtures=true,fail_metadata=false;
std::atomic<unsigned> mapped_calls{},reader_calls{};
namespace VideoCommon::NativeShaderFixture {
bool Enabled(){return fixtures;}
std::filesystem::path Root(){return root;}
std::string RunId(){return run;}
std::string Generation(){return generation;}
bool Replace(const std::filesystem::path& a,const std::filesystem::path& b) noexcept {
 if(b.extension()==".json"){
  auto blob=b;blob.replace_extension();blob+="-cbuf0.bin";assert(std::filesystem::is_regular_file(blob));
  if(fail_metadata)return false;
 }
 std::error_code ec;std::filesystem::rename(a,b,ec);return !ec;
}
}
#ifdef INPUTS_PRESENT
namespace I=VideoCommon::NativeComputeInputs;
std::array<uint8_t,32> native{};
I::Context context(uint64_t token=1){return {0x10000,0x10000,token,0x20000,1024,true};}
bool capture(const I::Context& c,bool on=true,std::string target={},bool mapped=true,bool throws=false){
 if(target.empty())target=N::Hex(N::Hash(native));
 return I::Capture(native,c,on,target,[&](uint64_t a,size_t n){++mapped_calls;assert(a==c.cbuf_address&&n<=256);return mapped;},
   [&](uint64_t a,std::span<uint8_t> out){++reader_calls;assert(a==c.cbuf_address);if(throws)throw std::runtime_error("controlled reader failure");
     for(size_t i=0;i<out.size();++i)out[i]=uint8_t(i+c.dispatch_token);});
}
#endif
int main(int argc,char**argv){assert(argc==3);const std::string test=argv[1];root=argv[2];std::filesystem::create_directories(root);
#ifndef INPUTS_PRESENT
 assert(!std::filesystem::exists(root/"native-compute-inputs"));
 std::cout<<"NATIVE_COMPUTE_INPUTS_BASELINE_PASS reads=0 collector_present=0 CPU_ONLY=1\n";return 0;
#else
 for(size_t i=0;i<native.size();++i)native[i]=uint8_t(i);
 if(test=="off"){
  assert(!capture(context(),false));fixtures=false;assert(!capture(context()));
  assert(reader_calls==0&&mapped_calls==0&&I::registry.attempts==0);
 }else if(test=="target"){
  for(const auto& t:{std::string(16,'0'),std::string(15,'a'),std::string(16,'A'),std::string(16,'z')})assert(!capture(context(),true,t));
  assert(reader_calls==0&&mapped_calls==0&&I::registry.attempts==0);
 }else if(test=="lifecycle"){
  assert(capture(context()));assert(!capture(context()));assert(reader_calls==1&&mapped_calls==1);
  assert(capture(context(2)));run="2";generation="4";
  assert(capture(context()));assert(capture(context(2)));assert(!capture(context(3)));
  assert(reader_calls==4&&mapped_calls==4&&I::registry.ready==4&&I::registry.duplicates==1&&I::registry.budget_drops==1);
 }else if(test=="invalid"){
  auto c=context();c.cbuf_declared_size=0;assert(!capture(c));
  c=context(2);c.cbuf_enabled=false;assert(!capture(c));
  c=context(3);c.cbuf_address=UINT64_MAX-127;assert(!capture(c));
  c=context(4);c.dispatch_program++;assert(!capture(c));
  assert(!capture(context(5)));assert(reader_calls==0&&mapped_calls==0&&I::registry.invalid==4&&I::registry.budget_drops==1);
 }else if(test=="identity"){
  auto c=context();c.dispatch_token=0;assert(!capture(c));
  c=context(2);c.program=c.dispatch_program=0;assert(!capture(c));
  c=context(3);c.cbuf_address=0;assert(!capture(c));
  run="";assert(!capture(context(4)));assert(reader_calls==0&&mapped_calls==0&&I::registry.invalid==4);
 }else if(test=="unmapped"){
  auto c=context();c.cbuf_address=0xfff;c.cbuf_declared_size=256;
  assert(!capture(c,true,{},false));assert(!capture(c,true,{},false));
  assert(reader_calls==0&&mapped_calls==1&&I::registry.unmapped==1&&I::registry.duplicates==1);
 }else if(test=="exception"){
  for(unsigned i=1;i<=4;++i)assert(!capture(context(i),true,{},true,true));
  assert(!capture(context(5)));assert(reader_calls==4&&mapped_calls==4&&I::registry.exceptions==4&&I::registry.budget_drops==1);
 }else if(test=="mappedexception"){
  for(unsigned i=1;i<=4;++i)assert(!I::Capture(native,context(i),true,N::Hex(N::Hash(native)),
    [](uint64_t,size_t)->bool{++mapped_calls;throw std::runtime_error("controlled mapping failure");},
    [](uint64_t,std::span<uint8_t>){++reader_calls;}));
  assert(!capture(context(5)));assert(reader_calls==0&&mapped_calls==4&&I::registry.exceptions==4&&I::registry.budget_drops==1);
 }else if(test=="io"){
  fail_metadata=true;assert(!capture(context()));assert(I::registry.io_errors==1&&I::registry.ready==0);
  assert(std::filesystem::is_regular_file(root/"native-compute-inputs/compute-input-0-cbuf0.bin"));
  assert(!std::filesystem::exists(root/"native-compute-inputs/compute-input-0.json"));
  assert(!capture(context()));assert(reader_calls==1&&mapped_calls==1&&I::registry.duplicates==1);
 }else if(test=="existing"){
  const auto d=root/"native-compute-inputs";std::filesystem::create_directories(d);
  {std::ofstream f(d/"compute-input-0-cbuf0.bin",std::ios::binary);f<<"original";}
  assert(!capture(context()));assert(I::registry.io_errors==1&&I::registry.ready==0);
  assert(!std::filesystem::exists(d/"compute-input-0.json"));
  std::ifstream f(d/"compute-input-0-cbuf0.bin",std::ios::binary);std::string original;f>>original;assert(original=="original");
 }else if(test=="root"){
  root=root/"missing";assert(!capture(context()));assert(reader_calls==0&&mapped_calls==0&&I::registry.io_errors==1);
 }else if(test=="bound"){
  auto c=context();c.cbuf_declared_size=1;assert(capture(c));
  c=context(2);c.cbuf_address=UINT64_MAX;c.cbuf_declared_size=1;assert(capture(c));
  c=context(3);c.cbuf_declared_size=UINT32_MAX;assert(capture(c));
  assert(reader_calls==3&&I::registry.bytes==258);
 }else if(test=="concurrent"){
  std::array<std::thread,8> workers;for(unsigned i=0;i<8;++i)workers[i]=std::thread([i]{capture(context(i+1));});
  for(auto& t:workers)t.join();assert(reader_calls<=4&&mapped_calls<=4&&I::registry.ready==reader_calls);
  assert(I::registry.budget_drops+I::registry.lock_drops+I::registry.ready==8);
 }else{throw std::runtime_error("unknown case");}
 std::cout<<"NATIVE_COMPUTE_INPUTS_CASE_PASS "<<test<<" CPU_ONLY=1 GPU_VISIBILITY=0\nRESULT "<<I::CountersJson()<<'\n';
#endif
}
'''

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{value:016x}'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--source-root', type=Path)
    ap.add_argument('--mode', choices=['baseline', 'modified'], default='modified')
    a = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = (a.source_root or repo / 'src').resolve()
    header = source / 'yuzu_video_core/native_compute_input_fixture.h'
    base_header = source / 'yuzu_video_core/native_shader_fixture.h'
    assert base_header.is_file()
    assert header.is_file() if a.mode == 'modified' else not header.exists()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cpp = out / 'test.cpp'
    cpp.write_text(CPP, encoding='utf-8')
    exe = out / 'test.exe'
    command = ['cl', '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/MT', '/O2', '/I' + str(source)]
    if a.mode == 'modified':
        command.append('/DINPUTS_PRESENT')
    command += [str(cpp), '/Fe:' + str(exe), '/Fo:' + str(out / 'test.obj')]
    build = out / 'build.cmd'
    build.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n' + subprocess.list2cmdline(command) + '\n')
    frozen = {str(p): sha(p) for p in [Path(__file__), cpp, base_header, *([header] if header.exists() else [])]}
    report = dict(command=subprocess.list2cmdline([sys.executable, '-B', Path(__file__), *sys.argv[1:]]),
                  input='actual CPU-only header, injected mapping/ReadBlockUnsafe contract, synthetic bytes; no GPU or emulator execution',
                  mode=a.mode, hashes=frozen, compiler_command=subprocess.list2cmdline(command), results=[])
    try:
        b = subprocess.run(['cmd', '/c', str(build)], capture_output=True, text=True, timeout=90)
        (out / 'build.log').write_text(b.stdout + b.stderr)
        report['build_exit_status'] = b.returncode
        assert b.returncode == 0
        cases = ['baseline'] if a.mode == 'baseline' else ['off', 'target', 'lifecycle', 'invalid', 'identity', 'unmapped', 'exception', 'mappedexception', 'io', 'existing', 'root', 'bound', 'concurrent']
        metadata_count = 0
        for case in cases:
            cmd = [str(exe), case, str(out / case)]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
            row = dict(command=subprocess.list2cmdline(cmd), input=case, exit_status=r.returncode, stdout=r.stdout, stderr=r.stderr)
            report['results'].append(row)
            assert r.returncode == 0
            for path in (out / case / 'native-compute-inputs').glob('*.json'):
                data = json.loads(path.read_text())
                blob = path.parent / data['cbuf']['blob']
                content = blob.read_bytes()
                assert data['status'] == 'ready' and data['read_mode'] == 'CPU_MIRROR_NO_FLUSH'
                assert data['GPU_visibility_proven'] is False and data['synchronization_added'] is False
                assert data['full_resource_replay'] is False
                assert data['program'] == data['dispatch_program'] and data['dispatch_token'] > 0
                assert data['native_content_id'] == fnv(bytes(range(32)))
                assert data['cbuf']['content_id'] == fnv(content)
                assert len(content) == data['cbuf']['captured_size'] <= 256
                assert content == bytes((i + data['dispatch_token']) & 255 for i in range(len(content)))
                metadata_count += 1
        assert all(sha(Path(p)) == h for p, h in frozen.items())
        report.update(exit_status=0, actual_header=True, CPU_ONLY=True, GPU=False, no_libraries_copied=True,
                      metadata_payloads_verified=metadata_count, executable_sha256=sha(exe))
        (out / 'RESULT.json').write_text(json.dumps(report, indent=2))
        print(f'NATIVE_COMPUTE_INPUTS_PASS mode={a.mode} cases={len(cases)} CPU_ONLY=1 GPU_VISIBILITY=0')
    except BaseException as error:
        report.update(exit_status=1, failure=repr(error))
        (out / 'FAILURE.json').write_text(json.dumps(report, indent=2))
        raise

if __name__ == '__main__':
    main()
