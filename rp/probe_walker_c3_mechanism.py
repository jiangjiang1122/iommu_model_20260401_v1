#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[Walker C3 / S2 C3 命中率机理取证 20261111]

回答两问:
  Q1 16盘 C1->C8 为何 VS walker C3 命中率单调上升 (14.30 -> 42.80%)
  Q2 为何 S2 C3 恒在 30% 多 (34.9/37.7/37.5/36.3%) 且几乎不随 C 变

做法:
  (a) 从 sim.log 提取 walker_ptw_c1/c2/c3 三级链式 accesses/hits/misses, 验证
      C3miss==C2acc, C2miss==C1acc 的回退链;
  (b) 从 requests.csv 按 admit_ns 排序, 以 (did, iova>>21) 为 VS C3 段键、
      (did, iova>>21 对应的大页) 为 S2 段键, 计算:
        distinct 段数 / 段平均包数 / 同段相邻访问的平均间隔包数 /
        重访距离 < 256(C3容量) 的比例  ← 这是 LRU 命中率的直接几何代理量
      同页间隔(iova>>12)一并算出作对照。
"""
import csv, os, re, statistics
from collections import defaultdict

SC = [('16盘C1', '/tmp/nvrr6_ptfifo1024_20261109/s4_n16c1'),
      ('16盘C2', '/tmp/nvrr6_ptfifo1024_20261109/s5_n16c2'),
      ('16盘C4', '/tmp/nvrr6_ptfifo1024_20261109/s6_n16c4'),
      ('16盘C8', '/tmp/nvrr6_ptfifo1024_20261109/s8_n16c8'),
      ('8盘C1',  '/tmp/nvrr6_ptfifo1024_20261109/s1_n8c1'),
      ('8盘C2',  '/tmp/nvrr6_ptfifo1024_20261109/s2_n8c2'),
      ('8盘C4',  '/tmp/nvrr6_ptfifo1024_20261109/s3_n8c4'),
      ('8盘C8',  '/tmp/nvrr6_ptfifo1024_20261109/s7_n8c8'),
      ('real8',  '/tmp/nvrr6_real8_calib/v1')]

C3_CAP = 64 * 4   # 256 条目 (base_sets=64 x ptw_c3.num_ways=4)

# 只取"带小数 hit_rate"的那张表(Cache 总表); 其余报表段第4字段是整数 0, 会误覆盖。
ROW = re.compile(r'^(walker_ptw_c[123])\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+\.\d+)')


def chain(d):
    """取 log 尾部统计区的三级回退链。同一名多次出现时保留首次(Cache 总表)。"""
    out = {}
    with open(os.path.join(d, 'sim.log'), errors='ignore') as fh:
        fh.seek(0, os.SEEK_END)
        sz = fh.tell()
        fh.seek(max(0, sz - 8000000))
        data = fh.read()
    for ln in data.splitlines():
        m = ROW.match(ln.strip())
        if m and m.group(1) not in out:
            out[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)), float(m.group(5)))
    return out


def s2_block(d):
    """S2 Walker Cache Statistics 段原文(找 C3 相关行)"""
    with open(os.path.join(d, 'sim.log'), errors='ignore') as fh:
        fh.seek(0, os.SEEK_END)
        sz = fh.tell()
        fh.seek(max(0, sz - 8000000))
        tail = fh.read()
    i = tail.find('S2 Walker Cache Statistics')
    return tail[i:i + 900] if i >= 0 else ''


def seg_stats(d):
    p = [os.path.join(d, f) for f in os.listdir(d) if f.endswith('_requests.csv')][0]
    rows = []
    with open(p) as fh:
        for r in csv.DictReader(fh):
            if r['epoch'] != '3':
                continue
            rows.append((float(r['admit_ns']), int(r['did']), int(r['iova']), int(r['bytes'])))
    rows.sort()
    out = {}
    for key_shift, name in ((21, 'seg2m'), (12, 'page4k')):
        idx = defaultdict(list)
        for n, (a, did, iova, b) in enumerate(rows):
            idx[(did, iova >> key_shift)].append(n)
        gaps = []
        for k, v in idx.items():
            for x, y in zip(v, v[1:]):
                gaps.append(y - x)
        near = sum(1 for g in gaps if g < C3_CAP)
        out[name] = (len(idx), len(rows),
                     statistics.mean(gaps) if gaps else 0.0,
                     (near / len(gaps) * 100.0) if gaps else 0.0,
                     statistics.median(gaps) if gaps else 0.0)
    return out


print('## (a) VS Walker 三级回退链 (accesses / hits / misses / hit_rate)')
print()
print('| 场景 | C3 acc | C3 hit | C3 miss | C3命中率 | C2 acc | C2 hit | C2命中率 | C1 acc | C1命中率 | 链式对账 |')
print('|---|---|---|---|---|---|---|---|---|---|---|')
for tag, d in SC:
    c = chain(d)
    if 'walker_ptw_c3' not in c:
        print('| %s | 无数据 |' % tag); continue
    a3, h3, m3, r3 = c['walker_ptw_c3']
    a2, h2, m2, r2 = c['walker_ptw_c2']
    a1, h1, m1, r1 = c['walker_ptw_c1']
    ok = 'C3miss==C2acc %s ; C2miss==C1acc %s' % ('✓' if m3 == a2 else '✗(%d/%d)' % (m3, a2),
                                                  '✓' if m2 == a1 else '✗')
    print('| %s | %d | %d | %d | %.2f%% | %d | %d | %.2f%% | %d | %.2f%% | %s |'
          % (tag, a3, h3, m3, r3 * 100, a2, h2, r2 * 100, a1, r1 * 100, ok))

print()
print('## (b) 段级时间局部性 (测量段, 按 admit_ns 全局包序; C3 容量=256条目)')
print()
print('| 场景 | 2MB段distinct | 总包数 | 段均包数 | 同段间隔均值(包) | 同段间隔中位数 | 间隔<256占比 | 4KB页distinct | 同页间隔均值 |')
print('|---|---|---|---|---|---|---|---|---|')
for tag, d in SC:
    res = seg_stats(d)
    s = res['seg2m']; p = res['page4k']
    nd, nrow, mean_gap, near_pct, med_gap = s
    print('| %s | %d | %d | %.2f | %.2f | %.1f | %.2f%% | %d | %.2f |'
          % (tag, nd, nrow, nrow / nd if nd else 0, mean_gap, med_gap, near_pct,
             p[0], p[2]))
print()
print('随机访问理论命中率 = 256 / distinct段数 :')
for tag, d in SC:
    nd = seg_stats(d)['seg2m'][0]
    print('  %-8s distinct=%4d  256/distinct=%5.2f%%' % (tag, nd, 25600.0 / nd if nd else 0))

print()
print('## (c) S2 Walker Cache Statistics 段 (16盘C1 / 16盘C8 / 8盘C1)')
for tag in ('16盘C1', '16盘C8', '8盘C1'):
    d = dict(SC)[tag]
    blk = s2_block(d)
    print('--- %s ---' % tag)
    print('\n'.join(blk.splitlines()[:16]))
    print()
