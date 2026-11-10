#!/usr/bin/env python3
"""跨归档对照：agg_flush释放优先修复前后、以及FIFO扩容替代方案的关键指标汇总。

只读各归档内的 full_perf.json 与 sim_tail.txt，不重跑仿真、不修改任何历史产物。
用法：python3 rp/compare_release_first_fix.py [--out 文件]
"""
import argparse
import json
from pathlib import Path
import re

# 标签 -> 归档目录（相对仓库根）
ARCHIVES = [
    ('n8c1 修复前 8×250', 'rwsplit_results/nvme10_n8c1_20261029_fit10/n8c1'),
    ('n8c1 修复后 8×250', 'rwsplit_results/nvme10_n8c1_20261030_relfirst_g250/n8c1'),
    ('n16c1 修复前 16×125', 'rwsplit_results/nvme10_n16c1_20261030_fifo32_g125/n16c1'),
    ('n16c1 修复前 16×250 FIFO1024', 'rwsplit_results/nvme10_n16c1_20261029_expand16_fix1024/n16c1'),
    ('n16c1 修复后 16×250 FIFO32', 'rwsplit_results/nvme10_n16c1_20261030_relfirst_g250/n16c1'),
]


def log_value(text, pattern, cast=float):
    m = re.search(pattern, text, re.M)
    return cast(m.group(1)) if m else None


def collect(root, rel):
    d = root / rel
    perf = json.loads((d / 'full_perf.json').read_text(encoding='utf-8'))
    text = (d / 'sim_tail.txt').read_text(encoding='utf-8', errors='replace')
    req, op, raw = perf['requests'], perf['operations'], perf['raw_log_metrics']
    # 监控点2专段：避免与监控点1(dedup flush FIFO)的同名计数字段混淆。
    i2 = text.find('监控点2: PT Cache UPDATE 阻塞')
    seg2 = text[i2:i2+1500] if i2 >= 0 else ''
    mi = d / 'model_info.json'
    if mi.exists():
        info = json.loads(mi.read_text(encoding='utf-8'))
        model = '释放优先' if info.get('agg_flush_release_first') == 1 else '旧交替顺序'
    else:
        model = '旧交替顺序'
    cd = d / 'config_delta.json'
    fifo = None
    if cd.exists():
        delta = json.loads(cd.read_text(encoding='utf-8'))
        fifo = delta.get('pt_update_fifo_depth')
    if fifo is None and cd.exists():
        # 旧归档的config_delta无独立字段，从changed文本解析覆盖值。
        for c in json.loads(cd.read_text(encoding='utf-8')).get('changed', []):
            m = re.search(r'PT_UPDATE_FIFO_DEPTH=\d+\(default\) -> (\d+)', c)
            if m:
                fifo = int(m.group(1))
    if fifo is None:
        fifo = log_value(text, r'^\s*pt_update_fifo\s+(\d+)', int)
    return {
        'dir': rel, 'model': model, 'fifo_cfg': fifo,
        'devices': len(req['per_device']), 'packets': req['total'],
        'fifo_depth_log': log_value(text, r'^\s*pt_update_fifo\s+(\d+)', int),
        'fifo_peak_log': log_value(text, r'^\s*pt_update_fifo\s+\d+\s+(\d+)', int),
        'fifo_avg_log': log_value(text, r'^\s*pt_update_fifo\s+\d+\s+\d+\s+[\d.]+%\s+([\d.]+)'),
        'pt_upd_writes': log_value(seg2, r'PT UPDATE写入总次数:\s*(\d+)', int),
        'pt_upd_blocks': log_value(seg2, r'FIFO阻塞次数:\s*(\d+)', int),
        'pt_upd_block_ns': log_value(seg2, r'FIFO阻塞总耗时:\s*([\d.]+) ns'),
        'pt_upd_max_ns': log_value(seg2, r'单次PT UPDATE最大耗时:\s*([\d.]+) ns'),
        'adm_stall_waits': log_value(text, r'Admission stall waits:\s*(\d+)', int),
        'adm_stall_ns': log_value(text, r'Admission stall total:\s*([\d.]+) ns'),
        'hash_bp_count': log_value(text, r'Tasks hashed:\s*\d+\s*\n\s*Busy time:\s*[\d.]+ ns\s*\n\s*Backpressure count:\s*(\d+)', int),
        'buffer_avg_model': log_value(text, r'Avg Occupancy:\s*([\d.]+) entries'),
        'buffer_final': log_value(text, r'Current Valid:\s*(\d+) entries', int),
        'dedup_final': log_value(text, r'Final valid lines:\s*(\d+)', int),
        'port_mismatch': log_value(text, r'端口方向校验失配次数\(必须=0\):\s*(\d+)', int),
        'all_iops': req['all_iops'], 'data512_iops': req['data512_iops'],
        'input_bw': req['ports']['accept_ns']['bw'],
        'jain': req['jain'], 'window_ns': req['window'][2],
        'e2e_avg': req['e2e']['ALL']['avg'], 'e2e_max': req['e2e']['ALL']['max'],
        'conc_global': req['concurrency']['global']['avg'],
        'conc_full_pct': req['concurrency']['global']['full_ratio'],
        'page512_avg': req['page512']['avg'], 'page512_peak': req['page512']['peak'],
        'hold_avg_ns': req['concurrency']['global']['avg'] / req['all_iops'] * 1000,
        'dedup_hit_pct': op['cache']['dedup']['w_rate'],
        'pt512_hit_pct': op['cache']['pt_512']['w_rate'],
        'vs_c3_pct': op['cache']['vs_c3']['w_rate'],
        's2_c3_pct': op['cache']['s2_c3']['w_rate'],
        'ptw_out_avg': op['ptw_outstanding_w']['avg'],
        'ptw_out_peak': op['ptw_outstanding_w']['peak'],
        'ptw_total': raw.get('ptw_total', raw['ptw_main'] + raw['ptw_prefetch']),
        'ptw_main': raw['ptw_main'], 'ptw_pf': raw['ptw_prefetch'],
        'ptw_iops_w': sum(op['ptw'][k]['w_iops'] for k in ('main_complete', 'prefetch_complete')),
        'ptw_main_ddr_w': op['ptw']['main_complete']['w_ddr']['avg'],
        'ptw_group_ns': op['ptw']['group_complete']['w_latency']['avg'],
        'buf_w_avg': op['buffer_w']['avg'],
        'susp_w1': perf['model_log']['hold'].get('susp', {}).get('w1'),
        'main_w1': perf['model_log']['hold'].get('main_miss', {}).get('w1'),
    }


ROWS = [
    ('模型版本(agg_flush)', 'model', '%s'),
    ('PT更新FIFO配置/日志深度', None, None),
    ('设备数×每设备组数', None, None),
    ('正式包数', 'packets', '%d'),
    ('ALL IOPS (Mreq/s)', 'all_iops', '%.3f'),
    ('512B IOPS (Mreq/s)', 'data512_iops', '%.3f'),
    ('入口带宽 (GB/s)', 'input_bw', '%.3f'),
    ('Jain 公平性', 'jain', '%.6f'),
    ('稳态窗口时长 (ns)', 'window_ns', '%.1f'),
    ('平均在系统时长 (ns)', 'hold_avg_ns', '%.1f'),
    ('Global并发平均 / 顶满%', None, None),
    ('IO e2e 平均 / 最大 (ns)', None, None),
    ('512B页并发 平均 / 峰值', None, None),
    ('去重命中率 %', 'dedup_hit_pct', '%.3f'),
    ('PT 512B命中率 %', 'pt512_hit_pct', '%.3f'),
    ('VS C3命中率 %', 'vs_c3_pct', '%.3f'),
    ('S2 C3命中率 %', 's2_c3_pct', '%.3f'),
    ('PTW总任务(主+预取)', None, None),
    ('PTW IOPS (W)', 'ptw_iops_w', '%.3f'),
    ('PTW额度并发 平均 / 峰值', None, None),
    ('主任务平均DDR (W)', 'ptw_main_ddr_w', '%.3f'),
    ('PTW组执行延时 (ns)', 'ptw_group_ns', '%.1f'),
    ('Buffer占用平均(W) / 模型全期', None, None),
    ('Buffer持有W1 主 / 挂起 (ns)', None, None),
    ('PT更新写入次数 / 阻塞次数', None, None),
    ('PT更新阻塞总耗时 (ns)', 'pt_upd_block_ns', '%.1f'),
    ('PT更新单次最大阻塞 (ns)', 'pt_upd_max_ns', '%.1f'),
    ('PT更新FIFO峰值 / 全期平均', None, None),
    ('准入停顿 次数 / 总时长(ns)', None, None),
    ('PT Hash反压次数', 'hash_bp_count', '%d'),
    ('最终Buffer / dedup有效行', None, None),
    ('端口方向失配', 'port_mismatch', '%d'),
]


def cell(rec, key, fmt):
    v = rec.get(key)
    return 'N/A' if v is None else (fmt % v)


def render(recs):
    labels = [r[0] for r in recs]
    out = ['| 指标 | ' + ' | '.join(labels) + ' |',
           '|---|' + '---:|' * len(labels)]
    for name, key, fmt in ROWS:
        vals = []
        for _, rec in recs:
            if key is not None:
                vals.append(cell(rec, key, fmt))
            elif name.startswith('PT更新FIFO配置'):
                vals.append('%s / %s' % (rec['fifo_cfg'], rec['fifo_depth_log']))
            elif name.startswith('设备数×'):
                vals.append('%d×%d' % (rec['devices'], rec['packets']//rec['devices']//10))
            elif name.startswith('Global并发平均'):
                vals.append('%.2f / %.2f%%' % (rec['conc_global'], rec['conc_full_pct']))
            elif name.startswith('IO e2e'):
                vals.append('%.1f / %.1f' % (rec['e2e_avg'], rec['e2e_max']))
            elif name.startswith('512B页并发'):
                vals.append('%.3f / %d' % (rec['page512_avg'], rec['page512_peak']))
            elif name.startswith('PTW总任务'):
                vals.append('%d = %d+%d' % (rec['ptw_total'], rec['ptw_main'], rec['ptw_pf']))
            elif name.startswith('PTW额度并发'):
                vals.append('%.3f / %d' % (rec['ptw_out_avg'], rec['ptw_out_peak']))
            elif name.startswith('Buffer占用平均'):
                vals.append('%.3f / %.2f' % (rec['buf_w_avg'], rec['buffer_avg_model'] or float('nan')))
            elif name.startswith('Buffer持有W1'):
                vals.append('%s / %s' % (rec['main_w1'], rec['susp_w1']))
            elif name.startswith('PT更新写入次数'):
                vals.append('%s / %s' % (rec['pt_upd_writes'], rec['pt_upd_blocks']))
            elif name.startswith('PT更新FIFO峰值'):
                vals.append('%s / %s' % (rec['fifo_peak_log'], rec['fifo_avg_log']))
            elif name.startswith('准入停顿'):
                vals.append('%s / %s' % (rec['adm_stall_waits'], rec['adm_stall_ns']))
            elif name.startswith('最终Buffer'):
                vals.append('%s / %s' % (rec['buffer_final'], rec['dedup_final']))
            else:
                vals.append('N/A')
        out.append('| %s | %s |' % (name, ' | '.join(vals)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    recs = []
    for label, rel in ARCHIVES:
        d = root / rel
        if not (d / 'full_perf.json').exists():
            print('SKIP（缺少full_perf.json）', label, rel, flush=True)
            continue
        recs.append((label, collect(root, rel)))
    lines = ['# agg_flush释放优先修复：跨归档关键指标对照', '',
             '- 全部数据来自各归档已有的 full_perf.json 与 sim_tail.txt，只读复算，未重跑任何仿真。',
             '- “修复前”=按iova交替写(释放→回填)；“修复后”=整组先写释放(dedup_update)再写回填(pt_update)。',
             '- 死锁归档（16×250、FIFO32、修复前）无full_perf.json，不在表内；其证据见 deadlock_evidence.md。', '']
    lines += render(recs)
    text = '\n'.join(lines) + '\n'
    if args.out:
        Path(args.out).write_text(text, encoding='utf-8')
        print('WRITTEN', args.out, flush=True)
    print(text, flush=True)


if __name__ == '__main__':
    main()
