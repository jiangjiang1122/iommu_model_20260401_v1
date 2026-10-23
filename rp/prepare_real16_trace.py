#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""16 盘实测 TLP 数据回放的输入预处理脚本 (real16_trace_replay)。

在 8 盘脚本(rp/prepare_real8_trace.py)口径基础上扩展:
  - 组A: 工程根目录实测 xlsx (MRd / MWr), RequesterID 205/206/207/208/211/212/213/214
    -> DID 0..7;
  - 组B: 组A 的"内存虚拟副本"(不落地 _1.xlsx 文件), 等价于 request id 逐行 +8
    (205->213 ... 214->222), 映射为 DID 8..15; len/addr/行序/包长与组A逐行相同;
  - 两组各取每方向前 --limit 行(默认 10000), 保持源行序;
  - 两组 4KB 页映射在仿真中完全独立(地址相同也不复用), 本脚本输出
    两组原始源页(page_no)交集统计以量化该口径的必要性。

只用 Python 标准库(zipfile + ElementTree.iterparse), 不依赖 openpyxl。

用法:
  python3 rp/prepare_real16_trace.py --out-dir <dir> [--limit N] [--src-dir .]

输出(全部写入 --out-dir):
  read_trace.csv      组A 读方向有序记录(保持 xlsx 行序, did 0..7)
  write_trace.csv     组A 写方向有序记录(did 0..7)
  read_trace_1.csv    组B 读方向虚拟副本(did 8..15)
  write_trace_1.csv   组B 写方向虚拟副本(did 8..15)
  real16_manifest.json 源文件 SHA-256、16设备/方向/包长统计、两组独立性口径

CSV 列(顺序固定, C++ 侧按列号解析, 与 real8 完全一致):
  excel_row,did,tlp_type,dw,bytes,is_ctrl,addr_raw,addr_low32,page_base,offset
"""

import argparse
import hashlib
import json
import os
import re
import sys
import zipfile
import xml.etree.ElementTree as ET
from collections import Counter, OrderedDict

NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"

# 设备映射固定值: RequesterID 字符串 -> DID(组A)。组B = 同一白名单顺序 did+8。
REQUESTER_MAP = OrderedDict([
    ("205:00:00", 0),
    ("206:00:00", 1),
    ("207:00:00", 2),
    ("208:00:00", 3),
    ("211:00:00", 4),
    ("212:00:00", 5),
    ("213:00:00", 6),
    ("214:00:00", 7),
])
# 组B request id 字面值(等价于组A逐行+8), 仅记录于 manifest, 不落任何文件。
GROUP_B_REQUESTERS = OrderedDict([
    ("205:00:00", 8),
    ("206:00:00", 9),
    ("207:00:00", 10),
    ("208:00:00", 11),
    ("211:00:00", 12),
    ("212:00:00", 13),
    ("213:00:00", 14),
    ("214:00:00", 15),
])
GROUP_B_DID_OFFSET = 8

PAGE_SIZE = 4096
# 16B 写请求按控制包处理; 其余(含 16B 读)均为 DATA。
CTRL_DW = 4

CSV_HEADER = ("excel_row,did,tlp_type,dw,bytes,is_ctrl,addr_raw,"
              "addr_low32,page_base,offset")


class ParseError(Exception):
    pass


def fail(msg):
    raise ParseError(msg)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_shared_strings(zf, name):
    """载入 sharedStrings.xml, 返回下标 -> 字符串。"""
    if name not in zf.namelist():
        return []
    strings = []
    with zf.open(name) as f:
        for _, elem in ET.iterparse(f, events=("end",)):
            if elem.tag == NS + "si":
                text = "".join(t.text or "" for t in elem.iter(NS + "t"))
                strings.append(text)
                elem.clear()
    return strings


def col_index(ref):
    """把单元格引用(如 'D123')的列字母转 0-based 列号。"""
    idx = 0
    for ch in ref:
        if "A" <= ch <= "Z":
            idx = idx * 26 + (ord(ch) - ord("A") + 1)
        else:
            break
    return idx - 1


def cell_text(cell, shared, where):
    """按单元格类型取文本值。"""
    t = cell.get("t")
    v = cell.find(NS + "v")
    if t == "s":
        if v is None or v.text is None:
            fail("%s: sharedString 单元格缺少 <v>" % where)
        try:
            return shared[int(v.text)]
        except (ValueError, IndexError):
            fail("%s: sharedString 下标非法: %r" % (where, v.text))
    if t == "inlineStr":
        node = cell.find(NS + "is")
        if node is None:
            fail("%s: inlineStr 单元格缺少 <is>" % where)
        return "".join(x.text or "" for x in node.iter(NS + "t"))
    if v is None or v.text is None:
        return ""
    return v.text


def parse_address(raw, where):
    """解析 'HHHHHHHH:HHHHHHHH' 形式的地址, 返回 (low32, page_base, offset)。"""
    digits = raw.replace(":", "").replace(" ", "")
    if not digits:
        fail("%s: Address 为空: %r" % (where, raw))
    if not re.fullmatch(r"[0-9A-Fa-f]+", digits):
        fail("%s: Address 含非十六进制字符: %r" % (where, raw))
    value = int(digits, 16)
    low32 = value & 0xFFFFFFFF
    page_base = low32 & ~(PAGE_SIZE - 1)
    offset = low32 - page_base
    return low32, page_base, offset


def parse_sheet(path, sheet_name, direction, limit, log):
    """解析一份 xlsx, 返回记录列表与统计信息(组A口径, did 0..7)。"""
    stats = {
        "direction": direction,
        "file": os.path.basename(path),
        "rows": 0,
        "dropped": 0,
        "bytes": 0,
        "tlp_types": Counter(),
        "requester_ids": Counter(),
        "length_dw": Counter(),
        "per_device_rows": Counter(),
        "per_device_bytes": Counter(),
        "ctrl_rows": 0,
        "data_rows": 0,
        "cross_page": 0,
        "offset_max_by_len": {},
    }
    records = []
    with zipfile.ZipFile(path) as zf:
        shared = load_shared_strings(zf, "xl/sharedStrings.xml")
        sheet = sheet_name or "xl/worksheets/sheet1.xml"
        if sheet not in zf.namelist():
            fail("%s: 找不到工作表 %s" % (path, sheet))
        with zf.open(sheet) as f:
            header_cols = None
            for event, elem in ET.iterparse(f, events=("end",)):
                if elem.tag != NS + "row":
                    continue
                row_no = int(elem.get("r"))
                if row_no == 1:
                    header_cols = {}
                    for cell in elem.findall(NS + "c"):
                        text = cell_text(cell, shared, "%s!A1" % path)
                        header_cols[text.strip()] = col_index(cell.get("r"))
                    need = ["TLP Type", "Length", "RequesterID", "Address"]
                    for name in need:
                        if name not in header_cols:
                            fail("%s: 表头缺少列 %r (实际: %s)"
                                 % (path, name, list(header_cols)))
                    elem.clear()
                    continue
                if header_cols is None:
                    fail("%s: 第 %d 行之前没有表头" % (path, row_no))
                where = "%s row %d" % (os.path.basename(path), row_no)
                values = {}
                for cell in elem.findall(NS + "c"):
                    ci = col_index(cell.get("r"))
                    values[ci] = cell_text(cell, shared, where)
                elem.clear()

                tlp = values.get(header_cols["TLP Type"], "").strip()
                length_raw = values.get(header_cols["Length"], "").strip()
                req = values.get(header_cols["RequesterID"], "").strip()
                addr = values.get(header_cols["Address"], "").strip()

                if not tlp:
                    fail("%s: TLP Type 为空" % where)
                if not length_raw:
                    fail("%s: Length 为空" % where)
                if not req:
                    fail("%s: RequesterID 为空" % where)
                if not addr:
                    fail("%s: Address 为空" % where)

                if req not in REQUESTER_MAP:
                    fail("%s: 未知 RequesterID %r(白名单: %s)"
                         % (where, req, ",".join(REQUESTER_MAP)))
                did = REQUESTER_MAP[req]

                if not re.fullmatch(r"\d+", length_raw):
                    fail("%s: Length 非整数: %r" % (where, length_raw))
                dw = int(length_raw)
                if dw <= 0 or dw % 4 != 0:
                    fail("%s: Length 非法(须为正且 4DW 对齐): %d" % (where, dw))
                nbytes = dw * 4
                if nbytes > PAGE_SIZE:
                    fail("%s: Length 超过 4KB 页: %d DW = %d B" % (where, dw, nbytes))

                exp_prefix = "MRd" if direction == "read" else "MWr"
                if not tlp.startswith(exp_prefix):
                    fail("%s: TLP Type %r 与文件方向 %s 不符(应以 %s 开头)"
                         % (where, tlp, direction, exp_prefix))

                low32, page_base, offset = parse_address(addr, where)
                if offset + nbytes > PAGE_SIZE:
                    stats["cross_page"] += 1
                    fail("%s: 请求跨 4KB 页(offset=%d bytes=%d)"
                         % (where, offset, nbytes))

                is_ctrl = 1 if (direction == "write" and nbytes == CTRL_DW * 4) else 0
                rec = (row_no, did, tlp, dw, nbytes, is_ctrl, addr,
                       low32, page_base, offset)
                records.append(rec)

                stats["rows"] += 1
                stats["bytes"] += nbytes
                stats["tlp_types"][tlp] += 1
                stats["requester_ids"][req] += 1
                stats["length_dw"][dw] += 1
                stats["per_device_rows"][did] += 1
                stats["per_device_bytes"][did] += nbytes
                if is_ctrl:
                    stats["ctrl_rows"] += 1
                else:
                    stats["data_rows"] += 1
                prev = stats["offset_max_by_len"].get(nbytes, 0)
                if offset > prev:
                    stats["offset_max_by_len"][nbytes] = offset
                if limit and len(records) >= limit:
                    # 阶梯验证截断: 剩余行不解析, 但要记录统计口径。
                    stats["truncated"] = True
                    stats["truncated_rows"] = None
                    break
                if len(records) % 200000 == 0:
                    log("  %s: 已解析 %d 行" % (os.path.basename(path), len(records)))
    return records, stats


def write_csv(path, records, did_offset=0):
    """写一份规范化 CSV; did_offset 非 0 时生成组B虚拟副本(did+8)。"""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(CSV_HEADER + "\n")
        for r in records:
            row_no, did, tlp, dw, nbytes, is_ctrl, addr, low32, page_base, offset = r
            f.write("%d,%d,%s,%d,%d,%d,%s,%08X,%08X,%d\n"
                    % (row_no, did + did_offset, tlp, dw, nbytes, is_ctrl, addr,
                       low32, page_base, offset))


def total_rows_without_limit(path):
    """统计数据行数(表头不计), 用于截断时报告全量规模。"""
    with zipfile.ZipFile(path) as zf:
        with zf.open("xl/worksheets/sheet1.xml") as f:
            n = 0
            for event, elem in ET.iterparse(f, events=("end",)):
                if elem.tag == NS + "row":
                    n += 1
                    elem.clear()
    return max(0, n - 1)


def group_page_bound(recs_by_dir):
    """单组页需求上界: floor(组内总字节/4096) + 两方向各自残页上界。
    残页上界 = 该方向出现过的不同 4KB 原始页基址数(每组独立映射, 不跨组共享)。
    recs_by_dir: {方向: 记录列表}, 记录元组下标 4=bytes, 8=page_base。"""
    total_bytes = sum(r[4] for recs in recs_by_dir.values() for r in recs)
    residual = sum(len({r[8] for r in recs}) for recs in recs_by_dir.values())
    return total_bytes // PAGE_SIZE + residual


def main():
    ap = argparse.ArgumentParser(description="16 盘实测 TLP 数据回放输入预处理")
    ap.add_argument("--src-dir", default=".", help="包含两份原 xlsx 的目录")
    ap.add_argument("--out-dir", required=True, help="输出目录")
    ap.add_argument("--limit", type=int, default=10000,
                    help="每方向每文件按源行序取前N包(默认10000, 显式0=全量)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    if args.limit < 0:
        ap.error("--limit 必须非负")

    def log(msg):
        if not args.quiet:
            print(msg, flush=True)

    read_path = os.path.join(args.src_dir, "4k_randrw_mem_1021_MRd.xlsx")
    write_path = os.path.join(args.src_dir, "4k_randrw_mem_1021_MWr.xlsx")
    for p in (read_path, write_path):
        if not os.path.isfile(p):
            fail("找不到输入文件: %s" % p)

    os.makedirs(args.out_dir, exist_ok=True)

    log("[prepare] 解析读方向(组A): %s" % read_path)
    read_recs, read_stats = parse_sheet(read_path, None, "read", args.limit, log)
    log("[prepare] 解析写方向(组A): %s" % write_path)
    write_recs, write_stats = parse_sheet(write_path, None, "write", args.limit, log)

    # 组B = 组A 同一解析结果的虚拟副本: 仅 did+8, 其余逐行相同(不二次解析)。
    write_csv(os.path.join(args.out_dir, "read_trace.csv"), read_recs, 0)
    write_csv(os.path.join(args.out_dir, "write_trace.csv"), write_recs, 0)
    write_csv(os.path.join(args.out_dir, "read_trace_1.csv"), read_recs,
              GROUP_B_DID_OFFSET)
    write_csv(os.path.join(args.out_dir, "write_trace_1.csv"), write_recs,
              GROUP_B_DID_OFFSET)

    if args.limit:
        read_stats["source_rows"] = total_rows_without_limit(read_path)
        write_stats["source_rows"] = total_rows_without_limit(write_path)

    def stats_public(d):
        return {k: (dict(v) if isinstance(v, Counter) else v)
                for k, v in d.items()}

    # 两组独立映射: 页需求上界 = 组A上界 + 组B上界(同一虚拟副本, 数值相同)。
    bound_a = group_page_bound({"read": read_recs, "write": write_recs})
    bound_b = bound_a  # 组B与组A逐行相同, 上界一致
    total_bytes_a = read_stats["bytes"] + write_stats["bytes"]

    # 两组原始源页重叠统计: 组B为组A副本, 交集=并集, 正是"必须独立映射"的原因。
    pages_r = {r[8] >> 12 for r in read_recs}
    pages_w = {r[8] >> 12 for r in write_recs}
    overlap_pages = len(pages_r | pages_w)

    manifest = {
        "generator": "rp/prepare_real16_trace.py",
        "python": sys.version.split()[0],
        "page_size": PAGE_SIZE,
        "requester_map_group_a": REQUESTER_MAP,
        "requester_map_group_b": GROUP_B_REQUESTERS,
        "group_b_generation": ("内存虚拟副本: 不落地 _1.xlsx; 组B记录=组A解析结果"
                               "逐行复制, 仅 did+8(等价 request id 逐行+8: "
                               "205->213,206->214,207->215,208->216,"
                               "211->219,212->220,213->221,214->222); "
                               "len/addr/行序与组A完全相同"),
        "interleave": "C++ 侧每方向逐包交替合并 A[0],B[0],A[1],B[1]...(等长)",
        "page_mapping_independence": ("两组 4KB 页映射完全独立: 同一原始地址在两组"
                                      "各自建代, 映射到不同 IOVA 4KB 页, 不复用"),
        "ctrl_rule": "MWr 且 Length=4DW(16B) -> is_ctrl=1, 其余 is_ctrl=0",
        "limit": args.limit,
        "sources": {
            "read": {
                "path": os.path.abspath(read_path),
                "sha256": sha256_of(read_path),
                "used_by": ["groupA", "groupB(virtual)"],
            },
            "write": {
                "path": os.path.abspath(write_path),
                "sha256": sha256_of(write_path),
                "used_by": ["groupA", "groupB(virtual)"],
            },
        },
        "groupA": {
            "reads": stats_public(read_stats),
            "writes": stats_public(write_stats),
            "totals": {
                "rows": len(read_recs) + len(write_recs),
                "bytes": total_bytes_a,
                "data_rows": read_stats["data_rows"] + write_stats["data_rows"],
                "ctrl_rows": read_stats["ctrl_rows"] + write_stats["ctrl_rows"],
                "distinct_source_pages": overlap_pages,
                "iova_page_need_upper_bound": bound_a,
            },
        },
        "groupB": {
            "virtual_copy_of": "groupA",
            "did_offset": GROUP_B_DID_OFFSET,
            "totals": {
                "rows": len(read_recs) + len(write_recs),
                "bytes": total_bytes_a,
                "iova_page_need_upper_bound": bound_b,
            },
        },
        "totals_16dev": {
            "rows": 2 * (len(read_recs) + len(write_recs)),
            "bytes": 2 * total_bytes_a,
            "data_rows": 2 * (read_stats["data_rows"] + write_stats["data_rows"]),
            "ctrl_rows": 2 * (read_stats["ctrl_rows"] + write_stats["ctrl_rows"]),
            "source_page_overlap": overlap_pages,
            "iova_page_need_upper_bound": bound_a + bound_b,
        },
    }
    with open(os.path.join(args.out_dir, "real16_manifest.json"), "w",
              encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2, sort_keys=True)

    for name, st in (("组A读", read_stats), ("组A写", write_stats)):
        log("[prepare] %s: %d 条, %d 字节, DATA=%d CTRL=%d"
            % (name, st["rows"], st["bytes"], st["data_rows"], st["ctrl_rows"]))
    log("[prepare] 组B虚拟副本: did+8, 逐行同组A (read %d + write %d 条)"
        % (len(read_recs), len(write_recs)))
    log("[prepare] 16盘合计: %d 条, %d 字节"
        % (manifest["totals_16dev"]["rows"], manifest["totals_16dev"]["bytes"]))
    log("[prepare] 两组源页重叠: %d 页(组B为副本, 交集=并集; 仿真中两组独立映射)"
        % overlap_pages)
    log("[prepare] 页需求上界: 组A=%d + 组B=%d = %d(候选池容量 131069)"
        % (bound_a, bound_b, bound_a + bound_b))
    log("[prepare] 输出目录: %s" % os.path.abspath(args.out_dir))


if __name__ == "__main__":
    try:
        main()
    except ParseError as exc:
        sys.stderr.write("[prepare][ERROR] %s\n" % exc)
        sys.exit(2)
