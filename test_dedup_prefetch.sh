#!/bin/bash
# Test 4: P1~P8连续同页访问 (4KB页, D=8)
# 验证PT Cache去重+预取功能

echo "=========================================="
echo "Test 4: P1~P8 连续同页访问 (D=8)"
echo "=========================================="
echo ""

# 运行测试（10个请求，确保覆盖P1~P8）
./iommu_model 2>&1 | tee output_test4_dedup_prefetch.txt

echo ""
echo "=========================================="
echo "Analysis: 提取关键日志"
echo "=========================================="
echo ""

# 1. 统计PT Cache MISS次数 (应该是1次 - P1)
echo "[1] PT Cache MISS 次数:"
grep -c "\[PT_CACHE\].*-> MISS response received" output_test4_dedup_prefetch.txt
echo "   预期: 1次 (P1)"
echo ""

# 2. 统计占位CL HIT次数 (应该是7次 - P2~P8)
echo "[2] PT Cache HIT (占位CL) 次数:"
grep -c "\[DEDUP\].*-> Placeholder HIT" output_test4_dedup_prefetch.txt
echo "   预期: 7次 (P2~P8)"
echo ""

# 3. 统计PTW执行次数 (应该是1次)
echo "[3] PTW执行次数 (预取组初始化):"
grep -c "\[PTW_PREFETCH\].*-> Prefetch mode, spawning" output_test4_dedup_prefetch.txt
echo "   预期: 1次"
echo ""

# 4. 统计Buffer分配
echo "[4] Buffer分配:"
grep "\[DEDUP\].*Main placeholder created" output_test4_dedup_prefetch.txt | head -1
echo "   预期: head_index=0"
echo ""

# 5. 统计预取占位CL创建 (应该是8个)
echo "[5] 预取占位CL创建:"
grep -c "\[DEDUP\] Prefetch placeholder" output_test4_dedup_prefetch.txt
echo "   预期: 8个"
echo ""

# 6. 统计预取组完成
echo "[6] 预取组完成:"
grep "\[PTW_PREFETCH\].*Group.*ALL COMPLETED" output_test4_dedup_prefetch.txt
echo "   预期: 1次, group pending=0"
echo ""

# 7. 统计批量更新PT Cache
echo "[7] 批量更新PT Cache:"
grep "\[PTW_PREFETCH_MONITOR\].*batch update completed" output_test4_dedup_prefetch.txt
echo "   预期: 9 entries (1主+8预取)"
echo ""

# 8. 统计Buffer链表刷新
echo "[8] Buffer链表刷新:"
grep "\[DEDUP_FLUSH\].*Chain flush completed" output_test4_dedup_prefetch.txt
echo "   预期: 8 tasks flushed"
echo ""

# 9. 检查Forwarder接收任务数
echo "[9] Forwarder接收任务数 (从pt_cache_to_fwd_fifo):"
grep -c "pt_cache_to_fwd_fifo.write" output_test4_dedup_prefetch.txt 2>/dev/null || echo "N/A (需检查实际输出)"
echo "   预期: 8个任务 (P1~P8)"
echo ""

echo "=========================================="
echo "Test 4 完成"
echo "=========================================="
