"""Pure scheduler contracts; fake SDK, no GPU or emulator launch."""
import json
from frame_series import FrameSeries
class SDK:
    def __init__(self):self.calls=0;self.run='r';self.gen=3;self.fresh=True;self.ready=True;self.frames=1;self.outcome='captured'
    def status(self):return {'fresh':self.fresh,'video_snapshot_available':self.ready,'composites_completed':self.frames,'run_id':self.run,'module_generation':self.gen}
    def capture_frame(self,timeout):self.calls+=1;return {'result':self.outcome,'timeout':timeout}
def rejected(times,duration=60,enabled=True):
    try:FrameSeries(times,duration,enabled)
    except ValueError:return
    raise AssertionError('invalid input accepted')
for t in [[],[1,2,3,4],[50,30],[30,30],[float('nan')],[float('inf')],[-1],[51]]:rejected(t)
rejected([30],enabled=False)
sdk=SDK();f=FrameSeries([30,50],60,True);f.poll(sdk,29);assert sdk.calls==0
sdk.fresh=False;f.poll(sdk,30);assert sdk.calls==0
sdk.fresh=True;sdk.frames=0;f.poll(sdk,31);assert sdk.calls==0
sdk.frames=1;f.poll(sdk,32);assert sdk.calls==1;f.poll(sdk,40);assert sdk.calls==1
f.poll(sdk,50);f.poll(sdk,51);assert sdk.calls==2 and len(f.result()['captures'])==2
sdk=SDK();f=FrameSeries([30,50],60,True);f.poll(sdk,30);sdk.gen=5;f.poll(sdk,50);assert sdk.calls==1 and f.halted=='session_changed'
sdk=SDK();sdk.outcome='observer_timeout';f=FrameSeries([30,50],60,True);f.poll(sdk,30);f.poll(sdk,50);assert sdk.calls==1 and f.halted
sdk=SDK();f=FrameSeries([30],60,True);f.poll(sdk,51);assert sdk.calls==0 and f.halted=='capture_deadline_passed'
sdk=SDK();f=FrameSeries([30,40,50],60,True)
for n in [30,40,50]:f.poll(sdk,n)
assert sdk.calls==3 and f.result()['max_captures']==3
print('FRAME_SERIES_TEST_PASS; fake_SDK=True; ordered_bounds=True; stale_zero_frames=True; one_outstanding=True; session_change=True; default_no_capture=True; GPU_executed=False')
