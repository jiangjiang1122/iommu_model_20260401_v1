#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""16盘 vs 8盘实测回放 37项固定指标逐项对比。

输入:
  --r16-json  16盘 real16_run_perf.json (rp/analyze_real16_trace.py 产物)
  --r8-json   8盘 real8_run_perf.json  (rp/analyze_real8_trace.py 产物,
               默认取 rwsplit_results/real8_trace_20261022_100602/, 即用户锚点:
               稳态 IOPS 226.631 Mreq/s、读写端口平均带宽 56.46 GB/s)
输出:
  <out-prefix>_compare.md / <out-prefix>_compare.csv

对比口径: 每项取"稳态W"主数值(与两侧报告 W 列同源, 均为各自测量的
W=(t10,t90] 窗口), 8盘 N=20000/16盘 N=40000; 差值=16盘-8盘, 比值=16盘/8盘。
"""

import argparse
import csv
import json
import os
import sys

# 每项的取值定义: (json key, 字段, 备注后缀)
# 字段路径基于 analyze_*_trace.py 的 metrics 结构。
PICK = {
    1:  ("01", "all", "IOPS ALL"),
    2:  ("02", "agg", "注入间隔聚合"),
    3:  ("03", "agg", "入口聚合带宽"),
    4:  ("04", "read", "读口带宽"),
    5:  ("05", "write", "写口带宽"),
    6:  ("06", "avg", "Global并发平均"),
    7:  ("07", "avg", "Read并发平均"),
    8:  ("08", "avg", "Write并发平均"),
    9:  ("09", "all", "e2e ALL平均"),
    10: ("10", "all_max", "e2e最大"),
    11: ("11", "avg", "4KB页平均并发"),
    12: ("12", "agg", "输入端口间隔"),
    13: ("13", "all", "输出端口间隔(全期)"),
    14: ("14", "w_rate", "DC命中率%"),
    15: ("15", "w_rate", "PC命中率%"),
    16: ("16", "w_rate", "PT命中率%"),
    17: ("17", "w_rate", "PT 512B命中率%"),
    18: ("18", "w_rate", "PT CTRL命中率%"),
    19: ("19", "w_rate", "Walker C3命中率%"),
    20: ("20", "w_rate", "Walker C2命中率%"),
    21: ("21", "w_rate", "S2 C3命中率%"),
    22: ("22", "w_rate", "MSIPT命中率%"),
    23: ("23", "w_rate", "去重Cache命中率%"),
    24: ("24", "w_conflicts", "hash冲突次数"),
    25: ("25", "w_bypass", "bypass任务数"),
    26: ("26", "w_avg_v", "去重Cache平均占用"),
    27: ("27", "avg", "Buffer平均占用"),
    28: ("28", "count", "Buffer满bypass"),
    29: ("29", "main.avg", "Buffer主任务持有"),
    30: ("30", "w_total", "PTW任务数(W)"),
    31: ("31", "w_avg", "PTW并发平均"),
    32: ("32", "w_iops", "PTW IOPS"),
    33: ("33", "avg", "主任务DDR次数"),
    34: ("34", "avg", "预取任务DDR次数"),
    35: ("35", "w_group_avg", "PTW组执行延时"),
    36: ("36", "all", "PTW注入间隔"),
    37: ("37", "all", "PTW输出间隔"),
}

TITLES = {
    1: "IOMMU IOPS 稳态", 2: "平均任务注入间隔",
    3: "IOMMU端口带宽(读写共128GB/s)", 4: "IOMMU端口带宽(单独读)",
    5: "IOMMU端口带宽(单独写)", 6: "IOMMU并发峰值/平均(640)",
    7: "IOMMU Read并发峰值/平均(320)", 8: "IOMMU Write并发峰值/平均(320)",
    9: "IO avg e2e(DATA/CTRL)", 10: "IO max e2e", 11: "4KB页平均并发",
    12: "IOMMU输入端口注入任务平均间隔",
    13: "IOMMU输出端口注入任务平均间隔", 14: "DC Cache命中率",
    15: "PC Cache命中率", 16: "PT Cache命中率",
    17: "PT Cache 512byte数据命中率",
    18: "PT Cache控制信息命中率(SQ/CQ/MSI对照)",
    19: "Walker Cache C3命中率", 20: "Walker Cache C2命中率",
    21: "S2 Walker Cache C3命中率", 22: "MSIPT Cache命中率",
    23: "去重Cache命中率", 24: "去重Cache hash冲突及比例",
    25: "去重Cache bypass任务数及比例",
    26: "去重Cache实际占用cacheline", 27: "去重Buffer峰值/平均占用",
    28: "去重Buffer满bypass(正常/预取)",
    29: "Buffer持有延时(主/预取)", 30: "PTW总任务数(主+预取)",
    31: "PTW并发峰值/平均", 32: "PTW IOPS(主+预取)",
    33: "PTW主任务平均DDR次数", 34: "PTW预取任务平均DDR次数",
    35: "PTW(主+预取)执行延时", 36: "PTW注入任务间隔",
    37: "PTW输出任务间隔",
}


def dig(obj, path):
    """支持 'a.b' 形式的两级字段路径; 缺失返回 None。"""
    cur = obj
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def fs(v, nd=3):
    return "N/A" if v is None else ("%.*f" % (nd, v))


def main():
    ap = argparse.ArgumentParser(description="16盘 vs 8盘 37项指标对比")
    ap.add_argument("--r16-json", required=True)
    ap.add_argument("--r8-json", default=os.path.join(
        "rwsplit_results", "real8_trace_20261022_100602", "real8_run_perf.json"))
    ap.add_argument("--out-prefix", required=True)
    args = ap.parse_args()

    with open(args.r16_json, encoding="utf-8") as f:
        j16 = json.load(f)
    with open(args.r8_json, encoding="utf-8") as f:
        j8 = json.load(f)
    m16 = j16["metrics"]
    m8 = j8["metrics"]
    w16 = j16.get("window", {})
    w8 = j8.get("window", {})

    # 用户锚点: 读写端口平均带宽 = (读口带宽+写口带宽)/2
    avg_rw16 = None
    b_r, b_w = fnum(dig(m16, "04.read")), fnum(dig(m16, "05.write"))
    if b_r is not None and b_w is not None:
        avg_rw16 = (b_r + b_w) / 2.0
    avg_rw8 = None
    b_r8, b_w8 = fnum(dig(m8, "04.read")), fnum(dig(m8, "05.write"))
    if b_r8 is not None and b_w8 is not None:
        avg_rw8 = (b_r8 + b_w8) / 2.0
    iops16 = fnum(dig(m16, "01.all"))
    iops8 = fnum(dig(m8, "01.all"))

    rows = []   # [no, title, unit, v16, v8, diff, ratio]
    for no in range(1, 38):
        key, field, _tag = PICK[no]
        v16 = fnum(dig(m16, "%s.%s" % (key, field)))
        v8 = fnum(dig(m8, "%s.%s" % (key, field)))
        diff = (v16 - v8) if (v16 is not None and v8 is not None) else None
        rt = (v16 / v8) if (v16 is not None and v8 not in (None, 0.0)) else None
        rows.append([no, TITLES[no], v16, v8, diff, rt])

    with open(args.out_prefix + "_compare.csv", "w", encoding="utf-8-sig",
              newline="") as f:
        w = csv.writer(f)
        w.writerow(["no", "metric", "real16_steady_W", "real8_steady_W",
                    "diff(16-8)", "ratio(16/8)"])
        for no, title, v16, v8, diff, rt in rows:
            w.writerow([no, title, fs(v16, 6), fs(v8, 6), fs(diff, 6), fs(rt, 4)])
        w.writerow([])
        w.writerow(["avg_rw_bw", "读写端口平均带宽(GB/s)", fs(avg_rw16, 3),
                    fs(avg_rw8, 3),
                    fs((avg_rw16 - avg_rw8) if avg_rw16 is not None and
                       avg_rw8 is not None else None, 3),
                    fs(avg_rw16 / avg_rw8 if avg_rw16 and avg_rw8 else None, 4)])

    # ---- 归因分段 ----
    def util(key, field):
        return fnum(dig(m16, "%s.%s" % (key, field)))

    warn = []
    for no, title, v16, v8, diff, rt in rows:
        if no in (1, 3) and rt is not None and not (0.85 <= rt <= 1.15):
            warn.append("第%d项 %s: 比值 %s 偏离±15%%, 需人工归因"
                        % (no, title, fs(rt, 3)))

    lines = [
        "# 16盘 vs 8盘 实测回放 37项固定指标对比",
        "",
        "- 16盘: `%s` (N=%s, W=(%s, %s] ns)" %
        (args.r16_json, j16.get("N_manifest"), fs(w16.get("t10_ns")),
         fs(w16.get("t90_ns"))),
        "- 8盘锚点: `%s` (N=%s, W=(%s, %s] ns)" %
        (args.r8_json, j8.get("N_manifest"), fs(w8.get("t10_ns")),
         fs(w8.get("t90_ns"))),
        "",
        "## 用户锚点对账",
        "",
        "| 锚点 | 8盘 | 16盘 | 比值 |",
        "|---|---|---|---|",
        "| 稳态 IOPS (Mreq/s) | %s | %s | %s |" %
        (fs(iops8), fs(iops16),
         fs(iops16 / iops8 if iops16 and iops8 else None, 4)),
        "| 读写端口平均带宽 (GB/s) | %s | %s | %s |" %
        (fs(avg_rw8), fs(avg_rw16),
         fs(avg_rw16 / avg_rw8 if avg_rw16 and avg_rw8 else None, 4)),
        "",
        "## 37项逐项对比 (稳态W口径)",
        "",
        "| 序号 | 指标 | 16盘 | 8盘 | 差值(16-8) | 比值(16/8) |",
        "|---:|---|---|---|---|---|",
    ]
    for no, title, v16, v8, diff, rt in rows:
        lines.append("| %d | %s | %s | %s | %s | %s |" %
                     (no, title, fs(v16, 6), fs(v8, 6), fs(diff, 6), fs(rt, 4)))

    sat = (
        "",
        "## 归因说明",
        "",
        "(a) **预期持平项(吞吐瓶颈在 IOMMU 内部, 与盘数无关)**:",
        "- 读口 64GB/s 已 100%% 饱和(8盘 util=%s%%), 写并发 320 已满"
        "(8盘 full=%s%%), 全局 640 高占用(8盘 util=%s%%); 16盘负载翻倍后这些"
        "瓶颈只会更饱和, 稳态 IOPS 应与 8盘同量级。" %
        (fs(fnum(dig(m8, "04.util")), 2), fs(fnum(dig(m8, "08.full_ratio")), 2),
         fs(fnum(dig(m8, "06.util")), 2)),
        "- 命中率类指标(14-23)由访问模式与 Cache 容量决定: 16盘独立页数翻倍、"
        "去重Cache条目仍256条, 命中率小幅波动属预期。",
        "(b) **预期变化项**: W 时长与注入规模(40000 vs 20000)翻倍; PT Cache/"
        "去重Cache命中率、PTW 主/预取比随两组独立页流变化; e2e 分布随排队深度变化。",
        "(c) **异常偏离警示**: %s" % ("; ".join(warn) if warn else "无"),
    )
    lines += sat

    with open(args.out_prefix + "_compare.md", "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("[compare] 输出: %s_compare.{md,csv}" % args.out_prefix, flush=True)
    print("[compare] IOPS 16/8 = %s / %s; 平均读写带宽 16/8 = %s / %s GB/s" %
          (fs(iops16), fs(iops8), fs(avg_rw16), fs(avg_rw8)), flush=True)


if __name__ == "__main__":
    sys.exit(main())
