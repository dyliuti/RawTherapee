#!/usr/bin/env python3
"""inspect_rt_demosaic.py — 查看 RT 规则系统实际给各机型设置的 demosaic 方法"""
import re
import pathlib

t = pathlib.Path(r'c:\pack\project\raw\RawTherapee\rtengine\raw_engine.cc').read_text(encoding='utf-8', errors='replace')

m = re.search(r'void use_fast_demosaic.*?\n\}', t, re.S)
print('=== use_fast_demosaic 定义 ===')
print(m.group(0) if m else '(未找到)')

print()
print('=== 各规则设置的 demosaic 方法（含机型线索） ===')
for mm in re.finditer(r'p\.raw\.bayersensor\.method\s*=\s*RAWParams::BayerSensor::getMethodString\(\s*RAWParams::BayerSensor::Method::(\w+)', t):
    line_no = t[:mm.start()].count('\n') + 1
    ctx = t[max(0, mm.start() - 2000):mm.start()]
    models = re.findall(r'ieq\(c\.model,\s*"([^"]+)"\)|ieq\(c\.make,\s*"([^"]+)"\)', ctx)
    hint = ' <- '.join((a or b) for a, b in models[-2:]) if models else '(兜底/复合条件)'
    print(f'  行{line_no}: {mm.group(1):<18s} {hint}')

# use_fast_demosaic 被哪些规则调用
print()
print('=== use_fast_demosaic 调用点 ===')
for mm in re.finditer(r'use_fast_demosaic\(p\)', t):
    line_no = t[:mm.start()].count('\n') + 1
    ctx = t[max(0, mm.start() - 2000):mm.start()]
    models = re.findall(r'ieq\(c\.model,\s*"([^"]+)"\)', ctx)
    print(f'  行{line_no}: 机型 {models[-1] if models else "?"}')
