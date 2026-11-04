"""Actual QueriesPrefixScanPass GPU oracle; explicit shader selector adapter only."""
from pathlib import Path
import argparse,hashlib,json,re,struct,subprocess,sys

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);ap.add_argument('--shader-root',type=Path);ap.add_argument('--compute-source',type=Path);ap.add_argument('--variant',choices=['native','subgroup','fallback'],default='native');ap.add_argument('--cases',choices=['small','all'],default='small');ap.add_argument('--expect-defects',type=int,choices=range(9),default=0);a=ap.parse_args()
 R=Path(__file__).resolve().parents[1];B=R/'build/windows-ninja';O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
 shaders=a.shader_root.resolve(strict=True) if a.shader_root else R/'src/yuzu_video_core/host_shaders';source=a.compute_source.resolve(strict=True) if a.compute_source else R/'src/yuzu_video_core/renderer_vulkan/vk_compute_pass.cpp'
 report=dict(variant=a.variant,cases=a.cases,expected_defects=a.expect_defects,scope='actual production QueriesPrefixScanPass constructor/Run/descriptors/dispatch/barriers with actual Vulkan GPU output and accumulator readback; controlled buffer input and CPU oracle, not actual game or full QueryCacheRuntime',selector_adapter='native unchanged' if a.variant=='native' else 'constructor selects specified real shader; test-only bounded actual pushconstant trace; Run GPU semantics unchanged',commands=[],passed=False)
 compiler=Path(r'C:\VulkanSDK\1.4.357.0\Bin\glslangValidator.exe');overlay=O/'overlay/yuzu_video_core/host_shaders';overlay.mkdir(parents=True)
 shader_paths=[]
 for name,ident in [('queries_prefix_scan_sum','QUERIES_PREFIX_SCAN_SUM_COMP_SPV'),('queries_prefix_scan_sum_nosubgroups','QUERIES_PREFIX_SCAN_SUM_NOSUBGROUPS_COMP_SPV')]:
  p=shaders/(name+'.comp');shader_paths.append(p);spv=O/(name+'.spv');cmd=[str(compiler),'-V','--target-env','vulkan1.1',str(p),'-o',str(spv)];c=subprocess.run(cmd,capture_output=True,text=True,timeout=30);report['commands'].append(dict(command=subprocess.list2cmdline(cmd),exit_status=c.returncode,literal_stdout=c.stdout,literal_stderr=c.stderr));assert c.returncode==0
  data=spv.read_bytes();assert len(data)%4==0;words=struct.unpack('<'+str(len(data)//4)+'I',data);(overlay/(name+'_comp_spv.h')).write_text('#pragma once\nconst uint32_t '+ident+'[] = {'+','.join(hex(w) for w in words)+'};\n')
 code=source.read_text();adapter=O/'vk_compute_pass_selector.cpp'
 if a.variant!='native':
  start=code.index('QueriesPrefixScanPass::QueriesPrefixScanPass(');end=code.index('void QueriesPrefixScanPass::Run(',start);section=code[start:end]
  pattern=r'device_\.IsSubgroupFeatureSupported\(VK_SUBGROUP_FEATURE_BASIC_BIT\).*?: std::span<const u32>\(QUERIES_PREFIX_SCAN_SUM_NOSUBGROUPS_COMP_SPV\)\),'
  chosen='QUERIES_PREFIX_SCAN_SUM_COMP_SPV' if a.variant=='subgroup' else 'QUERIES_PREFIX_SCAN_SUM_NOSUBGROUPS_COMP_SPV'
  section,count=re.subn(pattern,'std::span<const u32>('+chosen+')),',section,flags=re.S);assert count==1
  code=code[:start]+section+code[end:]
 # Test-only telemetry records the actual normalized push-constant struct,
 # not the caller's pre-normalization checkpoint fields. Production is untouched.
 start=code.index('void QueriesPrefixScanPass::Run(');end=code.index('ASTCDecoderPass::ASTCDecoderPass',start)
 section=code[start:end];needle_push='            cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, uniforms);'
 assert section.count(needle_push)==1
 telemetry='            static unsigned prefix_probe_packets=0; if(prefix_probe_packets++<32) std::cerr << "PREFIX_PUSH_CONSTANTS min=" << uniforms.min_accumulation_base << " max=" << uniforms.max_accumulation_base << " accumulation_limit=" << uniforms.accumulation_limit << " buffer_offset=" << uniforms.buffer_offset << " actual_struct=True test_only=True\\n";\n'
 section=section.replace(needle_push,telemetry+needle_push);code='#include <iostream>\n'+code[:start]+section+code[end:]
 report['push_constant_trace']='test-only actual struct before CmdPushConstants; bounded32; no production telemetry or changed GPU semantics'
 adapter.write_text(code)
 sources=[R/'tools/prefix_backend_harness.cpp',adapter,R/'src/yuzu_video_core/renderer_vulkan/vk_scheduler.cpp',R/'src/yuzu_video_core/vulkan_common/vulkan_device.cpp',R/'src/nxemu-video/video_settings.cpp',R/'src/nxemu-video/video_enum_strings.cpp']
 includes=[O/'overlay',R/'tools',R/'src',R/'src/nxemu-video',R/'src/yuzu_video_core',R/'src/yuzu_video_core/renderer_vulkan',R/'external/boost',R/'external/fmt/include',R/'external/Vulkan-Headers/include',R/'external/vulkan-memory-allocator/include',R/'external/vulkan_utility_libraries/include',B/'src',B/'src/yuzu_video_core/host_shaders/include',B/'generated']
 ninja=(B/'CMakeFiles/impl-Release.ninja').read_text();start=ninja.index('build D$:\\project\\nxemu-switch2-upgrade\\repo\\modules\\x64\\video\\Release\\nxemu-video.dll ');line=re.search(r'^  LINK_LIBRARIES = (.+)$',ninja[start:],re.M).group(1)
 libs=[]
 for part in line.split():
  p=Path(part);p=p if p.is_absolute() else B/p;libs.append(str(p) if p.is_file() else part)
 exe=O/'prefix_backend.exe';cmd=['cl','/nologo','/std:c++20','/utf-8','/EHsc','/MT','/Od','/Gy','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/DARCHITECTURE_x86_64=1',*[f'/I{x}' for x in includes],*map(str,sources),f'/Fe:{exe}',f'/Fo:{O}/','/link','/OPT:REF',*libs,'ws2_32.lib','winmm.lib','bcrypt.lib','crypt32.lib','iphlpapi.lib','psapi.lib','shlwapi.lib','userenv.lib','wtsapi32.lib','version.lib']
 inputs=[Path(__file__).resolve(),source,compiler,*shader_paths,*sources,R/'tools/query_guest_fixture.h',R/'src/yuzu_video_core/renderer_vulkan/vk_compute_pass.h',R/'src/yuzu_video_core/renderer_vulkan/vk_scheduler.h',R/'src/yuzu_video_core/vulkan_common/vulkan_device.h',*[Path(x) for x in libs if Path(x).is_file()],*overlay.iterdir()];hashes={str(p):sha(p) for p in inputs};report['source_hashes']=hashes
 script=O/'build.cmd';script.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(cmd)+'\n')
 try:
  build=subprocess.run(['cmd','/c',str(script)],capture_output=True,text=True,timeout=180);(O/'build.log').write_text(build.stdout+build.stderr);report['build']=dict(command=subprocess.list2cmdline(cmd),exit_status=build.returncode,literal_stdout=build.stdout,literal_stderr=build.stderr)
  if build.returncode==0:
   run_cmd=[str(exe),str(a.expect_defects),a.cases];report['execution_input_hashes']={str(exe):sha(exe)};run=subprocess.run(run_cmd,capture_output=True,text=True,timeout=60);report['run']=dict(command=subprocess.list2cmdline(run_cmd),input='controlled u64 buffers: Reset0,carry256,u32carry,midReset; all adds2048/2049/4097 and Reset2048; actual prefix and accumulator plus physical guard',exit_status=run.returncode,literal_stdout=run.stdout,literal_stderr=run.stderr)
   report['actual_push_constants']=[dict(zip(['min','max','accumulation_limit','buffer_offset'],x)) for x in re.findall(r'PREFIX_PUSH_CONSTANTS min=(\d+) max=(\d+) accumulation_limit=(\d+) buffer_offset=(\d+)',run.stderr)]
   report['case_records']=[dict(zip(['name','n','initial','min','max','prefix_last','prefix_expected','accumulator','accumulator_expected','first_bad','mismatch'],x)) for x in re.findall(r'PREFIX_GPU_CASE name=(\S+) n=(\d+) initial=(\d+) min=(\d+) max=(\d+) prefix_last=(\d+) prefix_expected=(\d+) accumulator=(\d+) accumulator_expected=(\d+) first_bad=(-?\d+) mismatch=(\d+)',run.stdout)]
   report['passed']=bool(report['actual_push_constants']) and run.returncode==0 and len(report['case_records'])==(4 if a.cases=='small' else 8) and 'PREFIX_GPU_EXPECTATIONS_PASS' in run.stdout and 'PREFIX_VALIDATION errors=0 warnings=0 loader_errors=0' in run.stdout and sha(exe)==report['execution_input_hashes'][str(exe)]
 except subprocess.TimeoutExpired as e:report['timeout']=dict(watchdog_seconds=180 if not exe.exists() else 60,literal_stdout=str(e.stdout),literal_stderr=str(e.stderr))
 report['inputs_unchanged']=all(sha(Path(p))==h for p,h in hashes.items());report['passed']&=report['inputs_unchanged'];report['exit_status']=0 if report['passed'] else 1;report['command']=subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]])
 (O/'RESULT.json').write_text(json.dumps(report,indent=2));print('PREFIX_BACKEND_TEST_PASS' if report['passed'] else 'PREFIX_BACKEND_TEST_FAILED');return report['exit_status']
if __name__=='__main__':raise SystemExit(main())
