#!/usr/bin/env python3
"""check_lens_align.py — 验证 lensfun 修复后 RT 与线上版在这些机型上是否更对齐"""
import pathlib

import numpy as np
from PIL import Image

RT_OLD = pathlib.Path(r'C:\pack\file\output\rawtherapee_v2')
RT_NEW = pathlib.Path(r'C:\pack\file\output\rawtherapee_v2b')
PROD = pathlib.Path(r'C:\pack\file\output\librawengine_prod')

for name in ['NIKON Z 7_2/10Nikon-Z7-2.jpg', 'NIKON Z 6/5Nikon-Z6.jpg']:
    imgs = {}
    for tag, d in [('fix_old', RT_OLD), ('fix_new', RT_NEW), ('prod', PROD)]:
        p = d / name
        if p.exists():
            with Image.open(p) as im:
                imgs[tag] = (im.size, np.asarray(im, dtype=np.float32))
    if 'prod' not in imgs:
        print(f'{name}: 线上版无此图，跳过')
        continue
    ref_size, ref = imgs['prod']
    print(name)
    for tag in ['fix_old', 'fix_new']:
        if tag not in imgs:
            print(f'  {tag}: 缺失')
            continue
        size, arr = imgs[tag]
        if size != ref_size or arr.shape != ref.shape:
            print(f'  {tag}: 尺寸不同 {size} vs {ref_size}')
            continue
        mae = float(np.abs(arr - ref).mean())
        print(f'  {tag} vs 线上版: MAE {mae:.2f}')
