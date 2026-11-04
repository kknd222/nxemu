"""Bounded read-only guest fatal-event extraction from a specified run log."""
from pathlib import Path
import re,hashlib,json
LIMIT=8*1024*1024
MARKERS=('Userspace PANIC!','debug_buffer_err_code=','Emulated program broke execution!','Switch 2 SVC error result=','SetTerminateResult:')

HALT_BITS={0:'Step',1:'CacheInvalidation',2:'MemoryAbort',24:'UserDefined1',25:'UserDefined2',26:'UserDefined3',27:'UserDefined4',28:'UserDefined5',29:'UserDefined6',30:'UserDefined7',31:'UserDefined8'}
HALT_DEFINED_MASK=sum(1<<b for b in HALT_BITS)

def parse_halt_line(line):
 fields=dict(re.findall(r'(\w+)=("(?:\\.|[^"\\])*"|\S+)',line))
 def value(k):
  v=fields[k]
  return json.loads(v) if v.startswith('"') else v
 def number(k):return int(value(k),0)
 raw=number('raw');valid=number('context_valid')==1;kind=value('raw_kind');direction=value('direction')
 if not 0<=raw<=0xffffffff or kind not in ('bitmask','enum') or direction not in ('dynarmic_to_cpu','cpu_to_dynarmic'):raise ValueError('Invalid halt domain')
 unknown=raw&~HALT_DEFINED_MASK if kind=='bitmask' else None
 return dict(schema=1,raw=raw,raw_kind=kind,direction=direction,mapped=number('mapped'),classification=value('classification'),
  defined_bits=[n for b,n in HALT_BITS.items() if raw&(1<<b)] if kind=='bitmask' else [],unknown_bits=unknown,
  metadata_consistent=(unknown==number('unknown_bits')) if kind=='bitmask' else True,
  core=number('core') if number('core')!=0xffffffff else None,architecture=number('arch') or None,context_valid=valid,
  pc=number('pc') if valid else None,sp=number('sp') if valid else None,lr=number('lr') if valid else None,pstate=number('pstate') if valid else None,
  gpr_x0_x7=[number('x'+str(i)) for i in range(8)] if valid else None,operation=value('operation'),ordinal=number('ordinal'),
  limit=number('limit'),budget_scope=value('budget_scope'),epoch_ms=number('epoch_ms'),run_id=value('run_id'),module_generation=value('generation'),
  source='actual halt return/conversion boundary; report-time environment tags, not active CPU state',line=line[:2048])

def parse_text(text):
 rows=[];total=0;first_panic=None;first_svc_error=None;cpu_halts=[];halt_total=0;halt_malformed=0;legacy_halt_assertions=0
 for line in text.splitlines():
  if 'arm_dynarmic.cpp' in line and 'Assertion Failed' in line:legacy_halt_assertions+=1
  if 'NXEMU_CPU_HALT_MAP ' in line:
   halt_total+=1
   try:cpu_halts.append(parse_halt_line(line))
   except (KeyError,ValueError,TypeError,json.JSONDecodeError):halt_malformed+=1
   if len(cpu_halts)>64:cpu_halts.pop(0)
   continue
  if not any(x in line for x in MARKERS):continue
  total+=1;stamp=re.search(r'^\[\s*([0-9.]+)\]',line)
  if 'Userspace PANIC!' in line:kind='guest_panic'
  elif 'debug_buffer_err_code=' in line:kind='panic_payload_result'
  elif 'broke execution!' in line:kind='guest_break'
  elif 'Switch 2 SVC error result=' in line:kind='svc_error'
  else:kind='terminate_result'
  m=re.search(r'(?:result=0x|debug_buffer_err_code=)([0-9A-Fa-f]+)',line)
  code=int(m.group(1),16) if m else None
  svc=re.search(r'svc=0x([0-9a-f]+) name=(\w+)',line)
  row=dict(kind=kind,guest_log_seconds=float(stamp.group(1)) if stamp else None,result=code,
           module=(code&511) if code is not None else None,description=(code>>9) if code is not None else None,
           svc_id=int(svc.group(1),16) if svc else None,svc_name=svc.group(2) if svc else None,line=line[:2048])
  if code==0xe401:row['kernel_result_name']='ResultInvalidHandle'
  if code==0xf601:row['kernel_result_name']='ResultSessionClosed'
  if kind=='guest_panic' and first_panic is None:first_panic=row
  if kind=='svc_error' and first_svc_error is None:first_svc_error=row
  rows.append(row)
  if len(rows)>64:rows.pop(0)
 return dict(cpu_halts=cpu_halts,cpu_halt_total_matching=halt_total,cpu_halt_malformed=halt_malformed,cpu_halt_overwritten=max(0,halt_total-halt_malformed-64),cpu_halt_legacy_assertions=legacy_halt_assertions,halt_budget_may_be_exhausted=any(x["ordinal"]==x["limit"] for x in cpu_halts),events=rows,total_matching=total,events_overwritten=max(0,total-64),guest_panic_observed=first_panic is not None,
             first_panic=first_panic,first_svc_error=first_svc_error,
             warning='A normal host process exit or loading heartbeat does not imply healthy guest execution. Error proximity is not causality.')
def guest_faults(log):
 p=Path(log).resolve(strict=True);size=p.stat().st_size
 with p.open('rb') as f:
  first=f.read(LIMIT//2)
  if size>LIMIT//2:f.seek(max(LIMIT//2,size-LIMIT//2));tail=f.read(LIMIT//2)
  else:tail=b''
 raw=first+tail
 # Noncontiguous budget slices must never manufacture a complete record across the omitted middle.
 text=(first.decode('utf-8',errors='replace')+'\n[LOG_SAMPLE_GAP]\n'+tail.decode('utf-8',errors='replace')) if size>LIMIT else raw.decode('utf-8',errors='replace')
 d=parse_text(text)
 d.update(schema=1,log=str(p),log_bytes=size,bytes_read=len(raw),bytes_omitted=max(0,size-len(raw)),complete_log_scan=size<=LIMIT,
          sampled_sha256=hashlib.sha256(raw).hexdigest(),scope='specified log artifact history, not live guest state; no current-session assertion',
          first_and_last_budget_bytes=LIMIT)
 return d
