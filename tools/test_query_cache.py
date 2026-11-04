"""Run entire production QueryCacheBase on legal CPU-only GPU/channel fixture."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess,sys,os
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 R=Path(__file__).resolve().parents[1];B=R/'build/windows-ninja';O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
 header=R/'src/nxemu-module-spec/operating_system.h';text=header.read_text();section=text.split('nxinterface IOperatingSystem\n')[1].split('\n};')[0]
 signatures=re.findall(r'virtual\s+([^;{}]+?)\s*=\s*0\s*;',section);assert len(signatures)==60
 # MSVC __interface treats even the default-body member as abstract.
 methods=['void GatherGPUDirtyMemoryForReadback(ICacheInvalidator*) override {throw std::runtime_error("unused readback OS");}']
 for signature in signatures:
  name=re.search(r'(\w+)\s*\(',signature).group(1)
  body={'GetGPUTicks':'++tick_calls;return ticks;','DeviceMemory':'return backing;','IsPoweredOn':'return true;','IsShuttingDown':'return false;'}.get(name,'throw std::runtime_error("unused OS method '+name+'");')
  methods.append(signature+' override {'+body+'}')
 generated=O/'query_cache_os.h';generated.write_text('#pragma once\n#include "nxemu-module-spec/operating_system.h"\nstruct OperatingSystemMock final : IOperatingSystem { Memory& backing; unsigned tick_calls{}; static constexpr uint64_t ticks=0x99887766aa00ULL; explicit OperatingSystemMock(Memory& m):backing(m){}\n'+'\n'.join(methods)+'\n};\n')
 cpp=R/'tools/query_cache_harness.cpp';exe=O/'query_cache.exe'
 includes=[O,R/'tools',R/'src',R/'src/nxemu-video',R/'src/yuzu_video_core',R/'external/boost',R/'external/fmt/include',B/'src']
 libs=[B/'src/yuzu_video_core/Release/video_core.lib',B/'src/common/Release/common.lib',B/'src/yuzu_common/Release/yuzu_common.lib',B/'external/fmt/Release/fmt.lib',B/'external/zstd/lib/Release/zstd_static.lib',B/'src/3rd_party/lz4/Release/lz4.lib',B/'src/nxemu-core/Release/nxemu_core.lib',B/'external/boost/libs/context/Release/boost_context.lib',B/'src/3rd_party/glad/Release/glad.lib',B/'src/yuzu_shader_recompiler/Release/shader_recompiler.lib',B/'src/3rd_party/bc_decoder/Release/bc_decoder.lib',B/'external/Release/stb_dxt.lib',B/'external/sirit/src/Release/sirit.lib',*[R/'external/ffmpeg/lib'/('lib'+name+'.lib') for name in ['avcodec','avdevice','avfilter','avformat','avutil','swresample','swscale']]]
 sources=[cpp,R/'src/nxemu-video/video_settings.cpp',R/'src/nxemu-video/video_enum_strings.cpp']
 inputs=[Path(__file__),R/'tools/query_cache_fixture.h',R/'tools/query_guest_fixture.h',generated,header,R/'src/nxemu-module-spec/base.h',R/'src/yuzu_video_core/query_cache/query_cache.h',R/'src/yuzu_video_core/query_cache/query_cache_base.h',R/'src/yuzu_video_core/query_cache/query_base.h',R/'src/yuzu_video_core/query_cache/query_stream.h',R/'src/yuzu_video_core/device_memory_manager.h',R/'src/yuzu_video_core/memory_manager.cpp',R/'src/yuzu_video_core/gpu.cpp',R/'src/yuzu_video_core/gpu_thread.cpp',R/'src/yuzu_video_core/control/channel_state.cpp',*sources,*libs]
 hashes={str(x):sha(x) for x in inputs}
 command=['cl','/nologo','/std:c++20','/utf-8','/EHsc','/MT','/Od','/Gy',*[f'/I{x}' for x in includes],*map(str,sources),f'/Fe:{exe}',f'/Fo:{O}/',*map(str,libs),'/link','/OPT:REF','ws2_32.lib','winmm.lib','bcrypt.lib','crypt32.lib','iphlpapi.lib','psapi.lib','ole32.lib','shell32.lib','user32.lib']
 build=O/'build.cmd';build.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(command)+'\n')
 r=subprocess.run(['cmd','/c',str(build)],capture_output=True,text=True,timeout=120);(O/'build.log').write_text(r.stdout+r.stderr)
 result={'command':subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),'scope':'entire production QueryCacheBase, real Host1x/GPU/ChannelState/MemoryManager, controlled GuestStreamer CPU SyncValues and OS timestamp; not SamplesStreamer/Vulkan','source_hashes':hashes,'build':dict(command=subprocess.list2cmdline(command),exit_status=r.returncode,literal_stdout=r.stdout,literal_stderr=r.stderr),'passed':False}
 if r.returncode==0:
  env=dict(os.environ);env['PATH']=str(R/'bin/x64/Release')+';'+str(R/'external/ffmpeg/bin')+';'+env.get('PATH','')
  run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30,env=env);result['run']=dict(command=str(exe),input='16KiB backing,32/64 report widths,NotifyWFI,Commit/Pop,OS timestamp,fence,payload fast path,invalidate,rewrite,+4 conditional lookup;17cases; CPU callbacks not GPU fences',exit_status=run.returncode,literal_stdout=run.stdout,literal_stderr=run.stderr);result['passed']=run.returncode==0 and 'QUERY_CACHE_FULL_TEMPLATE_PASS' in run.stdout
 result['inputs_unchanged']=all(sha(Path(x))==h for x,h in hashes.items());result['passed']&=result['inputs_unchanged'];result['exit_status']=0 if result['passed'] else 1;result['literal_output']='QUERY_CACHE_STAGE_PASS; actual_entire_template=True; actual_GPU_object=True; SamplesStreamer=False; Vulkan=False' if result['passed'] else 'QUERY_CACHE_STAGE_FAILED'
 (O/'RESULT.json').write_text(json.dumps(result,indent=2));print(result['literal_output']);return result['exit_status']
if __name__=='__main__':raise SystemExit(main())
