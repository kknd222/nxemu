"""Reset-only samples zero publication regression; actual GPU, bounded worker."""
from pathlib import Path
import argparse, hashlib, json, subprocess, sys

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--query-source', type=Path)
    a=ap.parse_args()
    r=Path(__file__).resolve().parent
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    harness=r/'samples_zero_backend_harness.cpp'
    inputs=[Path(__file__).resolve(),harness,r/'test_samples_backend.py']
    hashes={str(p):sha(p) for p in inputs}
    command=[sys.executable,'-B',str(r/'test_samples_backend.py'),'--out',str(out/'backend'),'--harness-source',str(harness)]
    if a.query_source:command+=['--query-source',str(a.query_source.resolve(strict=True))]
    run=subprocess.run(command,capture_output=True,text=True,timeout=270)
    (out/'wrapper.log').write_text(run.stdout+run.stderr)
    backend_path=out/'backend/RESULT.json'
    backend=json.loads(backend_path.read_text()) if backend_path.exists() else {}
    stdout=backend.get('run',{}).get('literal_stdout','')
    required=['SAMPLES_ZERO_BACKEND_PASS cases=14','ZERO_FILTER_PASS flag=64','ZERO_FILTER_PASS flag=32','ZERO_FILTER_PASS flag=128','ZERO_FREE_REUSE_PASS same_id=True','ZERO_RESET_PRESERVES_PENDING_PASS','ZERO_MIXED_DRAW_PASS zero=0 draw=256','ZERO_HISTORY_GATE_PASS amend=256 value_plus_amend=256 actualStreamerCPUamend=True fullCPUcompletion=False']
    unchanged=all(sha(Path(p))==h for p,h in hashes.items())
    passed=run.returncode==0 and backend.get('passed',False) and all(x in stdout for x in required) and unchanged
    report={'command':subprocess.list2cmdline(command),'input':'construct/reset bankless 4/8 widths; neighbors; invalidated/rewritten/orphan filters; Free ID reuse; repeated Sync; Reset pending retention; mixed zero/draw; real 256/512 GPU queries; historical GPU Draw256 accumulated gate with actual streamer amend callback','scope':'Actual SamplesStreamer/Scheduler/Vulkan GPU copies to guest backing. Historical accumulation uses actual streamer replication and amend callbacks, with no caller injected accumulation. No complete QueryCacheRuntime CPU completion or consumer chain claimed.','literal_stdout':run.stdout,'literal_stderr':run.stderr,'child_exit_status':run.returncode,'backend_result':str(backend_path),'required_literals':required,'source_hashes':hashes,'inputs_unchanged':unchanged,'passed':passed,'exit_status':0 if passed else 1}
    (out/'RESULT.json').write_text(json.dumps(report,indent=2))
    print('SAMPLES_ZERO_BACKEND_TEST_PASS' if passed else 'SAMPLES_ZERO_BACKEND_TEST_FAILED')
    return report['exit_status']
if __name__=='__main__':raise SystemExit(main())
