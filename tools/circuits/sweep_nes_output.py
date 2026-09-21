#!/usr/bin/env python3
"""Sweep a partial NES output buffer; assumed devices are not calibration data."""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile


def simulate(deck, bias, load, inductance, output):
    name = f"bias{bias:g}-load{load}-bead{inductance}n"
    parameters = (f".param bias={bias} rb=100 bead_l={inductance}n "
                  f"bead_r=10 load_r={load}")
    deck = re.sub(r"^\.param .*$", parameters, deck, flags=re.M)
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        (work / "model.cir").write_text(deck)
        result = subprocess.run(
            ["ngspice", "-b", "model.cir"], cwd=work, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        data = [list(map(float, line.split()))
                for line in (work / "ac.txt").read_text().splitlines()[1:]]
        (output / f"{name}.txt").write_text(result.stdout)
        (output / f"{name}-ac.txt").write_text((work / "ac.txt").read_text())
    low = data[0][1]
    carrier = min(data, key=lambda row: abs(math.log(row[0] / 3579545)))
    # Cutoff cases have almost no DC transfer. Normalizing them would turn
    # tiny feedthrough into a misleading +50 dB 'gain'. Preserve absolute gain.
    operating = low > -20
    return dict(
        bias=bias, load_ohm=load, bead_nH=inductance, operating=operating,
        low_frequency_gain_db=low, carrier_frequency_hz=carrier[0],
        carrier_absolute_gain_db=carrier[1],
        carrier_relative_gain_db=carrier[1] - low if operating else None,
        carrier_phase_deg=carrier[2] * 180 / math.pi if operating else None,
        peak_relative_gain_db=max(row[1] for row in data) - low if operating else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    deck = Path(__file__).with_name("nes_output_buffer.cir").read_text()
    rows = [simulate(deck, bias, load, inductance, args.output)
            for bias in (1., 2., 3.) for load in (75, 1000, 10000)
            for inductance in (10, 100, 1000)]
    result = {"model": "Motherboard buffer only; generic PNP, assumed bead/source/load. Not calibrated.",
              "results": rows}
    (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    for row in rows:
        if row["bead_nH"] == 100:
            print(row)


if __name__ == "__main__":
    main()
