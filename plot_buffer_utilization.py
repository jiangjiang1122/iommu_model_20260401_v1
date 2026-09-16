#!/usr/bin/env python3
"""
Dedup Buffer 利用率曲线生成脚本
读取 dedup_buffer_utilization.csv 并生成可视化曲线

横轴: Buffer申请序号 (第N次allocate)
纵轴: 该任务申请Buffer时，当前Buffer的已写入任务数目
"""

import csv
import sys
import os

try:
    import matplotlib.pyplot as plt
    import matplotlib
    matplotlib.use('Agg')  # 使用非交互式后端
    MATPLOTLIB_AVAILABLE = True
except ImportError:
    MATPLOTLIB_AVAILABLE = False

def read_csv(filename):
    """读取CSV数据，只保留allocate事件"""
    request_ids = []
    valid_counts = []
    timestamps = []
    req_id = 0
    
    with open(filename, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            is_allocate = int(row['is_allocate'])
            if is_allocate == 1:
                req_id += 1
                request_ids.append(req_id)
                valid_counts.append(int(row['valid_count']))
                timestamps.append(float(row['timestamp_ns']))
    
    return request_ids, valid_counts, timestamps

def generate_ascii_curve(request_ids, valid_counts, buffer_size):
    """生成ASCII曲线图: 横轴=请求ID, 纵轴=Buffer占用数"""
    if not request_ids:
        print("No allocate data to plot")
        return
    
    # 计算统计信息
    total_requests = len(request_ids)
    max_count = max(valid_counts)
    avg_count = sum(valid_counts) / len(valid_counts)
    
    print("\n" + "="*70)
    print("  Dedup Buffer 利用率曲线 (按请求序号)")
    print("="*70)
    print(f"  Buffer Size:        {buffer_size} entries")
    print(f"  Total Allocates:    {total_requests}")
    print(f"  Peak Usage:         {max_count} entries ({100.0*max_count/buffer_size:.1f}%)")
    print(f"  Avg Usage:          {avg_count:.2f} entries ({100.0*avg_count/buffer_size:.1f}%)")
    print("-"*70)
    
    # ASCII曲线参数
    WIDTH = 60
    HEIGHT = 20
    
    # 将数据分桶采样 (按请求ID分桶)
    num_buckets = WIDTH
    bucket_size = total_requests / num_buckets if total_requests > 0 else 1
    
    bucket_sum = [0] * num_buckets
    bucket_cnt = [0] * num_buckets
    
    for req_id, count in zip(request_ids, valid_counts):
        bucket_idx = min(int((req_id - 1) / bucket_size), num_buckets - 1)
        bucket_sum[bucket_idx] += count
        bucket_cnt[bucket_idx] += 1
    
    # 绘制曲线
    print(f"  Buffer占用数 (entries)")
    print(f"  {buffer_size:4d} |")
    
    for row in range(HEIGHT, -1, -1):
        threshold = row * buffer_size / HEIGHT
        line = f"  {threshold:5.0f} |"
        
        for col in range(num_buckets):
            if bucket_cnt[col] > 0:
                avg_val = bucket_sum[col] / bucket_cnt[col]
                if avg_val >= threshold:
                    line += "*"
                else:
                    line += " "
            else:
                line += " "
        
        print(line)
    
    print(f"      0 +{'-'*WIDTH}")
    print(f"        1{'':^{WIDTH-20}}{total_requests}")
    print(f"        {'Buffer Allocate Request ID':^{WIDTH}}")
    print("="*70)
    
    # 打印占用率分布直方图
    print("\n  Buffer占用率分布直方图:")
    print("-"*50)
    
    hist_bins = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
    hist_counts = [0] * (len(hist_bins) - 1)
    
    for count in valid_counts:
        pct = 100.0 * count / buffer_size
        for i in range(len(hist_bins) - 1):
            if hist_bins[i] <= pct < hist_bins[i+1]:
                hist_counts[i] += 1
                break
        else:
            if pct >= 100:
                hist_counts[-1] += 1
    
    max_hist = max(hist_counts) if hist_counts else 1
    bar_width = 30
    
    for i in range(len(hist_bins) - 1):
        pct_range = f"[{hist_bins[i]:3d}%,{hist_bins[i+1]:3d}%)"
        bar_len = int(bar_width * hist_counts[i] / max_hist) if max_hist > 0 else 0
        bar = "#" * bar_len
        print(f"  {pct_range}: {bar} {hist_counts[i]}")
    
    print("-"*50)

def generate_png_chart(request_ids, valid_counts, buffer_size, output_file):
    """使用 matplotlib 生成 PNG 图表"""
    if not MATPLOTLIB_AVAILABLE:
        print("\n  Warning: matplotlib not available, skipping PNG generation")
        print("  Install with: pip3 install matplotlib")
        return False
    
    try:
        # 创建图表
        fig, ax = plt.subplots(figsize=(14, 7))
        
        # 绘制曲线
        ax.plot(request_ids, valid_counts, linewidth=0.8, color='blue', alpha=0.7)
        
        # 添加平均线
        avg_count = sum(valid_counts) / len(valid_counts)
        ax.axhline(y=avg_count, color='red', linestyle='--', linewidth=1.5, 
                   label=f'Avg: {avg_count:.1f} ({100*avg_count/buffer_size:.1f}%)')
        
        # 添加峰值线
        max_count = max(valid_counts)
        ax.axhline(y=max_count, color='orange', linestyle=':', linewidth=1.5,
                   label=f'Peak: {max_count} ({100*max_count/buffer_size:.1f}%)')
        
        # 设置标签和标题
        ax.set_xlabel('Buffer Allocate Request ID', fontsize=12)
        ax.set_ylabel('Buffer Valid Count (entries)', fontsize=12)
        ax.set_title('Dedup Buffer Utilization by Request ID', fontsize=14, fontweight='bold')
        
        # 设置 Y 轴范围
        ax.set_ylim(0, buffer_size)
        ax.set_xlim(1, len(request_ids))
        
        # 添加网格
        ax.grid(True, alpha=0.3, linestyle='-', linewidth=0.5)
        
        # 添加图例
        ax.legend(loc='upper right', fontsize=10)
        
        # 添加 Buffer 容量标注
        ax.text(0.02, 0.98, f'Buffer Size: {buffer_size} entries\nTotal Allocates: {len(request_ids)}',
                transform=ax.transAxes, fontsize=10, verticalalignment='top',
                bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
        
        # 调整布局并保存
        plt.tight_layout()
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"\n  PNG chart saved to: {output_file}")
        
        return True
        
    except Exception as e:
        print(f"\n  Error generating PNG chart: {e}")
        return False

def main():
    csv_file = "dedup_buffer_utilization.csv"
    
    if len(sys.argv) > 1:
        csv_file = sys.argv[1]
    
    if not os.path.exists(csv_file):
        print(f"Error: File '{csv_file}' not found")
        print("Please run the simulation first to generate the CSV file")
        sys.exit(1)
    
    request_ids, valid_counts, timestamps = read_csv(csv_file)
    
    # 从CSV推断buffer_size (最大valid_count的向上取整到常见值)
    max_count = max(valid_counts) if valid_counts else 512
    if max_count <= 256:
        buffer_size = 256
    elif max_count <= 512:
        buffer_size = 512
    else:
        buffer_size = max_count
    
    generate_ascii_curve(request_ids, valid_counts, buffer_size)
    
    # 生成 PNG 图表 (使用 matplotlib)
    png_file = "dedup_buffer_utilization.png"
    generate_png_chart(request_ids, valid_counts, buffer_size, png_file)
    
    # 生成按请求ID的gnuplot脚本 (备用方案)
    gnuplot_file = "plot_buffer_utilization.gnuplot"
    with open(gnuplot_file, 'w') as f:
        f.write(f"""# Gnuplot script for Dedup Buffer Utilization (by Request ID)
set terminal png size 1200,600
set output 'dedup_buffer_utilization.png'
set title 'Dedup Buffer Utilization by Buffer Allocate Request ID'
set xlabel 'Buffer Allocate Request ID'
set ylabel 'Buffer Valid Count (entries)'
set yrange [0:{buffer_size}]
set grid
set datafile separator ','

# 只绘制allocate事件 (is_allocate=1)
plot '{csv_file}' using ($5==1 ? ($.0+1) : 1/0):2 with lines title 'Buffer Valid Count' lw 1 lc rgb 'blue'
""")
    
    print(f"\n  Gnuplot script generated: {gnuplot_file}")
    print(f"  Run 'gnuplot {gnuplot_file}' to generate PNG chart")

if __name__ == "__main__":
    main()
