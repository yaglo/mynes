#!/usr/bin/env python3
"""Audit the saved TV schema against OSD coverage and every shipped value.
This checks parameter/control contracts, not hardware calibration.
"""
import json,math,re
from pathlib import Path
root=Path(__file__).resolve().parents[3]
source=(root/'frontends/gpu/preset_apply.c').read_text()
schema=(root/'frontends/gpu/preset_json.h').read_text()
fields=set(re.findall(r'MATCH_\w+\(PJSON_SEC_TV,\s*"(\w+)"',schema))
# The old misleading mm spelling imports into mask_pitch_px, exposed in pixels.
fields.discard('mask_pitch_mm')
missing=[f for f in sorted(fields) if '&vc->tv.'+f not in source]
assert not missing, f'Saved TV parameters missing from OSD: {missing}'
vhs_fields=set(re.findall(r'MATCH_\w+\(PJSON_SEC_VHS,\s*"(\w+)"',schema))
missing=[f for f in sorted(vhs_fields) if '&vc->vhs.'+f not in source]
assert not missing, f'Saved VHS parameters missing from OSD: {missing}'
ranges={}
pattern=r'MI_FLOAT\("[^"\n]*",\s*&vc->(tv\.\w+|cable\.\w+|rf\.\w+|vhs\.\w+|\w+),\s*([^,]+),\s*([^,]+),\s*([^,]+),'
for field,step,lo,hi in re.findall(pattern,source):
    try: ranges[field]=(float(lo.strip().removesuffix('f')),float(hi.strip().removesuffix('f')))
    except ValueError: pass
count=0
for path in sorted((root/'presets').glob('*.json')):
    preset=json.loads(path.read_text()); count+=1
    for field,(lo,hi) in ranges.items():
        value=preset
        for key in field.replace('cable.','video_cable.').split('.'):
            value=value.get(key) if isinstance(value,dict) else None
        if not isinstance(value,(int,float)): continue
        assert math.isfinite(value), (path.name,field,'nonfinite')
        # Zero RF fields in a non-RF profile are legacy auto/default metadata.
        if field.startswith('rf.') and preset['connection']!='rf' and value==0: continue
        assert lo<=value<=hi, (path.name,field,value,'OSD limits',lo,hi)
    t=preset['tv']
    assert t.get('beam_fwhm_min',0)<=t.get('beam_fwhm_max',2.35), path.name
    assert t.get('persistence_tail_weight',0)==0 or t.get('persistence_tail_ms',0)>0,path.name
    if preset.get('vhs',{}).get('enabled'):
        assert preset['connection'] in ('composite','rf') and preset['region']=='ntsc',path.name
print(f'{count} presets: finite control ranges and {len(fields)} saved TV controls covered by OSD')
