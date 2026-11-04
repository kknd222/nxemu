"""Actual native scalar F16 RN regression with per-operation integer-bit emitter overlay.
No global RTE mode, Int16 feature, game/ROM/driver changes. Independent Python IEEE oracle.
"""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, sys, traceback, struct, random, math
import test_native_hmma as support

def make_oracle():
    critical=[0x3f803000,0x3f801000,0x477ff000,0x387fe000,0x33000000,0x33000001,0,1,0x007fffff,0x00800000,0x32800000,0x32ffffff,0x33800000,0x387fc000,0x38800000,0x477fe000,0x477fefff,0x47800000,0x7f7fffff,0x7f800000,0x7f800001,0x7fc00000,0x7fffffff]
    bits=[]
    for x in critical: bits.extend([x,x^0x80000000])
    rng=random.Random(0x304)
    for _ in range(70): bits.append(rng.getrandbits(32))
    for e in [101,102,103,111,112,113]:
        for m in [0,0x1fff,0x7fffff]: bits.append((e<<23)|m)
    bits.extend([0]*(32- len(bits)%32 if len(bits)%32 else 0))
    result=[]
    for x in bits:
        f=struct.unpack('<f',struct.pack('<I',x))[0]
        if math.isnan(f): y=0xffffffff # quiet NaN class; payload not oracle
        else:
            try: y=struct.unpack('<H',struct.pack('<e',f))[0]
            except OverflowError: y=(x>>16&0x8000)|0x7c00
        result.append([x,y])
    return result

def make_cpp():
    s=support.make_cpp();s=s[:s.index('int main(')]
    s=s.replace('std::filesystem::path actual_file;', 'std::filesystem::path actual_file;unsigned numeric_mismatches{};')
    s=s.replace('if(got[n]!=expected[n]){if(!mismatch)', 'if(expected[n]==0xffffffffU ? !((got[n]&0x7c00U)==0x7c00U&&(got[n]&0x3ffU)!=0&&(got[n]&0x200U)!=0&&(got[n]>>16)==0) : got[n]!=expected[n]){++numeric_mismatches;if(!mismatch)')
    s=s.replace('if(mismatch)throw std::runtime_error("HMMA GPU numeric mismatch");','')
    marker='auto* last=program.blocks.back();'
    inject='if(const char* test=std::getenv("NXEMU_F16_OVERRIDE")){unsigned count{};for(auto* block:program.blocks)for(auto& inst:*block)if(inst.GetOpcode()==Opcode::ConvertF16F32){auto f=inst.Flags<IR::FpControl>();switch(std::stoul(test)){case 1:f.rounding=IR::FpRounding::RZ;break;case 2:f.rounding=IR::FpRounding::RM;break;case 3:f.rounding=IR::FpRounding::RP;break;case 4:f.fmz_mode=IR::FmzMode::FTZ;break;case 5:f.rounding=IR::FpRounding::DontCare;break;case 6:f.fmz_mode=IR::FmzMode::FMZ;break;case 7:f.fmz_mode=IR::FmzMode::DontCare;break;}inst.SetFlags(f);++count;}assert(count>0);} '
    s=s.replace(marker, inject+marker)
    return s+Path(__file__).with_name('native_f16_rounding_harness.cpp').read_text()

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--lower-source', type=Path)
    ap.add_argument('--mode', choices=['baseline', 'modified'], default='modified')
    ap.add_argument('--adapter-index', type=int)
    ap.add_argument('--tensor-emulate', action='store_true', help='Compatibility argument; scalar F2F tests do not enable tensor emulation')
    ap.add_argument('--emitter-source-root', type=Path, required=True)
    
    
    ap.add_argument('--native-half', action='store_true')
    ap.add_argument('--host64-partition', action='store_true', help='Explicit production SPIRV guest32 partition on a native64 subgroup; never claims host32')
    a = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    build = repo / 'build/windows-ninja'
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    lower = (a.lower_source or repo / 'src/yuzu_shader_recompiler/frontend/ampere/lower_to_maxwell.h').resolve(strict=True)
    override = out / 'lower_override.h'; shutil.copy2(lower, override)
    cpp = out / 'native-f16-rounding.cpp'; cpp.write_text(make_cpp(), encoding='utf-8')
    exe = out / 'native-f16-rounding.exe'; objects = out / 'objects'; objects.mkdir()
    sdk = Path(r'C:\VulkanSDK\1.4.357.0')
    emitter_convert = a.emitter_source_root.resolve() / 'yuzu_shader_recompiler/backend/spirv/emit_spirv_convert.cpp'
    emitter_header = emitter_convert.with_name('emit_spirv_instructions.h')
    emitter_dispatch = out / 'emit_spirv_dispatch.cpp'; shutil.copy2(repo / 'src/yuzu_shader_recompiler/backend/spirv/emit_spirv.cpp', emitter_dispatch)
    oracle_path = out / 'oracle-input.txt'; oracle = make_oracle(); oracle_path.write_text(''.join(f'{x:08x} {y:08x}\n' for x,y in oracle)); (out / 'ORACLE.json').write_text(json.dumps(oracle, indent=2))
    exclusion_path = out / 'exclusion-input.txt'; exclusion = []
    for x in [0x3f803000,0x3f801000,0xbf803000,0x3f80f000]*8:
        f=struct.unpack('<f',struct.pack('<I',x))[0]; y=struct.unpack('<H',struct.pack('<e',f))[0]
        if abs(struct.unpack('<e',struct.pack('<H',y))[0])>abs(f): y-=1
        exclusion.append([x,y])
    exclusion_path.write_text(''.join(f'{x:08x} {y:08x}\n' for x,y in exclusion)); (out / 'EXCLUSION_ORACLE.json').write_text(json.dumps(exclusion,indent=2))
    local = repo / 'src/yuzu_shader_recompiler/frontend/maxwell/translate/impl/load_store_local_shared.cpp'
    libs = [build / p for p in ['src/yuzu_shader_recompiler/Release/shader_recompiler.lib', 'src/common/Release/common.lib', 'src/yuzu_common/Release/yuzu_common.lib', 'external/fmt/Release/fmt.lib', 'external/sirit/src/Release/sirit.lib']]
    frozen = out / 'frozen'; frozen.mkdir(); frozen_libs = []
    for n, p in enumerate(libs):
        q = frozen / (str(n) + '-' + p.name); shutil.copy2(p, q); frozen_libs.append(q)
    command = ['cl', '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/MT', '/O2', '/I' + str(out), '/I' + str(a.emitter_source_root.resolve()), '/I' + str(repo / 'src'), '/I' + str(repo / 'external/boost'), '/I' + str(repo / 'external/sirit/include'), '/I' + str(repo / 'external/sirit/externals/SPIRV-Headers/include'), '/I' + str(repo / 'external/fmt/include'), '/I' + str(build / 'src'), '/I' + str(sdk / 'Include'), str(cpp), str(local), str(emitter_convert), str(emitter_dispatch), '/Fe:' + str(exe), '/Fo:' + str(objects) + '\\', *map(str, frozen_libs), str(sdk / 'Lib/vulkan-1.lib'), '/link', '/OPT:REF', 'ws2_32.lib', 'winmm.lib', 'bcrypt.lib', 'crypt32.lib', 'iphlpapi.lib', 'psapi.lib', 'ole32.lib', 'shell32.lib']
    cmd = out / 'build.cmd'; cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n' + subprocess.list2cmdline(command) + '\n')
    paths = [Path(__file__), Path(support.__file__), Path(support.support.__file__), Path(support.__file__).with_name('native_hmma_harness.cpp'), emitter_convert, emitter_header, emitter_dispatch, oracle_path, exclusion_path, Path(__file__).with_name('native_f16_rounding_harness.cpp'), lower, override, cpp, local, *frozen_libs]
    hashes = {str(p): sha(p) for p in paths}
    report = dict(command=subprocess.list2cmdline([sys.executable, '-B', Path(__file__), *sys.argv[1:]]), compiler_command=subprocess.list2cmdline(command), mode=a.mode, host64_partition_allowed=a.host64_partition, native_half=a.native_half, emitter_source_root=str(a.emitter_source_root), input='native 0x304 F2F RN/FmzNone; 160 IEEE independent struct.pack binary16 oracle inputs and7 controlled IR excluded flags; actual Lower/CFG/SSA/SPIRV/Vulkan; fixed shaderFloat16, host64 guest32 partition, no global RTE16/Int16; not game replay', hashes=hashes)
    try:
        b = subprocess.run(['cmd', '/c', str(cmd)], capture_output=True, text=True, timeout=180)
        (out / 'build.log').write_text(b.stdout + b.stderr); report.update(stage='compile', exit_status=b.returncode, literal_stdout=b.stdout, literal_stderr=b.stderr); assert b.returncode == 0
        native = [str(exe), a.mode, str(out / 'cases')]
        import os
        env = os.environ.copy()
        if a.host64_partition: env['NXEMU_HMMA_HOST64_PARTITION'] = '1'
        if a.native_half: env['NXEMU_HMMA_NATIVE_HALF'] = '1'
        env.pop('NXEMU_HMMA_RTE16', None)
        env['NXEMU_F16_INPUT'] = str(oracle_path)
        env['NXEMU_F16_EXCLUSION_INPUT'] = str(exclusion_path)
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
        print('NATIVE_F16_RN_PASS; mode=' + a.mode + '; actual_backend=True; GPU_cases=' + str(result['GPU_cases']))
    except BaseException:
        report['failure'] = traceback.format_exc(); (out / 'FAILURE.json').write_text(json.dumps(report, indent=2)); raise

if __name__ == '__main__': main()
