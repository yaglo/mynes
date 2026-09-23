#!/usr/bin/env python3
"""NES-001 +5 V ripple from the schematic: reservoir, 7805 rejection, console load.

Runs tools/circuits/nes001_psu.cir over the console's load current and the
regulator's ripple rejection, and reports the raw reservoir ripple and the
+5 V ripple (peak-to-peak and the 120/240/360 Hz components). The audio
jack hum follows from the 74HCU04 gate's supply gain in
golden/nes001_audio.h (about +9 dB) and the jack level scale.
"""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np


def run(deck, params, work):
    deck = re.sub(r"^\.param .*$", ".param " + " ".join(f"{k}={v}" for k, v in params.items()), deck, flags=re.M)
    (work / "model.cir").write_text(deck)
    out = subprocess.run(["ngspice", "-b", "model.cir"], cwd=work, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if out.returncode != 0 or not (work / "tran.txt").exists():
        raise RuntimeError(out.stdout[-3000:])
    return np.array([list(map(float, l.split())) for l in (work / "tran.txt").read_text().splitlines()[1:] if l.strip()])


def components(t, v, f0=120.0, n=3):
    tt = np.arange(t[0], t[-1], 10e-6)
    vv = np.interp(tt, t, v)
    vv = vv - vv.mean()
    cycles = math.floor((tt[-1] - tt[0]) * f0)
    keep = tt < tt[0] + cycles / f0
    tt, vv = tt[keep], vv[keep]
    return [abs(np.mean(vv * np.exp(-2j * math.pi * k * f0 * tt)) * 2) for k in range(1, n + 1)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--golden", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    deck = Path(__file__).with_name("nes001_psu.cir").read_text()
    results = []
    for i_load in (0.4, 0.6, 0.8):
        for rr in (62, 73):
            params = {"rr_db": rr, "i_load": i_load, "i_mod": 0.04, "r_load": 0.01, "r_src": 0.8, "vac": 9}
            with tempfile.TemporaryDirectory() as d:
                data = run(deck, params, Path(d))
            t, raw, five = data[:, 0], data[:, 1], data[:, 2]
            raw_pp = raw.max() - raw.min()
            five_pp = five.max() - five.min()
            h = components(t, five)
            r = {"i_load_a": i_load, "rr_db": rr, "raw_mean_v": float(raw.mean()), "raw_ripple_pp_v": float(raw_pp),
                 "five_ripple_pp_mv": float(five_pp * 1e3),
                 "five_120hz_mv": h[0] * 1e3, "five_240hz_mv": h[1] * 1e3, "five_360hz_mv": h[2] * 1e3}
            results.append(r)
            print(f"load {i_load:.1f} A, RR {rr} dB: raw {raw.mean():.2f} V, ripple {raw_pp:.2f} Vpp; +5 V ripple {five_pp*1e3:.2f} mVpp "
                  f"(120 Hz {h[0]*1e3:.2f}, 240 Hz {h[1]*1e3:.2f}, 360 Hz {h[2]*1e3:.2f} mV peak)")
    if args.golden:
        ref = [r for r in results if r["i_load_a"] == 0.6 and r["rr_db"] == 73][0]
        args.golden.mkdir(parents=True, exist_ok=True)
        (args.golden / "nes001_psu.json").write_text(json.dumps(ref, indent=2) + "\n")
    (args.output / "results.json").write_text(json.dumps({"model": "EDC NES-001 PSU trace; behavioural 7805; assumed transformer resistance and modulator load.",
                                                          "results": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
