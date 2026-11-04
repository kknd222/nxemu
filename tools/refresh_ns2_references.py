"""Explicit, bounded NS2 reference refresh; never pulls into the main source tree.

Each invocation creates a new evidence directory. Release numbers are not ordered
as source revisions: both branch HEAD and resolved release tag commit are retained.
"""
from pathlib import Path
import argparse, datetime, hashlib, json, re, urllib.request

REPOS = {'Djihads80/Eglantine': 'nx2', 'sskyNS/Eden-NS2-dlss-osd': 'main',
         'Wael3rd/Eglantine-MKW': 'mkw-eglantine', '0x2593/Eden-NS2': 'main'}
FILES = ('src/shader_recompiler/frontend/ampere/lower_to_maxwell.h',
         'src/shader_recompiler/frontend/maxwell/translate/impl/move_special_register.cpp',
         'src/video_core/renderer_vulkan/vk_query_cache.cpp',
         'src/video_core/texture_cache/descriptor_table.h')
LIMIT = 2 * 1024 * 1024

def utc(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def sha(data): return hashlib.sha256(data).hexdigest()
def valid_pin(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{40}', value):
        raise ValueError('Expected full lowercase commit SHA')
    return value

class Reader:
    def __init__(self, root): self.root, self.records = root, []
    def get(self, url, name):
        rec = {'url': url, 'utc': utc(), 'ok': False}
        self.records.append(rec)
        try:
            request = urllib.request.Request(url, headers={'User-Agent': 'NXEmu-reference-refresh',
                                                          'Accept': 'application/vnd.github+json'})
            with urllib.request.urlopen(request, timeout=12) as response:
                data = response.read(LIMIT + 1)
                rec['status'] = response.status
            if len(data) > LIMIT: raise ValueError('Response exceeds 2 MiB')
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            rec.update(ok=True, path=str(target.resolve()), bytes=len(data), sha256=sha(data))
            return data
        except (OSError, ValueError) as error:
            rec['error'] = str(error)
            return None

def refresh_one(repo, branch, previous, reader, sources=False):
    name = repo.replace('/', '_'); api = 'https://api.github.com/repos/' + repo
    row = {'repo': repo, 'ref': branch, 'url': 'https://github.com/' + repo,
           'old_head': previous.get(repo), 'head': None, 'status': 'head_unavailable',
           'releases': [], 'sources': []}
    raw = reader.get(api + '/commits/' + branch, name + '/HEAD.json')
    if raw is None: return row
    try: row['head'] = valid_pin(json.loads(raw)['sha'])
    except (ValueError, KeyError, TypeError):
        row['status'] = 'invalid_head'; return row
    row['changed'] = row['head'] != row['old_head'] if row['old_head'] else None
    row['status'] = 'head_verified'
    raw = reader.get(api + '/releases?per_page=3', name + '/RELEASES.json')
    if raw is not None:
        try:
            releases = json.loads(raw)
            if not isinstance(releases, list): raise ValueError('Invalid releases response')
            for release in releases[:3]:
                tag = release['tag_name']
                if not isinstance(tag, str): raise ValueError('Invalid release tag')
                item = {'tag': tag, 'published_at': release.get('published_at'),
                        'target_commitish': release.get('target_commitish'), 'resolved_commit': None}
                row['releases'].append(item)
                # GitHub /commits/{ref} resolves annotated/lightweight tags to commits.
                from urllib.parse import quote
                payload = reader.get(api + '/commits/' + quote(tag, safe=''),
                                     name + '/TAG-' + str(len(row['releases'])) + '.json')
                if payload is not None:
                    try: item['resolved_commit'] = valid_pin(json.loads(payload)['sha'])
                    except (ValueError, KeyError, TypeError): item['error'] = 'invalid_tag_commit'
        except (ValueError, KeyError, TypeError): row['release_error'] = 'invalid_releases'
    else: row['release_error'] = 'releases_unavailable'
    if sources and row['changed'] is not False:
        for file in FILES:
            payload = reader.get('https://raw.githubusercontent.com/' + repo + '/' + row['head'] + '/' + file,
                                 name + '/source/' + file)
            row['sources'].append({'file': file, 'ok': payload is not None,
                                   'sha256': sha(payload) if payload is not None else None})
    row['sources_skipped_unchanged'] = sources and row['changed'] is False
    return row

def self_test():
    class Fake:
        def __init__(self, head): self.head, self.calls = head, []
        def get(self, url, name):
            self.calls.append(url)
            if '/releases?' in url:
                return json.dumps([{'tag_name': 'build178', 'target_commitish': 'experimental'},
                                   {'tag_name': 'build177', 'target_commitish': 'nx2'}]).encode()
            if '/commits/build178' in url: return json.dumps({'sha': 'b'*40}).encode()
            if '/commits/build177' in url: return json.dumps({'sha': 'a'*40}).encode()
            if 'raw.githubusercontent' in url: return b'fixed source bytes'
            return None if self.head is None else json.dumps({'sha': self.head}).encode()
    repo = 'Djihads80/Eglantine'
    f = Fake('a'*40); r = refresh_one(repo, 'nx2', {repo: 'a'*40}, f, True)
    assert r['changed'] is False and r['sources_skipped_unchanged'] and len(r['sources']) == 0
    assert [x['resolved_commit'] for x in r['releases']] == ['b'*40, 'a'*40]
    f = Fake('c'*40); r = refresh_one(repo, 'nx2', {repo: 'a'*40}, f, True)
    assert r['changed'] is True and len(r['sources']) == 4
    assert all(x['sha256'] == sha(b'fixed source bytes') for x in r['sources'])
    assert refresh_one(repo, 'nx2', {}, Fake(None))['status'] == 'head_unavailable'
    assert refresh_one(repo, 'nx2', {}, Fake('invalid'))['status'] == 'invalid_head'
    assert refresh_one(repo, 'nx2', {}, Fake('a'*40))['changed'] is None
    print('NS2_REFRESH_SELFTEST_PASS; cases=5; network=False; build_number_not_ordered=True')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--previous', type=Path)
    parser.add_argument('--sources', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test: self_test(); return 0
    if args.out is None: parser.error('--out NEW_DIRECTORY required')
    old = {}
    if args.previous:
        data = args.previous.read_bytes()
        if len(data) > 4 * 1024 * 1024: raise ValueError('Previous metadata exceeds 4 MiB')
        for row in json.loads(data)['repositories']:
            if row.get('head'): old[row['repo']] = valid_pin(row['head'])
    root = args.out.resolve(); root.mkdir(parents=True, exist_ok=False)
    reader = Reader(root)
    rows = [refresh_one(repo, branch, old, reader, args.sources) for repo, branch in REPOS.items()]
    # Forgejo remains explicitly separate: HEAD is readable, pinned raw access may fail.
    import subprocess
    command = ['git', 'ls-remote', 'https://git.eden-emu.dev/Takoroka/eden-nx2.git', 'HEAD']
    forge = {'repo': 'Takoroka/eden-nx2', 'ref': 'HEAD', 'old_head': old.get('Takoroka/eden-nx2'),
             'head': None, 'status': 'head_unavailable', 'command': command, 'sources_reviewed': False}
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        forge.update(exit_status=result.returncode, literal_stdout=result.stdout, literal_stderr=result.stderr)
        if result.returncode == 0:
            forge['head'] = valid_pin(result.stdout.split()[0]); forge['status'] = 'head_verified'
            forge['changed'] = forge['head'] != forge['old_head'] if forge['old_head'] else None
    except (OSError, ValueError, IndexError, subprocess.TimeoutExpired) as error: forge['error'] = str(error)
    rows.append(forge)
    verified = all(sha(Path(x['path']).read_bytes()) == x['sha256'] for x in reader.records if x['ok'])
    report = {'schema': 1, 'utc': utc(), 'repositories': rows, 'acquisition': reader.records,
              'hashes_verified': verified, 'scope': 'metadata and opt-in changed pinned sources; no emulator modifications'}
    (root/'LATEST_REFERENCES.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    complete = verified and all(x['status'] == 'head_verified' for x in rows) and all(x['ok'] for x in reader.records)
    report['exit_status'] = 0 if complete else 3
    (root/'LATEST_REFERENCES.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('NS2_REFRESH_COMPLETE; heads=' + str(sum(x['status']=='head_verified' for x in rows)) +
          '; hashes_verified=' + str(verified) + '; exit=' + str(report['exit_status']))
    return report['exit_status']

if __name__ == '__main__': raise SystemExit(main())
