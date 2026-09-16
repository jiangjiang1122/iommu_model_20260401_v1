#!/usr/bin/env python3
"""
PTW=20 并发曲线图生成脚本
横轴: 任务序号 (按dispatch顺序)
纵轴: 实际并发任务数
"""

import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

def plot_ptw_concurrent(csv_file, output_file):
    # 读取CSV
    task_seq = []
    concurrent = []

    with open(csv_file, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            task_seq.append(int(row['task_seq']))
            concurrent.append(int(row['concurrent_count']))

    # 创建图表
    fig, ax = plt.subplots(figsize=(14, 6))

    # 绘制曲线
    ax.plot(task_seq, concurrent, linewidth=0.8, color='#2196F3', alpha=0.9)

    # 填充区域
    ax.fill_between(task_seq, concurrent, alpha=0.3, color='#2196F3')

    # 添加配置上限线
    config_limit = 20
    ax.axhline(y=config_limit, color='#FF5722', linestyle='--', linewidth=1.5,
               label=f'Config Limit (PTW={config_limit})', alpha=0.8)

    # 添加平均线
    avg_conc = sum(concurrent) / len(concurrent)
    ax.axhline(y=avg_conc, color='#4CAF50', linestyle='-', linewidth=1.5,
               label=f'Avg Concurrent ({avg_conc:.2f})', alpha=0.8)

    # 标注峰值
    max_conc = max(concurrent)
    max_idx = concurrent.index(max_conc)
    ax.annotate(f'Peak={max_conc}',
                xy=(task_seq[max_idx], max_conc),
                xytext=(task_seq[max_idx] + 50, max_conc + 1),
                arrowprops=dict(arrowstyle='->', color='#FF5722'),
                fontsize=10, color='#FF5722', fontweight='bold')

    # 设置标签和标题
    ax.set_xlabel('Task Sequence (Dispatch Order)', fontsize=12, fontweight='bold')
    ax.set_ylabel('Concurrent Task Count', fontsize=12, fontweight='bold')
    ax.set_title('PTW Concurrent Task Count vs Task Sequence (PTW=20, Scene7 4KB Random Read)',
                 fontsize=13, fontweight='bold', pad=15)

    # 设置图例
    ax.legend(loc='lower right', fontsize=10, framealpha=0.9)

    # 设置坐标轴范围
    ax.set_xlim(0, max(task_seq))
    ax.set_ylim(0, max_conc + 3)

    # 添加网格
    ax.grid(True, linestyle='--', alpha=0.5)

    # 添加统计信息框
    stats_text = f'Statistics:\n'
    stats_text += f'  Total Tasks: {len(concurrent)}\n'
    stats_text += f'  Avg Concurrent: {avg_conc:.2f}\n'
    stats_text += f'  Max Concurrent: {max_conc}\n'
    stats_text += f'  Min Concurrent: {min(concurrent)}\n'
    stats_text += f'  Config Limit: {config_limit}'

    props = dict(boxstyle='round', facecolor='wheat', alpha=0.8)
    ax.text(0.02, 0.98, stats_text, transform=ax.transAxes, fontsize=9,
            verticalalignment='top', bbox=props)

    # 调整布局
    plt.tight_layout()

    # 保存图片
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"图表已保存: {output_file}")

    # 同时保存PNG和SVG
    svg_file = output_file.replace('.png', '.svg')
    plt.savefig(svg_file, bbox_inches='tight')
    print(f"SVG图表已保存: {svg_file}")

    plt.close()

if __name__ == '__main__':
    csv_file = 'ptw_concurrent_by_taskid.csv'
    output_file = 'ptw_concurrent_curve_ptw20.png'
    plot_ptw_concurrent(csv_file, output_file)
