"""Build the visual feature tour from actual UHD renderer output.

Requires Pillow and NumPy; run audit_presets.py first. Synthetic fixtures and
explicit diagnostic settings need no ROM. Temporal crops retain every captured
frame, shown four times slower for inspection. No generated CRT imagery.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from audit_presets import ROOT, charts

BG = '#0c111b'
FG = '#f1f4f7'
MUTED = '#adb9c8'
ACCENT = '#70e0c5'
FONT = '/System/Library/Fonts/Avenir Next.ttc'
RENDERER_HASHES = {}


def font(size):
    return ImageFont.truetype(FONT, size)


def text(im, xy, label, size=26, fill=FG):
    ImageDraw.Draw(im).text(xy, label, font=font(size), fill=fill)


def panel(number, category, title, subtitle, height):
    im = Image.new('RGB', (1600, height), BG)
    text(im, (40, 25), number + '  /  ' + category.upper(), 24, ACCENT)
    text(im, (40, 65), title, 52)
    text(im, (40, 137), subtitle, 25, MUTED)
    return im


def crop(source, cx, cy, w, h, scale=1):
    with Image.open(source) as im:
        part = im.crop((cx-w//2, cy-h//2, cx+w//2, cy+h//2))
    return part.resize((w*scale, h*scale), Image.Resampling.NEAREST)


def capture(binary, out, name, data, preset, frames=1):
    """Cache by exact input, preset and executable hash; never reuse stale tuning."""
    if binary not in RENDERER_HASHES:
        renderer = hashlib.sha256(binary.read_bytes())
        shaders = binary.parent.parent / 'shaders'
        for path in sorted(shaders.rglob('*')):
            if path.suffix in ('.spv', '.msl'):
                renderer.update(str(path.relative_to(shaders)).encode())
                renderer.update(path.read_bytes())
        RENDERER_HASHES[binary] = renderer.digest()
    key = hashlib.sha256(data + json.dumps(preset, sort_keys=True).encode()
                         + RENDERER_HASHES[binary] + str(frames).encode()).hexdigest()
    directory = out / name
    directory.mkdir(parents=True, exist_ok=True)
    stamp = directory / 'capture.json'
    if stamp.exists() and json.loads(stamp.read_text())['sha256'] == key:
        return directory
    source = directory / 'source.raw'; source.write_bytes(data)
    settings = directory / 'preset.json'; settings.write_text(json.dumps(preset, indent=2)+'\n')
    base = directory / 'frame.ppm'
    with tempfile.TemporaryDirectory(prefix='mynes-feature-') as tmp:
        env = dict(os.environ, XDG_CONFIG_HOME=tmp, MYNES_REVIEW_NO_INPUT='1')
        with (directory / 'render.log').open('w') as log:
            subprocess.run([str(binary), '--simulate-frame', str(source), '--preset', str(settings),
                            '--offscreen', '3840x2160', '--sdr', '--room-reflections', '--mask-alignment', 'physical',
                            '--screenshot-after', '30', '--screenshot-frames', str(frames),
                            '--screenshot-path', str(base)], cwd=ROOT, env=env,
                           stdout=log, stderr=log, check=True, timeout=180)
    for i in range(frames):
        path = base if i == 0 else Path(str(base)+f'.frame-{i:03d}.ppm')
        with Image.open(path) as im:
            assert im.size == (3840, 2160)
            if frames == 1:
                im.save(directory / f'{i}.png')
            else:
                # Native pixels of uniform grey and normal NES black ($0f).
                im.crop((2380, 170, 2660, 270)).save(directory / f'grey-{i}.png')
                im.crop((600, 170, 880, 270)).save(directory / f'black-{i}.png')
        path.unlink(); Path(str(path)+'.linear.pfm').unlink()
    stamp.write_text(json.dumps({'sha256': key, 'renderer_sha256': RENDERER_HASHES[binary].hex(),
                                'size': [3840,2160], 'frames': [30,29+frames],
                                'preset': preset, 'source_sha256': hashlib.sha256(data).hexdigest()}, indent=2)+'\n')
    print('captured', name, flush=True)
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audit', type=Path, default=Path('/tmp/mynes-preset-audit-4k'))
    parser.add_argument('--work', type=Path, default=Path('/tmp/mynes-feature-showcase'))
    parser.add_argument('--dest', type=Path, default=ROOT/'docs/images/feature-tour')
    parser.add_argument('--binary', type=Path, default=ROOT/'build/bin/mynes_gpu')
    args = parser.parse_args(); dest=args.dest; dest.mkdir(parents=True, exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)
    preset=lambda n: json.loads((ROOT/'presets'/f'{n}.json').read_text())
    image_path=lambda kind,n: args.audit/kind/f'{n}-0.png'

    p=panel('01','Beam current','Light has a footprint.',
            'One source line. Three drive levels. Bright strokes spread further.',820)
    for j,label in enumerate(['LOW DRIVE  ·  $00','MID DRIVE  ·  $10','HIGH DRIVE  ·  $20']):
        text(p,(40+j*520,200),label,23,ACCENT)
    for row,(name,label) in enumerate([('sony_pvm_14l2','Sony PVM-14L2  /  focused monitor'),
                                        ('bedroom_rf_1990','Bedroom RF 1990  /  consumer tube')]):
        y=248+row*250
        text(p,(40,y),label,27)
        for j,cx in enumerate([790,1220,1680]):
            p.paste(crop(image_path('beam',name),cx,1080,120,40,4),(40+j*520,y+48))
    text(p,(40,766),'4K SDR captures · 4× nearest-pixel inspection · identical exposure · no phase averaging',23,MUTED)
    p.save(dest/'beam.png')

    p=panel('02','Phosphor structure','Three ways to give light a texture.',
            'The pattern belongs to the tube. The beam and glass determine how clearly you see it.',740)
    for j,(name,title,detail) in enumerate([('arcade_cabinet','Shadow mask','Arcade Cabinet · 440 triads'),
                 ('bedroom_rf_1990','Slot mask','Bedroom RF 1990 · 410 triads'),
                 ('late_crt_wega','Aperture grille','Late consumer grille · 680 triads')]):
        x=40+j*520
        text(p,(x,206),title,32)
        p.paste(crop(image_path('chart',name),2490,230,160,100,3),(x,270))
        text(p,(x,596),detail,24,MUTED)
    text(p,(40,674),'Same grey code $10 · 4K physical mask pitch · 3× nearest-pixel inspection',23,MUTED)
    p.save(dest/'masks.png')

    grid=np.full((240,256),0x0f,dtype=np.uint8)
    for y in range(12,229,24):grid[y,8:249]=0x10
    for x in range(8,249,24):grid[12:229,x]=0x10
    p=panel('03','Deflection & focus','The edges tell a different story.',
            'Raster geometry changes where the beam lands; edge focus changes its footprint.',1310)
    for j,(name,label) in enumerate([('toshiba_14af43','Near-flat  /  Toshiba 14AF'),('bedroom_rf_1990','Rounded  /  Bedroom RF 1990')]):
        d=capture(args.binary,args.work,'grid-'+name,grid.tobytes(),preset(name))
        text(p,(40+j*780,208),label,30)
        with Image.open(d/'0.png') as im:
            p.paste(im.crop((480,0,3360,2160)).resize((740,555),Image.Resampling.LANCZOS),(40+j*780,265))
    target=np.full((240,256),0x0f,dtype=np.uint8)
    for cx,cy in [(128,120),(224,32)]:
        target[cy,cx-6:cx+7]=0x20;target[cy-6:cy+7,cx]=0x20
    diagnostic=preset('reference_composite')
    diagnostic['tv'].update(edge_focus=1.2,convergence_static=0,convergence_dynamic=0,
                            conv_r_x=0,conv_r_y=0,conv_b_x=0,conv_b_y=0,vignette=0)
    d=capture(args.binary,args.work,'edge-focus-diagnostic',target.tobytes(),diagnostic)
    for j,(cx,cy,label) in enumerate([(1920,1080,'CENTRE'),(3000,288,'TOWARDS THE CORNER')]):
        x=40+j*780
        text(p,(x,852),label,23,ACCENT)
        # Locate the deflected cross, without resizing one more than the other.
        a=np.asarray(Image.open(d/'0.png')); yy,xx=np.mgrid[cy-100:cy+100,cx-100:cx+100]
        weight=a[cy-100:cy+100,cx-100:cx+100].astype(float).mean(2)
        mx=int((xx*weight).sum()/weight.sum());my=int((yy*weight).sum()/weight.sum())
        p.paste(crop(d/'0.png',mx,my,240,140,2),(x+100,899))
    text(p,(40,1206),'Top: full raster reduced from 4K. Bottom: identical crosses, 2× nearest-pixel inspection.',23,MUTED)
    text(p,(40,1246),'Focus diagnostic: Reference composite, edge focus 1.2, convergence and vignette disabled.',23,MUTED)
    p.save(dest/'geometry-focus.png')

    p=panel('04','Room light','A television lives in a room.',
            'Soft external reflections sit on the glass, independent of the phosphor’s own light.',1020)
    black=bytes([0x0f])*(256*240)
    for j,(name,label) in enumerate([('famicom_kitchen','Cool kitchen light'),('living_room_1988','Warm evening lamp'),('warm_desktop_monitor','Amber desktop light')]):
        x=40+j*520; text(p,(x,210),label,29)
        d=capture(args.binary,args.work,'glass-'+name,black,preset(name))
        with Image.open(d/'0.png') as im:p.paste(im.resize((480,270),Image.Resampling.LANCZOS),(x,265))
        with Image.open(image_path('game',name)) as im:
            p.paste(im.crop((480,0,3360,2160)).resize((480,360),Image.Resampling.LANCZOS),(x,570))
    text(p,(40,950),'Top: black signal. Bottom: gameplay. Reduced 4K views · authored lighting, not a measured room.',23,MUTED)
    p.save(dest/'room-light.png')

    series=[]
    for name in ['basement_tv','vhs_sp_consumer']:
        series.append(capture(args.binary,args.work,'noise-'+name,charts()['chart'],preset(name),48))
    frames=[]
    for i in range(48):
        p=panel('05','Reception & tape','Noise has a place in the chain.',
                'RF reception and tape playback disturb the signal before the beam and mask.',880)
        for j,(folder,title) in enumerate(zip(series,['Basement TV  /  RF','VHS SP playback'])):
            x=40+j*780;text(p,(x,206),title,34)
            for k,label in enumerate(['grey','black']):
                text(p,(x,268+k*250),'MID-GREY' if k==0 else 'NEAR BLACK',22,ACCENT)
                with Image.open(folder/f'{label}-{i}.png') as im:
                    p.paste(im.resize((560,200),Image.Resampling.NEAREST),(x+80,306+k*250))
        text(p,(40,818),'48 consecutive 4K frames · 2× nearest pixels · 4× slower · no exposure boost or added noise',23,MUTED)
        frames.append(p)
    frames[0].save(dest/'noise.webp',save_all=True,append_images=frames[1:],duration=67,loop=0,lossless=True)
    frames[0].save(dest/'noise-still.png')
    manifest={'size':[3840,2160],'output':'SDR','mask_alignment':'physical',
              'audit_source':str(args.audit),'inspection_enlargement':'nearest-neighbour; indicated on each panel',
              'noise_frames':48,'noise_frame_duration_ms':67,
              'diagnostic_captures':{f.parent.name:json.loads(f.read_text()) for f in args.work.glob('*/capture.json')}}
    (dest/'sources.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':main()
