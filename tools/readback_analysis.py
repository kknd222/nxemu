"""Analyze queued Vulkan image transfers; never label this PCIe or GPU completion."""
import json,sys
from pathlib import Path

def analyze(records,start=45,end=75):
    candidates=[]
    for r in records:
        elapsed=r.get('supervisor_elapsed_seconds',r.get('elapsed_seconds',0))
        doc=r.get('render',{}).get('readback-transfers',{})
        if start<=elapsed<=end and doc.get('fresh') and doc.get('session_matches') and 'totals' in doc.get('data',{}):
            candidates.append(doc)
    if len(candidates)<2:return {'available':False,'reason':'no_fresh_transfer_window','host_bytes_per_second':None}
    a,b=candidates[0],candidates[-1]
    if a['run_id']!=b['run_id'] or a['module_generation']!=b['module_generation']:
        return {'available':False,'reason':'mixed_session_window','host_bytes_per_second':None}
    seconds=(b['epoch_ms']-a['epoch_ms'])/1000
    if seconds<=0:return {'available':False,'reason':'no_elapsed_publication_time','host_bytes_per_second':None}
    x,y=a['data']['totals'],b['data']['totals']
    if y['calls']<x['calls'] or y['host_bytes_known']<x['host_bytes_known']:
        return {'available':False,'reason':'counter_reset','host_bytes_per_second':None}
    fields=['calls','regions','host_bytes_known','device_bytes_known','unknown_destination_bytes_known',
            'host_unknown_regions','unknown_destination_regions','rescaled_calls']
    delta={k:y.get(k,0)-x.get(k,0) for k in fields}
    complete=delta['host_unknown_regions']==0 and delta['unknown_destination_regions']==0
    reasons={}
    for k,v in b['data']['reasons'].items():
        old=a['data']['reasons'].get(k,{})
        reasons[k]={'calls':v['calls']-old.get('calls',0),
                    'host_bytes_known':v['host_bytes_known']-old.get('host_bytes_known',0)}
    return {'available':True,'scope':b['data']['scope'],'window_seconds':seconds,'delta':delta,
            'host_bytes_per_second':delta['host_bytes_known']/seconds if complete else None,
            'host_bytes_known_lower_bound_per_second':delta['host_bytes_known']/seconds,
            'host_bytes_complete_in_window':complete,'calls_per_second':delta['calls']/seconds,
            'reasons':reasons,'latest_top_images':b['data']['top_images'],
            'untracked_image_calls':b['data']['untracked_image_calls']}

if __name__=='__main__':
    records=[json.loads(s) for s in Path(sys.argv[1]).read_text().splitlines()]
    result=analyze(records,float(sys.argv[2]) if len(sys.argv)>2 else 45,float(sys.argv[3]) if len(sys.argv)>3 else 75)
    print(json.dumps(result,indent=2))
