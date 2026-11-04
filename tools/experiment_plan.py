"""Validate coupled experiment plans; validation never executes or passes a repair."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re


def validate_plan(plan):
    if not isinstance(plan, dict) or plan.get('schema') != 1:
        raise ValueError('Unsupported experiment plan')
    components = plan.get('components')
    if not isinstance(components, list) or not 2 <= len(components) <= 16:
        raise ValueError('Expected 2..16 components')
    by_id, writers = {}, {}
    for component in components:
        if not isinstance(component, dict):
            raise ValueError('Invalid component')
        cid = component.get('id')
        if not isinstance(cid, str) or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,31}', cid) or cid in by_id:
            raise ValueError('Invalid or duplicate component ID')
        for field in ('owner', 'local_oracle', 'coverage_limit'):
            if not isinstance(component.get(field), str) or not component[field].strip():
                raise ValueError('Missing component ' + field)
        requires, files = component.get('requires'), component.get('writes')
        if not isinstance(requires, list) or len(requires) > 16 or any(not isinstance(x, str) for x in requires) or len(set(requires)) != len(requires):
            raise ValueError('Invalid component dependencies')
        if not isinstance(files, list) or len(files) > 128:
            raise ValueError('Invalid file ownership')
        for filename in files:
            if not isinstance(filename, str) or '\\' in filename or ':' in filename:
                raise ValueError('Use canonical relative file paths')
            path = PurePosixPath(filename)
            if path.is_absolute() or '..' in path.parts or str(path) != filename or filename in ('', '.'):
                raise ValueError('Invalid owned path')
            key = filename.casefold()
            if key in writers:
                raise ValueError('Duplicate file writer: ' + filename)
            writers[key] = component['owner']
        by_id[cid] = component
    visiting, visited, order = set(), set(), []

    def visit(cid):
        if cid not in by_id:
            raise ValueError('Unknown dependency: ' + cid)
        if cid in visiting:
            raise ValueError('Dependency cycle')
        if cid in visited:
            return
        visiting.add(cid)
        for dep in by_id[cid]['requires']:
            visit(dep)
        visiting.remove(cid)
        visited.add(cid)
        order.append(cid)

    for cid in by_id:
        visit(cid)
    pair = plan.get('coupled_pair')
    if not isinstance(pair, list) or len(pair) != 2 or any(not isinstance(x, str) or x not in by_id for x in pair) or pair[0] == pair[1]:
        raise ValueError('Invalid coupled pair')
    fixture = plan.get('fixed_fixture')
    required = ('input_identity', 'reference_identity', 'scene', 'cache_policy', 'adapter_policy')
    if not isinstance(fixture, dict) or any(not isinstance(fixture.get(x), str) or not fixture[x].strip() for x in required):
        raise ValueError('Incomplete fixed fixture')
    if not isinstance(plan.get('system_oracle'), str) or not plan['system_oracle'].strip():
        raise ValueError('Missing joint system oracle')
    rows = plan.get('matrix')
    expected = {'0': set(), pair[0]: {pair[0]}, pair[1]: {pair[1]}, '+'.join(pair): set(pair)}
    if not isinstance(rows, list) or len(rows) != 4:
        raise ValueError('Expected exact 0/A/B/A+B matrix')
    seen = set()
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get('label'), str) or row['label'] not in expected or row['label'] in seen:
            raise ValueError('Invalid or duplicate matrix row')
        seen.add(row['label'])
        enabled = row.get('enabled')
        if not isinstance(enabled, list) or any(not isinstance(x, str) for x in enabled) or len(enabled) != len(set(enabled)) or set(enabled) != expected[row['label']]:
            raise ValueError('Matrix row differs from declared intervention')
        if row.get('execution_status') != 'planned':
            raise ValueError('Plan validation cannot certify executed results')
        # A single component may require the other: retain that diagnostic row,
        # but explicitly mark its system result non-interpretable without the dependency.
        missing = sorted({dep for cid in enabled for dep in by_id[cid]['requires'] if dep not in enabled})
        if row.get('missing_dependencies') != missing:
            raise ValueError('Matrix dependency gaps must be explicit')
    return {'schema': 1, 'validation': 'valid', 'execution_status': 'planned',
            'topological_order': order, 'file_writers': writers, 'matrix_rows': 4,
            'scope': 'plan structure only; no local or system tests executed'}


def load_plan(path):
    with Path(path).open('rb') as stream:
        raw = stream.read(262145)
    if len(raw) > 262144:
        raise ValueError('Plan budget exceeded')
    return validate_plan(json.loads(raw.decode('utf-8-sig')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path')
    args = parser.parse_args()
    try:
        print(json.dumps(load_plan(args.path)))
        return 0
    except (OSError, ValueError, TypeError) as error:
        print(json.dumps({'validation': 'invalid', 'reason': str(error)}))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
