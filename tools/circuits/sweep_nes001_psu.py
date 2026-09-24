#!/usr/bin/env python3
"""NES-001 +5 V ripple from the schematic: reservoir, 7805 rejection, console load.

Runs tools/circuits/nes001_psu.cir over the console's load current, the
regulator's ripple rejection, the reservoir capacitance and the adaptor
voltage, and reports the raw reservoir ripple and the +5 V ripple (peak to
peak and the 120/240/360 Hz components). It fits the closed form the
frontend uses to derive the ripple from a preset's PSU values,

    V_120 = k x I_load / (C_res x 10^(RR/20))            (regulator in)
    raw_min = a x Vac - b - c x I_load - raw_pp / 2       (reservoir trough)
    raw_pp  = k_raw x I_load / (2 f C_res)
    V_120  += q x d^2/raw_pp x (1 - d/raw_pp), d = max(0, 7 V - raw_min) (dropout)

writes them to golden/nes001_psu.h, and, with vid=1, runs the video output
stage's line- and field-rate draw on the rail to give the crosstalk that
reaches the audio jack through the gate's supply gain in
golden/nes001_audio.json. The video runs are differenced against the same
run without the video load so the mains ripple does not leak into the
60 Hz reading.
"""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np

MAINS = 60.0
DROPOUT_V = 7.0


def run(deck, params, work):
    deck = re.sub(r"^\.param .*$", ".param " + " ".join(f"{k}={v}" for k, v in params.items()), deck, flags=re.M)
    (work / "model.cir").write_text(deck)
    out = subprocess.run(["ngspice", "-b", "model.cir"], cwd=work, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if out.returncode != 0 or not (work / "tran.txt").exists():
        raise RuntimeError(out.stdout[-3000:])
    return np.array([list(map(float, l.split())) for l in (work / "tran.txt").read_text().splitlines()[1:] if l.strip()])


def resample(t, v, dt):
    tt = np.arange(t[0], t[-1], dt)
    return tt, np.interp(tt, t, v)


def component(tt, vv, f0):
    vv = vv - vv.mean()
    cycles = math.floor((tt[-1] - tt[0]) * f0)
    keep = tt < tt[0] + cycles / f0
    tt, vv = tt[keep], vv[keep]
    return abs(np.mean(vv * np.exp(-2j * math.pi * f0 * tt)) * 2)


BASE = {"rr_db": 73, "i_load": 0.6, "i_mod": 0.04, "r_load": 0.01, "r_src": 0.8, "vac": 10,
        "c_res": "2200u", "l_out": "1u", "r_esr": 0.3, "vid": 0, "apl": 0.25}


def case(deck, **over):
    params = dict(BASE, **over)
    with tempfile.TemporaryDirectory() as d:
        data = run(deck, params, Path(d))
    t, raw, five = data[:, 0], data[:, 1], data[:, 2]
    tt, ff = resample(t, five, 10e-6)
    h = [component(tt, ff, k * 2 * MAINS) for k in (1, 2, 3)]
    return {"c_res_uf": float(str(params["c_res"]).rstrip("u")), "i_load_a": params["i_load"], "rr_db": params["rr_db"],
            "vac": params["vac"], "raw_mean_v": float(raw.mean()), "raw_min_v": float(raw.min()),
            "raw_ripple_pp_v": float(raw.max() - raw.min()), "five_ripple_pp_mv": float((five.max() - five.min()) * 1e3),
            "five_120hz_mv": h[0] * 1e3, "five_240hz_mv": h[1] * 1e3, "five_360hz_mv": h[2] * 1e3}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--golden", type=Path)
    parser.add_argument("--audio-golden", type=Path, default=Path(__file__).with_name("golden") / "nes001_audio.json")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    deck = Path(__file__).with_name("nes001_psu.cir").read_text()
    results = []
    for c_res in (1000, 2200, 4700):
        for i_load in (0.4, 0.6, 0.8):
            for rr in (62, 73):
                r = case(deck, rr_db=rr, i_load=i_load, c_res=f"{c_res}u")
                results.append(r)
                print(f"C {c_res:4d} uF, load {i_load:.1f} A, RR {rr} dB, {BASE['vac']} VAC: raw {r['raw_mean_v']:.2f} V (min {r['raw_min_v']:.2f}), "
                      f"ripple {r['raw_ripple_pp_v']:.2f} Vpp; +5 V 120 Hz {r['five_120hz_mv']:.3f}, 240 Hz {r['five_240hz_mv']:.3f}, "
                      f"360 Hz {r['five_360hz_mv']:.4f} mV peak")
    # Regulator-in rows: closed form V_120 = k * I / (C * 10^(RR/20)).
    reg = [r for r in results if r["raw_min_v"] > DROPOUT_V + 0.2]
    xs = np.array([r["i_load_a"] / (r["c_res_uf"] * 1e-6) * 10 ** (-r["rr_db"] / 20) for r in reg])
    ys = np.array([r["five_120hz_mv"] * 1e-3 for r in reg])
    k = float(np.sum(xs * ys) / np.sum(xs * xs))
    err = float(np.max(np.abs(ys - k * xs) / ys))
    h2 = float(np.mean([r["five_240hz_mv"] / r["five_120hz_mv"] for r in reg]))
    h3 = float(np.mean([r["five_360hz_mv"] / r["five_120hz_mv"] for r in reg]))
    print(f"fit ({len(reg)} rows with the regulator in): V_120 = {k:.4e} x I/(C x 10^(RR/20)), worst error {err*100:.1f}%; "
          f"240 Hz {h2:.3f}, 360 Hz {h3:.4f} of 120 Hz")
    # Reservoir: raw_pp = k_raw * I/(2 f C); raw_mean = a*Vac - b - c*I - raw_pp/2.
    xs = np.array([r["i_load_a"] / (2 * MAINS * r["c_res_uf"] * 1e-6) for r in results])
    ys = np.array([r["raw_ripple_pp_v"] for r in results])
    k_raw = float(np.sum(xs * ys) / np.sum(xs * xs))
    A = np.array([[r["vac"], -1.0, -r["i_load_a"]] for r in results])
    y = np.array([r["raw_mean_v"] + r["raw_ripple_pp_v"] / 2 for r in results])
    (a, b, c), *_ = np.linalg.lstsq(A, y, rcond=None)
    print(f"reservoir: raw_pp = {k_raw:.3f} x I/(2fC); raw_mean + raw_pp/2 = {a:.3f} x Vac - {b:.3f} - {c:.3f} x I")
    # Adaptor voltage: the dropout onset and the hum it makes.
    vac_rows = []
    for vac in (8, 8.5, 9, 9.5, 10, 11):
        r = case(deck, vac=vac)
        vac_rows.append(r)
        print(f"{vac:4.1f} VAC: raw {r['raw_mean_v']:.2f} V (min {r['raw_min_v']:.2f}), +5 V 120 Hz {r['five_120hz_mv']:.3f} mV, pp {r['five_ripple_pp_mv']:.1f} mV")
    drop = [r for r in vac_rows if r["raw_min_v"] < DROPOUT_V - 0.05]
    q = 0.0
    if drop:
        # A triangular notch of depth d over the fraction d/raw_pp of the cycle:
        # its fundamental is about d x (d/raw_pp) x (1 - d/raw_pp).
        xs = np.array([(DROPOUT_V - r["raw_min_v"]) ** 2 / r["raw_ripple_pp_v"]
                       * (1 - (DROPOUT_V - r["raw_min_v"]) / r["raw_ripple_pp_v"]) for r in drop])
        ys = np.array([r["five_120hz_mv"] * 1e-3 - k * r["i_load_a"] / (r["c_res_uf"] * 1e-6) * 10 ** (-r["rr_db"] / 20) for r in drop])
        q = float(np.sum(xs * ys) / np.sum(xs * xs))
        print(f"dropout: V_120 += {q:.3f} x d^2/raw_pp x (1 - d/raw_pp) over {len(drop)} rows, worst error "
              f"{np.max(np.abs(ys - q * xs) / np.maximum(ys, 1e-9)) * 100:.0f}%")
    # Video stage draw on the rail: line and field components, differenced against no video load.
    video = {}
    supply = {s["hz"]: s["gain_db"] for s in json.load(open(args.audio_golden))["supply"]} if args.audio_golden.exists() else {}
    g_line = 10 ** (supply.get(15734, supply.get(1000, 9.0)) / 20)
    g_field = 10 ** (supply.get(60, 9.0) / 20)
    with tempfile.TemporaryDirectory() as d:
        base = run(deck, dict(BASE, vid=1, apl=0.25), Path(d))  # same step plan as the video runs
    for apl in (0.1, 0.25, 0.5):
        with tempfile.TemporaryDirectory() as d:
            data = run(deck, dict(BASE, vid=1, apl=apl), Path(d))
        tt, ff = resample(data[:, 0], data[:, 2], 1e-6)
        # Difference against a run with the video load switched off by a zero APL and no sync: use vid=0 with the same tran plan.
        with tempfile.TemporaryDirectory() as d:
            off = run(deck.replace("if {vid} = 1", "if 1 = 1"), dict(BASE, vid=0), Path(d))
        t0, f0 = resample(off[:, 0], off[:, 2], 1e-6)
        n = min(len(ff), len(f0))
        diff = ff[:n] - f0[:n]
        line = component(tt[:n], diff, 15734.264)
        field = component(tt[:n], diff, 60.055)
        video[str(apl)] = {"rail_15734hz_uv": line * 1e6, "rail_60hz_uv": field * 1e6,
                           "jack_15734hz_uv": line * g_line * 1e6, "jack_60hz_uv": field * g_field * 1e6}
        print(f"video load, APL {apl:.2f}: rail 15.734 kHz {line*1e6:.1f} uV, 60 Hz {field*1e6:.1f} uV; "
              f"jack 15.734 kHz {line*g_line*1e6:.1f} uV, 60 Hz {field*g_field*1e6:.1f} uV")
    fit = {"ripple_k": k, "ripple_fit_error": err, "harmonic_2": h2, "harmonic_3": h3, "raw_pp_k": k_raw,
           "raw_a": float(a), "raw_b": float(b), "raw_c": float(c), "dropout_q": q, "dropout_v": DROPOUT_V}
    ref = [r for r in results if r["i_load_a"] == 0.6 and r["rr_db"] == 73 and r["c_res_uf"] == 2200][0]
    if args.golden:
        args.golden.mkdir(parents=True, exist_ok=True)
        (args.golden / "nes001_psu.json").write_text(json.dumps({"reference": ref, "fit": fit, "vac": vac_rows, "video": video}, indent=2) + "\n")
        rows = results + vac_rows
        lines = ["/* Generated by tools/circuits/sweep_nes001_psu.py --golden: ngspice run of",
                 " * nes001_psu.cir (NES-002 adaptor, bridge, reservoir, 7805 with dropout). Do not edit. */",
                 "#ifndef NES001_PSU_GOLDEN_H", "#define NES001_PSU_GOLDEN_H",
                 f"#define NES001_PSU_RIPPLE_K {k:.5e}f       /* V_120 = k x I/(C x 10^(RR/20)) */",
                 f"#define NES001_PSU_HARMONIC_2 {h2:.4f}f", f"#define NES001_PSU_HARMONIC_3 {h3:.4f}f",
                 f"#define NES001_PSU_RAW_PP_K {k_raw:.4f}f      /* raw_pp = k_raw x I/(2 f C) */",
                 f"#define NES001_PSU_RAW_A {a:.4f}f", f"#define NES001_PSU_RAW_B {b:.4f}f", f"#define NES001_PSU_RAW_C {c:.4f}f",
                 f"#define NES001_PSU_DROPOUT_V {DROPOUT_V:.1f}f", f"#define NES001_PSU_DROPOUT_Q {q:.4f}f",
                 f"#define NES001_PSU_VIDEO_JACK_15734HZ_UV {video['0.25']['jack_15734hz_uv']:.1f}f",
                 f"#define NES001_PSU_VIDEO_JACK_60HZ_UV {video['0.25']['jack_60hz_uv']:.1f}f",
                 "/* c_res_uf, i_load_a, rr_db, vac, five_120hz_mv */",
                 "static const float nes001_psu_golden_rows[][5] = {"]
        lines += [f"    {{{r['c_res_uf']:.1f}f, {r['i_load_a']:.1f}f, {float(r['rr_db']):.1f}f, {float(r['vac']):.1f}f, {r['five_120hz_mv']:.5f}f}}," for r in rows]
        lines += ["};", "#endif"]
        (args.golden / "nes001_psu.h").write_text("\n".join(lines) + "\n")
    (args.output / "results.json").write_text(json.dumps({"model": "EDC NES-001 PSU trace; behavioural 7805 with dropout and inductive output impedance; assumed transformer resistance, ESR and modulator load.",
                                                          "results": results, "vac": vac_rows, "fit": fit, "video": video}, indent=2) + "\n")


if __name__ == "__main__":
    main()
