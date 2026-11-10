#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""8 盘实测 TLP 数据回放的输入预处理脚本。

把工程根目录的两份实测 xlsx（MRd / MWr）解析成仿真可直接加载的规范化 CSV，
并输出输入清单 JSON。只用 Python 标准库（zipfile + ElementTree.iterparse），
不依赖 openpyxl。

用法:
  python3 rp/prepare_real8_trace.py --out-dir <dir> [--limit N] [--src-dir .]

  # 换用其它批次的实测激励表(默认文件名/表头/RequesterID 白名单之外的数据):
  python3 rp/prepare_real8_trace.py --out-dir <dir> \
      --read-xlsx  <path>/IOMMU_MRd64_Read_Stimulus_10000.xlsx \
      --write-xlsx <path>/IOMMU_MWr64_Write_Stimulus_10000.xlsx \
      --requester-map auto          # 表内恰有 8 个不同 RequesterID -> 升序映射 DID 0..7

输出（全部写入 --out-dir）:
  read_trace.csv      读方向有序记录（保持 xlsx 行序）
  write_trace.csv     写方向有序记录（保持 xlsx 行序）
  real8_manifest.json 源文件 SHA-256、逐设备/方向/包长统计、规范化口径

CSV 列（顺序固定，C++ 侧按列号解析）:
  excel_row           源 xlsx 中的 1-based 行号（表头为第 1 行）
  did                 设备号 0..7（由 RequesterID 映射，见 --requester-map）
  tlp_type            TLP Type 原字符串（MRd(...) / MWr(...)）
  dw                  Length/Length_DW 列原始值（单位 DW）
  bytes               字节长度 = dw * 4（若表内有 Length_Byte 则强制交叉校验）
  is_ctrl             1=16B 写请求（控制包），其余 0
  addr_raw            原始 Address 字符串
  addr_low32          去分隔符后地址的低 32 位（十六进制，无 0x）
  page_base           addr_low32 的 4KB 页基址（十六进制）
  offset              页内偏移（十进制字节）

Address 列映射规则不随数据批次变化: 去分隔符 -> 取低 32 位 -> 拆 4KB 页基址与页内偏移。
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

# 设备映射固定值：RequesterID 字符串 -> DID。
# 默认(legacy)白名单对应 2026-10-21 批次实测表(4k_randrw_mem_1021_MRd/MWr.xlsx)。
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

# Length 列表头别名: 旧批次为 "Length"; 新批次(20261029 激励表)拆成
# "Length_DW"(DW) + "Length_Byte"(字节) + "Cycles (1DW/cyc)" 三列。
LENGTH_DW_ALIASES = ("Length", "Length_DW")
LENGTH_BYTE_ALIASES = ("Length_Byte",)

DEFAULT_READ_NAME = "4k_randrw_mem_1021_MRd.xlsx"
DEFAULT_WRITE_NAME = "4k_randrw_mem_1021_MWr.xlsx"
DEFAULT_SHEET = "xl/worksheets/sheet1.xml"

PAGE_SIZE = 4096
# 16B 写请求按控制包处理；其余（含 16B 读）均为 DATA。
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
    """载入 sharedStrings.xml，返回下标 -> 字符串。"""
    if name not in zf.namelist():
        return []
    strings = []
    with zf.open(name) as f:
        # iterparse 事件驱动，避免整份 XML 驻留内存。
        for _, elem in ET.iterparse(f, events=("end",)):
            if elem.tag == NS + "si":
                # 一个 <si> 可能含多个 <t>（富文本 run），拼接其文本。
                text = "".join(t.text or "" for t in elem.iter(NS + "t"))
                strings.append(text)
                elem.clear()
    return strings


def col_index(ref):
    """把单元格引用（如 'D123'）的列字母转 0-based 列号。"""
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
    """解析 'HHHHHHHH:HHHHHHHH' 形式的地址，返回 (low32, page_base, offset)。

    先去分隔符，再取低 32 位；不去掉高位前先做严格的字符校验，
    避免把非法字符静默当作合法十六进制。
    """
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


def parse_sheet(path, sheet_name, direction, limit, log, requester_map=None):
    """解析一份 xlsx，返回记录列表与统计信息。

    requester_map: RequesterID -> DID；None 表示沿用模块级 legacy 白名单。
    """
    if requester_map is None:
        requester_map = REQUESTER_MAP
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
        sheet = sheet_name or DEFAULT_SHEET
        if sheet not in zf.namelist():
            fail("%s: 找不到工作表 %s" % (path, sheet))
        with zf.open(sheet) as f:
            header_cols = None
            len_col = None
            len_byte_col = None
            for event, elem in ET.iterparse(f, events=("end",)):
                if elem.tag != NS + "row":
                    continue
                row_no = int(elem.get("r"))
                if row_no == 1:
                    header_cols = {}
                    for cell in elem.findall(NS + "c"):
                        text = cell_text(cell, shared, "%s!A1" % path)
                        header_cols[text.strip()] = col_index(cell.get("r"))
                    need = ["TLP Type", "RequesterID", "Address"]
                    for name in need:
                        if name not in header_cols:
                            fail("%s: 表头缺少列 %r (实际: %s)"
                                 % (path, name, list(header_cols)))
                    len_col = next((n for n in LENGTH_DW_ALIASES if n in header_cols), None)
                    if len_col is None:
                        fail("%s: 表头缺少长度列 %s (实际: %s)"
                             % (path, list(LENGTH_DW_ALIASES), list(header_cols)))
                    len_byte_col = next((n for n in LENGTH_BYTE_ALIASES
                                         if n in header_cols), None)
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
                length_raw = values.get(header_cols[len_col], "").strip()
                req = values.get(header_cols["RequesterID"], "").strip()
                addr = values.get(header_cols["Address"], "").strip()

                if not tlp:
                    fail("%s: TLP Type 为空" % where)
                if not length_raw:
                    fail("%s: %s 为空" % (where, len_col))
                if not req:
                    fail("%s: RequesterID 为空" % where)
                if not addr:
                    fail("%s: Address 为空" % where)

                if req not in requester_map:
                    fail("%s: 未知 RequesterID %r（映射表: %s）"
                         % (where, req, ",".join(requester_map)))
                did = requester_map[req]

                if not re.fullmatch(r"\d+", length_raw):
                    fail("%s: %s 非整数: %r" % (where, len_col, length_raw))
                dw = int(length_raw)
                if dw <= 0 or dw % 4 != 0:
                    fail("%s: %s 非法（须为正且 4DW 对齐）: %d" % (where, len_col, dw))
                nbytes = dw * 4
                if nbytes > PAGE_SIZE:
                    fail("%s: %s 超过 4KB 页: %d DW = %d B" % (where, len_col, dw, nbytes))
                if len_byte_col is not None:
                    # 新批次同时给出 DW 与 Byte 两列，强制一致，避免列名误读导致长度错位。
                    byte_raw = values.get(header_cols[len_byte_col], "").strip()
                    if not re.fullmatch(r"\d+", byte_raw) or int(byte_raw) != nbytes:
                        fail("%s: %s=%r 与 %s=%d DW(%d B) 不一致"
                             % (where, len_byte_col, byte_raw, len_col, dw, nbytes))

                exp_prefix = "MRd" if direction == "read" else "MWr"
                if not tlp.startswith(exp_prefix):
                    fail("%s: TLP Type %r 与文件方向 %s 不符（应以 %s 开头）"
                         % (where, tlp, direction, exp_prefix))

                low32, page_base, offset = parse_address(addr, where)
                if offset + nbytes > PAGE_SIZE:
                    stats["cross_page"] += 1
                    fail("%s: 请求跨 4KB 页（offset=%d bytes=%d）"
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
                    # 阶梯验证截断：剩余行不解析，但要记录统计口径。
                    stats["truncated"] = True
                    stats["truncated_rows"] = None
                    break
                if len(records) % 200000 == 0:
                    log("  %s: 已解析 %d 行" % (os.path.basename(path), len(records)))
    return records, stats


def req_sort_key(req):
    """RequesterID 自然序键: 纯数字分段按数值比较，其余退化为字符串比较。

    例: '13:00' < '14:00' < ... < '22:00'；'205:00:00' < ... < '214:00:00'。
    两种批次的分段数不同，但同一批次内分段数一致，可直接排序。
    """
    parts = [p for p in re.split(r"[:.\-\s]+", req.strip()) if p != ""]
    if parts and all(p.isdigit() for p in parts):
        return (0, tuple(int(p) for p in parts), "")
    return (1, (), req)


def scan_requester_ids(paths, sheet_name, expect):
    """只读扫描若干 xlsx 的 RequesterID 列，返回去重集合（超过 expect 立即失败）。"""
    ids = set()
    for path in paths:
        with zipfile.ZipFile(path) as zf:
            shared = load_shared_strings(zf, "xl/sharedStrings.xml")
            sheet = sheet_name or DEFAULT_SHEET
            if sheet not in zf.namelist():
                fail("%s: 找不到工作表 %s" % (path, sheet))
            with zf.open(sheet) as f:
                req_ci = None
                for _, elem in ET.iterparse(f, events=("end",)):
                    if elem.tag != NS + "row":
                        continue
                    row_no = int(elem.get("r"))
                    cells = elem.findall(NS + "c")
                    if row_no == 1:
                        for cell in cells:
                            text = cell_text(cell, shared, "%s!A1" % path).strip()
                            if text == "RequesterID":
                                req_ci = col_index(cell.get("r"))
                        elem.clear()
                        if req_ci is None:
                            fail("%s: 表头缺少列 'RequesterID'" % path)
                        continue
                    for cell in cells:
                        if col_index(cell.get("r")) == req_ci:
                            v = cell_text(cell, shared, "%s row %d" % (path, row_no)).strip()
                            if v:
                                ids.add(v)
                    elem.clear()
                    if len(ids) > expect:
                        fail("%s: RequesterID 超过 %d 个(auto 映射要求恰好 %d 个): %s"
                             % (path, expect, expect, sorted(ids, key=req_sort_key)))
    return ids


def build_auto_requester_map(paths, sheet_name, expect, log):
    """按表内实际 RequesterID 升序映射 DID 0..expect-1（新批次数据用）。"""
    ids = scan_requester_ids(paths, sheet_name, expect)
    if len(ids) != expect:
        fail("auto 映射要求恰好 %d 个不同 RequesterID，实际 %d 个: %s"
             % (expect, len(ids), sorted(ids, key=req_sort_key)))
    ordered = sorted(ids, key=req_sort_key)
    mapping = OrderedDict((r, i) for i, r in enumerate(ordered))
    log("[prepare] RequesterID -> DID (auto, 升序): %s"
        % ", ".join("%s->%d" % (k, v) for k, v in mapping.items()))
    return mapping


def write_csv(path, records):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(CSV_HEADER + "\n")
        for r in records:
            row_no, did, tlp, dw, nbytes, is_ctrl, addr, low32, page_base, offset = r
            f.write("%d,%d,%s,%d,%d,%d,%s,%08X,%08X,%d\n"
                    % (row_no, did, tlp, dw, nbytes, is_ctrl, addr,
                       low32, page_base, offset))


def total_rows_without_limit(path, sheet_name=None):
    """统计数据行数（表头不计），用于截断时报告全量规模。"""
    sheet = sheet_name or DEFAULT_SHEET
    with zipfile.ZipFile(path) as zf:
        with zf.open(sheet) as f:
            n = 0
            for event, elem in ET.iterparse(f, events=("end",)):
                if elem.tag == NS + "row":
                    n += 1
                    elem.clear()
    return max(0, n - 1)


def main():
    ap = argparse.ArgumentParser(description="8 盘实测 TLP 数据回放输入预处理")
    ap.add_argument("--src-dir", default=".", help="包含两个 xlsx 的目录")
    ap.add_argument("--out-dir", required=True, help="输出目录")
    ap.add_argument("--limit", type=int, default=10000,
                    help="每方向按源行序取前N包（默认10000，显式0=全量）")
    ap.add_argument("--read-xlsx", default=None,
                    help="读方向 xlsx 路径（默认 <src-dir>/%s）" % DEFAULT_READ_NAME)
    ap.add_argument("--write-xlsx", default=None,
                    help="写方向 xlsx 路径（默认 <src-dir>/%s）" % DEFAULT_WRITE_NAME)
    ap.add_argument("--sheet", default=None,
                    help="工作表 XML 路径（默认 %s；激励表第2张 Stats 页不参与解析）"
                         % DEFAULT_SHEET)
    ap.add_argument("--requester-map", choices=("legacy", "auto"), default="legacy",
                    help="legacy=固定白名单(205/206/207/208/211/212/213/214->0..7)；"
                         "auto=按表内实际 RequesterID 升序映射 0..num-devices-1")
    ap.add_argument("--num-devices", type=int, default=8,
                    help="auto 映射时的设备数（默认8，须与 TEST_CFG_NUM_DEVICES 一致）")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    if args.limit < 0:
        ap.error("--limit 必须非负")
    if args.num_devices < 1 or args.num_devices > 16:
        ap.error("--num-devices 必须在 1..16")

    def log(msg):
        if not args.quiet:
            print(msg, flush=True)

    read_path = args.read_xlsx or os.path.join(args.src_dir, DEFAULT_READ_NAME)
    write_path = args.write_xlsx or os.path.join(args.src_dir, DEFAULT_WRITE_NAME)
    for p in (read_path, write_path):
        if not os.path.isfile(p):
            fail("找不到输入文件: %s" % p)

    os.makedirs(args.out_dir, exist_ok=True)

    if args.requester_map == "auto":
        req_map = build_auto_requester_map([read_path, write_path], args.sheet,
                                           args.num_devices, log)
    else:
        req_map = REQUESTER_MAP
        if len(req_map) != args.num_devices:
            fail("legacy 白名单为 %d 个设备，与 --num-devices=%d 不符"
                 % (len(req_map), args.num_devices))
        log("[prepare] RequesterID -> DID (legacy 白名单): %s"
            % ", ".join("%s->%d" % (k, v) for k, v in req_map.items()))

    log("[prepare] 解析读方向: %s" % read_path)
    read_recs, read_stats = parse_sheet(read_path, args.sheet, "read", args.limit,
                                        log, req_map)
    log("[prepare] 解析写方向: %s" % write_path)
    write_recs, write_stats = parse_sheet(write_path, args.sheet, "write", args.limit,
                                          log, req_map)

    write_csv(os.path.join(args.out_dir, "read_trace.csv"), read_recs)
    write_csv(os.path.join(args.out_dir, "write_trace.csv"), write_recs)

    if args.limit:
        read_stats["source_rows"] = total_rows_without_limit(read_path, args.sheet)
        write_stats["source_rows"] = total_rows_without_limit(write_path, args.sheet)

    # 全局页需求上界（计划第三节口径）: floor(总字节/4096) + 两方向各自残页上界。
    def residual_bound(recs):
        """残页数上界: 有效页表以原始页基址为全局共享键(跨设备/方向),
        每个被覆盖过的不同 4KB 原始页基址最多产生 1 个残页。"""
        return len({r[8] for r in recs})

    total_bytes = read_stats["bytes"] + write_stats["bytes"]
    bound = total_bytes // PAGE_SIZE + residual_bound(read_recs) + residual_bound(write_recs)

    manifest = {
        "generator": "rp/prepare_real8_trace.py",
        "python": sys.version.split()[0],
        "page_size": PAGE_SIZE,
        "requester_map": req_map,
        "requester_map_mode": args.requester_map,
        "num_devices": args.num_devices,
        "sheet": args.sheet or DEFAULT_SHEET,
        "ctrl_rule": "MWr 且 Length=4DW(16B) -> is_ctrl=1，其余 is_ctrl=0",
        "limit": args.limit,
        "reads": {
            "source": os.path.abspath(read_path),
            "sha256": sha256_of(read_path),
            **{k: (dict(v) if isinstance(v, Counter) else v)
               for k, v in read_stats.items()},
        },
        "writes": {
            "source": os.path.abspath(write_path),
            "sha256": sha256_of(write_path),
            **{k: (dict(v) if isinstance(v, Counter) else v)
               for k, v in write_stats.items()},
        },
        "totals": {
            "rows": len(read_recs) + len(write_recs),
            "bytes": total_bytes,
            "data_rows": read_stats["data_rows"] + write_stats["data_rows"],
            "ctrl_rows": read_stats["ctrl_rows"] + write_stats["ctrl_rows"],
            "distinct_source_pages": len({r[8] for r in read_recs + write_recs}),
            "iova_page_need_upper_bound": bound,
        },
    }
    with open(os.path.join(args.out_dir, "real8_manifest.json"), "w",
              encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2, sort_keys=True)

    log("[prepare] 读方向 : %d 条, %d 字节, DATA=%d CTRL=%d"
        % (len(read_recs), read_stats["bytes"], read_stats["data_rows"],
           read_stats["ctrl_rows"]))
    log("[prepare] 写方向 : %d 条, %d 字节, DATA=%d CTRL=%d"
        % (len(write_recs), write_stats["bytes"], write_stats["data_rows"],
           write_stats["ctrl_rows"]))
    log("[prepare] 页需求上界: %d（候选池容量 131069）" % bound)
    log("[prepare] 输出目录: %s" % os.path.abspath(args.out_dir))


if __name__ == "__main__":
    try:
        main()
    except ParseError as exc:
        sys.stderr.write("[prepare][ERROR] %s\n" % exc)
        sys.exit(2)
