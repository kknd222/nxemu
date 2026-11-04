"""Request-bound command-record history, not complete GPU dependency or results."""
from pathlib import Path
import json,re,time
OPS={'draw_recorded','draw_skipped_unbuilt','clear_recorded','copy_recorded','scale_up_recorded',
     'scale_down_recorded','scale_select_without_copy','upload_recorded','scale_helper_queued_not_record_proof',
     'copy_queued_not_record_proof','upload_queued_not_record_proof'}
def uint(x):return type(x) is int and 0<=x<=0xffffffffffffffff
def validate_admissions(a,r):
    """Owner/birth candidates, not resolved framebuffer attachments or GPU writes."""
    if not isinstance(a,dict) or type(a.get('schema')) is not int or a['schema']!=1:raise ValueError('Invalid admission schema')
    for k in ('selected_image','serial','lock_drops','unbound','binding_overflow','binding_lockmiss','evictions','overflow','nonmatching_packets'):
        if not uint(a.get(k)):raise ValueError('Invalid admission counter')
    if 'first_rejection_replaced' in a and not uint(a['first_rejection_replaced']):raise ValueError('Invalid first rejection counter')
    packets=a.get('packets')
    if (not isinstance(packets,list) or len(packets)>20 or type(a.get('available')) is not bool or
        a['available']!=bool(packets) or a['selected_image']!=r['selected_image']):raise ValueError('Invalid admission sample identity/bounds')
    identities={(n['image'],n['birth']):n for n in r['nodes']}
    seen=set()
    for p in packets:
        if not isinstance(p,dict):raise ValueError('Invalid admission packet')
        for k in ('token','record_seq','requested_pre_lookup_key','stage_visited_mask','stage_hash_valid_mask','owner_total','owner_retained','non_selected_bindings','rejection_program','rejection_offset','rejection_stage','opcode'):
            if not uint(p.get(k)):raise ValueError('Invalid admission packet number')
        if not p['token'] or not 0<p['record_seq']<=a['serial']:raise ValueError('Invalid admission sequence')
        for k in ('key_valid','bindings_overflow','bindings_lockmiss','rejection','reason_truncated'):
            if type(p.get(k)) is not bool:raise ValueError('Invalid admission flag')
        if 'retained_first_rejection' in p and type(p['retained_first_rejection']) is not bool:raise ValueError('Invalid first rejection flag')
        if p.get('outcome') not in {'pipeline_ready','pipeline_null','async_skipped','wait_failed','exception_aborted'}:raise ValueError('Invalid admission outcome')
        if p.get('refresh') not in {'unknown','cached_valid','cached_invalid','refreshed','invalid_address','exception'}:raise ValueError('Invalid admission refresh')
        if type(p.get('failed_stage')) is not int or not -1<=p['failed_stage']<6:raise ValueError('Invalid failed stage')
        for k,size in (('hashes',6),('programs',6),('stage_hashes',6),('words',4)):
            if not isinstance(p.get(k),list) or len(p[k])!=size or any(not uint(v) for v in p[k]):raise ValueError('Invalid admission shader packet')
        if p['stage_visited_mask']>63 or p['stage_hash_valid_mask'] & ~p['stage_visited_mask']:raise ValueError('Invalid admission stage validity')
        if not p['key_valid'] and (p['requested_pre_lookup_key'] or any(p['hashes'])):raise ValueError('Stale admission key')
        t=p.get('target')
        if not isinstance(t,dict) or any(not uint(t.get(k)) for k in ('owner','slot','output','raw_width','raw_height','format','base_layer','depth','array_pitch','tile_mode','color_mask')):raise ValueError('Invalid admission target')
        if not t['owner'] or t['slot']>7 or t['output']>7:raise ValueError('Invalid admission target mapping')
        identity=(p['token'],p['record_seq'])
        if identity in seen:raise ValueError('Duplicate admission packet')
        seen.add(identity)
        bindings=p.get('bindings_before_call')
        if not isinstance(bindings,list) or not 0<len(bindings)<=8:raise ValueError('Invalid admission bindings')
        bound=set()
        for b in bindings:
            if not isinstance(b,dict) or any(not uint(b.get(k)) for k in ('image','birth','width','height')):raise ValueError('Invalid admission birth')
            bi=(b['image'],b['birth']);n=identities.get(bi)
            if bi in bound or not n or n['gpu_owner']!=t['owner'] or b['width']!=n['width'] or b['height']!=n['height']:raise ValueError('Admission owner/birth mismatch')
            bound.add(bi)
        if not isinstance(p.get('reason'),str) or len(p['reason'])>191:raise ValueError('Invalid admission reason')
    return {'available':True,'matching_packets':len(packets),'data':a,'scope':'guest owner plus before-call cached birth candidates; not resolved host attachment or GPU write proof'}
def inspect_writers(bridge,request_id,status):
    if not isinstance(request_id,str) or not re.fullmatch('[0-9a-f]{32}',request_id):raise ValueError('Expected capture UUID')
    p=Path(bridge)/'diagnostics'/('frame-capture-'+request_id+'.writers.json')
    if not p.is_file():return {'available':False,'reason':'writers_pending_or_disabled','path':str(p)}
    if p.is_symlink() or p.stat().st_size>65536:raise ValueError('Invalid writers metadata path/budget')
    d=json.loads(p.read_text(encoding='utf-8'))
    if type(d.get('schema')) is not int or d['schema']!=1 or d.get('request_id')!=request_id:raise ValueError('Writer schema/request mismatch')
    if any(not isinstance(d.get(k),str) or not 0<len(d[k])<=64 for k in ('run_id','generation')):raise ValueError('Invalid writer session')
    if not uint(d.get('composite_ordinal')) or not uint(d.get('snapshot_epoch_ms')) or type(d.get('gpu_finish_boundary')) is not bool:raise ValueError('Invalid writer sampling boundary')
    r=d.get('registry');nodes=r.get('nodes') if isinstance(r,dict) else None
    if (not isinstance(nodes,list) or len(nodes)>4 or any(not isinstance(n,dict) for n in nodes) or
        type(r.get('schema')) is not int or r['schema']!=1 or type(r.get('available')) is not bool):raise ValueError('Invalid writer registry')
    for k in ['selected_image','serial','lock_drops','stale_drops','missing_drops','evictions','overflow']:
        if not uint(r.get(k)):raise ValueError('Invalid writer counter')
    if r['available']!=bool(nodes):raise ValueError('Writer availability/node mismatch')
    if nodes and nodes[0].get('image')!=r['selected_image']:raise ValueError('Writer selected-image mismatch')
    seen=set()
    for n in nodes:
        for k in ['image','birth','cpu_owner','gpu_owner','width','height','parent','parent_birth','events_total','events_retained']:
            if not uint(n.get(k)):raise ValueError('Invalid writer node identity')
        if not n['image'] or not 0<n['birth']<=r['serial'] or (n['image'],n['birth']) in seen:raise ValueError('Duplicate/missing writer lifetime')
        seen.add((n['image'],n['birth']))
        if n['parent_birth']>r['serial'] or (not n['parent'] and n['parent_birth']):raise ValueError('Invalid parent birth')
        events=n.get('events')
        if not isinstance(events,list) or len(events)>8 or n['events_retained']!=len(events) or n['events_total']<len(events):raise ValueError('Invalid writer event bound')
        previous=0
        for e in events:
            if not isinstance(e,dict) or e.get('op') not in OPS:raise ValueError('Invalid writer operation')
            for k in ['seq','dst','dst_birth','src','src_birth','shader_key']:
                if not uint(e.get(k)):raise ValueError('Invalid writer event identity')
            if e['dst']!=n['image'] or e['dst_birth']!=n['birth'] or not max(previous,n['birth'])<e['seq']<=r['serial']:raise ValueError('Stale/out-of-order writer event')
            previous=e['seq']
            if e['src_birth']>=e['seq'] or (not e['src'] and e['src_birth']):raise ValueError('Invalid source birth')
            for k,count in [('hashes',6),('aux',4)]:
                if not isinstance(e.get(k),list) or len(e[k])!=count or any(not uint(v) for v in e[k]):raise ValueError('Invalid writer shader/operation packet')
            if 'clear_region_known' in e:
                if type(e['clear_region_known']) is not bool:raise ValueError('Invalid clear region availability')
                for k,count in [('clear_rect',4),('clear_layers',2)]:
                    if not isinstance(e.get(k),list) or len(e[k])!=count or any(not uint(v) for v in e[k]):raise ValueError('Invalid clear region')
    admissions={'available':False,'reason':'admission_pending_or_old_writer_schema'}
    if 'admissions' in d:
        try:admissions=validate_admissions(d['admissions'],r)
        except (ValueError,TypeError,KeyError) as error:admissions={'available':False,'reason':'admission_invalid','error':str(error)[:256]}
    match=d['run_id']==status.get('run_id') and d['generation']==str(status.get('module_generation'))
    return {'available':True,'registry_available':r['available'],'matches_current_session':match,
            'historical_snapshot':True,'snapshot_age_seconds':max(0,time.time()-d['snapshot_epoch_ms']/1000),
            'path':str(p.resolve()),'data':d,'admissions':admissions,'scope':'CPU record events up to bounded capture boundary; omissions/unknown writers not proof of no writes'}
