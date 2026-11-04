"""Explicit bounded CPU input snapshots; not executed shader inputs or an oracle."""
from pathlib import Path
import hashlib,json,re,struct,time

BOUNDS={'native':4096,'header':128,'parsed_header':80,'regs':14336,'cbuf':1024}
def uint(x):return type(x) is int and 0<=x<=0xffffffffffffffff
def fnv(b):
    h=14695981039346656037
    for v in b:h=((h^v)*1099511628211)&0xffffffffffffffff
    return f'{h:016x}'
def read_invocation(d):
    if d is None:return dict(available=False,reason='legacy_snapshot_without_invocation')
    if not isinstance(d,dict) or type(d.get('schema')) is not int or d['schema']!=1 or type(d.get('available')) is not bool:raise ValueError('Invocation schema')
    if not d['available']:
        if set(d)!=set(('schema','available','kind')) or type(d['kind']) is not int or d['kind']!=0:raise ValueError('Invocation unavailable')
        return d
    u32=['kind','instance_count','retained_instance_count','topology','draw_mode','vertex_first','vertex_count','index_first','index_count','base_index','base_instance','index_format','inline_prefix_size','instance_mask']
    u64=['index_address','inline_bytes_size','count_address','indirect_address','buffer_size','max_draw_count','stride']
    flags=['is_indexed','instance_count_available','retained_draw_indexed','is_byte_count','include_count','indexed_vertex_id_decoded','indirect_arguments_decoded']
    if set(d)!=set(['schema','available','inline_prefix','scope',*u32,*u64,*flags]):raise ValueError('Invocation fields')
    if any(not uint(d[k]) or d[k]>0xffffffff for k in u32) or any(not uint(d[k]) for k in u64) or any(type(d[k]) is not bool for k in flags):raise ValueError('Invocation types')
    if d['kind'] not in (1,2) or d['draw_mode']>2 or d['index_format']>2 or d['indexed_vertex_id_decoded'] or d['indirect_arguments_decoded'] or d['scope']!='cpu_retained_state_and_caller_arguments':raise ValueError('Invocation scope/mode')
    prefix=d['inline_prefix']
    if not isinstance(prefix,list) or d['inline_prefix_size']!=min(d['inline_bytes_size'],16) or len(prefix)!=d['inline_prefix_size'] or any(not uint(v) or v>255 for v in prefix):raise ValueError('Invocation prefix bound')
    if d['kind']==1:
        if not d['instance_count_available'] or d['is_byte_count'] or d['include_count'] or any(d[k] for k in ['count_address','indirect_address','buffer_size','max_draw_count','stride','instance_mask']):raise ValueError('Invocation direct arguments')
    elif d['instance_count_available'] or d['instance_count']:raise ValueError('Invocation indirect count not decoded')
    return d
def read_index_input(d,invocation):
    if d is None:return dict(available=False,reason='legacy_snapshot_without_index_prefix')
    fields=['requested','range_valid','mapped','address','limit_address','element_size','count','first','bytes_requested','prefix']
    if not isinstance(d,dict) or set(d)!=set(fields) or any(type(d[k]) is not bool for k in fields[:3]) or any(not uint(d[k]) for k in fields[3:-1]):raise ValueError('Index input fields')
    if any(d[k]>0xffffffff for k in ['element_size','count','first','bytes_requested']):raise ValueError('Index input u32')
    if not isinstance(d['prefix'],list) or any(not uint(v) or v>255 for v in d['prefix']):raise ValueError('Index prefix bytes')
    if not d['requested']:
        if any(d[k] for k in fields[1:]):raise ValueError('Unrequested index filled')
        return dict(available=False,data=d)
    if not invocation.get('available') or invocation['kind']!=1 or not invocation['is_indexed'] or invocation['draw_mode']==2:raise ValueError('Index invocation mismatch')
    size=1<<invocation['index_format'];address=invocation['index_address']+invocation['index_first']*size
    if d['element_size']!=size or d['count']!=invocation['index_count'] or d['first']!=invocation['index_first'] or d['address']!=(address if address<2**64 else 0) or d['bytes_requested']!=min(d['count']*size,16):raise ValueError('Index input addressing')
    if d['range_valid'] and (not d['bytes_requested'] or not d['address'] or d['bytes_requested']-1>2**64-1-d['address'] or (d['limit_address'] and (d['address']>d['limit_address'] or d['bytes_requested']-1>d['limit_address']-d['address']))):raise ValueError('Index input range')
    if d['mapped'] and not d['range_valid']:raise ValueError('Index input mapping')
    if len(d['prefix'])!=(d['bytes_requested'] if d['mapped'] else 0):raise ValueError('Index input availability')
    b=bytes(d['prefix']);values=[int.from_bytes(b[n:n+size],'little') for n in range(0,len(b)-size+1,size)]
    return dict(available=d['mapped'],data=d,index_prefix_values=values,prefix_complete=d['mapped'] and len(b)==d['count']*size,guest_limit_available=bool(d['limit_address']),GPU_visibility_verified=False)
def decode_clip_encoding(native):
    """Field decoding only. Public sm75 ISA dump b46f0b19, not a CSM oracle."""
    if not native or len(native)>4096 or len(native)%16:raise ValueError('Native encoding bound')
    csm=[];attributes=[]
    for offset in range(0,len(native),16):
        words=struct.unpack_from('<4I',native,offset);bits=int.from_bytes(native[offset:offset+16],'little')
        get=lambda start,count:(bits>>start)&((1<<count)-1)
        opcode=get(0,12)
        if opcode==0x80d:
            mode=get(62,2);cmp=get(76,3)
            csm.append(dict(offset=offset,words=list(words),execution_predicate=get(12,3),execution_negate=get(15,1),
                vote_index_28=get(32,28),reserved60_61=get(60,2),vtgmode=mode,vtgmode_name=['INVALID','R','A','RA'][mode],
                ccmp=cmp,ccmp_name=['F','CSM_TA','CSM_TR','CSM_MX','T','FCSM_TA','FCSM_TR','FCSM_MX'][cmp],
                bop=get(74,2),pu=get(81,3),pv=get(84,3),input_predicate=get(87,3),input_negate=get(90,1),opcode_extension=get(91,1)))
        elif opcode in (0x321,0x322):
            address=get(40,10);value=get(16,8) if opcode==0x321 else get(32,8)
            attributes.append(dict(offset=offset,operation='ALD' if opcode==0x321 else 'AST',byte_address=address,
                value_register=value,address_register=get(24,8),components=get(74,2)+1))
    return dict(csmtest=csm,attributes=attributes,numeric_GPU_semantics=False)
def read_case(path,status):
    if path.is_symlink() or not path.is_file() or path.stat().st_size>16384:raise ValueError('Invalid input manifest')
    d=json.loads(path.read_text(encoding='utf-8'))
    if type(d.get('schema')) is not int or d['schema']!=1 or d.get('status')!='ready':raise ValueError('Input schema/status')
    if any(not isinstance(d.get(k),str) or not re.fullmatch('[0-9]{1,64}',d[k]) for k in ('run_id','generation')):raise ValueError('Input session')
    for k in ('epoch_ms','token','program','stage','header_size','error_offset','opcode'):
        if not uint(d.get(k)):raise ValueError('Invalid input identity')
    if not d['token'] or not d['program'] or d['stage'] not in (0,4) or d['header_size']!=128:raise ValueError('Input stage/header')
    identity=f"draw-input-{d['run_id']}-{d['generation']}-{d['token']}-"
    if not isinstance(d.get('id'),str) or not re.fullmatch(re.escape(identity)+'[0-3]',d['id']) or path.name!=d['id']+'.json':raise ValueError('Input filename identity')
    if type(d.get('header_fully_mapped')) is not bool or type(d.get('reason_truncated')) is not bool:raise ValueError('Input availability flags')
    if not isinstance(d.get('reason'),str) or len(d['reason'])>192:raise ValueError('Input reason')
    words=d.get('words')
    if not isinstance(words,list) or len(words)!=4 or any(not uint(v) or v>0xffffffff for v in words):raise ValueError('Input words')
    c=d.get('cbuf')
    if not isinstance(c,dict):raise ValueError('Input cbuf')
    for k in ('bank','offset','address','declared_size'):
        if not uint(c.get(k)):raise ValueError('Input cbuf identity')
    if c['bank']>=18 or c['offset']>c['declared_size'] or any(type(c.get(k)) is not bool for k in ('enabled','fully_mapped')):raise ValueError('Input cbuf availability')
    files=d.get('files')
    if not isinstance(files,dict) or set(files)!=set(BOUNDS):raise ValueError('Input file roles')
    raw={};verified={}
    for role,limit in BOUNDS.items():
        x=files[role];name=d['id']+'.'+role+'.bin'
        if not isinstance(x,dict) or x.get('file')!=name or not uint(x.get('bytes')) or x['bytes']>limit:raise ValueError('Input path/size')
        f=path.parent/name
        if f.is_symlink() or not f.is_file() or f.stat().st_size!=x['bytes']:raise ValueError('Input file changed')
        b=f.read_bytes()
        if len(b)!=x['bytes'] or fnv(b)!=x.get('fnv1a64'):raise ValueError('Input integrity')
        raw[role]=b;verified[role]=dict(path=str(f.resolve()),bytes=len(b),sha256=hashlib.sha256(b).hexdigest())
    if not raw['native'] or len(raw['native'])%16 or len(raw['parsed_header'])!=80 or len(raw['regs'])!=14336:raise ValueError('Input fixed sizes')
    offset=d['error_offset']
    if offset%16 or offset+16>len(raw['native']) or list(struct.unpack_from('<4I',raw['native'],offset))!=words or words[0]&0xfff!=d['opcode']:raise ValueError('Input rejected instruction mismatch')
    if d['header_fully_mapped']:
        match=raw['header'][:80]==raw['parsed_header']
        if len(raw['header'])!=128 or type(d.get('parsed_header_matches_raw')) is not bool or d['parsed_header_matches_raw']!=match:raise ValueError('Raw/parsed header correlation')
    elif raw['header'] or d.get('parsed_header_matches_raw') is not None:raise ValueError('Unavailable header filled')
    if len(raw['cbuf'])>c['declared_size']-c['offset']:raise ValueError('Input cbuf slice bound')
    if (not c['enabled'] or not c['fully_mapped']) and raw['cbuf']:raise ValueError('Unavailable cbuf filled')
    if (not c['enabled'] and c['fully_mapped']) or (c['fully_mapped'] and not c['address']):raise ValueError('Input cbuf mapping')
    # Public Turing class fields, preserved separately from legacy bit0. They are
    # candidate command state, not a formula for returned WScale special registers.
    clip=struct.unpack_from('<I',raw['regs'],0x193c)[0]
    state=dict(raw_193c=clip,geometry_guardband_bit7=(clip>>7)&1,line_point_guardband_bit10=(clip>>10)&1,
               guardband_z_bits2_1=(clip>>1)&3,turing_z_clip_range_bits17_16=(clip>>16)&3,legacy_depth_bit0=clip&1)
    invocation=read_invocation(d.get('invocation'))
    return dict(available=True,data=d,files=verified,state=state,invocation=invocation,index_input=read_index_input(d.get('index_input'),invocation),encoding=decode_clip_encoding(raw['native']),matches_current_session=d['run_id']==status.get('run_id') and d['generation']==str(status.get('module_generation')),
                historical_snapshot=True,snapshot_age_seconds=max(0,time.time()-d['epoch_ms']/1000),scope='same admission token CPU snapshots; cbuf prefix is not full resource input; no GPU visibility or numeric oracle')

def inspect_inputs(bridge,status):
    directory=Path(bridge)/'diagnostics/native-draw-inputs'
    if not directory.exists():return dict(available=False,reason='native_draw_inputs_pending_or_disabled',cases=[])
    if directory.is_symlink() or not directory.is_dir():raise ValueError('Invalid input directory')
    paths=list(directory.glob('draw-input-*.json'))
    if len(paths)>4:raise ValueError('Input module-load case bound')
    cases=[];errors=[]
    for path in sorted(paths):
        try:cases.append(read_case(path,status))
        except (OSError,ValueError,TypeError,KeyError,struct.error) as error:errors.append(dict(path=str(path),error=str(error)[:256]))
    return dict(available=bool(cases),cases=cases,errors=errors,scope='explicit bounded historical CPU snapshots; no shader/GPU execution proof')
