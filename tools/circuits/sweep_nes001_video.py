#!/usr/bin/env python3
"""NES-001 PPU-to-jack video path: edge response and chroma distortion from the schematic.

Runs tools/circuits/nes001_video_chain.cir over the unknown module bias
resistor and choke capacitor with one PWL stimulus: black, a one-pixel white
pulse, an eight-pixel white pulse, then the 3.58 MHz chroma square wave of
palette rows $0x, $1x, $2x and $3x (NESdev terminated levels mapped onto the
PPU pin swing). It reports, at the 75 ohm jack, the 10-90% rise and 90-10%
fall times, how much of white a single pixel reaches, and each row's chroma
fundamental amplitude and phase against the pin's, so the per-row hue
rotation can be compared with the NESdev estimate (-2.5 deg 2C02E, -5 deg 2C02G
per row) and the GPU console stage.

Devices in the deck are generic; the numbers are sensitivities of a traced
topology, not measurements of a console. Requires ngspice and numpy.
"""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile

import numpy as np

FSC = 3579545.0
PIXEL = 4 / (6 * FSC)        # 186.2 ns, four master clocks
TERMINATED_ROWS = [(0.228, 0.616), (0.312, 0.840), (0.552, 1.100), (0.880, 1.100)]
BLANK_T, WHITE_T = 0.312, 1.100


def pin_level(terminated, vblank, vwhite):
    return vblank + (terminated - BLANK_T) / (WHITE_T - BLANK_T) * (vwhite - vblank)


def stimulus(vblank, vwhite):
    """(time, volts) breakpoints; returns the PWL text and the schedule."""
    edge = 2e-9
    pts = [(0.0, vblank)]
    sched = {}

    def step(t, v):
        pts.append((t, pts[-1][1]))
        pts.append((t + edge, v))

    step(2e-6, vwhite); step(2e-6 + PIXEL, vblank)
    sched["pulse1"] = (2e-6, 2e-6 + PIXEL)
    step(6e-6, vwhite); step(6e-6 + 8 * PIXEL, vblank)
    sched["pulse8"] = (6e-6, 6e-6 + 8 * PIXEL)
    t = 12e-6
    cycles = 40
    for row, (lo, hi) in enumerate(TERMINATED_ROWS):
        vlo, vhi = pin_level(lo, vblank, vwhite), pin_level(hi, vblank, vwhite)
        step(t, vlo)
        start = t + 2 / FSC
        for c in range(cycles):
            tc = start + c / FSC
            step(tc, vhi); step(tc + 0.5 / FSC, vlo)
        sched[f"row{row}"] = (start, start + cycles / FSC, vlo, vhi)
        t = start + (cycles + 2) / FSC + 1e-6
        step(t - 1e-6, vblank)
    end = t + 1e-6
    pts.append((end, vblank))
    pwl = "PWL(" + " ".join(f"{tt:.10g} {vv:.6g}" for tt, vv in pts) + ")"
    return pwl, sched, end


def run(deck, params, pwl, end, work):
    deck = re.sub(r"^\.param rb=.*$", ".param " + " ".join(f"{k}={v}" for k, v in params.items()),
                  deck, flags=re.M)
    deck = deck.replace("PWL_SOURCE", pwl).replace("tran 1n 60u", f"tran 0.5n {end:.6g}")
    (work / "model.cir").write_text(deck)
    out = subprocess.run(["ngspice", "-b", "model.cir"], cwd=work, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if out.returncode != 0 or not (work / "tran.txt").exists():
        raise RuntimeError(out.stdout[-2000:])
    rows = [list(map(float, l.split())) for l in (work / "tran.txt").read_text().splitlines()[1:] if l.strip()]
    data = np.array(rows)
    t = np.arange(0, end, 0.5e-9)
    cols = {name: np.interp(t, data[:, 0], data[:, i + 1]) for i, name in enumerate(["in", "e1", "vout", "jack"])}
    return t, cols, out.stdout


def edge_times(t, v, t_on, t_off, lo, hi):
    """10-90% rise after t_on and 90-10% fall after t_off, seconds."""
    span = hi - lo
    win = (t >= t_on) & (t < t_off)
    tt, vv = t[win], v[win]
    r10 = tt[np.argmax(vv >= lo + 0.1 * span)] if np.any(vv >= lo + 0.1 * span) else float("nan")
    r90 = tt[np.argmax(vv >= lo + 0.9 * span)] if np.any(vv >= lo + 0.9 * span) else float("nan")
    win = (t >= t_off) & (t < t_off + 3e-6)
    tt, vv = t[win], v[win]
    f90 = tt[np.argmax(vv <= hi - 0.1 * span)] if np.any(vv <= hi - 0.1 * span) else float("nan")
    f10 = tt[np.argmax(vv <= hi - 0.9 * span)] if np.any(vv <= hi - 0.9 * span) else float("nan")
    return r90 - r10, f10 - f90


def fundamental(t, v, start, stop):
    """Amplitude and phase (deg) of the FSC component over whole cycles."""
    win = (t >= start + 10 / FSC) & (t < stop - 2 / FSC)
    tt, vv = t[win], v[win] - v[win].mean()
    z = np.mean(vv * np.exp(-2j * math.pi * FSC * tt)) * 2
    return abs(z), math.degrees(math.atan2(z.imag, z.real))


def analyse(t, cols, sched, vblank, vwhite):
    jack = cols["jack"]
    base = jack[(t > 1.0e-6) & (t < 1.9e-6)].mean()
    on8, off8 = sched["pulse8"]
    white8 = jack[(t > on8 + 5 * PIXEL) & (t < off8 - 0.2e-6)].mean()
    rise, fall = edge_times(t, jack, on8, off8, base, white8)
    on1, off1 = sched["pulse1"]
    peak1 = jack[(t >= on1) & (t < off1 + 0.5e-6)].max()
    rows = []
    for r in range(4):
        start, stop, vlo, vhi = sched[f"row{r}"]
        a_in, p_in = fundamental(t, cols["in"], start, stop)
        a_j, p_j = fundamental(t, jack, start, stop)
        mean_in = cols["in"][(t >= start + 10 / FSC) & (t < stop)].mean()
        mean_j = jack[(t >= start + 10 / FSC) & (t < stop)].mean()
        rows.append({"row": r, "gain": a_j / a_in, "phase_deg": (p_j - p_in + 180) % 360 - 180,
                     "mean_in": mean_in, "mean_jack": mean_j,
                     "ideal_mean_jack": base + (mean_in - vblank) / (vwhite - vblank) * (white8 - base)})
    ref = rows[1]["phase_deg"]
    for r in rows:
        r["phase_rel_row1_deg"] = r["phase_deg"] - ref
        r["gain_rel_row1"] = r["gain"] / rows[1]["gain"]
        r["luma_error_of_white"] = (r["mean_jack"] - r["ideal_mean_jack"]) / (white8 - base)
    return {"jack_blank_V": base, "jack_white_V": white8, "swing_V": white8 - base,
            "rise_10_90_ns": rise * 1e9, "fall_90_10_ns": fall * 1e9,
            "one_pixel_peak_of_white": (peak1 - base) / (white8 - base),
            "rows": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--vblank", type=float, default=1.4)
    parser.add_argument("--vwhite", type=float, default=2.9)
    parser.add_argument("--golden", type=Path, help="write the rb=3300, cpk=2p case as golden JSON and a C header for the GPU test")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    deck = Path(__file__).with_name("nes001_video_chain.cir").read_text()
    pwl, sched, end = stimulus(args.vblank, args.vwhite)
    results = []
    for rb in (2200, 3300, 4700, 5600, 10000, 1e9):
        for cpk in ("2p", "380p"):
            params = {"rb": rb, "cpk": cpk, "bead_l": "100n", "bead_r": 10, "rppu": 100,
                      "vblank": args.vblank, "vwhite": args.vwhite,
                      "t0": "1u", "tpx": "186.2n", "tcyc": "279.4n"}
            with tempfile.TemporaryDirectory() as d:
                work = Path(d)
                t, cols, log = run(deck, params, pwl, end, work)
                name = f"rb{int(rb) if rb < 1e8 else 'open'}-cpk{cpk}"
                (args.output / f"{name}.log").write_text(log)
                np.savetxt(args.output / f"{name}-jack.txt", np.column_stack([t, cols["in"], cols["vout"], cols["jack"]])[::4],
                           header="t in vout jack", fmt="%.6g")
            r = analyse(t, cols, sched, args.vblank, args.vwhite)
            r.update({"rb_ohm": rb if rb < 1e8 else None, "cpk": cpk})
            results.append(r)
            print(f"{name:16s} swing {r['swing_V']:.3f} V  rise {r['rise_10_90_ns']:6.1f} ns  fall {r['fall_90_10_ns']:6.1f} ns  "
                  f"1px {r['one_pixel_peak_of_white']:.2f}  rows gain "
                  + " ".join(f"{x['gain_rel_row1']:.2f}" for x in r["rows"]) + "  phase "
                  + " ".join(f"{x['phase_rel_row1_deg']:+.1f}" for x in r["rows"]) + "  luma err "
                  + " ".join(f"{x['luma_error_of_white']:+.3f}" for x in r["rows"]))
    if args.golden:
        ref = [r for r in results if r["rb_ohm"] == 3300 and r["cpk"] == "2p"][0]
        args.golden.mkdir(parents=True, exist_ok=True)
        (args.golden / "nes001_video.json").write_text(json.dumps(ref, indent=2) + "\n")
        lines = ["/* Generated by tools/circuits/sweep_nes001_video.py --golden: ngspice run of",
                 " * nes001_video_chain.cir with rb=3300, cpk=2p. Do not edit. */",
                 "#ifndef NES001_VIDEO_GOLDEN_H", "#define NES001_VIDEO_GOLDEN_H",
                 f"#define NES001_GOLDEN_RISE_10_90_NS {ref['rise_10_90_ns']:.1f}f",
                 f"#define NES001_GOLDEN_FALL_90_10_NS {ref['fall_90_10_ns']:.1f}f",
                 f"#define NES001_GOLDEN_ONE_PIXEL_PEAK {ref['one_pixel_peak_of_white']:.3f}f",
                 "static const float nes001_golden_row_gain_rel_row1[4] = {" + ", ".join(f"{r['gain_rel_row1']:.4f}f" for r in ref["rows"]) + "};",
                 "static const float nes001_golden_row_phase_rel_row1_deg[4] = {" + ", ".join(f"{r['phase_rel_row1_deg']:.2f}f" for r in ref["rows"]) + "};",
                 "static const float nes001_golden_row_luma_error[4] = {" + ", ".join(f"{r['luma_error_of_white']:.4f}f" for r in ref["rows"]) + "};",
                 "#endif", ""]
        (args.golden / "nes001_video.h").write_text("\n".join(lines))
    (args.output / "results.json").write_text(json.dumps({
        "model": "NES-001 motherboard follower (Schenk) + EDC AV module trace; generic transistors, assumed bead and PPU source. Not calibrated.",
        "stimulus": "black; 1-pixel and 8-pixel white; 40-cycle chroma squares of rows $0x-$3x at terminated levels mapped to the pin swing",
        "results": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
