"""Compile actual pair-publication header and test actual SDK parsing with fake GPU data.

Real filesystem/production helper tests; no renderer, command-buffer or GPU numerical
execution. Use a fresh output directory because deliberate IO failure artifacts remain.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys
import time
import traceback

CPP = r'''
#include "yuzu_video_core/paired_pixel_capture.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <limits>
using namespace VideoCommon::PairedPixelCapture;
namespace D=VideoCommon::DiagnosticFrameCapture;
std::string id(unsigned n){std::ostringstream s;s<<std::hex<<std::setw(32)<<std::setfill('0')<<n;return s.str();}
D::Ticket ticket(const std::filesystem::path& root,unsigned n){D::Ticket t;t.root=root;t.request.id=id(n);t.request.run_id="test-run";t.request.generation="3";t.request.epoch_ms=D::EpochMs();return t;}
Image image(std::span<const uint8_t> bytes,uint32_t format=37){Image i;i.width=2;i.height=2;i.format=format;i.mip=2;i.layer=3;i.image=101;i.view=102;i.gpu_completed=true;i.pixels=bytes;i.reason.clear();return i;}
std::string contents(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
std::filesystem::path pair(const std::filesystem::path& root,unsigned n){return root/("frame-capture-"+id(n)+".pair.json");}
void publish(const std::filesystem::path& root,unsigned n,const Image& s,const Image& t){assert(Publish(ticket(root,n),100+n,s,t,12345));assert(std::filesystem::is_regular_file(pair(root,n)));}
int main(int argc,char** argv){
 assert(argc==2);const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
 static_assert(MaxImageBytes==8*1024*1024);
 assert(Bytes(2,2)==16&&Bytes(2048,1024)==MaxImageBytes);
 assert(Bytes(2048,1025)==0&&Bytes(0,1)==0&&Bytes(1,0)==0);
 assert(Bytes(std::numeric_limits<uint32_t>::max(),std::numeric_limits<uint32_t>::max())==0);
 for(auto f:{37u,43u,44u,50u,51u,57u})assert(Supported(f));
 for(auto f:{0u,1u,36u,38u,45u,999u})assert(!Supported(f));
 assert(Bgra(44)&&Bgra(50)&&!Bgra(37)&&!Bgra(51));
 std::vector<uint8_t> rgba{1,2,3,0, 4,5,6,127, 7,8,9,255, 0,0,0,255};
 auto bgra=rgba;for(size_t n=0;n<bgra.size();n+=4)std::swap(bgra[n],bgra[n+2]);
 auto s=image(rgba);auto t=image(bgra,44);t.image=201;t.view=202;
 publish(root,1,s,t);
 assert(contents(root/("frame-capture-"+id(1)+".source.ppm"))==contents(root/("frame-capture-"+id(1)+".target.ppm")));
 std::vector<uint8_t> zero(16,0);auto z=image(zero);publish(root,2,z,z);
 auto opaque=zero;for(size_t n=3;n<opaque.size();n+=4)opaque[n]=255;auto o=image(opaque);publish(root,3,o,o);
 // Mapping classifications are encoded-byte facts, not sRGB-linear/shader numerical values.
 const std::array<std::array<uint32_t,4>,5> mappings{{{2,2,2,2},{0,0,0,2},{5,4,3,6},{3,4,5,3},{1,1,1,1}}};
 for(unsigned n=0;n<mappings.size();++n){auto a=n<2?z:s;a.swizzle_known=true;a.identity_swizzle=false;a.swizzle=mappings[n];publish(root,100+n,a,a);}
 {auto a=s;a.swizzle_known=true;a.swizzle={7,0,0,0};publish(root,105,a,t);}
 for(unsigned format:{43u,50u,51u,57u}){auto a=image(Bgra(format)?std::span<const uint8_t>(bgra):std::span<const uint8_t>(rgba),format);publish(root,format,a,a);}
 auto bad=s;bad.format=999;publish(root,4,bad,t);
 bad=s;bad.samples=4;publish(root,5,bad,t);
 bad=s;bad.width=2048;bad.height=1025;publish(root,6,bad,t);
 bad=s;bad.width=0;publish(root,7,bad,t);
 bad=s;bad.pixels=std::span<const uint8_t>(rgba.data(),15);publish(root,8,bad,t);
 bad=s;bad.gpu_completed=false;bad.reason="not_reached";publish(root,9,bad,t);
 bad=s;bad.gpu_completed=false;bad.reason.clear();publish(root,10,bad,t);
 // Exact request-bound path validation: invalid IDs and invalid roots must not publish.
 for(auto name:{"", "../escape", "XYZ", "0123456789abcdef"}){auto q=ticket(root,11);q.request.id=name;assert(!Publish(q,1,s,t,0));}
 for(auto name:{std::string{},std::string(65,'x')}){auto q=ticket(root,11);q.request.run_id=name;assert(!Publish(q,1,s,t,0));q=ticket(root,11);q.request.generation=name;assert(!Publish(q,1,s,t,0));}
 auto rel=ticket("relative",12);assert(!Publish(rel,1,s,t,0));
 const auto blocker=root/"not-a-directory";{std::ofstream f(blocker);f<<"sentinel";}
 assert(!Publish(ticket(blocker,13),1,s,t,0));assert(contents(blocker)=="sentinel");
 // Pixel IO failure is reflected as unavailable, not an apparently valid captured image.
 const auto io=root/("frame-capture-"+id(14)+".source.ppm");std::filesystem::create_directory(io);
 publish(root,14,s,t);assert(std::filesystem::is_directory(io));
 // Metadata blocked by directory: no successful publication is reported.
 std::filesystem::create_directory(pair(root,15));assert(!Publish(ticket(root,15),1,s,t,0));
 bool invalid_role=false;try{(void)ImageJson(root,id(16),"../bad",s);}catch(const std::exception&){invalid_role=true;}assert(invalid_role);
 bool invalid_id=false;try{(void)ImageJson(root,"../bad","source",s);}catch(const std::exception&){invalid_id=true;}assert(invalid_id);
 // Real symlink tests are conditionally reported where Windows privileges permit them.
 unsigned symlink_passed=0,symlink_skipped=0;std::error_code ec;
 auto victim=root/"victim";{std::ofstream f(victim);f<<"unchanged";}
 auto link=root/("frame-capture-"+id(17)+".source.rgba");std::filesystem::create_symlink(victim,link,ec);
 if(!ec){publish(root,17,s,t);assert(contents(victim)=="unchanged");++symlink_passed;}else ++symlink_skipped;
 ec.clear();auto rootlink=root/"root-link";std::filesystem::create_directory_symlink(root,rootlink,ec);
 if(!ec){assert(!Publish(ticket(rootlink,18),1,s,t,0));++symlink_passed;}else ++symlink_skipped;
 ec.clear();auto metalink=pair(root,19);std::filesystem::create_symlink(victim,metalink,ec);
 if(!ec){assert(!Publish(ticket(root,19),1,s,t,0));assert(contents(victim)=="unchanged");++symlink_passed;}else ++symlink_skipped;
 std::cout<<"{\"passed\":true,\"actual_header\":true,\"fake_GPU_data\":true,\"GPU_executed\":false,\"max_image_bytes\":"<<MaxImageBytes<<",\"symlink_passed\":"<<symlink_passed<<",\"symlink_skipped\":"<<symlink_skipped<<"}\n";
}
'''


def validate_header_files(files):
    prefix = lambda n: 'frame-capture-' + f'{n:032x}'
    read = lambda n: json.loads((files / (prefix(n) + '.pair.json')).read_text())
    first = read(1)
    assert first['schema'] == 1 and first['composite_ordinal'] == 101
    assert first['request_id'] == f'{1:032x}' and first['run_id'] == 'test-run'
    assert first['generation'] == '3' and first['readback_ns'] == 12345
    for role in ['source', 'target']:
        data = first[role]
        assert data['status'] == 'captured' and data['bytes'] == 16
        assert data['mip'] == 2 and data['layer'] == 3 and data['gpu_readback_completed']
        assert data['exact_black_pixels'] == 1
        assert not data['all_rgb_zero'] and not data['all_rgba_zero']
        assert (data['alpha_min'], data['alpha_max']) == (0, 255)
        assert (data['alpha_zero_pixels'], data['alpha_full_pixels']) == (1, 2)
        assert data['raw_order'] == ('RGBA8' if role == 'source' else 'BGRA8')
    rgb = bytes([1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0])
    for role in ['source', 'target']:
        assert (files / first[role]['ppm_file']).read_bytes() == b'P6\n2 2\n255\n' + rgb
    zero, opaque = read(2)['source'], read(3)['source']
    assert zero['all_rgb_zero'] and zero['all_rgba_zero'] and zero['alpha_zero_pixels'] == 4
    assert opaque['all_rgb_zero'] and not opaque['all_rgba_zero'] and opaque['alpha_full_pixels'] == 4
    for unknown in [first['source'], zero, opaque]:
        assert unknown['swizzle_known'] is False and unknown['component_swizzle'] is None
        for field in ['view_all_rgb_zero', 'view_alpha_zero_pixels', 'view_alpha_full_pixels',
                      'view_alpha_min_encoded', 'view_alpha_max_encoded']:
            assert unknown[field] is None
    mapped_expect = {
        100: ([2, 2, 2, 2], False, 0, 4, 255, 255),
        101: ([0, 0, 0, 2], True, 0, 4, 255, 255),
        102: ([5, 4, 3, 6], False, 1, 2, 0, 255),
        103: ([3, 4, 5, 3], False, 1, 0, 0, 7),
        104: ([1, 1, 1, 1], True, 4, 0, 0, 0),
    }
    for n, expect in mapped_expect.items():
        d = read(n)['source']
        assert d['status'] == 'captured' and d['swizzle_known'] is True
        actual = (d['component_swizzle'], d['view_all_rgb_zero'], d['view_alpha_zero_pixels'],
                  d['view_alpha_full_pixels'], d['view_alpha_min_encoded'], d['view_alpha_max_encoded'])
        assert actual == expect, (n, actual, expect)
        original = read(2 if n < 102 else 1)['source']
        assert (files / d['rgba_file']).read_bytes() == (files / original['rgba_file']).read_bytes()
        assert (files / d['ppm_file']).read_bytes() == (files / original['ppm_file']).read_bytes()
    for fmt in [43, 50, 51, 57]:
        d = read(fmt)['source']
        assert d['status'] == 'captured' and d['vk_format'] == fmt
        assert (files / d['ppm_file']).read_bytes() == b'P6\n2 2\n255\n' + rgb
    expected = {4: 'unsupported_format', 5: 'unsupported_samples', 6: 'invalid_extent_or_byte_budget',
                7: 'invalid_extent_or_byte_budget', 8: 'pixel_size_mismatch', 9: 'not_reached',
                10: 'gpu_not_completed', 14: 'pixel_file_io_failed', 105: 'unsupported_component_swizzle'}
    for n, reason in expected.items():
        d = read(n)['source']
        assert d['status'] == 'unavailable' and d['reason'] == reason
        assert d['bytes'] == 0 and d['rgba_file'] is None and d['ppm_file'] is None
        assert d['all_rgb_zero'] is None and d['alpha_min'] is None
        assert d['view_all_rgb_zero'] is None and d['view_alpha_min_encoded'] is None
        assert read(n)['target']['status'] == 'captured'
    assert (files / (prefix(15) + '.pair.json')).is_dir()
    # Every published metadata object claiming captured pixels resolves to complete binary/PPM files.
    count = 0
    for path in files.glob('*.pair.json'):
        if not path.is_file() or path.is_symlink():
            continue
        data = json.loads(path.read_text())
        for role in ['source', 'target']:
            d = data[role]
            if d['status'] == 'captured':
                assert (files / d['rgba_file']).stat().st_size == d['bytes']
                raw = (files / d['ppm_file']).read_bytes()
                assert raw.startswith(f"P6\n{d['width']} {d['height']}\n255\n".encode())
        count += 1
    return count


def sdk_tests(root, sdk_source, out):
    """Call actual NxEmuSDK methods; only the GPU publisher is simulated."""
    sys.path.insert(0, str(root / 'tools'))
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    source_before = sha(sdk_source)
    parser_source = root / 'tools/paired_pixels.py'
    parser_before = sha(parser_source) if parser_source.is_file() else None
    namespace = {'__name__': 'paired_sdk_fixture', '__file__': str(sdk_source)}
    exec(compile(sdk_source.read_text(encoding='utf-8'), str(sdk_source), 'exec'), namespace)
    Base = namespace['NxEmuSDK']
    modified = hasattr(Base, 'paired_pixels')
    out.mkdir(parents=True, exist_ok=True)
    valid_swizzles = {'swizzle_all_one': [2, 2, 2, 2], 'swizzle_alpha_one': [0, 0, 0, 2],
                     'swizzle_swap_rb': [5, 4, 3, 6], 'swizzle_alpha_from_r': [3, 4, 5, 3],
                     'swizzle_all_zero': [1, 1, 1, 1]}
    scenarios = ['missing', 'valid', 'invalid', 'wrong_session', 'unavailable', 'basename', 'raw_short',
                 'ppm_wrong', 'stats_wrong', 'unknown_format', 'oversized', 'bad_composite',
                 'sample_bool', 'format_float', 'stats_float', 'black_count_bool',
                 *valid_swizzles, 'swizzle_unknown', 'swizzle_enum', 'swizzle_bool', 'swizzle_length',
                 'swizzle_stat_wrong', 'swizzle_stat_float', 'swizzle_unknown_derived'] if modified else ['baseline']
    invalid = set(scenarios) - {'missing', 'valid', 'wrong_session', 'unavailable', 'baseline',
                               *valid_swizzles, 'swizzle_unknown'}
    rows = []

    def publish_pair(directory, request_id, scenario):
        def meta(role):
            prefix = 'frame-capture-' + request_id + '.' + role
            raw = bytes([1, 2, 3, 255] * 4)
            ppm = b'P6\n2 2\n255\n' + bytes([1, 2, 3] * 4)
            (directory / (prefix + '.rgba')).write_bytes(raw)
            (directory / (prefix + '.ppm')).write_bytes(ppm)
            return {'status': 'captured', 'reason': '', 'width': 2, 'height': 2, 'vk_format': 37,
                    'raw_order': 'RGBA8', 'mip': 0, 'layer': 0, 'samples': 1, 'image': 1, 'view': 2,
                    'gpu_readback_completed': True, 'bytes': 16, 'rgba_file': prefix + '.rgba',
                    'ppm_file': prefix + '.ppm', 'exact_black_pixels': 0, 'all_rgb_zero': False,
                    'all_rgba_zero': False, 'alpha_min': 255, 'alpha_max': 255, 'alpha_zero_pixels': 0,
                    'alpha_full_pixels': 4, 'swizzle_known': True, 'identity_swizzle': True,
                    'component_swizzle': [0, 0, 0, 0], 'view_all_rgb_zero': False,
                    'view_alpha_zero_pixels': 0, 'view_alpha_full_pixels': 4,
                    'view_alpha_min_encoded': 255, 'view_alpha_max_encoded': 255}
        pair = {'schema': 1, 'request_id': request_id, 'run_id': 'r', 'generation': '3',
                'composite_ordinal': 1, 'snapshot_epoch_ms': int(time.time() * 1000),
                'source': meta('source'), 'target': meta('target'), 'scope': 'fake GPU test evidence'}
        source = pair['source']
        if scenario == 'wrong_session':
            pair['generation'] = '99'
        elif scenario == 'unavailable':
            pair['target'] = {'status': 'unavailable', 'reason': 'not_reached'}
        elif scenario == 'basename':
            source['rgba_file'] = '../escape.rgba'
        elif scenario == 'raw_short':
            (directory / source['rgba_file']).write_bytes(b'short')
        elif scenario == 'ppm_wrong':
            (directory / source['ppm_file']).write_bytes(b'P6\n2 2\n255\n' + b'\x00' * 12)
        elif scenario == 'stats_wrong':
            source['alpha_min'] = 1
        elif scenario == 'unknown_format':
            source['vk_format'] = 999
        elif scenario == 'oversized':
            source['width'] = 8 * 1024 * 1024
        elif scenario == 'bad_composite':
            pair['composite_ordinal'] = True
        elif scenario == 'sample_bool':
            source['samples'] = True
        elif scenario == 'format_float':
            source['vk_format'] = 37.0
        elif scenario == 'stats_float':
            source['alpha_min'] = 255.0
        elif scenario == 'black_count_bool':
            source['exact_black_pixels'] = False
        elif scenario in valid_swizzles:
            mapping = valid_swizzles[scenario]
            source['component_swizzle'] = mapping
            source['identity_swizzle'] = False
            encoded = [1, 2, 3, 255]
            view = [encoded[i] if c == 0 else (0 if c == 1 else (255 if c == 2 else encoded[c - 3]))
                    for i, c in enumerate(mapping)]
            source.update(view_all_rgb_zero=not any(view[:3]), view_alpha_zero_pixels=4 if view[3] == 0 else 0,
                          view_alpha_full_pixels=4 if view[3] == 255 else 0,
                          view_alpha_min_encoded=view[3], view_alpha_max_encoded=view[3])
        elif scenario == 'swizzle_unknown':
            source.update(swizzle_known=False, identity_swizzle=False, component_swizzle=None,
                          view_all_rgb_zero=None, view_alpha_zero_pixels=None, view_alpha_full_pixels=None,
                          view_alpha_min_encoded=None, view_alpha_max_encoded=None)
        elif scenario == 'swizzle_enum':
            source['component_swizzle'] = [7, 0, 0, 0]
        elif scenario == 'swizzle_bool':
            source['component_swizzle'] = [True, 0, 0, 0]
        elif scenario == 'swizzle_length':
            source['component_swizzle'] = [0, 0, 0]
        elif scenario == 'swizzle_stat_wrong':
            source['view_alpha_min_encoded'] = 0
        elif scenario == 'swizzle_stat_float':
            source['view_alpha_min_encoded'] = 255.0
        elif scenario == 'swizzle_unknown_derived':
            source['swizzle_known'] = False
            source['component_swizzle'] = None
        text = 'invalid JSON' if scenario == 'invalid' else json.dumps(pair)
        (directory / ('frame-capture-' + request_id + '.pair.json')).write_text(text, encoding='utf-8')

    for scenario in scenarios:
        bridge = out / scenario
        bridge.mkdir(exist_ok=True)
        class Fake(Base):
            def status(self):
                request = bridge / 'diagnostics/frame-capture-request.json'
                if request.is_file():
                    data = json.loads(request.read_text())
                    request_id, directory = data['id'], request.parent
                    # Publish the optional pair before marking legacy capture complete, matching the backend contract.
                    if scenario not in ['missing', 'baseline']:
                        publish_pair(directory, request_id, scenario)
                    (directory / ('frame-capture-' + request_id + '.ppm')).write_bytes(
                        b'P6\n1280 720\n255\n' + b'\x01' * (1280 * 720 * 3))
                    (directory / ('frame-capture-' + request_id + '.json')).write_text(json.dumps(
                        {'request_id': request_id, 'run_id': 'r', 'generation': '3',
                         'status': 'completed', 'gpu_readback_completed': True}), encoding='utf-8')
                return {'fresh': True, 'video_snapshot_available': True,
                        'diagnostic_config': {'frame_capture': True}, 'run_id': 'r', 'module_generation': 3}
        result = Fake(bridge).capture_frame(1)
        assert result['result'] == 'captured' and result['all_rgb_zero'] is False, scenario
        if scenario == 'baseline':
            assert 'paired_pixels' not in result
        elif scenario == 'missing':
            assert not result['paired_pixels']['available'] and result['paired_pixels']['reason'] == 'pair_pending_or_disabled'
        elif scenario in invalid:
            assert not result['paired_pixels']['available'] and result['paired_pixels']['reason'] == 'pair_invalid', (scenario, result['paired_pixels'])
        else:
            pair = result['paired_pixels']
            assert pair['available'] and pair['matches_current_session'] == (scenario != 'wrong_session'), scenario
            assert pair['images']['source']['available'], scenario
            assert pair['images']['target']['available'] == (scenario != 'unavailable'), scenario
            assert len(pair['images']['source']['rgba_sha256']) == 64
            if scenario == 'swizzle_unknown':
                assert pair['images']['source']['data']['view_all_rgb_zero'] is None
            if scenario in valid_swizzles:
                assert pair['images']['source']['data']['component_swizzle'] == valid_swizzles[scenario]
        rows.append({'scenario': scenario, 'legacy_pixel_sha256': result['sha256'], 'pair': result.get('paired_pixels')})
    assert len({r['legacy_pixel_sha256'] for r in rows}) == 1
    if modified:
        from paired_pixels import inspect_pair
        for request_id in ['', '../escape', 'X' * 32, '0' * 31]:
            try:
                inspect_pair(out, request_id, {'run_id': 'r', 'module_generation': 3})
            except ValueError:
                pass
            else:
                raise AssertionError('Malformed UUID accepted')
    record = {'actual_SDK_method': True, 'fake_GPU_publisher': True, 'real_filesystem': True,
              'GPU_executed': False, 'baseline_pair_api_absent': not modified,
              'source_sha256': hashlib.sha256(sdk_source.read_bytes()).hexdigest(), 'cases': rows}
    assert sha(sdk_source) == source_before, 'SDK source changed during test'
    if modified:
        assert sha(parser_source) == parser_before, 'Parser source changed during test'
        record['parser_sha256'] = parser_before
    record['inputs_unchanged'] = True
    (out / 'RESULT.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sdk-source', type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve(strict=True)
    if (root / 'repo/src').is_dir():
        root = root / 'repo'
    header = root / 'src/yuzu_video_core/paired_pixel_capture.h'
    assert header.is_file()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    files = out / 'files'
    assert not files.exists(), 'Use a new output directory; IO failures intentionally retained'
    cpp = out / 'paired-pixels-test.cpp'
    cpp.write_text(CPP, encoding='utf-8')
    exe, obj = out / 'paired-pixels-test.exe', out / 'paired-pixels-test.obj'
    compiler = ['cl', '/nologo', '/std:c++20', '/EHsc', '/MT', '/O2', '/utf-8',
                '/I' + str(root / 'src'), str(cpp), '/Fe:' + str(exe), '/Fo:' + str(obj)]
    cmd = out / 'build.cmd'
    cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
                   'if errorlevel 1 exit /b %errorlevel%\n' + subprocess.list2cmdline(compiler) + '\n', encoding='utf-8')
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    hashes = {str(p): sha(p) for p in [header, root / 'src/yuzu_video_core/diagnostic_frame_capture.h', cpp, Path(__file__).resolve()]}
    report = {'command': subprocess.list2cmdline([sys.executable, '-B', Path(__file__).resolve(), *sys.argv[1:]]),
              'compiler_command': subprocess.list2cmdline(compiler), 'hashes': hashes,
              'input': 'actual production pair publication header; fake GPU byte payload; real filesystem; no GPU/renderer execution'}
    build = subprocess.run(['cmd', '/c', str(cmd)], capture_output=True, text=True, timeout=90)
    (out / 'build.log').write_text(build.stdout + build.stderr, encoding='utf-8')
    if build.returncode:
        report.update(stage='compile', literal_stdout=build.stdout, literal_stderr=build.stderr, exit_status=build.returncode)
        (out / 'FAILURE.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        raise SystemExit(build.returncode)
    run = subprocess.run([str(exe), str(files)], capture_output=True, text=True, timeout=30)
    (out / 'run.log').write_text(run.stdout + run.stderr, encoding='utf-8')
    report.update(native_command=subprocess.list2cmdline([str(exe), str(files)]), literal_stdout=run.stdout,
                  literal_stderr=run.stderr, exit_status=run.returncode, executable_sha256=sha(exe))
    if run.returncode:
        report['stage'] = 'run'
        (out / 'FAILURE.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        raise SystemExit(run.returncode)
    try:
        report['result'] = json.loads(run.stdout)
        report['publication_count'] = validate_header_files(files)
        assert all(sha(Path(p)) == h for p, h in hashes.items()), 'Input changed during test'
        if args.sdk_source:
            report['sdk'] = sdk_tests(root, args.sdk_source.resolve(strict=True), out / 'sdk')
    except Exception as error:
        report.update(stage='independent_serialization_or_sdk', error=str(error), python_traceback=traceback.format_exc(), exit_status=1)
        (out / 'FAILURE.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        raise
    report['inputs_unchanged'] = True
    (out / 'RESULT.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('PAIRED_PIXELS_PASS; actual_header=True; fake_GPU=True; real_filesystem=True; GPU_executed=False; sdk=' + str(bool(args.sdk_source))
          + '; baseline_pair_api_absent=' + str(report.get('sdk', {}).get('baseline_pair_api_absent', 'not_tested')))


if __name__ == '__main__':
    main()
