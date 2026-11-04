"""External startup/load supervisor. Observe exits even before status.json exists.

GUI launch uses the existing Cua Driver; control after startup uses NxEmuSDK.
No persistent debugger, network listener, registry change or save modification.
"""
from pathlib import Path
import ctypes,json,os,subprocess,time,shutil

CUA=Path(r'C:\Users\Administrator\AppData\Local\Programs\Cua\cua-driver\bin\cua-driver.exe')
PROCDUMP=Path(r'C:\Users\Administrator\AppData\Local\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\procdump64.exe')

def classify(result):
    if result.get('supervisor_error'):return 'observer_error'
    if result.get('guest_fatal'):return 'guest_fatal'
    if result.get('lifecycle_validation_failed'):return 'lifecycle_validation_failed'
    if result.get('exit_code') is None:return 'exit_unknown'
    if result.get('forced_cleanup'):return 'hung_or_stale_cleanup'
    if result['exit_code']==0 and result.get('quit'):return 'normal_exit'
    if result.get('exit_before_requested_quit')!=259:return 'early_exit'
    return 'abnormal_shutdown'

def capture_exit_stall(exe,pid,out,timeout=15):
    """Bounded non-invasive capture for this supervisor's still-live exact EXE.

    It deliberately does not require a fresh frontend heartbeat: shutdown may
    already have stopped the bridge. No invasive debugger or forced suspension.
    """
    import psutil
    root=Path(out);root.mkdir(parents=True,exist_ok=True)
    result={'input_pid':pid,'noninvasive':True,'heartbeat_required':False,'attempted':False}
    try:
        if Path(psutil.Process(pid).exe()).resolve()!=Path(exe).resolve():
            raise RuntimeError('Supervised executable identity changed')
        cdb=Path(r'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe')
        if not cdb.is_file():raise RuntimeError('CDB not present')
        symbols=[str(Path(exe).parent)]
        modules=Path(exe).parents[3]/'modules/x64'
        if modules.is_dir():symbols.extend(str(p.parent) for p in sorted(modules.glob('*/Release/nxemu-*.pdb'))[:16])
        args=[str(cdb),'-pvr','-p',str(pid),'-y',';'.join(symbols),'-c','~* k 24; q']
        result.update(attempted=True,command=subprocess.list2cmdline(args),input='all host threads,24 frames,noninvasive')
        started=time.monotonic()
        with (root/'host-stacks.txt').open('w',encoding='utf-8') as f:
            try:
                p=subprocess.run(args,stdout=f,stderr=subprocess.STDOUT,timeout=timeout,creationflags=subprocess.CREATE_NO_WINDOW)
                result['exit_status']=p.returncode
            except subprocess.TimeoutExpired:result['exit_status']='timeout'
        result['seconds']=time.monotonic()-started
        result['path']=str((root/'host-stacks.txt').resolve())
    except (OSError,RuntimeError,psutil.Error) as error:result['error']=str(error)
    (root/'stack-result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    return result

def diagnostic_arguments(options=None):
    """Explicit startup switches, also propagated through the Cua launcher."""
    allowed={'debug-detail','handle-trace','shader-rejection-trace','shader-fixtures','frame-capture','paired-pixels','image-writers','native-draw-inputs','shader-progress','pipeline-fixtures','pipeline-fixture-key','ssa-fixtures','native-compute-inputs','native-compute-input-hash'}
    options=options or {}
    if set(options)-allowed or any(type(v) is not int or v not in (0,1) for k,v in options.items() if k not in ('pipeline-fixture-key','native-compute-input-hash')):
        raise ValueError('Diagnostic options require supported names and integer 0/1')
    if 'pipeline-fixture-key' in options:
        import re
        if not isinstance(options['pipeline-fixture-key'],str) or not re.fullmatch('[0-9a-f]{16}',options['pipeline-fixture-key']):
            raise ValueError('Pipeline key requires 16 lowercase hex digits')
    if options.get('ssa-fixtures') == 1 and 'pipeline-fixture-key' not in options:
        raise ValueError('SSA fixtures require an explicit single pipeline-fixture-key')
    if options.get('native-draw-inputs') == 1 and (options.get('shader-fixtures') != 1 or options.get('image-writers') != 1):
        raise ValueError('Native draw inputs require shader-fixtures and image-writers')
    if 'native-compute-input-hash' in options:
        import re
        if not isinstance(options['native-compute-input-hash'],str) or not re.fullmatch('[0-9a-f]{16}',options['native-compute-input-hash']):
            raise ValueError('Compute input hash requires 16 lowercase hex digits')
    if options.get('native-compute-inputs') == 1 and (options.get('shader-fixtures') != 1 or 'native-compute-input-hash' not in options):
        raise ValueError('Compute inputs require shader-fixtures and explicit native-compute-input-hash')
    return [item for key in sorted(options) for item in ('--'+key,str(options[key]))]

def supervise(bridge,exe,game=None,seconds=30,out=None,capture_dump=True,readback_segments=None,keyboard_name=None,diagnostic_options=None,lifecycle_probe=None):
    from debug_sdk import NxEmuSDK
    import psutil
    exe=Path(exe).resolve(strict=True);bridge=Path(bridge).resolve()
    out=Path(out or bridge.parent).resolve();out.mkdir(parents=True,exist_ok=True)
    if seconds<=0:raise ValueError('seconds must be positive')
    diagnostic_args=diagnostic_arguments(diagnostic_options)
    if readback_segments not in (None,0,1):raise ValueError('readback_segments must be 0 or 1')
    if game:game=Path(game).resolve(strict=True)
    if any(p.info['name'] and p.info['name'].lower()=='nxemu.exe' for p in psutil.process_iter(['name'])):
        raise RuntimeError('Close existing NXEmu before an isolated supervised launch')
    if (bridge/'status.json').exists():raise ValueError('Use a new bridge directory for each launch')
    result={'schema':1,'exe':str(exe),'game':str(game) if game else None,'bridge':str(bridge),
            'duration_seconds':seconds,'started_epoch_ms':round(time.time()*1000),'exit_code':None,
            'bridge_ready':False,'load_accepted':False,'forced_cleanup':False,'samples':0}
    sdk=NxEmuSDK(bridge);handle=None;monitor=None;mlog=None;pid=None;last=None;fatal_watcher=None
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.restype=ctypes.c_void_p
    kernel.OpenProcess.argtypes=[ctypes.c_ulong,ctypes.c_bool,ctypes.c_ulong]
    kernel.GetExitCodeProcess.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
    kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    def process_exit():
        if not handle:return None
        value=ctypes.c_ulong()
        if not kernel.GetExitCodeProcess(handle,ctypes.byref(value)):return None
        return value.value
    def save():
        (out/'launch-result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    try:
        if capture_dump and PROCDUMP.exists():
            mlog=(out/'procdump.log').open('wb')
            args=[str(PROCDUMP),'-accepteula','-e','-n','1','-w','nxemu.exe',str(out)]
            result['dump_command']=subprocess.list2cmdline(args)
            monitor=subprocess.Popen(args,stdout=mlog,stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            time.sleep(.4)
        args=['--debug-bridge',str(bridge),'--nxshader-timing','1',
              '--nxshader-timing-file',str(out/'shader-startup-timing.json'),
              '--nxshader-runtime-report',str(out/'shader-runtime.json')]
        args+=diagnostic_args
        result['diagnostic_options_requested']=diagnostic_options or {}
        if readback_segments is not None:args+=['--buffer-readback-segments',str(readback_segments)]
        if keyboard_name is not None:args+=['--software-keyboard-name',keyboard_name]
        result['keyboard_name_requested']=keyboard_name
        result['readback_segments_requested']=readback_segments
        call=[str(CUA),'call','launch_app']
        p=subprocess.run(call,input=json.dumps({'path':str(exe),'additional_arguments':args}),
                         capture_output=True,text=True,timeout=35)
        result['launch_transport']={'command':subprocess.list2cmdline(call),'stdout':p.stdout,'stderr':p.stderr,'exit':p.returncode}
        if p.returncode:raise RuntimeError('Cua launch failed: '+p.stderr)
        launch=json.loads(p.stdout);result['launch']=launch;pid=launch['pid']
        handle=kernel.OpenProcess(0x1000,False,pid)
        if not handle:raise RuntimeError('Process handle unavailable after launch; exit code is unknown')
        start=time.monotonic();load_at=None
        with (out/'launch-status.jsonl').open('w',encoding='utf-8') as stream:
            while time.monotonic()-start<seconds:
                code=process_exit()
                if code!=259:result['exit_code']=code;break
                try:
                    last=sdk.status();result['last_status']=last
                    if last.get('fresh') and last.get('modules_valid'):
                        if not result['bridge_ready']:
                            result['bridge_ready']=True;result['bridge_ready_seconds']=round(time.monotonic()-start,3)
                            print('STARTUP_BRIDGE_READY pid='+str(pid),flush=True)
                        if game and load_at is None:
                            from debug_run_faults import RunFatalWatcher
                            fatal_watcher=RunFatalWatcher(exe.parent/'user/log/nxemu_log.txt')
                            result['load']=sdk.command('load',str(game));load_at=time.monotonic()
                            result['load_accepted']=result['load'].get('response')=='accepted'
                    sample=sdk.telemetry();sample['supervisor_elapsed_seconds']=round(time.monotonic()-start,3)
                    if (diagnostic_options or {}).get('shader-progress') == 1:
                        from debug_inspect import document
                        sample['shader_progress'] = document(sdk, 'shader-progress.json', last)
                    stream.write(json.dumps(sample)+'\n');stream.flush()
                    result['samples']+=1
                    if result['samples']%5==0:
                        print('STARTUP_SAMPLE '+json.dumps({'seconds':sample['supervisor_elapsed_seconds'],
                              'phase':last.get('phase'),'fps':last.get('game_fps'),
                              'frames':last.get('composites_completed')}),flush=True)
                except (OSError,ValueError,RuntimeError,TimeoutError) as error:
                    result['last_observer_error']=str(error)
                if fatal_watcher:
                    fault=fatal_watcher.poll()
                    result['fatal_watch']={'bytes_read':fatal_watcher.bytes_read,'scan_ns':fatal_watcher.scan_ns,
                                           'per_poll_budget':fatal_watcher.budget,'rotations':fatal_watcher.rotations}
                    if fault:
                        result['guest_fatal']=fault
                        result['guest_fatal']['supervisor_elapsed_seconds']=round(time.monotonic()-start,3)
                        print('STARTUP_GUEST_FATAL '+json.dumps(fault),flush=True)
                        save();break
                save();time.sleep(.5)
        result['exit_before_requested_quit']=process_exit()
        if process_exit()==259:
            if result['bridge_ready']:
                try:
                    if lifecycle_probe is not None and not result.get('guest_fatal'):
                        try:
                            result['lifecycle_probe']=lifecycle_probe(sdk,pid)
                            result['lifecycle_validation_failed']=result['lifecycle_probe'].get('passed') is not True
                        except Exception as error:
                            result['lifecycle_validation_failed']=True
                            result['lifecycle_probe']={'passed':False,'observer_error':str(error)}
                        save()
                    result['inspection']=sdk.inspect();result['events']=sdk.events(256,include_lifecycle=True)
                    from backing_identity import diagnose
                    result['backing_diagnosis']=diagnose(result['events']['events'])
                    result['quit']=sdk.command('quit')
                except (OSError,ValueError,RuntimeError,TimeoutError) as error:result['quit_error']=str(error)
            end=time.monotonic()+20
            while time.monotonic()<end and process_exit()==259:time.sleep(.1)
        result['exit_code']=process_exit()
    except Exception as error:
        result['supervisor_error']=str(error);result['exit_code']=process_exit()
    finally:
        if handle and process_exit()==259 and pid:
            proc=psutil.Process(pid)
            if Path(proc.exe()).resolve()==exe:
                result['shutdown_stall_capture']=capture_exit_stall(exe,pid,out/'shutdown-stall')
                result['exit_after_stall_capture']=process_exit()
                if process_exit()==259:
                    result['forced_cleanup']=True;proc.terminate();result['cleanup_exit']=proc.wait(10)
                result['exit_code']=process_exit()
        if hasattr(sdk,'_telemetry'):sdk._telemetry.gpu.close()
        if handle:kernel.CloseHandle(handle)
        if monitor:
            try:monitor.wait(10)
            except subprocess.TimeoutExpired:monitor.terminate();monitor.wait(5)
            mlog.close();result['dump_monitor_exit']=monitor.returncode
        log=exe.parent/'user/log/nxemu_log.txt'
        if log.exists():shutil.copy2(log,out/'runtime-game.log')
        result['dumps']=[str(p) for p in out.glob('*.dmp')]
        result['outcome']=classify(result)
        save()
    compact={k:result.get(k) for k in ['outcome','bridge_ready','load_accepted','samples','exit_code','forced_cleanup','dumps','guest_fatal']}
    print('STARTUP_RESULT '+json.dumps(compact),flush=True)
    return result,0 if result['outcome']=='normal_exit' else 4
