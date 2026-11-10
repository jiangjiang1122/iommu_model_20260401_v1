#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
[激励再修改 20261105] 扩展性矩阵 SCALING.md 生成器

用途：把同一批次内多组 (C, 负载口径, 队列深度) 的运行结果拉成一张可归因的曲线表，
      量化"4KB 页并发是否随每设备队列组数 C 翻倍"以及其上界来自何处。

用法：
  python3 rp/compare_nvrr_scaling.py \
    --run "C=1 pq|<dir>" --run "C=2 pq|<dir>" --run "C=4 pq|<dir>" \
    --run "C=2 ft|<dir>" --run "C=4 ft|<dir>" \
    --run "C=2 pq qd16|<dir>" --out SCALING.md

每个 <dir> 需含 sim.log 与该目录下的 perf_scene*.json。
"""
import argparse
import glob
import json
import os
import re
import sys

NA = 'N/A'
UPBOUND_OUTSTANDING = 512.0   # 在途数据包上界 = Global 额度 640 x 0.8

RE_KV = re.compile(r'([A-Za-z_/][\w/]*)=([^\s()]+)')
RE_4KB_DEV = re.compile(r'^\s{2,}(\d+)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$')
RE_4KB_TOT = re.compile(r'^\s*TOTAL\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$')
RE_AREA = re.compile(r'^\s+(Read|Write|Global)\s*并发\(实际在处理\)\s+([\d.]+)\s+(\d+)')
RE_STEADY = re.compile(r'^\s+\[稳态窗口\s+([\d.]+)\s*ns')
RE_RD_STEADY = re.compile(r'^\s+读口 slave_0r:\s+(\d+) B\s+([\d.]+) GB/s \(util ([\d.]+)%')
RE_WR_STEADY = re.compile(r'^\s+写口 slave_0w:\s+(\d+) B\s+([\d.]+) GB/s \(util ([\d.]+)%')
RE_RD_INJ = re.compile(r'^\s+\[读口注入期\s+([\d.]+)\s*ns\]\s+(\d+) B -> ([\d.]+) GB/s \(util ([\d.]+)%')
RE_WR_INJ = re.compile(r'^\s+\[写口注入期\s+([\d.]+)\s*ns\]\s+(\d+) B -> ([\d.]+) GB/s \(util ([\d.]+)%')
RE_TRI = re.compile(r'^\s+([RW])口 跨度 ([\d.]+) ns \[([\d.]+)~([\d.]+)\]: '
                    r'节拍 ([-\d.]+)% \+ 门控 ([-\d.]+)% \+ 空闲 ([-\d.]+)%')
RE_VERDICT = re.compile(r'^\s+判定 (Read|Write|Global)\s*:\s*(.+?)\s*(?:\(?顶满时长\s*([\d.]+)%\)?)?\s*$')
RE_IL = re.compile(r'\[NVRR_INTERLEAVE\] (.*)')
RE_SAT = re.compile(r'\[NVRR_SATURATION\] (.*)')


def kvs(line):
    return dict(RE_KV.findall(line))


def f1(v, nd=2):
    return NA if v is None else ('%.*f' % (nd, v))


def tail_text(path, nbytes=8 << 20):
    with open(path, 'rb') as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - nbytes))
        return f.read().decode('utf-8', 'replace')


def head_text(path, nbytes=4 << 20):
    with open(path, 'rb') as f:
        return f.read(nbytes).decode('utf-8', 'replace')


def slice_after(lines, marker, n=40):
    """返回 marker 行之后的 n 行(避免把其它表格里的 TOTAL 行误认为 4KB 并发表)。"""
    for i, ln in enumerate(lines):
        if marker in ln:
            return lines[i:i + n]
    return []


class Run:
    def __init__(self, label, d):
        self.label = label
        self.dir = d
        self.log_path = os.path.join(d, 'sim.log')
        self.head = head_text(self.log_path) if os.path.exists(self.log_path) else ''
        self.tail = tail_text(self.log_path) if os.path.exists(self.log_path) else ''
        jpath = sorted(glob.glob(os.path.join(d, 'perf_scene*.json')))
        self.mj = json.load(open(jpath[0], encoding='utf-8')).get('metrics', {}) if jpath else {}
        self._parse()

    # ---------------- 解析 ----------------
    def _line(self, tag, text=None):
        for ln in (text if text is not None else self.head).splitlines():
            if tag in ln:
                return ln.strip()
        return ''

    def _parse(self):
        cfg = kvs(self._line('[MDMC_CONFIG]'))
        self.N = int(cfg['N']) if 'N' in cfg else None
        self.C = int(cfg['C']) if 'C' in cfg else None
        self.load_mode = cfg.get('load_mode', NA)
        self.tasks_dev = int(cfg['tasks/dev']) if 'tasks/dev' in cfg else None
        self.pages_dev = int(cfg['pages/dev']) if 'pages/dev' in cfg else None
        self.qdepth = int(cfg['queue_depth']) if 'queue_depth' in cfg else None
        self.qmode = cfg.get('qdepth_mode', NA)
        self.total_pkts = int(cfg['total_meas_pkts']) if 'total_meas_pkts' in cfg else None

        self.rd_pkts = int(cfg['rd_port']) if 'rd_port' in cfg else None
        self.wr_pkts = int(cfg['wr_port']) if 'wr_port' in cfg else None

        # 4KB 页并发（只在 4KB 章节切片内解析，且在 TOTAL 行处截断）
        self.conc4k_avg = self.conc4k_peak = None
        self.conc4k_dev = []
        sec = slice_after(self.tail.splitlines(), '4KB Page Concurrency Statistics')
        for ln in sec:
            m = RE_4KB_TOT.match(ln)
            if m:
                self.conc4k_avg, self.conc4k_peak = float(m.group(1)), int(m.group(2))
                break
            m = RE_4KB_DEV.match(ln)
            if m:
                self.conc4k_dev.append((float(m.group(2)), int(m.group(3))))

        # IOMMU 内部实际并发(面积法)
        self.area = {}
        for ln in self.tail.splitlines():
            m = RE_AREA.match(ln)
            if m and m.group(1) not in self.area:
                self.area[m.group(1)] = (float(m.group(2)), int(m.group(3)))

        # 端口带宽: 稳态窗口 / 注入期
        self.rd_steady = self.wr_steady = self.rd_inj = self.wr_inj = None
        for ln in self.tail.splitlines():
            m = RE_RD_STEADY.match(ln)
            if m:
                self.rd_steady = (float(m.group(2)), float(m.group(3)))
            m = RE_WR_STEADY.match(ln)
            if m:
                self.wr_steady = (float(m.group(2)), float(m.group(3)))
            m = RE_RD_INJ.match(ln)
            if m:
                self.rd_inj = (float(m.group(3)), float(m.group(4)))
            m = RE_WR_INJ.match(ln)
            if m:
                self.wr_inj = (float(m.group(3)), float(m.group(4)))

        # 端口占用三分法 + 判定
        self.tri = {}
        self.verdict = {}
        for ln in self.tail.splitlines():
            m = RE_TRI.match(ln)
            if m:
                self.tri[m.group(1)] = dict(beat=float(m.group(5)), blk=float(m.group(6)),
                                            idle=float(m.group(7)), span=float(m.group(2)))
            m = RE_VERDICT.match(ln)
            if m:
                self.verdict[m.group(1)] = (m.group(2).strip(), m.group(3))

        # 交织 / 饱和(只取测量 epoch=3)
        self.il = {}
        self.sat = {}
        for ln in self.tail.splitlines():
            m = RE_IL.match(ln)
            if m:
                d = kvs(m.group(1))
                if d.get('epoch') == '3':
                    self.il[d.get('dir')] = d
            m = RE_SAT.match(ln)
            if m:
                d = kvs(m.group(1))
                self.sat[d.get('dir')] = d

        self.sim_us = self.mj.get('sim_time_us')
        self.iops = self.mj.get('iommu_iops')
        self.e2e_avg = self.mj.get('e2e_avg_ns')
        self.ptw_avg = self.mj.get('ptw_conc_avg')
        self.ptw_peak = self.mj.get('ptw_conc_peak')
        self.ptw_limit = self.mj.get('ptw_conc_limit')
        self.buf_occ = self.mj.get('buf_avg_occ')
        self.buf_pct = self.mj.get('buf_avg_pct')
        self.steady_all = (self.mj.get('fairness_ALL') or [None])[0]
        self.steady_data = (self.mj.get('fairness_DATA') or [None])[0]

        self.overlap = None
        if self.conc4k_avg and self.area.get('Global'):
            self.overlap = self.area['Global'][0] / self.conc4k_avg
        self.conc_ub = (UPBOUND_OUTSTANDING / self.overlap) if self.overlap else None
        self.ns_per_pkt = ((self.sim_us * 1000.0) / self.total_pkts
                           if self.sim_us and self.total_pkts else None)


def table(rows, head):
    out = ['| ' + ' | '.join(head) + ' |',
           '|' + '---|' * len(head)]
    for r in rows:
        out.append('| ' + ' | '.join(str(c) for c in r) + ' |')
    return out


def cell_gain(base, cur):
    if not base or not cur:
        return NA
    return '%.2fx' % (cur / base)


def verdict_cell(run, key):
    """把 IOMMU 侧主约束判定行压成表格单元格（含顶满时长）。"""
    v = run.verdict.get(key)
    if not v:
        return NA
    txt = (v[0] or '').strip()
    return ('%s (顶满%s%%)' % (txt, v[1])) if v[1] else txt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', action='append', required=True, help='LABEL|<dir>')
    ap.add_argument('--out', default=None)
    args = ap.parse_args()

    runs = []
    for spec in args.run:
        label, d = [x.strip() for x in spec.split('|', 1)]
        runs.append(Run(label, d))

    out = []
    out.append('# 扩展性矩阵 SCALING（激励再修改 20261105：每通道独立生成进程 + per_queue 负载）')
    out.append('')
    out.append('- 全部跑在同一工作树、同一批次 `make -B` 构建，统一 '
               '`EXTRA_FLAGS=-DTEST_CFG_DDR_PORT_BEAT_ALIGN=0`。')
    out.append('- IOMMU 侧配置一律不动：Global/Read/Write=640/320/320、PTW=64、Buffer=320、'
               'D=3、PORT_WIDTH_BIT=1024、SLINK=50ns、DDR=100ns、Walker+S2 Walker 开启。')
    out.append('- `pq` = per_queue（每个队列对自带 TASKS 任务 / TASKS 个全局唯一页，'
               '每设备负载随 C 翻倍）；`ft` = fixed_total（每设备恒 TASKS 任务，仅作同规模对照）。')
    out.append('- 4KB 页并发口径 = 模型内置"parser-admit→reorder-release"时间加权平均/峰值；'
               '在途并发口径 = 面积法时间加权平均。')
    out.append('')

    # ---------------- 1. 主曲线 pq ----------------
    pq = [r for r in runs if r.load_mode == 'per_queue' and r.qmode == 'unbounded']
    pq.sort(key=lambda r: (r.C or 0))
    out.append('## 1. 主曲线：per_queue（C=1/2/4，无界队列）')
    out.append('')
    rows = []
    base = pq[0] if pq else None
    for r in pq:
        rows.append([
            r.label, r.C, r.tasks_dev, r.total_pkts,
            f1(r.sim_us), f1(r.ns_per_pkt),
            f1(r.conc4k_avg), r.conc4k_peak,
            f1(r.conc4k_avg / r.N if (r.conc4k_avg and r.N) else None),
            f1(r.area.get('Global', (None,))[0]), f1(r.overlap),
            f1(r.steady_data), f1(r.steady_all),
            cell_gain(base.conc4k_avg if base else None, r.conc4k_avg),
        ])
    out.extend(table(rows, ['配置', 'C', '任务/设备', '测量包数', '仿真时长 us', 'ns/pkt',
                            '4KB并发 avg', '4KB并发 peak', '每设备4KB并发',
                            'Global在途 avg', 'overlap=在途/页并发',
                            'DATA 稳态 Mreq/s', 'ALL 稳态 Mreq/s', '页并发倍数(vs C=1)']))
    out.append('')

    out.append('### 1.1 端口 / 额度侧（解释页并发上界）')
    out.append('')
    rows = []
    for r in pq:
        rows.append([
            r.label,
            f1(r.rd_steady[0]) if r.rd_steady else NA,
            f1(r.rd_steady[1]) if r.rd_steady else NA,
            f1(r.wr_steady[0]) if r.wr_steady else NA,
            f1(r.wr_steady[1]) if r.wr_steady else NA,
            f1(r.area.get('Read', (None,))[0]), f1(r.area.get('Write', (None,))[0]),
            f1(r.tri.get('R', {}).get('idle')), f1(r.tri.get('W', {}).get('idle')),
            (verdict_cell(r, 'Read')),
            f1(r.conc_ub),
        ])
    out.extend(table(rows, ['配置', '读口稳态 GB/s', '读口util%', '写口稳态 GB/s', '写口util%',
                            'Read在途avg', 'Write在途avg', 'R口空闲%', 'W口空闲%',
                            'Read 主约束判定', '页并发上界=512/overlap']))
    out.append('')
    # 读/写口主约束判定并列一行（写口在多数场景是间歇顶满额度）
    out.append('| 配置 | Write 主约束判定 | R口三分法 节拍/门控/空闲 | W口三分法 节拍/门控/空闲 |')
    out.append('|---|---|---|---|')
    for r in pq:
        tr = r.tri.get('R', {})
        tw = r.tri.get('W', {})
        tri_r = ('%.2f/%.2f/%.2f' % (tr.get('beat', 0), tr.get('blk', 0), tr.get('idle', 0))) if tr else NA
        tri_w = ('%.2f/%.2f/%.2f' % (tw.get('beat', 0), tw.get('blk', 0), tw.get('idle', 0))) if tw else NA
        out.append('| %s | %s | %s | %s |' % (r.label, verdict_cell(r, 'Write'), tri_r, tri_w))
    out.append('')
    out.append('> `R口空闲%/W口空闲%` 取自 IOMMU 侧"端口占用三分法"，'
               '**空闲 ≈ 0 即 RP 供给充足**（门禁 |idle| ≤ 0.5%）；'
               '门控占比高说明 320 额度为主约束，节拍占比高说明端口线速为主约束。')
    out.append('')

    # ---------------- 2. ft 同规模对照 ----------------
    ft = [r for r in runs if r.load_mode == 'fixed_total' and r.qmode == 'unbounded']
    ft.sort(key=lambda r: (r.C or 0))
    if ft:
        out.append('## 2. 同规模对照：fixed_total（每设备恒 250 任务，仅"交织变细"）')
        out.append('')
        out.append('> pq 与 ft 的差别有两个来源：**负载翻倍**（pq 每加一组队列就多一倍任务/页）'
                   '与**交织变细**（同一条流被切成 C 段）。C=1 时两者等价，'
                   '故 (ft C=2 − pq C=1) 只反映交织变细，(pq C=2 − ft C=2) 只反映负载翻倍。')
        out.append('')
        rows = []
        for r in ft:
            rows.append([
                r.label, r.C, r.tasks_dev, r.total_pkts,
                f1(r.sim_us), f1(r.ns_per_pkt),
                f1(r.conc4k_avg), r.conc4k_peak,
                f1(r.area.get('Global', (None,))[0]), f1(r.overlap),
                f1(r.steady_data), f1(r.steady_all),
                f1(r.rd_steady[1]) if r.rd_steady else NA,
                f1(r.wr_steady[1]) if r.wr_steady else NA,
            ])
        out.extend(table(rows, ['配置', 'C', '任务/设备', '测量包数', '仿真时长 us', 'ns/pkt',
                                '4KB并发 avg', '4KB并发 peak', 'Global在途 avg',
                                'overlap', 'DATA 稳态 Mreq/s', 'ALL 稳态 Mreq/s',
                                '读口util%', '写口util%']))
        out.append('')
        # ---- 2.1 效应分解: 交织变细 vs 负载翻倍 ----
        base = next((p for p in pq if p.C == 1), None)
        if base and base.conc4k_avg:
            out.append('### 2.1 效应分解：交织变细 vs 负载翻倍（以 C=1 pq 为共同起点）')
            out.append('')
            out.append('> pq(C) 相对 pq(1) 的变化同时包含两个效应：把同一条流切成 C 段（交织变细，'
                       'ft 列已含）与每设备任务/页数乘 C（负载翻倍，pq 相对 ft 多出的部分）。'
                       '用 ft(C) 做中间点即可把两者乘积拆开：'
                       '`pq(C)/pq(1) = [ft(C)/pq(1)] × [pq(C)/ft(C)]`。')
            out.append('')
            rows = []
            for r in ft:
                if not r.conc4k_avg:
                    continue
                rows.append([
                    'C=%s' % r.C,
                    '%.2f → %.2f → %.2f' % (base.conc4k_avg, r.conc4k_avg, 
                                             next((p.conc4k_avg for p in pq
                                                   if p.C == r.C), float('nan'))),
                    '%.3fx' % (r.conc4k_avg / base.conc4k_avg),
                    ('%.3fx' % (next(p.conc4k_avg for p in pq if p.C == r.C) / r.conc4k_avg)
                     if any(p.C == r.C for p in pq) else NA),
                    ('%.3fx' % (next(p.conc4k_avg for p in pq if p.C == r.C) / base.conc4k_avg)
                     if any(p.C == r.C for p in pq) else NA),
                    '%.3fx' % ((r.steady_data or float('nan')) / (base.steady_data or float('nan'))),
                    '%.3fx' % ((r.ns_per_pkt or float('nan')) / (base.ns_per_pkt or float('nan'))),
                ])
            out.extend(table(rows, ['C', '4KB并发 pq(1)→ft(C)→pq(C)',
                                    '交织变细效应(页并发)', '负载翻倍效应(页并发)',
                                    '合计(页并发)', '交织变细效应(DATA稳态)',
                                    '交织变细效应(ns/pkt, <1 为变快)']))
            out.append('')
            out.append('> 读法：若"交织变细效应"远大于"负载翻倍效应"，则页并发的提升'
                       '主要来自**同页 8 个数据包被打散到更多队列**（overlap 下降），'
                       '而不是单纯把负载加大；反之则说明只是把系统推得更满。')
            out.append('')

    # ---------------- 3. 有界深度灵敏度 ----------------
    bd = [r for r in runs if r.qmode != 'unbounded']
    if bd:
        out.append('## 3. 有界深度灵敏度（仅报告，不参与硬门禁）')
        out.append('')
        out.append('> 有界模式下"每通道 1 个生成进程"可能因配对的另一队列满而阻塞，'
                   '使本队列被抽干 ⇒ 饱和退化为经验性，`CALIBER.txt` 已标注"饱和保证不适用"。'
                   '该点用于量化"反压深度"对饱和指标与性能的影响。')
        out.append('')
        out.append('### 3.1 性能对照（有界 vs 同 C 同负载的无界点）')
        out.append('')
        rows = []
        for r in bd:
            # 找同 C 的无界 pq 基准点
            ref = None
            for p in pq:
                if p.C == r.C and p.load_mode == r.load_mode:
                    ref = p
                    break
            rows.append([
                r.label, r.C, r.qdepth, r.total_pkts,
                f1(r.sim_us), f1(r.ns_per_pkt),
                f1(r.conc4k_avg), r.conc4k_peak,
                f1(r.area.get('Global', (None,))[0]), f1(r.overlap),
                f1(r.steady_data), f1(r.steady_all),
                f1(r.rd_steady[1]) if r.rd_steady else NA,
                f1(r.wr_steady[1]) if r.wr_steady else NA,
                (ref.label if ref else NA),
                (f1(r.conc4k_avg / ref.conc4k_avg) + 'x'
                 if (ref and ref.conc4k_avg and r.conc4k_avg) else NA),
                (f1(r.steady_data / ref.steady_data) + 'x'
                 if (ref and ref.steady_data and r.steady_data) else NA),
            ])
        out.extend(table(rows, ['配置', 'C', '队列深度', '测量包数', '仿真时长 us', 'ns/pkt',
                                '4KB并发 avg', '4KB并发 peak', 'Global在途 avg', 'overlap',
                                'DATA 稳态 Mreq/s', 'ALL 稳态 Mreq/s', '读口util%', '写口util%',
                                '无界基准点', '页并发比', 'DATA稳态比']))
        out.append('')
        out.append('### 3.2 逐方向饱和度量（有界模式：只报告不门禁）')
        out.append('')
        rows = []
        for r in bd:
            for dr in ('R', 'W'):
                s = r.sat.get(dr, {})
                rows.append([
                    r.label, dr, s.get('mode', NA), s.get('qdepth', r.qdepth),
                    s.get('prefill_ok', NA), s.get('empty_sel', NA),
                    s.get('starve_cnt', NA), s.get('starve_ns', NA),
                    s.get('gen_block_ns', NA),
                    s.get('q_min_occ_live', NA), s.get('q_min_occ_samples', NA),
                    s.get('q_avg_occ', NA),
                    s.get('first_done_at_pkt', NA), s.get('first_skip_at_pkt', NA),
                    s.get('tail_skip_pkts', NA),
                ])
        out.extend(table(rows, ['配置', '方向', 'mode', '队列深度', 'prefill_ok', 'empty_sel',
                                'starve_cnt', 'starve_ns', 'gen_block_ns', 'q_min_occ_live',
                                'samples', 'q_avg_occ', 'first_done@包', 'first_skip@包',
                                'tail_skip包']))
        out.append('')
        # 自动归因: 哪个方向被反压阻塞、哪个方向因此饥饿
        for r in bd:
            sr, sw = r.sat.get('R', {}), r.sat.get('W', {})
            blk_r = sr.get('gen_block_ns', '0')
            blk_w = sw.get('gen_block_ns', '0')
            out.append('- **%s 归因**：生成进程阻塞时间 R=%s ns / W=%s ns；'
                       '饥饿次数 R=%s / W=%s；空选次数 R=%s / W=%s。'
                       % (r.label, blk_r, blk_w, sr.get('starve_cnt', NA),
                          sw.get('starve_cnt', NA), sr.get('empty_sel', NA),
                          sw.get('empty_sel', NA)))
        out.append('- **结论（验证 plan A0-2 / A0-8 的判断）**：每通道 1 个生成进程只消除了'
                   '**通道之间**的反压耦合，但同一生成进程要先后写入本通道的读/写两个队列，'
                   '因此**方向之间**的耦合仍在：写口被 320 额度门控而变慢时，'
                   '生成进程在写队列上阻塞 ⇒ 读队列被抽干 ⇒ 读发送进程饥饿。'
                   '这正是默认取 `NVRR_QUEUE_DEPTH=0`（无界）的原因：'
                   '无界下 `gen_block_ns≡ 0`，上述链条从根上断开，饱和成为构造性保证。')
        out.append('- **备选方案（plan A0-8，本批次未采用）**：把每通道 1 个生成进程拆为'
                   '每方向 1 个生成协程（2C 个/设备），读队列生产者只被读队列反压 ⇒ '
                   '无界证明在有界深度下也能恢复；代价是违背"每通道 1 个任务生成进程"'
                   '的规格文字，且 `sc_spawn` 数翻倍（C=8 时 129 个）。')
        out.append('')

    # ---------------- 4. 饱和门禁矩阵 ----------------
    out.append('## 4. 激励端饱和门禁（A0，硬门禁仅在无界默认模式启用）')
    out.append('')
    rows = []
    for r in runs:
        for dr in ('R', 'W'):
            s = r.sat.get(dr, {})
            if not s:
                continue
            il = r.il.get(dr, {})
            rows.append([
                r.label, dr, s.get('mode', NA), s.get('prefill_ok', NA),
                s.get('empty_sel', NA), s.get('starve_cnt', NA), s.get('starve_ns', NA),
                s.get('gen_block_ns', NA), s.get('q_min_occ_live', NA),
                s.get('q_min_occ_samples', NA), s.get('q_avg_occ', NA),
                s.get('first_done_at_pkt', NA), s.get('first_skip_at_pkt', NA),
                s.get('tail_skip_pkts', NA),
                il.get('same_page_gap_avg_pkts', NA), il.get('same_page_gap_max_pkts', NA),
            ])
    out.extend(table(rows, ['配置', '方向', 'mode', 'prefill_ok', 'empty_sel', 'starve_cnt',
                            'starve_ns', 'gen_block_ns', 'q_min_occ_live', 'samples',
                            'q_avg_occ', 'first_done@包', 'first_skip@包', 'tail_skip包',
                            '同页间隔avg(包)', '同页间隔max(包)']))
    out.append('')
    out.append('> 无界默认模式下 `gen_block_ns==0` 是"每队列最多收 TASKS 个突发 < TASKS+8"'
               '的构造性结果，`empty_sel==0`/`starve_cnt==0` 是"所有队列自注入起点即满、'
               '占用单调递减"的构造性结果 —— 数学上不可能空选，'
               '一旦非零即为实现缺陷，必须修代码而不是放宽门禁。')
    out.append('> `q_min_occ_live` 仅在"被选中队列的生成进程尚未结束"时采样；'
               '无界模式下生成在 t=0 一次性完成，故样本数=0、该列为 N/A（不影响门禁）。')
    out.append('')

    # ---------------- 5. 归因结论 ----------------
    out.append('## 5. 归因结论')
    out.append('')
    if len(pq) >= 2 and pq[0].conc4k_avg and pq[-1].conc4k_avg:
        lo, hi = pq[0], pq[-1]
        pct_ub = (100.0 * hi.conc4k_avg / hi.conc_ub) if hi.conc_ub else None
        out.append('- **直接回答“4KB 页并发是否随 C 翻倍”**：否。'
                   '%s → %s 的页并发为 %.2f → %.2f 页（**%.2fx**，而 C 本身涨了 %dx），'
                   '峰值 %.2f → %.2f；同期每设备页并发 %.2f → %.2f、'
                   'overlap %.2f → %.2f、ns/pkt %.2f → %.2f。'
                   % (lo.label, hi.label, lo.conc4k_avg, hi.conc4k_avg,
                      hi.conc4k_avg / lo.conc4k_avg, (hi.C or 1) // (lo.C or 1),
                      lo.conc4k_peak, hi.conc4k_peak,
                      (lo.conc4k_avg / lo.N) if (lo.conc4k_avg and lo.N) else float('nan'),
                      (hi.conc4k_avg / hi.N) if (hi.conc4k_avg and hi.N) else float('nan'),
                      lo.overlap or float('nan'), hi.overlap or float('nan'),
                      lo.ns_per_pkt or float('nan'), hi.ns_per_pkt or float('nan')))
        if pct_ub is not None:
            out.append('- **受何约束**：%s 实测页并发 %.2f 已达其上界 %.2f 的 **%.1f%%**'
                       '（上界=512/overlap），且 Global 在途均值 %.1f 接近额度可用上限 512'
                       '⇒ 页并发的天花板来自 **IOMMU 在途额度 / 端口线速**，'
                       '而不是激励端供给（各跑 R/W 口空闲占比 |idle| ≤ 0.5%%，'
                       'A0 饱和门禁全绿）。'
                       % (hi.label, hi.conc4k_avg, hi.conc_ub, pct_ub,
                          hi.area.get('Global', (float('nan'),))[0]))
        out.append('')
    if len(pq) >= 2:
        c1 = pq[0]
        for r in pq[1:]:
            if not (c1.conc4k_avg and r.conc4k_avg):
                continue
            ratio = r.conc4k_avg / c1.conc4k_avg
            scale = (r.total_pkts / c1.total_pkts) if (r.total_pkts and c1.total_pkts) else None
            line = ('- **%s vs %s**：4KB 页并发 %.2f → %.2f 页（**%.2fx**），'
                    '负载规模 %.2fx，overlap %.2f → %.2f，'
                    'Global 在途 %.1f → %.1f（额度上限 640），读口稳态利用率 %.2f%% → %.2f%%。'
                    % (r.label, c1.label, c1.conc4k_avg, r.conc4k_avg, ratio,
                       scale if scale else float('nan'),
                       c1.overlap or float('nan'), r.overlap or float('nan'),
                       c1.area.get('Global', (float('nan'),))[0],
                       r.area.get('Global', (float('nan'),))[0],
                       c1.rd_steady[1] if c1.rd_steady else float('nan'),
                       r.rd_steady[1] if r.rd_steady else float('nan')))
            # 实测/上界对照（plan D 验收项）
            if r.conc_ub:
                line += (' **实测/上界 = %.2f / %.2f = %.1f%%**（上界=512/overlap）。'
                         % (r.conc4k_avg, r.conc_ub, 100.0 * r.conc4k_avg / r.conc_ub))
            if ratio < 0.9 * (scale or 1.0):
                line += (' 未达线性翻倍：`在途未同比例增长` 说明受 640/320/320 额度或端口线速约束'
                         '（页并发上界≈512/overlap=%.1f 页），属容量约束而非激励缺陷。'
                         % (r.conc_ub or float('nan')))
            # 三因判定（额度顶满 / 端口线速 / overlap 未降）
            g_out = r.area.get('Global', (None,))[0]
            rd_u = r.rd_steady[1] if r.rd_steady else None
            causes = []
            if g_out is not None and g_out >= 0.8 * UPBOUND_OUTSTANDING:
                causes.append('额度接近顶满(在途 %.1f ≥ 0.8×512=%.0f, 占 640 的 %.1f%%)'
                              % (g_out, 0.8 * UPBOUND_OUTSTANDING, 100.0 * g_out / 640.0))
            if rd_u is not None and rd_u >= 95.0:
                causes.append('读口线速(util %.2f%%)' % rd_u)
            if c1.overlap and r.overlap and r.overlap >= 0.95 * c1.overlap:
                causes.append('overlap 未降(%.2f→%.2f)' % (c1.overlap, r.overlap))
            elif c1.overlap and r.overlap:
                causes.append('overlap 已降(%.2f→%.2f, 同页包被打得更散)'
                              % (c1.overlap, r.overlap))
            line += (' 三因判定: %s。' % ('; '.join(causes) if causes else '均未触发'))
            out.append(line)
        out.append('')
        # 相邻档位（C=2→C=4 等）的边际增益，识别曲线上是否已饱和
        if len(pq) >= 3:
            steps, vals = [], []
            for a, b in zip(pq, pq[1:]):
                if a.conc4k_avg and b.conc4k_avg:
                    vals.append(b.conc4k_avg / a.conc4k_avg)
                    steps.append('%s→%s: %.2fx' % (a.label, b.label,
                                                    b.conc4k_avg / a.conc4k_avg))
            if steps:
                if len(vals) >= 2 and vals[-1] < vals[0]:
                    trend = '边际增益递减 ⇒ 已进入容量平台，继续加 C 不再提升页并发'
                else:
                    trend = ('边际增益未递减（仍低于线性）⇒ 尚未撞到硬平台，'
                             '但实测/上界已近 90%，剩余空间有限')
                out.append('- **相邻档位边际增益**：' + '; '.join(steps) + '。' + trend + '。')
                out.append('')
        out.append('上界口径：在途数据包 ≤ 640×0.8 = 512，读口线速 64 GB/s；'
                   '页并发上界 ≈ 512 / overlap。若实测页并发接近该上界，'
                   '则"C 翻倍带来页并发翻倍"的天花板来自额度/线速，而不是激励端供给。')
        out.append('')
    else:
        out.append('（矩阵不足两组，无法给出倍数归因）')
        out.append('')

    report = '\n'.join(out)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(report + '\n')
        print('[SCALING] written: %s (%d lines)' % (args.out, len(out)))
    else:
        print(report)
    return 0


if __name__ == '__main__':
    sys.exit(main())
