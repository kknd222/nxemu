"""Bounded same-process stop/reload regression through actual NXEmu bridge.
No game input, ROM/save edits, drive/driver changes. Uses existing launch and stall capture.
"""
from pathlib import Path
import argparse,json,time
from debug_launch import supervise
from debug_run_faults import RunFatalWatcher

def reload_cycle(sdk,pid,game,out,seconds,exe):
    out=Path(out);result={'passed':False,'same_process':False};first=sdk.inspect(with_host=False)
    initial=first['status'];result['first']=first
    (out/'reload-first-inspection.json').write_text(json.dumps(first,indent=2))
    try:
        if not initial['fresh'] or initial['pid']!=pid:raise RuntimeError('First run not fresh or wrong PID')
        stop=sdk.command('stop');result['stop_ack']=stop
        if stop['response']!='accepted':raise RuntimeError('Stop rejected')
        deadline=time.monotonic()+20
        while True:
            idle=sdk.status()
            if idle['pid']!=pid or idle['run_id']!=initial['run_id']:raise RuntimeError('Process/run changed during stop')
            if not idle['fresh']:raise RuntimeError('Stop heartbeat stale')
            if idle['phase']=='idle' and idle['milestone']=='backend_ready' and idle['modules_valid']:break
            if time.monotonic()>=deadline:raise TimeoutError('Stop/reset not ready within20s')
            time.sleep(.1)
        result['idle']=idle
        watcher=RunFatalWatcher(Path(exe).parent/'user/log/nxemu_log.txt')
        load=sdk.command('load',str(game));result['reload_ack']=load
        if load['response']!='accepted':raise RuntimeError('Reload rejected')
        deadline=time.monotonic()+seconds;observed=[]
        with (out/'reload-status.jsonl').open('w') as stream:
            while time.monotonic()<deadline:
                state=sdk.status()
                if state['pid']!=pid or state['run_id']!=initial['run_id'] or not state['fresh']:raise RuntimeError('Reload process/run mismatch or stale')
                row={'elapsed_seconds':seconds-(deadline-time.monotonic()),'status':state}
                stream.write(json.dumps(row)+'\n');stream.flush();observed.append(state)
                fault=watcher.poll()
                if fault:result['guest_fatal']=fault;raise RuntimeError('Guest fatal during reload')
                time.sleep(.5)
        second=sdk.inspect(with_host=False);result['second']=second
        progress=second['render']['shader-progress']['data'];result['same_process']=second['status']['pid']==pid and second['status']['run_id']==initial['run_id']
        result['generation_advanced']=second['status']['module_generation']>initial['module_generation']
        result['tasks_completed']=progress['begun']>0 and progress['begun']==progress['completed']
        result['mixed_drops']=progress['mixed_session_drops'];result['update_drops']=progress['update_drops']
        result['frames_observed']=max(s['composites_completed'] for s in observed)>0
        result['passed']=result['same_process'] and result['generation_advanced'] and result['tasks_completed'] and result['mixed_drops']==0 and result['update_drops']==0 and result['frames_observed']
    except (RuntimeError,TimeoutError,OSError,KeyError,ValueError) as error:result['observer_error']=str(error)
    (out/'reload-result.json').write_text(json.dumps(result,indent=2))
    print('RELOAD_RESULT '+json.dumps({k:v for k,v in result.items() if k not in ['first','second','stop_ack','idle','reload_ack']}),flush=True)
    return result

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--game',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--seconds',type=int,choices=range(1,31),default=30)
    ap.add_argument('--exe',type=Path,default=Path(__file__).resolve().parents[1]/'bin/x64/Release/nxemu.exe')
    a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    result,code=supervise(out/'bridge',a.exe,a.game,a.seconds,out=out,diagnostic_options={'debug-detail':0,'shader-progress':1},lifecycle_probe=lambda sdk,pid:reload_cycle(sdk,pid,a.game,out,a.seconds,a.exe))
    raise SystemExit(code)
