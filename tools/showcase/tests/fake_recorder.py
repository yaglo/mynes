"""A stand-in for build/bin/mynes_gpu's recorder, for the record-stage tests.

It takes the recorder's arguments, refuses paths it cannot open from its
working directory (as the real recorder does), and writes OUT.mov with
ffmpeg's testsrc2 at the requested size and frame count plus the OUT.json
sidecar: libx264 yuv444p for SDR, ProRes 4444 tagged BT.2020 PQ for HDR,
or whatever MYNES_RECORD_CODEC_ARGS / MYNES_RECORD_HDR_CODEC_ARGS say, as
the real recorder reads them. Each run appends a JSON line (cwd, argv,
environment) to FAKE_RECORDER_LOG. FAKE_RECORDER_FRAMES overrides the frame
count, FAKE_RECORDER_CODEC_ARGS the codec arguments (a recorder that
ignores the pipeline's environment) and FAKE_RECORDER_EXIT the exit status,
after the file is written."""
import json
import math
import os
import shlex
import subprocess
import sys

SDR_CODEC = "-c:v libx264 -preset ultrafast -crf 12 -pix_fmt yuv444p"
HDR_CODEC = "-c:v prores_ks -profile:v 4 -pix_fmt yuv444p10le -vendor apl0"
HDR_TAGS = ["-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
            "-color_range", "tv"]
HDR_PARAMS = "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv"
FLAGS = {"--sdr", "--record-hdr"}


def main(argv):
    opts, rest, i = {}, [], 0
    while i < len(argv):
        a = argv[i]
        if a in FLAGS:
            opts[a] = True
            i += 1
        elif a.startswith("--"):
            opts[a] = argv[i + 1]
            i += 2
        else:
            rest.append(a)
            i += 1
    log = os.environ.get("FAKE_RECORDER_LOG")
    if log:
        with open(log, "a") as f:
            f.write(json.dumps({"cwd": os.getcwd(), "argv": argv,
                                "env": {k: v for k, v in os.environ.items() if k.startswith("MYNES_")}}) + "\n")
    rom = rest[-1] if rest else ""
    for path in [rom, opts.get("--load-state"), opts.get("--preset"), opts.get("--input-replay")]:
        if path is not None and not os.path.exists(path):
            print(f"fake recorder: cannot open {path}", file=sys.stderr)
            return 2
    hdr = bool(opts.get("--record-hdr"))
    out = opts["--record"]
    w, h = (int(v) for v in opts["--offscreen"].split("x"))
    frames = int(math.floor(float(opts["--record-seconds"]) * 60.0988 + 0.5))
    frames = int(os.environ.get("FAKE_RECORDER_FRAMES") or frames)
    codec = (os.environ.get("FAKE_RECORDER_CODEC_ARGS")
             or os.environ.get("MYNES_RECORD_HDR_CODEC_ARGS" if hdr else "MYNES_RECORD_CODEC_ARGS")
             or (HDR_CODEC if hdr else SDR_CODEC))
    cmd = [os.environ.get("MYNES_FFMPEG", "ffmpeg"), "-y", "-v", "error", "-f", "lavfi",
           "-i", f"testsrc2=size={w}x{h}:rate=60.0988", "-frames:v", str(frames),
           *(["-vf", HDR_PARAMS] if hdr else []), *shlex.split(codec), *(HDR_TAGS if hdr else []), out]
    if subprocess.run(cmd).returncode:
        return 1
    sidecar = {"frames": frames, "rate": 60.0988, "width": w, "height": h, "hdr": hdr,
               "white_nits": float(opts.get("--record-hdr-white", 100)) if hdr else 100,
               "headroom": float(opts.get("--record-headroom", 1)) if hdr else 1}
    if hdr:
        sidecar.update(max_cll=812, max_fall=50)
    with open(os.path.splitext(out)[0] + ".json", "w") as f:
        json.dump(sidecar, f)
    return int(os.environ.get("FAKE_RECORDER_EXIT") or 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
