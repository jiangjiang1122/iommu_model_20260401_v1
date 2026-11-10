#!/usr/bin/env python3
"""从 compare_37.json 导出指定两列的37项对比表（Markdown），只读、不重算、不重跑仿真。"""
import argparse
import json
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--json', required=True, help='compare_37.json 路径')
    ap.add_argument('--cols', default='1,2', help='场景列序号，从0开始，逗号分隔')
    ap.add_argument('--out', default=None)
    cli = ap.parse_args()
    data = json.loads(Path(cli.json).read_text(encoding='utf-8'))
    scenes = data['scenes']
    idx = [int(x) for x in cli.cols.split(',')]
    for i in idx:
        assert 0 <= i < len(scenes), '列序号越界：%d' % i
    labels = [scenes[i]['label'] for i in idx]
    lines = ['# 37项对比（%s）' % ' vs '.join(labels), '',
             '|编号|指标|口径|' + '|'.join(labels) + '|说明|',
             '|---:|---|---|' + '---|' * len(labels) + '---|']
    for r in data['rows37']:
        vals = [r['values'][i] for i in idx]
        lines.append('|%d|%s|%s|%s|%s|' % (r['id'], r['metric'], r['scope'],
                                           '|'.join(vals), r['note']))
    text = '\n'.join(lines) + '\n'
    if cli.out:
        Path(cli.out).write_text(text, encoding='utf-8')
        print('WRITTEN', cli.out, flush=True)
    print(text, flush=True)


if __name__ == '__main__':
    main()
