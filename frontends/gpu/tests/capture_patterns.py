#!/usr/bin/env python3
"""Render PPU-code charts through real presets for visual review (not timing).
Usage: python3 frontends/gpu/tests/capture_patterns.py build/bin/mynes_gpu /tmp/crt-review
Outputs SDR PPM captures; EDR highlights above reference white are clipped.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[3]
binary = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
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
    for preset in ["reference_composite", "studio_pvm", "living_room_1988"]:
        capture = Path(tmp) / (preset + ".ppm")
        env = dict(os.environ, XDG_CONFIG_HOME=tmp, MYNES_CAPTURE_PATH=str(capture))
        with (out / (preset + ".log")).open("w") as log:
            process = subprocess.Popen([str(binary), "--simulate-frame", str(source),
                "--preset", "presets/" + preset + ".json"], cwd=root, env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 60
                previous_size = 0
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("Renderer exited: " + preset)
                    size = capture.stat().st_size if capture.exists() else 0
                    if size and size == previous_size:
                        break
                    previous_size = size
                    time.sleep(0.25)
                else:
                    raise TimeoutError("Missing capture: " + preset)
                (out / capture.name).write_bytes(capture.read_bytes())
                print(out / capture.name, flush=True)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
