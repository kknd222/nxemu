"""Bounded, request/session-linked source and renderer-target pixel evidence."""
from pathlib import Path
import hashlib,json,re,time

MAX_BYTES=8*1024*1024
FORMATS={37,43,44,50,51,57}

def inspect_pair(bridge,request_id,status):
    if not isinstance(request_id,str) or not re.fullmatch('[0-9a-f]{32}',request_id):
        raise ValueError('Expected SDK capture UUID')
    root=Path(bridge)/'diagnostics';p=root/('frame-capture-'+request_id+'.pair.json')
    if not p.is_file():return {'available':False,'reason':'pair_pending_or_disabled','path':str(p)}
    if p.is_symlink() or p.stat().st_size>16384:raise ValueError('Invalid pair metadata path or size')
    d=json.loads(p.read_text(encoding='utf-8'))
    if d.get('schema')!=1 or d.get('request_id')!=request_id:raise ValueError('Pair request/schema mismatch')
    if type(d.get('composite_ordinal')) is not int or d['composite_ordinal']<0:raise ValueError('Invalid composite identity')
    if type(d.get('snapshot_epoch_ms')) is not int or d['snapshot_epoch_ms']<0:raise ValueError('Invalid snapshot time')
    if any(not isinstance(d.get(k),str) or not 0<len(d[k])<=64 for k in ('run_id','generation')):raise ValueError('Invalid pair session')
    images={}
    for role in ['source','target']:
        m=d.get(role)
        if not isinstance(m,dict) or m.get('status') not in ['captured','unavailable']:raise ValueError('Invalid image status')
        result={'data':m,'available':m['status']=='captured'}
        if result['available']:
            w,h=m.get('width'),m.get('height')
            if type(w) is not int or type(h) is not int or w<=0 or h<=0 or w*h>MAX_BYTES//4:raise ValueError('Image extent exceeds budget')
            n=w*h*4
            if (type(m.get('bytes')) is not int or m['bytes']!=n or
                type(m.get('vk_format')) is not int or m['vk_format'] not in FORMATS or
                type(m.get('samples')) is not int or m['samples']!=1 or m.get('gpu_readback_completed') is not True):
                raise ValueError('Invalid captured pixel metadata')
            prefix='frame-capture-'+request_id+'.'+role
            files={}
            for field,suffix in [('rgba_file','.rgba'),('ppm_file','.ppm')]:
                expected=prefix+suffix
                if m.get(field)!=expected:raise ValueError('Unexpected paired pixel basename')
                q=root/expected
                if q.is_symlink() or not q.is_file() or q.resolve().parent!=root.resolve():raise ValueError('Invalid pixel file path')
                limit=n if suffix=='.rgba' else (w*h*3+128)
                if q.stat().st_size>limit:raise ValueError('Pixel file exceeds byte budget')
                raw=q.read_bytes()
                files[field]=(q,raw)
            raw=files['rgba_file'][1];ppm=files['ppm_file'][1]
            head=f'P6\n{w} {h}\n255\n'.encode()
            if len(raw)!=n or not ppm.startswith(head) or len(ppm)!=len(head)+w*h*3:raise ValueError('Paired payload size/header mismatch')
            bgra=m['vk_format'] in (44,50)
            if m.get('raw_order')!=('BGRA8' if bgra else 'RGBA8'):raise ValueError('Raw format order mismatch')
            r,g,b=raw[2 if bgra else 0::4],raw[1::4],raw[0 if bgra else 2::4]
            rgb=ppm[len(head):]
            if rgb[0::3]!=r or rgb[1::3]!=g or rgb[2::3]!=b:raise ValueError('Raw and PPM differ')
            alpha=raw[3::4];black=sum(not (a|c|e) for a,c,e in zip(r,g,b))
            observed={'all_rgb_zero':not any(rgb),'all_rgba_zero':not any(raw),'alpha_min':min(alpha),
                      'alpha_max':max(alpha),'alpha_zero_pixels':alpha.count(0),'alpha_full_pixels':alpha.count(255),
                      'exact_black_pixels':black}
            if any(type(m.get(k)) is not type(v) or m[k]!=v for k,v in observed.items()):raise ValueError('Pixel statistics mismatch')
            known=m.get('swizzle_known');mapping=m.get('component_swizzle')
            if type(known) is not bool:raise ValueError('Invalid swizzle availability')
            view_keys=['view_all_rgb_zero','view_alpha_min_encoded','view_alpha_max_encoded','view_alpha_zero_pixels','view_alpha_full_pixels']
            view={k:None for k in view_keys}
            if known:
                if (not isinstance(mapping,list) or len(mapping)!=4 or
                    any(type(s) is not int or not 0<=s<=6 for s in mapping)):raise ValueError('Invalid component swizzle')
                stored=[r,g,b,alpha];sampled=[]
                for c,s in enumerate(mapping):
                    sampled.append(stored[c] if s==0 else bytes(w*h) if s==1 else b'\xff'*(w*h) if s==2 else stored[s-3])
                va=sampled[3]
                view={'view_all_rgb_zero':not any(sampled[0]) and not any(sampled[1]) and not any(sampled[2]),
                      'view_alpha_min_encoded':min(va),'view_alpha_max_encoded':max(va),
                      'view_alpha_zero_pixels':va.count(0),'view_alpha_full_pixels':va.count(255)}
            elif mapping is not None:raise ValueError('Unknown swizzle has a mapping')
            if any(type(m.get(k)) is not type(v) or m.get(k)!=v for k,v in view.items()):raise ValueError('Derived view statistics mismatch')
            result.update(view)
            result.update(observed)
            result['rgba_path']=str(files['rgba_file'][0].resolve());result['ppm_path']=str(files['ppm_file'][0].resolve())
            result['rgba_sha256']=hashlib.sha256(raw).hexdigest();result['ppm_sha256']=hashlib.sha256(ppm).hexdigest()
        images[role]=result
    match=d.get('run_id')==status.get('run_id') and str(d.get('generation'))==str(status.get('module_generation'))
    return {'available':True,'matches_current_session':match,'historical_snapshot':True,'path':str(p.resolve()),
            'snapshot_age_seconds':max(0,time.time()-d.get('snapshot_epoch_ms',0)/1000),
            'data':d,'images':images,'scope':d.get('scope','renderer-owned pixels, not OS display')}
