"""Read-only Everything inventory and content-verified duplicate space audit.

No deletion, hardlink replacement, compression or executable launch is performed.
"""
from pathlib import Path
import argparse, collections, csv, ctypes, hashlib, io, json, os, shutil, subprocess, time

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(4 * 1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()

def allocation(path, fallback):
    if os.name != 'nt':
        return fallback
    fn = ctypes.WinDLL('kernel32', use_last_error=True).GetCompressedFileSizeW
    fn.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
    fn.restype = ctypes.c_ulong
    high = ctypes.c_ulong()
    ctypes.set_last_error(0)
    low = fn(str(path), ctypes.byref(high))
    if low == 0xffffffff and ctypes.get_last_error():
        return fallback
    return (high.value << 32) | low

def within(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--min-duplicate-mib', type=int, default=8)
    a = ap.parse_args()
    root = a.root.resolve()
    out = a.out.resolve()
    assert root.is_dir() and within(out, root) and not out.exists()
    assert a.min_duplicate_mib >= 1
    out.mkdir()
    start = time.time()
    before = shutil.disk_usage(root.anchor)
    cmd = [r'D:\Program Files\Everything\es.exe', '-path', str(root), '/a-d', '-csv',
           '-size', '-size-format', '1', '-date-modified', '-date-format', '1']
    result = subprocess.run(cmd, capture_output=True, timeout=90)
    assert result.returncode == 0, result.stderr
    # ES uses UTF-8 for pipe output on this installation.
    raw = result.stdout.decode('utf-8-sig')
    (out / 'EVERYTHING_INVENTORY.csv').write_text(raw, encoding='utf-8')
    rows = list(csv.DictReader(io.StringIO(raw)))
    top = collections.Counter()
    repo_parts = collections.Counter()
    extension = collections.Counter()
    by_size = collections.defaultdict(list)
    seen = set()
    physical = collections.Counter()
    errors = []
    inventory = []
    for row in rows:
        p = Path(row['Filename'])
        try:
            resolved = p.resolve()
            if not within(resolved, root):
                errors.append({'path': str(p), 'reason': 'outside resolved root'})
                continue
            s = p.stat()
            if not p.is_file():
                continue
            rel = p.relative_to(root)
            alloc = allocation(p, s.st_size)
            ident = (s.st_dev, s.st_ino)
            top[rel.parts[0]] += s.st_size
            extension[p.suffix.lower()] += s.st_size
            if rel.parts[0] == 'repo' and len(rel.parts) > 1:
                repo_parts[rel.parts[1]] += s.st_size
            if ident not in seen:
                physical[rel.parts[0]] += alloc
                seen.add(ident)
            item = {'path': str(p), 'size': s.st_size, 'allocated_bytes': alloc,
                    'identity': list(ident), 'mtime_ns': s.st_mtime_ns,
                    'date_modified_index': row['Date Modified']}
            inventory.append(item)
            # Evidence only; never select unique repo, live builds, ROM, save or firmware.
            if (rel.parts[0].startswith('nx2-') and p.suffix.lower() in {'.lib', '.pdb', '.dll', '.exe'}
                    and s.st_size >= a.min_duplicate_mib * 1024 * 1024):
                by_size[s.st_size].append(item)
        except OSError as ex:
            errors.append({'path': str(p), 'reason': str(ex)})
    print('SPACE_INVENTORY files=' + str(len(inventory)), flush=True)
    duplicates = []
    for index, (size, items) in enumerate(sorted(by_size.items(), reverse=True)):
        if len(items) < 2:
            continue
        hashes = collections.defaultdict(list)
        for item in items:
            path = Path(item['path'])
            before_stat = path.stat()
            h = digest(path)
            after_stat = path.stat()
            if (before_stat.st_size, before_stat.st_mtime_ns) != (after_stat.st_size, after_stat.st_mtime_ns):
                errors.append({'path': str(path), 'reason': 'changed during hash; excluded'})
                continue
            hashes[h].append(item)
        for h, group in hashes.items():
            unique = {tuple(x['identity']): x['allocated_bytes'] for x in group}
            if len(unique) > 1:
                duplicates.append({'sha256': h, 'size': size, 'copies': len(group),
                    'distinct_file_ids': len(unique),
                    'estimated_hardlink_reclaim_bytes': sum(unique.values()) - max(unique.values()),
                    'files': group})
        if index % 8 == 0:
            print('SPACE_HASH_PROGRESS group=' + str(index) + ' duplicate_groups=' + str(len(duplicates)), flush=True)
    summary = {'root': str(root), 'started_epoch': start, 'elapsed_seconds': time.time() - start,
        'read_only': True, 'delete_performed': False, 'hardlink_performed': False,
        'everything_command': subprocess.list2cmdline(cmd), 'file_count': len(inventory),
        'logical_bytes': sum(top.values()), 'unique_allocated_bytes': sum(physical.values()),
        'free_bytes_before': before.free, 'free_bytes_after': shutil.disk_usage(root.anchor).free,
        'top_level_logical_bytes': dict(top.most_common()),
        'top_level_allocated_bytes': dict(physical.most_common()),
        'repo_parts_logical_bytes': dict(repo_parts.most_common()),
        'extension_logical_bytes': dict(extension.most_common()),
        'duplicate_groups': len(duplicates),
        'estimated_hardlink_reclaim_bytes': sum(g['estimated_hardlink_reclaim_bytes'] for g in duplicates),
        'limitations': ['Everything may be stale; errors retained; protected paths excluded from duplicate candidates',
            'Physical attribution of existing hardlinks uses first occurrence only',
            'Same hash is not deletion approval; current binaries/source/unique fixtures/reports retained',
            'Hardlink candidate copies must remain immutable; no linking against live build files'], 'errors': errors}
    (out / 'DUPLICATES.json').write_text(json.dumps(duplicates, indent=2), encoding='utf-8')
    (out / 'VERIFICATION.txt').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    (out / 'INVENTORY.json').write_text(json.dumps(inventory), encoding='utf-8')
    lines = ['# NXEmu 空间审计（只读）', '',
             '删除/硬链接替换均未执行；以下为实测，不是清理授权。', '', '|目录|逻辑GiB|', '|---|---:|']
    lines += [f'|{key}|{value / 2**30:.3f}|' for key,value in top.most_common(25)]
    lines += ['', f'合计逻辑 {sum(top.values()) / 2**30:.3f} GiB；唯一文件分配 {sum(physical.values()) / 2**30:.3f} GiB。',
        f'内容核验的证据二进制重复组 {len(duplicates)}；保留路径硬链接去重估计可释放 {summary["estimated_hardlink_reclaim_bytes"] / 2**30:.3f} GiB。',
        '保留唯一repo、当前build/bin、全部源补丁/原件/报告/fixture/回滚；重复证据二进制先征求确认。',
        '优先建议：不可变证据副本按SHA256硬链接去重，保持原路径；下一阶段再决定旧测试对象/非最终可执行文件是否删除。',
        '预计收益是唯一分配字节之差，执行前仍需重新核hash、文件ID、存储卷与活动占用。']
    (out / 'REPORT.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print('SPACE_AUDIT_PASS read_only=1 duplicate_groups=' + str(len(duplicates)) +
          ' estimated_reclaim_GiB=' + str(round(summary['estimated_hardlink_reclaim_bytes'] / 2**30, 3)))

if __name__ == '__main__':
    main()
