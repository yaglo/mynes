"""Render every shipped preset at UHD with matched signal, beam and game inputs.

Requires Pillow and NumPy. Captures stay outside the repository by default.
Full-resolution PNGs are unaveraged; overview images are only for navigation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]


def settings_hash(preset):
    rendering = {k: v for k, v in preset.items() if k not in ('name', 'description')}
    return hashlib.sha256(json.dumps(rendering, sort_keys=True).encode()).hexdigest()


def read_pfm(path):
    with path.open('rb') as stream:
        assert stream.readline().strip() == b'PF'
        width, height = map(int, stream.readline().split())
        assert float(stream.readline()) == -1
        return np.frombuffer(stream.read(), '<f4').reshape(height, width, 3)[::-1]


def charts():
    chart = np.zeros((240, 256), dtype=np.uint8)
    beam = np.full_like(chart, 0x0f)
    focus = np.full_like(chart, 0x0f)
    gray = [0x0f, 0x0d, 0x2d, 0x3d, 0x00, 0x10, 0x20, 0x30]
    for y in range(240):
        for x in range(256):
            if y < 48:
                code = gray[x // 32]
            elif y < 176:
                code = ((y - 48) // 32) * 16 + x // 16
            elif y < 208:
                code = 0x20 if (x // (1 + (y - 176) // 8)) % 2 else 0x0f
            else:
                code = 0x20 if 40 < x < 216 and (y % 8 == 0 or x % 8 == 0) else 0x0f
            chart[y, x] = code
            if y in (40, 80, 120, 160, 200) and 8 < x < 248:
                beam[y, x] = [0x00, 0x10, 0x20, 0x16, 0x1a, 0x12][min(x // 43, 5)]
    for cy in (32, 120, 208):
        for cx in (32, 128, 224):
            focus[cy-8:cy+9, cx-8:cx+9] = 0x10
            focus[cy-1:cy+2, cx-6:cx+7] = 0x20
            focus[cy-6:cy+7, cx-1:cx+2] = 0x20
    return {'chart': chart.tobytes(), 'beam': beam.tobytes(), 'focus': focus.tobytes()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/bin/mynes_gpu')
    parser.add_argument('--out', type=Path, default=Path('/tmp/mynes-preset-audit-4k'))
    parser.add_argument('--game-codes', type=Path, required=True)
    parser.add_argument('--kinds', nargs='+', choices=['chart', 'beam', 'game', 'focus'],
                        default=['chart', 'beam', 'game'])
    parser.add_argument('--presets', nargs='+', help='Optional preset filenames without .json')
    parser.add_argument('--publish', type=Path,
                        help='Export lossless native crops, navigation sheets and manifest to a docs directory')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    inputs = charts()
    inputs['game'] = args.game_codes.read_bytes()
    if len(inputs['game']) != 256 * 240:
        raise ValueError('Expected a 256x240 palette-index game frame')
    inputs = {k: v for k, v in inputs.items() if k in args.kinds}
    library = sorted((ROOT / 'presets').glob('*.json'))
    presets = library
    if args.presets:
        unknown = set(args.presets) - {p.stem for p in library}
        if unknown:
            parser.error('Unknown presets: ' + ', '.join(sorted(unknown)))
        presets = [p for p in presets if p.stem in args.presets]
    report_path = out / 'report.json'
    report = json.loads(report_path.read_text()) if report_path.exists() else {
        'size': [3840, 2160], 'output': 'SDR', 'mask_alignment': 'physical',
        'frames': [30, 31, 32], 'inputs_sha256': {}, 'presets': {}}
    for kind, data in inputs.items():
        digest = hashlib.sha256(data).hexdigest()
        previous = report['inputs_sha256'].get(kind)
        if previous and previous != digest:
            parser.error(f'{kind} input changed; use a new output directory')
        report['inputs_sha256'][kind] = digest
    with tempfile.TemporaryDirectory(prefix='mynes-audit-') as tmp:
        env = dict(os.environ, XDG_CONFIG_HOME=tmp, MYNES_REVIEW_NO_INPUT='1')
        for kind, data in inputs.items():
            source = Path(tmp) / (kind + '.raw')
            source.write_bytes(data)
            dest = out / kind
            dest.mkdir(exist_ok=True)
            for preset in presets:
                name = preset.stem
                current = json.loads(preset.read_text())
                entry = report['presets'].setdefault(name, {'settings': current})
                # Preserve each input's old tuning when only one kind is
                # refreshed. A partial capture must not certify stale images.
                entry.setdefault('rendered_settings_sha256', {
                    k: settings_hash(entry['settings']) for k in report['inputs_sha256']
                    if (out / k / f'{name}-0.png').exists()})
                capture = dest / (name + '.ppm')
                with (dest / (name + '.log')).open('w') as log:
                    subprocess.run([str(args.binary.resolve()), '--simulate-frame', str(source),
                        '--preset', str(preset), '--offscreen', '3840x2160', '--sdr',
                        '--mask-alignment', 'physical', '--screenshot-after', '30',
                        '--screenshot-frames', '3', '--screenshot-path', str(capture)],
                        cwd=ROOT, env=env, stdout=log, stderr=log, check=True, timeout=90)
                files = [capture] + [Path(str(capture) + f'.frame-{i:03d}.ppm') for i in (1, 2)]
                entry['settings'] = current
                entry['rendered_settings_sha256'][kind] = settings_hash(current)
                if kind == 'chart':
                    # Interior of uniform black and grey patches. A common
                    # 4:3 game area occupies x480..3359, y0..2159 even in FW900.
                    a = read_pfm(Path(str(files[0]) + '.linear.pfm'))
                    b = read_pfm(Path(str(files[2]) + '.linear.pfm'))
                    weights = np.array([.2126, .7152, .0722])
                    entry['patches'] = {}
                    for label, x0 in [('black_0f', 600), ('below_black_0d', 960),
                                      ('grey_10', 2400), ('white_20', 2760)]:
                        roi = (slice(180, 270), slice(x0, x0 + 120))
                        light = a[roi] @ weights
                        diff = (b[roi] - a[roi]) @ weights
                        entry['patches'][label] = {'roi_xyxy': [x0, 180, x0 + 120, 270],
                            'mean_linear_Y': float(light.mean()),
                            'same_phase_difference_rms_Y': float(np.sqrt(np.mean(diff * diff)))}
                    del a, b
                for i, path in enumerate(files):
                    with Image.open(path) as im:
                        im.save(dest / f'{name}-{i}.png', compress_level=1)
                    path.unlink()
                    Path(str(path) + '.linear.pfm').unlink()
                (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
                print(kind, name, flush=True)
    for kind in inputs:
        available = [p for p in library if (out / kind / f'{p.stem}-0.png').exists()]
        for page in range((len(available) + 5) // 6):
            sheet = Image.new('RGB', (1440, 1000), '#181818')
            draw = ImageDraw.Draw(sheet)
            for j, preset in enumerate(available[page * 6:page * 6 + 6]):
                x, y = (j % 3) * 480, (j // 3) * 500
                with Image.open(out / kind / f'{preset.stem}-0.png') as im:
                    sheet.paste(im.resize((480, 270), Image.Resampling.LANCZOS), (x, y + 24))
                    # Native pixels, no resampling: middle horizontal stroke
                    # for beam, central detail otherwise.
                    crop = im.crop((1680, 1000, 2160, 1160))
                    sheet.paste(crop, (x, y + 318))
                draw.text((x + 6, y + 5), preset.stem, fill='white')
                draw.text((x + 6, y + 298), 'Native pixels (1:1)', fill='#aaaaaa')
            sheet.save(out / f'{kind}-{page + 1}.jpg', quality=94)
    if args.publish:
        docs = args.publish.resolve()
        images = docs / 'images/preset-audit-4k'
        images.mkdir(parents=True, exist_ok=True)
        for preset in library:
            entry = report['presets'][preset.stem]
            current = json.loads(preset.read_text())
            if settings_hash(current) != settings_hash(entry['settings']):
                raise ValueError(f'Render {preset.stem} again before publishing changed settings')
            entry['settings'] = current
            for kind in ('game', 'chart', 'beam'):
                digest = entry.get('rendered_settings_sha256', {}).get(kind, settings_hash(entry['settings']))
                if digest != settings_hash(current):
                    raise ValueError(f'Render {preset.stem}/{kind} again before publishing changed settings')
                source = out / kind / f'{preset.stem}-0.png'
                with Image.open(source) as im:
                    im.crop((1680, 1000, 2160, 1160)).save(images / f'{preset.stem}-{kind}-native.png')
        for sheet in out.glob('game-*.jpg'):
            shutil.copy2(sheet, images / sheet.name)
        report['renderer_commit'] = subprocess.check_output(
            ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        report['native_crop_xyxy'] = [1680, 1000, 2160, 1160]
        (docs / 'preset-audit-4k.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
