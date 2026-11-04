"""Bounded scheduled offscreen recomposition observations, not presented-window pixels."""
import math,time
class FrameSeries:
    def __init__(self, times, seconds, enabled, timeout=8):
        if not enabled:raise ValueError('Scheduled captures require --capture-frame')
        if not 0<timeout<=10:raise ValueError('timeout must be 0..10s')
        if not 1<=len(times)<=3:raise ValueError('capture-at allows 1..3 times')
        if any(not math.isfinite(t) or t<0 or t>seconds-timeout-2 for t in times):raise ValueError('capture-at needs a timeout plus 2s shutdown margin')
        if any(b<=a for a,b in zip(times,times[1:])):raise ValueError('capture-at must be strictly increasing')
        self.times=list(times);self.seconds=seconds;self.timeout=timeout;self.rows=[];self.session=None;self.halted=None
    def poll(self,sdk,elapsed):
        if self.halted or len(self.rows)>=len(self.times) or elapsed<self.times[len(self.rows)]:return
        scheduled=self.times[len(self.rows)]
        if elapsed>self.seconds-self.timeout-2:
            self.halted='capture_deadline_passed';return
        try:s=sdk.status()
        except (OSError,ValueError,KeyError):return
        if not s.get('fresh') or not s.get('video_snapshot_available') or s.get('composites_completed',0)==0:return
        session=(s.get('run_id'),s.get('module_generation'))
        if self.session is not None and self.session!=session:self.halted='session_changed';return
        self.session=session;started=time.monotonic()
        row={'scheduled_seconds':scheduled,'observed_submit_seconds':elapsed,'status_before':s}
        try:row['capture']=sdk.capture_frame(self.timeout)
        except (OSError,ValueError,KeyError,RuntimeError) as e:row['capture']={'result':'observer_error','reason':str(e)}
        row['observer_wall_seconds']=time.monotonic()-started
        try:row['status_after']=sdk.status()
        except (OSError,ValueError,KeyError) as e:row['status_after_error']=str(e)
        self.rows.append(row)
        # The SDK timeout does not cancel GPU work. Never overwrite an outstanding request.
        if row['capture'].get('result')!='captured':self.halted='capture_not_completed; no further requests'
        elif (row.get('status_after',{}).get('run_id'),row.get('status_after',{}).get('module_generation'))!=session:self.halted='session_changed_after_capture'
    def result(self):
        return {'schema':1,'source':'independent offscreen recomposition from same framebuffer inputs; not presented swapchain pixels',
                'clock_basis':'monotonic elapsed since lab submitted supervisor; not game load completion',
                'requested_times':self.times,'timeout_seconds':self.timeout,'max_captures':3,'max_image_bytes':12*1024*1024,
                'captures':self.rows,'unobserved_times':self.times[len(self.rows):],'halted':self.halted,
                'GPU_finish_perturbs_timing':True,'timeout_does_not_cancel_GPU':True}
