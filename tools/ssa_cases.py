"""Bounded read-only paired SSA fixture inspection; not full GPU replay."""
from pathlib import Path
import json, hashlib, time

def fnv(raw):
    h=14695981039346656037
    for b in raw:h=((h^b)*1099511628211)&((1<<64)-1)
    return f'{h:016x}'

def inspect_cases(bridge, status=None):
    from debug_sdk import NxEmuSDK
    status=status if status is not None else NxEmuSDK(bridge).status()
    root=Path(bridge)/'diagnostics'/'ssa-fixtures'
    result={'available':False,'cases':[],'pending_only':[],'errors':[],
            'session_fresh_at_start':bool(status.get('fresh')),'status_sample_epoch_ms':status.get('epoch_ms'),
            'status_rechecked':False,'total_blob_budget':128*1024*1024,'scope':'SSA boundary graph only; environment, syntax and GPU resources absent'}
    if not root.is_dir():return result
    # One pair per module load; bounded file scan even for an externally supplied root.
    ready=sorted(root.glob('ssa-*.json'))[:32]
    consumed=0
    for path in ready:
        if path.name.endswith('.pending.json'):
            if not path.with_name(path.name.replace('.pending.json','.json')).exists():result['pending_only'].append(str(path))
            continue
        try:
            if path.stat().st_size>16384:raise ValueError('metadata budget exceeded')
            meta=json.loads(path.read_text(encoding='utf-8'))
            if meta.get('schema')!=1 or path.name!=meta.get('id','')+'.json':raise ValueError('schema or identity mismatch')
            same=meta.get('run_id')==status.get('run_id') and str(meta.get('module_generation'))==str(status.get('module_generation'))
            meta['matches_current_session']=same
            meta['historical_record']=True
            meta['phase_integrity']={}
            if meta.get('status')=='ready':
                for phase in ('pre','post'):
                    name=meta.get(phase+'_file');expected=meta['id']+'.'+phase+'.bin'
                    if name!=expected:raise ValueError('invalid phase filename')
                    blob=root/name;size=blob.stat().st_size
                    if not 0<size<=64*1024*1024 or size!=meta.get(phase+'_bytes'):raise ValueError('phase size mismatch')
                    if consumed+size>128*1024*1024:raise ValueError('aggregate blob inspection budget exceeded')
                    raw=blob.read_bytes();consumed+=size
                    if not raw.startswith(b'NXSSA001') or fnv(raw)!=meta.get(phase+'_content_id'):raise ValueError('phase content mismatch')
                    meta['phase_integrity'][phase]={'bytes':size,'sha256':hashlib.sha256(raw).hexdigest(),'path':str(blob.resolve())}
                result['available']=result['available'] or same
            result['cases'].append(meta)
        except (OSError,ValueError,KeyError,TypeError) as error:result['errors'].append({'path':str(path),'error':str(error)})
    result['max_metadata_files_scanned']=32
    result['bytes_inspected']=consumed
    result['inspection_finished_epoch_ms']=round(time.time()*1000)
    return result
