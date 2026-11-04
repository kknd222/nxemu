"""NXEmu experiment entry: bounded native cases and opt-in supervised observations.

No game input. A native case is NOT a complete GPU task replay: resources,
bindings, synchronization and numerical outputs are not captured by this layer.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess
import time

MAX_BLOB = 1024 * 1024
MAX_TOTAL = 4 * MAX_BLOB


def read_json(path, limit=262144):
    with Path(path).open('rb') as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise ValueError('JSON budget exceeded')
    return json.loads(raw.decode('utf-8-sig'))


def member(root, name, suffix):
    if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,96}' + re.escape(suffix), name):
        raise ValueError('Invalid fixture basename')
    path = root / name
    if path.is_symlink() or path.resolve().parent != root.resolve() or not path.is_file():
        raise ValueError('Fixture member missing or redirected')
    return path


def fnv1a64(raw):
    value = 14695981039346656037
    for byte in raw:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{value:016x}'


def compute_context(meta):
    """Validate copied compiler/dispatch metadata, never assert GPU resource visibility."""
    c = meta.get('compute_environment')
    if c is None:
        return {'available': False, 'reason': 'legacy_metadata_no_compute_environment'}
    if not isinstance(c, dict) or type(c.get('available')) is not bool:
        raise ValueError('Invalid compute environment')
    if type(meta.get('stage')) is not int or not 0 <= meta['stage'] <= 5:
        raise ValueError('Invalid native stage identity')
    if type(meta.get('program')) is not int or not 0 <= meta['program'] < 1 << 64:
        raise ValueError('Invalid native program identity')
    if c['available'] and meta.get('stage') != 5:
        raise ValueError('Compute environment attached to non-compute stage')
    def uint(name, maximum=(1 << 32) - 1):
        v = c.get(name)
        if type(v) is not int or not 0 <= v <= maximum:
            raise ValueError('Invalid compute field: ' + name)
    for name in ('local_memory_size', 'shared_memory_size', 'texture_bound_buffer'):
        uint(name)
    uint('cbuf_mask', 0xff)
    for name in ('dispatch_token', 'dispatch_program'):
        uint(name, (1 << 64) - 1)
    for name in ('workgroup_size', 'grid'):
        v = c.get(name)
        if not isinstance(v, list) or len(v) != 3 or any(type(x) is not int or not 0 <= x < 1 << 32 for x in v):
            raise ValueError('Invalid compute dimensions')
    if type(c.get('dispatch_available')) is not bool:
        raise ValueError('Invalid dispatch availability')
    if c['dispatch_available'] and (not c['available'] or not c['dispatch_token'] or c['dispatch_program'] != meta.get('program')):
        raise ValueError('Compute dispatch identity mismatch')
    buffers = c.get('constant_buffers')
    if not isinstance(buffers, list) or len(buffers) != 8:
        raise ValueError('Invalid compute constant-buffer count')
    for slot, b in enumerate(buffers):
        if not isinstance(b, dict) or type(b.get('slot')) is not int or b['slot'] != slot or type(b.get('enabled')) is not bool:
            raise ValueError('Invalid compute constant-buffer descriptor')
        for key, maximum in [('address', (1 << 64) - 1), ('size', (1 << 32) - 1)]:
            if type(b.get(key)) is not int or not 0 <= b[key] <= maximum:
                raise ValueError('Invalid compute buffer bounds')
        if c['dispatch_available'] and b['enabled'] != bool(c['cbuf_mask'] & (1 << slot)):
            raise ValueError('Compute constant-buffer mask mismatch')
    if not c['dispatch_available'] and (
            c['dispatch_token'] or c['dispatch_program'] or c['cbuf_mask'] or any(c['grid']) or
            any(b['enabled'] or b['address'] or b['size'] for b in buffers)):
        raise ValueError('Unavailable dispatch contains identity or bindings')
    if c.get('resource_bytes_captured') is not False or c.get('GPU_visibility_proven') is not False:
        raise ValueError('Compute metadata cannot claim resource visibility')
    return c


def inspect_cases(bridge, expected_session=None):
    root = Path(bridge) / 'diagnostics/native-fixtures'
    index_path = root / 'index.json'
    if not index_path.is_file():
        return {'available': False, 'reason': 'native_fixture_index_not_published', 'cases': []}
    index = read_json(index_path)
    if index.get('schema') != 1:
        raise ValueError('Unsupported fixture index')
    session = (index.get('run_id'), str(index.get('module_generation')))
    if expected_session is not None and session != tuple(expected_session):
        raise ValueError('Fixture index session mismatch')
    entries = index.get('entries', [])
    if not isinstance(entries, list) or len(entries) > 8:
        raise ValueError('Fixture entry budget exceeded')
    cases = []
    total = 0
    seen = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get('id'), str) or entry['id'] in seen:
            raise ValueError('Invalid or duplicate fixture ID')
        seen.add(entry['id'])
        meta_path = member(root, entry.get('metadata'), '.json')
        blob_path = member(root, entry.get('blob'), '.bin')
        meta = read_json(meta_path, 65536)
        if meta.get('schema') != 1 or meta.get('status') != 'ready':
            raise ValueError('Fixture metadata not ready')
        if meta.get('id') != entry['id'] or meta.get('blob') != entry['blob']:
            raise ValueError('Fixture metadata identity mismatch')
        if (meta.get('run_id'), str(meta.get('module_generation'))) != session:
            raise ValueError('Fixture metadata session mismatch')
        size = blob_path.stat().st_size
        if not 0 < size <= MAX_BLOB or size % 16 or size != meta.get('stored_bytes') or size != entry.get('stored_bytes'):
            raise ValueError('Fixture size or instruction alignment mismatch')
        total += size
        if total > MAX_TOTAL:
            raise ValueError('Fixture total budget exceeded')
        with blob_path.open('rb') as stream:
            raw = stream.read(MAX_BLOB + 1)
        if len(raw) != size:
            raise ValueError('Fixture changed during read')
        if meta.get('hash_algorithm') != 'fnv1a64-nonauthenticating' or meta.get('content_id') != fnv1a64(raw):
            raise ValueError('Fixture content ID mismatch')
        offset = meta.get('error_offset')
        if type(offset) is not int or offset < 0 or offset % 16:
            raise ValueError('Invalid native error offset')
        source_size = meta.get('source_bytes')
        if type(source_size) is not int or source_size < size or source_size % 16:
            raise ValueError('Invalid native source size')
        if bool(meta.get('truncated')) != (size != source_size):
            raise ValueError('Inconsistent truncation metadata')
        if offset + 16 > source_size:
            raise ValueError('Failure outside declared source')
        words = None
        if offset + 16 <= size:
            words = list(struct.unpack_from('<4I', raw, offset))
            if words != meta.get('words') or (words[0] & 0xfff) != meta.get('opcode'):
                raise ValueError('Failure instruction or opcode mismatch')
        elif not meta.get('truncated'):
            raise ValueError('Failure outside complete stored source')
        cases.append({'id': entry['id'], 'blob': str(blob_path.resolve()),
                      'sha256': hashlib.sha256(raw).hexdigest(), 'stored_bytes': size,
                      'source_bytes': source_size, 'error_offset': offset,
                      'opcode': meta.get('opcode'), 'words': words,
                      'stage': meta.get('stage'), 'reason': meta.get('reason'),
                      'program': meta.get('program'),
                      'compute_environment': compute_context(meta),
                      'capture_termination': meta.get('capture_termination'),
                      'truncated': meta.get('truncated'),
                      'failure_instruction_verified': words is not None,
                      'replay_ready': False,
                      'missing': ['resource_inputs', 'bindings', 'synchronization', 'numerical_output_oracle']})
    return {'available': True, 'run_id': session[0], 'module_generation': session[1],
            'history_not_live': True, 'stored_bytes_verified': total, 'cases': cases,
            'collector': {k: v for k, v in index.items() if k != 'entries'},
            'scope': 'native program integrity only, not full GPU replay or numerical correctness'}


def collect(bridge, out, capture=False):
    from debug_sdk import NxEmuSDK
    sdk = NxEmuSDK(bridge)
    status = sdk.status()
    expected = (status['run_id'], str(status['module_generation'])) if status['fresh'] else None
    root = Path(out)
    root.mkdir(parents=True, exist_ok=True)
    report = {'schema': 1, 'collected_epoch_ms': round(time.time() * 1000),
              'status': status, 'cases': inspect_cases(bridge, expected), 'capture_requested': capture,
              'scope': 'observation, not gameplay acceptance'}
    if capture:
        report['capture'] = sdk.capture_frame(15)
    report['reader_wall_seconds'] = max(0, time.time() - report['collected_epoch_ms'] / 1000)
    target = root / 'lab-observation.json'
    temporary = root / 'lab-observation.tmp'
    temporary.write_text(json.dumps(report, indent=2), encoding='utf-8')
    temporary.replace(target)
    return report


def build_probe(out):
    repo = Path(__file__).resolve().parents[1]
    root = Path(out).resolve()
    root.mkdir(parents=True, exist_ok=True)
    lower = repo / 'src/yuzu_shader_recompiler/frontend/ampere/lower_to_maxwell.h'
    inputs = [lower, repo / 'tools/nx2_native_probe.cpp', repo / 'tools/build_native_probe.cmd',
              repo / 'src/yuzu_common/common_types.h']
    input_hashes = {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    before = hashlib.sha256(lower.read_bytes()).hexdigest()
    command = ['cmd.exe', '/d', '/c', str(repo / 'tools/build_native_probe.cmd'), str(root)]
    process = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (root / 'build.log').write_text(process.stdout + process.stderr, encoding='utf-8')
    exe = root / 'nx2-native-probe.exe'
    after = hashlib.sha256(lower.read_bytes()).hexdigest()
    inputs_after = {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    if process.returncode or before != after or input_hashes != inputs_after or not exe.is_file():
        raise ValueError('Probe build failed or production lowerer changed; see build.log')
    manifest = {'schema': 1, 'lower_sha256': after, 'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                'input_sha256': input_hashes, 'command': command,
                'exit_code': process.returncode, 'built_epoch_ms': round(time.time()*1000)}
    (root / 'build-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    return manifest


def probe_cases(bridge, probe_directory):
    repo = Path(__file__).resolve().parents[1]
    root = Path(probe_directory).resolve()
    manifest = read_json(root / 'build-manifest.json', 65536)
    exe = root / 'nx2-native-probe.exe'
    lower = repo / 'src/yuzu_shader_recompiler/frontend/ampere/lower_to_maxwell.h'
    if manifest.get('schema') != 1 or hashlib.sha256(exe.read_bytes()).hexdigest() != manifest.get('exe_sha256'):
        raise ValueError('Native probe executable identity mismatch')
    if hashlib.sha256(lower.read_bytes()).hexdigest() != manifest.get('lower_sha256'):
        raise ValueError('Production lowerer changed: rebuild native probe')
    for name, expected in manifest.get('input_sha256', {}).items():
        path = repo / name
        if path.resolve().is_relative_to(repo.resolve()) is False or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('Native probe build inputs changed: rebuild native probe')
    report = inspect_cases(bridge)
    results = []
    for case in report['cases']:
        # The verified SHA256 and exact bytes must still match immediately before invocation.
        if hashlib.sha256(Path(case['blob']).read_bytes()).hexdigest() != case['sha256']:
            raise ValueError('Fixture changed after integrity check')
        process = subprocess.run([str(exe), case['blob']], capture_output=True, text=True, timeout=30)
        if process.returncode or len(process.stdout) > 65536:
            raise ValueError('Native probe failed or exceeded output budget')
        result = json.loads(process.stdout)
        if result.get('schema') != 1 or result.get('source_bytes') != case['stored_bytes']:
            raise ValueError('Native probe source mismatch')
        result['case_id'] = case['id']
        result['rejection_reproduced'] = bool(not result.get('lowered') and
                                              result.get('error_offset') == case['error_offset'] and
                                              result.get('reason') == case['reason'])
        results.append(result)
    return {'schema': 1, 'probe_manifest': manifest, 'results': results,
            'scope': 'actual production lowerer only; not IR/SPIRV/GPU numerical replay'}


def stack_summary(path):
    path = Path(path)
    with path.open('rb') as stream:
        raw = stream.read(8 * MAX_BLOB + 1)
    if len(raw) > 8 * MAX_BLOB:
        raise ValueError('Host stack text budget exceeded')
    text = raw.decode('utf-8', errors='replace')
    headers = list(re.finditer(r'(?m)^\s*[.*]?\s*(\d+)\s+Id:\s+([0-9a-f]+)\.([0-9a-f]+).*$', text))
    if len(headers) > 256:
        raise ValueError('Host thread budget exceeded')
    patterns = {'driver_pipeline_creation': ('Device::CreateComputePipeline', 'Device::CreateGraphicsPipeline'),
                'pipeline_wait': ('WaitForNativeBuild', 'WaitForShaderBuilds'),
                'guest_shader_translation': ('TranslateProgram', 'CollectStorageBuffers', 'GlobalMemoryToStorageBufferPass'),
                'spirv_emission': ('EmitSPIRV',), 'gpu_finish_wait': ('Scheduler::Finish',)}
    observed = []
    for number, header in enumerate(headers):
        block = text[header.end():headers[number+1].start() if number+1 < len(headers) else len(text)]
        roles = [name for name, symbols in patterns.items() if any(symbol in block for symbol in symbols)]
        if roles:
            lines = [line[:512] for line in block.splitlines() if
                     any(symbol in line for symbols in patterns.values() for symbol in symbols)]
            driver_modules = sorted(set(re.findall(r'\b(amdvlk64|nvoglv64|igvk64|vulkan-1)!?', block)))
            observed.append({'host_thread_index': int(header[1]), 'pid_hex': header[2], 'tid_hex': header[3],
                             'observed_stack_roles': roles, 'driver_modules': driver_modules,
                             'matching_frames': lines[:8]})
    return {'schema': 1, 'source': str(path.resolve()), 'source_sha256': hashlib.sha256(raw).hexdigest(),
            'available': bool(headers), 'parsed_threads': len(headers), 'observed': observed,
            'unclassified_or_unrecognized_is_not_idle': True,
            'scope': 'offline stack snapshot; not continuous sampling, exact driver internals, or proof of root cause'}


def runtime_device(path):
    path = Path(path)
    if not path.is_file():
        return {'available': False, 'reason': 'runtime_log_not_saved'}
    with path.open('rb') as stream:
        text = stream.read(512 * 1024).decode('utf-8', errors='replace')
    result = {'available': False, 'source': 'runtime Vulkan Report log, not adapter enumeration',
              'header_read_budget': 512 * 1024}
    for field in ('Driver', 'Device'):
        matches = re.findall(r'Report:\d+: ' + field + r': ([^\r\n]+)', text)
        if matches:
            result[field.lower()] = matches[-1]
    result['available'] = 'device' in result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    pipeline_probe = commands.add_parser('pipeline-probe')
    pipeline_probe.add_argument('--metadata', required=True)
    pipeline_probe.add_argument('--out', required=True)
    pipeline_probe.add_argument('--adapter-match-index', type=int, choices=range(256))
    plan = commands.add_parser('validate-plan')
    plan.add_argument('--path', required=True)
    plan.add_argument('--out')
    cases = commands.add_parser('cases')
    cases.add_argument('--bridge', required=True)
    cases.add_argument('--out')
    pipeline_cases = commands.add_parser('pipeline-cases')
    pipeline_cases.add_argument('--bridge', required=True)
    pipeline_cases.add_argument('--out')
    build = commands.add_parser('build-probe')
    build.add_argument('--out', default=str(Path(__file__).resolve().parents[1] / 'build/native-probe'))
    probe = commands.add_parser('probe')
    probe.add_argument('--bridge', required=True)
    probe.add_argument('--probe-directory', default=str(Path(__file__).resolve().parents[1] / 'build/native-probe'))
    probe.add_argument('--out')
    stacks = commands.add_parser('stacks')
    stacks.add_argument('--path', required=True)
    stacks.add_argument('--out')
    observe = commands.add_parser('collect')
    observe.add_argument('--bridge', required=True)
    observe.add_argument('--out', required=True)
    observe.add_argument('--capture-frame', action='store_true')
    run = commands.add_parser('run')
    run.add_argument('--game', required=True)
    run.add_argument('--out', required=True)
    run.add_argument('--exe', default=str(Path(__file__).resolve().parents[1] / 'bin/x64/Release/nxemu.exe'))
    run.add_argument('--seconds', type=int, choices=[30, 60], default=30)
    run.add_argument('--detail', type=int, choices=[0, 1], default=0)
    run.add_argument('--shader-fixtures', type=int, choices=[0, 1], default=0)
    run.add_argument('--shader-progress', type=int, choices=[0, 1], default=0)
    run.add_argument('--ssa-fixtures', type=int, choices=[0, 1], default=0)
    run.add_argument('--pipeline-fixtures', type=int, choices=[0, 1], default=0)
    run.add_argument('--pipeline-fixture-key')
    run.add_argument('--capture-frame', action='store_true')
    run.add_argument('--paired-pixels', action='store_true', help='explicit source/renderer-target readback; requires capture-frame; default off')
    run.add_argument('--image-writers',action='store_true',help='bounded birth/command record history; requires paired-pixels')
    run.add_argument('--native-draw-inputs',action='store_true',help='explicit bounded header/regs/cbuf-prefix capture on graphics rejection; requires shader-fixtures/image-writers')
    run.add_argument('--capture-at',type=float,action='append',default=[],help='1..3 increasing seconds; requires capture-frame, leaves GPU timeout/shutdown margin')
    args = parser.parse_args()
    try:
        if args.action == 'pipeline-probe':
            command=[__import__('sys').executable,'-B',str(Path(__file__).with_name('pipeline_probe.py')),
                     '--metadata',args.metadata,'--out',args.out]
            if args.adapter_match_index is not None:command+=['--adapter-match-index',str(args.adapter_match_index)]
            started=time.monotonic()
            try:
                process=subprocess.run(command,capture_output=True,text=True,timeout=30)
                if len(process.stdout)>262144:raise ValueError('Pipeline helper output budget exceeded')
                result=json.loads(process.stdout);code=process.returncode
                result['external_watchdog']={'seconds':30,'command':subprocess.list2cmdline(command),'exit_status':code,'wall_seconds':time.monotonic()-started}
            except subprocess.TimeoutExpired:
                result={'result':'isolated_pipeline_helper_timeout','watchdog_seconds':30,'helper_process_terminated':True,
                        'not_in_call_cancellation':True,'creation_succeeded':False,'command':subprocess.list2cmdline(command)}
                code=4
            Path(args.out).write_text(json.dumps(result,indent=2),encoding='utf-8')
        elif args.action == 'validate-plan':
            from experiment_plan import load_plan
            result = load_plan(args.path)
            if args.out:
                Path(args.out).write_text(json.dumps(result, indent=2), encoding='utf-8')
            code = 0
        elif args.action == 'stacks':
            result = stack_summary(args.path)
            if args.out:
                Path(args.out).write_text(json.dumps(result, indent=2), encoding='utf-8')
            code = 0
        elif args.action == 'build-probe':
            result = build_probe(args.out)
            code = 0
        elif args.action == 'probe':
            result = probe_cases(args.bridge, args.probe_directory)
            if args.out:
                Path(args.out).write_text(json.dumps(result, indent=2), encoding='utf-8')
            code = 0
        elif args.action == 'pipeline-cases':
            from pipeline_cases import inspect_cases as inspect_pipeline_cases
            result = inspect_pipeline_cases(args.bridge)
            if args.out:Path(args.out).write_text(json.dumps(result,indent=2),encoding='utf-8')
            code=0 if result['available'] else 3
        elif args.action == 'cases':
            result = inspect_cases(args.bridge)
            if args.out:
                Path(args.out).write_text(json.dumps(result, indent=2), encoding='utf-8')
            code = 0 if result['available'] else 3
        elif args.action == 'collect':
            result = collect(args.bridge, args.out, args.capture_frame)
            code = 0
        else:
            from debug_launch import supervise
            from debug_sdk import NxEmuSDK
            from concurrent.futures import ThreadPoolExecutor
            root = Path(args.out)
            capture_result = None
            series = None
            if args.capture_at:
                from frame_series import FrameSeries
                series=FrameSeries(args.capture_at,args.seconds,args.capture_frame)
            observation_start=time.monotonic()
            options={'debug-detail':args.detail,'shader-rejection-trace':1,'shader-fixtures':args.shader_fixtures,
                      'shader-progress':args.shader_progress,'pipeline-fixtures':args.pipeline_fixtures,'ssa-fixtures':args.ssa_fixtures,'frame-capture':int(args.capture_frame)}
            if args.paired_pixels:
                if not args.capture_frame: raise ValueError('paired-pixels requires capture-frame')
                options['paired-pixels']=1
            if args.image_writers:
                if not args.paired_pixels:raise ValueError('image-writers requires paired-pixels')
                options['image-writers']=1
            if args.native_draw_inputs:
                if not args.image_writers or args.shader_fixtures!=1:raise ValueError('native-draw-inputs requires image-writers and shader-fixtures=1')
                options['native-draw-inputs']=1
            if args.pipeline_fixture_key is not None:options['pipeline-fixture-key']=args.pipeline_fixture_key
            with ThreadPoolExecutor(max_workers=1) as pool:
                future = pool.submit(supervise, root / 'bridge', args.exe, args.game, args.seconds, root,
                                     diagnostic_options=options)
                while not future.done():
                    if series is not None:
                        series.poll(NxEmuSDK(root/'bridge'),time.monotonic()-observation_start)
                        root.mkdir(parents=True,exist_ok=True)
                        tmp=root/'frame-series.json.tmp'
                        tmp.write_text(json.dumps(series.result(),indent=2),encoding='utf-8')
                        tmp.replace(root/'frame-series.json')
                    if series is None and args.capture_frame and capture_result is None:
                        try:
                            sdk = NxEmuSDK(root / 'bridge')
                            status = sdk.status()
                            if status['fresh'] and status.get('composites_completed', 0) >= 50:
                                capture_result = sdk.capture_frame(15)
                        except (OSError, ValueError, KeyError, RuntimeError) as error:
                            if (root / 'bridge/status.json').exists():
                                capture_result = {'result': 'observer_error', 'reason': str(error)}
                    time.sleep(.5)
                result, code = future.result()
            if series is not None:result['frame_series']=series.result()
            result['capture'] = ({'result':'see_frame_series','observed':len(series.rows)} if series is not None else capture_result or {'result': 'not_requested_or_milestone_not_observed'})
            result['runtime_vulkan_device'] = runtime_device(root / 'runtime-game.log')
            result['native_cases'] = inspect_cases(root / 'bridge')
            from pipeline_cases import inspect_cases as inspect_pipeline_cases
            result['pipeline_cases'] = inspect_pipeline_cases(root/'bridge')
            stack_path = root / 'shutdown-stall/host-stacks.txt'
            if stack_path.is_file():
                result['host_stack_summary'] = stack_summary(stack_path)
            (root / 'lab-result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False))
        return code
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(json.dumps({'result': 'lab_error', 'reason': str(error)}, ensure_ascii=False))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
