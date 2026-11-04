"""Bounded, read-only loose ROM set preflight. Not proof of runtime file selection."""
from pathlib import Path, PurePosixPath
import argparse, hashlib, json, re, time
DEFAULT_MANIFEST = Path(__file__).resolve().parents[2]/'nx2-rom-audit-20261103/FILE_MANIFEST.json'
DEFAULT_ROOT = Path(r'E:\download\SW2-dumpster\KirbyAirRiders_GlobalTestRide')
KEYS = ['exefs/'+n for n in ['main','main.npdm','rtld','sdk','subsdk0','subsdk1']]+['romfs/'+n for n in ['Asset/assets.cat','Bin/config.bin','startup.dat','.nrr/EffectScript.nrr','Bin/Module/EffectScript.nro','Asset/assets_a000.dat','Asset/assets_b000.dat','Asset.Streaming/C93975A2MV.bin']]
MAX_HASH_FILE=128*1024*1024
MAX_HASH_TOTAL=256*1024*1024

def read_manifest(path):
    if path.stat().st_size>2*1024*1024: raise ValueError('manifest byte budget')
    rows=json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(rows,list) or len(rows)>4096: raise ValueError('manifest row budget/type')
    result={}
    for row in rows:
        if not isinstance(row,dict) or not isinstance(row.get('path'),str) or not isinstance(row.get('sha256'),str) or 'size' not in row: raise ValueError('manifest row fields')
        key=row['path'].replace('\\','/'); parts=PurePosixPath(key)
        if parts.is_absolute() or '..' in parts.parts or ':' in key or not key: raise ValueError('manifest escaping path')
        if key in result or not isinstance(row['size'],int) or row['size']<0 or not re.fullmatch('[0-9a-f]{64}',row['sha256']): raise ValueError('manifest row/duplicate')
        result[key]=row
    return result

def audit(main, manifest=None, manifest_root=None):
    start=time.monotonic_ns(); main=Path(main).absolute(); exefs=main.parent; set_root=exefs.parent; sibling=set_root/'romfs'
    r={'schema':1,'source':'filesystem_preflight_not_runtime','runtime_verified':False,'main':str(main),'exefs':str(exefs),'set_root':str(set_root),'sibling_romfs':str(sibling),'issues':[],'files':[],'hashed_bytes':0,'hash_budget':MAX_HASH_TOTAL}
    if main.name!='main' or not main.is_file(): r['issues'].append('main_missing_or_wrong_name')
    images=[x for x in exefs.iterdir() if x.is_file() and x.suffix=='.romfs'] if exefs.is_dir() else []
    r['image_candidates']=[str(x) for x in images];r['image_order']='host directory enumeration; not guaranteed equal to VFS order'
    if images:
        r['romfs_kind']='exefs_image';r['romfs_path']=str(images[0]) if len(images)==1 else None
        r['issues'].append('romfs_image_unverified_by_directory_manifest')
        if len(images)>1:r['issues'].append('ambiguous_multiple_romfs_images')
    else:
        r['romfs_kind']='sibling_directory' if sibling.is_dir() else 'missing';r['romfs_path']=str(sibling) if sibling.is_dir() else None
        if not sibling.is_dir() or not any(x.name!='.listing' for x in sibling.iterdir()):r['issues'].append('sibling_romfs_missing_or_empty')
        if sibling.is_dir() and sibling.resolve().parent!=exefs.resolve().parent:r['issues'].append('resolved_exefs_romfs_roots_differ')
    rows={}; prefix=None
    if manifest:
        rows=read_manifest(Path(manifest));r['manifest']=str(Path(manifest).absolute());r['manifest_root']=str(Path(manifest_root).absolute()) if manifest_root else None
        if not manifest_root:raise ValueError('manifest_root required')
        try:prefix=set_root.resolve().relative_to(Path(manifest_root).resolve()).as_posix()
        except ValueError:r['issues'].append('set_outside_manifest_root')
        r['expected_variant']=prefix;r['manifest_sha256']=hashlib.sha256(Path(manifest).read_bytes()).hexdigest()
    for rel in KEYS:
        p=set_root/rel
        if rel.startswith('romfs/') and images:continue
        row={'relative_path':rel,'path':str(p),'resolved_path':str(p.resolve()),'exists':p.is_file()}
        if not row['exists']:
            if rel in ['exefs/main','exefs/main.npdm','exefs/rtld','exefs/sdk'] or prefix and prefix+'/'+rel in rows:r['issues'].append('required_file_missing:'+rel)
            row['status']='absent';r['files'].append(row);continue
        if not p.resolve().is_relative_to(set_root.resolve()):r['issues'].append('file_resolves_outside_set:'+rel)
        before=p.stat();row['size']=before.st_size
        do_hash=before.st_size<=MAX_HASH_FILE and r['hashed_bytes']+before.st_size<=MAX_HASH_TOTAL
        if do_hash:
            h=hashlib.sha256()
            with p.open('rb') as f:
                while b:=f.read(4*1024*1024):h.update(b)
            row['sha256']=h.hexdigest();r['hashed_bytes']+=before.st_size
        else:
            row['hash_omitted']='bounded hash budget; size-only is not integrity verification'
            if rel.startswith('exefs/') or rel in ['romfs/Asset/assets.cat','romfs/Bin/config.bin','romfs/startup.dat','romfs/.nrr/EffectScript.nrr','romfs/Bin/Module/EffectScript.nro']: r['issues'].append('critical_hash_budget_exhausted:'+rel)
        after=p.stat()
        if (before.st_size,before.st_mtime_ns,before.st_ino)!=(after.st_size,after.st_mtime_ns,after.st_ino):r['issues'].append('file_changed_during_hash:'+rel)
        if rel in ['exefs/main','exefs/main.npdm']:
            with p.open('rb') as f:magic=f.read(4)
            row['magic']=magic.decode(errors='replace')
            if magic!=(b'NSO0' if rel=='exefs/main' else b'META'):r['issues'].append('unexpected_header:'+rel)
        key=(prefix+'/'+rel) if prefix else None;expected=rows.get(key)
        row['expected_manifest_path']=key
        if rows and not expected:row['status']='unlisted';r['issues'].append('not_in_expected_manifest:'+rel)
        elif expected:
            row['size_match']=before.st_size==expected['size'];row['hash_match']=row.get('sha256')==expected['sha256'] if do_hash else None
            row['status']='hash_match' if do_hash and row['hash_match'] and row['size_match'] else 'size_only_match' if not do_hash and row['size_match'] else 'mismatch'
            if row['status']=='mismatch':r['issues'].append('manifest_mismatch:'+rel)
            row['matching_variants']=[k[:-len(rel)-1] for k,v in rows.items() if k.endswith('/'+rel) and row['size']==v['size'] and do_hash and row['sha256']==v['sha256']]
        else:row['status']='hashed_no_reference' if do_hash else 'size_only_no_reference'
        r['files'].append(row)
    discriminators=[x for x in r['files'] if x['relative_path'] in ['exefs/main','exefs/main.npdm','romfs/Asset/assets.cat','romfs/Bin/config.bin','romfs/startup.dat'] and 'matching_variants' in x]
    candidates=None
    for x in discriminators:candidates=set(x['matching_variants']) if candidates is None else candidates&set(x['matching_variants'])
    r['consistent_hash_variants']=sorted(candidates or []);r['variant_ambiguous']=len(r['consistent_hash_variants'])!=1
    r['status']='FAIL' if r['issues'] else 'MANIFEST_CRITICAL_MATCH' if rows else 'STRUCTURAL_ONLY'
    r['limits']=['not runtime selection proof','large resource content not rehashed','local manifest not independent authenticity','mods/patch overlays not predicted']
    r['duration_ns']=time.monotonic_ns()-start
    return r

def main():
    p=argparse.ArgumentParser();p.add_argument('--main',required=True);p.add_argument('--manifest',type=Path);p.add_argument('--manifest-root',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
    try:r=audit(a.main,a.manifest,a.manifest_root)
    except (ValueError,KeyError,TypeError,OSError,json.JSONDecodeError) as e:r={'status':'FAIL','source':'filesystem_preflight_not_runtime','runtime_verified':False,'issues':[str(e)]}
    text=json.dumps(r,indent=2,ensure_ascii=False)
    if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(text,encoding='utf-8')
    print(text);return 2 if r['status']=='FAIL' else 0
if __name__=='__main__':raise SystemExit(main())
