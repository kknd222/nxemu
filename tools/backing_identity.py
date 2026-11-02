"""Read-only backing-lifecycle diagnosis from a saved SDK event snapshot."""
def diagnose(events):
    entries=[e for e in events if e.get('category')=='device-backing']
    mismatches=[e for e in entries if e.get('data',{}).get('matches') is False]
    ready=[e for e in entries if e.get('data',{}).get('stage')=='gpu_ready']
    return {'observed':bool(entries),'events':len(entries),'mismatch_seen':bool(mismatches),
            'gpu_ready_matches':ready[-1]['data'].get('matches') if ready else None,
            'latest_ready':ready[-1]['data'] if ready else None,
            'initial_mismatch':mismatches[0]['data'] if mismatches else None}
