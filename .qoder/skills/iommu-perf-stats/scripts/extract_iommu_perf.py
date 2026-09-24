#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Extract the full IOMMU performance metric set from an iommu_model simulation log.

Usage:
    python3 extract_iommu_perf.py <sim_log> [--json OUT.json] [--md OUT.md]

Only the tail of the log is read (the statistics block lives at the end), so
multi-hundred-MB logs parse in well under a second. Missing metrics print N/A.
"""
import re
import sys
import json
import argparse

TAIL_LINES = 20000


def tail_lines(path, n):
    """Return the last n lines of a (possibly huge) file without reading it all."""
    with open(path, 'rb') as f:
        f.seek(0, 2)
        pos = f.tell()
        buf = b''
        while pos > 0:
            step = min(1 << 20, pos)
            pos -= step
            f.seek(pos)
            buf = f.read(step) + buf
            if buf.count(b'\n') > n:
                break
        return [l.decode('utf-8', 'replace') for l in buf.split(b'\n')[-n:]]


def F(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def pct(a, b):
    return (100.0 * a / b) if b else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--json', default=None)
    ap.add_argument('--md', default=None)
    args = ap.parse_args()

    lines = tail_lines(args.log, TAIL_LINES)
    m = {}
    perdev = []
    section = ''
    ddr_pending = None  # 'main' | 'pf' for the DDR-reads distribution sub-blocks

    re_sec = re.compile(r'^=+\s*(.+?)\s*=+\s*$')
    for ln in lines:
        sm = re_sec.match(ln.strip())
        if sm:
            section = sm.group(1)
            ddr_pending = None
            continue

        g = re.search(r'\[MD_RP\] epoch=3 did=(\d+) injected=(\d+) completed=(\d+)', ln)
        if g:
            perdev.append((int(g.group(1)), int(g.group(2)), int(g.group(3))))
            continue
        g = re.search(r'\[MD_FAIRNESS\] (ALL|DATA) Mreq/s=([\d.]+) Jain=([\d.]+)', ln)
        if g:
            m['fairness_' + g.group(1)] = [F(g.group(2)), F(g.group(3))]
            continue
        g = re.search(r'\[MD_WINDOW\] mode=steady begin=(\d+) end=(\d+)', ln)
        if g:
            m['steady_window_ns'] = [F(g.group(1)), F(g.group(2))]
            continue

        # ---- Throughput / e2e / PTW counts / intervals ----
        if section == 'IOMMU / PTW Throughput (IOPS)':
            for pat, key in (
                (r'Sim Time:\s+([\d.]+) us', 'sim_time_us'),
                (r'IOMMU Completed:\s+(\d+) trans', 'iommu_completed'),
                (r'IOMMU IOPS:\s+([\d.]+) M trans/s', 'iommu_iops'),
                (r'PTW  Completed:\s+(\d+) tasks', 'ptw_completed'),
                (r'Main tasks:\s+(\d+)', 'ptw_main'),
                (r'Prefetch tasks:\s+(\d+)', 'ptw_prefetch'),
                (r'PTW  IOPS:\s+([\d.]+) M tasks/s', 'ptw_iops'),
                (r'PTW  avg DDR reads:\s+([\d.]+)', 'ptw_avg_ddr_all'),
                (r'IO   avg e2e lat:\s+([\d.]+) ns', 'e2e_avg_ns'),
                (r'IO   max e2e lat:\s+([\d.]+) ns', 'e2e_max_ns'),
                (r'Avg inject interval:\s+([\d.]+) ns', 'ptw_inject_interval_ns'),
                (r'Avg output interval:\s+([\d.]+) ns', 'ptw_output_interval_ns'),
                (r'Avg exec time:\s+([\d.]+) ns', 'ptw_group_exec_avg_ns'),
                (r'Completed groups:\s+(\d+)', 'ptw_groups'),
            ):
                g = re.search(pat, ln)
                if g:
                    m[key] = F(g.group(1))
            g = re.search(r'PTW DDR reads distribution \(MAIN tasks, count=(\d+)\)', ln)
            if g:
                ddr_pending = 'main'
                continue
            g = re.search(r'PTW DDR reads distribution \(PREFETCH tasks, count=(\d+)\)', ln)
            if g:
                ddr_pending = 'pf'
                continue
            g = re.search(r'Avg DDR reads:\s+([\d.]+)', ln)
            if g and ddr_pending:
                m['ptw_main_ddr' if ddr_pending == 'main' else 'ptw_pf_ddr'] = F(g.group(1))
            continue

        # ---- IOMMU port intervals ----
        if section == 'IOMMU Port Task Interval Statistics':
            g = re.search(r'Avg input interval:\s+([\d.]+) ns', ln)
            if g:
                m['in_interval_ns'] = F(g.group(1))
                continue
            g = re.search(r'Avg output interval:\s+([\d.]+) ns', ln)
            if g:
                m['out_interval_ns'] = F(g.group(1))
                continue

        # ---- Cache hit-rate table ----
        if section == 'IOMMU Cache Statistics Summary':
            g = re.match(r'\s*(dc_cache|pc_cache|pt_cache|msipt_cache|walker_ptw_c2|walker_ptw_c3)\s+'
                         r'(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)', ln)
            if g:
                m['cache_' + g.group(1)] = [int(g.group(2)), int(g.group(3)),
                                            int(g.group(4)), F(g.group(5))]
            continue

        # ---- Walker level hit distribution ----
        if section == 'VS-stage Walker Cache Statistics':
            g = re.search(r'VS Hit Rate:\s+([\d.]+)%', ln)
            if g:
                m['vs_hit'] = F(g.group(1))
                continue
            g = re.search(r'C3 \(leaf, 1 DDR\):\s+(\d+)/(\d+) = ([\d.]+)%', ln)
            if g:
                m['vs_c3'] = [int(g.group(1)), int(g.group(2)), F(g.group(3))]
                continue
            g = re.search(r'C2 \(L1, 2 DDR\):\s+(\d+)/(\d+) = ([\d.]+)%', ln)
            if g:
                m['vs_c2'] = [int(g.group(1)), int(g.group(2)), F(g.group(3))]
                continue
        if section == 'S2 Walker Cache Statistics':
            g = re.search(r'S2 Hit Rate:\s+([\d.]+)%', ln)
            if g:
                m['s2_hit'] = F(g.group(1))
                continue
            g = re.search(r'C3 \(leaf, 1 DDR\):\s+(\d+)/(\d+) = ([\d.]+)%', ln)
            if g:
                m['s2_c3'] = [int(g.group(1)), int(g.group(2)), F(g.group(3))]
                continue
            g = re.search(r'C2 \(L1, 2 DDR\):\s+(\d+)/(\d+) = ([\d.]+)%', ln)
            if g:
                m['s2_c2'] = [int(g.group(1)), int(g.group(2)), F(g.group(3))]
                continue

        # ---- Control (SQ/CQ/MSI) PT-cache hit ----
        if section == 'SQ/CQ/MSI PT Cache Hit Statistics':
            g = re.search(r'Control packets \(SQ\+CQ\+MSI\) total:\s+(\d+)', ln)
            if g:
                m['ctrl_total'] = int(g.group(1))
                continue
            g = re.search(r'PT Cache HIT \(bypass dedup/PTW\):\s+(\d+)', ln)
            if g:
                m['ctrl_pt_hits'] = int(g.group(1))
                continue
            g = re.search(r'PT Cache hit rate for ctrl:\s+([\d.]+)%', ln)
            if g:
                m['pt_ctrl_hit'] = F(g.group(1))
                continue

        # ---- Dedup cache: hit / conflict / bypass ----
        if section == 'Dedup Cache Multi-RAM Report':
            for pat, key, cast in (
                (r'Conflict count \(main MISS & set-full\):\s+(\d+)', 'dedup_conflict', int),
                (r'总查询\(lookup\):\s+(\d+)', 'dedup_lookups', int),
                (r'总命中率:\s+([\d.]+)%', 'dedup_hit', F),
                (r'Conflict bypass to PTW tasks:\s+(\d+)', 'dedup_bypass', int),
                (r'Bypass to PTW count:\s+(\d+)', 'dedup_buffull_bypass', int),
            ):
                g = re.search(pat, ln)
                if g:
                    m[key] = cast(g.group(1))
            continue

        # ---- Dedup Buffer peak / occupancy / hold ----
        if section == 'PT Dedup Buffer Statistics':
            for pat, key in (
                (r'Buffer Size:\s+(\d+) entries', 'buf_size'),
                (r'Peak Valid Count:\s+(\d+) entries', 'buf_peak'),
                (r'Avg hold:\s+([\d.]+) ns', 'hold_avg_ns'),
                (r'Max hold:\s+([\d.]+) ns', 'hold_max_ns'),
                (r'Min hold:\s+([\d.]+) ns', 'hold_min_ns'),
            ):
                g = re.search(pat, ln)
                if g:
                    m[key] = F(g.group(1))
            g = re.search(r'Avg Occupancy:\s+([\d.]+) entries / \d+ \(([\d.]+)%\)', ln)
            if g:
                m['buf_avg_occ'] = F(g.group(1))
                m['buf_avg_pct'] = F(g.group(2))
            continue

        if section == 'Dedup Buffer Hold Decomposition':
            g = re.search(r'(MAIN|SUSP) entries: n=(\d+)\s+avg W1=([\d.]+) W2=([\d.]+) W3=([\d.]+)\s*ns'
                          r'\s*\|\s*max\s*([\d.]+)/([\d.]+)/([\d.]+)', ln)
            if g:
                m['hold_' + g.group(1).lower()] = [int(g.group(2))] + [F(g.group(i)) for i in range(3, 9)]
            continue

        # ---- Outstanding / concurrency peaks ----
        if section == 'Outstanding Peak Statistics':
            for pat, key in (
                (r'IOMMU Global:\s+peak=\s*(\d+) / max=\s*(\d+)', 'conc_global'),
                (r'IOMMU Read \(场景13\):\s+peak=\s*(\d+) / max=\s*(\d+)', 'conc_read'),
                (r'IOMMU Write \(场景13\):\s+peak=\s*(\d+) / max=\s*(\d+)', 'conc_write'),
                (r'PTW:\s+peak=\s*(\d+) / max=\s*(\d+)', 'conc_ptw'),
            ):
                g = re.search(pat, ln)
                if g:
                    m[key] = [int(g.group(1)), int(g.group(2))]
            continue

        # ---- PTW ratio & concurrency ----
        if section == 'PTW Task Ratio Statistics':
            g = re.search(r'IOMMU input tasks \(total\):\s+(\d+)', ln)
            if g:
                m['iommu_input_total'] = int(g.group(1))
                continue
            g = re.search(r'PTW/IOMMU ratio:\s+([\d.]+)%', ln)
            if g:
                m['ptw_ratio_all'] = F(g.group(1))
                continue
        if section == 'PTW Concurrency & Execution Latency':
            g = re.search(r'Avg concurrency \(time-weighted\):\s+([\d.]+)', ln)
            if g:
                m['ptw_conc_avg'] = F(g.group(1))
                continue
            g = re.search(r'Max concurrency \(peak\):\s+(\d+) tasks\s+\(limit=(\d+)\)', ln)
            if g:
                m['ptw_conc_peak'] = int(g.group(1))
                m['ptw_conc_limit'] = int(g.group(2))
                continue
            g = re.search(r'Utilization \(avg/limit\):\s+([\d.]+)%', ln)
            if g:
                m['ptw_conc_util'] = F(g.group(1))
                continue
            g = re.search(r'\[ALL\]\s+count=(\d+)\s+avg=\s+([\d.]+) ns', ln)
            if g:
                m['ptw_exec_all_avg_ns'] = F(g.group(2))
                continue
            g = re.search(r'\[MAIN\]\s+count=(\d+)\s+avg=\s+([\d.]+) ns', ln)
            if g:
                m['ptw_exec_main_avg_ns'] = F(g.group(2))
                continue

    # ---------------- derived ratios ----------------
    pt = m.get('cache_pt_cache')
    ctrl_total = m.get('ctrl_total')
    if pt and ctrl_total:
        m['total_data_tasks'] = pt[0] - ctrl_total
    if m.get('dedup_conflict') is not None and m.get('dedup_lookups'):
        m['dedup_conflict_ratio_pct'] = pct(m['dedup_conflict'], m['dedup_lookups'])
    if m.get('dedup_bypass') is not None and m.get('total_data_tasks'):
        m['dedup_bypass_ratio_pct'] = pct(m['dedup_bypass'], m['total_data_tasks'])
    if m.get('ptw_completed') is not None and m.get('total_data_tasks'):
        m['ptw_to_data_ratio_pct'] = pct(m['ptw_completed'], m['total_data_tasks'])
    if pt and m.get('ctrl_pt_hits') is not None and m.get('total_data_tasks'):
        m['pt_data_hits'] = pt[1] - m['ctrl_pt_hits']
        m['pt_data_hit_pct'] = pct(m['pt_data_hits'], m['total_data_tasks'])
    if m.get('ptw_main') is not None and m.get('dedup_bypass') is not None:
        m['ptw_main_bypass'] = m['dedup_bypass']
        m['ptw_main_normal'] = m['ptw_main'] - m['dedup_bypass']

    # ---------------- render ----------------
    def g(k, fmt='{:.2f}', na='N/A'):
        v = m.get(k)
        if v is None:
            return na
        # integer format on a float capture -> cast to int
        if isinstance(v, float) and fmt.rstrip('}').endswith('d'):
            v = int(v)
        return fmt.format(v) if isinstance(v, (int, float)) else str(v)

    def gp(k):
        return g(k, '{:.2f}%')

    out = []
    out.append('# IOMMU 全量性能指标')
    out.append('')
    out.append('## 1. 任务数与均衡性')
    out.append('| 设备 | injected | completed |')
    out.append('|---|---|---|')
    for d, i, c in perdev:
        out.append('| %d | %d | %d |' % (d, i, c))
    fa, fd = m.get('fairness_ALL'), m.get('fairness_DATA')
    out.append('- Jain ALL / DATA: %s / %s ; 稳态吞吐 ALL %.2f / DATA %.2f Mreq/s'
               % (fa[1] if fa else 'N/A', fd[1] if fd else 'N/A',
                  fa[0] if fa else 0, fd[0] if fd else 0))
    out.append('')
    out.append('## 2. 吞吐 / 并发 / e2e / 端口间隔')
    out.append('| 指标 | 值 |')
    out.append('|---|---|')
    out.append('| 仿真总时长 | %s us |' % g('sim_time_us'))
    out.append('| IOMMU IOPS | %s M trans/s |' % g('iommu_iops'))
    out.append('| IOMMU 完成事务 | %s |' % g('iommu_completed'))
    cg, cr, cw = m.get('conc_global'), m.get('conc_read'), m.get('conc_write')
    out.append('| IOMMU 全局并发 peak/max | %s |' % ('%d/%d' % tuple(cg) if cg else 'N/A'))
    out.append('| IOMMU Read 并发 peak/max | %s |' % ('%d/%d' % tuple(cr) if cr else 'N/A'))
    out.append('| IOMMU Write 并发 peak/max | %s |' % ('%d/%d' % tuple(cw) if cw else 'N/A'))
    out.append('| IO avg e2e | %s ns |' % g('e2e_avg_ns', '{:.1f}'))
    out.append('| IO max e2e | %s ns |' % g('e2e_max_ns', '{:.1f}'))
    out.append('| 输入端口注入平均间隔 | %s ns |' % g('in_interval_ns'))
    out.append('| 输出端口注入平均间隔 | %s ns |' % g('out_interval_ns'))
    out.append('')
    out.append('## 3. Cache 命中率')
    for name, label in (('dc_cache', 'DC'), ('pc_cache', 'PC'), ('pt_cache', 'PT(总)'),
                        ('msipt_cache', 'MSIPT'), ('walker_ptw_c2', 'Walker C2(内部)'),
                        ('walker_ptw_c3', 'Walker C3(内部)')):
        c = m.get('cache_' + name)
        out.append('| %s | %s |' % (label, ('%.2f%% (%d/%d)' % (c[3] * 100, c[1], c[0])) if c else 'N/A'))
    out.append('| PT 512B 数据命中率 | %s |' % gp('pt_data_hit_pct'))
    out.append('| PT SQ/CQ/MSI 控制命中率 | %s |' % gp('pt_ctrl_hit'))
    v3, v2 = m.get('vs_c3'), m.get('vs_c2')
    out.append('| Walker(VS) C3 命中率 | %s |' % ('%.2f%% (%d/%d)' % (v3[2], v3[0], v3[1]) if v3 else 'N/A'))
    out.append('| Walker(VS) C2 命中率 | %s |' % ('%.2f%% (%d/%d)' % (v2[2], v2[0], v2[1]) if v2 else 'N/A'))
    s3 = m.get('s2_c3')
    out.append('| S2 Walker C3 命中率 | %s |' % ('%.2f%% (%d/%d)' % (s3[2], s3[0], s3[1]) if s3 else 'N/A'))
    out.append('| S2 Walker 整体命中率 | %s |' % gp('s2_hit'))
    out.append('')
    out.append('## 4. 去重 Cache / Buffer')
    out.append('| 指标 | 值 |')
    out.append('|---|---|')
    out.append('| 去重 Cache 命中率 | %s |' % gp('dedup_hit'))
    out.append('| 去重 hash 冲突次数 | %s |' % g('dedup_conflict', '{:d}'))
    out.append('|   占总查询比例 | %s |' % gp('dedup_conflict_ratio_pct'))
    out.append('| 去重 bypass 任务数 | %s |' % g('dedup_bypass', '{:d}'))
    out.append('|   占总数据任务比例 | %s |' % gp('dedup_bypass_ratio_pct'))
    out.append('| 去重 Buffer Peak | %s / %s |' % (g('buf_peak', '{:d}'), g('buf_size', '{:d}')))
    out.append('| 去重 Buffer 平均占用 | %s (%s) |' % (g('buf_avg_occ'), gp('buf_avg_pct')))
    out.append('| Buffer 满 bypass(正常预取) | %s |' % g('dedup_buffull_bypass', '{:d}'))
    out.append('| Buffer 持有延时 avg/max/min | %s / %s / %s ns |'
               % (g('hold_avg_ns', '{:.1f}'), g('hold_max_ns', '{:.1f}'), g('hold_min_ns', '{:.1f}')))
    for tag in ('main', 'susp'):
        h = m.get('hold_' + tag)
        if h:
            out.append('| Hold %s n/W1/W2/W3 (avg) | %d / %.1f / %.1f / %.1f ns |'
                       % (tag.upper(), h[0], h[1], h[2], h[3]))
            out.append('| Hold %s max W1/W2/W3 | %.1f / %.1f / %.1f ns |'
                       % (tag.upper(), h[4], h[5], h[6]))
    out.append('')
    out.append('## 5. PTW')
    out.append('| 指标 | 值 |')
    out.append('|---|---|')
    out.append('| PTW 总任务(主+预取) | %s |' % g('ptw_completed', '{:d}'))
    out.append('|   主任务 | %s |' % g('ptw_main', '{:d}'))
    out.append('|     其中正常任务 | %s |' % g('ptw_main_normal', '{:d}'))
    out.append('|     其中 bypass 任务 | %s |' % g('ptw_main_bypass', '{:d}'))
    out.append('|   预取任务 | %s |' % g('ptw_prefetch', '{:d}'))
    out.append('| PTW 占 IOMMU 全部输入比 | %s |' % gp('ptw_ratio_all'))
    out.append('| PTW 占 IOMMU 数据任务比 | %s |' % gp('ptw_to_data_ratio_pct'))
    cp = m.get('conc_ptw')
    out.append('| PTW 并发 peak/max | %s |' % ('%d/%d' % tuple(cp) if cp else 'N/A'))
    out.append('| PTW 并发 平均占用(时间加权) | %s |' % g('ptw_conc_avg', '{:.3f}'))
    out.append('| PTW 并发 利用率 | %s |' % gp('ptw_conc_util'))
    out.append('| PTW IOPS(主+预取) | %s M tasks/s |' % g('ptw_iops'))
    out.append('| PTW 主任务平均 DDR 次数 | %s |' % g('ptw_main_ddr'))
    out.append('| PTW 预取任务平均 DDR 次数 | %s |' % g('ptw_pf_ddr'))
    out.append('| PTW 执行延时 ALL 平均 | %s ns |' % g('ptw_exec_all_avg_ns', '{:.2f}'))
    out.append('| PTW 执行延时 MAIN 平均 | %s ns |' % g('ptw_exec_main_avg_ns', '{:.2f}'))
    out.append('| PTW 任务组(1主+D预取)平均执行 | %s ns |' % g('ptw_group_exec_avg_ns', '{:.1f}'))
    out.append('| PTW 注入任务平均间隔 | %s ns |' % g('ptw_inject_interval_ns'))
    out.append('| PTW 输出任务平均间隔 | %s ns |' % g('ptw_output_interval_ns'))
    report = '\n'.join(out)
    print(report)
    if args.md:
        with open(args.md, 'w', encoding='utf-8') as f:
            f.write(report + '\n')
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump({'metrics': m, 'per_device': perdev}, f, ensure_ascii=False, indent=2)
    return 0


if __name__ == '__main__':
    sys.exit(main())
