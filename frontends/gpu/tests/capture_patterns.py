#!/usr/bin/env python3
"""Render PPU-code charts or a game through real presets for visual review.
Usage: python3 frontends/gpu/tests/capture_patterns.py build/bin/mynes_gpu /tmp/crt-review
Outputs final-render PPM + linear PFM, offscreen and silent by default.
"""
import os
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary",type=Path)
parser.add_argument("out",type=Path)
parser.add_argument("presets",nargs="*")
parser.add_argument("--all",action="store_true")
parser.add_argument("--rom",type=Path)
parser.add_argument("--frame",type=int,default=180)
parser.add_argument("--native-fullscreen",action="store_true")
parser.add_argument("--onscreen",action="store_true")
parser.add_argument("--size",default="2560x1664",help="offscreen drawable pixels")
parser.add_argument("--mask-alignment",choices=["pixels","physical"],default="pixels")
parser.add_argument("--window-size",default="1280x960")
parser.add_argument("--codes",type=Path,help="optional 256x240 PPU-code fixture")
args=parser.parse_args()
binary = args.binary.resolve()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
presets=args.presets or ["sony_pvm_14l2", "jvc_d_series_2000", "toshiba_14af43", "stass_favourite"]
if args.all: presets=[p.stem for p in sorted((root/"presets").glob("*.json"))]
pattern = bytearray(256 * 240)
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
        pattern[y * 256 + x] = code
with tempfile.TemporaryDirectory(prefix="mynes-chart-") as tmp:
    source = Path(tmp) / "chart.bin"
    source.write_bytes(pattern)
    for preset in presets:
        capture = out / (preset + ".ppm")
        env = dict(os.environ, XDG_CONFIG_HOME=tmp)
        with (out / (preset + ".log")).open("w") as log:
            input_args=[str(args.rom.resolve())] if args.rom else ["--simulate-frame",str(args.codes.resolve() if args.codes else source)]
            host_args=["--mask-alignment",args.mask_alignment,"--window-size",args.window_size]
            if not args.onscreen: host_args += ["--offscreen",args.size]
            elif args.native_fullscreen: host_args.append("--native-fullscreen")
            subprocess.run([str(binary), *input_args, *host_args,
                "--preset", "presets/" + preset + ".json", "--screenshot-after", str(args.frame),
                "--screenshot-pair", "--screenshot-path", str(capture)], cwd=root, env=env, stdout=log,
                stderr=log, check=True, timeout=60)
        assert capture.exists(), "Missing final CRT capture: " + preset
        print(capture, flush=True)
