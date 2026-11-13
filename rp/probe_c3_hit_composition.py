#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[C3 命中构成拆解] 判定"命中率提升"是否转化为真实 DDR 工作量下降。

地址布局(rp/test_rp_nvme10_rr_arb_thread.cc):
  数据区  IOVA_BASE=0x100000, RANGE_MB=512  -> [0x100000, 0x20100000)
  SQ 区   0x22000000 ~ 0x22400000  (SQ_PAGES=45, 步长8页)
  CQ 区   0x22400000 ~ 0x22800000  (CQ_PAGES=24, 步长8页)
  PROBE   0x22800000
"""
import os, re, sys
from collections import Counter

D = {'8盘C2': '/tmp/nvrr6_ptfifo1024_20261109/s2_n8c2',
     '8盘C4': '/tmp/nvrr6_ptfifo1024_20261109/s3_n8c4',
     '8盘C1': '/tmp/nvrr6_ptfifo1024_20261109/s1_n8c1',
     '8盘C8': '/tmp/nvrr6_ptfifo1024_20261109/s7_n8c8'}

PAT = re.compile(r'\[WALKER_CACHE\] (HIT|MISS): (ptw_c[123]) .*?gscid=(\d+).*?iova=0x([0-9a-fA-F]+)')
PAT2 = re.compile(r'\[WALKER_CACHE\] (HIT|MISS)')


def zone(iova):
    if 0x100000 <= iova < 0x20100000:
        return 'DATA'
    if 0x22000000 <= iova < 0x22400000:
        return 'SQ'
    if 0x22400000 <= iova < 0x22800000:
        return 'CQ'
    if iova >= 0x22800000:
        return 'PROBE'
    return 'OTHER'


for tag, d in D.items():
    cnt = Counter()
    tot = 0
    with open(os.path.join(d, 'sim.log'), errors='ignore') as fh:
        for ln in fh:
            if '[WALKER_CACHE]' not in ln:
                continue
            m = PAT.search(ln)
            if not m:
                continue
            res, lvl, gscid, io = m.group(1), m.group(2), m.group(3), int(m.group(4), 16)
            cnt[(lvl, res, zone(io))] += 1
            tot += 1
    print('=== %s  (解析到 %d 条 WALKER_CACHE 事件)' % (tag, tot))
    for lvl in ('ptw_c3', 'ptw_c2', 'ptw_c1'):
        parts = []
        for z in ('DATA', 'SQ', 'CQ', 'PROBE', 'OTHER'):
            h = cnt[(lvl, 'HIT', z)]; mi = cnt[(lvl, 'MISS', z)]
            if h or mi:
                parts.append('%s: H%d/M%d' % (z, h, mi))
        print('   %-8s HIT合计=%6d  MISS合计=%6d   |  %s'
              % (lvl, sum(cnt[(lvl, 'HIT', z)] for z in ('DATA', 'SQ', 'CQ', 'PROBE', 'OTHER')),
                 sum(cnt[(lvl, 'MISS', z)] for z in ('DATA', 'SQ', 'CQ', 'PROBE', 'OTHER')),
                 '  '.join(parts)))
    print()
