"""SHA256-verified evidence-only hardlink dedup; default dry-run, journalled rollback."""
from pathlib import Path
import argparse, base64, ctypes, hashlib, json, os, shutil, stat, time

def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(4*1024*1024), b''):h.update(b)
    return h.hexdigest()

def checked(p,root):
    p=Path(p).absolute(); r=p.resolve(); rel=r.relative_to(root)
    if p!=r or not rel.parts[0].startswith('nx2-') or r.suffix.lower() not in {'.lib','.pdb','.dll','.exe'}:
        raise ValueError('not an ordinary evidence binary: '+str(p))
    if not r.is_file():raise ValueError('missing file: '+str(r))
    return r

def security(p):
    if os.name!='nt':return b''
    f=ctypes.WinDLL('advapi32',use_last_error=True).GetFileSecurityW
    f.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32)]
    f.restype=ctypes.c_int
    n=ctypes.c_uint32();f(str(p),7,None,0,ctypes.byref(n))
    if not n.value:raise ctypes.WinError(ctypes.get_last_error())
    buf=ctypes.create_string_buffer(n.value)
    if not f(str(p),7,buf,n.value,ctypes.byref(n)):raise ctypes.WinError(ctypes.get_last_error())
    return buf.raw[:n.value]

def save(path,value):
    temp=path.with_name(path.name+'.tmp');temp.write_text(json.dumps(value,indent=2),encoding='utf-8');os.replace(temp,path)

def run(a):
    root=a.root.resolve();out=a.out.resolve();out.relative_to(root)
    if out.exists():raise ValueError('fresh output required')
    out.mkdir();before=shutil.disk_usage(root.anchor).free
    report={'root':str(root),'apply':a.apply,'changes':[],'skipped':[],'free_bytes_before':before,'completed':False}
    journal=out/'JOURNAL.json';save(journal,report)
    groups=json.loads(a.plan.read_text(encoding='utf-8-sig'))
    reclaimed=0
    for i,g in enumerate(groups):
        records=sorted(g['files'],key=lambda x:x['path'].lower())
        source=checked(records[0]['path'],root); source_stat=source.stat(); sec=security(source)
        if sha(source)!=g['sha256']:raise ValueError('canonical hash changed: '+str(source))
        for rec in records[1:]:
            dest=checked(rec['path'],root);s=dest.stat()
            if s.st_dev!=source_stat.st_dev:raise ValueError('cross volume')
            if os.path.samefile(source,dest):continue
            if s.st_size!=g['size'] or sha(dest)!=g['sha256']:raise ValueError('duplicate hash changed: '+str(dest))
            if security(dest)!=sec or getattr(s,'st_file_attributes',0)!=getattr(source_stat,'st_file_attributes',0):
                report['skipped'].append({'path':str(dest),'reason':'security or attributes differ'});continue
            change={'source':str(source),'path':str(dest),'sha256':g['sha256'],'size':s.st_size,
                'atime_ns':s.st_atime_ns,'mtime_ns':s.st_mtime_ns,'mode':stat.S_IMODE(s.st_mode),
                'security':base64.b64encode(sec).decode(),'original_identity':[s.st_dev,s.st_ino],
                'state':'prepared','hardlink_temp':str(dest.with_name(dest.name+'.nx2-dedup.tmp'))}
            report['changes'].append(change);save(journal,report)
            if a.apply:
                temp=Path(change['hardlink_temp'])
                if temp.exists():raise ValueError('temporary path exists')
                # Both paths verified under the exact evidence root; atomic file replacement only.
                os.link(source,temp);os.replace(temp,dest)
                if not os.path.samefile(source,dest):raise ValueError('hardlink verification failed')
                change['state']='linked';reclaimed+=rec.get('allocated_bytes',s.st_size)
            else:change['state']='dry_run'
            save(journal,report)
        if i%8==0:print('DEDUP_PROGRESS group='+str(i)+' paths='+str(len(report['changes'])),flush=True)
    report.update(completed=True,free_bytes_after=shutil.disk_usage(root.anchor).free,
        expected_reclaim_bytes=reclaimed,observed_free_delta=shutil.disk_usage(root.anchor).free-before,
        source_bytes_preserved=True,paths_preserved=True,
        metadata_note='Hardlinks share canonical creation/mtime and ACL; old per-path mtime saved for independent-file rollback.',
        mutable_live_builds_excluded=True)
    save(journal,report)
    print('EVIDENCE_DEDUP_PASS apply='+str(int(a.apply))+' linked_paths='+str(len(report['changes']))+
        ' skipped='+str(len(report['skipped']))+' observed_free_GiB='+str(round(report['observed_free_delta']/2**30,3)))

def rollback(a):
    root=a.root.resolve();journal=a.journal.resolve();journal.relative_to(root)
    r=json.loads(journal.read_text());restored=0
    for c in r['changes']:
        if c['state'] not in {'linked','prepared'}:continue
        src=checked(c['source'],root);dest=checked(c['path'],root)
        if sha(dest)!=c['sha256']:raise ValueError('changed evidence; stop rollback')
        if not os.path.samefile(src,dest):
            c['state']='independent';save(journal,r);continue
        temp=dest.with_name(dest.name+'.nx2-unlink.tmp')
        if temp.exists():raise ValueError('rollback temp exists')
        shutil.copyfile(dest,temp)
        if sha(temp)!=c['sha256']:raise ValueError('rollback copy hash mismatch')
        os.chmod(temp,c['mode']);os.utime(temp,ns=(c['atime_ns'],c['mtime_ns']))
        os.replace(temp,dest)
        if os.path.samefile(src,dest) or sha(dest)!=c['sha256']:raise ValueError('independent restore failed')
        if security(dest)!=base64.b64decode(c['security']):raise ValueError('restored security differs')
        c['state']='restored_independent';restored+=1;save(journal,r)
    r['rollback_restored_paths']=restored;r['rollback_exit_status']=0;save(journal,r)
    print('EVIDENCE_ROLLBACK_PASS restored_paths='+str(restored)+' bytes_preserved=1')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True)
    sub=p.add_subparsers(dest='action',required=True)
    d=sub.add_parser('dedup');d.add_argument('--plan',type=Path,required=True);d.add_argument('--out',type=Path,required=True);d.add_argument('--apply',action='store_true')
    b=sub.add_parser('rollback');b.add_argument('--journal',type=Path,required=True)
    a=p.parse_args()
    if a.action=='dedup':run(a)
    else:rollback(a)

if __name__=='__main__':main()
