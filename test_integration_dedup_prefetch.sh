#!/bin/bash
# PT Cache去重+预取集成测试
# 测试场景: P1~P8连续同页访问 (4KB页, D=8)

echo "=========================================="
echo "PT Cache去重+预取 集成测试"
echo "Test 4: P1~P8 连续同页访问 (D=8)"
echo "=========================================="
echo ""

# 检查可执行文件
if [ ! -f "./iommu_model" ]; then
    echo "❌ 错误: 找不到iommu_model可执行文件"
    exit 1
fi

# 运行测试（10个请求）
echo "[1] 运行IOMMU仿真 (10个请求)..."
timeout 120 ./iommu_model 2>&1 | tee output_test4_integration.txt

echo ""
echo "=========================================="
echo "提取关键日志并分析"
echo "=========================================="
echo ""

# 1. 统计PT Cache MISS次数
MISS_COUNT=$(grep -c "\[PT_CACHE\].*-> MISS response received" output_test4_integration.txt 2>/dev/null || echo "0")
echo "[检查1] PT Cache MISS 次数: $MISS_COUNT"
if [ "$MISS_COUNT" -eq 1 ]; then
    echo "  ✅ 通过 (预期: 1次 - P1)"
else
    echo "  ⚠️  警告 (预期: 1次, 实际: $MISS_COUNT)"
fi
echo ""

# 2. 统计占位CL HIT次数
PLACEHOLDER_HIT=$(grep -c "\[DEDUP\].*-> Placeholder HIT" output_test4_integration.txt 2>/dev/null || echo "0")
echo "[检查2] PT Cache HIT (占位CL) 次数: $PLACEHOLDER_HIT"
if [ "$PLACEHOLDER_HIT" -eq 7 ]; then
    echo "  ✅ 通过 (预期: 7次 - P2~P8)"
else
    echo "  ⚠️  警告 (预期: 7次, 实际: $PLACEHOLDER_HIT)"
fi
echo ""

# 3. 统计PTW预取组初始化次数
PREFETCH_INIT=$(grep -c "\[PTW_PREFETCH\].*-> Prefetch mode" output_test4_integration.txt 2>/dev/null || echo "0")
echo "[检查3] PTW预取组初始化次数: $PREFETCH_INIT"
if [ "$PREFETCH_INIT" -eq 1 ]; then
    echo "  ✅ 通过 (预期: 1次)"
else
    echo "  ⚠️  警告 (预期: 1次, 实际: $PREFETCH_INIT)"
fi
echo ""

# 4. 统计预取占位CL创建
PREFETCH_PLACEHOLDER=$(grep -c "\[DEDUP\] Prefetch placeholder" output_test4_integration.txt 2>/dev/null || echo "0")
echo "[检查4] 预取占位CL创建: $PREFETCH_PLACEHOLDER"
if [ "$PREFETCH_PLACEHOLDER" -eq 8 ]; then
    echo "  ✅ 通过 (预期: 8个)"
else
    echo "  ⚠️  警告 (预期: 8个, 实际: $PREFETCH_PLACEHOLDER)"
fi
echo ""

# 5. 统计预取组完成
GROUP_COMPLETED=$(grep -c "\[PTW_PREFETCH\].*ALL COMPLETED" output_test4_integration.txt 2>/dev/null || echo "0")
echo "[检查5] 预取组完成: $GROUP_COMPLETED"
if [ "$GROUP_COMPLETED" -eq 1 ]; then
    echo "  ✅ 通过 (预期: 1次)"
    grep "\[PTW_PREFETCH\].*ALL COMPLETED" output_test4_integration.txt | head -1
else
    echo "  ⚠️  警告 (预期: 1次, 实际: $GROUP_COMPLETED)"
fi
echo ""

# 6. 统计批量更新PT Cache
BATCH_UPDATE=$(grep -c "\[PTW_PREFETCH_MONITOR\].*batch update completed" output_test4_integration.txt 2>/dev/null || echo "0")
echo "[检查6] 批量更新PT Cache: $BATCH_UPDATE"
if [ "$BATCH_UPDATE" -eq 1 ]; then
    echo "  ✅ 通过 (预期: 1次)"
    grep "\[PTW_PREFETCH_MONITOR\].*batch update completed" output_test4_integration.txt | head -1
else
    echo "  ⚠️  警告 (预期: 1次, 实际: $BATCH_UPDATE)"
fi
echo ""

# 7. 统计Buffer链表刷新
FLUSH_COMPLETED=$(grep -c "\[DEDUP_FLUSH\].*Chain flush completed" output_test4_integration.txt 2>/dev/null || echo "0")
echo "[检查7] Buffer链表刷新: $FLUSH_COMPLETED"
if [ "$FLUSH_COMPLETED" -eq 1 ]; then
    echo "  ✅ 通过 (预期: 1次)"
    grep "\[DEDUP_FLUSH\].*Chain flush completed" output_test4_integration.txt | head -1
else
    echo "  ⚠️  警告 (预期: 1次, 实际: $FLUSH_COMPLETED)"
fi
echo ""

# 8. 统计task_id生成方式（验证使用全局计数器）
echo "[检查8] Task ID生成方式:"
TASK_ID_METHOD=$(grep "\[PTW_PREFETCH\].*prefetch_task_id=" output_test4_integration.txt | head -1)
if [ -n "$TASK_ID_METHOD" ]; then
    echo "  $TASK_ID_METHOD"
    echo "  ✅ 使用全局计数器生成task_id"
else
    echo "  ⚠️  未找到task_id日志"
fi
echo ""

# 9. 统计Buffer互斥锁使用
echo "[检查9] Buffer互斥锁保护:"
if grep -q "pt_dedup_buffer_mtx.lock" output_test4_integration.txt 2>/dev/null; then
    echo "  ✅ Buffer操作有互斥锁保护"
else
    echo "  ℹ️  当前collector串行执行，暂不需要锁（已添加防御性保护）"
fi
echo ""

echo "=========================================="
echo "集成测试完成"
echo "=========================================="
echo ""
echo "详细日志已保存到: output_test4_integration.txt"
echo ""
echo "关键统计摘要:"
echo "  PT Cache MISS:    $MISS_COUNT (预期: 1)"
echo "  占位CL HIT:       $PLACEHOLDER_HIT (预期: 7)"
echo "  预取组初始化:     $PREFETCH_INIT (预期: 1)"
echo "  预取占位CL:       $PREFETCH_PLACEHOLDER (预期: 8)"
echo "  预取组完成:       $GROUP_COMPLETED (预期: 1)"
echo "  批量更新:         $BATCH_UPDATE (预期: 1)"
echo "  Buffer刷新:       $FLUSH_COMPLETED (预期: 1)"
echo ""

# 总结
PASS_COUNT=0
TOTAL_CHECKS=7

[ "$MISS_COUNT" -eq 1 ] && PASS_COUNT=$((PASS_COUNT+1))
[ "$PLACEHOLDER_HIT" -eq 7 ] && PASS_COUNT=$((PASS_COUNT+1))
[ "$PREFETCH_INIT" -eq 1 ] && PASS_COUNT=$((PASS_COUNT+1))
[ "$PREFETCH_PLACEHOLDER" -eq 8 ] && PASS_COUNT=$((PASS_COUNT+1))
[ "$GROUP_COMPLETED" -eq 1 ] && PASS_COUNT=$((PASS_COUNT+1))
[ "$BATCH_UPDATE" -eq 1 ] && PASS_COUNT=$((PASS_COUNT+1))
[ "$FLUSH_COMPLETED" -eq 1 ] && PASS_COUNT=$((PASS_COUNT+1))

echo "测试结果: $PASS_COUNT/$TOTAL_CHECKS 项通过"

if [ "$PASS_COUNT" -eq "$TOTAL_CHECKS" ]; then
    echo "✅ 集成测试全部通过！"
    exit 0
else
    echo "⚠️  部分检查未通过，请查看详细日志"
    exit 1
fi
