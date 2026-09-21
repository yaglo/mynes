#!/usr/bin/env python3
"""Full real-game playback: core, audio and complete GPU path. Optional window/vsync.
Reports submission cadence/age, not panel photon latency or isolated GPU time.
Optional readback includes final render, fence, download, PPM/PFM encoding and IO.
"""
import argparse
import csv
import datetime
import json
import os
from pathlib import Path
import platform
import resource
import re
import statistics
import subprocess
import tempfile

root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('binary',type=Path);p.add_argument('rom',type=Path);p.add_argument('output',type=Path)
p.add_argument('presets',nargs='*')
p.add_argument('--frames',type=int,default=720);p.add_argument('--warmup',type=int,default=120)
p.add_argument('--size',default='2560x1664',help='offscreen drawable pixels')
p.add_argument('--onscreen',action='store_true',help='explicitly include window/vsync; otherwise hidden and silent')
p.add_argument('--native-fullscreen',action='store_true')
p.add_argument('--modes',nargs='+',choices=['cpu','gpu','gpu-readback'],default=['cpu','gpu','gpu-readback'])
a=p.parse_args();a.binary=a.binary.resolve();a.rom=a.rom.resolve();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
assert a.frames>a.warmup+120
presets=a.presets or ['sony_pvm_14l2','jvc_d_series_2000','toshiba_14af43','stass_favourite']
def stats(values):
    values=sorted(values)
    def pct(q):return values[min(len(values)-1,int((len(values)-1)*q))]
    return dict(mean=statistics.mean(values),median=statistics.median(values),p95=pct(.95),p99=pct(.99),maximum=max(values))
def frontends():
    return [r.strip() for r in subprocess.check_output(['ps','-axo','pid=,comm='],text=True).splitlines() if r.split() and Path(r.split()[-1]).name=='mynes_gpu']
report=dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),platform=platform.platform(),rom=a.rom.name,
    offscreen=not a.onscreen,frames=a.frames,warmup=a.warmup,load_before=os.getloadavg(),other_frontends=frontends(),results=[],
    metric='Real ROM, core/APU, selected CPU/GPU audio and complete GPU signal/CRT. SDL audio queue active (muted offscreen). Offscreen mode fences the full render; no window presentation/vsync. Timestamps stop at submission, not scanout. Readback additionally captures once per 60 emulated frames; CPU encoding/writing runs in one bounded worker, drained before exit. capture_ms is render-thread readback/copy cost; capture_write_ms includes encoding and disk IO.')
if platform.system()=='Darwin':report['hardware']=subprocess.check_output(['sysctl','hw.model','hw.memsize','machdep.cpu.brand_string'],text=True).strip()
for preset in presets:
    for mode in a.modes:
        slug=preset+'-'+mode; trace=a.output/(slug+'.csv');audio=a.output/(slug+'-audio.csv')
        with tempfile.TemporaryDirectory(prefix='mynes-playback-bench-') as config:
            env=dict(os.environ,XDG_CONFIG_HOME=config,MYNES_REVIEW_NO_INPUT='1',MYNES_REVIEW_START_FRAME='90',
                MYNES_PLAYBACK_FRAMES=str(a.frames),MYNES_PLAYBACK_TRACE=str(trace),MYNES_AUDIO_TRACE=str(audio))
            for key in ['MYNES_GPU_VALIDATION','MYNES_GPU_AUDIO','MYNES_PRESENT_STALL_MS','MYNES_PLAYBACK_READBACK_PATH']:env.pop(key,None)
            if mode.startswith('gpu'):env['MYNES_GPU_AUDIO']='1'
            if mode.endswith('readback'):env['MYNES_PLAYBACK_READBACK_PATH']=str(a.output/(slug+'.ppm'))
            args=[str(a.binary),str(a.rom),'--preset','presets/'+preset+'.json','--mask-alignment','pixels']
            if not a.onscreen:args+=['--offscreen',a.size]
            elif a.native_fullscreen:args.append('--native-fullscreen')
            before=resource.getrusage(resource.RUSAGE_CHILDREN)
            with (a.output/(slug+'.log')).open('w') as log:subprocess.run(args,cwd=root,env=env,stdout=log,stderr=log,check=True,timeout=max(90,a.frames/30))
            after=resource.getrusage(resource.RUSAGE_CHILDREN)
        writes=re.findall(r'CAPTURE_WRITE ms=([0-9.]+) saved=([01])',(a.output/(slug+'.log')).read_text())
        if mode.endswith('readback'):
            assert writes and all(saved=='1' for _,saved in writes), 'Capture writes missing or failed'
        rows=[{k:float(v) for k,v in r.items()} for r in csv.DictReader(trace.open())]
        if mode.endswith('readback'):
            assert len(writes)==sum(r['capture_ms']>0 for r in rows), 'A requested capture was not written'
        rows=[r for r in rows if r['frame']>a.warmup]
        if len(rows)<30:raise RuntimeError('Insufficient presented frames: '+slug)
        gaps=[(b['submit_ns']-c['submit_ns'])/1e6 for c,b in zip(rows,rows[1:])]
        duration=(rows[-1]['submit_ns']-rows[0]['submit_ns'])/1e9
        ar=[r for r in csv.DictReader(audio.open()) if int(r['frame'])>a.warmup]
        capture=[r['capture_ms'] for r in rows if r['capture_ms']>0]
        result=dict(preset=preset,mode=mode,width=int(rows[-1]['width']),height=int(rows[-1]['height']),
            rendered=len(rows),fps=(len(rows)-1)/duration,emulated_fps=(rows[-1]['frame']-rows[0]['frame'])/duration,
            skipped=sum(r['skipped'] for r in rows),cadence_ms=stats(gaps),
            frame_age_ms=stats([(r['submit_ns']-r['start_ns'])/1e6 for r in rows]),
            ready_age_ms=stats([(r['submit_ns']-r['ready_ns'])/1e6 for r in rows]),
            timings={k:stats([r[k] for r in rows]) for k in ['emulation_ms','audio_ms','encode_ms','present_ms','swap_wait_ms']},
            capture_ms=stats(capture) if capture else None,
            capture_write_ms=stats([float(ms) for ms,_ in writes]) if writes else None,
            capture_writes=len(writes),
            audio_queue_ms=stats([(int(r['queued'])+int(r['samples']))/44.1 for r in ar]),
            gpu_audio_fraction=sum(int(r['gpu']) for r in ar)/len(ar),
            process_cpu_seconds=after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime)
        report['results'].append(result)
        report['load_after']=os.getloadavg();(a.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        print(f"{slug:34} {result['fps']:.2f} rendered / {result['emulated_fps']:.2f} emulated fps; p95 cadence {result['cadence_ms']['p95']:.2f} ms; skipped {result['skipped']:.0f}; audio p95 {result['audio_queue_ms']['p95']:.1f} ms",flush=True)
        # Keep reports and screenshots, release the large diagnostic raw captures.
        for suffix in ['.ppm','.ppm.linear.pfm']:(a.output/(slug+suffix)).unlink(missing_ok=True)
