#!/usr/bin/env python3
"""Verify the host reflection switch through real 4K rendering, with isolated config.

Usage: python3 frontends/gpu/tests/test_room_reflections.py build/bin/mynes_gpu
No ROM or Python imaging dependencies required. Checks migration defaults, saved
preferences, both CLI overrides, and retention of intrinsic CRT optics.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="mynes-reflections-test-") as directory:
    tmp = Path(directory)
    frame = tmp / "frame.bin"
    codes = [0x30 if 80 <= x < 176 and 70 <= y < 170 else 0x0f
             for y in range(240) for x in range(256)]
    frame.write_bytes(struct.pack("<61440H", *codes))
    preset = json.loads((root / "presets/famicom_kitchen.json").read_text())
    preset["tv"].update(glass_glare=0.04, ambient_light=0.03,
                        halation=0.3, glass_reflection=0.2)

    def render(name, settings, preference=None, flags=()):
        config = tmp / name
        (config / "mynes").mkdir(parents=True)
        # Existing installations have no room preference: those migrate to OFF.
        saved = {"gpu_mask_alignment": 1}
        if preference is not None:
            saved["gpu_room_reflections"] = preference
        (config / "mynes/config.json").write_text(json.dumps(saved, indent=2))
        settings_path = config / "preset.json"
        settings_path.write_text(json.dumps(settings))
        capture = config / "frame.ppm"
        env = dict(os.environ, XDG_CONFIG_HOME=str(config), MYNES_REVIEW_NO_INPUT="1")
        result = subprocess.run(
            [str(binary), "--simulate-frame", str(frame), "--preset", str(settings_path),
             "--offscreen", "3840x2160", "--sdr", "--mask-alignment", "physical",
             "--screenshot-after", "4", "--screenshot-path", str(capture), *flags],
            cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout)
        image = Path(str(capture) + ".linear.pfm").read_bytes()
        assert image.startswith(b"PF\n3840 2160\n"), "Wrong capture dimensions"
        # Runtime/CLI switches must never rewrite the preset's configured strengths.
        assert json.loads(settings_path.read_text()) == settings
        persisted = json.loads((config / "mynes/config.json").read_text())
        assert persisted.get("gpu_room_reflections", 0) == (preference or 0)
        return hashlib.sha256(image).hexdigest()

    default = render("default", preset)
    on = render("on", preset, 0, ["--room-reflections"])
    assert on != default, "Room light toggle has no effect"
    assert render("saved-on", preset, 1) == on, "Saved preference was ignored"
    assert render("override-off", preset, 1, ["--no-room-reflections"]) == default
    preset["tv"].update(glass_glare=0, ambient_light=0)
    assert render("zero-room", preset, 1) == default, "Toggle changed intrinsic CRT optics"
    preset["tv"].update(halation=0, glass_reflection=0)
    assert render("zero-scatter", preset, 1) != default, "Intrinsic scattering was lost"
    print("4K room reflections: defaults, persistence, overrides and intrinsic optics PASS")
