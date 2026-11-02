"""Compare fresh BufferCache queued copies; these bytes are NOT PCIe traffic."""
import json,sys
from pathlib import Path

def analyze(records,start=45,end=110):
    candidates=[]
    for r in records:
        t=r.get('supervisor_elapsed_seconds',r.get('elapsed_seconds',0))
        d=r.get('render',{}).get('buffer-transfers',{})
        if start<=t<=end and d.get('fresh') and d.get('session_matches') and 'kinds' in d.get('data',{}):
            candidates.append(d)
    if len(candidates)<2:return {'available':False,'reason':'no_fresh_window','bytes_per_second':None}
    a,b=candidates[0],candidates[-1]
    if a['run_id']!=b['run_id'] or a['module_generation']!=b['module_generation']:
        return {'available':False,'reason':'mixed_session','bytes_per_second':None}
    seconds=(b['epoch_ms']-a['epoch_ms'])/1000
    if seconds<=0:return {'available':False,'reason':'no_elapsed_time','bytes_per_second':None}
    delta={}
    for kind,v in b['data']['kinds'].items():
        old=a['data']['kinds'].get(kind)
        if old is None or any(v[k]<old[k] for k in ['calls','regions','bytes']):
            return {'available':False,'reason':'counter_reset_or_missing','bytes_per_second':None}
        x={k:v[k]-old[k] for k in ['calls','regions','bytes']}
        x['bytes_per_second']=x['bytes']/seconds;delta[kind]=x
    fallback={}
    for kind,v in b['data'].get('fallback_constraints_at_direct_copy',{}).items():
        old=a['data'].get('fallback_constraints_at_direct_copy',{}).get(kind)
        if old is None:continue
        fallback[kind]={'calls':v['calls']-old['calls'],'bytes':v['bytes']-old['bytes'],
            'max_copy_bytes':v['max_copy_bytes'],'max_buffer_bytes':v['max_buffer_bytes']}
    retirement=None
    if 'retired_payload' in a['data'] and 'retired_payload' in b['data']:
        x,y=a['data']['retired_payload'],b['data']['retired_payload']
        keys=['calls','logical_bytes','changed_bytes','unchanged_bytes','chunks','changed_chunks','flag_bytes']
        if all(k in x and k in y and y[k]>=x[k] for k in keys):
            retirement={k:y[k]-x[k] for k in keys}
            retirement['changed_bytes_per_second']=retirement['changed_bytes']/seconds
            retirement['changed_fraction']=retirement['changed_bytes']/retirement['logical_bytes'] if retirement['logical_bytes'] else None
    segments=None
    if 'segmented_plans' in a['data'] and 'segmented_plans' in b['data']:
        x,y=a['data']['segmented_plans'],b['data']['segmented_plans']
        if all(y[k]>=x[k] for k in ['calls','segments','logical_bytes']):segments={k:y[k]-x[k] for k in ['calls','segments','logical_bytes']}
    cpu=b['data']['diagnostic_cpu_ns']-a['data']['diagnostic_cpu_ns']
    return {'available':True,'scope':b['data']['scope'],'seconds':seconds,'kinds':delta,
        'segmented_readback_enabled':b['data'].get('segmented_readback_enabled'),'segmented_plans':segments,'retired_payload':retirement,'fallback_constraints_at_direct_copy':fallback,'diagnostic_cpu_ms':cpu/1e6 if cpu>=0 else None,
        'diagnostic_cpu_percent_one_core':cpu/1e9/seconds*100 if cpu>=0 else None,
        'differential_bytes_are_logical_not_actual_host_writes':True}

if __name__=='__main__':
    records=[json.loads(s) for s in Path(sys.argv[1]).read_text(encoding='utf-8-sig').splitlines()]
    print(json.dumps(analyze(records,float(sys.argv[2]) if len(sys.argv)>2 else 45,float(sys.argv[3]) if len(sys.argv)>3 else 110),indent=2))
