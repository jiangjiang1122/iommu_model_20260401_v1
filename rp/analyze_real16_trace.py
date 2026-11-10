#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[16盘实测回放] 37项固定指标离线分析(沿用8盘计划第六节口径)。

只读关联运行目录产物, 不改公共提取脚本:
  real16_input/real16_manifest.json  输入清单(N=totals_16dev.rows, 默认40000)
  real16_trace_requests.csv          请求生命周期(md_trace report)
  real16_operations_raw.csv          流式 Cache/DDR/PTW 操作(宏内导出, 无 in_window 列)
  real16_dedup_occupancy.csv         去重Cache占用变化采样(宏内新增)
  real16_trace_pairs.csv             全局有效页配对表(含组别列, 残页)
  仿真日志                            Buffer/PTW/输出端口等模型内统计

16盘特有: 文末追加“16盘专项验收”节 —— 两组独立映射断言(组间mapped_page
不相交)、逐包交替轮询顺序抽查、16设备逐设备完成对账与Jain、IOVA池用量
对manifest上界校验。

统一窗口 W=(t10,t90]: 按正式请求(测量epoch, N=manifest totals.rows)响应完成顺序,
第 floor(N*10%) 与 floor(N*90%) 笔(1-based)的 response_ns。并发/带宽/间隔/命中率
均在 W 内重算; 模型内仅有全期口径的项如实标注 N/A(附原因)。同刻释放先于准入。

用法:
  python3 rp/analyze_real8_trace.py --run-dir <dir> --log <sim.log> --out-prefix <pfx>
输出: <pfx>_perf.md / <pfx>_perf.csv / <pfx>_perf.json
"""
import argparse
import csv
import json
import math
import os
import sys

# ------------------------------------------------------------ 基础工具
def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return 0.0


def ratio(num, den):
    return (100.0 * num / den) if den > 0 else None


def fmt_r(v):
    return "N/A" if v is None else "%.4f" % v


def fmt_f(v, nd=3):
    return "N/A" if v is None else ("%.*f" % (nd, v))


class Counter:
    """按 (source, operation, level) 维度聚合 W 内与全期事件。"""
    __slots__ = ("w_queries", "w_hits", "all_queries", "all_hits")

    def __init__(self):
        self.w_queries = 0
        self.w_hits = 0
        self.all_queries = 0
        self.all_hits = 0

    def add(self, hit, in_w):
        self.all_queries += 1
        self.all_hits += hit
        if in_w:
            self.w_queries += 1
            self.w_hits += hit


class ConcIntegrator:
    """ACCEPT(+1)/RELEASE(-1) 事件流在 W=(t0,t1] 内积分。

    带入窗口前在途任务: 区间与 W 相交即计入(起点并发为初值);
    同刻先释放后准入; peak 取 W 内每次事件后的最大值。
    """
    def __init__(self, cap):
        self.cap = cap
        self.events = []  # (t, delta)

    def add(self, t_in, t_out):
        self.events.append((t_in, 1))
        self.events.append((t_out, -1))

    def solve(self, t0, t1):
        evs = sorted(self.events, key=lambda e: (e[0], e[1]))  # -1 先于 +1
        n = len(evs)
        # 窗口起点在途初值: t0 前事件差值(同刻 -1 先于 +1)
        init = 0
        idx = 0
        while idx < n and evs[idx][0] <= t0:
            init += evs[idx][1]
            idx += 1
        area = 0.0
        cur = init
        peak = cur
        full_ns = 0.0
        t_prev = t0
        c_prev = cur
        # 每段只累计一次，末段统一在循环后补齐。
        while idx < n and evs[idx][0] <= t1:
            t_ev, d = evs[idx]
            area += c_prev * (t_ev - t_prev)
            if c_prev == self.cap and self.cap > 0:
                full_ns += t_ev - t_prev
            cur = c_prev + d
            if cur > peak:
                peak = cur
            t_prev = t_ev
            c_prev = cur
            idx += 1
        area += c_prev * (t1 - t_prev)
        if c_prev == self.cap and self.cap > 0:
            full_ns += (t1 - t_prev)
        dur = t1 - t0
        return {
            "peak": peak, "init": init,
            "avg": (area / dur) if dur > 0 else None,
            "full_ratio": ratio(full_ns, dur),
        }


class PageIntegrator:
    """按页引用数的 0↔1 变化统计不同在途页，不累加同页请求数。"""
    def __init__(self):
        self.events = []

    def add(self, key, t_in, t_out):
        assert t_out >= t_in, "页生命周期倒序"
        if t_out > t_in:
            self.events.extend(((t_in, 1, key), (t_out, -1, key)))

    def solve(self, t0, t1):
        refs = {}
        distinct = ConcIntegrator(0)
        for t, delta, key in sorted(self.events, key=lambda x: (x[0], x[1])):
            old = refs.get(key, 0)
            new = old + delta
            assert new >= 0, "页引用计数下溢"
            refs[key] = new
            if old == 0:
                distinct.events.append((t, 1))
            elif new == 0:
                distinct.events.append((t, -1))
        assert all(v == 0 for v in refs.values()), "页引用未排空"
        result = distinct.solve(t0, t1)
        result["n_pages"] = len(refs)
        return result


def add_walker_lookup(cat, src, op, level, hit, in_w):
    """VS RAM 记录目标子表；S2 汇总记录的是最终命中层级。"""
    if op != "lookup":
        return
    if src == "walker_vs_ram":
        cat("walker_vs", "lookup", level).add(hit, in_w)
    elif src == "walker_s2":
        cat("walker_s2", "lookup").add(hit, in_w)
        # S2 总查询都会先查 C3；只有 C3 miss 才查询 C2。
        cat("walker_s2", "lookup", 3).add(int(hit and level == 3), in_w)
        if not (hit and level == 3):
            cat("walker_s2", "lookup", 2).add(int(hit and level == 2), in_w)


# ------------------------------------------------------------ 主流程
def main():
    ap = argparse.ArgumentParser(description="16盘实测回放 37项固定指标分析")
    ap.add_argument("--run-dir", required=True)
    ap.add_argument("--log", required=True, help="仿真日志(模型内全期统计来源)")
    ap.add_argument("--out-prefix", required=True)
    args = ap.parse_args()

    rd = args.run_dir
    with open(os.path.join(rd, "real16_input", "real16_manifest.json"),
              encoding="utf-8") as f:
        man = json.load(f)
    N = man["totals_16dev"]["rows"]

    # ---------------- requests.csv 加载(测量 epoch) ----------------
    req_path = os.path.join(rd, "real16_trace_requests.csv")
    epoch_rows = {}
    with open(req_path, encoding="utf-8") as f:
        rdr = csv.reader(f)
        header = next(rdr)
        col = {name: i for i, name in enumerate(header)}
        for row in rdr:
            epoch_rows[row[col["epoch"]]] = epoch_rows.get(row[col["epoch"]], 0) + 1
    meas_epoch = None
    for e, c in epoch_rows.items():
        if c == N:
            meas_epoch = e
            break
    if meas_epoch is None:
        meas_epoch = max(epoch_rows, key=lambda k: epoch_rows[k])
    print("[analyze] epoch行数=%s 测量epoch=%s(=%d行)" %
          (epoch_rows, meas_epoch, epoch_rows[meas_epoch]), flush=True)

    R = {k: [] for k in ("did", "seq", "type", "write", "bytes", "task_id",
                         "iova", "accept_ns", "pop_ns", "response_ns",
                         "release_ns")}
    with open(req_path, encoding="utf-8") as f:
        rdr = csv.reader(f)
        next(rdr)
        c = col
        for row in rdr:
            if row[c["epoch"]] != meas_epoch:
                continue
            R["did"].append(int(row[c["did"]]))
            R["seq"].append(int(row[c["seq"]]))
            R["type"].append(int(row[c["type"]]))
            R["write"].append(int(row[c["write"]]))
            R["bytes"].append(int(row[c["bytes"]]))
            R["task_id"].append(int(row[c["task_id"]]))
            R["iova"].append(int(row[c["iova"]]))
            R["accept_ns"].append(fnum(row[c["accept_ns"]]))
            R["pop_ns"].append(fnum(row[c["pop_ns"]]))
            R["response_ns"].append(fnum(row[c["response_ns"]]))
            R["release_ns"].append(fnum(row[c["release_ns"]]))
    n_req = len(R["did"])
    print("[analyze] 测量期请求数=%d (期望%d)" % (n_req, N), flush=True)

    # ---------------- 稳态窗口 W=(t10,t90] ----------------
    order = sorted(range(n_req), key=lambda i: (R["response_ns"][i], i))
    if n_req != N:
        print("[analyze][WARN] 请求数与manifest不一致, 按实际n_req取分位", flush=True)
        nn = n_req
    else:
        nn = N
    i10 = math.floor(nn * 0.10) - 1
    i90 = math.floor(nn * 0.90) - 1
    t10 = R["response_ns"][order[i10]]
    t90 = R["response_ns"][order[i90]]
    win = t90 - t10
    print("[analyze] W=(%.3f, %.3f] Δt=%.3f ns" % (t10, t90, win), flush=True)

    is_data = [t == 0 for t in R["type"]]
    is_ctrl = [t == 1 for t in R["type"]]

    # ---------------- 指标1/2/9/10/12: IOPS/间隔/e2e ----------------
    in_w_resp = [t10 < t <= t90 for t in R["response_ns"]]
    in_w_acc = [t10 < t <= t90 for t in R["accept_ns"]]

    def iops_of(idxs):
        return (len(idxs) / win * 1000.0) if win > 0 else None

    idx_resp_w = [i for i in range(n_req) if in_w_resp[i]]
    idx_acc_w = [i for i in range(n_req) if in_w_acc[i]]
    idx_data_w = [i for i in idx_resp_w if is_data[i]]
    idx_ctrl_w = [i for i in idx_resp_w if is_ctrl[i]]
    idx_r_acc_w = [i for i in idx_acc_w if R["write"][i] == 0]
    idx_w_acc_w = [i for i in idx_acc_w if R["write"][i] == 1]
    idx_r_resp_w = [i for i in idx_resp_w if R["write"][i] == 0]
    idx_w_resp_w = [i for i in idx_resp_w if R["write"][i] == 1]

    m = {}  # 结果容器: (序号, 名称) -> {值, 单位, 窗口, 样本, 说明}

    m["01"] = {
        "name": "IOMMU IOPS 稳态",
        "all": iops_of(idx_resp_w), "data": iops_of(idx_data_w),
        "ctrl": iops_of(idx_ctrl_w),
        "read": iops_of(idx_r_resp_w), "write": iops_of(idx_w_resp_w),
        "unit": "Mreq/s", "n_w": len(idx_resp_w),
    }
    # 注入间隔(聚合ACCEPT, W内)
    def interval_avg(idxs, key):
        ts = sorted((R[key][i], i) for i in idxs)
        if len(ts) < 2:
            return None, len(ts)
        return (ts[-1][0] - ts[0][0]) / (len(ts) - 1), len(ts)

    agg_iv, agg_n = interval_avg(idx_acc_w, "accept_ns")
    r_iv, r_n = interval_avg(idx_r_acc_w, "accept_ns")
    w_iv, w_n = interval_avg(idx_w_acc_w, "accept_ns")
    m["02"] = {"name": "平均任务注入间隔", "agg": agg_iv, "read": r_iv,
               "write": w_iv, "unit": "ns", "n_agg": agg_n}
    m["12"] = {"name": "IOMMU输入端口注入任务平均间隔", "agg": agg_iv,
               "read": r_iv, "write": w_iv, "unit": "ns", "n_agg": agg_n,
               "note": "与第02项同源(聚合ACCEPT)"}

    # e2e = RESPONSE - POP, 响应落入W
    def e2e_stats(idxs):
        if not idxs:
            return None, 0, None, None
        vals = [(R["response_ns"][i] - R["pop_ns"][i], i) for i in idxs]
        avg = sum(v for v, _ in vals) / len(vals)
        mx, mi = max(vals)
        return avg, len(vals), mx, mi

    e2e_data, n_d, mx_d, mi_d = e2e_stats(idx_data_w)
    e2e_ctrl, n_c, mx_c, mi_c = e2e_stats(idx_ctrl_w)
    e2e_all, n_a, mx_a, mi_a = e2e_stats(idx_resp_w)
    m["09"] = {"name": "IO avg e2e(DATA/CTRL分开)", "data": e2e_data,
               "ctrl": e2e_ctrl, "all": e2e_all, "unit": "ns",
               "n_data": n_d, "n_ctrl": n_c,
               "note": "RESPONSE-POP(parser取出时重置task timestamp)"}
    m["10"] = {"name": "IO max e2e", "all_max": mx_a, "data_max": mx_d,
               "ctrl_max": mx_c,
               "all_id": None if mi_a is None else
                         [R["did"][mi_a], R["seq"][mi_a],
                          "W" if R["write"][mi_a] else "R",
                          R["bytes"][mi_a]],
               "data_id": None if mi_d is None else
                          [R["did"][mi_d], R["seq"][mi_d],
                           "W" if R["write"][mi_d] else "R", R["bytes"][mi_d]],
               "ctrl_id": None if mi_c is None else
                          [R["did"][mi_c], R["seq"][mi_c],
                           "W" if R["write"][mi_c] else "R",
                           R["bytes"][mi_c]],
               "unit": "ns"}

    # ---------------- 指标3/4/5: 端口带宽(实际字节) ----------------
    def bw(idxs):
        return (sum(R["bytes"][i] for i in idxs) / win) if win > 0 else None

    bw_agg = bw(idx_acc_w)
    bw_r = bw(idx_r_acc_w)
    bw_w = bw(idx_w_acc_w)
    m["03"] = {"name": "IOMMU端口带宽(读写共128GB/s)", "agg": bw_agg,
               "util": ratio(bw_agg, 128.0), "unit": "GB/s(B/ns)"}
    m["04"] = {"name": "IOMMU端口带宽(单独读通道)", "read": bw_r,
               "util": ratio(bw_r, 64.0), "unit": "GB/s"}
    m["05"] = {"name": "IOMMU端口带宽(单独写通道)", "write": bw_w,
               "util": ratio(bw_w, 64.0), "unit": "GB/s"}

    # ---------------- 指标6/7/8: 并发(ACCEPT->RELEASE, W内积分) ----------------
    g_conc = ConcIntegrator(640)
    r_conc = ConcIntegrator(320)
    w_conc = ConcIntegrator(320)
    for i in range(n_req):
        g_conc.add(R["accept_ns"][i], R["release_ns"][i])
        (w_conc if R["write"][i] else r_conc).add(R["accept_ns"][i],
                                                  R["release_ns"][i])
    for key, res in (("06", g_conc.solve(t10, t90)),
                     ("07", r_conc.solve(t10, t90)),
                     ("08", w_conc.solve(t10, t90))):
        cap = g_conc.cap if key == "06" else 320
        m[key] = {"name": {6: "Global并发peak/avg", 7: "Read并发peak/avg",
                           8: "Write并发peak/avg"}[int(key)],
                  "peak": res["peak"], "avg": res["avg"],
                  "util": ratio(res["avg"], cap),
                  "full_ratio": res["full_ratio"],
                  "init": res["init"], "cap": cap, "unit": "个"}
    # 记录释放顺序(供同刻校验)
    # G=R+W 验证在 report 段做逐时刻校验说明。

    # ---------------- 指标11: 4KB页平均并发(DATA, (did,映射后页)) ----------------
    page_conc = PageIntegrator()
    page_dirs = [PageIntegrator(), PageIntegrator()]
    page_all = PageIntegrator()
    page_devs = {d: PageIntegrator() for d in set(R["did"])}
    data_refs = ConcIntegrator(640)
    for i in range(n_req):
        key = (R["did"][i], R["iova"][i] >> 12)
        times = (R["accept_ns"][i], R["release_ns"][i])
        page_all.add(key, *times)
        if not is_data[i]:
            continue
        page_conc.add(key, *times)
        page_dirs[R["write"][i]].add(key, *times)
        page_devs[R["did"][i]].add(key, *times)
        data_refs.add(*times)
    m["11"] = {"name": "4KB页平均并发(DATA,(did,映射后IOVA页))",
               **page_conc.solve(t10, t90),
               "read": page_dirs[0].solve(t10, t90),
               "write": page_dirs[1].solve(t10, t90),
               "including_ctrl": page_all.solve(t10, t90),
               "per_device": {d: p.solve(t10, t90) for d, p in page_devs.items()},
               "data_task_concurrency": data_refs.solve(t10, t90),
               "unit": "页",
               "note": "Σ 1{该(DID,IOVA页)在途引用数>0}，峰值由全局事件流精确计算；"
                       "data_task_concurrency仅作旧错误口径对照，不是页数"}
    assert m["11"]["avg"] <= m["11"]["data_task_concurrency"]["avg"] + 1e-6
    assert m["11"]["peak"] <= m["06"]["peak"] <= 640

    print("[analyze] 请求侧指标完成", flush=True)

    # ---------------- operations.csv 单遍扫描 ----------------
    ops_path = os.path.join(rd, "real16_operations_raw.csv")
    CATS = {}
    def cat(src, op, level=None):
        key = (src, op, level)
        if key not in CATS:
            CATS[key] = Counter()
        return CATS[key]

    task_kind = {}  # task_id -> 0主请求/1CTRL/2内部(预取等)
    for i in range(n_req):
        task_kind[R["task_id"][i]] = 2 if is_ctrl[i] else 1
        # 1=DATA主请求(占位), 后面用独立字典存 is_ctrl/bytes
    task_ctrl = {}
    task_bytes = {}
    for i in range(n_req):
        task_ctrl[R["task_id"][i]] = 1 if is_ctrl[i] else 0
        task_bytes[R["task_id"][i]] = R["bytes"][i]

    pt_total = Counter(); pt_data = Counter(); pt_512 = Counter(); pt_ctrl = Counter()
    iv_main = []; iv_pf = []; iv_grp = []   # PTW注入/输出事件时刻
    grp_exec = []                            # 任务组执行延时
    grp_done_tasks = 0                       # W内完成任务数(Σ group_complete.value)
    dedup_main = Counter(); dedup_inner = Counter()
    bypass_task_ids = []
    buf_iv = []                              # dedup flush 区间(主/内部)
    bf_conflict = 0
    ddrcnt = Counter()
    ptw_main_ids, ptw_pf_ids = set(), set()
    ptw_ddr_reads = {}
    dedup_pf_skip = 0

    with open(ops_path, encoding="utf-8") as f:
        rdr = csv.reader(f)
        next(rdr)
        for row in rdr:
            # epoch,did,task_id,source,operation,bank,level,hit,value,start_ns,end_ns,queue_ns
            try:
                src = row[3]; op = row[4]
                level = int(row[6]); hit = int(row[7])
                value = int(row[8]); s = fnum(row[9]); e = fnum(row[10])
                tid = int(row[2])  # 与 requests.task_id 同为 int, 供任务归属关联
            except (ValueError, IndexError):
                continue
            in_w = t10 < e <= t90
            if src == "pt_cache" and op == "lookup":
                pt_total.add(hit, in_w)
                kind = task_ctrl.get(tid, -1)
                if kind == 0:
                    pt_data.add(hit, in_w)
                    if task_bytes.get(tid, -1) == 512:
                        pt_512.add(hit, in_w)
                elif kind == 1:
                    pt_ctrl.add(hit, in_w)
            elif src == "dc_cache" and op == "lookup":
                cat("dc_cache", "lookup").add(hit, in_w)
            elif src == "pc_cache" and op == "lookup":
                cat("pc_cache", "lookup").add(hit, in_w)
            elif src == "msipt_cache" and op == "lookup":
                cat("msipt_cache", "lookup").add(hit, in_w)
            elif src in ("walker_vs_ram", "walker_s2"):
                add_walker_lookup(cat, src, op, level, hit, in_w)
            elif src == "dedup_cache" and op == "lookup":
                if tid in task_kind and task_kind.get(tid, 2) != 2:
                    dedup_main.add(hit, in_w)
                elif tid in task_kind and task_kind.get(tid) == 2:
                    dedup_main.add(hit, in_w)   # CTRL主请求
                else:
                    dedup_inner.add(hit, in_w)  # 预取占位等内部查询
            elif src == "dedup" and op == "hash_conflict_bypass":
                if in_w:
                    bf_conflict += 1
                    bypass_task_ids.append(tid)
            elif src == "dedup" and op == "flush":
                if t10 < e <= t90 or (s <= t10 and e > t10):
                    buf_iv.append((s, e, tid))
            elif src == "ptw":
                if op == "main_start":
                    ptw_main_ids.add(tid)
                elif op == "prefetch_spawn":
                    ptw_pf_ids.add(tid)
                if op == "main_start" and t10 < s <= t90:
                    iv_main.append(s)
                elif op == "prefetch_spawn" and t10 < s <= t90:
                    iv_pf.append(s)
                elif op == "group_complete":
                    if in_w:
                        iv_grp.append(e)
                        grp_exec.append(e - s)
                        grp_done_tasks += value  # 组大小(1主+D预取)
            elif src == "ddr" and op == "read":
                ddrcnt.add(hit, in_w)
                if int(row[5]) == 1:
                    ptw_ddr_reads[tid] = ptw_ddr_reads.get(tid, 0) + 1

    print("[analyze] operations扫描完成", flush=True)

    # ---------------- 日志全期统计(模型内报告) ----------------
    logstats = parse_log(args.log)

    # ---------------- 指标14-25: Cache命中率 ----------------
    def rate_block(c, name, note=""):
        return {"name": name, "w_hits": c.w_hits, "w_queries": c.w_queries,
                "w_rate": ratio(c.w_hits, c.w_queries),
                "all_hits": c.all_hits, "all_queries": c.all_queries,
                "all_rate": ratio(c.all_hits, c.all_queries), "note": note}

    m["14"] = rate_block(CATS.get(("dc_cache", "lookup", None), Counter()),
                         "DC Cache命中率")
    m["15"] = rate_block(CATS.get(("pc_cache", "lookup", None), Counter()),
                         "PC Cache命中率")
    m["16"] = rate_block(pt_total, "PT Cache命中率(全部)",
                         "DATA口径另见16b")
    m["16b"] = rate_block(pt_data, "PT Cache命中率(全部DATA口径)")
    m["17"] = rate_block(pt_512, "PT Cache 512byte数据信息命中率",
                         "仅is_ctrl=0且bytes=512")
    m["18"] = rate_block(pt_ctrl, "PT Cache SQ/CQ/MSI控制信息命中率",
                         "本场景为CTRL聚合(16B写控制包), 不细分SQ/CQE/MSI")
    m["19"] = rate_block(CATS.get(("walker_vs", "lookup", 3), Counter()),
                         "Walker Cache C3命中率(VS, level=3)")
    m["20"] = rate_block(CATS.get(("walker_vs", "lookup", 2), Counter()),
                         "Walker Cache C2命中率(VS, level=2)")
    m["21"] = rate_block(CATS.get(("walker_s2", "lookup", 3), Counter()),
                         "S2 Walker Cache C3命中率(level=3)")
    m["21b"] = rate_block(CATS.get(("walker_s2", "lookup", None), Counter()),
                          "S2 Walker Cache整体命中率")
    m["22"] = rate_block(CATS.get(("msipt_cache", "lookup", None), Counter()),
                         "MSIPT Cache命中率",
                         "本场景不合成MSI; queries=0时命中率N/A")
    m["23"] = rate_block(dedup_main, "去重Cache命中率(REQUEST查询,主请求)",
                         "主请求=任务ID属于回放请求(含CTRL); 预取占位查询另列")
    m["23b"] = rate_block(dedup_inner, "去重Cache命中率(预取占位等内部查询)")
    m["24"] = {"name": "去重Cache hash冲突次数及占总查询比例",
               "w_conflicts": bf_conflict,
               "ratio_of_q23": ratio(bf_conflict, dedup_main.w_queries),
               "note": "口径=operations dedup|hash_conflict_bypass 行数(按end落W); "
                       "预取占位因set-full跳过事件未单独打点, 见日志对照"}
    n_data_entry = sum(1 for i in range(n_req) if is_data[i])
    bypass_data = sum(1 for t in bypass_task_ids if task_ctrl.get(t, -1) == 0)
    bypass_ctrl = sum(1 for t in bypass_task_ids if task_ctrl.get(t, -1) == 1)
    m["25"] = {"name": "去重Cache bypass任务数及占总数据任务比例",
               "w_bypass": len(bypass_task_ids),
               "bypass_data": bypass_data, "bypass_ctrl": bypass_ctrl,
               "ratio_of_data_entry": ratio(bypass_data,
                                           sum(is_data[i] for i in idx_acc_w)),
               "note": "W内bypass任务按hash_conflict_bypass关联; 全期值见日志对照"}

    # ---------------- 指标27/29: Buffer占用与持有(区间法) ----------------
    ev = []
    for s, e, _ in buf_iv:
        ev.append((s, 1)); ev.append((e, -1))
    ev.sort(key=lambda x: (x[0], x[1]))
    init_b = 0
    for s, e, _ in buf_iv:
        if s <= t10 and e > t10:
            init_b += 1
    area_b = 0.0; cur = init_b; peak_b = init_b
    t_prev = t10; idx = 0
    while idx < len(ev) and ev[idx][0] <= t10:
        idx += 1
    while idx < len(ev) and ev[idx][0] <= t90:
        t_e, d = ev[idx]
        area_b += cur * (t_e - t_prev)
        cur += d
        peak_b = max(peak_b, cur)
        t_prev = t_e
        idx += 1
    area_b += cur * (t90 - t_prev)
    m["27"] = {"name": "去重Buffer Peak及实际平均占用", "peak": peak_b,
               "avg": (area_b / win) if win > 0 else None,
               "util": ratio(area_b / win if win > 0 else None, 320.0),
               "init": init_b, "cap": 320, "unit": "entry",
               "note": "由dedup|flush持有区间[start,end]在W内积分; "
                       "reserved(申请~正式入链)未单独打点, 不混入valid"}

    hold_main = [e - s for s, e, tid in buf_iv if task_kind.get(tid, -1) >= 1]
    hold_inner = [e - s for s, e, tid in buf_iv if tid not in task_kind]
    def hold_stats(vals):
        if not vals:
            return {"n": 0, "avg": None, "max": None, "min": None}
        return {"n": len(vals), "avg": sum(vals) / len(vals),
                "max": max(vals), "min": min(vals)}
    m["29"] = {"name": "Buffer持有延时: 主任务/预取任务",
               "main": hold_stats(hold_main), "inner": hold_stats(hold_inner),
               "unit": "ns",
               "note": "main含普通MISS链头与预取占位升级链头两类(operations无法"
                       "二次拆分, W1/W2/W3与SUSP见日志); 真实PTW预取无Buffer申请"
                       "->样本0/N/A, 不以PTW执行延时冒充"}

    # ---------------- 指标30/32/35/36/37: PTW ----------------
    pf_acc_n = len(iv_pf); main_acc_n = len(iv_main); grp_n = len(iv_grp)
    pf_acc_iv = _iv_of(iv_pf); main_acc_iv = _iv_of(iv_main)
    grp_iv = _iv_of(iv_grp)
    grp_exec_avg = (sum(grp_exec) / len(grp_exec)) if grp_exec else None
    m["30"] = {"name": "PTW总任务数(主+预取)", "w_main": main_acc_n,
               "w_prefetch": pf_acc_n, "w_total": main_acc_n + pf_acc_n,
               "all_total": logstats["ptw_ratio"]["total"],
               "all_main": logstats["ptw_ratio"]["main"],
               "all_prefetch": logstats["ptw_ratio"]["pf"],
               "all_main_bypass": logstats["conflict"]["bypass_tasks"],
               "all_main_normal": _sub_none(logstats["ptw_ratio"]["main"],
                                            logstats["conflict"]["bypass_tasks"]),
               "main_of_data_entry": ratio(logstats["ptw_ratio"]["main"],
                                           n_data_entry),
               "bypass_of_data_entry": ratio(logstats["conflict"]["bypass_tasks"],
                                             n_data_entry),
               "note": "W内按准入事件(main_start/prefetch_spawn); "
                       "main_bypass口径=日志Conflict bypass to PTW(hash冲突降级), "
                       "main=normal+bypass; W内normal/bypass拆分N/A"
                       "(operations无bypass标签)"}
    m["32"] = {"name": "PTW IOPS(主+预取全部)",
               "w_done_tasks": grp_done_tasks,
               "w_iops": (grp_done_tasks / win * 1000.0)
                         if win > 0 and grp_done_tasks else None,
               "all_iops": logstats["ptw_iops"],
               "note": "W内=Σ group_complete.value(组大小=1主+D预取)/Δt, "
                       "Mtask/s; 全期=日志PTW IOPS(分母为全仿真时长, 单独标注)"}
    m["35"] = {"name": "PTW(主+预取)执行延时", "w_group_avg": grp_exec_avg,
               "w_group_n": len(grp_exec), "unit": "ns",
               "note": "W内=group_complete(end-start)任务组口径; 单任务ALL/MAIN"
                       "均值见日志全期"}
    m["36"] = {"name": "PTW模块注入任务延时间隔", "main": main_acc_iv,
               "prefetch": pf_acc_iv, "all": _iv_of(iv_main + iv_pf),
               "unit": "ns"}
    m["37"] = {"name": "PTW模块输出任务延时间隔", "all": grp_iv,
               "prefetch": "N/A(预取无独立输出事件,并入组完成)",
               "unit": "ns"}

    # ---------------- 指标26: 去重Cache占用(占用变化CSV) ----------------
    occ_path = os.path.join(rd, "real16_dedup_occupancy.csv")
    occ = {"have": False}
    if os.path.isfile(occ_path):
        ev2 = []
        init_v = init_r = None
        end_v = end_r = None
        area_v = area_r = 0.0
        peak_v = peak_r = 0
        prev_t = t10
        cv = cr = 0
        rows = 0
        with open(occ_path, encoding="utf-8") as f:
            rdr = csv.reader(f)
            next(rdr)
            for row in rdr:
                t = fnum(row[0]); v = int(row[1]); r = int(row[2])
                rows += 1
                if t <= t10:
                    init_v = v; init_r = r; cv = v; cr = r
                    peak_v = v; peak_r = r
                    continue
                if t > t90:
                    break
                area_v += cv * (t - prev_t); area_r += cr * (t - prev_t)
                peak_v = max(peak_v, cv, v); peak_r = max(peak_r, cr, r)
                prev_t = t; cv = v; cr = r
        area_v += cv * (t90 - prev_t); area_r += cr * (t90 - prev_t)
        end_v = cv; end_r = cr
        occ = {"have": True, "rows": rows,
               "w_avg_v": (area_v / win) if win > 0 and rows else None,
               "w_avg_r": (area_r / win) if win > 0 and rows else None,
               "w_peak_v": peak_v, "w_peak_r": peak_r,
               "end_v": end_v, "end_r": end_r,
               "init_v": init_v, "init_r": init_r}
    m["26"] = {"name": "去重Cache实际占用cacheline数目", **occ, "cap": 256,
               "unit": "line",
               "note": "时间加权由占用变化点(t,valid,req)积分; 主占位=req_lines, "
                       "预取占位=valid-req; 查询采样均值见附录(不作时间加权)"}

    # ---------------- 指标31: PTW并发(占用流W内积分 + 日志全期对照) ----------------
    pocc_path = os.path.join(rd, "real16_ptw_occupancy.csv")
    if os.path.isfile(pocc_path):
        pocc = []
        with open(pocc_path, encoding="utf-8") as f:
            rdr = csv.reader(f)
            next(rdr)
            for row in rdr:
                pocc.append((fnum(row[0]), int(row[1])))
        # W=(t10,t90] 内时间加权: 阶梯信号(带入窗口起点占用)
        area31 = 0.0; peak31 = 0; init31 = 0; cur31 = 0; t_prev31 = t10
        for t, v in pocc:
            if t <= t10:
                cur31 = v
                init31 = v
                peak31 = v
                t_prev31 = t10
                continue
            if t > t90:
                break
            area31 += cur31 * (t - t_prev31)
            cur31 = v
            peak31 = max(peak31, cur31)
            t_prev31 = t
        area31 += cur31 * (t90 - t_prev31)
        peak31 = max(peak31, cur31)
        m["31"] = {"name": "PTW并发peak/实际平均占用",
                   "w_peak": peak31, "w_avg": (area31 / win) if win > 0 else None,
                   "w_util": ratio(area31 / win if win > 0 else None, 64),
                   "init": init31, "cap": 64, "unit": "个(主任务计数)",
                   "all_avg": logstats["ptw_conc"]["avg"],
                   "all_peak": logstats["ptw_conc"]["peak"],
                   "all_cap": logstats["ptw_conc"]["cap"],
                   "note": "W内按占用变化流(t_ns,outstanding)积分; "
                           "额度按主任务计数(每组=1主+D预取); 全期见模型报告"}
    else:
        m["31"] = {"name": "PTW并发peak/实际平均占用",
                   "w_peak": None, "w_avg": None, "unit": "个",
                   "all_avg": logstats["ptw_conc"]["avg"],
                   "all_peak": logstats["ptw_conc"]["peak"],
                   "all_cap": logstats["ptw_conc"]["cap"],
                   "note": "W内N/A: 无real16_ptw_occupancy.csv; 全期见模型报告"}

    # ---------------- 指标33/34/28/13: 日志全期口径 ----------------
    m["33"] = {"name": "PTW主任务平均访问DDR次数", **logstats.get("ptw_main_ddr", {}),
               "note": "全期口径(模型内完成事件累计); W内N/A: "
                       "DDR次数无逐任务事件流"}
    m["34"] = {"name": "PTW预取任务平均访问DDR次数", **logstats.get("ptw_pf_ddr", {}),
               "note": "全期口径; W内N/A: 同上"}
    for key, ids in (("33", ptw_main_ids), ("34", ptw_pf_ids)):
        total_reads = sum(ptw_ddr_reads.get(tid, 0) for tid in ids)
        m[key].update({"log_avg": m[key].get("avg"), "count": len(ids),
                       "reads": total_reads,
                       "avg": total_reads / len(ids) if ids else None,
                       "note": "全期按任务ID关联PTW来源的实际DDR读事件；"
                               "W内未按逐任务完成时刻分组，保留N/A"})
    m["28"] = {"name": "去重Buffer满时bypass任务(正常/预取)",
               **logstats.get("buf_full_bypass", {}),
               "note": "ADM=1下普通请求满Buffer bypass应为0"}
    m["13"] = {"name": "IOMMU输出端口注入任务平均间隔",
               **logstats.get("out_interval", {}),
               "note": "模型输出端口统计点(全期); W内N/A(无逐事件导出), "
                       "与OUT_LAUNCH事件区分"}

    checks16 = check16(rd, man, args.log, R, n_req)
    print("[analyze] 16盘专项验收: mapped_page_disjoint=%s interleave_ok=%s "
          "per_dev_match=%s jain=%s pool=%s/%s" %
          (checks16.get("mapped_page_disjoint"), checks16.get("interleave_ok"),
           checks16.get("per_dev_match"), checks16.get("jain_done"),
           checks16.get("pool_used"), checks16.get("pool_bound_manifest")),
          flush=True)

    emit(args, m, man, (t10, t90, win), logstats,
         (n_req, len(idx_resp_w), n_data_entry), checks16)
    print("[analyze] 输出: %s_perf.{md,csv,json}" % args.out_prefix, flush=True)


def _sub_none(a, b):
    return (a - b) if (a is not None and b is not None) else None


def _iv_of(ts):
    ts = sorted(ts)
    if len(ts) < 2:
        return None
    return (ts[-1] - ts[0]) / (len(ts) - 1)


def parse_log(path):
    """从仿真日志提取模型内全期统计。

    GB级日志(全量sim.log约3.15GB)不可全文读入做多次re.search——
    改为逐行流式状态机: 块头needle命中后激活对应模式组, 全部命中即关闭;
    行内独立模式(单行命中即完成)单独快筛。返回结构与旧全文版完全一致。"""
    out = {
        "ptw_ratio": {"total": None, "main": None, "pf": None},
        "ptw_conc": {"avg": None, "peak": None, "cap": None},
        "ptw_main_ddr": {"avg": None},
        "ptw_pf_ddr": {"avg": None},
        "iommu_iops": None,
        "ptw_iops": None,
        "ptw_inject": {"avg": None, "count": None},
        "ptw_output": {"avg": None, "count": None},
        "ptw_exec": {"all": None, "main": None, "prefetch": None},
        "out_interval": {"all": None},
        "in_interval": {"all": None, "read": None, "write": None},
        "buffer": {"peak": None, "avg": None, "full_bypass": None},
        "buf_full_bypass": {
            "count": None, "normal": None, "prefetch": None,
            "note": "模型仅单一计数(Bypass to PTW), 正常/预取拆分无独立打点"},
        "conflict": {"count": None, "bypass_tasks": None,
                     "pf_setfull_skip": None},
        "dedup_hit": {"queries": None, "hits": None, "misses": None},
        "hold": {},
        "exit_bw": None,
    }
    import re
    try:
        f = open(path, encoding="utf-8", errors="replace")
    except OSError:
        return out

    def put(sec, key, mt, gi, cast):
        try:
            out[sec][key] = cast(mt.group(gi))
        except (ValueError, IndexError):
            pass

    # 触发块: needle -> 未命中字段(落位段,键,模式,组号,类型); 全命中自动关闭
    BLOCKS = {
        "ratio": ("PTW Task Ratio Statistics", [
            ("ptw_ratio", "total", r"PTW completed tasks:\s*(\d+)", 1, int),
            ("ptw_ratio", "main", r"Main tasks:\s*(\d+)", 1, int),
            ("ptw_ratio", "pf", r"Prefetch tasks:\s*(\d+)", 1, int)]),
        "conc": ("PTW Concurrency & Execution Latency", [
            ("ptw_conc", "avg", r"Avg concurrency \(time-weighted\):\s*([\d.]+) tasks", 1, float),
            ("ptw_conc", "peak", r"Max concurrency \(peak\):\s*(\d+) tasks", 1, int),
            ("ptw_conc", "cap", r"Max concurrency \(peak\):\s*\d+ tasks\s*\(limit=(\d+)\)", 1, int)]),
        "ddr_main": ("PTW DDR reads distribution (MAIN tasks", [
            ("ptw_main_ddr", "avg", r"Avg DDR reads:\s*([\d.]+)", 1, float)]),
        "ddr_pf": ("PTW DDR reads distribution (PREFETCH tasks", [
            ("ptw_pf_ddr", "avg", r"Avg DDR reads:\s*([\d.]+)", 1, float)]),
        "inject": ("PTW inject interval", [
            ("ptw_inject", "avg", r"Avg inject interval:\s*([\d.]+)", 1, float),
            ("ptw_inject", "count", r"Inject count:\s*(\d+)", 1, int)]),
        "output": ("PTW output interval", [
            ("ptw_output", "avg", r"Avg output interval:\s*([\d.]+)", 1, float),
            ("ptw_output", "count", r"Output count:\s*(\d+)", 1, int)]),
        "oport": ("Output Port (master_0, response):", [
            ("out_interval", "all", r"Avg output interval:\s*([\d.]+)", 1, float)]),
        "in_all": ("Input Port (slave_0 合计", [
            ("in_interval", "all", r"Avg input interval:\s*([\d.]+)", 1, float)]),
        "in_r": ("Input Read Port", [
            ("in_interval", "read", r"Avg input interval:\s*([\d.]+)", 1, float)]),
        "in_w": ("Input Write Port", [
            ("in_interval", "write", r"Avg input interval:\s*([\d.]+)", 1, float)]),
        "buffer": ("PT Dedup Buffer Statistics", [
            ("buffer", "peak", r"Peak Valid Count:\s*(\d+)", 1, int),
            ("buffer", "avg", r"Avg Occupancy:\s*([\d.]+) entries", 1, float),
            ("buffer", "full_bypass", r"Buffer Full Bypass:\s*(\d+)", 1, int)]),
        "buffull": ("[Buffer Full Bypass]", [
            ("buf_full_bypass", "count", r"Bypass to PTW count:\s*(\d+)", 1, int)]),
        "conflict": ("[Hash Conflict Bypass]", [
            ("conflict", "count", r"Conflict count \(main MISS & set-full\):\s*(\d+)", 1, int),
            ("conflict", "bypass_tasks", r"Conflict bypass to PTW tasks:\s*(\d+)", 1, int),
            ("conflict", "pf_setfull_skip", r"Prefetch set-full skipped:\s*(\d+)", 1, int)]),
        "deduphit": ("[去重Cache命中率", [
            ("dedup_hit", "queries", r"总查询\(lookup\):\s*(\d+)", 1, int),
            ("dedup_hit", "hits", r"命中\(占位CL存在\):\s*(\d+)", 1, int),
            ("dedup_hit", "misses", r"未命中:\s*(\d+)", 1, int)]),
        "steadywin": ("[\u7a33\u6001\u7a97\u53e3", [
            ("NONE", "exit_bw", r"出口 master_0:\s*([\d.]+) GB/s", 1, float)]),
    }
    CP = {}   # bk -> [(sec,key,compiled,gi,cast)]; NEEDLE[bk]=触发子串
    NEEDLE = {}
    for bk, (needle, items) in BLOCKS.items():
        NEEDLE[bk] = needle
        CP[bk] = [(sec, key, re.compile(pat), gi, cast)
                  for sec, key, pat, gi, cast in items]
    active = set()
    LEFT = {}  # bk -> 剩余未命中字段集合
    P_SINGLE = {
        "iops_i": re.compile(r"IOMMU IOPS:\s+([\d.]+) M trans/s"),
        "iops_p": re.compile(r"PTW  IOPS:\s+([\d.]+) M tasks/s"),
        "steady": re.compile(r"输出端口平均间隔\(稳态\):\s*([\d.]+) ns\s+\(samples=(\d+)\)"),
        "e_all": re.compile(r"\[ALL\]\s+count=(\d+)\s+avg=\s*([\d.]+) ns\s+max=\s*([\d.]+)"),
        "e_main": re.compile(r"\[MAIN\]\s+count=(\d+)\s+avg=\s*([\d.]+) ns\s+max=\s*([\d.]+)"),
        "e_pf": re.compile(r"\[PREFETCH\]\s+count=(\d+)\s+avg=\s*([\d.]+) ns\s+max=\s*([\d.]+)"),
        "h_mm": re.compile(r"MAIN head \(dedup MISS\)\s+n=(\d+)\s+avg total=\s*([\d.]+) ns "
                           r"\(W1=\s*([\d.]+) W2=\s*([\d.]+) W3=\s*([\d.]+)\)"),
        "h_pf": re.compile(r"MAIN head \(PF placeholder\)\s+n=(\d+)\s+avg total=\s*([\d.]+) ns "
                           r"\(W1=\s*([\d.]+) W2=\s*([\d.]+) W3=\s*([\d.]+)\)"),
        "h_sp": re.compile(r"SUSPENDED \(deduped\)\s+n=(\d+)\s+avg total=\s*([\d.]+) ns "
                           r"\(W1=\s*([\d.]+) W2=\s*([\d.]+) W3=\s*([\d.]+)\)"),
    }

    with f:
        for line in f:
            # ---- 块激活与激活块的逐行匹配 ----
            for bk in NEEDLE:
                if bk not in active and NEEDLE[bk] in line:
                    active.add(bk)
                    LEFT[bk] = {it[1] for it in CP[bk]}
            for bk in tuple(active):
                for sec, key, pat, gi, cast in CP[bk]:
                    if key not in LEFT[bk]:
                        continue
                    mt = pat.search(line)
                    if mt:
                        if sec == "NONE":
                            try:
                                out[key] = cast(mt.group(gi))
                            except (ValueError, IndexError):
                                pass
                        else:
                            put(sec, key, mt, gi, cast)
                        LEFT[bk].discard(key)
                if not LEFT[bk]:
                    active.discard(bk)
            # ---- 行内独立模式 ----
            if out["iommu_iops"] is None and "IOMMU IOPS:" in line:
                mt = P_SINGLE["iops_i"].search(line)
                if mt:
                    out["iommu_iops"] = float(mt.group(1))
            if out["ptw_iops"] is None and "PTW  IOPS:" in line:
                mt = P_SINGLE["iops_p"].search(line)
                if mt:
                    out["ptw_iops"] = float(mt.group(1))
            if "steady" not in out["out_interval"] and \
                    "输出端口平均间隔(稳态)" in line:
                mt = P_SINGLE["steady"].search(line)
                if mt:
                    out["out_interval"]["steady"] = float(mt.group(1))
                    out["out_interval"]["steady_samples"] = int(mt.group(2))
            ex = out["ptw_exec"]
            if ex["all"] is None and "[ALL]" in line:
                mt = P_SINGLE["e_all"].search(line)
                if mt:
                    ex["all"] = {"count": int(mt.group(1)),
                                 "avg": float(mt.group(2)),
                                 "max": float(mt.group(3))}
            if ex["main"] is None and "[MAIN]" in line:
                mt = P_SINGLE["e_main"].search(line)
                if mt:
                    ex["main"] = {"count": int(mt.group(1)),
                                  "avg": float(mt.group(2)),
                                  "max": float(mt.group(3))}
            if ex["prefetch"] is None and "[PREFETCH]" in line:
                mt = P_SINGLE["e_pf"].search(line)
                if mt:
                    ex["prefetch"] = {"count": int(mt.group(1)),
                                      "avg": float(mt.group(2)),
                                      "max": float(mt.group(3))}
            hd = out["hold"]
            if "main_miss" not in hd and "MAIN head (dedup MISS)" in line:
                mt = P_SINGLE["h_mm"].search(line)
                if mt:
                    hd["main_miss"] = {"n": int(mt.group(1)),
                                       "avg_total": float(mt.group(2)),
                                       "w1": float(mt.group(3)),
                                       "w2": float(mt.group(4)),
                                       "w3": float(mt.group(5))}
            if "main_pf_up" not in hd and "MAIN head (PF placeholder)" in line:
                mt = P_SINGLE["h_pf"].search(line)
                if mt:
                    hd["main_pf_up"] = {"n": int(mt.group(1)),
                                        "avg_total": float(mt.group(2)),
                                        "w1": float(mt.group(3)),
                                        "w2": float(mt.group(4)),
                                        "w3": float(mt.group(5))}
            if "susp" not in hd and "SUSPENDED (deduped)" in line:
                mt = P_SINGLE["h_sp"].search(line)
                if mt:
                    hd["susp"] = {"n": int(mt.group(1)),
                                  "avg_total": float(mt.group(2)),
                                  "w1": float(mt.group(3)),
                                  "w2": float(mt.group(4)),
                                  "w3": float(mt.group(5))}
    return out


def check16(rd, man, logpath, R, n_req):
    """16盘专项验收: 两组独立映射/轮询顺序/16设备对账与Jain/IOVA池用量。"""
    import re as _re
    out = {"note": "组B=组A虚拟副本(request id逐行+8); 两组4KB映射完全独立"}
    # (1) remap: 组间 mapped_page 集合不相交; 每方向前8包组序 A/B 交替(0,1,0,1...)
    mp = {0: set(), 1: set()}
    gseq = {0: [], 1: []}
    p = os.path.join(rd, "real16_trace_remap.csv")
    if os.path.isfile(p):
        with open(p, encoding="utf-8") as f:
            rdr = csv.reader(f)
            next(rdr)
            for row in rdr:
                d = 1 if row[0] == "W" else 0
                g = int(row[1])
                mp[g].add(int(row[9]))
                if len(gseq[d]) < 8:
                    gseq[d].append(g)
        out["mapped_page_disjoint"] = not (mp[0] & mp[1])
        out["mapped_pages"] = {"A": len(mp[0]), "B": len(mp[1])}
        out["interleave_first8"] = {("R" if d == 0 else "W"): gseq[d]
                                    for d in (0, 1)}
        out["interleave_ok"] = all(
            all(g == (i % 2) for i, g in enumerate(gseq[d])) for d in (0, 1))
    # (2) 16设备逐设备完成对账 + Jain fairness(完成数)
    per_dev_done = {}
    for i in range(n_req):
        d = R["did"][i]
        per_dev_done[d] = per_dev_done.get(d, 0) + 1
    ga = man["groupA"]
    exp_rows = {}
    for did in range(8):
        exp_rows[did] = (int(ga["reads"]["per_device_rows"].get(str(did), 0)) +
                         int(ga["writes"]["per_device_rows"].get(str(did), 0)))
        exp_rows[did + 8] = exp_rows[did]
    counts = [per_dev_done.get(d, 0) for d in range(16)]
    s1 = sum(counts)
    s2 = sum(c * c for c in counts)
    out["per_dev_done"] = per_dev_done
    out["per_dev_done_expected"] = {str(k): v for k, v in exp_rows.items()}
    out["per_dev_match"] = all(per_dev_done.get(d, 0) == exp_rows.get(d, 0)
                               for d in range(16))
    out["jain_done"] = (s1 * s1 / (16 * s2)) if s2 else None
    # (3) IOVA 池用量 vs manifest 两组独立上界之和
    bound = man["totals_16dev"]["iova_page_need_upper_bound"]
    pool_used = None
    try:
        with open(logpath, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "[REAL16_RESULT]" in line:
                    mt = _re.search(r"pool_used=(\d+)", line)
                    if mt:
                        pool_used = int(mt.group(1))
    except OSError:
        pass
    out["pool_used"] = pool_used
    out["pool_bound_manifest"] = bound
    out["pool_within_bound"] = (pool_used is not None and pool_used <= bound)
    return out


def emit(args, m, man, win3, logstats, counts, checks16=None):
    """按计划第113节37项固定顺序输出 Markdown/CSV/JSON。"""
    t10, t90, win = win3
    n_req, n_w_resp, n_data_entry = counts

    def fs(v, nd=3):
        if v is None:
            return "N/A"
        if isinstance(v, float):
            return ("%.*f" % (nd, v))
        return str(v)

    def rate_str(block):
        return "%.4f%%(h=%d/q=%d)" % (
            block["w_rate"], block["w_hits"], block["w_queries"]) \
            if block.get("w_rate") is not None else \
            "N/A(h=%d/q=%d)" % (block.get("w_hits", 0), block.get("w_queries", 0))

    def rate_str_all(block):
        return "%.4f%%(h=%d/q=%d)" % (
            block["all_rate"], block["all_hits"], block["all_queries"]) \
            if block.get("all_rate") is not None else \
            "N/A(h=%d/q=%d)" % (block.get("all_hits", 0),
                                block.get("all_queries", 0))

    # ---- 37行主表: 序号 -> (W列, 全期/对照列, 单位, W样本, 说明) ----
    R = {}
    o = m["01"]
    R[1] = ("ALL=%s DATA=%s CTRL=%s R=%s W=%s" %
            (fs(o["all"]), fs(o["data"]), fs(o["ctrl"]), fs(o["read"]),
             fs(o["write"])),
            "全期仿真: %s" % fs(logstats.get("iommu_iops")),
            "Mreq/s", o["n_w"], "W内完成数/Δt; ALL含DATA+CTRL")
    o = m["02"]
    R[2] = ("agg=%s R=%s W=%s" % (fs(o["agg"], 3), fs(o["read"], 3),
                                  fs(o["write"], 3)),
            "全期: in_all=%s" % fs(logstats["in_interval"]["all"]),
            "ns", o["n_agg"], "W内相邻ACCEPT平均; 与12项同源")
    o = m["03"]
    R[3] = ("%s GB/s(util %s%%)" % (fs(o["agg"], 3), fs(o["util"], 2)),
            "出口稳态: %s GB/s" % fs(logstats.get("exit_bw")),
            "GB/s", None, "W内R/W入口实际字节/Δt; 对照128GB/s")
    o = m["04"]
    R[4] = ("%s GB/s(util %s%%)" % (fs(o["read"], 3), fs(o["util"], 2)),
            "全期输入间隔=%s ns" % fs(logstats["in_interval"]["read"]),
            "GB/s", None, "W内读口实际字节/Δt; 对照64GB/s")
    o = m["05"]
    R[5] = ("%s GB/s(util %s%%)" % (fs(o["write"], 3), fs(o["util"], 2)),
            "全期输入间隔=%s ns" % fs(logstats["in_interval"]["write"]),
            "GB/s", None, "W内写口实际字节/Δt; 对照64GB/s")
    o = m["06"]
    R[6] = ("peak=%s avg=%s(util %s%%,full %s%%)" %
            (fs(o["peak"], 0), fs(o["avg"], 3), fs(o["util"], 2),
             fs(o["full_ratio"], 2)),
            None, "个", None, "ACCEPT→RELEASE W内积分(cap=640)")
    o = m["07"]
    R[7] = ("peak=%s avg=%s(util %s%%,full %s%%)" %
            (fs(o["peak"], 0), fs(o["avg"], 3), fs(o["util"], 2),
             fs(o["full_ratio"], 2)),
            None, "个", None, "读口ACCEPT→RELEASE W内积分(cap=320)")
    o = m["08"]
    R[8] = ("peak=%s avg=%s(util %s%%,full %s%%)" %
            (fs(o["peak"], 0), fs(o["avg"], 3), fs(o["util"], 2),
             fs(o["full_ratio"], 2)),
            None, "个", None, "写口ACCEPT→RELEASE W内积分(cap=320)")
    o = m["09"]
    R[9] = ("DATA=%s(n=%d) CTRL=%s(n=%d) ALL=%s" %
            (fs(o["data"]), o["n_data"], fs(o["ctrl"]), o["n_ctrl"],
             fs(o["all"])),
            None, "ns", o["n_data"] + o["n_ctrl"], "RESPONSE−POP, 响应落W")
    o = m["10"]
    R[10] = ("ALL=%s DATA=%s CTRL=%s" %
             (fs(o["all_max"]), fs(o["data_max"]), fs(o["ctrl_max"])),
             None, "ns", None, "最大e2e; 请求身份见JSON all_id/data_id/ctrl_id")
    o = m["11"]
    R[11] = ("avg=%s peak=%s; R=%s W=%s" %
             (fs(o["avg"]), fs(o["peak"], 0), fs(o["read"]["avg"]), fs(o["write"]["avg"])),
             "含CTRL avg=%s; DATA任务avg=%s(非页数)" %
             (fs(o["including_ctrl"]["avg"]), fs(o["data_task_concurrency"]["avg"])),
             "页", None, "DATA按(DID,映射后IOVA页)去重后积分; 历史不同页=%s" % o["n_pages"])
    o = m["12"]
    R[12] = ("agg=%s R=%s W=%s" % (fs(o["agg"], 3), fs(o["read"], 3),
                                   fs(o["write"], 3)),
             None, "ns", o["n_agg"], "同第02项(聚合ACCEPT)")
    o = m["13"]
    R[13] = ("N/A(W内无逐事件)" ,
             "全期=%s 稳态=%s" % (fs(o.get("all")), fs(o.get("steady"))),
             "ns", None, "模型输出端口统计点; 与OUT_LAUNCH区分")
    o = m["14"]
    R[14] = (rate_str(o), rate_str_all(o), "%", o["w_queries"],
             "DC Cache hits/queries")
    o = m["15"]
    R[15] = (rate_str(o), rate_str_all(o), "%", o["w_queries"],
             "逐事件查询计数见h/q；命中0次；不将PC miss等同于新增DDR读取")
    o = m["16"]
    R[16] = (rate_str(o), rate_str_all(o), "%", o["w_queries"],
             "PT Cache全部; DATA口径=%s%%" % fs(o2w(m, "16b"), 2))
    o = m["17"]
    R[17] = (rate_str(o), rate_str_all(o), "%", o["w_queries"],
             "仅is_ctrl=0且bytes=512")
    o = m["18"]
    R[18] = (rate_str(o), rate_str_all(o), "%", o["w_queries"],
             "CTRL聚合(16B写), 不细分SQ/CQE/MSI")
    o = m["19"]
    R[19] = (rate_str(o), rate_str_all(o), "%", o["w_queries"],
             "VS Walker C3(level=3)")
    o = m["20"]
    R[20] = (rate_str(o), rate_str_all(o), "%", o["w_queries"],
             "VS Walker C2(level=2)")
    o = m["21"]
    R[21] = (rate_str(o), rate_str_all(o), "%", o["w_queries"],
             "S2 C3; S2整体=%s%%" % fs(o2w(m, "21b"), 2))
    o = m["22"]
    R[22] = (rate_str(o) if o["w_queries"] else "N/A(q=0)",
             rate_str_all(o) if o["all_queries"] else "N/A(q=0)",
             "%", o["w_queries"], "本场景不合成MSI")
    o = m["23"]
    R[23] = (rate_str(o), rate_str_all(o), "%", o["w_queries"],
             "主请求口径; 预取占位查询挂主任务tid→独立样本0; 日志对照q=%s" %
             fs(logstats["dedup_hit"]["queries"], 0))
    o = m["24"]
    R[24] = ("%s(%s%% of q23)" %
             (fs(o["w_conflicts"], 0), fs(o["ratio_of_q23"], 2)),
             "日志: 冲突=%s bypassPTW=%s PFskip=%s" %
             (fs(logstats["conflict"]["count"], 0),
              fs(logstats["conflict"]["bypass_tasks"], 0),
              fs(logstats["conflict"]["pf_setfull_skip"], 0)),
             "次", o["w_conflicts"], "operations口径=冲突降级行数落W")
    o = m["25"]
    R[25] = ("%s(D=%s,C=%s,%s%% of DATA)" %
             (fs(o["w_bypass"], 0), fs(o["bypass_data"], 0),
              fs(o["bypass_ctrl"], 0), fs(o["ratio_of_data_entry"], 2)),
             None, "个", o["w_bypass"], "hash冲突bypass按任务归属关联")
    o = m["26"]
    if o.get("have"):
        R[26] = ("peak_v=%s avg_v=%s(%s%% of 256) req=%s" %
                 (fs(o["w_peak_v"], 0), fs(o["w_avg_v"], 3),
                  fs(o.get("w_util_v") or ratio(o["w_avg_v"], 256), 2),
                  fs(o["w_avg_r"], 3)),
                 "结束=%s" % fs(o["end_v"], 0),
                 "line", o["rows"], "占用变化流(t,valid,req)W内积分")
    else:
        R[26] = ("N/A", None, "line", None, "无占用采样CSV")
    o = m["27"]
    R[27] = ("peak=%s avg=%s(%s%% of 320)" %
             (fs(o["peak"], 0), fs(o["avg"], 3), fs(o["util"], 2)),
             "全期: peak=%s avg=%s" % (fs(logstats["buffer"]["peak"], 0),
                                       fs(logstats["buffer"]["avg"], 3)),
             "entry", o["init"], "dedup flush持有区间W内积分")
    o = m["28"]
    R[28] = ("0" if o.get("count") == 0 else "N/A(无窗口打点)", "count=%s(normal/pf拆分无独立打点)" %
             fs(o.get("count"), 0), "个", None, "ADM=1下应为0")
    o = m["29"]
    R[29] = ("外部请求 n=%s avg=%s max=%s; pf n=%s" %
             (fs(o["main"]["n"], 0), fs(o["main"]["avg"]),
              fs(o["main"]["max"]), fs(o["inner"]["n"], 0)),
             "W1/W2/W3: MISS=%s PF升级=%s SUSP=%s" %
             (hold_str(logstats, "main_miss"), hold_str(logstats, "main_pf_up"),
              hold_str(logstats, "susp")),
             "ns", o["main"]["n"],
             "外部请求含MISS链头及SUSP；真实PTW预取不申请Buffer，持有延时N/A")
    o = m["30"]
    R[30] = ("main=%s pf=%s total=%s" %
             (fs(o["w_main"], 0), fs(o["w_prefetch"], 0), fs(o["w_total"], 0)),
             "total=%s main=%s(pf %s) normal=%s bypass=%s; main/DATA=%s%%" %
             (fs(o["all_total"], 0), fs(o["all_main"], 0),
              fs(o["all_prefetch"], 0), fs(o["all_main_normal"], 0),
              fs(o["all_main_bypass"], 0), fs(o["main_of_data_entry"], 2)),
             "个", o["w_total"],
             "校验: 总=main+pf, main=normal+bypass(hash冲突降级)")
    o = m["31"]
    R[31] = ("peak=%s avg=%s(util %s%%)" %
             (fs(o["w_peak"], 0), fs(o["w_avg"], 3), fs(o["w_util"], 2)),
             "全期: peak=%s(avg cap=%s)" %
             (fs(o["all_peak"], 0),
              "%s/limit %s" % (fs(o["all_avg"]), fs(o["all_cap"], 0))),
             "个", None, "占用流W内积分; 额度按主任务计数")
    o = m["32"]
    R[32] = ("%s Mtask/s(done=%s)" %
             (fs(o["w_iops"]), fs(o["w_done_tasks"], 0)),
             "全期日志: %s Mtask/s(分母全仿真时长)" % fs(o["all_iops"]),
             "Mtask/s", o["w_done_tasks"], "Σgroup_complete.value/Δt")
    o = m["33"]
    R[33] = ("N/A(W内)", "avg=%s (%s/%s)" %
             (fs(o.get("avg"), 6), o["reads"], o["count"]), "次/任务", None,
             "全期完成主任务累计DDR/主任务数")
    o = m["34"]
    R[34] = ("N/A(W内)", "avg=%s (%s/%s)" %
             (fs(o.get("avg"), 6), o["reads"], o["count"]), "次/任务", None,
             "全期完成预取任务累计DDR/预取任务数")
    o = m["35"]
    R[35] = ("group_avg=%s(n=%s)" % (fs(o["w_group_avg"]), fs(o["w_group_n"], 0)),
             "ALL avg=%s(count=%s) MAIN=%s PF=%s" %
             (fs(logstats["ptw_exec"]["all"]["avg"])
              if logstats["ptw_exec"]["all"] else "N/A",
              fs(logstats["ptw_exec"]["all"]["count"], 0)
              if logstats["ptw_exec"]["all"] else "N/A",
              exec_str(logstats, "main"), exec_str(logstats, "prefetch")),
             "ns", o["w_group_n"], "W内=任务组口径; 单任务延时见全期对照")
    o = m["36"]
    R[36] = ("main=%s pf=%s all=%s" %
             (fs(o["main"]), fs(o["prefetch"]), fs(o["all"])),
             "日志全期: %s(count=%s)" %
             (fs(logstats["ptw_inject"]["avg"]),
              fs(logstats["ptw_inject"]["count"], 0)),
             "ns", None, "PTW准入(main_start/prefetch_spawn)相邻平均")
    o = m["37"]
    R[37] = ("%s" % fs(o["all"]),
             "日志全期: %s(count=%s)" %
             (fs(logstats["ptw_output"]["avg"]),
              fs(logstats["ptw_output"]["count"], 0)),
             "ns", None, "组完成输出事件; 预取无独立输出并入组")

    meta = {
        "run_dir": args.run_dir, "log": args.log,
        "N_manifest": man["totals_16dev"]["rows"],
        "n_requests_measured": n_req,
        "n_w_response": n_w_resp, "n_data_entry": n_data_entry,
        "window": {"t10_ns": t10, "t90_ns": t90, "dur_ns": win,
                   "def": "W=(t10,t90], 按响应完成顺序floor(N*10%)/floor(N*90%)"},
        "metrics": m, "log_extract": logstats, "checks16": checks16,
    }
    with open(args.out_prefix + "_perf.json", "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1, default=float)

    lines_md = [
        "# 16盘实测回放 37项固定指标报告",
        "",
        "- run_dir: `%s`" % args.run_dir,
        "- log: `%s`" % args.log,
        "- N(manifest)=%d(16设备×两组前10000包), 测量期请求=%d, W内响应样本=%d, DATA入口=%d" %
        (man["totals_16dev"]["rows"], n_req, n_w_resp, n_data_entry),
        "- 稳态窗口 W=(%.3f, %.3f] ns, Δt=%.3f ns" % (t10, t90, win),
        "",
        "| 序号 | 指标 | 稳态W | 全期/对照 | 单位 | W样本 | 口径说明 |",
        "|---:|---|---|---|---|---:|---|",
    ]
    names = {k: m[k]["name"] for k in m}
    titles = {
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
    rows_csv = []
    for no in range(1, 38):
        wv, av, unit, nsamp, note = R[no]
        lines_md.append("| %d | %s | %s | %s | %s | %s | %s |" %
                        (no, titles[no], wv, av if av else "—", unit,
                         nsamp if nsamp is not None else "—", note))
        rows_csv.append([no, titles[no], wv, av if av else "N/A", unit,
                         nsamp if nsamp is not None else "N/A", note])
    lines_md += ["", "## 附录", "",
                 "- 全期hash冲突对照: %s" % json.dumps(logstats.get("conflict"),
                                                        ensure_ascii=False),
                 "- 全期去重Cache命中率对照: %s" %
                 json.dumps(logstats.get("dedup_hit"), ensure_ascii=False),
                 "- Buffer持有分解对照: %s" %
                 json.dumps(logstats.get("hold"), ensure_ascii=False),
                 "- PTW执行延时对照: %s" %
                 json.dumps(logstats.get("ptw_exec"), ensure_ascii=False),
                 "- 逐设备/逐包长与残页对账: 见 real16_trace_pairs.csv 与 "
                 "real16_trace_requests.csv"]
    if checks16:
        lines_md += ["", "### 16盘专项验收", "",
                     "- 组间mapped_page不相交(两组独立映射): %s (组A=%s页, 组B=%s页)" %
                     (checks16.get("mapped_page_disjoint"),
                      checks16.get("mapped_pages", {}).get("A"),
                      checks16.get("mapped_pages", {}).get("B")),
                     "- 每方向前8包组序A/B交替(逐包轮询): %s 首8包组序=%s" %
                     (checks16.get("interleave_ok"),
                      json.dumps(checks16.get("interleave_first8"))),
                     "- 16设备逐设备完成数对账: %s (期望=组A×2, 详情见JSON)" %
                     checks16.get("per_dev_match"),
                     "- Jain fairness(16设备完成数): %s" %
                     fs(checks16.get("jain_done"), 6),
                     "- IOVA池用量/两组独立上界: %s / %s → %s" %
                     (checks16.get("pool_used"), checks16.get("pool_bound_manifest"),
                      checks16.get("pool_within_bound"))]
    with open(args.out_prefix + "_perf.md", "w", encoding="utf-8") as f:
        f.write("\n".join(lines_md) + "\n")
    with open(args.out_prefix + "_perf.csv", "w", encoding="utf-8-sig",
              newline="") as f:
        w = csv.writer(f)
        w.writerow(["no", "metric", "w", "all_or_ref", "unit", "n_w", "note"])
        w.writerows(rows_csv)


def o2w(m, key):
    b = m.get(key)
    if not b or b.get("w_rate") is None:
        return None
    return b["w_rate"]


def hold_str(ls, key):
    h = ls.get("hold", {}).get(key)
    return "n=%s/avg=%.1f" % (h["n"], h["avg_total"]) if h else "N/A"


def exec_str(ls, key):
    e = ls.get("ptw_exec", {}).get(key)
    return ("avg=%.1f(n=%d)" % (e["avg"], e["count"])) if e else "N/A"


if __name__ == "__main__":
    main()
