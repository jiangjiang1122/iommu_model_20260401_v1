#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
[激励重构 20261103] 三场景"全量性能指标对照表"提取器(用户指定清单口径)

在 iommu-perf-stats skill JSON 之外, 额外从 sim.log 与 md_trace summary CSV 解析:
  - IOMMU 端口带宽: 全程平均 / 稳态窗口(10%~90%) 读口、写口、入口合计、出口
  - 并发峰值/时间加权平均: 全程面积法(三场景同口径) + 注入窗口(仅场景2/3, 场景1 无该窗口)
  - 4KB 页平均/峰值并发(TOTAL 行)
  - 去重 Cache 实际占用 cacheline 数(avg/max/main-req)
  - IO e2e 按包型拆分: 数据包(type0) vs 控制包(type1/2/3) vs 全部(type4), 口径=ACCEPT->RESPONSE
用法:
  python3 rp/extract_nvrr_full_metrics.py \
    --scene "场景1 实测8盘回放|<dir>" --scene "场景2 模型8盘1通道|<dir>" \
    --scene "场景3 模型8盘2通道|<dir>" --out EXTENDED_METRICS.md
  每个 <dir> 需含 sim.log、perf_sceneN.json 与 *_summary.csv(或 real8_trace_summary.csv)
"""
import argparse
import csv
import glob
import json
import os
import re
import sys

NA = 'N/A'


def tail_text(path, nbytes=6 << 20):
    with open(path, 'rb') as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - nbytes))
        return f.read().decode('utf-8', 'replace')


def f1(v, nd=2):
    return NA if v is None else ('%.*f' % (nd, v))


def pct_s(v, nd=2):
    return NA if v is None else ('%.*f%%' % (nd, v))


class Scene:
    def __init__(self, label, d):
        self.label = label
        self.dir = d
        self.log = tail_text(os.path.join(d, 'sim.log'))
        jpath = glob.glob(os.path.join(d, 'perf_scene*.json'))
        self.mj = json.load(open(jpath[0], encoding='utf-8'))['metrics'] if jpath else {}
        cpath = glob.glob(os.path.join(d, '*_summary.csv'))
        self.summary = self._agg_summary(cpath[0]) if cpath else {}

    # ---------- summary CSV: 按包型聚合 e2e ----------
    @staticmethod
    def _agg_summary(path):
        agg = {}
        with open(path, newline='', encoding='utf-8') as f:
            for row in csv.DictReader(f):
                t = int(row['type'])
                n = int(row['completed'])
                a = agg.setdefault(t, {'n': 0, 'port_sum': 0.0, 'rp_sum': 0.0, 'max': 0.0})
                a['n'] += n
                a['port_sum'] += n * float(row['avg_port_ns'])
                a['rp_sum'] += n * float(row['avg_rp_ns'])
                a['max'] = max(a['max'], float(row['max_ns']))
        out = {}
        for t, a in agg.items():
            out[t] = {'n': a['n'],
                      'port': (a['port_sum'] / a['n']) if a['n'] else None,
                      'rp': (a['rp_sum'] / a['n']) if a['n'] else None,
                      'max': a['max'] if a['n'] else None}
        ctrl_n = sum(out[t]['n'] for t in (1, 2, 3) if t in out)
        ctrl_port = sum(out[t]['port'] * out[t]['n'] for t in (1, 2, 3) if t in out and out[t]['port'])
        ctrl_max_vals = [out[t]['max'] for t in (1, 2, 3) if t in out and out[t]['max'] is not None]
        ctrl_max = max(ctrl_max_vals) if ctrl_max_vals else None
        out['ctrl'] = {'n': ctrl_n, 'port': (ctrl_port / ctrl_n) if ctrl_n else None,
                       'max': ctrl_max}
        return out

    # ---------- 日志段解析 ----------
    def g(self, pat, idx=1, nd=None, text=None):
        m = re.search(pat, text if text is not None else self.log)
        if not m:
            return None
        return float(m.group(idx))   # 本提取器的所有捕获组均为数值

    def port_bw(self):
        r = {}
        r['full_r'] = self.g(r'slave_0r \(读口入口\):\s+\d+ B\s+avg ([\d.]+) GB/s')
        r['full_w'] = self.g(r'slave_0w \(写口入口\):\s+\d+ B\s+avg ([\d.]+) GB/s')
        r['full_all'] = self.g(r'slave_0\s+\(入口合计\):\s+\d+ B\s+avg ([\d.]+) GB/s')
        r['full_util'] = self.g(r'slave_0\s+\(入口合计\):.*util ([\d.]+)%')
        blk = re.search(r'\[稳态窗口 ([\d.]+) ns: ([\d.~]+)\](.*?)(\[读口注入期|=====)', self.log, re.S)
        if blk:
            r['steady_win_ns'] = float(blk.group(1))
            r['steady_r'] = self.g(r'读口 slave_0r:\s+\d+ B\s+([\d.]+) GB/s', text=blk.group(3))
            r['steady_r_util'] = self.g(r'读口 slave_0r:.*util ([\d.]+)% of 64', text=blk.group(3))
            r['steady_w'] = self.g(r'写口 slave_0w:\s+\d+ B\s+([\d.]+) GB/s', text=blk.group(3))
            r['steady_w_util'] = self.g(r'写口 slave_0w:.*util ([\d.]+)% of 64', text=blk.group(3))
            r['steady_all'] = self.g(r'入口合计:\s+([\d.]+) GB/s', text=blk.group(3))
            r['steady_all_util'] = self.g(r'入口合计:\s+[\d.]+ GB/s \(聚合peak 128 GB/s, util ([\d.]+)%\)', text=blk.group(3))
            r['steady_out'] = self.g(r'出口 master_0:\s+([\d.]+) GB/s', text=blk.group(3))
        r['inj_r'] = self.g(r'\[读口注入期 [\d.]+ ns\] \d+ B -> ([\d.]+) GB/s')
        r['inj_w'] = self.g(r'\[写口注入期 [\d.]+ ns\] \d+ B -> ([\d.]+) GB/s')
        r['inj_all'] = self.g(r'\[入口合计注入期 [\d.]+ ns\] -> ([\d.]+) GB/s')
        return r

    def conc_inject(self):
        r = {}
        for name in ('Read', 'Write', 'Global'):
            m = re.search(r'%s\s+并发\s+([\d.]+)\s+(\d+)\s+([\d.]+)%%\s+(\d+)\s+([\d.]+)%%' % name, self.log)
            if m:
                r[name] = {'avg': float(m.group(1)), 'cap': int(m.group(2)),
                           'peak': int(m.group(4)), 'full_pct': float(m.group(5))}
        return r

    def conc_area(self):
        r = {}
        m = re.search(r'采样窗口:\s+([\d.]+) ns', self.log[self.log.find('IOMMU 内部实际并发'):]) if 'IOMMU 内部实际并发' in self.log else None
        r['win_ns'] = float(m.group(1)) if m else None
        for name in ('Read', 'Write', 'Global'):
            m = re.search(r'%s\s*并发\(实际在处理\)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+([\d.]+)%%' % name, self.log)
            if m:
                r[name] = {'avg': float(m.group(1)), 'peak': int(m.group(2)),
                           'cap': int(m.group(3)), 'util': float(m.group(4))}
        return r

    def kb4(self):
        m = re.search(r'TOTAL\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)', self.log)
        if not m:
            return {}
        return {'avg': float(m.group(1)), 'peak': int(m.group(2)),
                'in': int(m.group(3)), 'out': int(m.group(4)), 'pages': int(m.group(5))}

    def dedup_lines(self):
        r = {}
        r['avg'] = self.g(r'Avg valid lines:\s+([\d.]+)')
        r['pct'] = self.g(r'Avg valid lines:\s+[\d.]+\s+\(([\d.]+)% of 256\)')
        m = re.search(r'Max / Min valid:\s+(\d+) / (\d+)', self.log)
        if m:
            r['max'], r['min'] = int(m.group(1)), int(m.group(2))
        r['main_req'] = self.g(r'Avg main-req lines:\s+([\d.]+)')
        return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', action='append', required=True, help='LABEL|dir')
    ap.add_argument('--batch', default='', help='批次号(写入标题, 可省)')
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    scenes = []
    for s in args.scene:
        label, d = [x.strip() for x in s.split('|')]
        scenes.append(Scene(label, d))

    bw = [s.port_bw() for s in scenes]
    ci = [s.conc_inject() for s in scenes]
    ca = [s.conc_area() for s in scenes]
    kb = [s.kb4() for s in scenes]
    dl = [s.dedup_lines() for s in scenes]

    def row(label, vals, note=''):
        cells = ' | '.join(vals)
        return '| %s | %s |%s' % (label, cells, (' ' + note + ' |') if note else '')

    out = []
    out.append('# 全量性能指标对照表（用户清单口径%s）'
               % (('，批次 ' + args.batch) if args.batch else ''))
    out.append('')
    out.append('- 场景列: ' + ' ; '.join(s.label for s in scenes))
    out.append('- 稳态窗口 = 完成数 10%~90%（各场景同口径）；"全程"口径另行标注。')
    out.append('- e2e 拆分口径 = md_trace summary CSV 的 ACCEPT→RESPONSE（avg_port_ns）；'
               '总行与 skill 的 IO avg e2e（入口→出口）并列给出以便交叉核对。')
    out.append('')
    out.append('| 指标 | ' + ' | '.join(s.label for s in scenes) + ' |')
    out.append('|---|' + '---|' * len(scenes))

    # 1 吞吐 / 带宽
    out.append(row('IOMMU IOPS 稳态 (M trans/s)',
                   [f1(s.mj.get('fairness_ALL', [None])[0]) for s in scenes]))
    out.append(row('IOMMU IOPS 全程 (M trans/s)', [f1(s.mj.get('iommu_iops')) for s in scenes]))
    out.append(row('IOMMU 端口带宽·稳态入口合计 (GB/s, 聚合peak128)',
                   [f1(b['steady_all']) + (' (%s%%)' % f1(b['steady_all_util'], 2) if b['steady_all_util'] else '') for b in bw]))
    out.append(row('IOMMU 端口带宽·全程入口合计 (GB/s)',
                   [f1(b['full_all']) + (' (util %s%%)' % f1(b['full_util']) if b['full_util'] else '') for b in bw]))
    out.append(row('读通道平均带宽·稳态 (GB/s, 上限64)',
                   [f1(b['steady_r']) + (' (%s%%)' % f1(b['steady_r_util']) if b['steady_r_util'] else '') for b in bw]))
    out.append(row('写通道平均带宽·稳态 (GB/s, 上限64)',
                   [f1(b['steady_w']) + (' (%s%%)' % f1(b['steady_w_util']) if b['steady_w_util'] else '') for b in bw]))
    out.append(row('读/写通道平均带宽·注入期 (GB/s)',
                   [('%s / %s' % (f1(b['inj_r']), f1(b['inj_w']))) for b in bw]))
    out.append(row('出口 master_0 带宽·稳态 (GB/s)', [f1(b['steady_out']) for b in bw]))
    # 2 并发
    for name, cap, skey in (('Global', 640, 'conc_global'), ('Read', 320, 'conc_read'), ('Write', 320, 'conc_write')):
        out.append(row('IOMMU %s 并发 peak/上限' % name,
                       [('%d/%d' % tuple(s.mj[skey])) if s.mj.get(skey) else NA for s in scenes]))
        out.append(row('IOMMU %s 并发 时间加权平均(全程采样窗)' % name,
                       [f1(ca_i.get(name, {}).get('avg')) for ca_i in ca]))
        out.append(row('IOMMU %s 并发 平均/peak(注入窗口, 仅2/3有)' % name,
                       [('%s / %d' % (f1(ci_i.get(name, {}).get('avg')), ci_i.get(name, {}).get('peak', 0)))
                        if name in ci_i else NA for ci_i in ci]))
    out.append(row('注入窗口顶满时长占比 R/W/G',
                   [('%s%% / %s%% / %s%%' % (f1(ci_i.get('Read', {}).get('full_pct')),
                                             f1(ci_i.get('Write', {}).get('full_pct')),
                                             f1(ci_i.get('Global', {}).get('full_pct'))))
                    if ci_i else NA for ci_i in ci]))
    # 3 e2e
    out.append(row('IO avg e2e 总 (ns, 入口→出口)', [f1(s.mj.get('e2e_avg_ns'), 1) for s in scenes]))
    out.append(row('IO avg e2e 数据包 (ns, ACCEPT→RESPONSE)',
                   [f1(s.summary.get(0, {}).get('port'), 1) for s in scenes]))
    out.append(row('IO avg e2e 控制包 (ns, ACCEPT→RESPONSE)',
                   [f1(s.summary.get('ctrl', {}).get('port'), 1) for s in scenes]))
    out.append(row('IO max e2e 总 (ns)', [f1(s.mj.get('e2e_max_ns'), 1) for s in scenes]))
    out.append(row('IO max e2e 数据包 / 控制包 (ns)',
                   [('%s / %s' % (f1(s.summary.get(0, {}).get('max'), 1),
                                  f1(s.summary.get('ctrl', {}).get('max'), 1))) for s in scenes]))
    out.append(row('4KB页 平均并发数目 (TOTAL, 页)', [f1(k.get('avg'), 3) for k in kb]))
    out.append(row('4KB页 峰值并发数目 (TOTAL, 页)', [str(k.get('peak', NA)) for k in kb]))
    out.append(row('IOMMU 输入端口注入平均间隔 (ns)', [f1(s.mj.get('in_interval_ns')) for s in scenes]))
    out.append(row('IOMMU 输出端口注入平均间隔 (ns)', [f1(s.mj.get('out_interval_ns')) for s in scenes]))
    # 4 cache
    out.append(row('DC Cache 命中率', [pct_s((s.mj.get('cache_dc_cache') or [0, 0, 0, 0])[3] * 100) for s in scenes]))
    out.append(row('PC Cache 命中率', [pct_s((s.mj.get('cache_pc_cache') or [0, 0, 0, 0])[3] * 100) for s in scenes]))
    out.append(row('PT Cache 命中率(总)', [pct_s((s.mj.get('cache_pt_cache') or [0, 0, 0, 0])[3] * 100) for s in scenes]))
    out.append(row('PT Cache 512B 数据命中率', [pct_s(s.mj.get('pt_data_hit_pct')) for s in scenes]))
    out.append(row('PT Cache SQ/CQ/MSI 控制命中率', [pct_s(s.mj.get('pt_ctrl_hit')) for s in scenes]))
    out.append(row('Walker Cache C3 命中率', [pct_s((s.mj.get('vs_c3') or [0, 0, 0])[2]) for s in scenes]))
    out.append(row('Walker Cache C2 命中率', [pct_s((s.mj.get('vs_c2') or [0, 0, 0])[2]) for s in scenes]))
    out.append(row('S2 Walker Cache C3 命中率', [pct_s((s.mj.get('s2_c3') or [0, 0, 0])[2]) for s in scenes]))
    out.append(row('MSIPT Cache 命中率', [pct_s((s.mj.get('cache_msipt_cache') or [0, 0, 0, 0])[3] * 100) for s in scenes]))
    # 5 dedup
    out.append(row('去重 Cache 命中率', [pct_s(s.mj.get('dedup_hit')) for s in scenes]))
    out.append(row('去重 hash 冲突次数 (占总查询)',
                   [('%d (%s)' % (s.mj.get('dedup_conflict', 0), pct_s(s.mj.get('dedup_conflict_ratio_pct')))) for s in scenes]))
    out.append(row('去重 bypass 任务数 (占总数据任务)',
                   [('%d (%s)' % (s.mj.get('dedup_bypass', 0), pct_s(s.mj.get('dedup_bypass_ratio_pct')))) for s in scenes]))
    out.append(row('去重 Cache 实际占用 cacheline (avg/max, 总256)',
                   [('%s / %d' % (f1(d.get('avg')), d.get('max', 0))) for d in dl]))
    out.append(row('去重 Cache 主请求占位行 avg (is_req=1)', [f1(d.get('main_req')) for d in dl]))
    out.append(row('去重 Buffer Peak / 平均占用 (entries/320)',
                   [('%d / %s (%s)' % (s.mj.get('buf_peak', 0), f1(s.mj.get('buf_avg_occ')), pct_s(s.mj.get('buf_avg_pct')))) for s in scenes]))
    out.append(row('去重 Buffer 满时 bypass (正常预取)', [str(s.mj.get('dedup_buffull_bypass', NA)) for s in scenes]))
    out.append(row('Buffer 持有延时·主任务(dedup MISS) avg (ns)',
                   [f1((s.mj.get('hold_main') or [0, 0, 0, 0])[1] + (s.mj.get('hold_main') or [0, 0, 0, 0, 0])[2] + (s.mj.get('hold_main') or [0, 0, 0, 0, 0])[3], 1) for s in scenes]))
    out.append(row('Buffer 持有延时·主任务(命中PF占位) avg (ns)',
                   [f1(sum((s.mj.get('hold_pf') or [0, 0, 0, 0])[1:4]), 1) for s in scenes]))
    out.append(row('Buffer 持有延时·被去重挂起任务 avg (ns)',
                   [f1(sum((s.mj.get('hold_susp') or [0, 0, 0, 0])[1:4]), 1) for s in scenes]))
    out.append(row('Buffer 持有延时·预取任务', [NA + ' (预取不申请Buffer entry, 模型设计)' for s in scenes]))
    # 6 PTW
    out.append(row('PTW 总任务 (主+预取)', [str(int(s.mj.get('ptw_completed', 0))) for s in scenes]))
    out.append(row('  主任务 / 其中正常 / 其中bypass',
                   [('%d / %d / %d' % (int(s.mj.get('ptw_main', 0)), int(s.mj.get('ptw_main_normal', 0)), int(s.mj.get('ptw_main_bypass', 0)))) for s in scenes]))
    out.append(row('  预取任务', [str(int(s.mj.get('ptw_prefetch', 0))) for s in scenes]))
    out.append(row('PTW 占 IOMMU 入口总任务比 / 占数据任务比',
                   [('%s / %s' % (pct_s(s.mj.get('ptw_ratio_all')), pct_s(s.mj.get('ptw_to_data_ratio_pct')))) for s in scenes]))
    out.append(row('PTW 并发 peak/上限 与 时间加权平均',
                   [('%d/64, avg %s (util %s)' % (s.mj.get('conc_ptw', [0, 0])[0], f1(s.mj.get('ptw_conc_avg'), 2), pct_s(s.mj.get('ptw_conc_util')))) for s in scenes]))
    out.append(row('PTW IOPS (主+预取, M tasks/s)', [f1(s.mj.get('ptw_iops')) for s in scenes]))
    out.append(row('PTW 主任务平均 DDR 次数', [f1(s.mj.get('ptw_main_ddr')) for s in scenes]))
    out.append(row('PTW 预取任务平均 DDR 次数', [f1(s.mj.get('ptw_pf_ddr')) for s in scenes]))
    out.append(row('PTW 执行延时 (主+预取) avg (ns)', [f1(s.mj.get('ptw_exec_all_avg_ns')) for s in scenes]))
    out.append(row('PTW 执行延时 (仅主任务) avg (ns)', [f1(s.mj.get('ptw_exec_main_avg_ns')) for s in scenes]))
    out.append(row('PTW 注入任务平均间隔 (ns)', [f1(s.mj.get('ptw_inject_interval_ns')) for s in scenes]))
    out.append(row('PTW 输出任务平均间隔 (ns)', [f1(s.mj.get('ptw_output_interval_ns')) for s in scenes]))
    out.append('')
    out.append('## 口径备注')
    out.append('')
    out.append('1. "稳态窗口"= 完成事务的 10%~90%；端口带宽稳态值取自日志 `Port Bandwidth (稳态窗口/注入期口径)` 段。')
    out.append('2. 并发"平均"给两种窗口: 全程采样窗(时间加权面积法, 三场景都有, 主口径)与注入窗口'
               '(仅场景2/3, 因场景1 的 real8 发送流程不开关注入窗口, 该段对场景1 不适用)。')
    out.append('3. e2e 拆分取自 md_trace summary CSV 的 avg_port_ns(ACCEPT→RESPONSE); 控制包=type1/2/3 加权'
               '(场景1 的控制包只有 16B 写 type1; 场景2/3 为 SQ type1 + CQ type2)。')
    out.append('4. 去重 Buffer 持有延时按 W1+W2+W3 之和给出; 预取任务不申请 Buffer entry(只插 dedup Cache 占位行), 故无持有延时。')
    report = '\n'.join(out)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(report + '\n')
        print('[EXT_METRICS] written: %s (%d rows)' % (args.out, len(out)))
    else:
        print(report)
    return 0


if __name__ == '__main__':
    sys.exit(main())
