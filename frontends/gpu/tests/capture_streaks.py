#!/usr/bin/env python3
"""Render bright/dark recovery and Stas's Favourite through the full pipeline.
Usage: capture_streaks.py build/bin/mynes_gpu /tmp/streak-review [game.nes]
"""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[3]
binary, out=(Path(p).resolve() for p in sys.argv[1:3])
rom=Path(sys.argv[3]).resolve() if len(sys.argv)>3 else None
out.mkdir(parents=True,exist_ok=True)
pattern=bytearray([0x10])*(256*240)
for y in range(240):
    for x in range(256):
        if 40<=x<104 and 40<=y<80: pattern[y*256+x]=0x20
        if 40<=x<104 and 136<=y<176: pattern[y*256+x]=0x0f
with tempfile.TemporaryDirectory(prefix="mynes-streak-") as temp:
    config=Path(temp); directory=config/"mynes/presets"; directory.mkdir(parents=True)
    source=config/"pattern.bin"; source.write_bytes(pattern)
    base=json.loads((root/"presets/stass_favourite.json").read_text())
    for enabled in [False,True]:
        preset=copy.deepcopy(base)
        # Chart isolates the voltage recovery from geometry, noise, and mask.
        chart=copy.deepcopy(preset)
        for key in ["noise_level","mask_strength","halation","barrel","barrel_v","convergence_static",
                    "convergence_dynamic","conv_r_x","conv_r_y","conv_b_x","conv_b_y","hv_sag","focus_breathing"]:
            chart["tv"][key]=0
        for kind,p in [("chart",chart)]+([("mario",preset)] if rom else []):
            name=kind+("_recovery" if enabled else "_regulated")
            p["name"]=name
            if not enabled:
                for key in ["beam_current_load","video_black_droop","hv_sag","focus_breathing"]: p["tv"][key]=0
            path=directory/(name+".json"); path.write_text(json.dumps(p,indent=4))
            args=[str(binary),"--offscreen","2560x1664","--preset",str(path),"--screenshot-after","180","--screenshot-pair",
                  "--screenshot-path",str(out/(name+".ppm"))]
            args+= [str(rom)] if kind=="mario" else ["--simulate-frame",str(source)]
            with (out/(name+".log")).open("w") as log:
                subprocess.run(args,cwd=root,env=dict(os.environ,XDG_CONFIG_HOME=temp),stdout=log,stderr=log,check=True,timeout=60)
            print(name,flush=True)
