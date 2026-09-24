#!/usr/bin/env python3
"""Check the final CRT image for repeating RF snow, offscreen and silent.
Usage: test_rf_temporal.py build/bin/mynes_gpu /tmp/rf-review
Requires NumPy/Pillow, as does review_captures.py. Saves an animated WebP.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import numpy as np
from PIL import Image

root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('binary',type=Path);p.add_argument('output',type=Path)
a=p.parse_args();a.binary=a.binary.resolve();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='mynes-rf-review-') as temp:
    preset=json.loads((root/'presets/stass_favourite.json').read_text())
    # 30 dB carrier-to-noise in 4 MHz: the direct link's 61.7 dB less 31.4 dB of loss.
    preset['name']='RF temporal regression';preset['rf']['link_loss_db']=31.4
    preset['console_psu_hum']=0;preset['tv']['noise_level']=0;preset['tv']['hum_bar_amplitude']=0
    preset['video_cable']['shield_effectiveness']=1
    path=Path(temp)/'rf.json';path.write_text(json.dumps(preset))
    codes=Path(temp)/'gray.bin';codes.write_bytes(bytes([0x10])*(256*240))
    with (a.output/'rf.log').open('w') as log:
        subprocess.run([str(a.binary),'--offscreen','960x720','--simulate-frame',str(codes),
            '--preset',str(path),'--screenshot-after','60','--screenshot-frames','36',
            '--screenshot-path',str(a.output/'rf.ppm')],cwd=root,env=dict(os.environ,XDG_CONFIG_HOME=temp),
            stdout=log,stderr=log,check=True,timeout=60)
    assert 'RF temporal regression' in (a.output/'rf.log').read_text()
paths=[a.output/'rf.ppm.linear.pfm']+[a.output/f'rf.ppm.frame-{i:03d}.ppm.linear.pfm' for i in range(1,36)]
frames=[];previews=[]
for path in paths:
    with path.open('rb') as f:
        assert f.readline().strip()==b'PF'
        width,height=map(int,f.readline().split());scale=float(f.readline())
        image=np.frombuffer(f.read(),dtype='<f4' if scale<0 else '>f4').reshape(height,width,3)[::-1]
    frames.append(image[96:624:2,96:864:2].copy())
    linear=np.clip(image,0,1)
    encoded=np.where(linear<=.0031308,12.92*linear,1.055*linear**(1/2.4)-.055)
    previews.append(Image.fromarray(np.uint8(np.rint(encoded*255))))
stack=np.stack(frames);residual=stack-stack.mean(axis=0,keepdims=True)
rms=float(np.sqrt(np.mean(residual**2)));assert rms>.005,'RF noise missing or frozen'
lags={}
for lag in range(1,13):
    x=residual[lag:].ravel();y=residual[:-lag].ravel()
    lags[lag]=float(np.dot(x,y)/np.sqrt(np.dot(x,x)*np.dot(y,y)))
    assert abs(lags[lag])<.1, f'Repeating RF pattern at lag {lag}: {lags[lag]}'
report=dict(frames=36,correlations=lags,residual_rms=rms,
    method='Final linear output; fixed mask/image removed using 36-frame mean, which gives a small negative correlation bias. Static gray isolates noise from composite crawl.')
(a.output/'metrics.json').write_text(json.dumps(report,indent=2)+'\n')
previews[0].save(a.output/'rf-noise.webp',save_all=True,append_images=previews[1:],duration=17,loop=0,lossless=True)
print(json.dumps(report,indent=2))
# The animation and metrics retain the result; release large intermediate captures.
for path in paths:
    path.unlink();Path(str(path).removesuffix('.linear.pfm')).unlink()
