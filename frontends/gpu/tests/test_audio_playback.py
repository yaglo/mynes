#!/usr/bin/env python3
"""Capture real game audio through both backends, with live SDL queue checks.
Usage: test_audio_playback.py build/bin/mynes_gpu game.nes /tmp/audio-review
Runs at normal emulator pacing, not as a throughput benchmark.
"""
import array
import csv
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import wave

binary, rom, output = (Path(arg).resolve() for arg in sys.argv[1:4])
root = Path(__file__).resolve().parents[3]
output.mkdir(parents=True, exist_ok=True)
recordings = []
for backend in ["cpu", "gpu"]:
    with tempfile.TemporaryDirectory(prefix="mynes-audio-") as config:
        raw = output / (backend + ".f32")
        trace = output / (backend + ".csv")
        env = dict(os.environ, XDG_CONFIG_HOME=config,
                   MYNES_AUDIO_CAPTURE=str(raw), MYNES_AUDIO_TRACE=str(trace))
        env.pop("MYNES_GPU_AUDIO", None)
        if backend == "gpu":
            env["MYNES_GPU_AUDIO"] = "1"
        with (output / (backend + ".log")).open("w") as log:
            subprocess.run([str(binary), "--preset", "presets/reference_composite.json",
                "--screenshot-after", "600", "--screenshot-path", str(output / (backend + ".ppm")),
                str(rom)], cwd=root, env=env, stdout=log, stderr=log, check=True, timeout=90)
        data = array.array("f", raw.read_bytes())
        assert len(data) > 400000, "Too few game samples"
        assert all(math.isfinite(x) and abs(x) <= 1 for x in data), "Invalid samples"
        rms = math.sqrt(sum(x*x for x in data) / len(data))
        assert rms > 0.003, "Unexpectedly silent game capture"
        with trace.open() as file:
            rows = list(csv.DictReader(file))
        assert sum(int(row["samples"]) for row in rows) == len(data)
        assert all(int(row["gpu"]) == (backend == "gpu") for row in rows), "Backend fell back"
        queued = [int(row["queued"]) + int(row["samples"]) for row in rows]
        assert min(queued) >= 0 and max(queued) <= 3528, "Stale audio queue"
        assert all(.99499 <= float(row["ratio"]) <= 1.00501 for row in rows)
        with wave.open(str(output / (backend + ".wav")), "wb") as wav:
            wav.setparams((1, 2, 44100, 0, "NONE", "not compressed"))
            wav.writeframes(array.array("h", (round(x*32767) for x in data)).tobytes())
        print(f"{backend}: {len(data)} samples, RMS {rms:.5f}, max queued {max(queued)/44.1:.1f} ms", flush=True)
        recordings.append(data)
assert len(recordings[0]) == len(recordings[1]), "Backend changed playback duration"
error = max(abs(a-b) for a,b in zip(*recordings))
print(f"CPU/GPU capture maximum difference: {error:.8f}")
assert error < .0002, "Game playback differs between backends"
