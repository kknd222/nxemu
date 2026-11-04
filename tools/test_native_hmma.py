"""Bounded explicit test-process HMMA numerical backend experiments; production unchanged."""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, sys, traceback
import test_native_local_spill as support

def make_cpp():
    s = support.CPP[:support.CPP.index('int main(')]
    s = s.replace('return {1,1,1};', 'return {std::getenv("NXEMU_HMMA_THREADS")?u32(std::stoul(std::getenv("NXEMU_HMMA_THREADS"))):32U,1,1};')
    s = s.replace('struct Gpu {', 'std::filesystem::path actual_file;\nstruct Gpu {')
    s = s.replace('std::string name;', 'std::string name;bool control32{};VkPhysicalDeviceSubgroupSizeControlFeaturesEXT subgroup_enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT};VkPhysicalDeviceShaderFloat16Int8Features fp16_enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};')
    marker = 'vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);'
    query = r'''VkPhysicalDeviceSubgroupSizeControlPropertiesEXT sizes{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT};VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};VkPhysicalDeviceFloatControlsProperties floats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};sizes.pNext=&floats;subgroup.pNext=&sizes;VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props2.pNext=&subgroup;vkGetPhysicalDeviceProperties2(physical,&props2);VkPhysicalDeviceSubgroupSizeControlFeaturesEXT available_control{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT};VkPhysicalDeviceFeatures2 feats2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};VkPhysicalDeviceShaderFloat16Int8Features available_fp16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};available_control.pNext=&available_fp16;feats2.pNext=&available_control;vkGetPhysicalDeviceFeatures2(physical,&feats2);
std::cout<<"SUBGROUP native="<<subgroup.subgroupSize<<" min="<<sizes.minSubgroupSize<<" max="<<sizes.maxSubgroupSize<<" feature="<<available_control.subgroupSizeControl<<" requiredStages="<<sizes.requiredSubgroupSizeStages<<'\n';
std::cout<<"FLOAT_CONTROLS RTE16="<<floats.shaderRoundingModeRTEFloat16<<" RTZ16="<<floats.shaderRoundingModeRTZFloat16<<" independence="<<floats.roundingModeIndependence<<'\n';if(std::getenv("NXEMU_HMMA_RTE16")&&!floats.shaderRoundingModeRTEFloat16)throw std::runtime_error("RTE16 execution mode unsupported");if(!(subgroup.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT)||!(subgroup.supportedOperations&VK_SUBGROUP_FEATURE_SHUFFLE_BIT))throw std::runtime_error("compute subgroup shuffle unsupported");
u32 extension_count{};ok(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extension_count,nullptr));std::vector<VkExtensionProperties> extensions(extension_count);ok(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extension_count,extensions.data()));bool extension_ok=false;for(const auto& x:extensions)if(std::string(x.extensionName)==VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)extension_ok=true;
fp16_enabled.shaderFloat16=std::getenv("NXEMU_HMMA_NATIVE_HALF")!=nullptr;std::cout<<"SHADER_FLOAT16 available="<<available_fp16.shaderFloat16<<" enabled="<<fp16_enabled.shaderFloat16<<'\n';if(fp16_enabled.shaderFloat16&&!available_fp16.shaderFloat16)throw std::runtime_error("native-half requested but unsupported");control32=extension_ok&&available_control.subgroupSizeControl&&sizes.minSubgroupSize<=32&&sizes.maxSubgroupSize>=32&&(sizes.requiredSubgroupSizeStages&VK_SHADER_STAGE_COMPUTE_BIT);
if(!control32&&subgroup.subgroupSize!=32&&!(subgroup.subgroupSize==64&&std::getenv("NXEMU_HMMA_HOST64_PARTITION")))throw std::runtime_error("no guaranteed guest warp32; stop before HMMA");std::cout<<"HOST64_LOGICAL32_PARTITION "<<(subgroup.subgroupSize==64&&!control32)<<'\n';subgroup_enabled.subgroupSizeControl=control32;subgroup_enabled.computeFullSubgroups=control32&&available_control.computeFullSubgroups;std::cout<<"GUEST_WARP32_CONTRACT required_subgroup_request="<<control32<<" native32="<<(subgroup.subgroupSize==32)<<'\n';
'''
    s = s.replace(marker, query + marker, 1)
    s = s.replace('dc.pEnabledFeatures=&enabled;', 'dc.pEnabledFeatures=&enabled;std::vector<const char*> enabled_ext;if(control32){enabled_ext.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);fp16_enabled.pNext=&subgroup_enabled;}if(std::getenv("NXEMU_HMMA_RTE16"))enabled_ext.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);if(fp16_enabled.shaderFloat16)enabled_ext.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);dc.pNext=&fp16_enabled;dc.enabledExtensionCount=u32(enabled_ext.size());dc.ppEnabledExtensionNames=enabled_ext.data();')
    s = s.replace('bc.size=256;', 'bc.size=8192;').replace('bi{buffer,0,256}', 'bi{buffer,0,8192}').replace('memory,0,256,0,&mapped','memory,0,8192,0,&mapped').replace('mapped),64,0xdeadbeefU','mapped),2048,0xdeadbeefU')
    s = s.replace('stage.pName="main";', 'stage.pName="main";VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT};required.requiredSubgroupSize=32;if(control32){stage.pNext=&required;if(subgroup_enabled.computeFullSubgroups)stage.flags|=VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;}')
    s = s.replace('const auto* got=static_cast<const u32*>(mapped);', 'const auto* got=static_cast<const u32*>(mapped);std::ofstream dump(actual_file);for(unsigned n=0;n<expected.size();++n)dump<<std::hex<<got[n]<<\'\\n\';dump.close();')
    start = s.index('for(unsigned n=0;n<expected.size();++n)if(got[n]!=expected[n])')
    end = s.index('assert(got[expected.size()]', start)
    s = s[:start] + 'bool mismatch=false;for(unsigned n=0;n<expected.size();++n)if(got[n]!=expected[n]){if(!mismatch)std::cerr<<"GPU_MISMATCH word="<<n<<" actual="<<got[n]<<" expected="<<expected[n]<<\'\\n\';mismatch=true;}' + s[end:]
    s = s.replace('vkDestroyShaderModule(device,module,nullptr);', 'vkDestroyShaderModule(device,module,nullptr);if(mismatch)throw std::runtime_error("HMMA GPU numeric mismatch");')
    return s + Path(__file__).with_name('native_hmma_harness.cpp').read_text()

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--lower-source', type=Path)
    ap.add_argument('--mode', choices=['baseline', 'modified'], default='modified')
    ap.add_argument('--adapter-index', type=int)
    ap.add_argument('--tensor-emulate', action='store_true', required=True)
    ap.add_argument('--f2f-probe', action='store_true')
    ap.add_argument('--rte16', action='store_true')
    ap.add_argument('--skip-rounding-case', action='store_true')
    ap.add_argument('--native-half', action='store_true')
    ap.add_argument('--host64-partition', action='store_true', help='Explicit production SPIRV guest32 partition on a native64 subgroup; never claims host32')
    a = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    build = repo / 'build/windows-ninja'
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    lower = (a.lower_source or repo / 'src/yuzu_shader_recompiler/frontend/ampere/lower_to_maxwell.h').resolve(strict=True)
    override = out / 'lower_override.h'; shutil.copy2(lower, override)
    cpp = out / 'native-hmma.cpp'; cpp.write_text(make_cpp(), encoding='utf-8')
    exe = out / 'native-hmma.exe'; objects = out / 'objects'; objects.mkdir()
    sdk = Path(r'C:\VulkanSDK\1.4.357.0')
    local = repo / 'src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/load_store_local_shared.cpp'
    libs = [build / p for p in ['src/yuzu_shader_recompiler/Release/shader_recompiler.lib', 'src/common/Release/common.lib', 'src/yuzu_common/Release/yuzu_common.lib', 'external/fmt/Release/fmt.lib', 'external/sirit/src/Release/sirit.lib']]
    frozen = out / 'frozen'; frozen.mkdir(); frozen_libs = []
    for n, p in enumerate(libs):
        q = frozen / (str(n) + '-' + p.name); shutil.copy2(p, q); frozen_libs.append(q)
    command = ['cl', '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/MT', '/O2', '/I' + str(out), '/I' + str(repo / 'src'), '/I' + str(repo / 'external/boost'), '/I' + str(repo / 'external/fmt/include'), '/I' + str(build / 'src'), '/I' + str(sdk / 'Include'), str(cpp), str(local), '/Fe:' + str(exe), '/Fo:' + str(objects) + '\\', *map(str, frozen_libs), str(sdk / 'Lib/vulkan-1.lib'), '/link', '/OPT:REF', 'ws2_32.lib', 'winmm.lib', 'bcrypt.lib', 'crypt32.lib', 'iphlpapi.lib', 'psapi.lib', 'ole32.lib', 'shell32.lib']
    cmd = out / 'build.cmd'; cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n' + subprocess.list2cmdline(command) + '\n')
    paths = [Path(__file__), Path(support.__file__), Path(__file__).with_name('native_hmma_harness.cpp'), lower, override, cpp, local, *frozen_libs]
    hashes = {str(p): sha(p) for p in paths}
    report = dict(command=subprocess.list2cmdline([sys.executable, '-B', Path(__file__), *sys.argv[1:]]), compiler_command=subprocess.list2cmdline(command), mode=a.mode, host64_partition_allowed=a.host64_partition, native_half=a.native_half, f2f_probe=a.f2f_probe, rte16=a.rte16, skip_rounding_case=a.skip_rounding_case, input='single captured HMMA K16, controlled test-only tensor=1, 32 lanes, asymmetric matrix packing, actual CFG/SSA/SPIRV/Vulkan, SHFL31, independent matrix oracle; not game replay', hashes=hashes)
    try:
        b = subprocess.run(['cmd', '/c', str(cmd)], capture_output=True, text=True, timeout=150)
        (out / 'build.log').write_text(b.stdout + b.stderr); report.update(stage='compile', exit_status=b.returncode, literal_stdout=b.stdout, literal_stderr=b.stderr); assert b.returncode == 0
        native = [str(exe), a.mode, str(out / 'cases')]
        import os
        env = os.environ.copy()
        env['EDEN_NX2_TENSOR'] = '1'
        if a.host64_partition: env['NXEMU_HMMA_HOST64_PARTITION'] = '1'
        if a.native_half: env['NXEMU_HMMA_NATIVE_HALF'] = '1'
        if a.f2f_probe: env['NXEMU_HMMA_F2F'] = '1'
        if a.rte16: env['NXEMU_HMMA_RTE16'] = '1'
        if a.skip_rounding_case: env['NXEMU_HMMA_SKIP_ROUNDING'] = '1'
        if a.adapter_index is not None:
            assert a.adapter_index >= 0
            env['NXEMU_LOCAL_SPILL_ADAPTER_INDEX'] = str(a.adapter_index)
        report['explicit_adapter_index'] = a.adapter_index
        r = subprocess.run(native, capture_output=True, text=True, timeout=60, env=env)
        (out / 'run.log').write_text(r.stdout + r.stderr); report.update(stage='run', native_command=subprocess.list2cmdline(native), exit_status=r.returncode, literal_stdout=r.stdout, literal_stderr=r.stderr); assert r.returncode == 0
        result = json.loads(next(x[7:] for x in r.stdout.splitlines() if x.startswith('RESULT '))); report['result'] = result
        validator = sdk / 'Bin/spirv-val.exe'; validation = []
        for spv in sorted((out / 'cases').glob('*.spv')):
            v = subprocess.run([str(validator), '--target-env', 'vulkan1.1', str(spv)], capture_output=True, text=True, timeout=15)
            validation.append(dict(path=str(spv), sha256=sha(spv), exit_status=v.returncode, literal_stdout=v.stdout, literal_stderr=v.stderr)); assert v.returncode == 0
        report['SPIRV_validation'] = validation
        assert all(sha(Path(p)) == h for p, h in hashes.items()), 'Frozen input changed during test'
        report.update(stage='complete', inputs_unchanged=True, executable_sha256=sha(exe))
        (out / 'RESULT.json').write_text(json.dumps(report, indent=2))
        print('NATIVE_HMMA_PASS; mode=' + a.mode + '; actual_backend=True; GPU_cases=' + str(result['GPU_cases']))
    except BaseException:
        report['failure'] = traceback.format_exc(); (out / 'FAILURE.json').write_text(json.dumps(report, indent=2)); raise

if __name__ == '__main__': main()
