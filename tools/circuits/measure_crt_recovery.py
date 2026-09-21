#!/usr/bin/env python3
"""Compare ngspice's generic RC transient with actual GPU load-state samples.
Run MYNES_CRT_MEASUREMENTS=/tmp/gpu-rail.csv ../build/bin/test_fidelity from build/,
then this script /tmp/gpu-rail.csv. Requires ngspice; no Python packages.
"""
import argparse,csv,json,subprocess,tempfile,bisect
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('gpu_csv',type=Path);args=p.parse_args()
deck=Path(__file__).with_name('crt_video_recovery.cir')
with tempfile.TemporaryDirectory(prefix='mynes-rc-') as tmp:
    subprocess.run(['ngspice','-b',str(deck.resolve())],cwd=tmp,check=True,capture_output=True)
    rows=Path(tmp,'recovery.txt').read_text().splitlines()[1:]
    values=[tuple(map(float,line.split())) for line in rows]
    times=[v[0] for v in values]
    errors=[]
    for row in csv.DictReader(args.gpu_csv.open()):
        t=float(row['seconds']);i=bisect.bisect_left(times,t)
        a,b=values[i-1],values[i];v=a[1]+(b[1]-a[1])*(t-a[0])/(b[0]-a[0])
        errors.append(float(row['gpu_rail'])-v)
    result={'model':'generic 12 us RC rail, not measured hardware','samples':len(errors),
            'maximum_absolute_voltage_error':max(map(abs,errors)),
            'rms_voltage_error':(sum(e*e for e in errors)/len(errors))**.5}
    print(json.dumps(result,indent=2))
    assert result['maximum_absolute_voltage_error']<1e-5
