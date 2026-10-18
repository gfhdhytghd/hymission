"""FB15: compare Stage flight paths and apertures with the actual static slots."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageChops
from tiles_stage_flight_clip_regression import MARKER_COLORS

KEYS = ('x', 'y', 'width', 'height')

def error(a, b):
    return max(abs(a[k]-b[k]) for k in KEYS)

def card_for(screen, ws):
    return next(c for c in screen['cards'] if c['workspace']==ws)

def viewport(screen, card, count):
    if count == 1:
        return {k:card[k] for k in KEYS}
    desk=screen['desktop_geometry']
    scale=min(card['width']/desk['width'],card['height']/desk['height'])
    w,h=desk['width']*scale,desk['height']*scale
    return dict(x=card['x']+(card['width']-w)/2,y=card['y']+(card['height']-h)/2,width=w,height=h)

def verify(root, observe=False):
    checks=[]
    for result in json.loads((root/'results.json').read_text()):
        tile,route,start,end=(result[k] for k in ('tile','route','start','end'))
        name=f'tile{tile}-{route}-{start}-{end}'
        frames=json.loads((root/(name+'.json')).read_text())
        count=len(frames[0]['stage']['screens'])
        initial=frames[0]['stage']['screens'][tile];final=frames[-1]['stage']['screens'][tile]
        moving=[(i,f,f['stage']['screens'][tile]) for i,f in enumerate(frames) if f['stage']['screens'][tile]['flights']]
        assert moving,(name,'missing flights')
        incoming=card_for(initial,end)['windows'][0]
        outgoing=card_for(final,start)['windows'][0]
        endpoint_errors=[];path_errors=[];aperture_errors=[];segments={}
        held=max((json.loads(p.read_text())['screens'][tile]['swipe_progress']
                  for p in root.glob(name+'-held-*.json')),default=0)
        for index,frame,screen in moving:
            p=screen['transition_progress'];t=1-(1-p)**3
            for flight in screen['flights']:
                ws=flight['workspace'];a,b=flight['from'],flight['to'];actual=flight['frame']
                first=ws not in segments
                if first and ws==end:endpoint_errors.append(error(a,incoming))
                destination_card=next((c for c in screen['cards'] if c['workspace']==start),card_for(final,start))
                if ws==start:endpoint_errors.append(error(b,destination_card['windows'][0]))
                if flight.get('viewport'):
                    av=viewport(initial,card_for(initial,end),count) if ws==end else initial['desktop_geometry']
                    bv=screen['desktop_geometry'] if ws==end else viewport(screen,destination_card,count)
                    if not first:
                        previous=segments[ws]
                        if error(a,previous['from'])>.1:
                            # Committing a held gesture starts a new segment at
                            # the exact finger-owned frame, with a fresh clock.
                            q=1-(1-held)**3
                            prior_view={k:previous['av'][k]+(previous['bv'][k]-previous['av'][k])*q for k in KEYS}
                            prior_frame={}
                            for axis,extent in (('x','width'),('y','height')):
                                aa,bb=previous['from'],previous['to'];va,vb=previous['av'],previous['bv']
                                prior_frame[axis]=prior_view[axis]+((aa[axis]-va[axis])/va[extent]*(1-q)+(bb[axis]-vb[axis])/vb[extent]*q)*prior_view[extent]
                                prior_frame[extent]=(aa[extent]/va[extent]*(1-q)+bb[extent]/vb[extent]*q)*prior_view[extent]
                            endpoint_errors.append(error(a,prior_frame))
                            av=prior_view
                        else:av=previous['av']
                    segments[ws]={'from':a,'to':b,'av':av,'bv':bv}
                    expected_view={k:av[k]+(bv[k]-av[k])*t for k in KEYS}
                    aperture_errors.append(error(flight['viewport'],expected_view))
                    expected={k:0 for k in KEYS}
                    for axis,extent in (('x','width'),('y','height')):
                        expected[axis]=expected_view[axis]+((a[axis]-av[axis])/av[extent]*(1-t)+(b[axis]-bv[axis])/bv[extent]*t)*expected_view[extent]
                        expected[extent]=(a[extent]/av[extent]*(1-t)+b[extent]/bv[extent]*t)*expected_view[extent]
                else:
                    if not first and error(a,segments[ws]['from'])>.1:
                        previous=segments[ws];aa,bb=previous['from'],previous['to']
                        q=1-(1-held)**3
                        pos=q if bb['width']>aa['width'] or bb['height']>aa['height'] else held*held*(3-2*held)
                        prior={k:aa[k]+(bb[k]-aa[k])*q for k in ('width','height')}
                        for axis,extent in (('x','width'),('y','height')):
                            prior[axis]=aa[axis]+aa[extent]/2+(bb[axis]+bb[extent]/2-aa[axis]-aa[extent]/2)*pos-prior[extent]/2
                        endpoint_errors.append(error(a,prior))
                    segments[ws]={'from':a,'to':b}
                    pos=t if b['width']>a['width'] or b['height']>a['height'] else p*p*(3-2*p)
                    expected={k:a[k]+(b[k]-a[k])*t for k in ('width','height')}
                    for axis,extent in (('x','width'),('y','height')):
                        center=a[axis]+a[extent]/2+(b[axis]+b[extent]/2-a[axis]-a[extent]/2)*pos
                        expected[axis]=center-expected[extent]/2
                path_errors.append(error(actual,expected))
        # Subtract the analytic motion remaining after the last rendered flight
        # frame. A timer may retire it before an exact p=1 frame is presented.
        # Any extra landing jump in the real color bounds must be <=2 pixels.
        _,last,screen=moving[-1]
        def fitted_edges(b,native,view):
            scale=min(b['width']/native['size'][0],b['height']/native['size'][1])
            w,h=native['size'][0]*scale,native['size'][1]*scale
            x,y=b['x']+(b['width']-w)/2,b['y']+(b['height']-h)/2
            return (max(x,view['x']),max(y,view['y']),min(x+w,view['x']+view['width']),min(y+h,view['y']+view['height']))
        pixel_errors={}
        full=dict(x=0,y=0,width=Image.open(last['pixels']).width,height=Image.open(last['pixels']).height)
        for ws in (start,end):
            color=tuple(bytes.fromhex(MARKER_COLORS[2*(ws//100-1)+ws%100-1]))
            boxes=[]
            for f in (last,frames[-1]):
                im=Image.open(f['pixels']).convert('RGB');mask=Image.new('L',im.size,255)
                for channel,c in zip(im.split(),color):mask=ImageChops.multiply(mask,channel.point(lambda v,c=c:255 if abs(v-c)<45 else 0))
                boxes.append(mask.getbbox())
            assert all(boxes),(name,'missing endpoint client',ws,boxes)
            flight=next(f for f in screen['flights'] if f['workspace']==ws)
            native=next(n for n in last['native'] if n['workspace']==str(ws))
            end_native=next(n for n in frames[-1]['native'] if n['workspace']==str(ws))
            end_box=outgoing if ws==start else dict(x=end_native['position'][0],y=end_native['position'][1],width=end_native['size'][0],height=end_native['size'][1])
            end_view=viewport(final,card_for(final,start),count) if ws==start else final['desktop_geometry']
            expected_last=fitted_edges(flight['frame'],native,flight.get('viewport') or full)
            expected_final=fitted_edges(end_box,end_native,end_view)
            pixel_errors[ws]=max(abs((a-b)-(ea-eb)) for a,b,ea,eb in zip(*boxes,expected_last,expected_final))
        pixel_error=max(pixel_errors.values())
        row=dict(name=name,frames=len(frames),flight_frames=len(moving),source_and_slot_error=max(endpoint_errors,default=0),path_error=max(path_errors,default=0),aperture_error=max(aperture_errors,default=0),landing_pixel_error=pixel_error,landing_by_workspace=pixel_errors,last_flight_phase=screen['transition_progress'],final_slot=outgoing)
        checks.append(row)
        (root/'handoff-results.json').write_text(json.dumps(checks,indent=2))
        if not observe:
            assert row['source_and_slot_error']<.1 and row['path_error']<1 and row['aperture_error']<.1,row
            assert pixel_error<=2,row
    print(('OBSERVED' if observe else 'PASS')+': static/flight geometry and native apertures; landing raster <=2 px')

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('root',type=Path);ap.add_argument('--observe',action='store_true');args=ap.parse_args();verify(args.root,args.observe)
