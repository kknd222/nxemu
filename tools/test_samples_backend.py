"""No-game real SamplesStreamer Vulkan experiment, bounded worker process."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess, sys
VERT='''#version 450
vec2 p[3]=vec2[3](vec2(-1,-1),vec2(-1,3),vec2(3,-1));
void main(){gl_Position=vec4(p[gl_VertexIndex],0,1);}
'''
FRAG='''#version 450
layout(location=0)out vec4 color;void main(){color=vec4(1,0,0,1);}
'''
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);ap.add_argument('--expect-width-defect',action='store_true');ap.add_argument('--harness-source',type=Path);ap.add_argument('--query-source',type=Path);ap.add_argument('--device-source',type=Path);ap.add_argument('--expect-validation-errors',type=int,choices=[0,1,2],default=0);a=ap.parse_args()
 R=Path(__file__).resolve().parents[1];B=R/'build/windows-ninja';O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
 report={'expected_width_defect':a.expect_width_defect,'expected_validation_errors':a.expect_validation_errors,'scope':'actual SamplesStreamer/Device/Scheduler/query/prefix scan with actual GPU draws; lab callback GPU copy to real guest backing; not full QueryCacheRuntime','commands':[]}
 sources=[a.harness_source.resolve(strict=True) if a.harness_source else R/'tools/samples_backend_harness.cpp',a.query_source.resolve(strict=True) if a.query_source else R/'src/yuzu_video_core/renderer_vulkan/vk_query_cache.cpp',R/'src/yuzu_video_core/renderer_vulkan/vk_scheduler.cpp',a.device_source.resolve(strict=True) if a.device_source else R/'src/yuzu_video_core/vulkan_common/vulkan_device.cpp',R/'src/nxemu-video/video_settings.cpp',R/'src/nxemu-video/video_enum_strings.cpp']
 includes=[R/'src',R/'src/nxemu-video',R/'src/yuzu_video_core',R/'external/boost',R/'external/fmt/include',R/'external/Vulkan-Headers/include',R/'external/vulkan-memory-allocator/include',R/'external/vulkan_utility_libraries/include',B/'src',B/'generated']
 ninja=(B/'CMakeFiles/impl-Release.ninja').read_text();start=ninja.index('build D$:\\project\\nxemu-switch2-upgrade\\repo\\modules\\x64\\video\\Release\\nxemu-video.dll ')
 text=ninja[start:];line=re.search(r'^  LINK_LIBRARIES = (.+)$',text,re.M).group(1)
 libs=[]
 for part in line.split():
  p=Path(part);p=p if p.is_absolute() else B/p
  libs.append(str(p) if p.is_file() else part)
 exe=O/'samples_backend.exe'
 command=['cl','/nologo','/std:c++20','/utf-8','/EHsc','/MT','/Od','/Gy','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/DARCHITECTURE_x86_64=1',*[f'/I{x}' for x in includes],*map(str,sources),f'/Fe:{exe}',f'/Fo:{O}/','/link','/OPT:REF',*libs,'ws2_32.lib','winmm.lib','bcrypt.lib','crypt32.lib','iphlpapi.lib','psapi.lib','shlwapi.lib','userenv.lib','wtsapi32.lib','version.lib']
 inputs=[Path(__file__),*sources,R/'tools/query_guest_fixture.h',R/'src/yuzu_video_core/renderer_vulkan/samples_backend_lab.h',R/'src/yuzu_video_core/renderer_vulkan/vk_scheduler.h',*[Path(x) for x in libs if Path(x).is_file()]]
 hashes={str(p):sha(p) for p in inputs};report['source_hashes']=hashes
 cmd=O/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(command)+'\n')
 build=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=180);(O/'build.log').write_text(build.stdout+build.stderr)
 report['build']={'command':subprocess.list2cmdline(command),'exit_status':build.returncode,'literal_stdout':build.stdout,'literal_stderr':build.stderr}
 passed=False
 if build.returncode==0:
  compiler=Path(r'C:\VulkanSDK\1.4.357.0\Bin\glslangValidator.exe');report['glslang_sha256']=sha(compiler)
  for stage,text in [('vert',VERT),('frag',FRAG)]:
   src=O/('sample.'+stage);src.write_text(text);cmd2=[str(compiler),'-V',str(src),'-o',str(src)+'.spv']
   c=subprocess.run(cmd2,capture_output=True,text=True,timeout=20);report['commands'].append(dict(command=subprocess.list2cmdline(cmd2),exit_status=c.returncode,literal_stdout=c.stdout,literal_stderr=c.stderr));assert c.returncode==0
  run_cmd=[str(exe),str(O/'sample.vert.spv'),str(O/'sample.frag.spv'),'1' if a.expect_width_defect else '0']
  try:
   run=subprocess.run(run_cmd,capture_output=True,text=True,timeout=60)
   report['run']=dict(command=subprocess.list2cmdline(run_cmd),input='16x16 fullscreen triangle; one/two pass; reset; noTimestamp width neighbor',exit_status=run.returncode,literal_stdout=run.stdout,literal_stderr=run.stderr)
   passed=('SAMPLES_BACKEND_GPU_PASS' in run.stdout and f'SAMPLES_VALIDATION errors={a.expect_validation_errors} warnings=0' in run.stdout and run.returncode==(1 if a.expect_validation_errors else 0) and (not a.expect_validation_errors or 'LAB_ERROR Vulkan validation errors' in run.stderr))
  except subprocess.TimeoutExpired as e:report['run']=dict(command=subprocess.list2cmdline(run_cmd),exit_status=124,watchdog_seconds=60,literal_stdout=str(e.stdout),literal_stderr=str(e.stderr))
 report['inputs_unchanged']=all(sha(Path(p))==h for p,h in hashes.items());passed&=report['inputs_unchanged'];report['passed']=passed;report['exit_status']=0 if passed else 1
 report['command']=subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]])
 (O/'RESULT.json').write_text(json.dumps(report,indent=2));print('SAMPLES_BACKEND_TEST_PASS' if passed else 'SAMPLES_BACKEND_TEST_FAILED');return report['exit_status']
if __name__=='__main__':raise SystemExit(main())
