#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[PT Cache 命中率 作用域重算 20261110]

背景(已定位的缺陷):
  skill 提取器的 PT 数据命中率 = (pt_cache.hits - ctrl_pt_hits) / (pt_cache.accesses - ctrl_total)
  - pt_cache.hits 只在 PTCache::lookup_pt_ram 真 CL HIT 时 record_hit (pt_cache.cpp:190)
  - pt_cache.accesses 对 "HIT dedup placeholder" 也 record_access, 但该分支不 record_hit
  - ctrl_pt_cache_hit_count 在 pt_hit_response 线程累加, 且位于 is_placeholder 判定之前
    (iommu_perf_pt_cache_response.cc:55) => 含占位 HIT
  三者的作用域两两不重合, 故分子可为负。

本脚本不依赖任何上述聚合计数器, 直接用逐包事件重算:
  1) requests.csv 提供 task_id -> (epoch, bytes)  ；bytes==512 为数据包(统一口径)
  2) sim.log 的 [PT_CACHE] 行提供每个包的三分支之一:
       HIT regular CL / HIT dedup placeholder / MISS   (忽略 MISS 的重复回执行)
  3) 交叉列联后给出三种口径:
       A 真CL命中率   = regular / (regular + miss)      <- 真正查了 PT RAM 的那部分
       B 全口径命中率 = regular / 该组包总数             <- 与模型 accesses 同分母
       C 免PTW率      = (regular + placeholder) / 该组包总数
"""
import csv, os, re, sys

SCENES = [
    ('real8',  '/tmp/nvrr6_real8_calib/v1'),
    ('8盘C1',  '/tmp/nvrr6_ptfifo1024_20261109/s1_n8c1'),
    ('8盘C2',  '/tmp/nvrr6_ptfifo1024_20261109/s2_n8c2'),
    ('8盘C4',  '/tmp/nvrr6_ptfifo1024_20261109/s3_n8c4'),
    ('8盘C8',  '/tmp/nvrr6_ptfifo1024_20261109/s7_n8c8'),
    ('16盘C1', '/tmp/nvrr6_ptfifo1024_20261109/s4_n16c1'),
    ('16盘C2', '/tmp/nvrr6_ptfifo1024_20261109/s5_n16c2'),
    ('16盘C4', '/tmp/nvrr6_ptfifo1024_20261109/s6_n16c4'),
    ('16盘C8', '/tmp/nvrr6_ptfifo1024_20261109/s8_n16c8'),
]

BR = {
    'HIT regular CL':        'reg',
    'HIT dedup placeholder': 'ph',
    'MISS':                  'mis',
}
PAT = re.compile(r'\[PT_CACHE\] task_id=(\d+) -> (HIT regular CL|HIT dedup placeholder|MISS)(?![ a-z]*received)')


def run_dir(d):
    for f in os.listdir(d):
        if f.endswith('_requests.csv'):
            return os.path.join(d, f)
    return None


def analyze(tag, d):
    csvp = run_dir(d)
    logp = os.path.join(d, 'sim.log')
    if not csvp or not os.path.isfile(logp):
        print('%s MISSING', tag); return None
    # task_id -> bytes (测量段 epoch=3 优先; 若重复取首次出现)
    tb, ep = {}, {}
    with open(csvp) as fh:
        for row in csv.DictReader(fh):
            t = int(row['task_id'])
            if t not in tb:
                tb[t] = int(row['bytes']); ep[t] = int(row['epoch'])
    cnt = {}  # (is_data, epoch3, branch) -> n
    unmatched = 0
    with open(logp, errors='ignore') as fh:
        for ln in fh:
            if '[PT_CACHE]' not in ln:
                continue
            m = PAT.search(ln)
            if not m:
                continue
            t = int(m.group(1)); br = BR[m.group(2)]
            if t not in tb:
                unmatched += 1; continue
            key = (tb[t] == 512, ep[t] == 3, br)
            cnt[key] = cnt.get(key, 0) + 1
    return tb, cnt, unmatched


def cell(cnt, is_data, ep3, br):
    return cnt.get((is_data, ep3, br), 0)


print('# PT Cache 命中率作用域重算（逐包事件，不依赖聚合计数器）')
print()
print('口径: bytes==512 为数据包, 其余为控制包; 区间 = 每包在 PT_CACHE 判定点的三分支之一。')
print('  A 真CL命中率 = regular/(regular+miss)  ← 实际查了 PT RAM 的部分')
print('  B 全口径命中率 = regular/组内包总数      ← 与模型 accesses 同分母(含占位)')
print('  C 免PTW率 = (regular+placeholder)/组内包总数')
print()
hdr = ('场景', '组', '包数', '真CL HIT', '占位HIT', 'MISS',
       'A 真CL命中', 'B 全口径', 'C 免PTW', '旧口径(skill)')
print('| ' + ' | '.join(hdr) + ' |')
print('|' + '|'.join(['---'] * len(hdr)) + '|')

rows = []
for tag, d in SCENES:
    r = analyze(tag, d)
    if r is None:
        continue
    tb, cnt, unmatched = r
    for is_data, lab in ((True, '数据(512B)'), (False, '控制(非512B)')):
        reg = cell(cnt, is_data, True, 'reg')
        ph = cell(cnt, is_data, True, 'ph')
        mis = cell(cnt, is_data, True, 'mis')
        tot = reg + ph + mis
        look = reg + mis
        a = 100.0 * reg / look if look else float('nan')
        b = 100.0 * reg / tot if tot else float('nan')
        c = 100.0 * (reg + ph) / tot if tot else float('nan')
        print('| %s | %s | %d | %d | %d | %d | %.2f%% | %.2f%% | %.2f%% | 见下 |'
              % (tag, lab, tot, reg, ph, mis, a, b, c))
    if unmatched:
        print('| %s | 未匹配 task_id | %d |' % (tag, unmatched))
    rows.append((tag, cnt))
