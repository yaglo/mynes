#!/usr/bin/env python3
"""Review --screenshot-pair captures in linear light (requires numpy, Pillow).
Usage: review_captures.py /tmp/crt-review
Preserves both individual frames. The average is a still-review exposure,
not an extra temporal filter in the emulator. SDR PNG clips EDR above white.
"""
import json
import argparse
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw


def read_pfm(path):
    with path.open("rb") as f:
        assert f.readline().strip() == b"PF"
        width, height = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), dtype="<f4" if scale < 0 else ">f4")
        return data.reshape(height, width, 3)[::-1].copy()


def display(linear):
    v = np.clip(linear*args.exposure, 0, 1)
    encoded = np.where(v <= .0031308, 12.92*v, 1.055*v**(1/2.4)-.055)
    return Image.fromarray(np.uint8(np.rint(encoded*255)))


parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("root",type=Path)
parser.add_argument("--exposure",type=float,default=1.0,help="fixed linear exposure for every preview; 0.5 retains EDR detail on SDR")
parser.add_argument("--scene",choices=["mario","contra","chart"],default="mario")
args=parser.parse_args()
root=args.root
metrics, tiles, crops = {}, [], []
for first in sorted(root.glob("*.ppm.linear.pfm")):
    if ".next.ppm." in first.name:
        continue
    second = first.with_name(first.name.replace(".ppm.linear.pfm", ".ppm.next.ppm.linear.pfm"))
    a = read_pfm(first)
    name = first.name.removesuffix(".ppm.linear.pfm")
    if second.exists():
        b = read_pfm(second)
        assert a.shape == b.shape
        mean = (a+b)/2
        metrics[name] = {"linear_frame_difference_rms": float(np.sqrt(np.mean((a-b)**2))),
                         "linear_peak": float(mean.max()), "linear_mean": mean.mean((0,1)).tolist()}
        display(a).save(root / (name+"-phase-a.png"))
        display(b).save(root / (name+"-phase-b.png"))
    else:
        mean = a
    pic = display(mean)
    pic.save(root / (name+"-merged.png"))
    width, height = pic.size
    if args.scene=="contra":
        pic.crop((int(width*.305),int(height*.057),int(width*.695),int(height*.751))).save(root/(name+"-boss.png"))
    title=pic.crop((int(width*.32),int(height*.14),int(width*.32)+640,int(height*.14)+360))
    title.save(root/(name+"-title-crop.png"))
    crop_x,crop_y=(.4,.26) if args.scene=="contra" else (.64,.035)
    detail_x,detail_y=(.4,.57) if args.scene=="contra" else (.28,.77)
    crop = pic.crop((int(width*crop_x), int(height*crop_y), int(width*crop_x)+384, int(height*crop_y)+224))
    crop.save(root / (name+"-beam-crop.png"))
    detail = pic.crop((int(width*detail_x), int(height*detail_y), int(width*detail_x)+384, int(height*detail_y)+224))
    detail.save(root / (name+"-detail-crop.png"))
    panel = Image.new("RGB", (768, 250), "#202020")
    panel.paste(crop, (0,26)); panel.paste(detail, (384,26))
    ImageDraw.Draw(panel).text((8,7), name+(" - face and platform (native pixels)" if args.scene=="contra" else " - grayscale/beam and fine detail (native pixels)"), fill="white")
    crops.append(panel)
    # Average emitted light before encoding. Resizing an sRGB screenshot
    # biases fine masks darker and can invent differences between tubes.
    scale=min(640/width,480/height)
    size=(round(width*scale),round(height*scale))
    reduced=np.stack([np.asarray(Image.fromarray(mean[:,:,c]).resize(size,Image.Resampling.LANCZOS)) for c in range(3)],axis=2)
    thumb=display(reduced)
    tile = Image.new("RGB",(640,510),"#202020"); tile.paste(thumb,(0,30))
    ImageDraw.Draw(tile).text((8,9), name+" - linear average of consecutive frames", fill="white")
    tiles.append(tile)
for name, panels, width, height in [("overview",tiles,640,510),("native-crops",crops,768,250)]:
    sheet = Image.new("RGB",(width*2,height*((len(panels)+1)//2)),"#202020")
    for i,panel in enumerate(panels): sheet.paste(panel,((i%2)*width,(i//2)*height))
    sheet.save(root/(name+".png"))
    for start in range(0,len(panels),4):
        page=Image.new("RGB",(width*2,height*2),"#202020")
        for i,panel in enumerate(panels[start:start+4]): page.paste(panel,((i%2)*width,(i//2)*height))
        page.save(root/(name+f"-{start//4+1}.png"))
(root/"metrics.json").write_text(json.dumps(metrics,indent=2)+"\n")
print(json.dumps(metrics,indent=2))
