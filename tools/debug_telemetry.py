"""Read-only, bounded-cost host + emulator telemetry. No screenshots or debugger attachment."""
from pathlib import Path
import ctypes, json, os, time, statistics
try:
    import psutil
except ImportError:
    psutil = None

class NvidiaMetrics:
    class Util(ctypes.Structure):
        _fields_ = [('gpu', ctypes.c_uint), ('memory', ctypes.c_uint)]
    class Memory(ctypes.Structure):
        _fields_ = [('total', ctypes.c_ulonglong), ('free', ctypes.c_ulonglong), ('used', ctypes.c_ulonglong)]
    def __init__(self):
        self.lib = None; self.error = None
        try:
            # Absolute trusted driver location; never load a DLL from the game directory.
            p = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/nvml.dll'
            self.lib = ctypes.CDLL(str(p))
            self.lib.nvmlDeviceGetHandleByIndex_v2.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)]
            self.check(self.lib.nvmlInit_v2())
            n = ctypes.c_uint(); self.check(self.lib.nvmlDeviceGetCount_v2(ctypes.byref(n)))
            self.devices = []
            for i in range(n.value):
                h = ctypes.c_void_p(); self.check(self.lib.nvmlDeviceGetHandleByIndex_v2(i, ctypes.byref(h)))
                self.devices.append(h)
        except (OSError, RuntimeError, AttributeError) as e:
            self.error = str(e)
    @staticmethod
    def check(code):
        if code: raise RuntimeError('NVML status=' + str(code))
    def sample(self):
        if self.error: return {'available': False, 'reason': self.error}
        result = []
        for i, h in enumerate(self.devices):
            d = {'index': i, 'scope': 'whole_device_not_process', 'unsupported': []}
            name = ctypes.create_string_buffer(96)
            if not self.lib.nvmlDeviceGetName(h, name, 96): d['name'] = name.value.decode()
            driver = ctypes.create_string_buffer(96)
            if not self.lib.nvmlSystemGetDriverVersion(driver, 96): d['driver_version'] = driver.value.decode()
            u = self.Util(); m = self.Memory()
            if not self.lib.nvmlDeviceGetUtilizationRates(h, ctypes.byref(u)):
                d.update(gpu_util_percent=u.gpu, memory_controller_util_percent=u.memory)
            else: d['unsupported'].append('utilization')
            if not self.lib.nvmlDeviceGetMemoryInfo(h, ctypes.byref(m)):
                d.update(vram_used_bytes=m.used, vram_free_bytes=m.free, vram_total_bytes=m.total)
            for field, fn, args, scale in [
                ('graphics_clock_mhz','nvmlDeviceGetClockInfo',[0],1),
                ('memory_clock_mhz','nvmlDeviceGetClockInfo',[2],1),
                ('temperature_c','nvmlDeviceGetTemperature',[0],1),
                ('power_w','nvmlDeviceGetPowerUsage',[],.001),
                ('pstate','nvmlDeviceGetPerformanceState',[],1),
                ('pcie_rx_kib_s','nvmlDeviceGetPcieThroughput',[1],1),
                ('pcie_tx_kib_s','nvmlDeviceGetPcieThroughput',[0],1),
                ('pcie_link_gen','nvmlDeviceGetCurrPcieLinkGeneration',[],1),
                ('pcie_link_width','nvmlDeviceGetCurrPcieLinkWidth',[],1)]:
                v = ctypes.c_uint()
                try: code = getattr(self.lib, fn)(h, *args, ctypes.byref(v))
                except AttributeError: code = -1
                if code: d['unsupported'].append(field)
                else: d[field] = v.value * scale
            reason = ctypes.c_ulonglong()
            try: code = self.lib.nvmlDeviceGetCurrentClocksThrottleReasons(h, ctypes.byref(reason))
            except AttributeError: code = -1
            if code: d['unsupported'].append('clock_throttle_reasons')
            else:
                d['clock_throttle_mask'] = hex(reason.value)
                d['clock_throttle_reasons'] = [name for bit,name in [(1,'gpu_idle'),(2,'applications_clocks'),(4,'software_power_cap'),(8,'hardware_slowdown'),(16,'sync_boost'),(32,'software_thermal'),(64,'hardware_thermal'),(128,'hardware_power_brake'),(256,'display_clock_setting')] if reason.value & bit]
            result.append(d)
        return {'available': True, 'devices': result}
    def close(self):
        if self.lib and not self.error:
            self.lib.nvmlShutdown(); self.lib = None

class TelemetrySampler:
    def __init__(self, sdk):
        self.sdk = sdk; self.prev = None; self.proc = None; self.proc_identity = None
        self.thread_prev = {}; self.io_prev = None; self.disk_prev = None
        self.gpu = NvidiaMetrics(); self.session_start = time.monotonic()
        self.milestones = {}; self.last_run = None
        if psutil: psutil.cpu_percent(None, percpu=True)
    def sample(self):
        t0 = time.monotonic(); s = self.sdk.status(); identity = (s['pid'], s['run_id'], s.get('module_generation'))
        if identity != self.last_run:
            self.prev = None; self.milestones = {}; self.last_run = identity
            self.session_start = t0
        elapsed = t0-self.session_start
        for name, hit in [('backend_ready',s.get('modules_valid')), ('phase_loading',s.get('phase')=='loading'),
                          ('first_dma',s.get('dma_batches',0)>0), ('first_draw',s.get('draw_requests',0)>0),
                          ('first_compute',s.get('compute_requests',0)>0), ('first_composite',s.get('composites_completed',0)>0)]:
            if hit and name not in self.milestones: self.milestones[name] = round(elapsed,3)
        r = {'epoch_ms':round(time.time()*1000), 'status':s, 'elapsed_seconds':round(elapsed,3),
             'milestones_observed_seconds':dict(self.milestones), 'rates':{}, 'host':{},
             'notes':['frame_time_ms is render wall time, not measured controller input latency',
                      'composites are backend composites, not proof of visible gameplay',
                      'GPU utilization and disk IO are whole-device counters']}
        p = self.prev
        if p and p['run_id']==s['run_id'] and p.get('module_generation')==s.get('module_generation') and s['fresh'] and p['fresh']:
            dt=(s['epoch_ms']-p['epoch_ms'])/1000
            if dt>0 and s['composites_completed']>=p['composites_completed']:
                r['rates']['sample_seconds']=dt
                for k in ['composites_completed','dma_batches','draw_requests','compute_requests',
                          'display_accelerated','display_fallback','display_fallback_bytes']:
                    if k in p and k in s and s[k]>=p[k]: r['rates'][k+'_per_second']=(s[k]-p[k])/dt
                if 'display_fallback' in s:
                    calls=s['display_fallback']-p.get('display_fallback',0)
                    us=s['display_fallback_us']-p.get('display_fallback_us',0)
                    r['rates']['fallback_flush_mean_ms']=us/calls/1000 if calls>0 and us>=0 else None
                    r['rates']['fallback_flush_wall_fraction']=us/(dt*1e6) if us>=0 else None
        self.prev = s.copy()
        if psutil:
            host=r['host'];host['logical_cpus']=psutil.cpu_count();host['cpu_per_core_percent']=psutil.cpu_percent(None,percpu=True)
            host['memory']=psutil.virtual_memory()._asdict();host['swap']=psutil.swap_memory()._asdict()
            disks=psutil.disk_io_counters(perdisk=True)
            if self.disk_prev:
                dt=t0-self.disk_prev[0]
                host['disk_rates']={k:{f+'_per_second':max(0,getattr(v,f)-getattr(self.disk_prev[1].get(k,v),f))/dt
                    for f in ['read_bytes','write_bytes','read_count','write_count']} for k,v in disks.items()}
            self.disk_prev=(t0,disks)
            if s['process_alive']:
                try:
                    if identity!=self.proc_identity:
                        self.proc=psutil.Process(s['pid']);self.proc_identity=identity;self.proc.cpu_percent(None)
                        self.thread_prev={};self.io_prev=None
                    with self.proc.oneshot():
                        io=self.proc.io_counters(); mem=self.proc.memory_info()
                        process={'cpu_percent_one_core_100':self.proc.cpu_percent(None),'rss_bytes':mem.rss,
                                 'memory':mem._asdict(),'threads':self.proc.num_threads(),'handles':self.proc.num_handles(),
                                 'io_totals':io._asdict()}
                        ts=self.proc.threads()
                    tops=[]
                    for th in ts:
                        value=th.user_time+th.system_time;old=self.thread_prev.get(th.id)
                        if old: tops.append({'tid':th.id,'cpu_percent_one_core_100':max(0,value-old[1])/(t0-old[0])*100})
                        self.thread_prev[th.id]=(t0,value)
                    active={th.id for th in ts};self.thread_prev={k:v for k,v in self.thread_prev.items() if k in active}
                    process['top_threads']=sorted(tops,key=lambda x:x['cpu_percent_one_core_100'],reverse=True)[:8]
                    if self.io_prev:
                        dt=t0-self.io_prev[0];process['io_rates']={f+'_per_second':max(0,getattr(io,f)-getattr(self.io_prev[1],f))/dt for f in ['read_bytes','write_bytes','read_count','write_count']}
                    self.io_prev=(t0,io);host['process']=process
                except (psutil.Error,OSError) as e: host['process_error']=str(e)
        else: r['host']['error']='psutil not installed: python -m pip install psutil'
        r['host']['gpu']=self.gpu.sample()
        if s.get('bridge_version',0)>=4:
            from debug_inspect import render_documents, diagnosis
            r['render']=render_documents(self.sdk,s);r['diagnosis']=diagnosis(s,r['render'],r['host'])
        r['collector_ms']=(time.monotonic()-t0)*1000
        return r
    def watch(self, directory, seconds=60, interval=1, progress=True):
        if not .25<=interval<=60 or seconds<=0: raise ValueError('interval=.25..60 seconds; duration positive')
        root=Path(directory);root.mkdir(parents=True,exist_ok=True)
        samples=[];start=time.monotonic()
        try:
            with (root/'telemetry.jsonl').open('w',encoding='utf-8') as out:
                while time.monotonic()-start<seconds:
                    t=time.monotonic();r=self.sample();samples.append(r)
                    out.write(json.dumps(r,ensure_ascii=False)+'\n');out.flush()
                    tmp=root/'telemetry-latest.tmp';tmp.write_text(json.dumps(r,ensure_ascii=False,indent=2));os.replace(tmp,root/'telemetry-latest.json')
                    if progress:
                        s=r['status'];bar='#'*min(20,int((t-start)/seconds*20))
                        print(f"[{bar:<20}] {s['phase']} FPS={s.get('game_fps','n/a')} ms={s.get('frame_time_ms','n/a')} composites/s={r['rates'].get('composites_completed_per_second',0):.2f} cpu={r['host'].get('process',{}).get('cpu_percent_one_core_100','n/a')} collector={r['collector_ms']:.1f}ms",flush=True)
                    if not r['status']['fresh']: break
                    time.sleep(max(0,interval-(time.monotonic()-t)))
        finally: self.gpu.close()
        fps=[r['status']['game_fps'] for r in samples if r['status'].get('game_fps',0)>0 and r['status']['video_snapshot_available']]
        summary={'samples':len(samples),'game_fps_median':statistics.median(fps) if fps else None,
                 'collector_ms_median':statistics.median(r['collector_ms'] for r in samples) if samples else None,
                 'milestones':self.milestones,'last_status':samples[-1]['status'] if samples else None}
        (root/'telemetry-summary.json').write_text(json.dumps(summary,indent=2));return summary
