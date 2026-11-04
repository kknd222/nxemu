"""Bounded actual GuestStreamer/device-memory stage, not complete GPU query loop."""
from pathlib import Path
import argparse,hashlib,json,subprocess,sys,time
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);ap.add_argument('--samples-json',type=Path);a=ap.parse_args()
    R=Path(__file__).resolve().parents[1];B=R/'build/windows-ninja';O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
    cpp=R/'tools/query_guest_harness.cpp';exe=O/'query_guest.exe'
    includes=[R/'src',R/'src/nxemu-video',R/'src/yuzu_video_core',R/'external/boost',R/'external/fmt/include',B/'src']
    libs=[B/'src/yuzu_video_core/Release/video_core.lib',B/'src/common/Release/common.lib',B/'src/yuzu_common/Release/yuzu_common.lib',B/'external/fmt/Release/fmt.lib']
    inputs=[Path(__file__),cpp,R/'tools/query_guest_fixture.h',R/'src/nxemu-module-spec/cpu.h',R/'src/nxemu-module-spec/operating_system.h',R/'src/yuzu_video_core/device_memory_manager.h',R/'src/yuzu_video_core/query_cache/query_cache.h',R/'src/yuzu_video_core/query_cache/query_stream.h',R/'src/yuzu_video_core/query_cache/query_base.h',R/'src/yuzu_video_core/host1x/gpu_device_memory_manager.cpp',*libs]
    samples=[]
    if a.samples_json:
        sample_report=json.loads(a.samples_json.read_text());assert sample_report['result']=='VULKAN_CLIP_PASS' and sample_report['gpu_execution']
        assert len(sample_report['cases'])==3
        for case in sample_report['cases']:
            assert case['fence_completed'] and case['occlusion_available'] and case['passed']
            v=case['occlusion_samples'];assert type(v) is int and 0<=v<=0xffffffff;samples.append(str(v))
        inputs.append(a.samples_json.resolve())
    hashes={str(p):sha(p) for p in inputs};command=['cl','/nologo','/std:c++20','/utf-8','/EHsc','/MT','/Od',*[f'/I{p}' for p in includes],str(cpp),f'/Fe:{exe}',f'/Fo:{O}/',*map(str,libs),'/link','/OPT:REF','ws2_32.lib','winmm.lib','bcrypt.lib','crypt32.lib','iphlpapi.lib','psapi.lib','ole32.lib','shell32.lib']
    cmd=O/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(command)+'\n')
    record=dict(command=subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),scope='actual GuestStreamer template and actual DeviceMemoryManager, legal interface-backed 16KiB RAM; CPU-controlled SyncValues adapter; not full QueryCacheBase or SamplesStreamer or Vulkan',source_hashes=hashes)
    b=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=120);(O/'build.log').write_text(b.stdout+b.stderr);record['build']=dict(command=subprocess.list2cmdline(command),exit_status=b.returncode,literal_stdout=b.stdout,literal_stderr=b.stderr)
    if b.returncode==0:
        run_cmd=[str(exe),*samples];run=subprocess.run(run_cmd,capture_output=True,text=True,timeout=30);record['run']=dict(command=subprocess.list2cmdline(run_cmd),input='16KiB memory; pending/report32/report64/filter/reuse/zero/max/64bit-truncation; optional imported host samples '+','.join(samples),exit_status=run.returncode,literal_stdout=run.stdout,literal_stderr=run.stderr)
        record['passed']=run.returncode==0 and 'GUEST_STREAMER_DEVICE_MEMORY_PASS' in run.stdout
    else:record['passed']=False
    record['inputs_unchanged']=all(sha(Path(p))==h for p,h in hashes.items());record['passed']&=record['inputs_unchanged'];record['exit_status']=0 if record['passed'] else 1
    (O/'RESULT.json').write_text(json.dumps(record,indent=2));print('QUERY_GUEST_STAGE_PASS; actual_template=True; actual_device_manager=True; GPU=False; full_QueryCache=False' if record['passed'] else 'QUERY_GUEST_STAGE_FAILED')
    return record['exit_status']
if __name__=='__main__':raise SystemExit(main())
