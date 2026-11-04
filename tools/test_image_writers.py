"""Compile the actual bounded image-writer registry and validate serialized evidence.

Synthetic handles and CPU command events only. No renderer, shader or GPU execution.
The independent Python reader checks files produced by the real C++ registry.
"""
from pathlib import Path
import argparse
import hashlib
import json
import copy
import os
import subprocess
import sys
import time
import traceback

CPP = r'''
#include "yuzu_video_core/image_writer_trace.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <algorithm>
using namespace VideoCommon::ImageWriters;
namespace fs=std::filesystem;
void save(const fs::path& root,const char* name,const Snapshot& x){
 const auto text=Serialize(x);assert(text.size()<=MaxBytes);std::ofstream f(root/name,std::ios::binary);assert(f);f<<text;assert(f);
}
size_t input_bucket(uint64_t image){return size_t((image>>4)^(image>>19)^image)%Slots;}
std::vector<uint64_t> collisions(size_t bucket,size_t count){
 std::vector<uint64_t> out;for(uint64_t h=1;out.size()<count;++h)if(input_bucket(h)==bucket)out.push_back(h);return out;
}
int main(int argc,char** argv){
 if(argc==3&&std::string(argv[1])=="--global"){
  const bool expected=std::string(argv[2])=="on";assert(Enabled()==expected);
  SetEnvironmentVariableA("NXEMU_IMAGE_WRITERS",expected?"0":"1");assert(Enabled()==expected);
  const auto birth=Birth(0x1230,1,2,0,2,2);auto event=Stamp(0x1230,Op::DrawRecorded);
  assert(bool(birth)==expected&&event.valid==expected&&Record(event)==expected&&Read(0x1230).available==expected);
  std::cout<<"GLOBAL_STARTUP_PASS; enabled="<<expected<<"; cached_after_env_change=True\n";return 0;
 }
 assert(argc==2);fs::path root=argv[1];fs::create_directories(root);
 static_assert(noexcept(std::declval<Registry&>().Birth(1,2,3,0,4,5)));
 static_assert(noexcept(std::declval<Registry&>().Stamp(1,Op::DrawRecorded)));
 static_assert(noexcept(std::declval<Registry&>().Record(Event{})));
 static_assert(noexcept(std::declval<Registry&>().Read(1)));
 {
  auto r=std::make_unique<Registry>(false);
  for(unsigned n=0;n<10000;++n){assert(!r->Birth(n+1,2,3,0,4,5));auto e=r->Stamp(n+1,Op::DrawRecorded);assert(!e.valid&&!r->Record(e));}
  auto x=r->Read(99);assert(!x.available&&x.count==0&&std::string(x.reason)=="disabled"&&x.serial==0);save(root,"disabled.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);assert(!r->Birth(0,0,0,0,0,0));
  auto birth=r->Birth(0x1000,0x2000,0x3000,0,1280,720);assert(birth);
  Event invalid;assert(!r->Record(invalid));assert(!r->Stamp(0x7777,Op::DrawRecorded).valid);
  auto e=r->Stamp(0x1000,Op::DrawRecorded);assert(e.valid&&e.dst_birth==birth&&!e.src_birth);
  e.shader_key=0xabcdef;e.hashes={1,2,3,4,5,6};e.aux={7,8,9,10};assert(r->Record(e));
  auto x=r->Read(0x1000);assert(x.available&&x.count==1&&x.nodes[0].birth==birth&&x.nodes[0].total==1&&x.missing_drops==1);
  assert(x.nodes[0].cpu_owner==0x2000&&x.nodes[0].gpu_owner==0x3000&&x.nodes[0].width==1280);save(root,"one_draw.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);auto old=r->Birth(0x1100,1,2,0,2,2);auto e=r->Stamp(0x1100,Op::DrawRecorded);
  auto fresh=r->Birth(0x1100,3,4,0,4,4);assert(fresh>old&&!r->Record(e));
  auto x=r->Read(0x1100);assert(x.available&&x.stale_drops==1&&x.nodes[0].birth==fresh&&x.nodes[0].count==0&&x.nodes[0].cpu_owner==3);save(root,"stale_destination.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);auto source=r->Birth(0x1200,1,2,0,2,2);r->Birth(0x1300,3,4,0,2,2);
  auto e=r->Stamp(0x1300,Op::CopyRecorded,0x1200);assert(e.src_birth==source);
  assert(r->Birth(0x1200,5,6,0,2,2)>source);assert(r->Record(e));
  auto x=r->Read(0x1300);assert(x.available&&x.count==1&&std::string(x.reason)=="parent_birth_changed");save(root,"stale_source.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);r->Birth(0x1400,1,2,0x9999,2,2);
  auto x=r->Read(0x1400);assert(x.available&&x.count==1&&std::string(x.reason)=="parent_birth_unknown");save(root,"unknown_parent.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);r->Birth(0x1500,1,2,0,2,2);
  for(unsigned n=0;n<25;++n){auto e=r->Stamp(0x1500,static_cast<Op>(n%11));e.aux[0]=n;assert(r->Record(e));}
  auto x=r->Read(0x1500);assert(x.available&&x.nodes[0].count==History&&x.nodes[0].total==25);save(root,"ring.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);r->Birth(0x1600,1,2,0,2,2);r->Birth(0x1700,1,2,0,2,2);
  r->Birth(0x1800,1,2,0x1600,2,2);auto a=r->Stamp(0x1800,Op::CopyRecorded,0x1600);assert(r->Record(a));
  auto b=r->Stamp(0x1800,Op::CopyRecorded,0x1700);assert(r->Record(b));
  auto c=r->Stamp(0x1800,Op::DrawRecorded);assert(r->Record(c));
  auto x=r->Read(0x1800);assert(x.available&&x.count==2&&x.nodes[1].image==0x1700);save(root,"latest_copy_parent.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);r->Birth(0x2000,1,2,0,2,2);
  for(unsigned n=1;n<6;++n)r->Birth(0x2000+n*0x100,1,2,0x2000+(n-1)*0x100,2,2);
  auto x=r->Read(0x2500);assert(x.available&&x.count==MaxNodes&&std::string(x.reason)=="node_budget");save(root,"chain_budget.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);auto a=r->Birth(0x3000,1,2,0,2,2);auto b=r->Birth(0x3100,1,2,0,2,2);
  assert(r->Record(r->Stamp(0x3000,Op::CopyRecorded,0x3100)));assert(r->Record(r->Stamp(0x3100,Op::CopyRecorded,0x3000)));
  auto x=r->Read(0x3000);assert(x.available&&x.count==2&&std::string(x.reason)=="parent_cycle"&&a&&b);save(root,"cycle.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);const auto handles=collisions(0,ProbeSlots+1);
  for(size_t n=0;n<ProbeSlots;++n)assert(r->Birth(handles[n],1,2,0,2,2));
  auto old_event=r->Stamp(handles[0],Op::DrawRecorded);assert(old_event.valid);
  assert(r->Birth(handles.back(),1,2,0,2,2));assert(!r->Read(handles[0]).available&&!r->Record(old_event));
  auto x=r->Read(handles.back());assert(x.available&&x.evictions==1&&x.stale_drops==1);save(root,"collision_eviction.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);const auto handles=collisions(1,ProbeSlots+1);
  r->Birth(handles[0],1,2,0,2,2);uint64_t child=9;while(input_bucket(child)==1||input_bucket(child)==2||input_bucket(child)==3||input_bucket(child)==4)++child;
  r->Birth(child,1,2,handles[0],2,2);
  for(size_t n=1;n<handles.size();++n)r->Birth(handles[n],1,2,0,2,2);
  auto x=r->Read(child);assert(x.available&&x.count==1&&std::string(x.reason)=="parent_evicted_or_untracked");save(root,"parent_evicted.json",x);
 }
 {
  auto r=std::make_unique<Registry>(true);r->Birth(0x4400,1,2,0,1280,720);
  auto e=r->Stamp(0x4400,Op::ClearRecorded);e.clear_region_known=true;
  e.clear_rect={4,8,640,360};e.clear_layers={2,3};e.aux={0,0,0,0};assert(r->Record(e));
  save(root,"clear_region.json",r->Read(0x4400));
 }
 {
  auto r=std::make_unique<Registry>(true);std::atomic<unsigned> ready{},go{},finished{},accepted{};std::vector<std::thread> threads;
  for(unsigned t=0;t<8;++t)threads.emplace_back([&,t]{++ready;while(!go.load())std::this_thread::yield();
    for(unsigned n=0;n<40000;++n){const uint64_t h=0x500000+t*0x1000+(n%16)*16;r->Birth(h,t,n,0,2,2);auto e=r->Stamp(h,Op::DrawRecorded);e.aux[0]=n;if(r->Record(e))++accepted;}++finished;});
  while(ready.load()!=8)std::this_thread::yield();go=1;for(auto& t:threads)t.join();assert(finished==8&&accepted>0);
  // A deterministic post-stress target gives a successful snapshot; contention drops are
  // measured over 320k actual concurrent operations, not assumed from one racing call.
  r->Birth(0x777770,1,2,0,2,2);auto x=r->Read(0x777770);assert(x.available&&x.lock_drops>0&&x.count<=MaxNodes);save(root,"concurrent.json",x);
 }
 {
  for(unsigned mode=0;mode<3;++mode){Snapshot x;if(mode==0)x.count=MaxNodes+1;
   else{x.count=1;if(mode==1)x.nodes[0].count=History+1;else x.nodes[0].next=History;}
   bool failed=false;try{auto text=Serialize(x);(void)text;}catch(const std::length_error&){failed=true;}assert(failed);}
  Snapshot x;x.reason="quote\"slash\\newline\n";save(root,"quoted_reason.json",x);
  std::string large(MaxBytes+1,'x');x.reason=large.c_str();bool failed=false;try{auto text=Serialize(x);(void)text;}catch(const std::length_error&){failed=true;}assert(failed);
 }
 // The production API cannot seed its private uint64 counter. Saturation logic is source
 // checked by Python; no false claim of 2^64 dynamic operations is made.
 std::cout<<"{\"passed\":true,\"actual_header\":true,\"fake_GPU_handles\":true,\"GPU_executed\":false,\"off_operations\":10000,\"threads\":8,\"stress_iterations\":320000,\"history\":"<<History<<",\"slots\":"<<Slots<<",\"probe_slots\":"<<ProbeSlots<<",\"chain_nodes\":"<<MaxNodes<<",\"saturation_dynamic\":false}\n";
}
'''

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_files(root):
    data={p.name:json.loads(p.read_text(encoding='utf-8')) for p in root.glob('*.json')}
    assert len(data)==14, list(data)
    assert data['disabled.json']['reason']=='disabled' and not data['disabled.json']['available']
    one=data['one_draw.json'];node=one['nodes'][0]
    assert one['available'] and node['cpu_owner']==0x2000 and node['gpu_owner']==0x3000
    event=node['events'][0]
    assert event['shader_key']==0xabcdef and event['hashes']==[1,2,3,4,5,6] and event['aux']==[7,8,9,10]
    assert event['dst_birth']==node['birth'] and event['seq']>node['birth']
    assert data['stale_destination.json']['stale_drops']==1 and not data['stale_destination.json']['nodes'][0]['events']
    assert data['stale_source.json']['reason']=='parent_birth_changed' and len(data['stale_source.json']['nodes'])==1
    ring=data['ring.json']['nodes'][0]
    assert ring['events_total']==25 and ring['events_retained']==8
    assert [e['aux'][0] for e in ring['events']]==list(range(17,25))
    seq=[e['seq'] for e in ring['events']];assert seq==sorted(set(seq))
    assert data['latest_copy_parent.json']['nodes'][1]['image']==0x1700
    assert len(data['chain_budget.json']['nodes'])==4 and data['chain_budget.json']['reason']=='node_budget'
    assert data['cycle.json']['reason']=='parent_cycle'
    assert data['collision_eviction.json']['evictions']==1
    assert data['parent_evicted.json']['reason']=='parent_evicted_or_untracked'
    assert data['concurrent.json']['lock_drops']>0
    clear=data['clear_region.json']['nodes'][0]['events'][0]
    assert clear['op']=='clear_recorded' and clear['clear_region_known'] is True
    assert clear['clear_rect']==[4,8,640,360] and clear['clear_layers']==[2,3]
    assert data['quoted_reason.json']['reason']=='quote"slash\\newline\n'
    for name,d in data.items():
        assert d['schema']==1 and len(d['nodes'])<=4
        for n in d['nodes']:
            assert n['events_retained']==len(n['events'])<=8
            assert all(e['dst']==n['image'] and e['dst_birth']==n['birth'] for e in n['events'])
        assert (root/name).stat().st_size<=65536
    return data


def parser_cases(root, out, native_data, sdk_source):
    """Call the real current SDK with synthetic publication and actual filesystem IO."""
    sys.path.insert(0,str(root/'tools'))
    from image_writers import inspect_writers
    source=sdk_source.resolve(strict=True)
    ns={'__name__':'image_writer_sdk_fixture'}
    exec(compile(source.read_text(encoding='utf-8'),str(source),'exec'),ns)
    Base=ns['NxEmuSDK'];api=hasattr(Base,'image_writers')
    mutations={
        'request_id':lambda d:d.update(request_id='0'*32),
        'schema_bool':lambda d:d.update(schema=True),
        'registry_schema_bool':lambda d:d['registry'].update(schema=True),
        'epoch_bool':lambda d:d.update(snapshot_epoch_ms=True),
        'epoch_negative':lambda d:d.update(snapshot_epoch_ms=-1),
        'finish_integer':lambda d:d.update(gpu_finish_boundary=1),
        'empty_run':lambda d:d.update(run_id=''),
        'serial_float':lambda d:d['registry'].update(serial=3.0),
        'counter_bool':lambda d:d['registry'].update(lock_drops=False),
        'counter_overflow':lambda d:d['registry'].update(evictions=1<<64),
        'availability':lambda d:d['registry'].update(available=False),
        'selected_mismatch':lambda d:d['registry'].update(selected_image=999),
        'node_nondict':lambda d:d['registry'].update(nodes=[1]),
        'too_many_nodes':lambda d:d['registry'].update(nodes=d['registry']['nodes']*5),
        'duplicate_node':lambda d:d['registry'].update(nodes=d['registry']['nodes']*2),
        'zero_birth':lambda d:d['registry']['nodes'][0].update(birth=0),
        'birth_after_serial':lambda d:d['registry']['nodes'][0].update(birth=d['registry']['serial']+1,events=[],events_retained=0),
        'parent_birth_after_serial':lambda d:d['registry']['nodes'][0].update(parent_birth=d['registry']['serial']+1),
        'event_count_bool':lambda d:d['registry']['nodes'][0].update(events_retained=True),
        'event_count_wrong':lambda d:d['registry']['nodes'][0].update(events_retained=0),
        'event_total_low':lambda d:d['registry']['nodes'][0].update(events_total=0),
        'hash_length':lambda d:d['registry']['nodes'][0]['events'][0].update(hashes=[0]*5),
        'hash_bool':lambda d:d['registry']['nodes'][0]['events'][0].update(hashes=[False]*6),
        'aux_length':lambda d:d['registry']['nodes'][0]['events'][0].update(aux=[0]*3),
        'aux_negative':lambda d:d['registry']['nodes'][0]['events'][0].update(aux=[-1]*4),
        'wrong_event_birth':lambda d:d['registry']['nodes'][0]['events'][0].update(dst_birth=777),
        'wrong_event_dst':lambda d:d['registry']['nodes'][0]['events'][0].update(dst=777),
        'seq_zero':lambda d:d['registry']['nodes'][0]['events'][0].update(seq=0),
        'seq_at_birth':lambda d:d['registry']['nodes'][0]['events'][0].update(seq=d['registry']['nodes'][0]['birth']),
        'seq_after_serial':lambda d:d['registry']['nodes'][0]['events'][0].update(seq=d['registry']['serial']+1),
        'unknown_op':lambda d:d['registry']['nodes'][0]['events'][0].update(op='future_unknown'),
        'src_birth_after_serial':lambda d:d['registry']['nodes'][0]['events'][0].update(src_birth=d['registry']['serial']+1),
        'out_of_order':lambda d:d['registry']['nodes'][0].update(events=d['registry']['nodes'][0]['events']*2,events_retained=2,events_total=2),
        'clear_known_integer':lambda d:d['registry']['nodes'][0]['events'][0].update(clear_region_known=1),
        'clear_rect_length':lambda d:d['registry']['nodes'][0]['events'][0].update(clear_rect=[0]*3),
        'clear_rect_negative':lambda d:d['registry']['nodes'][0]['events'][0].update(clear_rect=[-1]*4),
        'clear_layers_bool':lambda d:d['registry']['nodes'][0]['events'][0].update(clear_layers=[True,False]),
    }
    scenarios=['baseline'] if not api else ['missing','valid','valid_clear','legacy_fields_missing','empty_registry','session_mismatch','generation_mismatch','invalid_json','over_budget',*mutations]
    rows=[];payload=b'P6\n1280 720\n255\n'+b'\x01'*1280*720*3
    status={'fresh':True,'video_snapshot_available':True,'diagnostic_config':{'frame_capture':True},'run_id':'run','module_generation':3}
    for scenario in scenarios:
        directory=out/scenario;directory.mkdir(parents=True,exist_ok=True)
        class Fake(Base):
            def status(self):
                request=directory/'diagnostics/frame-capture-request.json'
                if request.exists():
                    req=json.loads(request.read_text());identifier=req['id'];q=request.parent
                    (q/('frame-capture-'+identifier+'.ppm')).write_bytes(payload)
                    m={'request_id':identifier,'run_id':'run','generation':'3','status':'completed','gpu_readback_completed':True}
                    (q/('frame-capture-'+identifier+'.json')).write_text(json.dumps(m))
                    if scenario!='missing' and scenario!='baseline':
                        d={'schema':1,'request_id':identifier,'run_id':'run','generation':'3','composite_ordinal':5,
                           'snapshot_epoch_ms':int(time.time()*1000),'gpu_finish_boundary':True,'registry':copy.deepcopy(native_data['one_draw.json'])}
                        if scenario=='empty_registry':d['registry']=copy.deepcopy(native_data['disabled.json'])
                        elif scenario=='valid_clear':d['registry']=copy.deepcopy(native_data['clear_region.json'])
                        elif scenario=='legacy_fields_missing':
                            for event in d['registry']['nodes'][0]['events']:
                                for key in ['clear_region_known','clear_rect','clear_layers']:event.pop(key,None)
                        elif scenario=='session_mismatch':d['run_id']='other'
                        elif scenario=='generation_mismatch':d['generation']='4'
                        elif scenario in mutations:mutations[scenario](d)
                        raw='bad JSON' if scenario=='invalid_json' else ' '*65537 if scenario=='over_budget' else json.dumps(d)
                        (q/('frame-capture-'+identifier+'.writers.json')).write_text(raw)
                return dict(status)
        r=Fake(directory).capture_frame(2)
        assert r['result']=='captured' and not r['all_rgb_zero'] and r['sha256']==hashlib.sha256(payload).hexdigest(),scenario
        writer=r.get('image_writers')
        if scenario=='baseline':assert writer is None
        elif scenario=='missing':assert writer['available'] is False
        elif scenario in ('valid','valid_clear','legacy_fields_missing','empty_registry','session_mismatch','generation_mismatch'):
            assert writer['available'] is True
            assert writer['registry_available']==(scenario!='empty_registry')
            assert writer['matches_current_session']==(scenario not in ('session_mismatch','generation_mismatch'))
        else:assert writer['available'] is False and writer['reason']=='writers_invalid',scenario
        rows.append({'scenario':scenario,'pixel_sha256':r['sha256'],'writers':writer})
        (directory/'SDK_RESULT.json').write_text(json.dumps(r,indent=2),encoding='utf-8')
    # Validate request parser before any file lookup. Actual files use fixed basenames.
    for identifier in ['','../bad','A'*32,'0'*31,'0'*33]:
        try:inspect_writers(out,identifier,status)
        except ValueError:pass
        else:raise AssertionError('Invalid id accepted')
    identifier='b'*32;directory=out/'path-tests';q=directory/'diagnostics';q.mkdir(parents=True,exist_ok=True)
    valid={'schema':1,'request_id':identifier,'run_id':'run','generation':'3','composite_ordinal':5,
           'snapshot_epoch_ms':int(time.time()*1000),'gpu_finish_boundary':True,'registry':copy.deepcopy(native_data['one_draw.json'])}
    target=directory/'regular.json';target.write_text(json.dumps(valid))
    link=q/('frame-capture-'+identifier+'.writers.json');symlink={'tested':False}
    try:link.symlink_to(target)
    except OSError as error:symlink.update(skipped=True,reason=str(error))
    else:
        try:inspect_writers(directory,identifier,status)
        except ValueError:symlink.update(tested=True,rejected=True,skipped=False)
        else:raise AssertionError('Symlink writer metadata accepted')
    record={'actual_SDK_method':True,'fake_GPU_publisher':True,'real_filesystem':True,'GPU_executed':False,
            'source_sha256':sha(source),'baseline_writer_api_absent':not api,'scenarios':rows,'symlink':symlink}
    (out/'RESULT.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    return record


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--sdk-source',type=Path)
    args=ap.parse_args();root=args.source_root.resolve(strict=True)
    if (root/'repo/src').is_dir():root=root/'repo'
    header=root/'src/yuzu_video_core/image_writer_trace.h';assert header.is_file()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    files=out/'files';assert not files.exists(),'Use a new output directory'
    cpp=out/'image-writer-test.cpp';cpp.write_text(CPP,encoding='utf-8')
    exe=out/'image-writer-test.exe';obj=out/'image-writer-test.obj'
    compiler=['cl','/nologo','/std:c++20','/EHsc','/MT','/O2','/utf-8','/I'+str(root/'src'),str(cpp),'/Fe:'+str(exe),'/Fo:'+str(obj)]
    cmd=out/'build.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(compiler)+'\n',encoding='utf-8')
    bound=[header,root/'src/yuzu_common/debug_document.h',cpp,Path(__file__).resolve()]
    if args.sdk_source:bound.extend([args.sdk_source.resolve(strict=True),root/'tools/image_writers.py'])
    hashes={str(p):sha(p) for p in bound}
    report={'command':subprocess.list2cmdline([sys.executable,'-B',Path(__file__).resolve(),*sys.argv[1:]]),
            'compiler_command':subprocess.list2cmdline(compiler),'input':'actual registry header; synthetic handles/CPU events; actual threads and filesystem; no renderer/GPU',
            'hashes':hashes}
    try:
        b=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=90)
        (out/'build.log').write_text(b.stdout+b.stderr,encoding='utf-8')
        report.update(stage='compile',literal_stdout=b.stdout,literal_stderr=b.stderr,exit_status=b.returncode)
        if b.returncode:raise RuntimeError('Actual header compile failed')
        native=[str(exe),str(files)];r=subprocess.run(native,capture_output=True,text=True,timeout=45)
        (out/'run.log').write_text(r.stdout+r.stderr,encoding='utf-8')
        report.update(stage='run',native_command=subprocess.list2cmdline(native),literal_stdout=r.stdout,literal_stderr=r.stderr,
                      exit_status=r.returncode,executable_sha256=sha(exe))
        if r.returncode:raise RuntimeError('Actual registry test failed')
        data=validate_files(files)
        globals=[]
        for mode in ['off','on']:
            env=dict(os.environ)
            if mode=='on':env['NXEMU_IMAGE_WRITERS']='1'
            else:env.pop('NXEMU_IMAGE_WRITERS',None)
            g=subprocess.run([str(exe),'--global',mode],capture_output=True,text=True,env=env,timeout=15)
            assert g.returncode==0,(g.stdout,g.stderr)
            globals.append({'mode':mode,'command':subprocess.list2cmdline([str(exe),'--global',mode]),'literal_stdout':g.stdout,'literal_stderr':g.stderr,'exit_status':g.returncode})
        text=header.read_text(encoding='utf-8')
        assert 'serial==std::numeric_limits<uint64_t>::max()' in text
        assert all(sha(Path(p))==h for p,h in hashes.items()),'Input changed during test'
        report.update(stage='complete',result=json.loads(r.stdout),serialization_validated=True,publication_count=len(data),
                      published_files={str(p):sha(p) for p in files.glob('*.json')},inputs_unchanged=True,
                      saturation_source_contract_only=True)
        report['startup_switch_tests']=globals
        if args.sdk_source:report['sdk']=parser_cases(root,out/'sdk',data,args.sdk_source)
        assert all(sha(Path(p))==h for p,h in hashes.items()),'Bound source changed during SDK test'
        (out/'RESULT.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print('IMAGE_WRITERS_PASS; actual_header=True; fake_GPU=True; threads8=True; saturation_dynamic=False; serialization=True')
    except Exception as error:
        report.update(error=str(error),python_traceback=traceback.format_exc(),native_exit_status=report.get('exit_status'),
                      exit_status=report.get('exit_status') or 1)
        (out/'FAILURE.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        raise


if __name__=='__main__':main()
