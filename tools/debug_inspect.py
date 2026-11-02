"""Structured NXEmu inspection, bounded event history and opt-in non-invasive stacks."""
from pathlib import Path
import ctypes, hashlib, itertools, json, os, re, subprocess, time, zipfile
try:
    import psutil
except ImportError:
    psutil = None

def bounded_json(path, limit=262144):
    for attempt in range(4):
        try:
            with Path(path).open('rb') as f:
                raw=f.read(limit+1)
            if len(raw)>limit: raise ValueError('Document exceeds bounded limit')
            return json.loads(raw.decode('utf-8-sig'))
        except (PermissionError,json.JSONDecodeError):
            if attempt==3: raise
            time.sleep(.01)

def document(sdk, name, status=None):
    s=status or sdk.status();p=sdk.root/'diagnostics'/name
    if not p.is_file(): return {'available':False,'reason':'not_published_yet','path':str(p)}
    try:
        d=bounded_json(p);d['age_seconds']=max(0,time.time()-d['epoch_ms']/1000)
        d['session_matches']=d.get('run_id')==s['run_id'] and str(d.get('module_generation',''))==str(s.get('module_generation',''))
        d['fresh']=bool(s['fresh'] and s.get('video_snapshot_available') and d['session_matches'] and d['age_seconds']<(12 if name.startswith('stall-') else 3))
        d['available']=True;return d
    except (OSError,ValueError,KeyError) as e:return {'available':False,'reason':str(e),'path':str(p)}

def render_documents(sdk,status=None):
    s=status or sdk.status();r={n:document(sdk,n+'.json',s) for n in ['texture-cache','display','present','native-readback','texture-header','physical-alias','device-backing','readback-transfers','buffer-transfers','software-keyboard','hid-npad-writes']}
    root=sdk.root/'diagnostics'
    r['stalls']=[document(sdk,p.name,s) for p in sorted(itertools.islice(root.glob('stall-*.json'),32))] if root.exists() else []
    return r

def event_matches(data,status,include_lifecycle=False):
    if data.get('run_id')!=status['run_id']:return False
    if str(data.get('module_generation',''))==str(status.get('module_generation','')):return True
    # A prepared launch advances frontend generation after the Host1x events.
    # Opt in to same-run lifecycle HISTORY only; never mark this live/current.
    if include_lifecycle and data.get('category')=='device-backing':
        try:return 0<int(data['module_generation'])<=int(status['module_generation'])
        except (KeyError,TypeError,ValueError):return False
    return False

def select_events(out,limit,include_lifecycle=False):
    ordered=sorted(out,key=lambda d:(d['epoch_ms'],d['category'],d['sequence']))
    if not include_lifecycle:return ordered[-limit:]
    pinned=[d for d in ordered if d['category']=='device-backing'][-min(128,limit):]
    ordinary=[d for d in ordered if d['category']!='device-backing']
    count=limit-len(pinned)
    return sorted(pinned+(ordinary[-count:] if count else []),key=lambda d:(d['epoch_ms'],d['category'],d['sequence']))

def events(sdk,limit=128,include_lifecycle=False):
    if not 1<=limit<=512:raise ValueError('limit=1..512')
    s=sdk.status();root=sdk.root/'diagnostics/events';out=[]
    if root.is_dir():
        for p in itertools.islice(root.iterdir(),1024):
            if p.suffix!='.json':continue
            try:
                d=bounded_json(p)
                if event_matches(d,s,include_lifecycle):
                    d['is_current_generation']=str(d.get('module_generation',''))==str(s.get('module_generation',''))
                    out.append(d)
            except (OSError,ValueError,KeyError):continue
    if include_lifecycle and root.is_dir():
        # Startup events are old by the time shader/present history fills up.
        # Explicitly discover the bounded lifecycle ring and pin it in the result.
        known={(d['category'],d['sequence']) for d in out}
        for p in itertools.islice(root.glob('device-backing-*.json'),128):
            try:
                d=bounded_json(p)
                if event_matches(d,s,True) and (d['category'],d['sequence']) not in known:
                    d['is_current_generation']=str(d.get('module_generation',''))==str(s.get('module_generation',''))
                    out.append(d);known.add((d['category'],d['sequence']))
            except (OSError,ValueError,KeyError):continue
    out=select_events(out,limit,include_lifecycle)
    return {'run_id':s['run_id'],'module_generation':s.get('module_generation'),'events':out,'count':len(out),'include_lifecycle':include_lifecycle,'history_not_live_status':True}

_hash_cache={}
def file_identity(path):
    p=Path(path);st=p.stat();key=(str(p),st.st_size,st.st_mtime_ns)
    if key not in _hash_cache:
        h=hashlib.sha256()
        with p.open('rb') as f:
            for data in iter(lambda:f.read(1024*1024),b''):h.update(data)
        if len(_hash_cache)>=64:_hash_cache.clear()
        _hash_cache[key]=h.hexdigest()
    return {'path':str(p.resolve()),'bytes':st.st_size,'mtime_ns':st.st_mtime_ns,'sha256':_hash_cache[key]}

def thread_names(tids):
    if os.name!='nt':return {}
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    if not hasattr(k,'GetThreadDescription'):return {}
    k.OpenThread.argtypes=[ctypes.c_uint,ctypes.c_int,ctypes.c_uint];k.OpenThread.restype=ctypes.c_void_p
    k.GetThreadDescription.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p)]
    k.CloseHandle.argtypes=[ctypes.c_void_p];k.LocalFree.argtypes=[ctypes.c_void_p]
    result={}
    for tid in list(tids)[:256]:
        h=k.OpenThread(0x0800,False,tid)
        if not h:continue
        try:
            p=ctypes.c_void_p()
            if k.GetThreadDescription(h,ctypes.byref(p))==0 and p.value:
                try:result[str(tid)]=ctypes.wstring_at(p)
                finally:k.LocalFree(p)
        finally:k.CloseHandle(h)
    return result

def runtime_inventory(sdk,status=None):
    s=status or sdk.status();result={'process_alive':s['process_alive'],'loaded_modules':[]}
    if not psutil or not s['process_alive']:return result
    try:
        proc=psutil.Process(s['pid'])
        result.update(executable=proc.exe(),working_directory=proc.cwd(),created_epoch=proc.create_time())
        mapped={m.path for m in proc.memory_maps() if Path(m.path).suffix.lower() in {'.dll','.exe'}}
        for path in sorted(mapped):
            if Path(path).name.lower().startswith('nxemu'):
                try:result['loaded_modules'].append(file_identity(path))
                except OSError:result['loaded_modules'].append({'path':path,'error':'file_identity_unavailable'})
        ts=proc.threads();result['thread_names']=thread_names(t.id for t in ts)
        live_tids={str(t.id) for t in ts};result['thread_name_source']='GetThreadDescription plus emulator registry'
        for p in itertools.islice((sdk.root/'diagnostics').glob('thread-*.json'),512):
            try:
                d=bounded_json(p);tid=str(d.get('thread_id'))
                if d.get('run_id')==s['run_id'] and tid in live_tids:result['thread_names'][tid]=d['data']['name']
            except (OSError,ValueError,KeyError):continue
        # Bounded metadata only: no game file bodies, keys, environment, command line or sockets.
        try:result['open_files']=[{'path':f.path} for f in proc.open_files()[:128] if not any(x in f.path.lower() for x in ['.keys','token','password'])]
        except (psutil.Error,OSError):result['open_files']=[]
        game=s.get('game_path')
        if game and Path(game).is_file():result['game_executable']=file_identity(game)
    except (psutil.Error,OSError) as e:result['error']=str(e)
    return result

def diagnosis(s,render,host=None):
    findings=[]
    if not s['fresh']:findings.append({'kind':'heartbeat_stale','evidence':s.get('age_seconds'),'severity':'high'})
    tex=render.get('texture-cache',{})
    if tex.get('fresh') and not tex['data'].get('selected_image_base'):
        candidates=tex['data'].get('candidates',[])
        findings.append({'kind':'display_cache_miss','evidence':{'address':tex['data'].get('lookup_address'),'candidate_bases':[c['cpu_base'] for c in candidates]},'severity':'high'})
    present=render.get('present',{})
    if present.get('fresh') and present['data'].get('configure_source_us',0)>50000:
        findings.append({'kind':'present_source_stall','evidence_us':present['data']['configure_source_us'],'severity':'high'})
    if s.get('shaders_building',0):findings.append({'kind':'shader_building','count':s['shaders_building'],'severity':'info'})
    if s.get('unbound_methods',0):findings.append({'kind':'unbound_gpu_methods','count':s['unbound_methods'],'severity':'high'})
    if host:
        for gpu in host.get('gpu',{}).get('devices',[]):
            reasons=[x for x in gpu.get('clock_throttle_reasons',[]) if x!='gpu_idle']
            if reasons:findings.append({'kind':'gpu_clock_limit','evidence':reasons,'severity':'info'})
        mem=host.get('memory',{})
        if mem.get('percent',0)>=90:findings.append({'kind':'host_memory_pressure','evidence_percent':mem['percent'],'severity':'high'})
    return {'findings':findings,'note':'Evidence-ranked indicators, not automatic root-cause or playability claims'}

def inspect(sdk,with_host=True):
    s=sdk.status();render=render_documents(sdk,s)
    host=sdk.telemetry()['host'] if with_host else {}
    return {'schema':1,'status':s,'render':render,'inventory':runtime_inventory(sdk,s),'host':host,
            'diagnosis':diagnosis(s,render,host),'coverage':{
                'direct':['frontend_state','ui_perf','title_id','game_path','shader_cache_progress','cache_candidates','present_paths','scheduler_stalls','loaded_module_hashes','host_threads','gpu_metrics'],
                'on_demand':['noninvasive_host_stacks','bounded_frame_event_history','diagnostic_bundle'],
                'not_exposed_yet':['full_guest_thread_registers','per_service_ipc_counters','per_audio_voice_state','per_draw_renderdoc_capture'],
                'render_debug_abi_unchanged':3}}

def stack(sdk,out,timeout=30):
    s=sdk.status()
    if not s['fresh']:raise RuntimeError('Live heartbeat required for stack capture')
    root=Path(out);root.mkdir(parents=True,exist_ok=True)
    inventory=runtime_inventory(sdk,s)
    exe=Path(r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe')
    if os.name!='nt' or not exe.is_file():raise RuntimeError('Local Windows CDB not present')
    symbols=sorted({str(Path(m['path']).parent) for m in inventory['loaded_modules']})
    # Developer builds keep matching PDBs beside the module build outputs.
    executable = Path(inventory.get('executable', ''))
    if len(executable.parents) >= 4:
        developer_modules = executable.parents[3] / 'modules' / 'x64'
        if developer_modules.is_dir():
            symbols = sorted(set(symbols) | {str(p.parent) for p in developer_modules.glob('*/Release/nxemu-*.pdb')})
    args=[str(exe),'-pvr' ,'-p',str(s['pid']),'-y',';'.join(symbols),'-c','~* k 24; q']
    path=root/'host-stacks.txt';started=time.time()
    with path.open('w',encoding='utf-8') as f:
        try:r=subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,timeout=timeout,creationflags=subprocess.CREATE_NO_WINDOW);code=r.returncode
        except subprocess.TimeoutExpired:code='timeout'
    after=sdk.status();result={'command':subprocess.list2cmdline(args),'input_pid':s['pid'],'exit_status':code,
        'seconds':time.time()-started,'path':str(path.resolve()),'noninvasive':True,
        'same_run_after':after['run_id']==s['run_id'],'process_alive_after':after['process_alive'],'fresh_after':after['fresh']}
    (root/'stack-result.json').write_text(json.dumps(result,indent=2));return result

def bundle(sdk,out,log=None):
    root=Path(out);root.mkdir(parents=True,exist_ok=True)
    snapshot=inspect(sdk);history=events(sdk,256)
    files={'inspection.json':json.dumps(snapshot,ensure_ascii=False,indent=2),'events.json':json.dumps(history,ensure_ascii=False,indent=2)}
    if log:
        d=sdk.diagnose(log)
        d.pop('diagnostic_artifacts',None) # Never include raw shader, memory, firmware or game dumps.
        files['log-diagnosis.json']=json.dumps(d,ensure_ascii=False,indent=2)
    for name,body in files.items():
        body=re.sub(r'(?i)(password|api[_-]?key|access[_-]?token)(\s*[=:]\s*)[^\s,;]+',r'\1\2[redacted]',body)
        (root/name).write_text(body,encoding='utf-8')
    path=root/'diagnostic-bundle.zip'
    with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED) as z:
        for name in files:z.write(root/name,name)
    with zipfile.ZipFile(path) as z:assert z.testzip() is None
    return {'path':str(path.resolve()),'files':list(files),'game_or_key_bodies_included':False}
