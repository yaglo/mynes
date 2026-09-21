#!/usr/bin/env python3
"""Sweep the PVM-14L2 passive aperture input branch's UNKNOWN port load.
Requires ngspice. Produces CSVs and a JSON summary; never changes presets.
The active CXA1739S transfer is absent: these are not monitor responses.
"""
import argparse
import cmath
import csv
import json
import math
from pathlib import Path
import subprocess
import tempfile


def transfer(frequency, port_ohms):
    """Independent impedance-divider solution for the partial passive circuit."""
    s = 2j * math.pi * frequency
    series = s * 15e-6 + 1 / (s * 20e-12) + port_ohms
    shunt = 1 / (1 / 3900 + 1 / series)
    feed = 1 / (1 / 1800 + s * 39e-12)
    return shunt / (feed + shunt) * port_ohms / series


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    deck = Path(__file__).with_name('pvm14l2_aperture_input.cir').read_text()
    cases = []
    for load in (1000, 3300, 10000, 33000, 100000):
        with tempfile.TemporaryDirectory(prefix='mynes-pvm-aperture-') as tmp:
            netlist = Path(tmp, 'input.cir')
            netlist.write_text(deck.replace('RPORT=10000', f'RPORT={load}'))
            subprocess.run(['ngspice', '-b', str(netlist)], cwd=tmp,
                           check=True, capture_output=True, text=True)
            rows = [tuple(map(float, line.split())) for line in
                    Path(tmp, 'response.txt').read_text().splitlines()[1:]]
        assert rows and all(len(row) == 3 for row in rows)
        errors = [abs(complex(re, im) - transfer(f, load)) for f, re, im in rows]
        assert max(errors) < 1e-8, 'ngspice disagrees with impedance-divider solution'
        with (args.output / f'port_{load}_ohm.csv').open('w', newline='') as out:
            writer = csv.writer(out)
            writer.writerow(('frequency_hz', 'voltage_gain_db', 'phase_degrees'))
            for f, re, im in rows:
                value = complex(re, im)
                writer.writerow((f, 20 * math.log10(abs(value)), math.degrees(cmath.phase(value))))
        peak = max(rows, key=lambda row: abs(complex(row[1], row[2])))
        cases.append({'assumed_port_ohms': load, 'sampled_peak_hz': peak[0],
                      'sampled_peak_db': 20 * math.log10(abs(complex(peak[1], peak[2]))),
                      'max_complex_error_vs_divider': max(errors)})
    summary = {'model': 'Passive SHP IN branch only; unknown IC load; NOT monitor aperture response',
               'ideal_LC_resonance_hz': 1 / (2 * math.pi * math.sqrt(15e-6 * 20e-12)),
               'cases': cases}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
