"""Detect colored windows outside their owned scene, including hidden desktops."""
import json
from pathlib import Path
import numpy as np
from PIL import Image

COLORS=[(0,0,1),(1,0,1),(0,1,0),(1,1,0),(0,1,1)]


def body_mask(pixels,color,maximum=None):
    if maximum is None:maximum=pixels.max(2)
    mask=maximum>50
    for channel,lit in enumerate(color):mask=mask & ((pixels[:,:,channel]>.7*maximum) if lit else (pixels[:,:,channel]<.3*maximum))
    # Red CSD + filtered blue/green edges can create a one-row marker hue.
    return mask & np.roll(mask,1,0) & np.roll(mask,-1,0) & np.roll(mask,1,1) & np.roll(mask,-1,1)


def box_mask(mask,box,padding=2):
    x=max(0,int(box['x'])-padding);y=max(0,int(box['y'])-padding)
    x2=min(mask.shape[1],int(np.ceil(box['x']+box['width']))+padding)
    y2=min(mask.shape[0],int(np.ceil(box['y']+box['height']))+padding)
    if x2>x and y2>y:mask[y:y2,x:x2]=True


def verify(root,clients,fp16=False,opacity=False):
    root=Path(root);reports=[];negative=None;settings=json.loads((root/'fb21-settings.json').read_text())
    workspace={a:str(1 if i<2 else 2 if i<4 else 3) for i,a in enumerate(clients)}
    for meta in sorted(root.glob('*-frames.json'))+sorted(root.glob('*-release.json')):
        frames=json.loads(meta.read_text());assert frames,meta
        counts=dict(all=0,desktop=0,workspace=0,inactive=0);worst=0;alpha_checks=0
        for index,f in enumerate(frames):
            state=f['overview'];active=state['active'];scope=state.get('window_layout_scope');progress=state['visualProgress']
            if fp16:assert f['framebuffer']['drm_format']==f['framebuffer']['format']==0x48344241 and f['framebuffer']['mirror_format']
            phase='inactive' if not active else 'desktop' if progress<.001 else scope
            counts[phase]+=1
            pixels=np.asarray(Image.open(f['pixels']).convert('RGB'));maximum=pixels.max(2)
            native={n['address']:n for n in f['native']};managed={w['address']:w for w in state['windows']}
            focus=f.get('focus','')
            tiles=f['tiles']['tiles'];multi=len(tiles)>1
            displayed={str(t['activeWorkspace']['id']) for t in tiles} if multi else set()
            for address,color in zip(clients,COLORS):
                mask=body_mask(pixels,color,maximum)
                allowed=np.zeros(maximum.shape,dtype=bool)
                # Strip captures are a separate miniature scene, never ghosts.
                for strip in state.get('strip',[]):box_mask(allowed,strip['geometry'],4)
                if state.get('strip'):
                    # IPC entry rectangles describe settled cards; while the
                    # left strip slides in, pixels can precede those x values.
                    # Its entire reserved column is outside this main-scene
                    # oracle (FB19 separately checks actual strip pixels).
                    right=max(s['geometry']['x']+s['geometry']['width'] for s in state['strip'])
                    allowed[:,:min(allowed.shape[1],int(np.ceil(right))+4)]=True
                window=managed.get(address)
                # At zero openness the prepared exit can restore the opening
                # focus, rather than the hovered native focus. Its geometry is
                # still owned by the all-scope gesture until the scope flips.
                if active and window:
                    box_mask(allowed,window['contentGeometry'])
                elif not active:
                    n=native[address]
                    if n.get('active_workspace',False) or workspace[address] in displayed:
                        box_mask(allowed,dict(x=n['position'][0],y=n['position'][1],width=n['size'][0],height=n['size'][1]))
                leaked=int(np.count_nonzero(mask & ~allowed));worst=max(worst,leaked)
                assert leaked<=64,(meta,index,address,'window pixels outside owned scene',leaked,state['phase'],scope,progress)
                n=native[address]
                if active and scope=='workspace' and not window:
                    assert not n['visible'] and not n['forced'] and n['workspace_alpha']==0,(meta,index,'inactive workspace resurrected',n)
                    if negative is None:
                        # A synthetic ghost of an unmanaged workspace must
                        # fail the same body/ownership mask, including erosion.
                        planted=pixels.copy();y,x=pixels.shape[0]//2,pixels.shape[1]//2
                        planted[y:y+24,x:x+24]=np.array(color)*255
                        detected=int(np.count_nonzero(body_mask(planted,color) & ~allowed))
                        assert detected>64,('synthetic ghost escaped the oracle',detected)
                        negative=dict(synthetic=True,capture=meta.name,frame=index,foreign_pixels=detected)
                if opacity:
                    want=.85 if address==focus else .4
                    assert abs(n['active_alpha']-want)<.002,(meta,index,'native focus opacity',address,n['active_alpha'],want)
                    want_dim=0 if address==focus else .15
                    assert abs(n['dim']-want_dim)<.002,(meta,index,'native focus dim',address,n['dim'],want_dim)
                    alpha_checks+=1
        assert counts['workspace'] or meta.name.startswith('continuous-cancel-release'),(meta,'no workspace opening/closing evidence',counts)
        reports.append(dict(capture=meta.name,frames=len(frames),phases=counts,max_foreign_pixels=worst,native_alpha_checks=alpha_checks))
    assert len(reports)==6,reports
    observations=json.loads((root/'fb21-observations.json').read_text())
    target=clients[settings.get('target_index',2)]
    for case in observations:
        hover=case['hover'];assert hover['focus']['address']==target,('hover failed to change native focus',case['case'])
        before=case['opening'];after={w['address']:w for w in hover['overview']['windows']}
        original=next(w['contentGeometry'] for w in before['windows'] if w['address']==target);grown=after[target]['animationToGeometry']
        if settings['hover_delay']>=.3:assert grown['width']*grown['height']>original['width']*original['height']*1.01,('hover did not expand preview',case['case'])
        else:assert hover['overview']['relayout_active'],('gesture missed the in-flight hover expansion',case['case'])
        settled=case['settled']['overview'];assert not settled['relayout_active']
        if 'gesture' in settled:assert not settled['gesture']['active'] and settled['transient_window_count']==0
        closed=json.loads((root/(case['case']+'-closed.json')).read_text())['overview'];assert not closed['active'] and not closed['windows']
    assert negative is not None,'no inactive-workspace negative control'
    result=dict(captures=reports,frames=sum(r['frames'] for r in reports),max_foreign_pixels=max(r['max_foreign_pixels'] for r in reports),negative_control=negative,fp16=fp16,opacity=opacity)
    (root/'fb21-pixel-checks.json').write_text(json.dumps(result,indent=2));return result


if __name__=='__main__':
    import sys
    root=Path(sys.argv[1]);settings=json.loads((root/'fb21-settings.json').read_text())
    verify(root,json.loads((root/'fb21-clients.json').read_text()),settings['fp16'],settings['opacity'])
