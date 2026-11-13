#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[去重Cache 256->512 A/B 全量对照 20261112]
把两份同口径 EXTENDED_METRICS.md 按指标行对齐, 每格输出 "256值 -> 512值"。
唯一变量 = dedup_cache.num_sets 64->128 (cacheline 256->512); 二进制 sha 逐位相同。
"""
import re, os

BASE = '/mnt/d/Qoder_proj/iommu_model_20260401_v1/rwsplit_results/nvrr6_ptfifo1024_20261109/EXTENDED_METRICS.md'
NEW = '/mnt/d/Qoder_proj/iommu_model_20260401_v1/rwsplit_results/nvrr6_dedup512_20261112/EXTENDED_METRICS.md'
OUT = '/mnt/d/Qoder_proj/iommu_model_20260401_v1/rwsplit_results/nvrr6_dedup512_20261112/AB_256_vs_512.md'


def load(p):
    rows = {}
    order = []
    for ln in open(p, encoding='utf-8'):
        ln = ln.strip()
        if not ln.startswith('|'):
            continue
        cells = [c.strip() for c in ln.strip('|').split('|')]
        if len(cells) < 9 or cells[0] in ('指标',) or set(cells[0]) <= {'-'}:
            continue
        key = re.sub(r'\s*\(.*总\d+\)', '', cells[0])   # 去掉容量标签便于对齐
        key = re.sub(r'\s+', ' ', key)
        rows[key] = cells[1:9]
        order.append(key)
    return rows, order


b, ob = load(BASE)
n, on = load(NEW)

# 列序必须与 EXTENDED_METRICS.md 表头一致: 场景1..8 = 8C1,8C2,8C4,16C1,16C2,16C4,8C8,16C8
SC = ['8盘C1', '8盘C2', '8盘C4', '16盘C1', '16盘C2', '16盘C4', '8盘C8', '16盘C8']


def num(s):
    m = re.match(r'^(-?[\d.]+)', s)
    return float(m.group(1)) if m else None


lines = ['# 去重Cache 256 -> 512 全量 A/B 对照 (批次 dedup512_20261112)',
         '',
         '- 唯一变量: 运行目录 JSON `dedup_cache.num_sets` 64->128 (num_ways=4, num_rams=4 不变 => cacheline 256->512)。',
         '- 二进制与 256 基线 **sha256 逐位相同**(复用同批产物), 主配置未改(保护 real8/real16 的 ==256 断言)。',
         '- 每格格式: `256基线 -> 512`。IOPS 行附 Δ%。8 场景门禁全 PASS。',
         '',
         '| 指标 | ' + ' | '.join(SC) + ' |',
         '|' + '---|' * (len(SC) + 1)]

for k in ob:
    if k not in n:
        continue
    cells = []
    for i in range(8):
        bv, nv = b[k][i], n[k][i]
        if k.startswith('IOMMU IOPS 稳态'):
            x, y = num(bv), num(nv)
            d = (y - x) / x * 100 if x else 0
            cells.append('%s → **%s** (%+.2f%%)' % (bv, nv, d))
        else:
            cells.append('%s → %s' % (bv, nv))
    lines.append('| %s | %s |' % (k, ' | '.join(cells)))

open(OUT, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
print('written:', OUT, 'rows=', len(ob))

# 控制台只打印 IOPS 与去重关键行
print()
for k in ob:
    if k.startswith('IOMMU IOPS 稳态') or 'hash 冲突' in k or 'bypass 任务数' in k or '实际占用 cacheline' in k:
        print(k)
        for i in range(8):
            print('   %-7s %s -> %s' % (SC[i], b[k][i], n[k][i]))
