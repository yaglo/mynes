"""Check the GPU's console output follower against the ngspice deck.

Runs the rc_filter.comp.glsl follower law (rise limited by an RC toward the
headroom, fall immediate or with its own time constant, optional extra pole)
on the 12-sample carrier grid over the four NES palette rows, and prints the
chroma gain, chroma phase and luma error per row next to the deck's rows from
golden/nes001_video.json, so a change to the model's constants can be checked
without a GPU.

    python3 tools/circuits/check_follower_model.py tools/circuits/golden/nes001_video.json
"""
import json
import math
import sys

import numpy as np

FS = 12 * 3579545.0
# (low, high) PPU output volts of rows $0x, $1x, $2x, $3x at a saturated hue.
ROWS = [(0.228, 0.616), (0.312, 0.840), (0.552, 1.100), (0.880, 1.100)]
norm = lambda v: (v - 0.312) / 0.788


def follower(x, tau_ns, headroom, fall_tau_ns=0.0, pole_hz=0.0):
    k = math.exp(-1e9 / (FS * tau_ns)) if tau_ns > 0 else 0.0
    kf = math.exp(-1e9 / (FS * fall_tau_ns)) if fall_tau_ns > 0 else 0.0
    y = np.empty_like(x)
    p = x[0]
    for i, xi in enumerate(x):
        if xi > p:
            v = headroom + (p - headroom) * k
            p = min(xi, v)
        else:
            p = xi + (p - xi) * kf
        y[i] = p
    if pole_hz > 0:
        a = math.exp(-2 * math.pi * pole_hz / FS)
        z = y[0]
        for i in range(len(y)):
            z = a * z + (1 - a) * y[i]
            y[i] = z
    return y


def measure(tau, headroom, fall=0.0, pole=0.0):
    out = []
    for lo, hi in ROWS:
        n = 12 * 40
        x = np.array([norm(hi) if (i % 12) < 6 else norm(lo) for i in range(n)])
        y = follower(x, tau, headroom, fall, pole)
        w = np.arange(n) >= 120
        zx = np.mean(x[w] * np.exp(-2j * math.pi * np.arange(n)[w] / 12)) * 2
        zy = np.mean(y[w] * np.exp(-2j * math.pi * np.arange(n)[w] / 12)) * 2
        out.append((abs(zy) / abs(zx), math.degrees(math.atan2((zy / zx).imag, (zy / zx).real)),
                    y[w].mean() - x[w].mean()))
    g1, p1, _ = out[1]
    return [(g / g1, p - p1, l) for g, p, l in out]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    golden = json.load(open(sys.argv[1]))
    # The golden file holds the chosen case; a full sweep's results.json holds a list.
    cases = golden["results"] if "results" in golden else [golden]
    ref = [r for r in cases if r["rb_ohm"] == 3300 and r["cpk"] == "2p"][0]
    print("ngspice rb3300 cpk2p:", [(round(r["gain_rel_row1"], 3), round(r["phase_rel_row1_deg"], 1),
                                    round(r["luma_error_of_white"], 3)) for r in ref["rows"]])
    for tau, head, fall, pole in [(168, 2.0, 0, 0), (168, 2.0, 5, 0), (168, 2.0, 5, 6e6),
                                  (140, 2.0, 5, 0), (168, 1.8, 5, 0), (168, 2.2, 5, 0), (200, 2.0, 5, 0)]:
        m = measure(tau, head, fall, pole)
        print(f"model tau {tau} head {head} fall {fall} pole {pole / 1e6:.0f}MHz:",
              [(round(float(g), 3), round(float(p), 1), round(float(l), 3)) for g, p, l in m])
    return 0


if __name__ == "__main__":
    sys.exit(main())
