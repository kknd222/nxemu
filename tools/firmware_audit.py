"""Read-only ZIP vs installed registered NCA audit. No keys or firmware changes."""
from pathlib import Path, PurePosixPath
import argparse,hashlib,json,zipfile,sys,time

def digest(stream):
 h=hashlib.sha256()
 while b:=stream.read(1024*1024):h.update(b)
 return h.hexdigest()

def audit(archive,installed,transformation=None):
 archive=Path(archive).resolve(strict=True);installed=Path(installed).resolve(strict=True)
 start=time.monotonic();rows=[];ignored=[];names=set();budget=0;known={}
 if transformation:
  source=Path(transformation).resolve(strict=True)
  if source.stat().st_size>2**20:raise ValueError('Transformation manifest budget exceeded')
  data=json.loads(source.read_text(encoding='utf-8-sig'))
  for entry in data['files']:
   name=entry['file'].lower()
   if name in known:raise ValueError('Duplicate transformation entry')
   known[name]=entry
 with zipfile.ZipFile(archive) as z:
  if len(z.infolist())>4096:raise ValueError('ZIP entry budget exceeded')
  for info in z.infolist():
   p=PurePosixPath(info.filename)
   if p.is_absolute() or '..' in p.parts or '\\' in info.filename or ':' in info.filename:raise ValueError('Invalid ZIP path')
   if info.is_dir():continue
   if p.suffix.lower()!='.nca':ignored.append(info.filename);continue
   name=p.name.lower()
   if name in names:raise ValueError('Ambiguous duplicate NCA basename')
   names.add(name);budget+=info.file_size
   if info.file_size>2**31 or budget>8*2**30:raise ValueError('ZIP decompressed byte budget exceeded')
   target=installed/p.name
   if not target.resolve().is_relative_to(installed):raise ValueError('Escaping target')
   with z.open(info) as f:sha=digest(f) # CRC checked by ZipFile while streaming
   row=dict(zip_path=info.filename,installed_path=str(target),expected_bytes=info.file_size,expected_sha256=sha)
   if not target.is_file():row['result']='missing'
   elif target.stat().st_size!=info.file_size:row['result']='size_mismatch';row['actual_bytes']=target.stat().st_size
   else:
    with target.open('rb') as f:row['actual_sha256']=digest(f)
    row['result']='match' if sha==row['actual_sha256'] else 'hash_mismatch'
    entry=known.get(name)
    if row['result']=='hash_mismatch' and entry and entry.get('original_sha256')==sha and entry.get('modified_sha256')==row['actual_sha256']:
     with target.open('rb') as f:f.seek(0x200);magic=f.read(4)
     if magic==b'DNCA':row['result']='recorded_transform_match'
   rows.append(row)
   print(f'\r[{len(rows):4d}] audited={budget/2**20:.1f} MiB elapsed={time.monotonic()-start:.1f}s',end='',file=sys.stderr,flush=True)
 extras=[str(p) for p in installed.iterdir() if p.is_file() and p.suffix.lower()=='.nca' and p.name.lower() not in names]
 return dict(schema=1,archive=str(archive),installed=str(installed),scope='direct registered files compared to unique ZIP NCA basenames; encrypted bytes only',
             proves='copy consistency with supplied ZIP; not NCA decryption, title catalog, RomFS contents, or firmware version compatibility',
             matched=sum(r['result']=='match' for r in rows),transformed_matches=sum(r['result']=='recorded_transform_match' for r in rows),
             transformation_manifest=str(transformation) if transformation else None,transformation_boundary='matches recorded source/output hashes and DNCA magic; does not reverify decryption or authenticity',
             total=len(rows),extras=extras,ignored_zip_paths=ignored,rows=rows,seconds=time.monotonic()-start)

def main():
 p=argparse.ArgumentParser();p.add_argument('--zip',required=True);p.add_argument('--installed',required=True);p.add_argument('--out',required=True);p.add_argument('--transformation-manifest');a=p.parse_args()
 try:
  d=audit(a.zip,a.installed,a.transformation_manifest);Path(a.out).write_text(json.dumps(d,indent=2),encoding='utf-8')
  ok=d['total']>0 and d['matched']+d['transformed_matches']==d['total'] and not d['extras']
  print(f"\nFIRMWARE_COPY_AUDIT {'PASS' if ok else 'DIFFERENCE'}; matched={d['matched']}/{d['total']}; transformed={d['transformed_matches']}; extras={len(d['extras'])}");return 0 if ok else 2
 except (OSError,ValueError,zipfile.BadZipFile) as e:print(f'AUDIT_ERROR; {e}');return 1
if __name__=='__main__':sys.exit(main())
