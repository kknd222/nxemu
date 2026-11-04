"""Actual SDK reader with real files; synthetic identities, no GPU execution."""
import argparse,copy,hashlib,json,tempfile
from pathlib import Path
from image_writers import validate_admissions,inspect_writers

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);ap.add_argument('--cpp-output',type=Path);args=ap.parse_args()
    args.out.mkdir(parents=True,exist_ok=True);cases=[]
    n=dict(image=4096,birth=1,cpu_owner=8192,gpu_owner=12288,width=1280,height=720,parent=0,parent_birth=0,events_total=0,events_retained=0,events=[])
    r=dict(schema=1,available=True,selected_image=4096,serial=1,lock_drops=0,stale_drops=0,missing_drops=0,evictions=0,overflow=0,nodes=[n])
    p=dict(token=1,record_seq=1,outcome='pipeline_null',refresh='exception',key_valid=False,requested_pre_lookup_key=0,
           stage_visited_mask=1,stage_hash_valid_mask=0,failed_stage=0,hashes=[0]*6,programs=[99,0,0,0,0,0],stage_hashes=[0]*6,
           target=dict(owner=12288,slot=0,output=0,raw_width=2560,raw_height=1440,format=1,base_layer=0,depth=1,array_pitch=0,tile_mode=0,color_mask=4369),
           bindings_before_call=[dict(image=4096,birth=1,width=1280,height=720)],bindings_overflow=False,bindings_lockmiss=False,
           non_selected_bindings=0,owner_total=5,owner_retained=4,rejection=True,rejection_program=99,rejection_offset=16,rejection_stage=0,opcode=2499,
           words=[0x479c3,0,0x1e00,0xe7000],reason='native special register',reason_truncated=False)
    a=dict(schema=1,available=True,selected_image=4096,reason='matched',serial=5,lock_drops=0,unbound=0,binding_overflow=0,binding_lockmiss=0,evictions=0,overflow=0,nonmatching_packets=0,packets=[p])
    assert validate_admissions(a,r)['matching_packets']==1;cases.append('valid')
    changes=[('counter_bool',lambda x:x.update(serial=True)),('schema_bool',lambda x:x.update(schema=True)),
        ('availability',lambda x:x.update(available=False)),('selected',lambda x:x.update(selected_image=42)),
        ('packet_bound',lambda x:x.update(packets=x['packets']*21)),('duplicate',lambda x:x['packets'].append(copy.deepcopy(x['packets'][0]))),
        ('negative_token',lambda x:x['packets'][0].update(token=-1)),('sequence',lambda x:x['packets'][0].update(record_seq=6)),
        ('stale_key',lambda x:x['packets'][0].update(requested_pre_lookup_key=7)),('stale_hash',lambda x:x['packets'][0].update(hashes=[7]*6)),
        ('invalid_refresh',lambda x:x['packets'][0].update(refresh='rejected')),('invalid_outcome',lambda x:x['packets'][0].update(outcome='drawn')),
        ('flag_integer',lambda x:x['packets'][0].update(rejection=1)),('invalid_failed_stage',lambda x:x['packets'][0].update(failed_stage=True)),
        ('unvisited_hash',lambda x:x['packets'][0].update(stage_hash_valid_mask=2)),('array_length',lambda x:x['packets'][0].update(words=[0]*3)),
        ('owner_mismatch',lambda x:x['packets'][0]['target'].update(owner=1)),('slot',lambda x:x['packets'][0]['target'].update(slot=8)),
        ('output',lambda x:x['packets'][0]['target'].update(output=8)),('birth',lambda x:x['packets'][0]['bindings_before_call'][0].update(birth=2)),
        ('size_mismatch',lambda x:x['packets'][0]['bindings_before_call'][0].update(width=1)),('empty_bindings',lambda x:x['packets'][0].update(bindings_before_call=[])),
        ('reason_bound',lambda x:x['packets'][0].update(reason='x'*192))]
    for name,change in changes:
        b=copy.deepcopy(a);change(b)
        try:validate_admissions(b,r)
        except (ValueError,TypeError,KeyError):cases.append(name)
        else:raise AssertionError(name)
    request='a'*32;d=dict(schema=1,request_id=request,run_id='test',generation='1',composite_ordinal=1,snapshot_epoch_ms=1,gpu_finish_boundary=False,registry=r,admissions=a)
    with tempfile.TemporaryDirectory(dir=args.out) as t:
        root=Path(t);(root/'diagnostics').mkdir();f=root/'diagnostics'/('frame-capture-'+request+'.writers.json')
        f.write_text(json.dumps(d));valid=inspect_writers(root,request,{'run_id':'test','module_generation':1});assert valid['admissions']['matching_packets']==1
        d['admissions']['serial']=True;f.write_text(json.dumps(d));bad=inspect_writers(root,request,{'run_id':'test','module_generation':1})
        assert bad['available'] and bad['registry_available'] and bad['admissions']['reason']=='admission_invalid';cases.append('invalid_optional_preserves_writer')
        del d['admissions'];f.write_text(json.dumps(d));assert inspect_writers(root,request,{'run_id':'test','module_generation':1})['available'];cases.append('old_schema_preserved')
    cpp=[]
    if args.cpp_output:
        for f in args.cpp_output.glob('*.json'):
            x=json.loads(f.read_text());
            if not isinstance(x,dict) or 'packets' not in x:continue
            nodes={}
            for packet in x['packets']:
                for b in packet['bindings_before_call']:
                    nodes[b['image'],b['birth']]=dict(b,gpu_owner=packet['target']['owner'])
            rr=dict(selected_image=x['selected_image'],nodes=list(nodes.values()))
            if f.name=='maximum.json':
                # C++ serialization-size fixture deliberately has duplicate identities
                # and sequences outside its snapshot serial; it is not valid evidence.
                try:validate_admissions(x,rr)
                except ValueError:cases.append('cpp_maximum_semantically_invalid_rejected')
                else:raise AssertionError('maximum size fixture accepted as evidence')
            else:validate_admissions(x,rr);cpp.append(str(f))
    report=dict(passed=True,cases=cases,actual_cpp_json=cpp,GPU_executed=False,reader_sha256=hashlib.sha256(Path(__file__).with_name('image_writers.py').read_bytes()).hexdigest())
    (args.out/'RESULT.json').write_text(json.dumps(report,indent=2));print('ADMISSION_SDK_PASS; cases='+str(len(cases))+'; cpp_json='+str(len(cpp))+'; legacy_preserved=True; GPU=False')
if __name__=='__main__':main()
