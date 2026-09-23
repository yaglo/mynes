"""Fit the RGB console encoder's chroma axes from decoded colour bars.

Feed it the decoded bar values printed by a calibration run of the RGB
encoder (one line per bar: name R=.. G=.. B=..) on stdin. It solves for the
2x2 matrix between the sent (U, V) and the decoded (R-Y, B-Y), then reports
the correction as a gain and angle per axis, which is how the encoder's
axis phases in encoder_rgb.comp.glsl were arrived at (138 deg / 48 deg on
the raster slot grid).

    mynes_retro --calibrate ... | python3 tools/circuits/fit_encoder_axes.py
"""
import cmath
import math
import sys

import numpy as np

TRUTH = {'yellow': (1, 1, 0), 'cyan': (0, 1, 1), 'green': (0, 1, 0),
         'magenta': (1, 0, 1), 'red': (1, 0, 0), 'blue': (0, 0, 1)}
# The encoder's axis phases at the time of the fit; the correction is relative to these.
U_DEG, V_DEG = 30.0, -60.0


def main():
    sent, decoded = [], []
    for line in sys.stdin:
        parts = line.split()
        if not parts or parts[0] not in TRUTH:
            continue
        rd, gd, bd = [float(p.split('=')[1]) for p in parts[1:4]]
        r, g, b = TRUTH[parts[0]]
        y = .299 * r + .587 * g + .114 * b
        u, v = .492111 * (b - y), .877283 * (r - y)
        yd = .299 * rd + .587 * gd + .114 * bd
        sent.append((u, v))
        decoded.append((rd - yd, bd - yd))
        print(f"{parts[0]:8s} sent U={u:+.3f} V={v:+.3f}  decoded R-Y={rd - yd:+.3f} B-Y={bd - yd:+.3f}"
              f"  Y {yd:.3f} vs {y:.3f}")
    if len(sent) < 2:
        print("need at least two bars on stdin", file=sys.stderr)
        return 1
    A, B = np.array(sent), np.array(decoded)
    M, res, _, _ = np.linalg.lstsq(A, B, rcond=None)
    print("decoded (R-Y,B-Y) = M @ sent (U,V), M =\n", M.T, "\nresidual", res)
    # Wanted: R-Y = V/0.877283, B-Y = U/0.492111.
    T = np.array([[0, 1 / .877283], [1 / .492111, 0]])
    # Correction C so that M.T @ C = T; new (U,V) sent = C @ (U,V) true.
    C = np.linalg.inv(M.T) @ T
    print("correction C (apply to true U,V before the current axes):\n", C)
    eu = C[0, 0] * cmath.exp(1j * math.radians(U_DEG)) + C[1, 0] * cmath.exp(1j * math.radians(V_DEG))
    ev = C[0, 1] * cmath.exp(1j * math.radians(U_DEG)) + C[1, 1] * cmath.exp(1j * math.radians(V_DEG))
    print(f"true U axis: gain {abs(eu):.3f} angle {math.degrees(cmath.phase(eu)):+.1f} deg")
    print(f"true V axis: gain {abs(ev):.3f} angle {math.degrees(cmath.phase(ev)):+.1f} deg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
