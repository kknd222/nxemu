"""Typed invocation reader and actual C++ published-file tests, not GPU execution."""
from pathlib import Path
import argparse,copy,hashlib,json,sys
from native_draw_inputs import read_invocation,read_case,read_index_input
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--cpp-output',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=False)
 r=json.loads((a.cpp_output/'RESULT.json').read_text());assert r['exit_status']==0
 cases=[];base=None
 for row in r['results']:
  if row['input']['mode'] in ['mapping','inline','indirect']:
   d=row['parsed'];assert read_invocation(d)==d;cases.append('actual_'+row['input']['mode'])
   if row['input']['mode']=='mapping':base=d
 assert base
 assert not read_invocation(None)['available'];assert not read_invocation({'schema':1,'available':False,'kind':0})['available'];cases+=['legacy','unavailable']
 mutations=[lambda x:x.update(schema=True),lambda x:x.update(available=1),lambda x:x.update(kind=True),lambda x:x.update(kind=3),lambda x:x.update(draw_mode=3),lambda x:x.update(index_format=3),lambda x:x.update(vertex_first=2**32),lambda x:x.update(vertex_count=-1),lambda x:x.update(index_address=2**64),lambda x:x.update(instance_count=True),lambda x:x.update(is_indexed=1),lambda x:x.update(indexed_vertex_id_decoded=True),lambda x:x.update(indirect_arguments_decoded=True),lambda x:x.update(scope='GPU'),lambda x:x.update(inline_prefix_size=17),lambda x:x.update(inline_prefix=[1]),lambda x:x.update(inline_bytes_size=1),lambda x:x.update(instance_count_available=False),lambda x:x.update(indirect_address=1),lambda x:x.update(instance_mask=1),lambda x:x.update(extra='unknown'),lambda x:x.pop('base_instance')]
 for n,mutate in enumerate(mutations):
  d=copy.deepcopy(base);mutate(d)
  try:read_invocation(d)
  except ValueError:cases.append('bad_'+str(n))
  else:raise AssertionError(('accepted mutation',n,d))
 for d in [{'schema':1,'available':False,'kind':0,'invalid':True},{'schema':1,'available':False,'kind':False}]:
  try:read_invocation(d)
  except ValueError:cases.append('bad_unavailable')
  else:raise AssertionError('Unavailable invalid accepted')
 v=copy.deepcopy(base);v.update(is_indexed=True,index_address=4096,index_first=0,index_count=3,index_format=1)
 index=dict(requested=True,range_valid=True,mapped=True,address=4096,limit_address=4101,element_size=2,count=3,first=0,bytes_requested=6,prefix=[0,0,1,0,2,0])
 out=read_index_input(index,v);assert out['prefix_complete'] and out['index_prefix_values']==[0,1,2] and not out['GPU_visibility_verified'];cases.append('index_uint16')
 for mutate in [lambda x:x.update(requested=1),lambda x:x.update(address=4097),lambda x:x.update(count=4),lambda x:x.update(element_size=4),lambda x:x.update(bytes_requested=17),lambda x:x.update(prefix=[0]),lambda x:x.update(prefix=[True,0,1,0,2,0]),lambda x:x.update(limit_address=4100),lambda x:x.update(range_valid=False),lambda x:x.update(mapped=False),lambda x:x.update(count=2**32)]:
  d=copy.deepcopy(index);mutate(d)
  try:read_index_input(d,v)
  except ValueError:cases.append('bad_index')
  else:raise AssertionError(('bad index accepted',d))
 d=copy.deepcopy(index);d.update(mapped=False,prefix=[]);assert not read_index_input(d,v)['available'];cases.append('index_unmapped')
 d=copy.deepcopy(index);d.update(limit_address=0);assert read_index_input(d,v)['available'] and not read_index_input(d,v)['guest_limit_available'];cases.append('unknown_limit_zero')
 paths=list((a.cpp_output/'capture/native-draw-inputs').glob('draw-input-*.json'));assert len(paths)==4
 for p in paths:
  d=json.loads(p.read_text());c=read_case(p,{'run_id':d['run_id'],'module_generation':int(d['generation'])});assert c['invocation']['available'] and c['matches_current_session'];cases.append('actual_manifest_'+str(d['token']))
 hashes={str(p.resolve()):hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path(__file__),Path(__file__).parent/'native_draw_inputs.py']}
 output=f'DRAW_INVOCATION_SDK_PASS; cases={len(cases)}; actual_manifests4=True; GPU=False'
 report=dict(command=' '.join([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),input='actual C++ helper JSON and capture manifest files; typed CPU fixture mutations',literal_stdout=output+'\n',literal_stderr='',exit_status=0,hashes=hashes,cases=cases,GPU_executed=False)
 (a.out/'RESULT.json').write_text(json.dumps(report,indent=2));print(output)
if __name__=='__main__':main()
