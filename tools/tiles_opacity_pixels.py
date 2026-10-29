"""Pixel checks for the isolated checkerboard transparency controls.

Flat-color ROI means measure alpha; checkerboard variance distinguishes a live
blur backdrop from a missing blur. Overlapped/background-ambiguous animation
samples are excluded explicitly instead of being assigned an expected color.
"""
import json
from pathlib import Path
import numpy as np
from PIL import Image

FG=np.array([240.,160.,64.])
BG=127.5


def rectangle(client):
    return dict(x=client['at'][0],y=client['at'][1],width=client['size'][0],height=client['size'][1])


def expected(name, focus):
    if name=='fb20-rule':return np.array([217.5,153.5,76.5])
    if name=='fb20-client':return np.array([183.5,143.5,95.5])
    alpha=(.85 if focus=='fb20-active' else .4) if name=='fb20-active' else 1.
    return np.floor(FG*alpha+BG*(1-alpha)) + (.5 if alpha==.4 else 0)


def roi(image,box):
    x=int(box['x']+box['width']*.4);y=int(box['y']+box['height']*.4)
    assert x>=16 and y>=16 and x+16<=image.shape[1] and y+16<=image.shape[0], ('ROI outside output',box)
    return image[y-16:y+16,x-16:x+16]


def verify(root, fp16=False):
    root=Path(root);measurements=[];frames_measured=[];desktop_measured=[]
    for path in sorted(root.glob('blur-*.png')):
        data=json.loads(path.with_suffix('.json').read_text())
        image=np.array(Image.open(path).convert('RGB'))
        focus='fb20-active' if '-fb20-active-' in path.stem else 'fb20-opaque'
        if '-hover-' in path.stem:
            focus=path.stem.split('-hover-')[1]
        blur=path.stem.startswith('blur-1-')
        if '-desktop' in path.stem:
            visible_workspaces={t['activeWorkspace']['id'] for t in data['tiles']['tiles']}
            for window in data['clients']:
                if window['workspace']['id'] not in visible_workspaces or window['class'] not in ('fb20-rule','fb20-active','fb20-client','fb20-opaque'):continue
                b=rectangle(window);x=b['x']+b['width']*.4;y=b['y']+b['height']*.4
                if not (20<x<image.shape[1]-20 and 20<y<image.shape[0]-20):continue
                pixels=roi(image,b);mean=pixels.mean((0,1));std=pixels.std((0,1));want=expected(window['class'],focus)
                assert np.max(np.abs(mean-want))<2.1,(path,window['class'],'desktop alpha',mean,want)
                if window['class']=='fb20-client':assert std.max()<4 if blur else std.min()>30
                desktop_measured.append(dict(image=path.name,client=window['class'],mean=mean.tolist(),std=std.tolist()))
            continue
        for window in data['overview']['windows']:
            name=window['class'];pixels=roi(image,window['contentGeometry'])
            mean=pixels.mean((0,1));std=pixels.std((0,1));want=expected(name,focus)
            assert np.max(np.abs(mean-want))<2.1,(path,name,'incorrect alpha',mean,want)
            if name=='fb20-client':
                assert (std.max()<4 if blur else std.min()>30),(path,'blur backdrop missing or unexpected',std)
            measurements.append(dict(image=path.name,client=name,mean=mean.tolist(),std=std.tolist(),expected=want.tolist()))
    for path in sorted(root.glob('blur-*-opening.json'))+sorted(root.glob('blur-*-closing.json')):
        frames=json.loads(path.read_text());intermediate=0;sampled=0
        for frame in frames:
            if fp16:assert frame['framebuffer']['format']==0x48344241 and frame['framebuffer']['mirror_format'],('FP16/MRT missing',path,frame['framebuffer'])
            windows=frame['overview']['windows'];image=np.array(Image.open(frame['pixels']).convert('RGB'))
            for window in windows:
                if window['class']!='fb20-client':continue
                b=window['contentGeometry'];samples=[]
                for fx in (.25,.5,.75):
                    for fy in (.25,.5,.75):
                        x=int(b['x']+b['width']*fx);y=int(b['y']+b['height']*fy)
                        if x<20 or y<20 or x+20>image.shape[1] or y+20>image.shape[0] or min(b['width'],b['height'])<90:continue
                        obstacles=[w['contentGeometry'] for w in windows if w['address']!=window['address']]+[e['geometry'] for e in frame['overview']['strip']]
                        if any(not(x+20<o['x'] or x-20>o['x']+o['width'] or y+20<o['y'] or y-20>o['y']+o['height']) for o in obstacles):continue
                        pixels=image[y-16:y+16,x-16:x+16];mean=pixels.mean((0,1))
                        error=float(np.max(np.abs(mean-expected('fb20-client',''))))
                        if path.name.startswith('blur-1-') and pixels.std((0,1)).max()>=4:continue
                        samples.append(error)
                if not samples:continue
                error=min(samples)
                assert error<3,(path,'animation alpha mismatch',frame['overview']['visualProgress'],error)
                sampled+=1;intermediate+=int(.01<frame['overview']['visualProgress']<.99)
        assert sampled and intermediate,(path,'no unambiguous animation evidence',sampled,intermediate)
        frames_measured.append(dict(capture=path.name,frames=len(frames),sampled=sampled,intermediate=intermediate))
    assert len(measurements)>=32 and len(frames_measured)==16,(len(measurements),len(frames_measured))
    hover_measured=[]
    for path in sorted(root.glob('blur-*-hover-*-transition.json')):
        frames=json.loads(path.read_text());checked={};sizes={}
        for frame in frames:
            if fp16:assert frame['framebuffer']['format']==0x48344241 and frame['framebuffer']['mirror_format']
            windows=frame['overview']['windows'];image=np.array(Image.open(frame['pixels']).convert('RGB'))
            native={w['address']:w for w in frame['native']}
            for window in windows:
                name=window['class']
                if name not in ('fb20-rule','fb20-active','fb20-client'):continue
                b=window['contentGeometry'];sizes.setdefault(name,[]).append(b['width']*b['height'])
                errors=[]
                alpha=native[window['address']]['active_alpha']
                want=FG*alpha+BG*(1-alpha) if name!='fb20-client' else expected(name,'')
                for fx in (.3,.4,.6,.7):
                    for fy in (.3,.4,.6,.7):
                        x=int(b['x']+b['width']*fx);y=int(b['y']+b['height']*fy)
                        if x<20 or y<20 or x+20>image.shape[1] or y+20>image.shape[0]:continue
                        obstacles=[w['contentGeometry'] for w in windows if w['address']!=window['address']]+[e['geometry'] for e in frame['overview']['strip']]
                        if any(not(x+20<o['x'] or x-20>o['x']+o['width'] or y+20<o['y'] or y-20>o['y']+o['height']) for o in obstacles):continue
                        pixels=image[y-16:y+16,x-16:x+16];mean=pixels.mean((0,1))
                        if name=='fb20-client' and path.name.startswith('blur-1-') and pixels.std((0,1)).max()>=4:continue
                        errors.append(float(np.max(np.abs(mean-want))))
                if not errors:continue
                assert min(errors)<3,(path,name,'hover animation alpha mismatch',alpha,min(errors))
                checked[name]=checked.get(name,0)+1
        assert all(checked.get(name,0)>1 for name in ('fb20-rule','fb20-active','fb20-client')),(path,checked)
        hover_measured.append(dict(capture=path.name,frames=len(frames),checked=checked,area_ranges={k:[min(v),max(v)] for k,v in sizes.items()}))
    report=dict(desktop=desktop_measured,stable=measurements,animations=frames_measured,hover_animations=hover_measured,fp16=fp16)
    (root/'pixel-checks.json').write_text(json.dumps(report,indent=2))
    return report
