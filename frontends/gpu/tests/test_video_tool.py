#!/usr/bin/env python3
"""mynes_video end to end: a short video with sound and a still picture
through two presets, checked with ffprobe and on their pixels.

Usage: python3 frontends/gpu/tests/test_video_tool.py build/bin/mynes_video
Exits 77 (skipped) when ffmpeg or ffprobe is missing. Uses the real shaders
and an isolated user configuration.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RATE = 60.0988
SKIP = 77


def probe(path, entries):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", entries, "-of", "json", str(path)],
                         check=True, stdout=subprocess.PIPE, text=True).stdout
    return json.loads(out)


def frame_mean(path, n):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-vf", f"select=eq(n\\,{n})",
                          "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                         check=True, stdout=subprocess.PIPE).stdout
    return sum(raw) / len(raw), raw


def main():
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        print("ffmpeg or ffprobe not found: skipped")
        return SKIP
    root = Path(__file__).resolve().parents[3]
    binary = Path(sys.argv[1]).resolve()
    failures = []
    with tempfile.TemporaryDirectory(prefix="mynes-video-test-") as directory:
        tmp = Path(directory)
        clip, still = tmp / "bars.mp4", tmp / "still.png"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "smptehdbars=size=640x360:rate=30",
                        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "1",
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", str(clip)], check=True)
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=800x600",
                        "-frames:v", "1", str(still)], check=True)
        env = dict(os.environ, XDG_CONFIG_HOME=str(tmp / "config"))
        seconds, frames = 0.25, round(0.25 * RATE)
        for source, stem in ((clip, "clip"), (still, "still")):
            out = tmp / f"{stem}.mov"
            result = subprocess.run([str(binary), "--preset", "sony_pvm_14l2", "--preset", "vhs_sp_consumer",
                                     "--size", "320x240", "--seconds", str(seconds), "--settle", "4",
                                     str(source), str(out)],
                                    cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=180)
            if result.returncode:
                print(result.stdout)
                failures.append(f"{stem}: exit {result.returncode}")
                continue
            means = {}
            for preset in ("sony_pvm_14l2", "vhs_sp_consumer"):
                path = tmp / f"{stem}-{preset}.mov"
                if not path.exists():
                    failures.append(f"{path.name} missing")
                    continue
                streams = probe(path, "stream=codec_type,codec_name,nb_frames,width,height,color_transfer")["streams"]
                video = [s for s in streams if s["codec_type"] == "video"]
                audio = [s for s in streams if s["codec_type"] == "audio"]
                if not video or int(video[0]["nb_frames"]) != frames:
                    failures.append(f"{path.name}: {video[0].get('nb_frames') if video else 'no'} video frames, want {frames}")
                if video and (video[0]["width"], video[0]["height"]) != (320, 240):
                    failures.append(f"{path.name}: size {video[0]['width']}x{video[0]['height']}")
                if video and video[0].get("color_transfer") != "iec61966-2-1":
                    failures.append(f"{path.name}: transfer {video[0].get('color_transfer')}")
                if stem == "clip" and not audio:
                    failures.append(f"{path.name}: the input's sound is missing")
                mean, raw = frame_mean(path, frames - 1)
                means[preset] = raw
                if not 20 < mean < 235:
                    failures.append(f"{path.name}: mean level {mean:.1f} of 255")
                print(f"{path.name}: {video[0]['nb_frames']} frames, {len(audio)} audio stream(s), mean {mean:.1f}")
            if len(means) == 2:
                a, b = means["sony_pvm_14l2"], means["vhs_sp_consumer"]
                diff = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
                print(f"{stem}: mean difference between the two presets {diff:.1f} of 255")
                if diff < 2:
                    failures.append(f"{stem}: the VHS preset left the picture unchanged")
    for f in failures:
        print("FAIL", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
