#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
[激励重构 20261103] 三场景性能对比: 场景1(8盘实测回放) vs 场景2/3(NVMe任务队列+嵌套轮询仲裁)

输入: 各场景由 .qoder/skills/iommu-perf-stats/scripts/extract_iommu_perf.py 产出的 JSON
      (指标口径三者完全一致) + 各自 sim.log(取激励形态/逐设备窗口计数, 只读头尾避免超大日志)。
输出: 单个 Markdown 对比表(指标 x 场景 + 相对基线的绝对/百分比差值), 另附:
      - 激励形态对照(证明"只有激励不同")
      - 逐设备稳态窗口计数(三场景统一取 [MD_DEVICE] window_count, 规避场景1 无 [MD_RP] 表的既有差异)
      - 口径说明(残余差异逐条评估)
      - 可选"旧激励"参考列(非同批次, 仅供趋势参考)

用法:
  python3 rp/compare_nvrr_3scene.py \
      --scene "场景1 实测8盘回放|perf_scene1.json|scene1/sim.log" \
      --scene "场景2 模型8盘1通道|perf_scene2.json|scene2/sim.log" \
      --scene "场景3 模型8盘2通道|perf_scene3.json|scene3/sim.log" \
      --ref   "旧场景2 逐包RR|.../perf_raw.json" \
      --out   COMPARISON.md
"""
import argparse
import json
import os
import re
import sys

NA = 'N/A'


# ---------------------------------------------------------------------------
# 日志读取: 只取头部(激励配置/包型自检)与尾部(统计段/[MD_DEVICE]/[MDMC_RP])
# ---------------------------------------------------------------------------
def head_tail_text(path, head_bytes=4 << 20, tail_bytes=32 << 20):
    if not path or not os.path.exists(path):
        return ''
    with open(path, 'rb') as f:
        head = f.read(head_bytes)
        f.seek(0, 2)
        size = f.tell()
        if size <= head_bytes + tail_bytes:
            f.seek(0)
            return f.read().decode('utf-8', 'replace')
        f.seek(size - tail_bytes)
        tail = f.read()
    return (head + b'\n' + tail).decode('utf-8', 'replace')


def load_json(path):
    if not path or not os.path.exists(path):
        return {}
    with open(path, 'r', encoding='utf-8') as f:
        d = json.load(f)
    return d.get('metrics', d) if isinstance(d, dict) else {}


# ---------------------------------------------------------------------------
# 指标规格: (章节, 显示名, 取值函数, 用于差值的数值 or None, 显示串)
#   kind: num / pair(peak,max) / cache([acc,hit,miss,ratio]) / hits([hit,acc,pct])
#         hold([n,W1,W2,W3,maxW1,maxW2,maxW3]) / fair([Mreq/s,Jain])
# ---------------------------------------------------------------------------
def fnum(v, fmt='{:.2f}'):
    return NA if v is None else fmt.format(v)


def spec_value(m, key, kind):
    """返回 (显示串, 可比较数值 or None)"""
    v = m.get(key)
    if v is None:
        return NA, None
    if kind == 'num':
        return fnum(v), float(v)
    if kind == 'num1':
        return fnum(v, '{:.1f}'), float(v)
    if kind == 'int':
        return '{:d}'.format(int(v)), float(v)
    if kind == 'pct':
        return '{:.2f}%'.format(v), float(v)
    if kind == 'pair':
        return '%d/%d' % (int(v[0]), int(v[1])), float(v[0])
    if kind == 'cache':
        return '%.2f%% (%d/%d)' % (v[3] * 100, v[1], v[0]), float(v[3] * 100)
    if kind == 'hits':
        return '%.2f%% (%d/%d)' % (v[2], v[0], v[1]), float(v[2])
    if kind == 'hold':
        return ('n=%d W1/W2/W3=%.1f/%.1f/%.1f max=%.1f/%.1f/%.1f'
                % (int(v[0]), v[1], v[2], v[3], v[4], v[5], v[6])), float(v[1] + v[2] + v[3])
    return str(v), None


SECTIONS = [
    ('1. 吞吐 / 并发 / e2e / 端口间隔', [
        ('仿真总时长 (us)', 'sim_time_us', 'num'),
        ('IOMMU IOPS (M trans/s)', 'iommu_iops', 'num'),
        ('IOMMU 完成事务', 'iommu_completed', 'int'),
        ('IOMMU 输入任务总数', 'iommu_input_total', 'int'),
        ('全局并发 peak/max', 'conc_global', 'pair'),
        ('Read 并发 peak/max', 'conc_read', 'pair'),
        ('Write 并发 peak/max', 'conc_write', 'pair'),
        ('IO avg e2e (ns)', 'e2e_avg_ns', 'num1'),
        ('IO max e2e (ns)', 'e2e_max_ns', 'num1'),
        ('输入端口注入平均间隔 (ns)', 'in_interval_ns', 'num'),
        ('输出端口注入平均间隔 (ns)', 'out_interval_ns', 'num'),
        ('稳态吞吐 ALL (Mreq/s)', 'fairness_ALL', 'fair0'),
        ('稳态吞吐 DATA (Mreq/s)', 'fairness_DATA', 'fair0'),
        ('Jain 公平性 ALL', 'fairness_ALL', 'fair1'),
        ('Jain 公平性 DATA', 'fairness_DATA', 'fair1'),
    ]),
    ('2. Cache 命中率', [
        ('DC Cache', 'cache_dc_cache', 'cache'),
        ('PC Cache', 'cache_pc_cache', 'cache'),
        ('PT Cache(总)', 'cache_pt_cache', 'cache'),
        ('MSIPT Cache', 'cache_msipt_cache', 'cache'),
        ('Walker C2(内部表)', 'cache_walker_ptw_c2', 'cache'),
        ('Walker C3(内部表)', 'cache_walker_ptw_c3', 'cache'),
        ('PT 512B 数据命中率', 'pt_data_hit_pct', 'pct'),
        ('PT SQ/CQ/MSI 控制命中率', 'pt_ctrl_hit', 'pct'),
        ('控制包总数 (SQ+CQ+MSI)', 'ctrl_total', 'int'),
        ('数据任务总数 (PT访问-控制包)', 'total_data_tasks', 'int'),
        ('Walker(VS) C3 命中率', 'vs_c3', 'hits'),
        ('Walker(VS) C2 命中率', 'vs_c2', 'hits'),
        ('Walker(VS) 整体命中率', 'vs_hit', 'pct'),
        ('S2 Walker C3 命中率', 's2_c3', 'hits'),
        ('S2 Walker 整体命中率', 's2_hit', 'pct'),
    ]),
    ('3. 去重 Cache / Buffer', [
        ('去重 Cache 命中率', 'dedup_hit', 'pct'),
        ('去重总查询 (lookup)', 'dedup_lookups', 'int'),
        ('去重 hash 冲突次数', 'dedup_conflict', 'int'),
        ('  冲突占总查询比例', 'dedup_conflict_ratio_pct', 'pct'),
        ('去重 bypass 任务数', 'dedup_bypass', 'int'),
        ('  bypass 占数据任务比例', 'dedup_bypass_ratio_pct', 'pct'),
        ('Buffer Peak / Size', 'buf_peak', 'int'),
        ('Buffer 平均占用 (entries)', 'buf_avg_occ', 'num'),
        ('Buffer 平均占用率', 'buf_avg_pct', 'pct'),
        ('Buffer 满 bypass(正常预取)', 'dedup_buffull_bypass', 'int'),
        ('Buffer 持有延时 avg (ns)', 'hold_avg_ns', 'num1'),
        ('Buffer 持有延时 max (ns)', 'hold_max_ns', 'num1'),
        ('Buffer 持有延时 min (ns)', 'hold_min_ns', 'num1'),
        ('Hold MAIN (n/W1/W2/W3)', 'hold_main', 'hold'),
        ('Hold PF占位 (n/W1/W2/W3)', 'hold_pf', 'hold'),
        ('Hold SUSP (n/W1/W2/W3)', 'hold_susp', 'hold'),
    ]),
    ('4. PTW', [
        ('PTW 总任务(主+预取)', 'ptw_completed', 'int'),
        ('  主任务', 'ptw_main', 'int'),
        ('    其中正常任务', 'ptw_main_normal', 'int'),
        ('    其中 bypass 任务', 'ptw_main_bypass', 'int'),
        ('  预取任务', 'ptw_prefetch', 'int'),
        ('PTW 占 IOMMU 全部输入比', 'ptw_ratio_all', 'pct'),
        ('PTW 占数据任务比', 'ptw_to_data_ratio_pct', 'pct'),
        ('PTW 并发 peak/max', 'conc_ptw', 'pair'),
        ('PTW 并发平均占用(时间加权)', 'ptw_conc_avg', 'num'),
        ('PTW 并发利用率', 'ptw_conc_util', 'pct'),
        ('PTW IOPS (M tasks/s)', 'ptw_iops', 'num'),
        ('PTW 主任务平均 DDR 次数', 'ptw_main_ddr', 'num'),
        ('PTW 预取任务平均 DDR 次数', 'ptw_pf_ddr', 'num'),
        ('PTW 执行延时 ALL 平均 (ns)', 'ptw_exec_all_avg_ns', 'num'),
        ('PTW 执行延时 MAIN 平均 (ns)', 'ptw_exec_main_avg_ns', 'num'),
        ('PTW 任务组平均执行 (ns)', 'ptw_group_exec_avg_ns', 'num1'),
        ('PTW 注入任务平均间隔 (ns)', 'ptw_inject_interval_ns', 'num'),
        ('PTW 输出任务平均间隔 (ns)', 'ptw_output_interval_ns', 'num'),
    ]),
]


def get_val(m, key, kind):
    """统一取值: 处理 fair0/fair1 这类派生 kind"""
    if kind == 'fair0':
        v = m.get(key)
        return (fnum(v[0]), float(v[0])) if v else (NA, None)
    if kind == 'fair1':
        v = m.get(key)
        return ('{:.6f}'.format(v[1]), float(v[1])) if v else (NA, None)
    if kind == 'pct':
        v = m.get(key)
        return (NA, None) if v is None else ('{:.2f}%'.format(v), float(v))
    return spec_value(m, key, kind)


def delta_cell(base, cur):
    if base is None or cur is None:
        return NA, NA
    d = cur - base
    p = (100.0 * d / base) if base else None
    return ('%+.2f' % d), (('%+.2f%%' % p) if p is not None else NA)


# ---------------------------------------------------------------------------
# 激励形态 / 逐设备表: 从日志抓取
# ---------------------------------------------------------------------------
RE_MD_DEVICE = re.compile(
    r'\[MD_DEVICE\] did=(\d+) kind=(\w+) window_count=(\d+) Mreq/s=([\d.]+) relative_bias=([-\d.]+)')
RE_NVRR_IL = re.compile(r'\[NVRR_INTERLEAVE\] (.*)')
RE_KV = re.compile(r'(\w+)=([^\s]+)')
# [归一化 20261105] 三场景规模不同(20000 vs 20000 vs 40000), 追加绝对量列以便直接比较
RE_4KB_TOT = re.compile(r'^\s*TOTAL\s+([\d.]+)\s+(\d+)')
RE_4KB_DEV = re.compile(r'^\s{2,}(\d+)\s+([\d.]+)\s+(\d+)')
RE_AREA = re.compile(r'^\s+(Read|Write|Global)\s*并发\(实际在处理\)\s+([\d.]+)\s+(\d+)')
RE_TRACKED = re.compile(r'Tracked devices:\s+(\d+)')


def grab_line(text, tag):
    for ln in text.splitlines():
        if tag in ln:
            return ln.strip()
    return ''


def kv(line):
    return {k: v for k, v in RE_KV.findall(line)}


def stimulus_table(scenes):
    rows = []
    for label, _m, text in scenes:
        cfg = grab_line(text, '[MDMC_CONFIG]') or grab_line(text, '[REAL8_CONFIG]')
        tot = grab_line(text, '[NVRR_PATTERN_OK]') or grab_line(text, '[REAL8_TOTAL]')
        stim = grab_line(text, '[NVRR_STIMULUS]')
        sup = grab_line(text, '[MDMC_SUPPLY]')
        res = grab_line(text, '[NVRR_RESULT]') or grab_line(text, '[REAL8_RESULT]')
        win = grab_line(text, '[MDMC_STAT_WINDOW]') or grab_line(text, '[REAL8_STAT_WINDOW]')
        rows.append((label, cfg, stim, tot, win, sup, res))
    out = ['', '## 激励形态对照（证明"只有激励不同"）', '']
    for label, cfg, stim, tot, win, sup, res in rows:
        out.append('### %s' % label)
        out.append('')
        out.append('```text')
        for x in (cfg, stim, tot, win, sup, res):
            if x:
                out.append(x)
        out.append('```')
        out.append('')
    # 交织量化(仅新激励场景有): 只取测量 epoch=3 的行, 避免哨兵/预热行干扰
    il = []
    for label, _m, text in scenes:
        for ln in text.splitlines():
            g = RE_NVRR_IL.match(ln.strip())
            if g:
                d = kv(g.group(1))
                if d.get('epoch') == '3':
                    il.append((label, d))
    if il:
        out.append('### 端口注入交织量化（新激励场景，仅测量 epoch=3）')
        out.append('')
        out.append('| 场景 | 方向 | 注入包数 | 同设备最大连发 | 同设备连发占比 | 同(设备,队列)最大连发 | 队列游标最大连发(全程) | 满供给阶段连发上界 dev/dq/q | 首次空队列跳过位置(包) | 空队列跳过次数 | 同页相邻数据包平均间隔(包) | 最大间隔(包) |')
        out.append('|---|---|---|---|---|---|---|---|---|---|---|---|')
        for label, d in il:
            pre = '%s/%s/%s' % (d.get('fullsupply_dev_max_run', NA),
                                d.get('fullsupply_dq_max_run', NA),
                                d.get('fullsupply_q_max_run', NA))
            out.append('| %s | %s | %s | %s | %s%% | %s | %s | %s | %s | %s | %s | %s |' % (
                label, d.get('dir', NA), d.get('sent', NA), d.get('dev_max_run', NA),
                d.get('dev_repeat_pct', NA), d.get('dq_max_run', NA), d.get('q_max_run', NA),
                pre, d.get('first_skip_at_pkt', NA), d.get('skip_cnt', NA),
                d.get('same_page_gap_avg_pkts', NA), d.get('same_page_gap_max_pkts', NA)))
        out.append('')
        out.append('> 说明：`队列游标最大连发(全程)` 包含注入尾部效应——各队列包数不完全相等时，')
        out.append('> 先耗尽的队列会迫使剩余包全来自另一队列（看 `首次空队列跳过位置` 即可定位尾部起点）；')
        out.append('> 衡量“多队列是否完全打散”应看 `满供给阶段连发上界`。C=1 时每方向只有 1 个队列，')
        out.append('> 队列游标连发等于总包数，属预期无意义值。')
        out.append('')
        for label, _m, text in scenes:
            for ln in text.splitlines():
                if '[NVRR_ORDER]' not in ln or 'first32=' not in ln:
                    continue
                head, _, seq = ln.partition('first32=')
                out.append('- `%s` 方向%s 前32包顺序: `%s`'
                           % (label, kv(head).get('dir', '?'), seq.strip()))
        out.append('')
    return out


def normalized_table(scenes):
    """[归一化 20261105] 三场景规模不同(场景3 = 2x 包量), 给出与规模无关/可比的绝对量。"""
    rows = []
    for label, m, text in scenes:
        cfg = kv(grab_line(text, '[MDMC_CONFIG]'))
        tot8 = kv(grab_line(text, '[REAL8_TOTAL]'))
        total = cfg.get('total_meas_pkts') or tot8.get('all')
        total = int(total) if total else None
        n = int(cfg['N']) if 'N' in cfg else None
        conc_avg = conc_peak = None
        ndev = 0
        # 只在 4KB 章节切片内解析: 日志里其它表格也有 TOTAL 行(如 real8 的逐包型表);
        # 且切片必须在 TOTAL 行处截断, 否则后续章节的数字行会被误计为设备行。
        lines = text.splitlines()
        sec = []
        for i, ln in enumerate(lines):
            if '4KB Page Concurrency Statistics' in ln:
                sec = lines[i:i + 40]
                break
        tracked = None
        for ln in sec:
            g = RE_TRACKED.search(ln)
            if g:
                tracked = int(g.group(1))
        for ln in sec:
            g = RE_4KB_TOT.match(ln)
            if g:
                conc_avg, conc_peak = float(g.group(1)), int(g.group(2))
                break                     # TOTAL 之后不再属于本表
            if RE_4KB_DEV.match(ln):
                ndev += 1
        # 设备数优先级: [MDMC_CONFIG] N= > 4KB 表的 Tracked devices > 数到的设备行数
        if n is None:
            n = tracked if tracked else (ndev or None)
        area = {}
        for ln in text.splitlines():
            g = RE_AREA.match(ln)
            if g and g.group(1) not in area:
                area[g.group(1)] = float(g.group(2))
        sim_us = m.get('sim_time_us')
        data_m = (m.get('fairness_DATA') or [None])[0]
        all_m = (m.get('fairness_ALL') or [None])[0]
        per_dev_pages = (conc_avg / n) if (conc_avg and n) else None
        per_dev_data = (data_m / n) if (data_m and n) else None
        ns_per_pkt = (sim_us * 1000.0 / total) if (sim_us and total) else None
        overlap = (area['Global'] / conc_avg) if (area.get('Global') and conc_avg) else None
        rows.append((label, total, per_dev_pages, per_dev_data, ns_per_pkt, overlap,
                     conc_avg, conc_peak, area.get('Global'), area.get('Read'), area.get('Write'),
                     data_m, all_m, n))
    if not any(r[1] for r in rows):
        return []
    out = ['', '## 0. 归一化口径（各场景规模不同，必须看这几列）', '']
    out.append('| 指标 | ' + ' | '.join(r[0] for r in rows) + ' |')
    out.append('|---|' + '---|' * len(rows))

    def line(name, idx, fmt='{:.2f}'):
        cells = []
        for r in rows:
            v = r[idx]
            cells.append(NA if v is None else (fmt.format(v) if not isinstance(v, int) else str(v)))
        out.append('| %s | %s |' % (name, ' | '.join(cells)))

    line('设备数 N', 13, '{:.0f}')
    line('测量总包数（读+写）', 1)
    line('4KB 页并发 TOTAL avg', 6)
    line('4KB 页并发 TOTAL peak', 7, '{:.0f}')
    line('**每设备 4KB 页并发**', 2)
    line('**DATA 稳态每设备 Mreq/s**', 3)
    line('**每包稳态耗时 ns/pkt**', 4)
    line('**overlap = Global在途 / 页并发**', 5)
    line('Global 在途 avg（面积法）', 8)
    line('Read 在途 avg（面积法）', 9)
    line('Write 在途 avg（面积法）', 10)
    line('DATA 稳态 Mreq/s（全系统）', 11)
    line('ALL 稳态 Mreq/s（全系统）', 12)
    out.append('')
    out.append('> 说明：场景2/3 的包量不同（场景3 per_queue ⇒ 40000 包），')
    out.append('> 因此**绝对总时长/总IOPS 不可直接对比**；判断“激励变化带来的效率差”要看')
    out.append('> `每设备 4KB 页并发`、`DATA 稳态每设备 Mreq/s` 与 `ns/pkt` 三列，')
    out.append('> 它们已剔除规模差异。`same_page_gap` 见下文交织表。')
    out.append('')
    return out


def perdev_table(scenes):
    # md_trace.report() 只对 ALL / DATA 两种 kind 输出 [MD_DEVICE] 逐设备行
    # (READ/WRITE 仅以 [MD_EXEC_LAT] 聚合形式给出), 故此处不列 READ/WRITE 避免全行 N/A。
    kinds = ('ALL', 'DATA')
    data = {}
    dids = set()
    for label, _m, text in scenes:
        tab = {}
        for g in RE_MD_DEVICE.finditer(text):
            did, kind, cnt, mreq, bias = int(g.group(1)), g.group(2), int(g.group(3)), g.group(4), g.group(5)
            if kind in kinds:
                tab[(did, kind)] = (cnt, mreq, bias)
                dids.add(did)
        data[label] = tab
    if not dids:
        return ['## 逐设备稳态窗口计数', '', '（日志中未找到 [MD_DEVICE] 行）', '']
    out = ['', '## 逐设备稳态窗口计数（各场景统一取 [MD_DEVICE] window_count）', '']
    for kind in kinds:
        out.append('### kind=%s' % kind)
        out.append('')
        header = '| did | ' + ' | '.join(s[0] for s in scenes) + ' |'
        out.append(header)
        out.append('|---|' + '---|' * len(scenes))
        for did in sorted(dids):
            cells = []
            for label, _m, _t in scenes:
                v = data[label].get((did, kind))
                cells.append('%d (%s Mreq/s)' % (v[0], v[1]) if v else NA)
            out.append('| %d | %s |' % (did, ' | '.join(cells)))
        out.append('')
    return out


CALIBER_NOTE = """## 口径说明（各场景一致性）

1. **IOMMU 逻辑一致**：各场景在同一工作树、同一批次内 `make -B` 重建；IOMMU 侧宏集合等价
   （PTW=64 / Dedup Buffer=320 / Global-Read-Write=640-320-320 / ADM=1 / 预取深度 D=3 /
   SPLIT_RW_PORTS=1 / PERDEV_WRITE_ORDER=0 / PORT_WIDTH_BIT=1024 / SLINK=50ns / DDR=100ns /
   Walker Cache 与 S2 Walker Cache 均开启），`iommu/cache_config/default_config.json` 为同一份拷贝。
   构建日志抽取的 `-DTEST_CFG_*` 清单见归档目录 `macro_diff_*.txt`。
2. **指标分析口径一致**：三者均由同一个 `extract_iommu_perf.py` 从 sim.log 提取；稳态窗口均为
   完成数的 10%~90%；测量前均执行 `reset_4kb_concurrency_window` + `reset_dedup_valid_line_stats`。
3. **唯一残余差异**：场景1 定义 `TEST_CFG_REAL8_TRACE_REPLAY=1`（去重/PTW 占用采样钩子、
   operations 流式落盘、逐包 printf 抑制、控制包不按固定 IOVA 细分 SQ/CQ/MSI、4KB 页并发纳入
   所有 DATA 包长）。逐条评估：场景2/3 的数据包**全部为 512B**，故 4KB 并发口径与场景1 等价；
   `ctrl_total` 三者都统计；其余为日志/辅助 CSV 差异，不进入本表任何指标。
4. **激励差异（本次实验变量）**：场景1 为实测 TLP 逐包回放（读写各 1 条常驻发送进程，顺序=实测行序）；
   场景2/3 为合成 NVMe 任务激励（**每设备 C 个任务生成进程，各自绑定一组读/写口队列**，
   2 个方向发送进程做"外层设备 RR + 内层设备内队列 RR"的逐包仲裁，SQ 与数据/CQ **同索引入队**）。
   负载口径为 per_queue：每个队列对自带 250 任务 / 250 个全局唯一页，故场景2（C=1）共 20000 包，
   场景3（C=2）共 40000 包 —— **场景3 与场景1/2 规模不同，属预期**；同规模对照见 `SCALING.md` 的 ft 列。
   若某列为 `矩阵 C=2 ft`：与场景2 **同为 20000 包**（fixed_total，每设备恒 250 任务），
   它把"负载翻倍"从"交织变细"中分离出去，专门度量 C=1→2 的纯交织效应。
5. **冷启动差异**：场景1 无哨兵/预热（冷启动直接进入回放）；场景2/3 保留哨兵 epoch(16 包)与
   预热 epoch(128 包)，测量 epoch 前 IOMMU 内部已 quiescent 并重置统计窗口，与旧场景2/3 一致。
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', action='append', required=True,
                    help='LABEL|metrics.json|sim.log（log 可省），可重复；第 1 个为对比基线')
    ap.add_argument('--ref', action='append', default=[],
                    help='LABEL|metrics.json（旧激励参考列，非同批次，仅趋势参考）')
    ap.add_argument('--out', default=None)
    args = ap.parse_args()

    def parse_spec(s, with_log):
        # 用 '|' 分隔, 避开 Windows 盘符中的 ':'
        parts = [p.strip() for p in s.split('|')]
        if with_log:
            if len(parts) == 2:
                parts.append('')
            if len(parts) != 3:
                sys.exit('--scene 需为 LABEL|json|log 格式: %s' % s)
            return parts[0], parts[1], parts[2]
        if len(parts) != 2:
            sys.exit('--ref 需为 LABEL|json 格式: %s' % s)
        return parts[0], parts[1], ''

    scenes = []
    for s in args.scene:
        label, jpath, lpath = parse_spec(s, True)
        scenes.append((label, load_json(jpath), head_tail_text(lpath)))
    refs = []
    for s in args.ref:
        label, jpath, _ = parse_spec(s, False)
        refs.append((label, load_json(jpath)))

    if len(scenes) < 2:
        sys.exit('至少需要 2 个场景')
    base_label, base_m, _ = scenes[0]

    out = []
    out.append('# 多场景性能对比（激励再修改 20261105：每通道独立生成进程 + per_queue 负载）')
    out.append('')
    out.append('- 基线列: **%s**；差值列 = 其它场景 − 基线（绝对值 / 百分比）' % base_label)
    out.append('- 场景列: ' + ' ; '.join(s[0] for s in scenes))
    if refs:
        out.append('- 参考列(旧激励, 非同批次): ' + ' ; '.join(r[0] for r in refs))
    out.append('- 指标来源: 同一份 `extract_iommu_perf.py` 输出 JSON（口径完全一致）')
    out.append('')

    na_cells = []
    for sec, items in SECTIONS:
        out.append('## %s' % sec)
        out.append('')
        head = '| 指标 | ' + ' | '.join(s[0] for s in scenes)
        for i in range(1, len(scenes)):
            head += ' | Δ(%d−1) | Δ%%(%d−1)' % (i + 1, i + 1)
        for r in refs:
            head += ' | [参考]%s' % r[0]
        out.append(head + ' |')
        ncols = len(head.split('|')) - 1
        out.append('|---|' + '---|' * (ncols - 1))
        for label, key, kind in items:
            cells, nums = [], []
            for _lbl, m, _t in scenes:
                disp, num = get_val(m, key, kind)
                cells.append(disp)
                nums.append(num)
                if disp == NA:
                    na_cells.append((sec, label, _lbl))
            row = '| %s | %s' % (label, ' | '.join(cells))
            for i in range(1, len(scenes)):
                d, p = delta_cell(nums[0], nums[i])
                row += ' | %s | %s' % (d, p)
            for _rl, rm in refs:
                rd, _ = get_val(rm, key, kind)
                row += ' | %s' % rd
                if rd == NA:
                    na_cells.append((sec, label, _rl))
            out.append(row + ' |')
        out.append('')

    out.extend(normalized_table(scenes))
    out.extend(stimulus_table(scenes))
    out.extend(perdev_table(scenes))
    out.append(CALIBER_NOTE)

    out.append('## N/A 单元格清单')
    out.append('')
    if na_cells:
        out.append('| 章节 | 指标 | 场景 |')
        out.append('|---|---|---|')
        for sec, label, lbl in na_cells:
            out.append('| %s | %s | %s |' % (sec, label.strip(), lbl))
        out.append('')
        out.append('> 说明: 上表 N/A 均为该场景日志中不存在对应统计段/字段所致, 需逐项确认原因后再解读。')
    else:
        out.append('无（各场景全部指标均已提取到）。')
    out.append('')

    report = '\n'.join(out)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(report + '\n')
        print('[COMPARE] written: %s (%d lines, N/A cells=%d)'
              % (args.out, len(out), len(na_cells)))
    else:
        print(report)
    return 0


if __name__ == '__main__':
    sys.exit(main())
