"""Request-linked bounded metadata, not resource-pixel or presentation-completion proof."""
from pathlib import Path
import json,re,time

def inspect_context(bridge,request_id,status):
    if not isinstance(request_id,str) or not re.fullmatch('[0-9a-f]{32}',request_id):raise ValueError('Expected exact SDK capture UUID')
    path=Path(bridge)/'diagnostics'/('frame-capture-'+request_id+'.context.json')
    if not path.is_file():return {'available':False,'reason':'context_pending_or_disabled','path':str(path)}
    if path.stat().st_size>16384:raise ValueError('Context metadata exceeds bound')
    data=json.loads(path.read_text(encoding='utf-8'))
    if data.get('schema')!=1 or data.get('request_id')!=request_id:raise ValueError('Context schema/request mismatch')
    if type(data.get('input_count')) is not int or not 0<=data['input_count']<=4 or len(data.get('inputs',[]))!=data['input_count']:raise ValueError('Invalid context inputs')
    if type(data.get('source_count')) is not int or not 0<=data['source_count']<=16 or len(data.get('sources',[]))!=data['source_count']:raise ValueError('Invalid context sources')
    for source in data['sources']:
        if source.get('role') not in ['offscreen_recomposition','swapchain_draw','other_draw'] or type(source.get('layer')) is not int or not 0<=source['layer']<4:raise ValueError('Invalid role/layer')
        if type(source.get('selection_valid')) is not bool or type(source.get('accelerated')) is not bool:raise ValueError('Invalid selection state')
        if len(source.get('crop',[]))!=4:raise ValueError('Invalid crop')
    match=data.get('run_id')==status.get('run_id') and str(data.get('generation'))==str(status.get('module_generation'))
    return {'available':True,'matches_current_session':match,'historical_snapshot':True,'snapshot_age_seconds':max(0,time.time()-data.get('snapshot_epoch_ms',0)/1000),'path':str(path.resolve()),'data':data,'scope':'request-linked metadata at renderer scope exit; no source pixels, writer identity, slot generation or presentation completion'}
