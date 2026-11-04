"""Bounded real Vulkan QueryCacheRuntime/BufferCache/SamplesStreamer equality differential; unresolved Conditional cases are observations."""
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
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);ap.add_argument('--expect-equality-mismatches',type=int,choices=range(17),default=0);ap.add_argument('--query-source',type=Path);ap.add_argument('--include-overlay',type=Path);ap.add_argument('--compute-source',type=Path);ap.add_argument('--device-source',type=Path);ap.add_argument('--expect-validation-errors',type=int,choices=[0,1,2],default=0);ap.add_argument('--capture-predicate',action='store_true');ap.add_argument('--expect-no-producer-defect',action='store_true');ap.add_argument('--expect-history-defect',action='store_true');ap.add_argument('--expect-accumulation-defect',action='store_true');a=ap.parse_args()
 R=Path(__file__).resolve().parents[1];B=R/'build/windows-ninja';O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
 header=R/'src/nxemu-module-spec/operating_system.h';text=header.read_text();section=text.split('nxinterface IOperatingSystem\n')[1].split('\n};')[0]
 signatures=re.findall(r'virtual\s+([^;{}]+?)\s*=\s*0\s*;',section);assert len(signatures)==60
 methods=['void GatherGPUDirtyMemoryForReadback(ICacheInvalidator*) override {throw std::runtime_error("unused readback OS");}']
 for signature in signatures:
  name=re.search(r'(\w+)\s*\(',signature).group(1)
  body={'GetGPUTicks':'++tick_calls;return ticks;','DeviceMemory':'return backing;','IsPoweredOn':'return true;','IsShuttingDown':'return false;'}.get(name,'throw std::runtime_error("unused OS method '+name+'");')
  methods.append(signature+' override {'+body+'}')
 generated=O/'query_cache_os.h';generated.write_text('#pragma once\n#include "nxemu-module-spec/operating_system.h"\nstruct OperatingSystemMock final : IOperatingSystem { Memory& backing; unsigned tick_calls{}; static constexpr uint64_t ticks=0x99887766aa00ULL; explicit OperatingSystemMock(Memory& m):backing(m){}\n'+'\n'.join(methods)+'\n};\n')
 report={'capture_predicate':a.capture_predicate,'expected_no_producer_defect':a.expect_no_producer_defect,'observer_effect':'capture mode inserts 8byte GPU copy and Finish between HCR selection and consumer draw; CPU callbacks are not drained; explicit unobserved comparison required','expected_equality_mismatches':a.expect_equality_mismatches,'expected_validation_errors':a.expect_validation_errors,'scope':'actual production QueryCacheRuntime/BufferCache/SamplesStreamer with actual GPU query draws to hostconditional consumer pixels; real guest memory and CPU condition differential','commands':[]}
 sources=[R/'tools/query_gpu_harness.cpp',a.query_source.resolve(strict=True) if a.query_source else R/'src/yuzu_video_core/renderer_vulkan/vk_query_cache.cpp',R/'src/yuzu_video_core/renderer_vulkan/vk_scheduler.cpp',a.device_source.resolve(strict=True) if a.device_source else R/'src/yuzu_video_core/vulkan_common/vulkan_device.cpp',R/'src/nxemu-video/video_settings.cpp',R/'src/nxemu-video/video_enum_strings.cpp']
 if a.compute_source:sources.insert(1,a.compute_source.resolve(strict=True))
 includes=[O,R/'tools',R/'src',R/'src/nxemu-video',R/'src/yuzu_video_core',R/'src/yuzu_video_core/renderer_vulkan',R/'external/boost',R/'external/fmt/include',R/'external/Vulkan-Headers/include',R/'external/vulkan-memory-allocator/include',R/'external/vulkan_utility_libraries/include',B/'src',B/'generated']
 if a.include_overlay:includes.insert(2,a.include_overlay.resolve(strict=True))
 ninja=(B/'CMakeFiles/impl-Release.ninja').read_text();start=ninja.index('build D$:\\project\\nxemu-switch2-upgrade\\repo\\modules\\x64\\video\\Release\\nxemu-video.dll ')
 text=ninja[start:];line=re.search(r'^  LINK_LIBRARIES = (.+)$',text,re.M).group(1)
 libs=[]
 for part in line.split():
  p=Path(part);p=p if p.is_absolute() else B/p
  libs.append(str(p) if p.is_file() else part)
 exe=O/'query_gpu.exe'
 command=['cl','/nologo','/std:c++20','/utf-8','/EHsc','/MT','/Od','/Gy','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/DARCHITECTURE_x86_64=1',*[f'/I{x}' for x in includes],*map(str,sources),f'/Fe:{exe}',f'/Fo:{O}/','/link','/OPT:REF',*libs,'ws2_32.lib','winmm.lib','bcrypt.lib','crypt32.lib','iphlpapi.lib','psapi.lib','shlwapi.lib','userenv.lib','wtsapi32.lib','version.lib']
 inputs=[Path(__file__),*sources,R/'tools/query_guest_fixture.h',R/'tools/query_gpu_fixture.h',generated,header,R/'src/yuzu_video_core/renderer_vulkan/vk_query_cache.h',R/'src/yuzu_video_core/renderer_vulkan/samples_backend_lab.h',R/'src/yuzu_video_core/renderer_vulkan/vk_compute_pass.h',R/'src/yuzu_video_core/renderer_vulkan/vk_compute_pass.cpp',R/'src/yuzu_video_core/host_shaders/resolve_conditional_render.comp',R/'src/yuzu_video_core/renderer_vulkan/vk_scheduler.h',R/'src/yuzu_video_core/vulkan_common/vulkan_device.h',R/'src/yuzu_video_core/query_cache/query_cache.h',R/'src/yuzu_video_core/query_cache/query_stream.h',R/'src/yuzu_video_core/query_cache/query_base.h',R/'src/yuzu_video_core/device_memory_manager.h',R/'src/yuzu_video_core/renderer_vulkan/vk_buffer_cache.h',*[Path(x) for x in libs if Path(x).is_file()]]
 inputs.extend([R/'src/yuzu_video_core/vulkan_common/vk_recipe_features.h',B/'src/yuzu_video_core/host_shaders/include/yuzu_video_core/host_shaders/resolve_conditional_render_comp_spv.h'])
 if a.include_overlay:inputs.extend(p for p in a.include_overlay.resolve(strict=True).rglob('*') if p.is_file())
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
  run_cmd=[str(exe),str(O/'sample.vert.spv'),str(O/'sample.frag.spv'),str(a.expect_equality_mismatches),'1' if a.capture_predicate else '0','1' if a.expect_no_producer_defect else '0','1' if a.expect_history_defect else '0','1' if a.expect_accumulation_defect else '0']
  execution_hashes={str(p):sha(p) for p in map(Path,run_cmd[:3])};report['execution_input_hashes']=execution_hashes
  try:
   run=subprocess.run(run_cmd,capture_output=True,text=True,timeout=60)
   report['run']=dict(command=subprocess.list2cmdline(run_cmd),input='28cases: three carry edges (discarded Reset slot, pending old Report then Reset, unreported newer draw) plus two crossWFI Draw256+Draw256 equality consumers plus four actual historical bankless GPU consumers (256/512, deferred/retired producers) plus four Conditional count0/256 x mode0/1 plus ten zero-pair Equality offset/page-boundary cases, four Equal24 actualGPU-written comparison cases and no-producer public-flags observation; actual GPU query/WFI/hostconditional pixels/commit/fence/pop/CPU condition',exit_status=run.returncode,literal_stdout=run.stdout,literal_stderr=run.stderr)
   conditional=re.search(r'unresolved_conditional_mismatches=(\d+)',run.stdout);no_producer=re.search(r'QUERY_GPU_NO_PRODUCER_OBSERVATION[^\n]* mismatch=(\d+)',run.stdout)
   report['unresolved_observations']={'conditional_mismatches':int(conditional.group(1)) if conditional else None,'no_producer_mismatches':int(no_producer.group(1)) if no_producer else None,'full_semantics_acceptance':False,'no_producer_expected_defect':a.expect_no_producer_defect}
   report['execution_inputs_unchanged']=all(sha(Path(p))==h for p,h in execution_hashes.items())
   predicate=re.search(r'QUERY_GPU_PREDICATE_BUFFER[^\n]* gpu_low=(\d+) gpu_high=(\d+)',run.stdout);report['predicate_capture']={'available':bool(predicate),'gpu_low':int(predicate.group(1)) if predicate else None,'gpu_high':int(predicate.group(2)) if predicate else None,'bytes':8 if predicate else 0,'source':'actual BC NoSynchronize region after HCR choice; no CPU upload or callbacks drained'}
   history_marker='QUERY_GPU_HISTORY_DEFECT_CONFIRMED' if a.expect_history_defect else 'QUERY_GPU_HISTORY_STAGE_PASS'
   report['history_cases_verified']=run.stdout.count(history_marker)==4
   report['expected_history_defect']=a.expect_history_defect
   accumulation_marker='QUERY_GPU_ACCUMULATION_DEFECT_CONFIRMED' if a.expect_accumulation_defect else 'QUERY_GPU_ACCUMULATION_STAGE_PASS'
   report['accumulation_cases_verified']=run.stdout.count(accumulation_marker)==2
   report['expected_accumulation_defect']=a.expect_accumulation_defect
   report['carry_edges_verified']=run.stdout.count('QUERY_GPU_CARRY_EDGE_PASS')==3
   passed=(report['carry_edges_verified'] and report['accumulation_cases_verified'] and report['history_cases_verified'] and (not a.capture_predicate or bool(predicate)) and ('QUERY_GPU_NO_PRODUCER_DEFECT_CONFIRMED' if a.expect_no_producer_defect else 'QUERY_GPU_NO_PRODUCER_STAGE_PASS') in run.stdout and report['execution_inputs_unchanged'] and ('QUERY_GPU_EQUALITY_DEFECT_CONFIRMED' if a.expect_equality_mismatches else 'QUERY_GPU_EQUALITY_STAGE_PASS') in run.stdout and f'SAMPLES_VALIDATION errors={a.expect_validation_errors} warnings=0' in run.stdout and run.returncode==(1 if a.expect_validation_errors else 0) and (not a.expect_validation_errors or 'LAB_ERROR Vulkan validation errors' in run.stderr))
  except subprocess.TimeoutExpired as e:report['run']=dict(command=subprocess.list2cmdline(run_cmd),exit_status=124,watchdog_seconds=60,literal_stdout=str(e.stdout),literal_stderr=str(e.stderr))
 report['inputs_unchanged']=all(sha(Path(p))==h for p,h in hashes.items());passed&=report['inputs_unchanged'];report['passed']=passed;report['exit_status']=0 if passed else 1
 report['command']=subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]])
 (O/'RESULT.json').write_text(json.dumps(report,indent=2));print('QUERY_GPU_TEST_PASS' if passed else 'QUERY_GPU_TEST_FAILED');return report['exit_status']
if __name__=='__main__':raise SystemExit(main())
