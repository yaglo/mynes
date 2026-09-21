#!/usr/bin/env python3
"""Recapture published GPU images/clips. Requires own ROMs, NumPy, Pillow, ffmpeg.

Full images are 3840x2880; diagnostic beam measurements retain their documented
3840x2160 fixture. No commercial input is written into the repository.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
IMAGES = ROOT / 'docs/images'
MAIN = ['sony_pvm_14l2', 'jvc_d_series_2000', 'toshiba_14af43', 'stass_favourite']
FPS = 60.0988139
EXPOSURE = .6
GAMES = {
    'castlevania': 'Castlevania (U) (PRG 1).nes',
    'darkwing': 'Darkwing Duck (U).nes',
    'kirby': "Kirby's Adventure (U) (PRG 1) [!].nes",
    'little-samson': 'Little Samson (U).nes',
    'mario-3': 'Super Mario Bros 3 (U) (PRG 1).nes',
    'mega-man-2': 'Mega Man 2 (U).nes',
    'contra-gameplay': 'Contra (U).nes',
}


def read_pfm(path):
    with path.open('rb') as f:
        assert f.readline().strip() == b'PF'
        w, h = map(int, f.readline().split())
        assert float(f.readline()) == -1
        a = np.frombuffer(f.read(), '<f4').reshape(h, w, 3)[::-1]
    assert np.isfinite(a).all()
    return a


def display(a):
    v = np.clip(a * EXPOSURE, 0, 1)
    v = np.where(v <= .0031308, 12.92*v, 1.055*v**(1/2.4)-.055)
    return Image.fromarray(np.uint8(np.rint(v*255)))


def thumbnail(a):
    # Filter emitted light, not gamma-encoded phosphor stripes.
    small = np.stack([np.asarray(Image.fromarray(a[:, :, c]).resize(
        (640, 480), Image.Resampling.LANCZOS)) for c in range(3)], axis=2)
    return display(small)


def label(pic, title):
    tile = Image.new('RGB', (pic.width, pic.height+32), '#17191c')
    tile.paste(pic, (0, 32))
    ImageDraw.Draw(tile).text((8, 7), title, fill='white', font=FONT)
    return tile


def sheet(tiles, path, columns=2):
    w, h = tiles[0].size
    im = Image.new('RGB', (w*columns, h*((len(tiles)+columns-1)//columns)), '#17191c')
    for i, tile in enumerate(tiles):
        assert tile.size == (w, h)
        im.paste(tile, ((i % columns)*w, (i//columns)*h))
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path)


def details(pic, title):
    # Separate, labelled native-pixel crops; never join two distant scanlines.
    w, h = pic.size
    parts = [pic.crop((int(w*x), int(h*y), int(w*x)+640, int(h*y)+400))
             for x, y in ((.4, .26), (.4, .57))]
    out = Image.new('RGB', (1296, 432), '#17191c')
    for x, part, region in zip((0, 656), parts, ('face', 'platform')):
        out.paste(label(part, title+' / '+region), (x, 0))
    return out


def capture(preset, source, frame=60, size='3840x2880', count=2, replay=None,
            start=None, extra_env=None):
    name = Path(str(preset)).stem
    with tempfile.TemporaryDirectory(prefix='mynes-docs-') as tmp:
        env = dict(os.environ, XDG_CONFIG_HOME=tmp, MYNES_REVIEW_NO_INPUT='1',
                   MYNES_OFFSCREEN_HEADROOM='1.6')
        for key in ('MYNES_REVIEW_INPUT_SCRIPT', 'MYNES_REVIEW_START_FRAME', 'MYNES_REVIEW_OSD'):
            env.pop(key, None)
        if replay:
            env['MYNES_REVIEW_INPUT_SCRIPT'] = str(ROOT/'tools/review'/replay)
        if start:
            env['MYNES_REVIEW_START_FRAME'] = str(start)
        env.update(extra_env or {})
        output = Path(tmp)/'frame.ppm'
        logpath = args.logs/f'{name}-{frame}-{size}-{len(records)}.log'
        cmd = [str(args.binary), *map(str, source), '--offscreen', size,
               '--mask-alignment', 'pixels', '--preset', str(preset),
               '--screenshot-after', str(frame), '--screenshot-path', str(output)]
        cmd += ['--screenshot-pair'] if count == 2 else ['--screenshot-frames', str(count)]
        with logpath.open('w') as log:
            subprocess.run(cmd, cwd=ROOT, env=env, stdout=log, stderr=log, check=True, timeout=360)
        numbers = list(map(int, re.findall(r'Capture frame (\d+)', logpath.read_text())))
        assert numbers == list(range(frame, frame+count)), numbers
        record = dict(preset=name, frame=frame, frames=count, size=size,
                      replay=replay, start_button_frame=start,
                      source_sha256=hashlib.sha256(Path(source[-1]).read_bytes()).hexdigest())
        records.append(record)
        for n in range(count):
            suffix = '' if n == 0 else ('.next.ppm' if count == 2 else f'.frame-{n:03d}.ppm')
            p = Path(str(output)+suffix+'.linear.pfm')
            a = read_pfm(p)
            yield a
            p.unlink()  # bound temporary storage; no raw capture files enter git
        print(name, frame, size, 'done', flush=True)


def gallery():
    folder = IMAGES/'contra-gallery'
    folder.mkdir(parents=True, exist_ok=True)
    presets = sorted(p.stem for p in (ROOT/'presets').glob('*.json') if p.stem != 'vhs_sp_consumer')
    presets.append('vhs_sp_consumer')  # preserve the established gallery group anchors
    tiles, crops, metrics = {}, {}, {}
    for p in presets:
        frames = capture(p, ['--simulate-frame', args.contra])
        a = next(frames); pic = display(a)
        pic.save(folder/f'{p}.webp', lossless=True, method=4)
        name = json.loads((ROOT/'presets'/f'{p}.json').read_text())['name']
        tiles[p] = label(thumbnail(a), name)
        crops[p] = details(pic, p)
        b = next(frames)
        metrics[p] = dict(linear_frame_difference_rms=float(np.sqrt(np.mean((a-b)**2))),
                          linear_peak=float(max(a.max(), b.max())),
                          linear_mean=a.mean((0, 1)).tolist(),
                          size=[3840, 2880], displayed_frame=60, exposure=EXPOSURE)
        list(frames)
        if p == 'studio_pvm':
            crops[p].save(folder/'studio-pvm-beam-detail.png')
        if p == 'vhs_sp_consumer':
            pic.save(IMAGES/'showcase/4k/contra-vhs-sp.png')
    for i in range(0, len(presets), 4):
        group = presets[i:i+4]
        sheet([tiles[p] for p in group], folder/f'overview-{i//4+1}.png', min(2, len(group)))
        sheet([crops[p] for p in group], folder/f'native-crops-{i//4+1}.png', 1)
    sheet([tiles[p] for p in MAIN], IMAGES/'crt-contra-review.png')
    sheet([crops[p] for p in MAIN], IMAGES/'crt-contra-native.png', 1)
    sheet([tiles[p] for p in ['bedroom_rf_1990', 'vivid_living_room', 'warm_desktop_monitor',
                             'dying_crt', 'nec_xm29_arcade', 'studio_pvm']],
          IMAGES/'preset-curation-contra.png')
    (ROOT/'docs/contra-gallery-metrics.json').write_text(json.dumps(metrics, indent=2)+'\n')


def gameplay():
    folder = IMAGES/'showcase/4k'
    for game, frame, replay, full, crop, box in [
        ('castlevania', 2500, 'castlevania-hall.input', 'castlevania-pvm-gameplay',
         'castlevania-pvm-detail', (1360, 1584, 2640, 2704)),
        ('darkwing', 8000, 'darkwing-bridge.input', 'darkwing-pvm-gameplay',
         'darkwing-pvm-gameplay-detail', (1600, 1000, 2880, 2152)),
    ]:
        for n, a in enumerate(capture(MAIN[0], [args.roms/GAMES[game]], frame, replay=replay)):
            if n == 0:
                pic = display(a); pic.save(folder/f'{full}.png')
                pic.crop(box).save(folder/f'{crop}.png')
    for p in MAIN:
        for n, a in enumerate(capture(p, [args.roms/GAMES['mega-man-2']], 900, start=600)):
            if n == 0:
                pic = display(a); pic.save(folder/f'{p}.png')
                pic.crop((1680, 427, 2704, 1110)).save(folder/f'{p}-beam.png')
                pic.crop((2973, 1413, 3741, 2096)).save(folder/f'{p}-rooftop.png')


def supporting():
    mario, recovery, window, inputs = [], [], [], []
    with tempfile.TemporaryDirectory(prefix='mynes-docs-pattern-') as tmp:
        pattern = np.full((240, 256), 16, dtype=np.uint8)
        pattern[50:90, 70:140] = 32; pattern[130:170, 70:140] = 15
        fixture = Path(tmp)/'recovery.bin'; fixture.write_bytes(pattern.tobytes())
        for p in MAIN:
            for n, a in enumerate(capture(p, [args.mario], 180)):
                if n == 0: mario.append(label(thumbnail(a), p))
            for n, a in enumerate(capture(p, ['--simulate-frame', fixture])):
                if n == 0: recovery.append(label(thumbnail(a), p))
            for n, a in enumerate(capture(p, ['--simulate-frame', args.contra], size='1280x960')):
                if n == 0:
                    pic=display(a)
                    window.append(label(pic.crop((450, 140, 1090, 620)), p+' / native 1280x960'))
        for conn in ('component', 'composite', 'rf'):
            data = json.loads((ROOT/'presets/sony_pvm_14l2.json').read_text())
            data['connection'] = conn
            data['rf'].update(enabled=int(conn=='rf'), carrier_level_dbm=-25,
                              noise_floor_dbm=-65, mod_bandwidth=4100000,
                              if_asymmetry=1, tuning_offset_hz=0,
                              agc_attack_ms=100, agc_release_ms=1000)
            preset = Path(tmp)/f'pvm-{conn}.json'; preset.write_text(json.dumps(data))
            for n, a in enumerate(capture(preset, ['--simulate-frame', args.contra])):
                if n == 0: inputs.append(label(thumbnail(a), 'PVM / '+conn))
    sheet(mario, IMAGES/'readme-mario.png')
    sheet(mario, IMAGES/'crt-preset-review.png')
    sheet(recovery, IMAGES/'crt-streak-review.png')
    sheet(window, IMAGES/'crt-contra-window-native.png')
    sheet(inputs, IMAGES/'crt-contra-inputs.png', 3)
    for mode, name in (('root','gpu-osd-review'),('adjust','gpu-osd-adjustment')):
        for n, a in enumerate(capture(MAIN[0], ['--simulate-frame', args.contra],
                                      size='1280x960', extra_env={'MYNES_REVIEW_OSD':mode})):
            if n == 0: display(a).save(IMAGES/(name+'.png'))


def beam():
    from measure_beam import measure
    metrics={}
    with tempfile.TemporaryDirectory(prefix='mynes-beam-') as tmp:
        codes=np.full((240,256),15,dtype=np.uint8)
        for x,code in ((32,0),(96,16),(160,32)):
            codes[64:112,x:x+48]=code; codes[144,x:x+48]=code
        fixture=Path(tmp)/'beam.bin'; fixture.write_bytes(codes.tobytes())
        for p in MAIN:
            metrics[p]=[]
            for phase,a in enumerate(capture(p,['--simulate-frame',fixture],size='3840x2160')):
                strokes=[]
                for x in (1110,1830,2550):
                    profile=(a[1240:1360,x-48:x+48]@np.array([.2126,.7152,.0722])).mean(1)
                    strokes.append(measure(profile))
                metrics[p].append(dict(phase=phase,strokes=strokes))
                if p==MAIN[0] and phase==0:
                    pic=display(a); panels=[]
                    for x,title,m in zip((1110,1830,2550),('Dark gray','Mid gray','White'),strokes):
                        tile=Image.new('RGB',(240,400),'#17191c');draw=ImageDraw.Draw(tile)
                        tile.paste(pic.crop((x-112,740,x+112,964)),(0,24))
                        tile.paste(pic.crop((x-112,1268,x+112,1340)),(0,272))
                        draw.text((8,4),title,fill='white',font=FONT)
                        draw.text((8,350),f"{m['fwhm_pixels']:.2f} px FWHM",fill='white',font=FONT)
                        draw.text((8,378),'Native pixels / phase 0',fill='white',font=FONT)
                        panels.append(tile)
                    sheet(panels,IMAGES/'showcase/4k/pvm-beam-levels.png',3)
    (ROOT/'docs/gpu-beam-measurements.json').write_text(json.dumps(metrics,indent=2)+'\n')


def motion(only_vhs=False):
    import imageio_ffmpeg
    ff = imageio_ffmpeg.get_ffmpeg_exe()
    manifest = json.loads((ROOT/'docs/showcase-captures.json').read_text())
    jobs = [(r['slug'].rsplit('-', 1)[0], r['slug'].rsplit('-', 1)[1], r['first_frame'],
             r['frames'], r.get('input_script'), r.get('start_button_frame'), 'showcase') for r in manifest]
    jobs += [('boss', p, 60, 240 if p in (MAIN[0], MAIN[3]) else 120, None, None, 'motion') for p in MAIN]
    jobs += [('contra-gameplay', p, 900, 120, None, 180, 'motion') for p in (MAIN[0], MAIN[3])]
    if only_vhs:
        jobs=[('boss','vhs_sp_consumer',60,240,None,None,'motion')]
    metrics_path=ROOT/'docs/gpu-motion-metrics.json'
    metrics = json.loads(metrics_path.read_text()) if only_vhs else {}
    for scene, p, frame, count, replay, start, folder in jobs:
        slug=scene+'-'+p; dest=IMAGES/folder
        source=['--simulate-frame', args.contra] if scene=='boss' else [args.roms/GAMES[scene]]
        encoder=subprocess.Popen([ff, '-y', '-loglevel', 'error', '-f', 'rawvideo',
            '-pixel_format', 'rgb24', '-video_size', '960x720', '-framerate', str(FPS),
            '-i', 'pipe:0', '-an', '-c:v', 'libx264', '-preset', 'fast', '-crf', '16',
            '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(dest/f'{slug}.mp4')], stdin=subprocess.PIPE)
        excerpt=[]; means=[]; diffs=[]; previous=None
        gif_offset=100 if scene=='darkwing' else 120 if scene=='little-samson' else 0
        try:
            for n, a in enumerate(capture(p, source, frame, '960x720', count,
                                        Path(replay).name if replay else None, start)):
                pic=display(a); encoder.stdin.write(pic.tobytes())
                means.append(a.mean((0,1)).tolist())
                if previous is not None: diffs.append(float(np.sqrt(np.mean((a-previous)**2))))
                previous=a.copy()
                if gif_offset <= n < gif_offset+24: excerpt.append(pic)
            encoder.stdin.close(); assert encoder.wait()==0
        finally:
            if encoder.poll() is None: encoder.kill(); encoder.wait()
        metrics[slug]=dict(source_frames=list(range(frame,frame+count)), playback_fps=FPS,
                           seconds=count/FPS, adjacent_linear_rms=diffs, frame_mean_rgb=means)
        if folder=='showcase' or (scene=='boss' and p in (MAIN[0], MAIN[3], 'vhs_sp_consumer')):
            small=[f.resize((640,480),Image.Resampling.LANCZOS) for f in excerpt]
            palette_source=Image.new('RGB',(640,480*len(small)))
            for i,pic in enumerate(small): palette_source.paste(pic,(0,i*480))
            palette=palette_source.quantize(colors=256,method=Image.Quantize.MEDIANCUT)
            indexed=[f.quantize(palette=palette,dither=Image.Dither.NONE) for f in small]
            indexed[0].save(dest/f'{slug}.gif',save_all=True,append_images=indexed[1:],
                            duration=20,loop=0,disposal=1,optimize=False)
            if scene in ('kirby','boss'):
                crops=[f.crop((288,160,672,416)) for f in excerpt]
                path=dest/('kirby-phase-detail.webp' if scene=='kirby' else f'{slug}-detail.webp')
                durations=[round((i+1)*1000/FPS)-round(i*1000/FPS) for i in range(len(crops))]
                crops[0].save(path,save_all=True,append_images=crops[1:],duration=durations,
                              lossless=True,method=4,loop=0)
        for row in manifest:
            if row['slug']==slug:
                row.update(exposure=EXPOSURE, gif_first_frame=frame+gif_offset,
                           gif_source='Consecutive rendered frames; fixed GIF palette')
    (ROOT/'docs/showcase-captures.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (ROOT/'docs/gpu-motion-metrics.json').write_text(json.dumps(metrics,indent=2)+'\n')
    if only_vhs: return
    with tempfile.TemporaryDirectory(prefix='mynes-concat-') as tmp:
        listing=Path(tmp)/'clips.txt'
        listing.write_text(''.join("file '"+str(IMAGES/'showcase'/(r['slug']+'.mp4'))+"'\n" for r in manifest))
        subprocess.run([ff,'-y','-loglevel','error','-f','concat','-safe','0','-i',str(listing),
                        '-c','copy','-movflags','+faststart',str(IMAGES/'showcase/showcase-reel.mp4')],check=True)
    subprocess.run([ff,'-y','-loglevel','error','-i',str(IMAGES/'motion/boss-sony_pvm_14l2.mp4'),
        '-i',str(IMAGES/'motion/boss-stass_favourite.mp4'),'-filter_complex','hstack=inputs=2',
        '-c:v','libx264','-crf','16','-pix_fmt','yuv420p','-movflags','+faststart',
        str(IMAGES/'showcase/sony-vs-rf.mp4')],check=True)


def vhs_motion():
    motion(only_vhs=True)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,default=ROOT/'build/bin/mynes_gpu')
    parser.add_argument('--roms',type=Path,required=True)
    parser.add_argument('--mario',type=Path,required=True)
    parser.add_argument('--contra',type=Path,required=True,help='256x240 PPU-code fixture')
    parser.add_argument('--logs',type=Path,required=True)
    parser.add_argument('--sections',nargs='+',choices=['gallery','gameplay','supporting','motion','vhs_motion','beam'],
                        default=['gallery','gameplay','supporting','motion','vhs_motion','beam'])
    args=parser.parse_args(); args.binary=args.binary.resolve(); args.logs.mkdir(parents=True,exist_ok=True)
    font=Path('/System/Library/Fonts/Helvetica.ttc')
    FONT=ImageFont.truetype(str(font),16) if font.exists() else ImageFont.load_default()
    records=[]
    (IMAGES/'showcase/4k').mkdir(parents=True,exist_ok=True)
    for section in args.sections:
        globals()[section]()
        evidence=dict(exposure=EXPOSURE, offscreen_headroom=1.6, mask_alignment='pixels',
                      frame_blending=False, captures=records,
                      shader_sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
                                     for p in sorted((ROOT/'frontends/gpu/shaders').rglob('*.glsl'))})
        (args.logs/'captures.json').write_text(json.dumps(evidence,indent=2)+'\n')
