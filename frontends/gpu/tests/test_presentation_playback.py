#!/usr/bin/env python3
"""Compare native and 60 Hz pacing without changing the game's signal phase.

Usage: test_presentation_playback.py binary game.nes output-directory
Runs two 15-second offscreen playbacks; measures submission, not panel scanout.
"""
import csv
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

binary, rom, output = (Path(arg).resolve() for arg in sys.argv[1:4])
root = Path(__file__).resolve().parents[3]
output.mkdir(parents=True, exist_ok=True)
reports, phases = {}, {}
for mode in ("hold", "60hz"):
    trace, audio = output / f"{mode}.csv", output / f"{mode}-audio.csv"
    with tempfile.TemporaryDirectory(prefix="mynes-pacing-") as config:
        env = dict(os.environ, XDG_CONFIG_HOME=config, MYNES_PRESENT_TRACE=str(trace),
                   MYNES_AUDIO_TRACE=str(audio), MYNES_REVIEW_START_FRAME="90",
                   MYNES_PLAYBACK_FRAMES="900")
        for key in ("MYNES_GPU_AUDIO", "MYNES_PRESENT_STALL_MS", "MYNES_REVIEW_INPUT_SCRIPT",
                    "MYNES_REVIEW_FRAME", "MYNES_REVIEW_PRESET"):
            env.pop(key, None)
        with (output / f"{mode}.log").open("w") as log:
            subprocess.run([str(binary), str(rom), "--offscreen", "1280x960",
                            "--preset", "presets/sony_pvm_14l2.json", "--presentation", mode],
                           cwd=root, env=env, stdout=log, stderr=log, check=True, timeout=60)
    rows = list(csv.DictReader(trace.open()))
    phases[mode] = {int(r["source_frame"]): int(r["source_phase"]) for r in rows}
    rows = rows[60:]
    assert len(rows) > 600, "Insufficient frames for pacing review"
    assert all(int(r["slot"]) == 0 and int(r["slots"]) == 1 for r in rows)
    times = [int(r["submit_ns"]) for r in rows]
    intervals = sorted((b-a)/1e6 for a, b in zip(times, times[1:]))
    frames = [int(r["source_frame"]) for r in rows]
    assert all(b > a for a, b in zip(frames, frames[1:])), "Repeated or reversed source frame"
    audio_rows = list(csv.DictReader(audio.open()))
    queues = [(int(r["queued"])+int(r["samples"]))/44.1 for r in audio_rows]
    assert max(queues) <= 80, "Stale audio queue"
    reports[mode] = dict(
        submissions_per_second=(len(times)-1)*1e9/(times[-1]-times[0]),
        median_ms=statistics.median(intervals), p95_ms=intervals[int(.95*(len(intervals)-1))],
        maximum_ms=max(intervals), skipped_source_frames=sum(b-a-1 for a, b in zip(frames, frames[1:])),
        max_audio_queue_ms=max(queues), gpu_audio_blocks=sum(int(r["gpu"]) for r in audio_rows))
    print(mode, reports[mode], flush=True)

common = phases["hold"].keys() & phases["60hz"].keys()
assert len(common) > 600
assert all(phases["hold"][n] == phases["60hz"][n] for n in common), "Pacing changed source phase"
reports["matching_source_phases"] = len(common)
reports["scope"] = "Offscreen submission timing and actual PPU phase; not physical display scanout."
(output / "results.json").write_text(json.dumps(reports, indent=2)+"\n")
print(f"Unmodified carrier phase confirmed on {len(common)} matching source frames")
