#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""[9场景统一口径对照 20261110]

严格满足"每个场景都从 log 用同一套脚本分析":
  - 全部指标由同一份代码从 <run_dir>/sim.log 及其伴生 CSV 提取, 无任何场景专属分支;
  - 命中率/IOPS/Buffer/PTW 类取自 skill 权威提取器产物 perf_scene.json
    (该 JSON 由 tmp/nvrr9_preflight.sh 对 9 个 log 统一重新生成);
  - 端口带宽(稳态) / 4KB页并发 / 去重Cache占用行 直接解析 sim.log 同名段落,
    正则沿用项目既有 rp/extract_nvrr_full_metrics.py 的已验证写法;
  - e2e 的数据包/控制包拆分按用户统一口径 **bytes==512 为数据包, 其余为控制包**,
    由 *_requests.csv 的 admit_ns->release_ns 计算, 9 个场景同一算法、同一 epoch 规则。

real8 使用 KB4_GATE_STRICT_512=1 变体, 使其 4KB 页并发与合成场景为同一仪器读数。
"""
import csv, glob, json, os, re, sys

SCENES = [
    ('real8 实测8盘回放',   '/tmp/nvrr6_real8_calib/v1'),
    ('8盘1通道',           '/tmp/nvrr6_ptfifo1024_20261109/s1_n8c1'),
    ('8盘2通道',           '/tmp/nvrr6_ptfifo1024_20261109/s2_n8c2'),
    ('8盘4通道',           '/tmp/nvrr6_ptfifo1024_20261109/s3_n8c4'),
    ('8盘8通道',           '/tmp/nvrr6_ptfifo1024_20261109/s7_n8c8'),
    ('16盘1通道',          '/tmp/nvrr6_ptfifo1024_20261109/s4_n16c1'),
    ('16盘2通道',          '/tmp/nvrr6_ptfifo1024_20261109/s5_n16c2'),
    ('16盘4通道',          '/tmp/nvrr6_ptfifo1024_20261109/s6_n16c4'),
    ('16盘8通道',          '/tmp/nvrr6_ptfifo1024_20261109/s8_n16c8'),
]

RE_KV = re.compile(r'([A-Za-z_/][\w/]*)=([^\s()]+)')
# 端口带宽(稳态窗口) —— 正则与 rp/extract_nvrr_full_metrics.py 保持一致
RE_STEADY_BLK = re.compile(r'\[稳态窗口\s+([\d.]+)\s*ns:?\s*([\d.~]*)\](.*?)(\[读口注入期|=====)', re.S)
RE_RD = re.compile(r'读口 slave_0r:\s+(\d+) B\s+([\d.]+) GB/s \(util ([\d.]+)%')
RE_WR = re.compile(r'写口 slave_0w:\s+(\d+) B\s+([\d.]+) GB/s \(util ([\d.]+)%')
RE_AGG = re.compile(r'入口合计:\s+([\d.]+) GB/s \(聚合peak 128 GB/s, util ([\d.]+)%\)')
RE_KB4 = re.compile(r'^\s*TOTAL\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)', re.M)
RE_KB4_WIN = re.compile(r'Sampling window:\s+([\d.]+) ns')
RE_KB4_DEVPEAK = re.compile(r'Max single-device peak:\s+(\d+)')
RE_VALID = re.compile(r'Avg valid lines:\s+([\d.]+)\s+\(([\d.]+)% of (\d+)\)')
RE_VALID_MAX = re.compile(r'Max / Min valid:\s+(\d+) / (\d+)')


def tail(path, nbytes=8 << 20):
    with open(path, 'rb') as f:
        f.seek(0, 2); size = f.tell()
        f.seek(max(0, size - nbytes))
        return f.read().decode('utf-8', 'replace')


def first(pat, text, grp=1, cast=float):
    # pat 均为已编译正则(必要时自带 re.M), 此处不得再传 flags
    m = pat.search(text)
    return cast(m.group(grp)) if m else None


def e2e_by_caliber(d):
    """统一口径 e2e: bytes==512 为数据包, 其余为控制包; 区间 admit_ns->release_ns。
       epoch 取该 log 的最大 epoch(正式测量段), 9 场景同一规则。"""
    g = sorted(glob.glob(os.path.join(d, '*requests.csv')))
    if not g:
        return None
    with open(g[0], newline='', encoding='utf-8') as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return None
    ep = max(int(r['epoch']) for r in rows if r.get('epoch', '').isdigit())
    rows = [r for r in rows if r.get('epoch') == str(ep)]
    out = {}
    for name, sel in (('data', lambda r: int(r['bytes']) == 512),
                      ('ctrl', lambda r: int(r['bytes']) != 512),
                      ('all',  lambda r: True)):
        v = []
        for r in rows:
            if not sel(r):
                continue
            try:
                a, b = float(r['admit_ns']), float(r['release_ns'])
            except (KeyError, ValueError):
                continue
            if b >= a:
                v.append(b - a)
        out[name] = (sum(v) / len(v) if v else None, max(v) if v else None, len(v))
    out['n_total'] = len(rows)
    out['n_data'] = out['data'][2]
    return out


def collect(d):
    log = tail(os.path.join(d, 'sim.log'))
    jp = sorted(glob.glob(os.path.join(d, 'perf_scene*.json')))
    m = json.load(open(jp[0], encoding='utf-8'))['metrics'] if jp else {}
    r = dict(m=m, log=log)
    # 端口带宽稳态
    blk = RE_STEADY_BLK.search(log)
    seg = blk.group(3) if blk else ''
    r['rd'] = RE_RD.search(seg); r['wr'] = RE_WR.search(seg); r['agg'] = RE_AGG.search(seg)
    r['steady_win'] = float(blk.group(1)) if blk else None
    # 4KB 页并发
    i = log.find('4KB Page Concurrency Statistics')
    kb = log[i:i + 60000] if i >= 0 else ''
    mm = RE_KB4.search(kb)
    r['kb4'] = dict(avg=float(mm.group(1)), peak=int(mm.group(2)),
                    in_tasks=int(mm.group(3)), pages=int(mm.group(5))) if mm else {}
    r['kb4']['dev_peak'] = first(RE_KB4_DEVPEAK, kb)
    # 去重 Cache 占用
    r['valid_avg'] = first(RE_VALID, log)
    r['valid_pct'] = first(RE_VALID, log, 2)
    r['valid_tot'] = first(RE_VALID, log, 3)
    mv = RE_VALID_MAX.search(log)
    r['valid_max'] = int(mv.group(1)) if mv else None
    r['e2e'] = e2e_by_caliber(d)
    return r


def pc(m, key):
    v = m.get(key)
    return 'N/A' if v is None else '%.2f%%' % v


def cache_pct(m, name):
    c = m.get('cache_' + name)
    return 'N/A' if not c else '%.2f%% (%d/%d)' % (c[3] * 100, c[1], c[0])


def hit3(m, key):
    c = m.get(key)
    return 'N/A' if not c else '%.2f%% (%d/%d)' % (c[2], c[0], c[1])


def f(v, fmt='%.2f'):
    return 'N/A' if v is None else (fmt % v)


def main():
    R = [(lb, collect(d)) for lb, d in SCENES]
    hdr = ['指标'] + [lb for lb, _ in R]
    rows = []
    A = rows.append

    m0 = lambda k: [x[1]['m'].get(k) for x in R]

    A(['IOMMU IOPS 稳态 (Mreq/s)',
       *[f((r['m'].get('fairness_ALL') or [None])[0]) for _, r in R]])
    A(['IOMMU 端口带宽·读写共128GB/s (稳态, GB/s / util%)',
       *[('%.2f (%.2f%%)' % (float(r['agg'].group(1)), float(r['agg'].group(2))))
         if r['agg'] else 'N/A' for _, r in R]])
    A([' 读通道平均带宽 (稳态, GB/s / util% of 64)',
       *[('%.2f (%.2f%%)' % (float(r['rd'].group(2)), float(r['rd'].group(3))))
         if r['rd'] else 'N/A' for _, r in R]])
    A([' 写通道平均带宽 (稳态, GB/s / util% of 64)',
       *[('%.2f (%.2f%%)' % (float(r['wr'].group(2)), float(r['wr'].group(3))))
         if r['wr'] else 'N/A' for _, r in R]])
    A(['IO avg e2e 总 (ns, 入口→出口)', *[f(r['m'].get('e2e_avg_ns'), '%.1f') for _, r in R]])
    A(['IO avg e2e 数据包 (ns, 512B, admit→release)',
       *[f(r['e2e']['data'][0], '%.1f') if r['e2e'] else 'N/A' for _, r in R]])
    A(['IO avg e2e 控制包 (ns, 非512B, admit→release)',
       *[f(r['e2e']['ctrl'][0], '%.1f') if r['e2e'] else 'N/A' for _, r in R]])
    A(['IO max e2e 总 (ns, 入口→出口)', *[f(r['m'].get('e2e_max_ns'), '%.1f') for _, r in R]])
    A(['IO max e2e 数据包 (ns)',
       *[f(r['e2e']['data'][1], '%.1f') if r['e2e'] else 'N/A' for _, r in R]])
    A(['IO max e2e 控制包 (ns)',
       *[f(r['e2e']['ctrl'][1], '%.1f') if r['e2e'] else 'N/A' for _, r in R]])
    A(['4KB页平均并发数目 (avg)',
       *[f(r['kb4'].get('avg'), '%.3f') for _, r in R]])
    A(['4KB页峰值并发数目 (peak)',
       *[f(r['kb4'].get('peak'), '%d') for _, r in R]])
    A([' 每设备峰值页', *[f(r['kb4'].get('dev_peak'), '%d') for _, r in R]])
    A([' 计入页并发的包数 / 不同页数',
       *['%d / %d' % (r['kb4']['in_tasks'], r['kb4']['pages']) if r['kb4'] else 'N/A' for _, r in R]])
    A(['PT Cache 命中率(总)', *[cache_pct(r['m'], 'pt_cache') for _, r in R]])
    A(['PT Cache 512B 数据命中率', *[pc(r['m'], 'pt_data_hit_pct') for _, r in R]])
    A(['PT Cache SQ/CQ/MSI 控制命中率', *[pc(r['m'], 'pt_ctrl_hit') for _, r in R]])
    A(['Walker Cache C3 命中率', *[hit3(r['m'], 'vs_c3') for _, r in R]])
    A(['Walker Cache C2 命中率', *[hit3(r['m'], 'vs_c2') for _, r in R]])
    A(['S2 Walker Cache C3 命中率', *[hit3(r['m'], 's2_c3') for _, r in R]])
    A(['去重 Cache 命中率', *[pc(r['m'], 'dedup_hit') for _, r in R]])
    A(['去重 hash 冲突次数 (占总查询比例)',
       *[('%d (%.2f%%)' % (r['m']['dedup_conflict'], r['m']['dedup_conflict_ratio_pct']))
         if r['m'].get('dedup_conflict') is not None else 'N/A' for _, r in R]])
    A(['去重 Cache 实际占用 cacheline (avg / max, 容量)',
       *[('%.2f / %s / %d' % (r['valid_avg'], f(r['valid_max'], '%s'), r['valid_tot']))
         if r['valid_avg'] is not None else 'N/A' for _, r in R]])
    A(['去重 Buffer Peak / Size',
       *[('%s / %s' % (f(r['m'].get('buf_peak'), '%d'), f(r['m'].get('buf_size'), '%d')))
         for _, r in R]])
    A(['去重 Buffer 实际平均占用 (entries / %)',
       *[('%.2f (%.2f%%)' % (r['m']['buf_avg_occ'], r['m']['buf_avg_pct']))
         if r['m'].get('buf_avg_occ') is not None else 'N/A' for _, r in R]])
    A(['去重 Buffer 任务持有延时 avg/max/min (ns, 申请→释放)',
       *[('%.1f / %.1f / %.1f' % (r['m']['hold_avg_ns'], r['m']['hold_max_ns'], r['m']['hold_min_ns']))
         if r['m'].get('hold_avg_ns') is not None else 'N/A' for _, r in R]])
    A(['PTW 主任务数 (正常 / bypass)',
       *[('%d (%d / %d)' % (r['m']['ptw_main'], r['m']['ptw_main_normal'], r['m']['ptw_main_bypass']))
         if r['m'].get('ptw_main') is not None else 'N/A' for _, r in R]])
    A(['PTW 预取任务数', *[f(r['m'].get('ptw_prefetch'), '%d') for _, r in R]])
    A(['PTW 总任务 (主+预取)', *[f(r['m'].get('ptw_completed'), '%d') for _, r in R]])
    A(['PTW 占 IOMMU 入口总任务比', *[pc(r['m'], 'ptw_ratio_all') for _, r in R]])
    A(['PTW 占 IOMMU 数据任务比', *[pc(r['m'], 'ptw_to_data_ratio_pct') for _, r in R]])
    A(['PTW 并发 peak / 平均占用 / 利用率',
       *[('%s / %.3f / %.2f%%' % (tuple(r['m']['conc_ptw'])[0], r['m']['ptw_conc_avg'],
                                  r['m']['ptw_conc_util']))
         if r['m'].get('conc_ptw') and r['m'].get('ptw_conc_avg') is not None else 'N/A'
         for _, r in R]])
    A(['PTW IOPS (主+预取, M tasks/s)', *[f(r['m'].get('ptw_iops')) for _, r in R]])
    A(['PTW 主任务平均执行延时 (ns)', *[f(r['m'].get('ptw_exec_main_avg_ns')) for _, r in R]])
    A(['PTW 主任务平均 DDR 次数', *[f(r['m'].get('ptw_main_ddr')) for _, r in R]])
    A(['PTW 预取任务平均 DDR 次数', *[f(r['m'].get('ptw_pf_ddr')) for _, r in R]])
    A(['[对账] 测量段包数 / 其中512B',
       *[('%d / %d' % (r['e2e']['n_total'], r['e2e']['n_data'])) if r['e2e'] else 'N/A' for _, r in R]])

    w = [max(len(str(rows[i][j])) for i in range(len(rows))) for j in range(len(hdr))]
    def line(cells):
        return '| ' + ' | '.join(str(c).ljust(w[k]) for k, c in enumerate(cells)) + ' |'
    print('# 9 场景统一口径性能指标对照（全部由同一脚本从各自 sim.log 提取）')
    print('')
    print('- real8 取 `KB4_GATE_STRICT_512=1` 变体: 4KB页并发与合成场景同一仪器(仅512B计入)。')
    print('- 全部运行点同工作树、同 IOMMU 配置(Global/Read/Write=640/320/320, PTW=64, Buffer=320, D=3, ADM=1, PT_UPDATE_FIFO=1024)。')
    print('- e2e 数据/控制拆分口径统一为 **bytes==512 为数据包, 其余为控制包**, 区间 parser-admit→reorder-release。')
    print('- 稳态窗口 W=完成事务 10%~90%; 4KB页并发/Cache命中/Buffer 为模型全程采样窗。')
    print('')
    print(line(hdr)); print('|' + '|'.join('-' * (x + 2) for x in w) + '|')
    for r in rows:
        print(line(r))
    return 0


if __name__ == '__main__':
    sys.exit(main())
