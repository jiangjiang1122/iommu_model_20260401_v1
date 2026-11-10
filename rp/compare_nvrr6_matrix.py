#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[N×C 扩展性矩阵 20261106] 6 场景全量性能指标并列对比。

用法:
    python3 rp/compare_nvrr6_matrix.py \
        --scene "场景1 8盘1通道|<run_dir>" --scene "场景2 8盘2通道|<run_dir>" ... \
        --batch nvrr6_<ts> --out COMPARE6.md

每个 <run_dir> 需含:
    sim.log                 仿真完整日志(用于抓激励口径/门禁旁证行)
    perf*.json              iommu-perf-stats skill 的 extract_iommu_perf.py 产物
    gate.txt                (可选) tmp/nvrr_gate_check.py 的门禁报告

指标行与 .qoder/skills/iommu-perf-stats 的 5 大段逐行同名同序, 只是把"每场景一份
纵向表"转置成"一行指标 × N 列场景", 便于横向对齐; 数值行额外给出相对场景1 的倍率。
"""
import argparse
import glob
import json
import os
import re

NA = 'N/A'
RE_KV = re.compile(r'([A-Za-z_/][\w/]*)=([^\s()]+)')
# 以下正则与 rp/compare_nvrr_scaling.py 保持一致(同一批日志段, 不得各自发明格式)
RE_4KB_TOT = re.compile(r'^\s*TOTAL\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$')
RE_AREA = re.compile(r'^\s+(Read|Write|Global)\s*并发\(实际在处理\)\s+([\d.]+)\s+(\d+)')
RE_RD_STEADY = re.compile(r'^\s+读口 slave_0r:\s+(\d+) B\s+([\d.]+) GB/s \(util ([\d.]+)%')
RE_WR_STEADY = re.compile(r'^\s+写口 slave_0w:\s+(\d+) B\s+([\d.]+) GB/s \(util ([\d.]+)%')
RE_RD_INJ = re.compile(r'^\s+\[读口注入期\s+([\d.]+)\s*ns\]\s+(\d+) B -> ([\d.]+) GB/s \(util ([\d.]+)%')
RE_WR_INJ = re.compile(r'^\s+\[写口注入期\s+([\d.]+)\s*ns\]\s+(\d+) B -> ([\d.]+) GB/s \(util ([\d.]+)%')
RE_TRI = re.compile(r'^\s+([RW])口 跨度 ([\d.]+) ns \[([\d.]+)~([\d.]+)\]: '
                    r'节拍 ([-\d.]+)% \+ 门控 ([-\d.]+)% \+ 空闲 ([-\d.]+)%')
RE_VERDICT = re.compile(r'^\s+判定 (Read|Write|Global)\s*:\s*(.+?)\s*(?:\(?顶满时长\s*([\d.]+)%\)?)?\s*$')
RE_MAXDEV = re.compile(r'Max single-device peak:\s+(\d+)')
RE_Q_PEAK = re.compile(r'peak tasks queued:\s+(\d+)\s+\(read (\d+) / write (\d+)\)')
RE_Q_AVG = re.compile(r'time-weighted avg tasks queued:\s+([\d.]+)\s+\(read ([\d.]+) / write ([\d.]+)\)')
# 在途数据包上界 = Global 额度 640 x 0.8 (与 compare_nvrr_scaling.py 同值)
UPBOUND_OUTSTANDING = 512.0


def slice_after(lines, marker, n=40):
    """返回 marker 行之后的 n 行(避免把其它表格里的 TOTAL 行误认为 4KB 并发表)。"""
    for i, ln in enumerate(lines):
        if marker in ln:
            return lines[i:i + n]
    return []


def kvs(line):
    return dict(RE_KV.findall(line))


def tail_text(path, nbytes=8 << 20):
    with open(path, 'rb') as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - nbytes))
        return f.read().decode('utf-8', 'replace')


def head_text(path, nbytes=16 << 20):
    # [窗口扩容 20261106] 原为 4MB: N=16 时建表段输出约 4.26MB, [MDMC_CONFIG]/
    #   [MDMC_UNIQUE_OK]/[NVRR_PATTERN_OK] 均位于日志头部, 4MB 窗口会把它们截掉。
    with open(path, 'rb') as f:
        return f.read(nbytes).decode('utf-8', 'replace')


def find_line(lines, tag):
    """在已切好的行列表里找首个包含 tag 的行(避免反复 splitlines 大字符串)。"""
    for ln in lines:
        if tag in ln:
            return ln.strip()
    return ''


def fnum(v, fmt='%.2f'):
    return NA if v is None else (fmt % v)


# ---------------------------------------------------------------------------
# 运行点
# ---------------------------------------------------------------------------
class Run:
    def __init__(self, label, d):
        self.label = label
        self.dir = d
        log = os.path.join(d, 'sim.log')
        # 头部 16MB + 尾部 8MB 切一次行列表后复用; 日志可达 ~100MB, 不得全量读。
        lines = []
        if os.path.exists(log):
            lines = head_text(log).splitlines() + tail_text(log).splitlines()
        self.lines = lines
        jp = sorted(glob.glob(os.path.join(d, 'perf*.json')))
        self.m, self.perdev = {}, []
        if jp:
            with open(jp[0], encoding='utf-8') as f:
                j = json.load(f)
            self.m = j.get('metrics', {})
            self.perdev = j.get('per_device', [])
        whole = lines
        self.cfg = kvs(find_line(whole, '[MDMC_CONFIG]'))
        self.pat = kvs(find_line(whole, '[NVRR_PATTERN_OK]'))
        self.res_line = find_line(whole, '[NVRR_RESULT]')
        self.res = kvs(self.res_line)
        self.uniq = kvs(find_line(whole, '[MDMC_UNIQUE_OK]'))
        self.stim = find_line(whole, '[NVRR_STIMULUS]')
        self.supply = find_line(whole, '[MDMC_SUPPLY]')
        self.ctrl = find_line(whole, '[NVRR_CTRL_SPREAD]')
        self.delta = kvs(find_line(whole, '[MDMC_DELTA]'))
        gp = os.path.join(d, 'gate.txt')
        self.gate = ''
        if os.path.exists(gp):
            with open(gp, encoding='utf-8', errors='replace') as f:
                for ln in f:
                    if 'GATE RESULT' in ln:
                        self.gate = ln.strip()
        # 门禁优先级: gate.txt 结论 > [NVRR_RESULT] 是否 PASS
        if self.gate:
            self.verdict = 'PASS' if '= PASS' in self.gate else 'FAIL'
        elif 'PASS' in self.res_line:
            self.verdict = 'PASS(无门禁报告)'
        else:
            self.verdict = NA
        # [MDMC_CONFIG] 用 N=/C=, [MDMC_UNIQUE_OK] 用 devices=/chan/dev= => 两边兼容取值
        self.N = self.cfg.get('N') or self.uniq.get('devices')
        self.C = self.cfg.get('C') or self.uniq.get('chan/dev')
        self._parse_model_sections()

    def _parse_model_sections(self):
        """从日志尾部抽 iommu-perf-stats skill 未覆盖、但本矩阵必需的模型侧指标。"""
        tail = '\n'.join(self.lines[-20000:]) if len(self.lines) > 20000 else '\n'.join(self.lines)
        tl = tail.splitlines()
        # 4KB 页并发: 只在章节切片内解析, 且在 TOTAL 行处截断
        self.conc4k_avg = self.conc4k_peak = self.conc4k_maxdev = None
        for ln in slice_after(tl, '4KB Page Concurrency Statistics'):
            mm = RE_4KB_TOT.match(ln)
            if mm:
                self.conc4k_avg, self.conc4k_peak = float(mm.group(1)), int(mm.group(2))
                break
        for ln in tl:
            mm = RE_MAXDEV.search(ln)
            if mm:
                self.conc4k_maxdev = int(mm.group(1))
        self.q_peak = self.q_avg = None
        for ln in tl:
            mm = RE_Q_PEAK.search(ln)
            if mm:
                self.q_peak = (int(mm.group(1)), int(mm.group(2)), int(mm.group(3)))
            mm = RE_Q_AVG.search(ln)
            if mm:
                self.q_avg = (float(mm.group(1)), float(mm.group(2)), float(mm.group(3)))
        # IOMMU 内部实际在途并发(面积法)
        self.area = {}
        for ln in tl:
            mm = RE_AREA.match(ln)
            if mm and mm.group(1) not in self.area:
                self.area[mm.group(1)] = (float(mm.group(2)), int(mm.group(3)))
        # 端口带宽: 稳态窗口 / 注入期
        self.rd_steady = self.wr_steady = self.rd_inj = self.wr_inj = None
        for ln in tl:
            mm = RE_RD_STEADY.match(ln)
            if mm:
                self.rd_steady = (float(mm.group(2)), float(mm.group(3)))
            mm = RE_WR_STEADY.match(ln)
            if mm:
                self.wr_steady = (float(mm.group(2)), float(mm.group(3)))
            mm = RE_RD_INJ.match(ln)
            if mm:
                self.rd_inj = (float(mm.group(3)), float(mm.group(4)))
            mm = RE_WR_INJ.match(ln)
            if mm:
                self.wr_inj = (float(mm.group(3)), float(mm.group(4)))
        # 端口占用三分法 + 主约束判定
        self.tri, self.verdict = {}, {}
        for ln in tl:
            mm = RE_TRI.match(ln)
            if mm:
                self.tri[mm.group(1)] = dict(span=float(mm.group(2)), beat=float(mm.group(5)),
                                             blk=float(mm.group(6)), idle=float(mm.group(7)))
            mm = RE_VERDICT.match(ln)
            if mm:
                self.verdict[mm.group(1)] = (mm.group(2).strip(), mm.group(3))
        # 派生: overlap = Global 在途 avg / 4KB 页并发 avg; 页并发上界 = 512/overlap
        self.overlap = None
        if self.conc4k_avg and self.area.get('Global'):
            self.overlap = self.area['Global'][0] / self.conc4k_avg
        self.conc_ub = (UPBOUND_OUTSTANDING / self.overlap) if self.overlap else None
        tot = self.m.get('sim_time_us')
        npk = self.res.get('expected')
        self.ns_per_pkt = (tot * 1000.0 / float(npk)) if (tot and npk) else None

    def g(self, k, fmt='%.2f'):
        return fnum(self.m.get(k), fmt)

    def gp(self, k):
        return fnum(self.m.get(k), '%.2f%%')


# ---------------------------------------------------------------------------
# 指标行定义: (段, 行名, 取值函数 -> (显示串, 可比较数值 or None))
# ---------------------------------------------------------------------------
def cache_cell(m, key):
    c = m.get('cache_' + key)
    if not c:
        return NA, None
    return '%.2f%% (%d/%d)' % (c[3] * 100, c[1], c[0]), c[3] * 100


def hit3_cell(m, key):
    c = m.get(key)
    if not c:
        return NA, None
    return '%.2f%% (%d/%d)' % (c[2], c[0], c[1]), c[2]


def peakmax_cell(m, key):
    c = m.get(key)
    if not c:
        return NA, None
    return '%d/%d' % (c[0], c[1]), float(c[0])


def fair_cell(m, key):
    c = m.get('fairness_' + key)
    if not c:
        return NA, None
    return '%.2f Mreq/s (Jain %s)' % (c[0], fnum(c[1], '%.4f')), c[0]


def hold_cell(m, key, which):
    h = m.get('hold_' + key)
    if not h:
        return NA, None
    if which == 'avg':
        return '%d / %.1f / %.1f / %.1f ns' % (h[0], h[1], h[2], h[3]), h[1]
    return '%.1f / %.1f / %.1f ns' % (h[4], h[5], h[6]), h[4]


def sc(m, key, fmt='%.2f'):
    v = m.get(key)
    return fnum(v, fmt), (float(v) if v is not None else None)


def ROWS():
    r = []
    A = r.append
    # ---- 1. 任务数与均衡性 ----
    # [口径命名 20261106] 与 rp/extract_nvrr_full_metrics.py 保持一致:
    #   "IOPS 稳态" = [MD_FAIRNESS] ALL/DATA Mreq/s, 窗口 W=(t10,t90] => **主口径**
    #   "IOPS 全程" = iommu_iops, 分母为整段仿真时长(含建表/预热/收尾排空) => 仅作交叉核对
    #   两者不得混用, 也不得拿全程值去对基线的稳态值。
    A(('1. 任务数与均衡性', 'IOPS 稳态 ALL (Mreq/s) 【主口径】', lambda m: fair_cell(m, 'ALL')))
    A(('1. 任务数与均衡性', 'IOPS 稳态 DATA (Mreq/s) 【主口径】', lambda m: fair_cell(m, 'DATA')))
    # ---- 2. 吞吐 / 并发 / e2e / 端口间隔 ----
    S = '2. 吞吐/并发/e2e/端口间隔'
    A((S, '仿真总时长 us', lambda m: sc(m, 'sim_time_us')))
    A((S, 'IOPS 全程 (M trans/s) 【非主口径】', lambda m: sc(m, 'iommu_iops')))
    A((S, 'IOMMU 完成事务', lambda m: sc(m, 'iommu_completed', '%d')))
    A((S, 'IOMMU 全局并发 peak/max', lambda m: peakmax_cell(m, 'conc_global')))
    A((S, 'IOMMU Read 并发 peak/max', lambda m: peakmax_cell(m, 'conc_read')))
    A((S, 'IOMMU Write 并发 peak/max', lambda m: peakmax_cell(m, 'conc_write')))
    A((S, 'IO avg e2e ns', lambda m: sc(m, 'e2e_avg_ns', '%.1f')))
    A((S, 'IO max e2e ns', lambda m: sc(m, 'e2e_max_ns', '%.1f')))
    A((S, '输入端口注入平均间隔 ns', lambda m: sc(m, 'in_interval_ns')))
    A((S, '输出端口注入平均间隔 ns', lambda m: sc(m, 'out_interval_ns')))
    # ---- 3. Cache 命中率 ----
    S = '3. Cache 命中率'
    for key, label in (('dc_cache', 'DC'), ('pc_cache', 'PC'), ('pt_cache', 'PT(总)'),
                       ('msipt_cache', 'MSIPT'), ('walker_ptw_c2', 'Walker C2(内部)'),
                       ('walker_ptw_c3', 'Walker C3(内部)')):
        A((S, '%s 命中率' % label, lambda m, k=key: cache_cell(m, k)))
    A((S, 'PT 512B 数据命中率', lambda m: sc(m, 'pt_data_hit_pct', '%.2f%%')))
    A((S, 'PT SQ/CQ/MSI 控制命中率', lambda m: sc(m, 'pt_ctrl_hit', '%.2f%%')))
    A((S, 'Walker(VS) C3 命中率', lambda m: hit3_cell(m, 'vs_c3')))
    A((S, 'Walker(VS) C2 命中率', lambda m: hit3_cell(m, 'vs_c2')))
    A((S, 'Walker(VS) 整体命中率', lambda m: sc(m, 'vs_hit', '%.2f%%')))
    A((S, 'S2 Walker C3 命中率', lambda m: hit3_cell(m, 's2_c3')))
    A((S, 'S2 Walker C2 命中率', lambda m: hit3_cell(m, 's2_c2')))
    A((S, 'S2 Walker 整体命中率', lambda m: sc(m, 's2_hit', '%.2f%%')))
    # ---- 4. 去重 Cache / Buffer ----
    S = '4. 去重 Cache/Buffer'
    A((S, '去重 Cache 命中率', lambda m: sc(m, 'dedup_hit', '%.2f%%')))
    A((S, '去重 hash 冲突次数', lambda m: sc(m, 'dedup_conflict', '%d')))
    A((S, '  占总查询比例', lambda m: sc(m, 'dedup_conflict_ratio_pct', '%.2f%%')))
    A((S, '去重 bypass 任务数', lambda m: sc(m, 'dedup_bypass', '%d')))
    A((S, '  占总数据任务比例', lambda m: sc(m, 'dedup_bypass_ratio_pct', '%.2f%%')))
    A((S, '去重 Buffer Peak / Size', lambda m: (
        '%s / %s' % (fnum(m.get('buf_peak'), '%d'), fnum(m.get('buf_size'), '%d')),
        float(m['buf_peak']) if m.get('buf_peak') is not None else None)))
    A((S, '去重 Buffer 平均占用', lambda m: (
        '%s (%s)' % (fnum(m.get('buf_avg_occ')), fnum(m.get('buf_avg_pct'), '%.2f%%')),
        m.get('buf_avg_occ'))))
    A((S, 'Buffer 满 bypass(正常预取)', lambda m: sc(m, 'dedup_buffull_bypass', '%d')))
    A((S, 'Buffer 持有延时 avg ns', lambda m: sc(m, 'hold_avg_ns', '%.1f')))
    A((S, 'Buffer 持有延时 max ns', lambda m: sc(m, 'hold_max_ns', '%.1f')))
    A((S, 'Buffer 持有延时 min ns', lambda m: sc(m, 'hold_min_ns', '%.1f')))
    for tag in ('main', 'pf', 'susp'):
        A((S, 'Hold %s n/W1/W2/W3 (avg)' % tag.upper(),
           lambda m, t=tag: hold_cell(m, t, 'avg')))
        A((S, 'Hold %s max W1/W2/W3' % tag.upper(),
           lambda m, t=tag: hold_cell(m, t, 'max')))
    # ---- 5. PTW ----
    S = '5. PTW'
    A((S, 'PTW 总任务(主+预取)', lambda m: sc(m, 'ptw_completed', '%d')))
    A((S, '  主任务', lambda m: sc(m, 'ptw_main', '%d')))
    A((S, '    其中正常任务', lambda m: sc(m, 'ptw_main_normal', '%d')))
    A((S, '    其中 bypass 任务', lambda m: sc(m, 'ptw_main_bypass', '%d')))
    A((S, '  预取任务', lambda m: sc(m, 'ptw_prefetch', '%d')))
    A((S, 'PTW 占 IOMMU 全部输入比', lambda m: sc(m, 'ptw_ratio_all', '%.2f%%')))
    A((S, 'PTW 占 IOMMU 数据任务比', lambda m: sc(m, 'ptw_to_data_ratio_pct', '%.2f%%')))
    A((S, 'PTW 并发 peak/max', lambda m: peakmax_cell(m, 'conc_ptw')))
    A((S, 'PTW 并发 平均占用(时间加权)', lambda m: sc(m, 'ptw_conc_avg', '%.3f')))
    A((S, 'PTW 并发 利用率', lambda m: sc(m, 'ptw_conc_util', '%.2f%%')))
    A((S, 'PTW IOPS(主+预取) M tasks/s', lambda m: sc(m, 'ptw_iops')))
    A((S, 'PTW 主任务平均 DDR 次数', lambda m: sc(m, 'ptw_main_ddr')))
    A((S, 'PTW 预取任务平均 DDR 次数', lambda m: sc(m, 'ptw_pf_ddr')))
    A((S, 'PTW 执行延时 ALL 平均 ns', lambda m: sc(m, 'ptw_exec_all_avg_ns')))
    A((S, 'PTW 执行延时 MAIN 平均 ns', lambda m: sc(m, 'ptw_exec_main_avg_ns')))
    A((S, 'PTW 任务组(1主+D预取)平均执行 ns', lambda m: sc(m, 'ptw_group_exec_avg_ns', '%.1f')))
    A((S, 'PTW 注入任务平均间隔 ns', lambda m: sc(m, 'ptw_inject_interval_ns')))
    A((S, 'PTW 输出任务平均间隔 ns', lambda m: sc(m, 'ptw_output_interval_ns')))
    return r


def table(rows, head):
    out = ['| ' + ' | '.join(str(h) for h in head) + ' |',
           '|' + '---|' * len(head)]
    for r in rows:
        out.append('| ' + ' | '.join(str(c) for c in r) + ' |')
    return out


def gain_cell(base, cur):
    if not base or cur is None:
        return NA
    return '%.3fx (%+.1f%%)' % (cur / base, (cur / base - 1.0) * 100.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', action='append', required=True, help='LABEL|<run_dir>')
    ap.add_argument('--batch', default='')
    ap.add_argument('--out', default=None)
    args = ap.parse_args()

    runs = [Run(*[x.strip() for x in s.split('|', 1)]) for s in args.scene]
    out = []
    out.append('# N×C 多设备扩展性矩阵 · 全量性能指标对比')
    out.append('')
    if args.batch:
        out.append('- 批次: `%s`' % args.batch)
    out.append('- 激励: NVMe 十包任务（SQ 读 64B → 8×512B 数据 → CQ 写 16B），**不含 MSI**；'
               '每设备严格"1 读任务 + 1 写任务"交替。')
    out.append('- 负载口径: `fixed_total` —— 每设备任务数与 C 无关，C 只把同一条任务流'
               '切给 C 组读/写口队列（同索引派发），因此**各场景总包数恒定**，'
               '横向差异只来自"设备数 N"与"通道交织粒度 C"。')
    out.append('- 仲裁: 全系统仅 2 个发送进程（读口/写口各 1，独占端口），'
               '嵌套轮询（外层设备 RR、内层设备内队列 RR），每步只出队 1 个包。')
    out.append('- IOMMU 侧配置逐宏一致: Global/Read/Write=640/320/320、PTW=64、Buffer=320、'
               'D=3、ADM=1、PORT_WIDTH_BIT=1024、SLINK=50ns、DDR=100ns、'
               'Walker+S2 Walker 开启、SPLIT_RW=1、IOVA=512MB、'
               '`EXTRA_FLAGS=-DTEST_CFG_DDR_PORT_BEAT_ALIGN=0`。')
    out.append('- 指标提取: `.qoder/skills/iommu-perf-stats/scripts/extract_iommu_perf.py`'
               '（全部场景同一脚本，只读日志尾部 20000 行）。')
    out.append('- **口径声明（重要）**：**主口径 = 稳态窗口 W=(t10,t90]**'
               '（完成事务的 10%~90%，峰值/均值均在 W 内重算），'
               '对应行名带「稳态」与【主口径】标记；'
               '行名带「全程」的是 `iommu_iops`（分母为整段仿真时长，'
               '含建表/预热/收尾排空），**仅作交叉核对，不得与基线的稳态值直接相比**。'
               '稳态口径的完整拆解另见同批次 `EXTENDED_METRICS.md`'
               '（由项目既有 `rp/extract_nvrr_full_metrics.py` 生成）。')
    out.append('')

    # ---------------- 0. 激励口径与门禁 ----------------
    out.append('## 0. 场景配置 / 激励口径 / 门禁')
    out.append('')
    rows = []
    for r in runs:
        rows.append([r.label, r.N, r.C,
                     r.cfg.get('tasks/dev', r.pat.get('tasks/dev', NA)),
                     r.cfg.get('load_mode', NA),
                     r.res.get('expected', NA), r.res.get('completed', NA),
                     r.res.get('rd_port_pkts', NA), r.res.get('wr_port_pkts', NA),
                     r.uniq.get('total_unique', NA),
                     r.cfg.get('queue_depth', NA), r.cfg.get('qdepth_mode', NA),
                     r.verdict])
    out.extend(table(rows, ['场景', 'N(盘)', 'C(通道)', '任务/设备', '负载口径',
                            '测量包数(期望)', '测量包数(实际)', '读口包数', '写口包数',
                            '全局唯一页', '队列深度', '队列模式', '门禁']))
    out.append('')
    rows = []
    for r in runs:
        rows.append([r.label,
                     r.pat.get('data512', NA), r.pat.get('sq64', NA), r.pat.get('cq16', NA),
                     r.pat.get('msi', NA), r.pat.get('bytes', NA),
                     r.pat.get('avg_bytes', NA), r.pat.get('order', NA),
                     r.pat.get('msiptp', NA), r.pat.get('sq_route', NA)])
    out.extend(table(rows, ['场景', '512B数据包', 'SQ(64B)', 'CQ(16B)', 'MSI',
                            '总字节', '平均包长B', '任务内包序', 'MSIPTP', 'SQ路由']))
    out.append('')
    rows = []
    for r in runs:
        ck = kvs(r.ctrl)
        rows.append([r.label, ck.get('sq_pages', NA), ck.get('cq_pages', NA),
                     ck.get('sq_pkts_per_page', NA), ck.get('cq_pkts_per_page', NA),
                     ck.get('expect_ctrl_pt_hit_pct', NA),
                     r.delta.get('PTW', NA), r.delta.get('DDR_reads', NA)])
    out.extend(table(rows, ['场景', 'SQ散布页数', 'CQ散布页数', 'SQ包/页', 'CQ包/页',
                            '控制PT命中理论下限%', 'PTW增量', 'DDR读增量']))
    out.append('')
    out.append('> 供给旁证（`[MDMC_SUPPLY]`，starve=0 表示端口从不因缺任务空等）：')
    for r in runs:
        out.append('> - %s: `%s`' % (r.label, r.supply or NA))
    out.append('')

    # ---------------- 1..5 全量指标并列 ----------------
    # 行 = 指标(与 iommu-perf-stats skill 同名同序), 列 = 场景; 末列给出末场景/场景1 倍率。
    all_rows = ROWS()
    sections = []
    for sec, _label, _fn in all_rows:
        if sec not in sections:
            sections.append(sec)
    for sec in sections:
        items = [(l, f) for (s, l, f) in all_rows if s == sec]
        rows = []
        for label, fn in items:
            cells, vals = [], []
            for r in runs:
                disp, val = fn(r.m)
                cells.append(disp)
                vals.append(val)
            row = [label] + cells
            if len(runs) > 1:
                row.append(gain_cell(vals[0], vals[-1]))
            rows.append(row)
        header = ['指标'] + [r.label for r in runs]
        if len(runs) > 1:
            header.append('%s / %s' % (runs[-1].label, runs[0].label))
        out.append('## %s' % sec)
        out.append('')
        out.extend(table(rows, header))
        out.append('')

    # ---------------- 6. 端口带宽 / 在途并发 / 4KB 页并发 ----------------
    out.append('## 6. 端口带宽 / 在途并发 / 4KB 页并发（模型侧扩展指标）')
    out.append('')
    out.append('> 这一段是 `iommu-perf-stats` skill 未覆盖、但对 N×C 矩阵最关键的指标：'
               '**4KB 页并发**直接反映“同时压在页表/去重子系统上的不同页数”，'
               '是解释 PTW 负载与去重 Buffer 占用变化的因。'
               '4KB 页并发口径 = 模型内置“parser-admit→reorder-release”时间加权平均/峰值；'
               '在途并发口径 = 面积法时间加权平均。')
    out.append('')
    rows = []
    for r in runs:
        rows.append([r.label,
                     fnum(r.ns_per_pkt, '%.3f'),
                     fnum(r.conc4k_avg, '%.3f'), r.conc4k_peak if r.conc4k_peak is not None else NA,
                     r.conc4k_maxdev if r.conc4k_maxdev is not None else NA,
                     fnum(r.area.get('Global', (None,))[0]), fnum(r.overlap, '%.3f'),
                     fnum(r.conc_ub, '%.1f')])
    out.extend(table(rows, ['场景', 'ns/pkt', '4KB页并发 avg', '4KB页并发 peak',
                            '单设备peak最大', 'Global在途 avg',
                            'overlap=在途/页并发', '页并发上界=512/overlap']))
    out.append('')
    rows = []
    for r in runs:
        rows.append([r.label,
                     fnum(r.area.get('Read', (None,))[0]), fnum(r.area.get('Write', (None,))[0]),
                     fnum(r.area.get('Global', (None,))[0]),
                     (r.area.get('Read', (None, None))[1] if r.area.get('Read') else NA),
                     (r.area.get('Write', (None, None))[1] if r.area.get('Write') else NA),
                     (r.area.get('Global', (None, None))[1] if r.area.get('Global') else NA)])
    out.extend(table(rows, ['场景', 'Read在途 avg', 'Write在途 avg', 'Global在途 avg',
                            'Read peak', 'Write peak', 'Global peak']))
    out.append('')
    rows = []
    for r in runs:
        rows.append([r.label,
                     fnum(r.rd_steady[0]) if r.rd_steady else NA,
                     fnum(r.rd_steady[1]) if r.rd_steady else NA,
                     fnum(r.wr_steady[0]) if r.wr_steady else NA,
                     fnum(r.wr_steady[1]) if r.wr_steady else NA,
                     fnum(r.rd_inj[0]) if r.rd_inj else NA,
                     fnum(r.rd_inj[1]) if r.rd_inj else NA,
                     fnum(r.wr_inj[0]) if r.wr_inj else NA,
                     fnum(r.wr_inj[1]) if r.wr_inj else NA])
    out.extend(table(rows, ['场景', '读口稳态 GB/s', '读口util%', '写口稳态 GB/s', '写口util%',
                            '读口注入期 GB/s', '读口注入期util%',
                            '写口注入期 GB/s', '写口注入期util%']))
    out.append('')
    rows = []
    for r in runs:
        tr, tw = r.tri.get('R', {}), r.tri.get('W', {})
        rows.append([r.label,
                     fnum(tr.get('span'), '%.1f'), fnum(tr.get('beat')), fnum(tr.get('blk')),
                     fnum(tr.get('idle')),
                     fnum(tw.get('span'), '%.1f'), fnum(tw.get('beat')), fnum(tw.get('blk')),
                     fnum(tw.get('idle'))])
    out.extend(table(rows, ['场景', 'R口跨度 ns', 'R口节拍%', 'R口门控%', 'R口空闲%',
                            'W口跨度 ns', 'W口节拍%', 'W口门控%', 'W口空闲%']))
    out.append('')
    rows = []
    for r in runs:
        vr, vw = r.verdict.get('Read'), r.verdict.get('Write')
        rows.append([r.label,
                     ('%s (顶满%s%%)' % (vr[0], vr[1])) if vr else NA,
                     ('%s (顶满%s%%)' % (vw[0], vw[1])) if vw else NA,
                     ('%d (r%d/w%d)' % r.q_peak) if r.q_peak else NA,
                     ('%.1f (r%.1f/w%.1f)' % r.q_avg) if r.q_avg else NA])
    out.extend(table(rows, ['场景', 'Read 主约束判定', 'Write 主约束判定',
                            '排队任务 peak(读/写)', '排队任务 avg(读/写)']))
    out.append('')
    out.append('> 空闲≈ 0（含小幅负值）即 RP 供给充足；负值只说明“节拍+门控”累加窗口'
               '比注入跨度多出一点（边界/重叠记账误差），即端口零空闲；'
               '门控占比高 => 320 额度为主约束，节拍占比高 => 端口线速为主约束。')
    out.append('')

    # ---------------- 7. 逐设备任务数与公平性 ----------------
    out.append('## 7. 逐设备任务数（epoch=3 测量段）与 Jain 公平性')
    out.append('')
    rows = []
    for r in runs:
        pd = r.perdev
        if not pd:
            rows.append([r.label, NA, NA, NA, NA, NA, NA])
            continue
        inj = [int(x[1]) for x in pd]
        comp = [int(x[2]) for x in pd]
        fa = r.m.get('fairness_ALL')
        fd = r.m.get('fairness_DATA')
        rows.append([r.label, len(inj), min(inj), max(inj),
                     '%.2f' % (sum(inj) / float(len(inj))),
                     sum(inj) - sum(comp),
                     '%s / %s' % (fnum(fa[1], '%.6f') if fa else NA,
                                  fnum(fd[1], '%.6f') if fd else NA)])
    out.extend(table(rows, ['场景', '设备数', 'injected min', 'injected max',
                            'injected avg', '未完成(inj-comp)', 'Jain ALL / DATA']))
    out.append('')

    # ---------------- 8. 门禁明细 ----------------
    out.append('## 8. 门禁结果')
    out.append('')
    for r in runs:
        out.append('- **%s**: `%s`' % (r.label, r.gate or '未运行门禁'))
    out.append('')
    out.append('> 门禁工具 `tmp/nvrr_gate_check.py`，覆盖：功能闭合（PASS + expected/completed/'
               '读写口包数）、包型自检（sq_route=same_index、页集合互不相交、msi=0）、'
               '页全局唯一、S2 Walker 大页叶>0、供给零饥饿、交织形态（dev_repeat_pct=0、'
               'fullsupply_q_max_run=N）、前 32 包序、队列均衡、A0 构造性饱和'
               '（prefill_ok=1 / empty_sel=0 / starve_ns=0 / gen_block_ns=0 / '
               'first_skip>=first_done）、端口占用三分法正向空闲<=0.5%。')
    out.append('')

    # ---------------- 9. 口径注意事项 ----------------
    out.append('## 9. 口径注意事项（阅读上表前必看）')
    out.append('')
    out.append('1. **总包数**：N=8 三点为精确 20000 包；N=16 三点为 19840 包（-0.8%）。'
               '原因是线程侧 `static_assert(TASKS % 2 == 0)` 要求每设备任务数为偶数'
               '（以保证“1 读任务 + 1 写任务”严格交替、读写任务数各半），'
               '而 20000 包 => 2000 任务 => N=16 时需 125 任务/设备（奇数）不可编译，'
               '故取最近偶数 124。因此 N=8 与 N=16 之间的绝对吞吐量对比带 0.8% 口径差，'
               '已同时给出 `ns/pkt`（每包归一化耗时）作为不受包数影响的横向口径。')
    out.append('2. **PT 512B 数据命中率出现负值**：该指标由 skill 按定义做减法'
               '`(pt_cache.hits - ctrl_pt_hits) / (pt_cache.accesses - ctrl_total)`。'
               '模型侧“控制包 PT 命中”计数器与 `pt_cache.hits` 作用域不完全重合'
               '（例：场景1 ctrl 命中 3998 > pt_cache 总命中 3840，差 158 包≈ 0.8%），'
               '相减得小负数。物理含义是**512B 数据包几乎不命中 PT**'
               '（每个任务独占一个全局唯一新 4KB 页），负值属计数器口径偏差，'
               '不是本次改动引入，也不应当作“负命中率”解读。'
               '可直接对比的是 **PT(总) 命中率** 与 **PT 控制命中率** 两个原始计数器。')
    out.append('3. **PC / MSIPT 命中率恒为 0**：本场景 MSIPTP=Off 且不发 MSI，'
               'MSIPT 访问数为 0；PC(进程上下文) 在本模型配置下未命中，两者均无对比意义。')
    out.append('4. **Hold PF 恒为 0**：预取占位条目未走持有延时统计路径，'
               '预取代价请看 `PTW 预取任务平均 DDR 次数` 与 `PTW 执行延时 ALL 平均`。')
    out.append('5. **倍率列**只对可数值化的行给出（末场景/场景1）；'
               '命中率类行的倍率是“百分点之比”而非“绝对差”，阅读时请注意。')
    out.append('')

    text = '\n'.join(out)
    print(text)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(text + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
