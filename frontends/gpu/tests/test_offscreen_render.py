#!/usr/bin/env python3
"""The hidden window must not affect an explicitly sized CRT capture.

Usage: python3 frontends/gpu/tests/test_offscreen_render.py build/bin/mynes_gpu
Uses synthetic PPU codes, the real shaders, and isolated user configuration.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="mynes-offscreen-test-") as directory:
    tmp = Path(directory)
    frame = tmp / "frame.bin"
    codes = [(x // 16 + y // 16 * 7) % 64 for y in range(240) for x in range(256)]
    frame.write_bytes(struct.pack("<61440H", *codes))
    preset = json.loads((root / "presets/sony_pvm_14l2.json").read_text())
    preset["tv"]["halation"] = 0.4
    preset_path = tmp / "optical-test.json"
    preset_path.write_text(json.dumps(preset))
    for mode in ([], ["--sdr"]):
        reference = None
        for window in ("400x300", "1151x863"):
            capture = tmp / "frame.ppm"
            with tempfile.TemporaryDirectory(dir=tmp) as config:
                env = dict(os.environ, XDG_CONFIG_HOME=config, MYNES_REVIEW_NO_INPUT="1",
                           MYNES_OFFSCREEN_HEADROOM="4")
                result = subprocess.run(
                    [str(binary), "--simulate-frame", str(frame), "--preset", str(preset_path),
                     "--offscreen", "640x480", "--window-size", window,
                     "--mask-alignment", "pixels", "--screenshot-after", "4",
                     "--screenshot-path", str(capture), *mode],
                    cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, timeout=30)
                if result.returncode:
                    raise RuntimeError(result.stdout)
            image = capture.with_suffix(".ppm.linear.pfm").read_bytes()
            assert image.startswith(b"PF\n640 480\n"), "Wrong capture dimensions"
            if reference is not None:
                assert image == reference, "Hidden window changed offscreen CRT pixels"
            reference = image
    print("Offscreen CRT output is independent of hidden-window size: PASS")
