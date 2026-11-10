#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""最小 xlsx 读写工具（只用 Python 标准库，不依赖 openpyxl）。

读：zipfile + ElementTree.iterparse 流式解析，避免整份 XML 驻留内存
（实测 trace 表可达数十万行 / 数十 MB）。
写：生成最小 OOXML 包（[Content_Types].xml / workbook / sheet），
字符串用 inlineStr，数值直接写 <v>，Excel 与 prepare_*.py 均可再解析。
"""

import re
import zipfile
import xml.etree.ElementTree as ET

NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"


def col_index(ref):
    """单元格引用（'D123'）的列字母 -> 0-based 列号。"""
    idx = 0
    for ch in ref:
        if "A" <= ch <= "Z":
            idx = idx * 26 + (ord(ch) - ord("A") + 1)
        else:
            break
    return idx - 1


def load_shared_strings(zf, name="xl/sharedStrings.xml"):
    if name not in zf.namelist():
        return []
    strings = []
    with zf.open(name) as f:
        for _, elem in ET.iterparse(f, events=("end",)):
            if elem.tag == NS + "si":
                strings.append("".join(t.text or "" for t in elem.iter(NS + "t")))
                elem.clear()
    return strings


def _cell_text(cell, shared):
    t = cell.get("t")
    v = cell.find(NS + "v")
    if t == "s":
        return shared[int(v.text)] if v is not None and v.text is not None else ""
    if t == "inlineStr":
        node = cell.find(NS + "is")
        if node is None:
            return ""
        return "".join(x.text or "" for x in node.iter(NS + "t"))
    return v.text if v is not None and v.text is not None else ""


def first_sheet_name(path):
    """返回主文档中第一个工作表的 zip 内路径。"""
    with zipfile.ZipFile(path) as zf:
        names = [n for n in zf.namelist()
                 if n.startswith("xl/worksheets/") and n.endswith(".xml")]
        for cand in ("xl/worksheets/sheet1.xml",):
            if cand in zf.namelist():
                return cand
        return sorted(names)[0]


def iter_rows(path, sheet=None):
    """逐行产出 (row_no, {0-based 列号: 文本值})，含表头行。"""
    with zipfile.ZipFile(path) as zf:
        shared = load_shared_strings(zf)
        name = sheet or first_sheet_name(path)
        if name not in zf.namelist():
            raise IOError("找不到工作表 %s（文件 %s）" % (name, path))
        with zf.open(name) as f:
            for event, elem in ET.iterparse(f, events=("end",)):
                if elem.tag != NS + "row":
                    continue
                row_no = int(elem.get("r"))
                values = {}
                for cell in elem.findall(NS + "c"):
                    values[col_index(cell.get("r"))] = _cell_text(cell, shared)
                elem.clear()
                yield row_no, values


def read_header(path, sheet=None):
    """读表头行，返回 (列名 -> 列号, 表头原始字典)。"""
    for row_no, values in iter_rows(path, sheet):
        if row_no != 1:
            raise RuntimeError("首行不是表头: row=%d" % row_no)
        header = {str(v).strip(): ci for ci, v in values.items() if str(v).strip()}
        return header, values
    raise RuntimeError("空表: %s" % path)


# ---------------------------------------------------------------- 写

def _esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
</Types>"""

ROOT_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>"""

WORKBOOK = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" \
xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
<sheets><sheet name="{name}" sheetId="1" r:id="rId1"/></sheets></workbook>"""

WORKBOOK_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
</Relationships>"""


def col_letter(idx0):
    s = ""
    idx = idx0 + 1
    while idx:
        idx, rem = divmod(idx - 1, 26)
        s = chr(ord("A") + rem) + s
    return s


def write_xlsx(path, header, rows, sheet_name="Sheet1"):
    """写 xlsx：header 为列名列表，rows 为等长元组列表（int/float 原样，其余转字符串）。"""
    def sheet_xml():
        parts = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
                 '<worksheet xmlns="%s">' % NS[1:-1], "<sheetData>"]
        parts.append("<row r=\"1\">")
        for ci, h in enumerate(header):
            ref = "%s1" % col_letter(ci)
            parts.append('<c r="%s" t="inlineStr"><is><t>%s</t></is></c>' % (ref, _esc(h)))
        parts.append("</row>")
        for ri, row in enumerate(rows, start=2):
            parts.append('<row r="%d">' % ri)
            for ci, val in enumerate(row):
                ref = "%s%d" % (col_letter(ci), ri)
                if isinstance(val, bool):
                    val = int(val)
                if isinstance(val, (int, float)):
                    parts.append('<c r="%s"><v>%s</v></c>' % (ref, val))
                else:
                    parts.append('<c r="%s" t="inlineStr"><is><t>%s</t></is></c>'
                                 % (ref, _esc("" if val is None else val)))
            parts.append("</row>")
        parts.append("</sheetData></worksheet>")
        return "".join(parts)

    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("[Content_Types].xml", CONTENT_TYPES)
        zf.writestr("_rels/.rels", ROOT_RELS)
        zf.writestr("xl/workbook.xml", WORKBOOK.format(name=sheet_name))
        zf.writestr("xl/_rels/workbook.xml.rels", WORKBOOK_RELS)
        zf.writestr("xl/worksheets/sheet1.xml", sheet_xml())


def count_data_rows(path, sheet=None):
    """统计数据行数（不含表头）。"""
    n = 0
    for row_no, _ in iter_rows(path, sheet):
        if row_no > 1:
            n += 1
    return n
