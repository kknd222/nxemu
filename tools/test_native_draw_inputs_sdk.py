"""Real reader/SDK + actual C++ fixture files; no GPU execution."""
from pathlib import Path
import argparse,copy,hashlib,json,os,shutil,subprocess,sys,tempfile,time
from native_draw_inputs import read_case,inspect_inputs,decode_clip_encoding
from debug_sdk import NxEmuSDK
from debug_launch import diagnostic_arguments

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--cpp-output',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=False)
    status=dict(schema=1,epoch_ms=round(time.time()*1000),pid=os.getpid(),exited=False,run_id='111',module_generation=1)
    cases=[];cpp=[]
    report=json.loads((a.cpp_output/'RESULT.json').read_text())
    for mode in [r['input']['mode'] for r in report['results']]:
        directory=a.cpp_output/mode/'native-draw-inputs'
        for f in directory.glob('draw-input-*.json'):
            x=read_case(f,status);assert x['available'];cpp.append(dict(path=str(f),sha256=hashlib.sha256(f.read_bytes()).hexdigest()))
    source=next((a.cpp_output/'valid/native-draw-inputs').glob('*.json'));original=json.loads(source.read_text())
    with tempfile.TemporaryDirectory(dir=a.out) as t:
        root=Path(t);directory=root/'diagnostics/native-draw-inputs';directory.mkdir(parents=True)
        for f in source.parent.iterdir():
            if f.name.startswith(original['id']+'.'):shutil.copy2(f,directory/f.name)
        f=directory/source.name;(root/'status.json').write_text(json.dumps(status));sdk=NxEmuSDK(root)
        assert sdk.native_draw_inputs()['available'] and sdk.native_draw_inputs()['live_state']['fresh'];cases.append('actual_SDK_files')
        (root/'status.json').write_text(json.dumps(dict(status,exited=True)))
        stale=sdk.native_draw_inputs();assert stale['available'] and not stale['live_state']['fresh'];cases.append('historical_not_live')
        (root/'status.json').write_text(json.dumps(status))
        x=read_case(f,dict(status,run_id='other'));assert not x['matches_current_session'];cases.append('historical_session')
        mutations=[('schema_bool',lambda x:x.update(schema=True)),('zero_token',lambda x:x.update(token=0)),
          ('stage',lambda x:x.update(stage=5)),('partial_header_size',lambda x:x.update(header_size=80)),('session_bool',lambda x:x.update(generation=True)),
          ('offset',lambda x:x.update(error_offset=1)),('opcode',lambda x:x.update(opcode=0)),('word_bool',lambda x:x.update(words=[True,0,0,0])),
          ('stage_bool',lambda x:x.update(stage=True)),('file_roles',lambda x:x['files'].pop('regs')),
          ('file_path',lambda x:x['files']['native'].update(file='../evil.bin')),('byte_bool',lambda x:x['files']['native'].update(bytes=True)),
          ('hash',lambda x:x['files']['native'].update(fnv1a64='0'*16)),('native_bound',lambda x:x['files']['native'].update(bytes=4097)),
          ('header_flag',lambda x:x.update(header_fully_mapped=1)),('parsed_header_match',lambda x:x.update(parsed_header_matches_raw=False)),
          ('cbuf_size',lambda x:x['cbuf'].update(declared_size=0)),('cbuf_flag',lambda x:x['cbuf'].update(fully_mapped=1)),
          ('cbuf_bank',lambda x:x['cbuf'].update(bank=18)),('cbuf_unavailable_filled',lambda x:x['cbuf'].update(fully_mapped=False)),
          ('reason_bound',lambda x:x.update(reason='x'*193)),('filename',lambda x:x.update(id='other'))]
        for name,mutate in mutations:
            d=copy.deepcopy(original);mutate(d);f.write_text(json.dumps(d))
            try:read_case(f,status)
            except (ValueError,TypeError,KeyError):cases.append(name)
            else:raise AssertionError(name)
            bad=sdk.native_draw_inputs();assert not bad['available'] and bad['errors']
        f.write_text(json.dumps(original));raw=directory/original['files']['native']['file'];b=raw.read_bytes();raw.write_bytes(b+b'x')
        assert not sdk.native_draw_inputs()['available'];raw.write_bytes(b);cases.append('changed_file')
        (directory/('draw-input-111-1-999-0.json')).write_text('{}');x=sdk.native_draw_inputs();assert x['available'] and x['errors'];cases.append('bad_optional_preserves_good')
        # Actual CLI reader reaches SDK without any emulator/game actions.
        p=subprocess.run([sys.executable,'-B',Path(__file__).with_name('debug_sdk.py'),'--bridge',root,'native-draw-inputs'],capture_output=True,text=True)
        assert p.returncode==0 and json.loads(p.stdout)['available'];cases.append('actual_cli')
    options={'native-draw-inputs':1,'shader-fixtures':1,'image-writers':1}
    assert '--native-draw-inputs' in diagnostic_arguments(options);cases.append('startup_options')
    for d in [{'native-draw-inputs':1},{'native-draw-inputs':1,'shader-fixtures':1}]:
        try:diagnostic_arguments(d)
        except ValueError:cases.append('missing_prerequisite')
        else:raise AssertionError(d)
    import struct
    two=struct.pack('<8I',0x780d,0xc4321111,0x03fe0000,0xfc800,0x780d,0x80000000,0x03f06000,0xfda00)
    decoded=decode_clip_encoding(two)['csmtest']
    assert decoded[0]['vote_index_28']==0x04321111 and decoded[0]['vtgmode_name']=='RA' and decoded[0]['pu']==7
    assert decoded[1]['vote_index_28']==0 and decoded[1]['vtgmode_name']=='A' and decoded[1]['ccmp_name']=='FCSM_TR' and decoded[1]['pu']==0
    cases.append('actual_CSM_encoding_two')
    # Do not confuse ALD's RZ address register with its ordinary R0 destination.
    ald=decode_clip_encoding(struct.pack('<4I',0xff007321,0x0002fcff,0,0xe2200))['attributes'][0]
    assert ald['value_register']==0 and ald['address_register']==255 and ald['byte_address']==0x2fc;cases.append('ALD_R0_not_RZ')
    text=Path(__file__).parents[1].joinpath('src/yuzu_video_core/shader_environment.cpp').read_text()
    constructor=text[text.index('GraphicsEnvironment::GraphicsEnvironment('):text.index('GraphicsEnvironment::CaptureNativeDrawInput(')]
    early=constructor.index('stage = program == Maxwell::ShaderType::VertexB')
    assert early<constructor.index('InitializeNativeCode();');cases.append('stage_identity_before_throw_source_contract')
    paths=[Path(__file__),Path(__file__).with_name('native_draw_inputs.py'),Path(__file__).with_name('debug_sdk.py'),Path(__file__).with_name('debug_launch.py')]
    result=dict(passed=True,command=subprocess.list2cmdline([sys.executable,'-B',Path(__file__),*sys.argv[1:]]),input='actual CPP published files, real SDK/CLI/typed reader; CPU-only fixtures',exit_status=0,
                cases=cases,cpp_files=cpp,hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},GPU_executed=False,stage_runtime_injection=False)
    output='NATIVE_INPUT_SDK_PASS; cases='+str(len(cases))+'; CPP_files='+str(len(cpp))+'; GPU=False';result['literal_stdout']=output+'\n';result['literal_stderr']=''
    (a.out/'RESULT.json').write_text(json.dumps(result,indent=2));print(output)
if __name__=='__main__':main()
