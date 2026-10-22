import json
import sys
from pathlib import Path

perf_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/tmp/real8_full/real8_full_perf.json')
d = json.loads(perf_path.read_text())
m = d['metrics']
print('== 10 身份 ==')
print('all_id :', m['10'].get('all_id'))
print('data_id:', m['10'].get('data_id'))
print('ctrl_id:', m['10'].get('ctrl_id'))
print('== 11 ==')
print('avg=%s peak=%s init=%s n_pages=%s' %
      (m['11'].get('avg'), m['11'].get('peak'),
       m['11'].get('init'), m['11'].get('n_pages')))
print('== 25 ==')
print('ratio_of_data_entry=%s' % m['25'].get('ratio_of_data_entry'))
print('== 30 ==')
print('main_of_data_entry=%s bypass_of_data_entry=%s' %
      (m['30'].get('main_of_data_entry'), m['30'].get('bypass_of_data_entry')))
print('== 26 ==')
print('init_v=%s init_r=%s end_v=%s end_r=%s rows=%s' %
      (m['26'].get('init_v'), m['26'].get('init_r'), m['26'].get('end_v'),
       m['26'].get('end_r'), m['26'].get('rows')))
print('== 27 ==')
print('init=%s full?=%s' % (m['27'].get('init'), m['27'].get('cap')))
print('== 31 ==')
print('init=%s w_util=%s' % (m['31'].get('init'), m['31'].get('w_util')))
print('== 6/7/8 full_ratio ==')
print(m['06'].get('full_ratio'), m['07'].get('full_ratio'), m['08'].get('full_ratio'))
print('== 32 ==')
print('w_iops=%s w_done=%s all_iops=%s' %
      (m['32'].get('w_iops'), m['32'].get('w_done_tasks'), m['32'].get('all_iops')))
print('== 16b/21b ==')
print('16b w=%s' % m.get('16b', {}).get('w_rate'))
print('21b w=%s' % m.get('21b', {}).get('w_rate'))

if len(sys.argv) > 1:
    import csv
    import math
    from collections import Counter, defaultdict
    run = Path(d['run_dir'])
    def rows(path):
        with open(path, newline='', encoding='utf-8') as f:
            return list(csv.DictReader(f))
    reqs = rows(run / 'real8_trace_requests.csv')
    remaps = rows(run / 'real8_trace_remap.csv')
    assert len(reqs) == len(remaps) == d['N_manifest'] == 20000
    inputs = {c: rows(run / 'real8_input' / (name + '_trace.csv'))
              for c, name in (('R', 'read'), ('W', 'write'))}
    by_seq = {(('W' if r['write'] == '1' else 'R'), int(r['seq'])): r for r in reqs}
    assert len(by_seq) == 20000
    mapped = {}
    generations = {}
    reverse = {}
    for r in remaps:
        direction, row = r['dir'], int(r['excel_row'])
        src = inputs[direction][row - 2]
        req = by_seq[direction, row - 2]
        assert int(src['excel_row']) == row
        assert r['did'] == src['did'] == req['did']
        assert r['bytes'] == src['bytes'] == req['bytes']
        assert r['is_ctrl'] == src['is_ctrl'] == req['type']
        assert int(r['source_page_no']) == int(src['page_base'], 16) >> 12
        assert int(r['offset']) == int(src['offset'])
        assert int(req['iova']) == 0x100000 + int(r['mapped_page_no']) * 4096 + int(r['offset'])
        assert int(req['expected_pa']) == int(r['expected_spa'])
        assert 0 <= int(r['mapped_page_no']) < 131069
        gen = r['gen_id']
        identity = (r['source_page_no'], r['mapped_page_no'])
        assert generations.setdefault(gen, identity) == identity
        assert reverse.setdefault(r['mapped_page_no'], gen) == gen
        mapped[direction, row] = r
    example = [mapped['W', i] for i in (2, 3, 4, 6, 7, 8, 10, 13)]
    assert {int(r['source_page_no']) for r in example} == {0xFAACB}
    assert len({r['mapped_page_no'] for r in example}) == 1
    assert len({r['gen_id'] for r in example}) == 1
    assert [int(r['offset']) for r in example] == list(range(0, 4096, 512))
    print('EXAMPLE_8_PACKETS_SAME_PAGE', example[0]['gen_id'],
          hex(0x100000 + int(example[0]['mapped_page_no']) * 4096))

    def sliding(keys, width=320):
        counts = Counter(keys[:width])
        vals = [len(counts)]
        for i in range(width, len(keys)):
            prev = keys[i-width]
            counts[prev] -= 1
            if not counts[prev]:
                del counts[prev]
            counts[keys[i]] += 1
            vals.append(len(counts))
        return {'avg': sum(vals)/len(vals), 'min': min(vals), 'max': max(vals), 'n': len(vals)}
    sliding_report = {}
    for direction in ('R', 'W'):
        src = inputs[direction]
        assert len(src) == 10000
        assert [int(r['excel_row']) for r in src] == list(range(2, 10002))
        sliding_report[direction] = {
            'source_all': sliding([r['page_base'] for r in src]),
            'source_data': sliding([r['page_base'] for r in src if r['is_ctrl'] == '0']),
            'mapped_all': sliding([mapped[direction, int(r['excel_row'])]['mapped_page_no'] for r in src]),
            'mapped_data': sliding([mapped[direction, int(r['excel_row'])]['mapped_page_no']
                                    for r in src if r['is_ctrl'] == '0'])}
    print('SLIDING320', json.dumps(sliding_report, ensure_ascii=False))

    # 独立对账：逐页合并重叠区间，再积分区间并集；不复用报告的引用事件算法。
    t0, t1 = d['window']['t10_ns'], d['window']['t90_ns']
    def unique_intervals(selected):
        per_page = defaultdict(list)
        for r in selected:
            per_page[int(r['did']), int(r['iova']) >> 12].append(
                (max(t0, float(r['accept_ns'])), min(t1, float(r['release_ns']))))
        area = 0.0
        events = []
        for intervals in per_page.values():
            merged = []
            for a, b in sorted(intervals):
                if b <= a:
                    continue
                if merged and a <= merged[-1][1]:
                    merged[-1][1] = max(b, merged[-1][1])
                else:
                    merged.append([a, b])
            for a, b in merged:
                area += b-a
                events.extend(((a, 1), (b, -1)))
        cur = peak = 0
        for _, delta in sorted(events):
            cur += delta
            peak = max(peak, cur)
        return {'avg': area/(t1-t0), 'peak': peak}
    pages = unique_intervals(r for r in reqs if r['type'] == '0')
    assert math.isclose(pages['avg'], m['11']['avg'], abs_tol=1e-6)
    assert pages['peak'] == m['11']['peak']
    assert math.isclose(m['06']['avg'], m['07']['avg'] + m['08']['avg'], abs_tol=1e-6)

    # 只统计 PTW source_module=1 的实际 DDR 请求，排除DC/XDTW相同task_id的访问。
    main_ids, pf_ids = set(), set()
    pf_starts, pf_done = {}, {}
    reads = Counter()
    s2_levels = {}
    vs3_hits = vs3_queries = 0
    s2_hits = s2_queries = 0
    writes = 0
    with open(run / 'real8_operations_raw.csv') as f:
        for r in csv.DictReader(f):
            tid = int(r['task_id'])
            src, op = r['source'], r['operation']
            if src == 'ptw':
                if op == 'main_start':
                    main_ids.add(tid)
                elif op == 'prefetch_spawn':
                    pf_ids.add(tid)
                    pf_starts[tid] = float(r['start_ns'])
                elif op == 'prefetch_complete':
                    assert tid not in pf_done
                    pf_done[tid] = (float(r['start_ns']), float(r['end_ns']), int(r['value']))
            elif src == 'ddr' and int(r['bank']) == 1:
                if op == 'read':
                    reads[tid] += 1
                elif op == 'write':
                    writes += 1
            elif src == 'walker_s2' and op == 'lookup':
                level = int(r['level']) if int(r['hit']) else 0
                s2_levels[tid] = level
                s2_queries += 1
                s2_hits += level == 3
            elif src == 'walker_vs_ram' and op == 'lookup' and int(r['level']) == 3:
                vs3_queries += 1
                vs3_hits += int(r['hit'])
    assert (vs3_hits, vs3_queries) == (m['19']['all_hits'], m['19']['all_queries'])
    assert (s2_hits, s2_queries) == (m['21']['all_hits'], m['21']['all_queries'])
    assert len(main_ids) == m['30']['all_main']
    assert len(pf_ids) == m['30']['all_prefetch']
    assert len(main_ids) + len(pf_ids) == m['30']['all_total']
    assert writes == 0
    ddr_report = {}
    for kind, ids, metric in (('main', main_ids, '33'), ('prefetch', pf_ids, '34')):
        hist = Counter(reads[tid] for tid in ids)
        avg = sum(reads[tid] for tid in ids)/len(ids)
        assert math.isclose(avg, m[metric]['avg'], abs_tol=1e-9)
        assert abs(avg - m[metric]['log_avg']) <= 0.0051
        ddr_report[kind] = {'count': len(ids), 'reads': sum(reads[tid] for tid in ids),
                            'avg': avg, 'histogram': dict(sorted(hist.items()))}
    leaf_ids = [tid for tid in pf_ids if s2_levels[tid] == 3]
    assert leaf_ids and all(reads[tid] == 1 for tid in leaf_ids)
    nonleaf_ids = [tid for tid in pf_ids if s2_levels[tid] == 2]
    assert nonleaf_ids and all(reads[tid] == 2 for tid in nonleaf_ids)
    assert set(pf_done) == pf_ids
    for tid, (start, end, count) in pf_done.items():
        assert math.isclose(start, pf_starts[tid], abs_tol=1e-6)
        assert end - start >= 200 and count == reads[tid]
    pf_latencies = [end - start for start, end, _ in pf_done.values()]
    pf_latency = {'count': len(pf_latencies), 'avg_ns': sum(pf_latencies)/len(pf_latencies),
                  'min_ns': min(pf_latencies), 'max_ns': max(pf_latencies)}
    assert abs(pf_latency['avg_ns'] - d['log_extract']['ptw_exec']['prefetch']['avg']) <= 0.0051
    print('PF_LIFETIME', json.dumps(pf_latency))
    devices = []
    for did in range(8):
        dev = [r for r in reqs if int(r['did']) == did]
        devices.append({'did': did, 'read': sum(r['write']=='0' for r in dev),
                        'write': sum(r['write']=='1' for r in dev), 'completed': len(dev),
                        'data': sum(r['type']=='0' for r in dev)})
        assert all(float(r['response_ns']) > 0 for r in dev)
        print('DEVICE', did, 'R', sum(r['write']=='0' for r in dev),
              'W', sum(r['write']=='1' for r in dev), 'completed', len(dev))
    print('ACTUAL_DDR', json.dumps(ddr_report))
    print('PF_S2_LEAF', len(leaf_ids), 'PF_S2_NONLEAF', len(nonleaf_ids))
    out = {'sliding320': sliding_report, 'page_interval_union': pages,
           'ddr': ddr_report, 'prefetch_latency': pf_latency, 'devices': devices,
           'prefetch_leaf_hits': len(leaf_ids),
           'prefetch_nonleaf_hits': len(nonleaf_ids),
           'example_iova_page': hex(0x100000 + int(example[0]['mapped_page_no']) * 4096),
           'checks': 'PASS'}
    (perf_path.parent / 'real8_validation.json').write_text(
        json.dumps(out, ensure_ascii=False, indent=2))
    print('REAL8_MAPPING_METRICS_DDR_PASS')
