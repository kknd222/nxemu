"""Actual feature-node ABI parser regression; no Vulkan creation or driver execution."""
from pathlib import Path
import argparse,copy,hashlib,importlib.util,json,subprocess,sys

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);ap.add_argument('--probe',type=Path);ap.add_argument('--expect-missing',action='store_true');a=ap.parse_args()
 R=Path(__file__).resolve().parents[1];src=a.probe.resolve(strict=True) if a.probe else R/'tools/pipeline_probe.py';O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
 sys.path.insert(0,str(R/'tools'));spec=importlib.util.spec_from_file_location('tested_pipeline_probe',src);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
 sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();h=sha(src)
 core=dict(s_type=1000059000,payload_offset=16,payload_bytes=220,values=[0]*55)
 node=dict(s_type=1000081001,payload_offset=16,payload_bytes=8,values=[1,0]);accepted=missing=rejects=0
 for x in range(2):
  for y in range(2):
   n=copy.deepcopy(node);n['values']=[x,y]
   try:
    result=m.feature_nodes([core,n]);assert len(result)==2 and list(result[1].values)==[x,y];accepted+=1
   except ValueError as e:
    assert a.expect_missing and 'Unknown' in str(e);missing+=1
 bad=[]
 for key,value in [('s_type',1000081000),('payload_offset',8),('payload_bytes',4),('values',[1]),('values',[1,0,0]),('values',[2,0]),('values',[True,0]),('values',None)]:
  n=copy.deepcopy(node);n[key]=value;bad.append([core,n])
 bad.extend([[core,node,node],[node,core],[],[core]*65])
 for chain in bad:
  try:m.feature_nodes(chain)
  except ValueError:rejects+=1
  else:raise AssertionError('bad ABI payload accepted')
 assert rejects==12 and (missing==4 if a.expect_missing else accepted==4)
 literal=f'CONDITIONAL_FEATURE_RECIPE_PASS accepted={accepted} expected_missing={missing} rejected={rejects} actual_feature_nodes=True GPU=False'
 j=dict(command=subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),input='actual node sType1000081001 payload8 bool32x2;4combinations;12 malformed ABI cases',source=str(src),source_sha256=h,inputs_unchanged=sha(src)==h,literal_output=literal,exit_status=0,passed=True)
 (O/'RESULT.json').write_text(json.dumps(j,indent=2));print(literal)
if __name__=='__main__':main()
