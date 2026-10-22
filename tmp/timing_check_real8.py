#!/usr/bin/env python3
# [8盘实测回放] 时序验收 v2: RP背靠背与grant-accept
#   - grant-accept 差: 期望恒0(零门控)
#   - 全局读/写各1条常驻发送进程(计划第540节), CSV行序=task_id序非时间序,
#     按(write, accept_ns)排序后差分 vs 上一包bytes/64 ns(16B CTRL固定1ns)
import csv, sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/real8_ladder_100000/real8_trace_requests.csv"
rows_all = []
with open(path, encoding="utf-8") as f:
    rdr = csv.reader(f)
    hdr = next(rdr)
    c = {n: i for i, n in enumerate(hdr)}
    for row in rdr:
        rows_all.append((int(row[c["write"]]), int(row[c["bytes"]]),
                         float(row[c["grant_ns"]]), float(row[c["accept_ns"]])))

# ---- grant-accept ----
ga = [g - a for _, _, g, a in rows_all]
print("grant-accept: n=%d min=%.3f max=%.3f" % (len(ga), min(ga), max(ga)))
ga_ok = max(abs(x) for x in ga) < 1e-6

# ---- 全局方向流背靠背(ACCEPT=TLP发送完成时刻, 差分=当前包bytes/64ns) ----
exp = {16: 1.0, 64: 1.0, 128: 2.0, 192: 3.0, 256: 4.0, 320: 5.0,
       384: 6.0, 448: 7.0, 512: 8.0, 1024: 16.0}
ok = ga_ok
for w in (0, 1):
    seqs = sorted((a, b, g) for ww, b, g, a in rows_all if ww == w)
    gaps = defaultdict(list)
    for i in range(1, len(seqs)):
        gaps[seqs[i][1]].append(seqs[i][0] - seqs[i - 1][0])  # 挂当前包bytes
    print("--- 全局%s方向流 n=%d" % ("写" if w else "读", len(seqs)))
    for b in sorted(gaps):
        ds = sorted(gaps[b])
        n = len(ds)
        med = ds[n // 2]
        want = exp.get(b, None)
        print("  bytes=%4d n=%7d median=%8.3f min=%8.3f max=%8.3f want=%s"
              % (b, n, med, ds[0], ds[-1], want))
        if want is not None and abs(med - want) > 0.5:
            ok = False
            print("    ^^ FAIL")
print("TIMING_%s" % ("PASS" if ok else "FAIL"))

if "--details" in sys.argv:
    # 只读已有产物，拆分真实节拍与准入等待，不修改模型或仿真结果。
    import json
    from pathlib import Path
    root = Path(path).parent
    perf = json.loads((root / "real8_run_perf.json").read_text())
    t0, t1 = perf["window"]["t10_ns"], perf["window"]["t90_ns"]
    with open(path, newline="", encoding="utf-8") as f:
        requests = list(csv.DictReader(f))
    def overlap(a, b):
        return max(0.0, min(b, t1) - max(a, t0))
    for direction in (0, 1):
        rs = sorted((r for r in requests if int(r["write"]) == direction),
                    key=lambda r: float(r["accept_ns"]))
        blocked, beats = [], []
        for i, r in enumerate(rs):
            ready, accept = float(r["ready_ns"]), float(r["accept_ns"])
            beat = 1.0 if r["type"] == "1" else int(r["bytes"])/64.0
            assert accept >= ready + beat - 1e-6
            if i:
                assert abs(ready - float(rs[i-1]["accept_ns"])) < 1e-6
            beats.append((ready, ready+beat))
            if accept > ready + beat + 1e-6:
                blocked.append((ready+beat, accept))
        w_rs = [r for r in rs if t0 < float(r["accept_ns"]) <= t1]
        total_bytes = sum(int(r["bytes"]) for r in rs)
        w_bytes = sum(int(r["bytes"]) for r in w_rs)
        duration = float(rs[-1]["accept_ns"]) - float(rs[0]["ready_ns"])
        report = {"direction": "W" if direction else "R", "count": len(rs),
                  "bytes": total_bytes, "avg_bytes": total_bytes/len(rs),
                  "send_duration_ns": duration, "send_bw_GBs": total_bytes/duration,
                  "block_count": len(blocked), "block_ns": sum(b-a for a,b in blocked),
                  "block_max_ns": max((b-a for a,b in blocked), default=0),
                  "w_count": len(w_rs), "w_bytes": w_bytes,
                  "w_bw_GBs": w_bytes/(t1-t0),
                  "w_block_count": sum(overlap(a,b)>0 for a,b in blocked),
                  "w_block_ns": sum(overlap(a,b) for a,b in blocked),
                  "w_beat_ns": sum(overlap(a,b) for a,b in beats)}
        assert abs(report["w_block_ns"] + report["w_beat_ns"] - (t1-t0)) < 1e-6
        print("PORT_DETAIL", json.dumps(report))
    main_ids, pf_ids, groups, holds = set(), set(), [], []
    with open(root / "real8_operations_raw.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            tid, source, operation = int(r["task_id"]), r["source"], r["operation"]
            if source == "ptw":
                if operation == "main_start":
                    main_ids.add(tid)
                elif operation == "prefetch_spawn":
                    pf_ids.add(tid)
                elif operation == "group_complete":
                    groups.append((float(r["start_ns"]), float(r["end_ns"]), int(r["value"])))
            elif source == "dedup" and operation == "flush":
                holds.append((tid, float(r["start_ns"]), float(r["end_ns"])))
    def stats(values):
        return {"n": len(values), "avg_ns": sum(values)/len(values) if values else None,
                "max_ns": max(values) if values else None}
    for scope in ("all", "window"):
        selected = [g for g in groups if scope == "all" or t0 < g[1] <= t1]
        sizes = defaultdict(int)
        for _, _, size in selected:
            sizes[size] += 1
        print("GROUP_DETAIL", scope, json.dumps({"groups": stats([e-s for s,e,_ in selected]),
              "size_histogram": dict(sizes),
              "exact_four_tasks": stats([e-s for s,e,n in selected if n == 4])}))
        selected_holds = [h for h in holds if scope == "all" or t0 < h[2] <= t1]
        for kind in ("main", "prefetch", "other_external"):
            values = [e-s for tid,s,e in selected_holds
                      if ("main" if tid in main_ids else "prefetch" if tid in pf_ids else "other_external") == kind]
            print("BUFFER_DETAIL", scope, kind, json.dumps(stats(values)))
