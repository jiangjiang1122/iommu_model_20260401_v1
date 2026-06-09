#!/bin/bash
# 1000请求测试：验证功能正确性和Cache命中率统计

echo "=========================================="
echo "1000请求测试 - 功能验证和Cache命中率统计"
echo "=========================================="
echo ""

# 检查可执行文件
if [ ! -f "./iommu_model" ]; then
    echo "❌ 错误: 找不到iommu_model可执行文件"
    echo "请先执行: make"
    exit 1
fi

# 运行测试（修改NUM_REQUESTS=1000）
echo "[1] 修改测试配置为1000请求..."
cp rp/test_rp_thread.cc rp/test_rp_thread.cc.bak
sed -i 's/const int NUM_REQUESTS = 10000;/const int NUM_REQUESTS = 1000;/' rp/test_rp_thread.cc
sed -i 's/Map 2500 pages for 10000 requests/Map 125 pages for 1000 requests/' rp/test_rp_thread.cc
sed -i 's/for (int p = 0; p < 2500; p++)/for (int p = 0; p < 125; p++)/' rp/test_rp_thread.cc
echo "  ✅ NUM_REQUESTS=1000, pages=125"
echo ""

echo "[2] 编译（如果需要）..."
make -j4 > /dev/null 2>&1
echo "  ✅ 编译完成"
echo ""

echo "[3] 运行1000请求测试..."
timeout 300 ./iommu_model 2>&1 | tee output_1000req_test.txt

echo ""
echo "=========================================="
echo "功能验证分析"
echo "=========================================="
echo ""

# 1. 检查是否正常完成
if grep -q "IOMMU simulation completed" output_1000req_test.txt; then
    echo "✅ [检查1] 仿真正常完成"
else
    echo "❌ [检查1] 仿真未正常完成"
fi
echo ""

# 2. 检查请求发送数量
REQ_SENT=$(grep -c "Sending translation request" output_1000req_test.txt 2>/dev/null || echo "0")
echo "[检查2] 发送请求数: $REQ_SENT"
if [ "$REQ_SENT" -eq 1000 ]; then
    echo "  ✅ 通过 (预期: 1000)"
else
    echo "  ⚠️  警告 (预期: 1000, 实际: $REQ_SENT)"
fi
echo ""

# 3. 检查响应接收数量
RSP_RECV=$(grep -c "Received translation response" output_1000req_test.txt 2>/dev/null || echo "0")
echo "[检查3] 接收响应数: $RSP_RECV"
if [ "$RSP_RECV" -eq 1000 ]; then
    echo "  ✅ 通过 (预期: 1000)"
else
    echo "  ⚠️  警告 (预期: 1000, 实际: $RSP_RECV)"
fi
echo ""

# 4. 检查错误数量
ERROR_COUNT=$(grep -c "ERROR\|FAULT\|error" output_1000req_test.txt 2>/dev/null || echo "0")
echo "[检查4] 错误数: $ERROR_COUNT"
if [ "$ERROR_COUNT" -eq 0 ]; then
    echo "  ✅ 通过 (无错误)"
else
    echo "  ⚠️  警告 (发现 $ERROR_COUNT 个错误)"
    grep "ERROR\|FAULT" output_1000req_test.txt | head -5
fi
echo ""

echo "=========================================="
echo "PT Cache命中率统计"
echo "=========================================="
echo ""

# 5. PT Cache HIT/MISS统计
PT_HIT=$(grep -c "\[PT_CACHE\].*-> HIT response received" output_1000req_test.txt 2>/dev/null || echo "0")
PT_MISS=$(grep -c "\[PT_CACHE\].*-> MISS response received" output_1000req_test.txt 2>/dev/null || echo "0")
PT_TOTAL=$((PT_HIT + PT_MISS))

echo "[统计1] PT Cache访问:"
echo "  HIT:  $PT_HIT"
echo "  MISS: $PT_MISS"
echo "  总计: $PT_TOTAL"

if [ "$PT_TOTAL" -gt 0 ]; then
    PT_HIT_RATE=$(echo "scale=2; $PT_HIT * 100 / $PT_TOTAL" | bc)
    echo "  命中率: ${PT_HIT_RATE}%"
else
    echo "  命中率: N/A (无访问)"
fi
echo ""

# 6. 占位CL HIT统计
PLACEHOLDER_HIT=$(grep -c "\[DEDUP\].*-> Placeholder HIT" output_1000req_test.txt 2>/dev/null || echo "0")
echo "[统计2] 占位CL HIT: $PLACEHOLDER_HIT"
if [ "$PLACEHOLDER_HIT" -gt 0 ]; then
    PLACEHOLDER_RATIO=$(echo "scale=2; $PLACEHOLDER_HIT * 100 / $PT_HIT" | bc 2>/dev/null || echo "0")
    echo "  占PT Cache HIT比例: ${PLACEHOLDER_RATIO}%"
fi
echo ""

# 7. 预取组统计
PREFETCH_GROUPS=$(grep -c "\[PTW_PREFETCH\].*Group.*ALL COMPLETED" output_1000req_test.txt 2>/dev/null || echo "0")
echo "[统计3] 预取组完成数: $PREFETCH_GROUPS"
echo ""

# 8. PTW执行次数
PTW_EXEC=$(grep -c "\[PTW_PREFETCH\].*-> Prefetch mode" output_1000req_test.txt 2>/dev/null || echo "0")
echo "[统计4] PTW执行次数: $PTW_EXEC"
echo ""

# 9. 批量更新统计
BATCH_UPDATES=$(grep -c "\[PTW_PREFETCH_MONITOR\].*batch update completed" output_1000req_test.txt 2>/dev/null || echo "0")
echo "[统计5] 批量更新PT Cache次数: $BATCH_UPDATES"
if [ "$BATCH_UPDATES" -gt 0 ]; then
    echo "  详情:"
    grep "\[PTW_PREFETCH_MONITOR\].*batch update completed" output_1000req_test.txt | head -3
fi
echo ""

# 10. Buffer链表刷新统计
FLUSH_COUNT=$(grep -c "\[DEDUP_FLUSH\].*Chain flush completed" output_1000req_test.txt 2>/dev/null || echo "0")
echo "[统计6] Buffer链表刷新次数: $FLUSH_COUNT"
if [ "$FLUSH_COUNT" -gt 0 ]; then
    FLUSHED_TASKS=$(grep "\[DEDUP_FLUSH\].*Chain flush completed" output_1000req_test.txt | grep -oP '\d+ tasks flushed' | head -1)
    echo "  详情: $FLUSHED_TASKS"
fi
echo ""

echo "=========================================="
echo "DC Cache命中率统计"
echo "=========================================="
echo ""

# 11. DC Cache HIT/MISS统计
DC_HIT=$(grep -c "\[DC Lookup\].*hit=1" output_1000req_test.txt 2>/dev/null || echo "0")
DC_MISS=$(grep -c "\[DC Lookup\].*hit=0" output_1000req_test.txt 2>/dev/null || echo "0")
DC_TOTAL=$((DC_HIT + DC_MISS))

echo "[统计7] DC Cache访问:"
echo "  HIT:  $DC_HIT"
echo "  MISS: $DC_MISS"
echo "  总计: $DC_TOTAL"

if [ "$DC_TOTAL" -gt 0 ]; then
    DC_HIT_RATE=$(echo "scale=2; $DC_HIT * 100 / $DC_TOTAL" | bc)
    echo "  命中率: ${DC_HIT_RATE}%"
else
    echo "  命中率: N/A (无访问)"
fi
echo ""

echo "=========================================="
echo "性能指标"
echo "=========================================="
echo ""

# 12. 仿真时间
SIM_TIME=$(grep "IOMMU simulation completed" output_1000req_test.txt | grep -oP '@\K[0-9.]+' | head -1)
if [ -n "$SIM_TIME" ]; then
    echo "[统计8] 仿真时间: ${SIM_TIME} ns"
    
    # 计算IOPS (假设稳态窗口80%-90%)
    STEADY_START=$(echo "$SIM_TIME * 0.8" | bc)
    STEADY_END=$SIM_TIME
    STEADY_DURATION=$(echo "$STEADY_END - $STEADY_START" | bc)
    if [ "$(echo "$STEADY_DURATION > 0" | bc)" -eq 1 ]; then
        IOPS=$(echo "scale=0; 1000 * 1000000000 / $STEADY_DURATION" | bc)
        echo "[统计9] IOPS (估算): $IOPS"
    fi
else
    echo "[统计8] 仿真时间: N/A"
fi
echo ""

# 13. DDR访问次数
DDR_READS=$(grep -c "DDR read request" output_1000req_test.txt 2>/dev/null || echo "0")
echo "[统计10] DDR读请求: $DDR_READS"
echo ""

echo "=========================================="
echo "测试总结"
echo "=========================================="
echo ""

# 总结
PASS_COUNT=0
TOTAL_CHECKS=4

[ "$REQ_SENT" -eq 1000 ] && PASS_COUNT=$((PASS_COUNT+1))
[ "$RSP_RECV" -eq 1000 ] && PASS_COUNT=$((PASS_COUNT+1))
[ "$ERROR_COUNT" -eq 0 ] && PASS_COUNT=$((PASS_COUNT+1))
[ "$PT_TOTAL" -gt 0 ] && PASS_COUNT=$((PASS_COUNT+1))

echo "功能验证: $PASS_COUNT/$TOTAL_CHECKS 项通过"
echo ""

echo "关键指标:"
echo "  PT Cache命中率: ${PT_HIT_RATE}% (HIT=$PT_HIT, MISS=$PT_MISS)"
echo "  DC Cache命中率: ${DC_HIT_RATE}% (HIT=$DC_HIT, MISS=$DC_MISS)"
echo "  占位CL HIT: $PLACEHOLDER_HIT (占PT Cache HIT ${PLACEHOLDER_RATIO}%)"
echo "  预取组完成: $PREFETCH_GROUPS"
echo "  PTW执行次数: $PTW_EXEC"
echo "  批量更新: $BATCH_UPDATES"
echo "  Buffer刷新: $FLUSH_COUNT"
echo ""

if [ "$PASS_COUNT" -eq "$TOTAL_CHECKS" ]; then
    echo "✅ 1000请求测试通过！"
    echo "   - 功能正常: 1000/1000请求完成"
    echo "   - 无错误: $ERROR_COUNT"
    echo "   - PT Cache命中率: ${PT_HIT_RATE}%"
    exit 0
else
    echo "⚠️  部分检查未通过，请查看详细日志"
    exit 1
fi
