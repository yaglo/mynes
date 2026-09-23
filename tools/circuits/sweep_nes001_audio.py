#!/usr/bin/env python3
"""NES-001 APU-to-jack audio response, supply gain and distortion from the schematic.

Runs tools/circuits/nes001_audio.cir (74HCU04 gate as a MOS pair fitted to
its data sheet, AV module follower) in ngspice:
  1. .ac from the pulse and the TND pin into 10k, 47k and 1M TV inputs:
     response relative to 1 kHz and the -3 dB corners.
  2. .ac from VCC: the gate's supply-to-jack gain, for PSU ripple.
  3. .tran 1 kHz sines at the APU's 0.3 V pin swing (uXe, lidnariq, nesdev
     thread 56) and twice that: jack level, THD, second and third harmonics.
Writes a golden file for the audio chain test (pulse pin, 47k input).
Compare with rainwarrior's hardware sweep (about 16 Hz, nesdev thread 17745).
"""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np

TEST_HZ = [10, 16, 20, 30, 50, 100, 200, 500, 1000, 2000, 5000, 8000, 10000, 12000, 15000, 20000]


def run(deck, params, work):
    deck = re.sub(r"^\.param .*$", ".param " + " ".join(f"{k}={v}" for k, v in params.items()), deck, flags=re.M)
    (work / "model.cir").write_text(deck)
    out = subprocess.run(["ngspice", "-b", "model.cir"], cwd=work, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if out.returncode != 0 or not (work / "ac.txt").exists():
        raise RuntimeError(out.stdout[-3000:])
    ac = np.array([list(map(float, l.split())) for l in (work / "ac.txt").read_text().splitlines()[1:] if l.strip()])
    tr = np.array([list(map(float, l.split())) for l in (work / "tran.txt").read_text().splitlines()[1:] if l.strip()])
    op = {}
    for line in out.stdout.splitlines():
        m = re.match(r"\s*(v\(\w+\)|@mn\[id\])\s*=\s*([-+0-9.eE]+)", line)
        if m: op[m.group(1)] = float(m.group(2))
    return ac, tr, op


def corners(f, db):
    ref = np.interp(1000.0, f, db)
    rel = db - ref
    lo = hi = None
    for i in range(1, len(f)):
        if f[i] <= 1000 and rel[i - 1] < -3 <= rel[i]:
            lo = f[i - 1] + (f[i] - f[i - 1]) * (-3 - rel[i - 1]) / (rel[i] - rel[i - 1])
        if f[i] > 1000 and rel[i - 1] >= -3 > rel[i] and hi is None:
            hi = f[i - 1] + (f[i] - f[i - 1]) * (rel[i - 1] + 3) / (rel[i - 1] - rel[i])
    return lo, hi, ref


def harmonics(t, v, f0, n=5):
    """Amplitudes of harmonics 1..n of f0 over whole cycles after 10 ms."""
    win = t >= 10e-3
    tt, vv = t[win], v[win] - v[win].mean()
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
    deck = Path(__file__).with_name("nes001_audio.cir").read_text()
    base = {"r_tv": 47000, "r_follower": 100, "src": 1, "vsup_ac": 0, "amp": 0, "fsig": "1k", "rload_follow": 3900}
    results = {"response": [], "supply": [], "distortion": []}
    for src in (1, 2):
        for r_tv in (10000, 47000, 1000000):
            params = dict(base, src=src, r_tv=r_tv)
            with tempfile.TemporaryDirectory() as d:
                ac, tr, op = run(deck, params, Path(d))
            f, jack_db, out_db, mb_db = ac[:, 0], ac[:, 1], ac[:, 3], ac[:, 4]
            lo, hi, ref = corners(f, jack_db)
            lo_mb, hi_mb, _ = corners(f, out_db)
            r = {"src": "pulse" if src == 1 else "tnd", "r_tv": r_tv, "gain_1khz_db": ref,
                 "hp_hz": lo, "lp_hz": hi, "board_hp_hz": lo_mb, "board_lp_hz": hi_mb,
                 "gate_bias_v": op.get("v(out)"), "gate_current_ma": (op.get("@mn[id]") or 0) * 1e3,
                 "response_db": {str(h): float(np.interp(h, f, jack_db) - ref) for h in TEST_HZ}}
            results["response"].append(r)
            print(f"{r['src']:5s} Rtv={r_tv:7d}: gain {ref:+.2f} dB, HP {lo:.1f} Hz (board {lo_mb:.1f}), "
                  f"LP {hi/1000:.1f} kHz (board {hi_mb/1000:.1f}), gate bias {r['gate_bias_v']:.2f} V, {r['gate_current_ma']:.1f} mA")
    # Supply gain: AC on VCC, both pins quiet.
    params = dict(base, src=0, vsup_ac=1)
    with tempfile.TemporaryDirectory() as d:
        ac, tr, op = run(deck, params, Path(d))
    f, jack_db = ac[:, 0], ac[:, 1]
    for h in (60, 120, 180, 240, 360, 1000):
        g = float(np.interp(h, f, jack_db))
        results["supply"].append({"hz": h, "gain_db": g})
        print(f"supply to jack at {h:4d} Hz: {g:+.1f} dB")
    # Distortion at NES levels.
    for src in (1, 2):
        for amp in (0.15, 0.30):
            params = dict(base, src=src, amp=amp)
            with tempfile.TemporaryDirectory() as d:
                ac, tr, op = run(deck, params, Path(d))
            t, jack = tr[:, 0], tr[:, 4]
            hs = harmonics(t, jack, 1000.0)
            thd = math.sqrt(sum(h * h for h in hs[1:])) / hs[0]
            r = {"src": "pulse" if src == 1 else "tnd", "pin_amplitude_v": amp, "jack_amplitude_v": hs[0],
                 "thd": thd, "h2_db": 20 * math.log10(hs[1] / hs[0]), "h3_db": 20 * math.log10(hs[2] / hs[0])}
            results["distortion"].append(r)
            print(f"{r['src']:5s} pin {amp:.2f} V peak -> jack {hs[0]:.3f} V peak, THD {thd*100:.2f}% "
                  f"(H2 {r['h2_db']:.1f} dB, H3 {r['h3_db']:.1f} dB)")
    if args.golden:
        ref = [r for r in results["response"] if r["src"] == "pulse" and r["r_tv"] == 47000][0]
        args.golden.mkdir(parents=True, exist_ok=True)
        (args.golden / "nes001_audio.json").write_text(json.dumps({"response": ref, "supply": results["supply"],
                                                                    "distortion": results["distortion"]}, indent=2) + "\n")
        sup120 = [s for s in results["supply"] if s["hz"] == 120][0]["gain_db"]
        lines = ["/* Generated by tools/circuits/sweep_nes001_audio.py --golden: ngspice run of",
                 " * nes001_audio.cir (74HCU04 MOS pair), pulse pin, 47k TV input. Do not edit. */",
                 "#ifndef NES001_AUDIO_GOLDEN_H", "#define NES001_AUDIO_GOLDEN_H",
                 f"#define NES001_AUDIO_GOLDEN_HP_HZ {ref['hp_hz']:.2f}f",
                 f"#define NES001_AUDIO_GOLDEN_LP_HZ {ref['lp_hz']:.1f}f",
                 f"#define NES001_AUDIO_GOLDEN_GAIN_DB {ref['gain_1khz_db']:.2f}f",
                 f"#define NES001_AUDIO_GOLDEN_SUPPLY_GAIN_120HZ_DB {sup120:.2f}f",
                 "static const float nes001_audio_golden_hz[] = {" + ", ".join(f"{h}.0f" for h in TEST_HZ) + "};",
                 "static const float nes001_audio_golden_db[] = {" + ", ".join(f"{ref['response_db'][str(h)]:.3f}f" for h in TEST_HZ) + "};",
                 "#endif", ""]
        (args.golden / "nes001_audio.h").write_text("\n".join(lines))
    (args.output / "results.json").write_text(json.dumps({
        "model": "NES-001 mixer, 74HCU04 gate (level-1 MOS pair fitted to the data sheet), AV module follower and jack; assumed follower and shunt capacitors.",
        **results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
