"""Opt-in NXEmu debug bridge client. No OS input or process termination."""
from pathlib import Path
import argparse, ctypes, json, os, sys, time, struct

def process_alive(pid):
    if os.name == 'nt':
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        k.OpenProcess.restype = ctypes.c_void_p
        k.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        k.CloseHandle.argtypes = [ctypes.c_void_p]
        h = k.OpenProcess(0x1000, False, pid)
        if not h: return False
        try:
            code = ctypes.c_ulong()
            return bool(k.GetExitCodeProcess(h, ctypes.byref(code)) and code.value == 259)
        finally: k.CloseHandle(h)
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False
    except PermissionError: return True

class NxEmuSDK:
    def __init__(self, directory):
        self.root = Path(directory)
    def status(self):
        # MoveFileEx can briefly contend with a reader on Windows. Retry only
        # transient sharing/partial-publication failures; preserve stale checks.
        for attempt in range(8):
            try:
                data = json.loads((self.root / 'status.json').read_text(encoding='utf-8-sig'))
                break
            except (PermissionError, json.JSONDecodeError):
                if attempt == 7: raise
                time.sleep(.02)

        if data.get('schema') != 1:
            raise ValueError('Unsupported status schema')
        data['age_seconds'] = max(0, time.time() - data['epoch_ms'] / 1000)
        data['process_alive'] = process_alive(data['pid'])
        data['fresh'] = data['age_seconds'] < 3 and not data['exited'] and data['process_alive']
        return data
    def diagnose(self, log):
        s = self.status(); p = Path(log)
        with p.open('rb') as f:
            data = f.read(524288)
            if p.stat().st_size > len(data):
                f.seek(max(len(data), p.stat().st_size - 65536)); data += f.read(65536)
        lines = data.decode('utf-8', errors='replace').splitlines()
        markers = [l for l in lines if any(k in l for k in
                   ['Startup:', 'Game launch:', 'NX2_PRESENT', 'NX2_KIND', 'DEBUG_MILESTONE', 'NX2_SHADER', 'NX2_NATIVE_CAPTURE', 'NX2_COMPUTE', 'NX2_QMD', 'NX2_VERTEX', 'NX2_VK_BUFFER_FAILURE', 'NX2_DRAW_CAPTURE', 'NX2_DISPLAY', 'NX2_COMPUTE_DISPATCH', 'NX2_COMPUTE_REJECT', 'NX2_READBACK_BATCH'])]
        unknown = [l for l in lines if 'Unknown / unimplemented function' in l]
        faults = [l for l in lines if any(k in l for k in
                  ['NX2_QMD native dispatch pending', 'native shader dispatch required', 'NX2_SHADER_REJECT', 'NX2_VK_BUFFER_FAILURE',
                   'NX2_VERTEX unsupported', 'NX2_COMPUTE register out of range', 'NX2_COMPUTE_REJECT'])]
        diagnostics = self.root / 'diagnostics'
        artifacts = [{'path':str(f.resolve()), 'bytes':f.stat().st_size}
                     for f in sorted(diagnostics.iterdir())[:64] if f.is_file()] if diagnostics.is_dir() else []
        descriptors = []
        if diagnostics.is_dir():
            for f in sorted(diagnostics.glob('compute-*-qmd.bin'))[:5]:
                data = f.read_bytes()
                if len(data) != 256: continue
                q = struct.unpack('<64I', data)
                descriptors.append({'path':str(f.resolve()), 'version':hex(q[18] & 0xff),
                    'program':hex(q[48] | ((q[49] & 0x1ffff) << 32)),
                    'grid':[q[12],q[13]&0xffff,q[14]&0xffff],
                    'block':[q[18]>>16,q[19]&0xffff,q[19]>>16],
                    'shared_bytes':q[17]&0x3ffff, 'cbuf_mask':hex(q[20]&0xff)})
        draws = []
        if diagnostics.is_dir():
            for f in diagnostics.glob('draw-*.json'):
                try: draws.append(json.loads(f.read_text()))
                except (OSError, json.JSONDecodeError): continue
            draws = sorted(draws, key=lambda d:d.get('sequence',0))[-8:]
        fault_probe = ('vertex_stream_size_or_vulkan_memory' if any('NX2_VK_BUFFER_FAILURE' in l or 'NX2_VERTEX' in l for l in faults)
                       else 'native_compute_qmd_and_shader' if any('NX2_QMD' in l or 'native shader dispatch required' in l for l in faults)
                       else 'native_shader_lowering' if any('NX2_SHADER_REJECT' in l for l in faults) else None)
        return {'status': s, 'log_bytes_sampled':len(data), 'milestones': markers[-25:],
                'unimplemented_tail':unknown[-8:], 'runtime_faults':faults[-16:], 'diagnostic_artifacts':artifacts, 'native_compute_descriptors':descriptors, 'native_draw_states':draws,
                'next_probe': fault_probe if fault_probe else 'process_exit' if not s['process_alive'] else
                    'present_fence_completion' if s['present_requests'] > s['composites_completed'] + 8 or
                                                   (s['present_requests'] and not s['composites_completed']) else
                    'memory_mapping' if s['mapping_bytes'] else
                    'guest_gpu_submission' if s.get('render_debug_version', 1) < 2 or not s.get('dma_batches', 0) else
                    'command_decode_or_engine_binding' if not s.get('draw_requests', 0) and not s.get('compute_requests', 0) else
                    'shader_pipeline' if (s.get('draw_requests', 0) > s.get('draw_pipeline_ready', 0) or
                                          s.get('compute_requests', 0) > s.get('compute_pipeline_ready', 0)) else
                    'render_target_or_shader_output',
                'counter_note': 'pipeline_ready means configured by the backend, not host execution or visible gameplay'}
    def telemetry(self):
        from debug_telemetry import TelemetrySampler
        if not hasattr(self, "_telemetry"): self._telemetry = TelemetrySampler(self)
        return self._telemetry.sample()
    def inspect(self, with_host=True):
        from debug_inspect import inspect
        return inspect(self, with_host)
    def events(self, limit=128, include_lifecycle=False):
        from debug_inspect import events
        return events(self, limit, include_lifecycle)
    def bundle(self, out, log=None):
        from debug_inspect import bundle
        return bundle(self, out, log)
    def stack(self, out, timeout=30):
        from debug_inspect import stack
        return stack(self, out, timeout)
    def keyboard(self, milestone=None, timeout=60, after_sequence=0):
        from debug_inspect import document
        start=self.status();deadline=time.monotonic()+timeout
        while True:
            status=self.status()
            if not status['fresh'] or status['run_id']!=start['run_id']:
                raise RuntimeError('Keyboard observation stopped or backend restarted')
            d=document(self,'software-keyboard.json',status)
            # A lifecycle event does not age into a heartbeat. Keep its timestamp
            # and report same-session validity separately from event freshness.
            valid=d.get('available',False) and d.get('session_matches',False)
            result={'observation':d,'same_session_event':valid,'live_ui_verified':None}
            if milestone is None:return result,0
            data=d.get('data',{})
            reached={'initialized':bool(data.get('instance_id')),'shown':data.get('shown_count',0)>0,
                     'submitted':data.get('submit_count',0)>0,
                     'closed':valid and data.get('active_at_event') is False}.get(milestone,False)
            if valid and d.get('sequence',0)>after_sequence and reached:
                result['result']='milestone_observed';return result,0
            if time.monotonic()>=deadline:
                result['result']='not_observed_before_timeout';return result,2
            time.sleep(.1)
    def command(self, verb, path='', timeout=10):
        if verb not in {'load', 'stop', 'pause', 'resume', 'quit', 'input'}:
            raise ValueError('Unknown command')
        s = self.status()
        if not s['fresh']:
            raise RuntimeError('Backend heartbeat is stale')
        request_id = max(time.time_ns(), s['request_id'] + 1)
        if '\n' in path or '\r' in path:
            raise ValueError('Newlines in paths are not supported')
        if verb == 'load':
            path = str(Path(path).resolve(strict=True))
        tmp = self.root / f'command.{request_id}.tmp'
        tmp.write_text(f"{s['run_id']} {request_id} {verb}\n{path}\n", encoding='utf-8')
        # Windows reader sharing contention: retry the SAME request, never replay
        # an acknowledged action or cross a run boundary. Bound this to 400ms.
        for attempt in range(20):
            try:
                os.replace(tmp, self.root / 'command.txt')
                break
            except PermissionError:
                current = self.status()
                if not current['fresh'] or current['run_id'] != s['run_id'] or attempt == 19:
                    tmp.unlink(missing_ok=True)
                    raise
                time.sleep(.02)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            current = self.status()
            if current['run_id'] != s['run_id']:
                raise RuntimeError('Backend restarted before acknowledgement')
            if current['request_id'] == request_id:
                return current
            time.sleep(.1)
        raise TimeoutError('Command acknowledgement timed out; outcome unknown')
    def input(self, button='A', player=0, hold_ms=100, gap_ms=100, count=1, timeout=60):
        names=['A','B','X','Y','LStick','RStick','L','R','ZL','ZR','Plus','Minus','DLeft','DUp','DRight','DDown','SL','SR','Home','Capture']
        if button not in names or not 0 <= player <= 9 or not 20 <= hold_ms <= 2000 or not 20 <= gap_ms <= 2000 or not 1 <= count <= 20:
            raise ValueError('Invalid bounded input sequence')
        start=self.status()
        if not start.get('input',{}).get('available'):
            raise RuntimeError('Input bridge is not available')
        acknowledged=self.command('input',f'{player} {names.index(button)} {hold_ms} {gap_ms} {count}')
        if acknowledged['response']!='accepted':
            return {'result':'rejected','status':acknowledged}
        request=acknowledged['request_id'];deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            state=self.status()
            if not state['fresh'] or state['run_id']!=start['run_id']:
                raise RuntimeError('Input outcome unknown: backend stopped or restarted')
            data=state.get('input',{})
            if data.get('request_id')==request and data.get('status') in ('completed','cancelled'):
                return {'result':data['status'],'status':state}
            time.sleep(.05)
        raise TimeoutError('Input sequence outcome unknown')
    def wait(self, frames=1, timeout=60, progress=False):
        initial = self.status(); run = initial['run_id']; pid = initial['pid']
        handle = None; kernel = None
        if os.name == 'nt':
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x1000, False, pid)
        deadline = time.monotonic() + timeout; previous = None
        try:
            while time.monotonic() < deadline:
                s = self.status()
                if s['run_id'] != run:
                    return {'result': 'restarted', 'status': s}, 3
                if handle:
                    code = ctypes.c_ulong()
                    if kernel.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value != 259:
                        return {'result': 'process_exited', 'exit_code': code.value, 'status': s}, 3
                if not s['process_alive']:
                    return {'result': 'process_missing', 'status': s}, 3
                key = (s['milestone'], s['composites_completed'], s['mapping_bytes'], s['fresh'])
                if progress and key != previous:
                    completed = min(frames, s['composites_completed'])
                    bar = '#' * (20 * completed // max(frames, 1))
                    print(f"[{bar:<20}] frames={s['composites_completed']}/{frames} "
                          f"{s['milestone']} maps_pending={s['mapping_bytes']} "
                          f"heartbeat_age={s['age_seconds']:.1f}s", file=sys.stderr, flush=True)
                    previous = key
                if s['fresh'] and s['composites_completed'] >= frames:
                    return {'result': 'frame_threshold_reached', 'status': s}, 0
                if s['exited']:
                    return {'result': 'backend_exited', 'status': s}, 3
                time.sleep(.25)
            return {'result': 'timeout', 'status': self.status()}, 2
        finally:
            if handle: kernel.CloseHandle(handle)

def main():
    p = argparse.ArgumentParser(); p.add_argument('--bridge', required=True)
    sub = p.add_subparsers(dest='action', required=True)
    sub.add_parser('status')
    sub.add_parser('inspect')
    e = sub.add_parser('events'); e.add_argument('--limit',type=int,default=128); e.add_argument('--include-lifecycle',action='store_true')
    b = sub.add_parser('bundle'); b.add_argument('--out',required=True); b.add_argument('--log')
    st = sub.add_parser('stack'); st.add_argument('--out',required=True); st.add_argument('--timeout',type=float,default=30)
    t = sub.add_parser('telemetry'); t.add_argument('--out'); t.add_argument('--seconds', type=float, default=60); t.add_argument('--interval', type=float, default=1)
    d = sub.add_parser('diagnose'); d.add_argument('--log', required=True)
    w = sub.add_parser('wait'); w.add_argument('--frames', type=int, default=1)
    w.add_argument('--timeout', type=float, default=60); w.add_argument('--progress', action='store_true')
    c = sub.add_parser('command'); c.add_argument('verb', choices=['load','stop','pause','resume','quit'])
    c.add_argument('path', nargs='?', default=''); c.add_argument('--timeout', type=float, default=10)
    i = sub.add_parser('input'); i.add_argument('button'); i.add_argument('--player',type=int,default=0)
    i.add_argument('--hold-ms',type=int,default=100); i.add_argument('--gap-ms',type=int,default=100)
    i.add_argument('--count',type=int,default=1); i.add_argument('--timeout',type=float,default=60)
    kb = sub.add_parser('keyboard');kb.add_argument('--wait',choices=['initialized','shown','submitted','closed'])
    kb.add_argument('--timeout',type=float,default=60);kb.add_argument('--after-sequence',type=int,default=0)
    launch = sub.add_parser('launch')
    launch.add_argument('--exe',default=str(Path(__file__).resolve().parents[1]/'bin/x64/Release/nxemu.exe'))
    launch.add_argument('--game'); launch.add_argument('--seconds',type=float,default=30)
    launch.add_argument('--out',required=True); launch.add_argument('--no-dump',action='store_true')
    launch.add_argument('--readback-segments',type=int,choices=[0,1])
    launch.add_argument('--keyboard-name')
    a = p.parse_args(); sdk = NxEmuSDK(a.bridge); code = 0
    try:
        if a.action == 'launch':
            from debug_launch import supervise
            result,code=supervise(a.bridge,a.exe,a.game,a.seconds,a.out,not a.no_dump,a.readback_segments,a.keyboard_name)
        elif a.action == 'input':
            result=sdk.input(a.button,a.player,a.hold_ms,a.gap_ms,a.count,a.timeout)
            if result['result']!='completed':code=4
        elif a.action == 'keyboard': result,code=sdk.keyboard(a.wait,a.timeout,a.after_sequence)
        elif a.action == 'status': result = sdk.status()
        elif a.action == 'inspect': result = sdk.inspect()
        elif a.action == 'events': result = sdk.events(a.limit,a.include_lifecycle)
        elif a.action == 'bundle': result = sdk.bundle(a.out,a.log)
        elif a.action == 'stack': result = sdk.stack(a.out,a.timeout)
        elif a.action == 'telemetry':
            from debug_telemetry import TelemetrySampler
            sampler = TelemetrySampler(sdk)
            try: result = sampler.watch(a.out, a.seconds, a.interval) if a.out else sampler.sample()
            finally: sampler.gpu.close()
        elif a.action == 'diagnose': result = sdk.diagnose(a.log)
        elif a.action == 'wait': result, code = sdk.wait(a.frames, a.timeout, a.progress)
        else:
            result = sdk.command(a.verb, a.path, a.timeout)
            if result['response'] != 'accepted': code = 4
        print(json.dumps(result, ensure_ascii=False))
    except (OSError, ValueError, RuntimeError, TimeoutError) as e:
        print(json.dumps({'result':'error','message':str(e)}, ensure_ascii=False)); code = 1
    return code
if __name__ == '__main__': sys.exit(main())
