#!/usr/bin/env python3
"""Short sequential benchmarks of actual presets, with contention metadata.
Usage: benchmark_pipeline.py build/bin/mynes_gpu /tmp/gpu-bench
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary",type=Path)
parser.add_argument("output",type=Path)
parser.add_argument("presets",nargs="*")
parser.add_argument("--all",action="store_true")
parser.add_argument("--mask-alignment",choices=["pixels","physical"],default="pixels")
parser.add_argument("--recompute-geometry",action="store_true",help="A/B check: regenerate even fixed deflection maps")
args=parser.parse_args()
binary,output=args.binary.resolve(),args.output.resolve()
presets=args.presets or ["sony_pvm_14l2", "jvc_d_series_2000", "toshiba_14af43", "stass_favourite"]
if args.all:presets=[p.stem for p in sorted((root/"presets").glob("*.json"))]
output.mkdir(parents=True, exist_ok=True)
def running_frontends():
    try:
        rows=subprocess.check_output(["ps","-axo","pid=,comm="],text=True).splitlines()
        return [int(parts[0]) for row in rows
                if len(parts:=row.split(None,1))==2 and Path(parts[1]).name=="mynes_gpu"]
    except (OSError,subprocess.CalledProcessError):
        return None


report = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "platform": platform.platform(), "cpu": platform.processor(),
          "recompute_geometry":args.recompute_geometry,
          "mask_alignment":args.mask_alignment,"panel_scale":1,"load_before": os.getloadavg(), "other_frontends_before": running_frontends(), "results": []}
if sys.platform == "darwin":
    report["hardware"] = subprocess.check_output(
        ["sysctl", "hw.model", "hw.memsize", "machdep.cpu.brand_string"], text=True).strip()
pattern = re.compile(r"BENCH (\d+)x(\d+) mean_ms=([\d.]+) median_ms=([\d.]+) p95_ms=([\d.]+) max_ms=([\d.]+) equivalent_fps=([\d.]+)")
with tempfile.TemporaryDirectory(prefix="mynes-bench-") as config:
    for preset in presets:
        env = dict(os.environ, XDG_CONFIG_HOME=config)
        env.pop("MYNES_GPU_VALIDATION", None)
        env.pop("MYNES_BENCH_RECOMPUTE_GEOMETRY", None)
        if args.recompute_geometry: env["MYNES_BENCH_RECOMPUTE_GEOMETRY"]="1"
        result = subprocess.run([str(binary), "--benchmark", "--mask-alignment",args.mask_alignment,"--preset", "presets/"+preset+".json"],
            cwd=root, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        (output / (preset+".log")).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(result.stdout)
        rows = pattern.findall(result.stdout)
        assert len(rows) == 4, result.stdout
        for width, height, *values in rows:
            entry = dict(zip(["mean_ms", "median_ms", "p95_ms", "max_ms", "equivalent_fps"], map(float, values)))
            entry.update(preset=preset, width=int(width), height=int(height))
            report["results"].append(entry)
            print(f"{preset:22s} {width}x{height}: median {entry['median_ms']:.3f} ms, p95 {entry['p95_ms']:.3f} ms", flush=True)
report["load_after"] = os.getloadavg()
report["other_frontends_after"] = running_frontends()
report["metric"] = "CPU submission to final GPU fence, one frame in flight; actual preset, complete DAC/receiver/CRT/mask/glass; no emulation, audio, vsync, capture, or validation"
(output / "results.json").write_text(json.dumps(report, indent=2)+"\n")
