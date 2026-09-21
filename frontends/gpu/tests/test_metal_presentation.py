#!/usr/bin/env python3
"""Windowed Metal cadence review with GPU audio and a real composite decoder.

Keep the window visible. Measures Metal drawable timestamps, not photons.
Example: test_metal_presentation.py build/bin/mynes_gpu game.nes /tmp/review
"""
import argparse
import collections
import csv
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("rom", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--mode", choices=("hold", "60hz"), default="60hz")
parser.add_argument("--frames", type=int, default=1800)
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="mynes-metal-") as config:
    env = dict(os.environ, XDG_CONFIG_HOME=config, MYNES_METAL_PRESENT_TRACE="1",
               MYNES_PRESENT_TRACE=str(output / "submissions.csv"),
               MYNES_AUDIO_TRACE=str(output / "audio.csv"),
               MYNES_PLAYBACK_FRAMES=str(args.frames), MYNES_REVIEW_START_FRAME="90")
    for key in ("MYNES_GPU_AUDIO", "MYNES_PRESENT_STALL_MS", "MYNES_REVIEW_INPUT_SCRIPT",
                "MYNES_REVIEW_FRAME", "MYNES_REVIEW_PRESET"):
        env.pop(key, None)
    with (output / "run.log").open("w") as log:
        subprocess.run([str(args.binary.resolve()), str(args.rom.resolve()),
                        "--preset", str(root / "presets/reference_composite.json"),
                        "--presentation", args.mode], cwd=root, env=env,
                       stdout=log, stderr=log, timeout=args.frames/40+30, check=True)

pattern = r"MYNES_METAL_PRESENT (\d+) ([\d.]+) ([\d.]+) ([\d.]+) (\d+)"
rows = sorted((int(i), float(t), float(target), float(submit), int(source))
              for i, t, target, submit, source in re.findall(pattern, (output/"run.log").read_text()))[60:]
assert rows, "No Metal drawable timestamps; use the bundled SDL Metal backend"
# Zero timestamps mean the drawable was not shown (e.g. covered/minimized).
# Never call an occluded run a successful visible-cadence test.
pairs = [(a, b) for a, b in zip(rows, rows[1:]) if a[1] > 0 and b[1] > 0]
assert len(pairs) >= 120, "Too few visible frames; keep the window visible and retry"
intervals = [(b[1]-a[1])*1000 for a, b in pairs]
audio = list(csv.DictReader((output/"audio.csv").open()))
gpu_blocks = sum(int(r["gpu"]) for r in audio)
assert gpu_blocks > 0, "GPU audio was not exercised"
submissions = list(csv.DictReader((output/"submissions.csv").open()))
frames = [int(r["source_frame"]) for r in submissions]
assert all(b > a for a, b in zip(frames, frames[1:])), "Source frames repeated/reversed"
report = dict(mode=args.mode, visible_intervals=len(intervals),
              unshown_drawables=sum(r[1] == 0 for r in rows),
              median_ms=statistics.median(intervals), maximum_ms=max(intervals),
              interval_histogram_ms=dict(collections.Counter(round(t, 2) for t in intervals)),
              irregular_60hz_intervals=sum(abs(t-1000/60) > 1 for t in intervals) if args.mode == "60hz" else None,
              skipped_source_frames=sum(b-a-1 for a, b in zip(frames, frames[1:])),
              gpu_audio_blocks=gpu_blocks,
              scope="Metal presentedTime on visible drawables; not photodiode measurement")
(output/"results.json").write_text(json.dumps(report, indent=2)+"\n")
print(json.dumps(report, indent=2))
