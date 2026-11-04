"""Bounded CPU mirror input reader; never certifies GPU visibility or replay."""
from pathlib import Path
import hashlib, json, re, time

def uint(value, bits=64):
    return type(value) is int and 0 <= value < 1 << bits

def fnv(data):
    h=14695981039346656037
    for byte in data:h=((h^byte)*1099511628211)&0xffffffffffffffff
    return f'{h:016x}'

def decode(path, status=None):
    path=Path(path)
    match=re.fullmatch(r'compute-input-([0-3])\.json',path.name)
    if not match or path.is_symlink() or path.stat().st_size>16384:raise ValueError('metadata path/size')
    d=json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(d,dict) or type(d.get('schema')) is not int or d['schema']!=1 or d.get('status')!='ready':raise ValueError('schema/status')
    for k in ('run_id','module_generation'):
        if not isinstance(d.get(k),str) or not re.fullmatch(r'[0-9]{1,256}',d[k]):raise ValueError('session')
    for k in ('epoch_ms','program','dispatch_program','dispatch_token'):
        if not uint(d.get(k)) or not d[k]:raise ValueError('identity')
    if d['program']!=d['dispatch_program']:raise ValueError('different program')
    if not isinstance(d.get('native_content_id'),str) or not re.fullmatch('[0-9a-f]{16}',d['native_content_id']):raise ValueError('native identity')
    if d.get('read_mode')!='CPU_MIRROR_NO_FLUSH' or any(d.get(k) is not False for k in ('GPU_visibility_proven','synchronization_added','full_resource_replay')):raise ValueError('false GPU claim')
    c=d.get('cbuf')
    if not isinstance(c,dict) or not uint(c.get('slot'),32) or c['slot']!=0:raise ValueError('cbuf slot')
    if not uint(c.get('address')) or not c['address'] or not uint(c.get('declared_size'),32) or not uint(c.get('captured_size'),32):raise ValueError('cbuf identity')
    if not 0<c['captured_size']<=min(c['declared_size'],256) or c['captured_size']-1>0xffffffffffffffff-c['address']:raise ValueError('cbuf range')
    if c.get('enabled') is not True or c.get('captured_range_fully_mapped_at_check') is not True:raise ValueError('availability')
    blob=f'compute-input-{match.group(1)}-cbuf0.bin'
    if c.get('blob')!=blob or not isinstance(c.get('content_id'),str) or not re.fullmatch('[0-9a-f]{16}',c['content_id']):raise ValueError('blob identity')
    p=path.parent/blob
    if p.is_symlink() or p.stat().st_size!=c['captured_size']:raise ValueError('blob path/size')
    data=p.read_bytes()
    if fnv(data)!=c['content_id']:raise ValueError('payload hash')
    session_match=None
    if status:session_match=(str(status.get('run_id'))==d['run_id'] and str(status.get('module_generation'))==d['module_generation'])
    return {'metadata':d,'blob_sha256':hashlib.sha256(data).hexdigest(),'cbuf_bytes_hex':data.hex(),
        'session_match':session_match,'age_seconds':max(0,(time.time()*1000-d['epoch_ms'])/1000),
        'GPU_visibility_proven':False,'replay_ready':False}

def inspect_inputs(bridge,status=None):
    directory=Path(bridge)/'diagnostics/native-compute-inputs'
    result={'available':False,'CPU_ONLY':True,'GPU_visibility_proven':False,'replay_ready':False,'inputs':[],'errors':[]}
    if not directory.exists():result['reason']='not_captured_or_disabled';return result
    if directory.is_symlink():result['errors'].append('symlink directory');return result
    for n in range(4):
        p=directory/f'compute-input-{n}.json'
        if not p.exists():continue
        try:result['inputs'].append(decode(p,status))
        except (OSError,ValueError,TypeError) as error:result['errors'].append({'path':str(p),'error':str(error)})
    result['available']=bool(result['inputs'])
    result['limitations']=['four CPU snapshots only','not atomic with guest writes or mapping','no texture/SSBO data','no GPU completion or consumer proof']
    return result
