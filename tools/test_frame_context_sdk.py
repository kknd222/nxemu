from pathlib import Path
import sys,json,time,hashlib,tempfile,types
P=Path(__file__).resolve().parents[2];R=P/'repo';sys.path.insert(0,str(R/'tools'))
source=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
n={'__name__':'sdk_fixture'};exec(compile(source.read_text(),str(source),'exec'),n)
Base=n['NxEmuSDK'];new=hasattr(Base,'frame_context');results=[]
for scenario in (['missing','valid','invalid'] if new else ['baseline']):
 root=out/scenario;root.mkdir(exist_ok=True);ctx=None
 class Fake(Base):
  def status(self):
   request=root/'diagnostics/frame-capture-request.json'
   if request.exists():
    d=json.loads(request.read_text());id=d['id'];q=root/'diagnostics'
    (q/('frame-capture-'+id+'.ppm')).write_bytes(b'P6\n1280 720\n255\n'+b'\x01'*1280*720*3)
    (q/('frame-capture-'+id+'.json')).write_text(json.dumps({'request_id':id,'run_id':'r','generation':'3','status':'completed','gpu_readback_completed':True}))
    if scenario=='valid':
     data={'schema':1,'request_id':id,'run_id':'r','generation':'3','snapshot_epoch_ms':int(time.time()*1000),'input_count':0,'inputs':[],'source_count':0,'sources':[]}
     (q/('frame-capture-'+id+'.context.json')).write_text(json.dumps(data))
    elif scenario=='invalid':(q/('frame-capture-'+id+'.context.json')).write_text('invalid JSON')
   return {'fresh':True,'video_snapshot_available':True,'diagnostic_config':{'frame_capture':True},'run_id':'r','module_generation':3}
 result=Fake(root).capture_frame(1)
 assert result['result']=='captured' and result['all_rgb_zero'] is False
 if scenario=='baseline':assert 'context' not in result
 if scenario=='missing':assert result['context']['available'] is False
 if scenario=='valid':assert result['context']['available'] and result['context']['matches_current_session']
 if scenario=='invalid':assert result['context']['available'] is False and result['context']['reason']=='context_invalid'
 results.append({'scenario':scenario,'pixel_sha256':result['sha256'],'context':result.get('context')})
assert len({x['pixel_sha256'] for x in results})==1
record={'actual_SDK_method':True,'fake_GPU_publisher':True,'real_filesystem':True,'GPU_executed':False,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'cases':results}
(out/'RESULT.json').write_text(json.dumps(record,indent=2))
print('SDK_CONTEXT_PASS; actual_method=True; fake_GPU=True; pixel_payload_preserved=True; scenarios='+','.join(x['scenario'] for x in results))
