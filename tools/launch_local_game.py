"""Verified local loose-game launcher. Starts GUI through Cua; never injects game input."""
from pathlib import Path
import argparse, datetime, json, subprocess, sys
import psutil
import rom_set_audit
REPO=Path(__file__).resolve().parents[1]
EXE=REPO/'bin/x64/Release/nxemu.exe'
GAMES={
    'pokopia': Path(r'E:\download\SW2-dumpster\pokopia\updates\655360\exefs\main'),
    'kirby': Path(r'E:\download\SW2-dumpster\KirbyAirRiders_GlobalTestRide\updates\65536\exefs\main'),
}
def main():
    p=argparse.ArgumentParser();p.add_argument('game',choices=GAMES);p.add_argument('--check',action='store_true');p.add_argument('--audit-output',type=Path);p.add_argument('--manifest',type=Path);p.add_argument('--manifest-root',type=Path);a=p.parse_args()
    game=GAMES[a.game];romfs=game.parent.parent/'romfs'
    if not EXE.is_file(): print('FAIL: NXEmu executable missing');return 2
    for name in ('main','main.npdm','rtld','sdk'):
        file=game.parent/name
        if not file.is_file() or not file.stat().st_size:print(f'FAIL: missing ExeFS {name}');return 2
    with game.open('rb') as f: main_magic=f.read(4)
    with (game.parent/'main.npdm').open('rb') as f: npdm_magic=f.read(4)
    if main_magic!=b'NSO0' or npdm_magic!=b'META':
        print('FAIL: unexpected main/NPDM header');return 2
    manifest=a.manifest or (rom_set_audit.DEFAULT_MANIFEST if a.game=='kirby' else None)
    manifest_root=a.manifest_root or (rom_set_audit.DEFAULT_ROOT if a.game=='kirby' else None)
    try: audit=rom_set_audit.audit(game,manifest,manifest_root)
    except (ValueError,KeyError,TypeError,OSError,json.JSONDecodeError) as error:
        print('ROM_SET_PREFLIGHT_FAIL; '+str(error));return 2
    if a.audit_output:
        a.audit_output.parent.mkdir(parents=True,exist_ok=True)
        a.audit_output.write_text(json.dumps(audit,indent=2,ensure_ascii=False),encoding='utf-8')
    print('ROM_SET_PREFLIGHT; STATUS='+audit['status']+'; KIND='+audit['romfs_kind']+'; RUNTIME_VERIFIED=False; HASHED_BYTES='+str(audit['hashed_bytes']))
    if audit['status']=='FAIL': print('ROM_SET_PREFLIGHT_FAIL; '+json.dumps(audit['issues']));return 2
    print('PREFLIGHT_PASS; GAME='+a.game+'; MAIN='+str(game)+'; ROMFS='+str(romfs))
    if a.check:return 0
    existing=[x.info['pid'] for x in psutil.process_iter(['name','pid']) if (x.info['name'] or '').lower()=='nxemu.exe']
    if existing:print('EXISTING_NXEMU; close before loading another game; pid='+str(existing));return 3
    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    bridge=REPO.parent/'debug-sessions'/f'{a.game}-{stamp}'/'bridge'
    bridge.mkdir(parents=True,exist_ok=False)
    args=['--debug-bridge',str(bridge),'--software-keyboard-name','kknd222','--load',str(game)]
    proc=subprocess.run(['cua-driver','call','launch_app'],input=json.dumps({'path':str(EXE),'additional_arguments':args}),capture_output=True,text=True,timeout=35)
    (bridge.parent/'rom-set-preflight.json').write_text(json.dumps(audit,indent=2,ensure_ascii=False),encoding='utf-8')
    (bridge.parent/'launch-transport.json').write_text(proc.stdout,encoding='utf-8')
    if proc.returncode:print(proc.stderr);return proc.returncode
    result=json.loads(proc.stdout)
    if result.get('isError') or not result.get('pid'):print(proc.stdout);return 4
    print('LAUNCHED; PID='+str(result['pid'])+'; BRIDGE='+str(bridge))
    print('Launch acceptance is not gameplay validation; log: '+str(EXE.parent/'user/log/nxemu_log.txt'))
    return 0
if __name__=='__main__':sys.exit(main())
