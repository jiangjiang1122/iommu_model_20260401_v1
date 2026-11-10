#!/usr/bin/env python3
"""从死锁复现日志中提取阻塞链证据；只读日志，不修改模型、不重跑仿真。"""
import argparse
import gzip
from collections import Counter, deque
from pathlib import Path
import re

TAGS = ('[AGG_FLUSH]', '[MONITOR_PT_UPD]', '[PT_HASH_BP]', '[PTW_PREFETCH_MONITOR]',
        '[DEDUP_CACHE_BYPASS]', '[DEDUP_FLUSH_CB]', '[DEDUP_UPDATE]', '[PT_CACHE]',
        '[MD_WATCHDOG]', '[MDMC_STATE]', '[MDMC_RWQ_STATE]', '[MDMC_QUEUE]',
        '[REORDER]', '[IOMMU_TOP]', '[PTW_STAT]', '[PTW_GRP_STAT]', '[DDR_ARBITER]')
TIME_RE = re.compile(r'\[t=(\d+(?:\.\d+)?) ns\]|t=(\d+(?:\.\d+)?) ns\]')
TAG_RE = re.compile(r'^\[([A-Z0-9_]+)\]')
RAM_RE = re.compile(r'RAM(\d+) FIFO full')


def opener(path):
    return gzip.open(path, 'rt', encoding='utf-8', errors='replace') if str(path).endswith('.gz') \
        else open(path, encoding='utf-8', errors='replace')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--log', required=True, help='sim.log.gz 或 sim_tail.txt')
    ap.add_argument('--out', required=True)
    ap.add_argument('--keep', type=int, default=30, help='每类保留的末尾行数')
    args = ap.parse_args()
    log = Path(args.log)

    tag_count = Counter()
    tail_by_tag = {t: deque(maxlen=args.keep) for t in TAGS}
    watchdog_blocks = deque(maxlen=6)
    current_block = []
    hash_bp_by_ram = Counter()
    last_time = None
    first_time = None
    fatal_ns = None
    total_lines = 0
    fatal = deque(maxlen=12)
    monitor_pt_upd_count = 0
    agg_flush_count = 0
    last_agg_flush_time = None
    last_response_time = None
    last_ptw_done_time = None
    # 阻塞点定位：PT更新写入按组归集、agg_flush完成集合、预取批次应有的iova。
    pt_upd_by_group = {}
    pt_upd_order = []
    agg_flush_groups = set()
    batch_by_group = {}
    cur_batch_group = None
    last_hash_bp = None
    md_state = None
    rwq_state = None
    wpd_state = None

    with opener(log) as f:
        for line in f:
            total_lines += 1
            fatal.append(line.rstrip('\n'))
            m = TIME_RE.search(line)
            if m:
                t = float(m.group(1) or m.group(2))
                if first_time is None:
                    first_time = t
                last_time = t
            tm = TAG_RE.match(line)
            if tm:
                tag = '[%s]' % tm.group(1)
                tag_count[tag] += 1
                if tag in tail_by_tag:
                    tail_by_tag[tag].append(line.rstrip('\n'))
                if tag == '[AGG_FLUSH]':
                    agg_flush_count += 1
                    gm = re.search(r'group_id=(\d+)', line)
                    if gm:
                        agg_flush_groups.add(int(gm.group(1)))
                    am = re.search(r'flushed at (\d+(?:\.\d+)?) ns', line)
                    last_agg_flush_time = float(am.group(1)) if am else last_time
                if tag == '[MONITOR_PT_UPD]':
                    monitor_pt_upd_count += 1
                    pm = re.search(r'group=(\d+), iova=(0x[0-9a-fA-F]+)', line)
                    if pm:
                        gid = int(pm.group(1))
                        if gid not in pt_upd_by_group:
                            pt_upd_by_group[gid] = []
                            pt_upd_order.append(gid)
                        pt_upd_by_group[gid].append(pm.group(2))
                if tag == '[PT_HASH_BP]':
                    rm = RAM_RE.search(line)
                    if rm:
                        hash_bp_by_ram['RAM' + rm.group(1)] += 1
                        last_hash_bp = (line.rstrip('\n'), 'RAM' + rm.group(1))
                if tag == '[PTW_PREFETCH_MONITOR]':
                    cm = re.search(r'Processing completed group (\d+)', line)
                    if cm:
                        cur_batch_group = int(cm.group(1))
                        batch_by_group.setdefault(cur_batch_group, [])
                    bm = re.search(r'Batch update\[\d+\]: iova=(0x[0-9a-fA-F]+)', line)
                    if bm and cur_batch_group is not None:
                        batch_by_group[cur_batch_group].append(bm.group(1))
            if 'send_response_to_initiator' in line:
                mm = re.search(r'exit=(\d+(?:\.\d+)?) ns', line)
                if mm:
                    last_response_time = float(mm.group(1))
            if 'WALK COMPLETE' in line or ('Prefetch task' in line and 'completed' in line):
                # 这类行自身不带时间戳，用最近一次出现的时间戳近似定位。
                last_ptw_done_time = last_time
            if line.startswith('In process:'):
                fm = re.search(r'@ (\d+(?:\.\d+)?) ns', line)
                if fm:
                    fatal_ns = float(fm.group(1))
            # 看门狗快照：以 [MD_WATCHDOG] DID= 开头，[MDMC_RWQ_STATE] 收尾；被打断则丢弃。
            stripped = line.rstrip('\n')
            if line.startswith('[MD_WATCHDOG] DID='):
                if not current_block:
                    current_block = []
                current_block.append(stripped)
            elif current_block and line.startswith(('[MD_WATCHDOG] input=', '[MDMC_STATE]',
                                                    '[MDMC_QUEUE]', '[MDMC_RWQ_STATE]')):
                current_block.append(stripped)
                if line.startswith('[MD_WATCHDOG] input='):
                    wpd_state = stripped
                elif line.startswith('[MDMC_STATE]'):
                    md_state = stripped
                elif line.startswith('[MDMC_RWQ_STATE]'):
                    rwq_state = stripped
                if line.startswith('[MDMC_RWQ_STATE]'):
                    watchdog_blocks.append(current_block)
                    current_block = []
            elif current_block:
                current_block = []

    out = Path(args.out)
    lines = ['# 死锁复现证据提取', '',
             '- 日志：`%s`' % log,
             '- 总行数：%d；日志内首个时间戳：%s ns；最后时间戳：%s ns' % (total_lines, first_time, last_time),
             '- 本脚本只读日志，不修改模型、不重跑仿真。', '',
             '## 冻结时刻对照（ns）',
             '| 事件 | 最后发生时刻 |', '|---|---:|',
             '| 最后一次响应下发 send_response | %s |' % last_response_time,
             '| 最后一次 AGG_FLUSH 完成 | %s |' % last_agg_flush_time,
             '| 最后一次 PTW walk/prefetch 完成 | %s |' % last_ptw_done_time,
             '| 日志最后时间戳 | %s |' % last_time,
             '| 看门狗Fatal时刻 | %s |' % fatal_ns,
             '| 最后活动→Fatal 的静默时长 | %s |' % (
                 ('%.1f ns' % (fatal_ns-last_time)) if fatal_ns and last_time else 'N/A'), '',
             '## 关键计数',
             '| 项目 | 数值 |', '|---|---:|',
             '| AGG_FLUSH 完成组数 | %d |' % agg_flush_count,
             '| MONITOR_PT_UPD 记录的PT更新FIFO阻塞次数 | %d |' % monitor_pt_upd_count,
             '| PT_HASH_BP 总次数 | %d |' % tag_count['[PT_HASH_BP]'],
             '| DEDUP_CACHE_BYPASS 行数 | %d |' % tag_count['[DEDUP_CACHE_BYPASS]'],
             '| DEDUP_FLUSH_CB 行数 | %d |' % tag_count['[DEDUP_FLUSH_CB]'], '']
    if hash_bp_by_ram:
        lines += ['### PT Hash 反压按RAM分布', '| RAM | 次数 |', '|---|---:|']
        lines += ['| %s | %d |' % kv for kv in sorted(hash_bp_by_ram.items())]
        lines.append('')
    lines += ['## 标签频次（前25）', '| 标签 | 次数 |', '|---|---:|']
    lines += ['| %s | %d |' % kv for kv in tag_count.most_common(25)]

    # 阻塞点定位：最后一个写过PT更新却没有AGG_FLUSH完成行的组，即卡在pt_update_fifo.write的组。
    stuck = [g for g in pt_upd_order if g not in agg_flush_groups]
    lines += ['', '## 阻塞点定位']
    if stuck:
        g = stuck[-1]
        wrote = pt_upd_by_group.get(g, [])
        expect = batch_by_group.get(g, [])
        missing = [x for x in expect if x not in wrote]
        lines += ['- 卡在PT更新写入的组：**group=%d**（有PT更新阻塞记录、但无AGG_FLUSH完成行）' % g,
                  '- 该组已成功写入的iova（%d个）：%s' % (len(wrote), ', '.join(wrote) or '无'),
                  '- 该组批次应有的iova（%d个）：%s' % (len(expect), ', '.join(expect) or '日志未见批次记录'),
                  '- 未能写入的iova：%s' % (', '.join(missing) or '无法从日志确定'),
                  '- 同类未完成的组数量：%d（%s）' % (len(stuck), ', '.join(str(x) for x in stuck[-8:]))]
    else:
        lines.append('- 未发现“有PT更新阻塞记录但无AGG_FLUSH完成行”的组；需结合完整日志进一步定位。')
    if last_hash_bp:
        lines += ['- 最后一次PT Hash反压：%s' % last_hash_bp[0],
                  '- 说明PT Hash单元当时正阻塞在 %s 的RAM FIFO写入上，无法继续消费pt_update_fifo。' % last_hash_bp[1]]
    for label, value in (('[MD_WATCHDOG] 汇总', wpd_state), ('[MDMC_STATE]', md_state),
                         ('[MDMC_RWQ_STATE]', rwq_state)):
        if value:
            lines.append('- %s：%s' % (label, value))
    lines.append('')
    lines += ['', '## 看门狗状态推进（末尾%d次快照）' % len(watchdog_blocks)]
    for i, block in enumerate(watchdog_blocks):
        lines += ['', '### 快照 %d' % (i+1), '```']
        lines += block
        lines.append('```')
    for tag in ('[AGG_FLUSH]', '[MONITOR_PT_UPD]', '[PT_HASH_BP]', '[PTW_PREFETCH_MONITOR]',
                '[DEDUP_CACHE_BYPASS]', '[DEDUP_FLUSH_CB]', '[PT_CACHE]'):
        if tail_by_tag[tag]:
            lines += ['', '## 末尾 %s（最后%d行）' % (tag, len(tail_by_tag[tag])), '```']
            lines += list(tail_by_tag[tag])
            lines.append('```')
    lines += ['', '## 日志末尾（含Fatal）', '```']
    lines += list(fatal)
    lines.append('```')
    out.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('DEADLOCK_EVIDENCE', out, 'lines=%d' % total_lines, 'last_time=%s' % last_time,
          'agg_flush=%d' % agg_flush_count, 'pt_upd_block=%d' % monitor_pt_upd_count, flush=True)


if __name__ == '__main__':
    main()
