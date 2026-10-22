#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[8盘实测回放] 解析与映射自测(计划第七节-1 的解析部分)。

用 zipfile 手工构造最小 xlsx(inlineStr, 无 sharedStrings), 覆盖:
  A. 正常解析: DW换算(4DW=16B..256DW=1024B)、8台RequesterID白名单映射、
     'HHHHHHHH:HHHHHHHH' 冒号地址、低32位截取、页基址/偏移拆分、
     16B写请求 is_ctrl=1、跨设备/方向共页、同页重复偏移、保留行序;
  B. 明确报错: 未知RequesterID、Length非4DW对齐、TLP与文件方向不符、
     跨4KB页、Address含非十六进制字符、缺字段;
  C. --limit 截断与 manifest 统计闭合。
运行: python3 tmp/real8_selftest.py
"""
import json
import os
import tempfile
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = tempfile.mkdtemp(prefix="real8_selftest_")

CT = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
</Types>"""

RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>"""

WB = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"
          xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
<sheets><sheet name="Sheet1" sheetId="1" r:id="rId1"/></sheets>
</workbook>"""

WB_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
</Relationships>"""


def cell(ref, text):
    return '<c r="%s" t="inlineStr"><is><t>%s</t></is></c>' % (ref, text)


def make_xlsx(path, rows):
    """rows: list of (tlp, length, requester, address); 表头自动加。"""
    parts = ["<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>",
             '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">']
    parts.append('<row r="1">%s%s%s%s</row>' % (
        cell("A1", "TLP Type"), cell("B1", "Length"),
        cell("C1", "RequesterID"), cell("D1", "Address")))
    for i, (tlp, ln, req, addr) in enumerate(rows, start=2):
        parts.append('<row r="%d">%s%s%s%s</row>' % (
            i, cell("A%d" % i, tlp), cell("B%d" % i, str(ln)),
            cell("C%d" % i, req), cell("D%d" % i, addr)))
    parts.append("</worksheet>")
    sheet = "".join(parts)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("[Content_Types].xml", CT)
        zf.writestr("_rels/.rels", RELS)
        zf.writestr("xl/workbook.xml", WB)
        zf.writestr("xl/_rels/workbook.xml.rels", WB_RELS)
        zf.writestr("xl/worksheets/sheet1.xml", sheet)


def run_prepare(extra=()):
    cmd = [sys.executable, os.path.join(ROOT, "rp", "prepare_real8_trace.py"),
           "--src-dir", WORK, "--out-dir", os.path.join(WORK, "out")] + list(extra)
    return subprocess.run(cmd, capture_output=True, text=True)


def load_csv(p):
    with open(p, encoding="utf-8") as f:
        lines = f.read().splitlines()
    hdr = lines[0].split(",")
    return [dict(zip(hdr, ln.split(","))) for ln in lines[1:] if ln]


PASS = 0


def ok(cond, msg):
    global PASS
    if not cond:
        print("[FAIL] %s" % msg)
        sys.exit(1)
    PASS += 1
    print("[ok] %s" % msg)


def main():
    sys.path.insert(0, os.path.join(ROOT, "rp"))
    from analyze_real8_trace import PageIntegrator, ConcIntegrator, Counter, add_walker_lookup

    # 同页8个重叠任务只算1页；跨设备同IOVA仍是不同翻译上下文。
    pages = PageIntegrator()
    for i in range(8):
        pages.add((0, 42), i, i + 10)
    got = pages.solve(0, 17)
    ok(got["avg"] == 1 and got["peak"] == 1, "同页8任务合并成1个在途页")
    pages.add((1, 42), 5, 8)
    got = pages.solve(4, 10)
    ok(got["avg"] == 1.5 and got["peak"] == 2 and got["init"] == 1,
       "跨设备隔离、窗口初值与裁剪面积")
    pages = PageIntegrator()
    pages.add((0, 1), 0, 5); pages.add((0, 2), 5, 10)
    got = pages.solve(0, 10)
    ok(got["avg"] == 1 and got["peak"] == 1, "同刻释放先于准入，峰值不虚增")
    ok(PageIntegrator().solve(0, 10)["avg"] == 0, "无DATA时真实0页")
    conc = ConcIntegrator(1)
    conc.add(0, 20)
    ok(conc.solve(1, 10)["full_ratio"] == 100, "顶满末段不重复积分")
    cats = {}
    def cat(src, op, level=None):
        return cats.setdefault((src, op, level), Counter())
    for level, hit in ((3, 1), (2, 1), (1, 1), (0, 0)):
        add_walker_lookup(cat, "walker_s2", "lookup", level, hit, True)
    c3 = cat("walker_s2", "lookup", 3)
    ok((c3.w_hits, c3.w_queries) == (1, 4), "S2 C3分母含全部miss")
    ok(cat("walker_s2", "lookup").w_hits == 3, "S2整体不重复计算子表")
    add_walker_lookup(cat, "walker_vs_ram", "lookup", 3, 0, True)
    add_walker_lookup(cat, "walker_vs_ram", "lookup", 3, 1, True)
    add_walker_lookup(cat, "walker_vs", "lookup", 3, 1, True)
    ok((cat("walker_vs", "lookup", 3).w_hits,
        cat("walker_vs", "lookup", 3).w_queries) == (1, 2), "VS按子表目标层级，忽略最终命中层级")

    # ---------- A. 正常解析 ----------
    # 地址: 高32位:低32位; 用固定高32位 FFFFFFFF, 低32位覆盖页基址+偏移。
    # 设计: did0/did4 共页 0x11223000(跨设备共页); did1 同页重复偏移;
    #       读方向含 128DW(512B)/256DW(1024B)/16DW(64B)/32DW(128B);
    #       写方向含 4DW(16B->CTRL) 与 128DW。
    rd = [
        ("MRd(某某)", 128, "205:00:00", "FFFFFFFF:11223040"),  # did0 off0x40 512B
        ("MRd(x)", 16, "211:00:00", "00FFFFFF:112230C0"),      # did4 同页 off0xC0 64B
        ("MRd(y)", 256, "206:00:00", "01234567:AABBCC00"),     # did1 off0 1024B
        ("MRd(z)", 32, "212:00:00", "FFFFFFFF:11223F80"),     # did5 同页 off0xF80 128B(页尾对齐不跨页)
        ("MRd(w)", 128, "213:00:00", "DEADBEEF:00000800"),     # did6 低页 off0x800
    ]
    wr = [
        ("MWr(p)", 4, "205:00:00", "FFFFFFFF:11223000"),       # did0 CTRL 同页 off0
        ("MWr(q)", 128, "207:00:00", "00000001:11223040"),     # did3 同页 off0x40 512B
        ("MWr(r)", 4, "214:00:00", "01234567:AABBCFF0"),       # did7 CTRL 页尾16B
        ("MWr(s)", 128, "208:00:00", "FFFFFFFF:9ABCDE00"),    # did4 512B off0xE00
    ]
    make_xlsx(os.path.join(WORK, "4k_randrw_mem_1021_MRd.xlsx"), rd)
    make_xlsx(os.path.join(WORK, "4k_randrw_mem_1021_MWr.xlsx"), wr)

    r = run_prepare()
    ok(r.returncode == 0, "正常输入解析成功 (%s)" % r.stdout.strip().splitlines()[-1] if r.stdout else "")
    rd_rows = load_csv(os.path.join(WORK, "out", "read_trace.csv"))
    wr_rows = load_csv(os.path.join(WORK, "out", "write_trace.csv"))
    ok(len(rd_rows) == 5 and len(wr_rows) == 4, "行数守恒: 读5 写4")

    # DW换算 + is_ctrl 规则
    ok(rd_rows[0]["bytes"] == "512" and rd_rows[0]["dw"] == "128", "128DW->512B")
    ok(rd_rows[1]["bytes"] == "64", "16DW->64B")
    ok(rd_rows[2]["bytes"] == "1024", "256DW->1024B")
    ok(all(x["is_ctrl"] == "0" for x in rd_rows), "读方向全部 DATA")
    ok(wr_rows[0]["is_ctrl"] == "1" and wr_rows[2]["is_ctrl"] == "1" and
       wr_rows[1]["is_ctrl"] == "0" and wr_rows[3]["is_ctrl"] == "0",
       "16B写=CTRL, 其余写=DATA")

    # 冒号地址 / 低32位截取 / 页基址偏移
    ok(rd_rows[0]["addr_low32"] == "11223040" and rd_rows[0]["page_base"] == "11223000"
       and rd_rows[0]["offset"] == "64", "冒号地址低32位+页基址+偏移")
    ok(rd_rows[3]["offset"] == "3968", "页尾对齐偏移(3968+128=4096 不跨页)")
    ok(rd_rows[2]["addr_low32"] == "AABBCC00", "无冒号纯hex低32位(高32位丢弃)")
    ok(wr_rows[2]["offset"] == "4080", "写方向页尾16B偏移")

    # RequesterID 映射
    ok([x["did"] for x in rd_rows] == ["0", "4", "1", "5", "6"], "读方向设备映射顺序")
    ok([x["did"] for x in wr_rows] == ["0", "2", "7", "3"], "写方向设备映射顺序")

    # 行序保留
    ok([x["excel_row"] for x in rd_rows] == ["2", "3", "4", "5", "6"], "读行序=excel行号")

    # manifest 统计闭合
    with open(os.path.join(WORK, "out", "real8_manifest.json"), encoding="utf-8") as f:
        man = json.load(f)
    ok(man["totals"]["rows"] == 9 and man["totals"]["data_rows"] == 7
       and man["totals"]["ctrl_rows"] == 2, "manifest 总计闭合")
    ok(man["totals"]["bytes"] == 512 + 64 + 1024 + 128 + 512 + 16 + 512 + 16 + 512,
       "manifest 字节合计")
    ok(man["totals"]["distinct_source_pages"] == 4, "distinct_source_pages=4(纯页基址全局键: 0x11223000/0xAABBC000/0x0/0x9ABCDE00, 跨设备/方向共页合并)")
    ok(man["requester_map"]["205:00:00"] == 0 and man["requester_map"]["214:00:00"] == 7,
       "manifest 设备映射表")

    # ---------- B. 明确报错 ----------
    def expect_fail(name, rows_r, rows_w, frag):
        make_xlsx(os.path.join(WORK, "4k_randrw_mem_1021_MRd.xlsx"), rows_r)
        make_xlsx(os.path.join(WORK, "4k_randrw_mem_1021_MWr.xlsx"), rows_w)
        rr = run_prepare()
        ok(rr.returncode == 2 and frag in rr.stderr,
           "%s -> exit=2 报错含'%s'" % (name, frag))

    base_rd = [("MRd(a)", 128, "205:00:00", "FFFFFFFF:11223000")]
    base_wr = [("MWr(b)", 128, "206:00:00", "FFFFFFFF:33445000")]

    expect_fail("未知RequesterID", [("MRd(a)", 128, "209:00:00", "FFFFFFFF:11223000")], base_wr, "未知 RequesterID")
    expect_fail("Length非4DW", [("MRd(a)", 3, "205:00:00", "FFFFFFFF:11223000")], base_wr, "4DW 对齐")
    expect_fail("方向不符", [("MWr(a)", 128, "205:00:00", "FFFFFFFF:11223000")], base_wr, "方向")
    expect_fail("跨4KB页", [("MRd(a)", 128, "205:00:00", "FFFFFFFF:11223FF0")], base_wr, "跨 4KB 页")
    expect_fail("地址非法字符", [("MRd(a)", 128, "205:00:00", "FFFFFFFF:11223G00")], base_wr, "非十六进制")
    expect_fail("缺RequesterID", [("", 128, "205:00:00", "FFFFFFFF:11223000")], base_wr, "TLP Type 为空")
    expect_fail("Length超页", [("MRd(a)", 2048, "205:00:00", "FFFFFFFF:11223000")], base_wr, "超过 4KB")

    # ---------- C. --limit 截断 ----------
    make_xlsx(os.path.join(WORK, "4k_randrw_mem_1021_MRd.xlsx"), rd)
    make_xlsx(os.path.join(WORK, "4k_randrw_mem_1021_MWr.xlsx"), wr)
    r = run_prepare(["--limit", "3"])
    ok(r.returncode == 0, "--limit=3 解析成功")
    rd3 = load_csv(os.path.join(WORK, "out", "read_trace.csv"))
    ok(len(rd3) == 3 and rd3[-1]["excel_row"] == "4", "limit 截断保行序")
    with open(os.path.join(WORK, "out", "real8_manifest.json"), encoding="utf-8") as f:
        man3 = json.load(f)
    ok(man3["reads"]["source_rows"] == 5 and man3["limit"] == 3, "manifest 记录源行数与limit")

    # 默认限制必须实际截断至读写各10000包，并保留最后一行身份。
    make_xlsx(os.path.join(WORK, "4k_randrw_mem_1021_MRd.xlsx"), base_rd * 10001)
    make_xlsx(os.path.join(WORK, "4k_randrw_mem_1021_MWr.xlsx"), base_wr * 10001)
    r = run_prepare()
    ok(r.returncode == 0, "默认10000包解析成功")
    for direction in ("read", "write"):
        rows = load_csv(os.path.join(WORK, "out", direction + "_trace.csv"))
        ok(len(rows) == 10000 and rows[-1]["excel_row"] == "10001",
           direction + "默认只取前10000包且保持源行序")
    r = run_prepare(["--limit", "-1"])
    ok(r.returncode == 2, "负数limit拒绝")
    print("\nSELFTEST_PASS all=%d checks" % PASS)


if __name__ == "__main__":
    main()
