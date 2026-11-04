"""Verify exact captured SPIR-V and create-only recipe; never dispatch or certify pixels."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import time

MAX_BLOB=4*1024*1024
def bounded_json(path):
    with Path(path).open('rb') as stream: raw=stream.read(262145)
    if len(raw)>262144:raise ValueError('Pipeline JSON budget exceeded')
    return json.loads(raw.decode('utf-8-sig'))

def spirv_statistics(raw):
    if len(raw)<20 or len(raw)%4:raise ValueError('SPIR-V size/alignment invalid')
    words=struct.unpack('<'+'I'*(len(raw)//4),raw)
    if words[0]!=0x07230203 or words[4]!=0:raise ValueError('Invalid SPIR-V header')
    counts={};offset=5;instructions=0;capabilities=[]
    while offset<len(words):
        count=words[offset]>>16;opcode=words[offset]&65535
        if not count or count>len(words)-offset:raise ValueError('SPIR-V instruction length invalid')
        counts[opcode]=counts.get(opcode,0)+1;instructions+=1
        if opcode==17:
            if count!=2:raise ValueError('Invalid OpCapability')
            capabilities.append(words[offset+1])
        offset+=count
    names={54:'functions',57:'function_calls',248:'labels',247:'selection_merges',246:'loop_merges',249:'branches',250:'conditional_branches',61:'loads',62:'stores'}
    return {'words':len(words),'instructions':instructions,'version':words[1],'id_bound':words[3],
            'op_counts':{name:counts.get(opcode,0) for opcode,name in names.items()},'capabilities':capabilities,
            'structurally_walked':True,'spirv_semantics_validation_not_performed':True}

def fnv(raw):
    h=14695981039346656037
    for value in raw:h=((h^value)*1099511628211)&((1<<64)-1)
    return f'{h:016x}'

def inspect_cases(bridge,expected_session=None):
    root=Path(bridge)/'diagnostics/pipeline-fixtures'
    audit_path=Path(bridge)/'diagnostics/pipeline-capture.json'
    audit={'available':False,'reason':'capture_diagnostics_not_published'}
    if audit_path.is_file():
        try:
            payload=bounded_json(audit_path)
            if payload.get('schema')!=1 or payload.get('category')!='pipeline-capture' or not isinstance(payload.get('data'),dict):raise ValueError('Invalid capture diagnostic envelope')
            counters=payload['data'].get('counters')
            if not isinstance(counters,dict) or len(counters)>64 or any(not isinstance(k,str) or type(v) is not int or v<0 for k,v in counters.items()):raise ValueError('Invalid capture counters')
            stamp=payload.get('epoch_ms')
            if type(stamp) is not int or stamp<0:raise ValueError('Invalid capture timestamp')
            audit={'available':True,'path':str(audit_path.resolve()),'age_seconds':max(0,time.time()-stamp/1000),
                   'session':[payload.get('run_id'),str(payload.get('module_generation'))],
                   'expected_session_matches':None if expected_session is None else (payload.get('run_id'),str(payload.get('module_generation')))==tuple(expected_session),
                   'data':payload['data'],'fresh_live_state_not_verified':True,'scope':'module lifetime counters; file timestamp/session not proof of live process or complete shader coverage'}
        except (OSError,ValueError,TypeError) as error:
            audit={'available':False,'reason':str(error),'path':str(audit_path.resolve())}
    if not root.is_dir():return {'available':False,'reason':'pipeline_archive_not_published','cases':[],'capture_diagnostics':audit,'pending_only':[]}
    paths=[]
    for p in root.iterdir():
        if p.name.endswith('.pending.json'):continue
        if p.suffix=='.json':
            paths.append(p)
            if len(paths)>16:raise ValueError('Pipeline case count exceeded')
    result=[];total=0
    for path in sorted(paths):
        meta=bounded_json(path)
        cid=meta.get('id')
        if not isinstance(cid,str) or not re.fullmatch(r'pipeline-[0-9a-f]{16}-[0-9a-f]{16}-g[0-9]{1,20}-[0-9]{1,2}',cid):raise ValueError('Invalid pipeline ID')
        if path.name!=cid+'.json' or path.is_symlink():raise ValueError('Pipeline metadata path mismatch')
        if meta.get('schema')!=1 or meta.get('status')!='ready':raise ValueError('Pipeline metadata not ready')
        if expected_session and (meta.get('run_id'),str(meta.get('module_generation')))!=tuple(expected_session):raise ValueError('Pipeline session mismatch')
        blob=root/(cid+'.spv')
        if meta.get('spirv_file')!=blob.name or blob.is_symlink() or blob.resolve().parent!=root.resolve():raise ValueError('Pipeline blob path invalid')
        size=blob.stat().st_size;total+=size
        if size>MAX_BLOB or size<20 or total>16*1024*1024 or size!=meta.get('spirv_bytes') or size!=meta.get('spirv_words',0)*4:raise ValueError('Pipeline blob budget/size mismatch')
        raw=blob.read_bytes()
        if len(raw)!=size or meta.get('byte_order')!='little' or meta.get('hash_algorithm')!='fnv1a64-nonauthenticating' or fnv(raw)!=meta.get('content_id'):raise ValueError('Pipeline content integrity mismatch')
        stats=spirv_statistics(raw);recipe=meta.get('recipe',{});device=recipe.get('device');layout=recipe.get('descriptor_layout')
        gaps=[]
        if not isinstance(device,dict) or not device.get('feature_chain_complete'):gaps.append('device_enable_chain_incomplete')
        else:
            chain=device.get('feature_chain',[])
            if not isinstance(chain,list) or not 1<=len(chain)<=64:raise ValueError('Invalid device feature chain')
            seen=set()
            for node in chain:
                if not isinstance(node,dict) or type(node.get('s_type')) is not int or node['s_type'] in seen:raise ValueError('Duplicate or invalid feature node')
                seen.add(node['s_type']);values=node.get('values',[])
                if not isinstance(values,list) or not 0<len(values)<=256 or node.get('payload_bytes')!=len(values)*4 or node.get('payload_offset')!=16 or any(type(x) is not int or x not in (0,1) for x in values):raise ValueError('Invalid typed feature payload')
            if device.get('abi_pointer_bits')!=64:gaps.append('unsupported_abi')
            if not isinstance(device.get('enabled_extensions'),list) or len(device['enabled_extensions'])>256:raise ValueError('Invalid enabled extensions')
        if not isinstance(layout,dict) or not layout.get('recipe_complete'):gaps.append('descriptor_recipe_incomplete')
        else:
            bindings=layout.get('bindings',[])
            if not isinstance(bindings,list) or len(bindings)>1024:raise ValueError('Invalid descriptor bindings')
            seen=set()
            for row in bindings:
                if not isinstance(row,dict) or any(type(row.get(x)) is not int for x in ['binding','type','count','stages']):raise ValueError('Invalid descriptor binding fields')
                if row['binding'] in seen or not 0<=row['binding']<1024 or not 1<=row['count']<=65536 or row.get('immutable_samplers') is not None:raise ValueError('Invalid descriptor binding recipe')
                seen.add(row['binding'])
            if layout.get('set_count') not in (0,1) or layout.get('pnext') is not None:raise ValueError('Unsupported layout recipe')
        if not recipe.get('cache_blob_captured'):gaps.append('application_warm_cache_not_captured')
        if not isinstance(device,dict) or device.get('instance_extensions_not_captured'):gaps.append('exact_instance_extension_recipe_not_captured')
        result.append({'metadata':str(path.resolve()),'blob':str(blob.resolve()),'id':cid,'pipeline_hash':meta.get('pipeline_hash'),
                       'session':[meta.get('run_id'),str(meta.get('module_generation'))],'sha256':hashlib.sha256(raw).hexdigest(),'statistics':stats,
                       'recipe':recipe,'missing_for_exact_application_replay':gaps,'exact_application_replay_ready':not gaps,
                       'cold_create_candidate':not any(x in gaps for x in ['device_enable_chain_incomplete','unsupported_abi','descriptor_recipe_incomplete']),
                       'dispatch_replay_ready':False,'scope':'exact SPIR-V and captured create inputs; no GPU numerical validation'})
    ready_ids={case['id'] for case in result};pending_only=[]
    for path in sorted(root.glob('*.pending.json')):
        if len(pending_only)>=16:break
        cid=path.name.removesuffix('.pending.json')
        if cid not in ready_ids:pending_only.append({'id':cid,'path':str(path.resolve()),'scope':'pending metadata present without ready file; not proof task still running'})
    return {'available':bool(result),'cases':result,'count':len(result),'total_bytes':total,'capture_diagnostics':audit,'pending_only':pending_only}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--bridge',required=True);p.add_argument('--out');a=p.parse_args()
    try:
        result=inspect_cases(a.bridge)
        if a.out:Path(a.out).write_text(json.dumps(result,indent=2))
        print(json.dumps(result));return 0 if result['available'] else 3
    except (OSError,ValueError,TypeError) as error:
        print(json.dumps({'result':'pipeline_case_error','reason':str(error)}));return 2
if __name__=='__main__':raise SystemExit(main())
