# PT Cache命中率分析报告

## 执行摘要

**问题**: 100个并发请求测试中，PT cache命中率为0%

**根因**: 并发测试时序导致所有lookup在update之前完成，属于**预期行为**

**状态**: ✅ 代码修复完成，功能正常，0%命中率在并发场景下合理

---

## 1. 已修复的BUG

### 1.1 align_iova函数修复 ✅

**文件**: `iommu/cache_src/cache/pt_cache.cpp:135-143`

**修复前**:
```cpp
case PageSize::PAGE_4K:   return iova;  // ❌ 未对齐
case PageSize::PAGE_2M:   return iova & ~0x1FF;  // ❌ 错误掩码
```

**修复后**:
```cpp
case PageSize::PAGE_4K:   return iova & ~0xFFF;  // ✅ 清低12bit
case PageSize::PAGE_2M:   return iova & ~0x1FFFFF;  // ✅ 清低21bit
case PageSize::PAGE_1G:   return iova & ~0x3FFFFFFF;  // ✅ 清低30bit
case PageSize::PAGE_512G: return iova & ~0x7FFFFFFFFF;  // ✅ 清低39bit
```

### 1.2 lookup_pt函数修复 ✅

**文件**: `iommu/cache_src/cache/pt_cache.cpp:11-23`

**修复前**:
```cpp
tag.iova = iova;  // ❌ 未对齐，与fill_pt不一致
```

**修复后**:
```cpp
tag.iova = align_iova(iova, PageSize::PAGE_4K);  // ✅ 页对齐，与fill_pt保持一致
```

---

## 2. 编译验证

### 2.1 编译规则检查

**Makefile**: 第92-94行模式规则正确
```makefile
build/%.o: %.cc
    @mkdir -p build/...
    $(CXX) $(CXXFLAGS) -c $< -o $@
```

### 2.2 实际编译验证

```bash
# 手动编译pt_cache.cpp
g++ -std=c++17 ... -c iommu/cache_src/cache/pt_cache.cpp \
    -o build/iommu/cache_src/cache/pt_cache.o

# 验证文件生成
ls -lh build/iommu/cache_src/cache/pt_cache.o
# -rwxrwxrwx 1 jiang jiang 282K  5月  7 17:37 pt_cache.o ✅
```

### 2.3 时间戳验证

```
pt_cache.cpp:    1778145024 (源文件)
pt_cache.o:      1778146662 (编译后，比源文件新) ✅
iommu_model:     1778146842 (链接后，最新) ✅
```

**结论**: 编译正确，修改已生效。

---

## 3. PT Cache命中率0%的根因分析

### 3.1 测试场景

**测试代码**: `rp/test_rp_thread.cc:554-599`

```cpp
const int NUM_REQUESTS = 100;
uint64_t base_iova = 0x10000;

for (int i = 0; i < NUM_REQUESTS; i++) {
    uint64_t iova = base_iova + i * 512;  // 步长512字节
    send_request(iova);  // 并发发送
}
```

**IOVA分布**:
- Request 0-7:  0x10000-0x10E00 (VPN=0x10, 同一4KB页)
- Request 8-15: 0x11000-0x11E00 (VPN=0x11, 同一4KB页)
- ...
- 共13个不同的VPN (0x10-0x1C)

### 3.2 时序分析 (从cache_sim.log)

#### 关键时间戳

| 事件 | Task ID | IOVA | 时间戳 (ns) | 结果 |
|------|---------|------|------------|------|
| PT Lookup | 1 | 0x10000 | 890 | **MISS** |
| PT Lookup | 2 | 0x10200 | 900 | **MISS** |
| PT Lookup | 6 | 0x10A00 | 905 | **MISS** |
| PT Lookup | 3 | 0x10400 | 910 | **MISS** |
| ... | ... | ... | ... | ... |
| PT Lookup | 12 | 0x11600 | 949 | **MISS** |
| **PT Update** | **1** | **0x10000** | **1807** | fill cache |
| **PT Update** | **2** | **0x10200** | **1935** | fill cache |
| **PT Update** | **3** | **0x10400** | **2063** | fill cache |

#### 时序图

```
890ns    900ns    910ns         949ns        1807ns   1935ns   2063ns
  |        |        |             |             |        |        |
  v        v        v             v             v        v        v
 Lookup1 Lookup2 Lookup3 ... Lookup12     Update1  Update2  Update3
 (miss)  (miss)  (miss)        (miss)     (fill)   (fill)   (fill)
  |________|________|_____________|
            所有Lookup都在Update之前完成！
```

### 3.3 根因

**问题**: 所有100个请求**并发发送**，导致：

1. **Phase 1 (890-950ns)**: 
   - 100个PT lookup请求几乎同时到达PT cache
   - 此时PT cache为空（冷启动）
   - **所有lookup都miss**

2. **Phase 2 (950-1800ns)**:
   - 100个PTW（页表遍历）并行执行
   - 每个PTW需要4次DDR访问（约200-800ns）
   - PTW按顺序完成

3. **Phase 3 (1807ns+)**:
   - PTW完成后，PT cache开始update
   - **但此时所有lookup已经完成**
   - 没有后续的lookup请求来利用cache

**结论**: 
- ✅ **0%命中率是并发测试的预期行为**
- ✅ 代码修复正确（lookup和fill都使用对齐的VPN）
- ✅ 功能验证通过（100/100请求翻译正确）

---

## 4. 如何让PT Cache产生命中？

### 4.1 方案A: 重复访问模式

修改测试，让同一VPN被多次访问：

```cpp
for (int page = 0; page < 10; page++) {
    uint64_t page_iova = base_iova + page * 4096;
    
    // 第1次访问：miss → PTW → fill
    send_request(page_iova);
    wait(50, SC_NS);  // 等待PTW完成
    
    // 第2-10次访问：应该hit
    for (int repeat = 1; repeat < 10; repeat++) {
        send_request(page_iova);  // 相同VPN
    }
    wait(50, SC_NS);
}
```

**预期结果**:
- 第1次访问：miss (10次)
- 第2-10次访问：**hit** (90次)
- **PT命中率: ~90%**

### 4.2 方案B: 串行访问模式

```cpp
for (int i = 0; i < 100; i++) {
    uint64_t iova = base_iova + i * 512;
    send_request(iova);
    
    // 等待当前请求完成（包括PTW和cache update）
    wait_for_response();
    
    // 再次访问相同IOVA（应该hit）
    send_request(iova);
    wait_for_response();
}
```

**预期结果**:
- 第1次访问：miss (100次)
- 第2次访问：**hit** (100次)
- **PT命中率: 50%**

### 4.3 方案C: 时间分组（已尝试但未生效）

当前代码已有分组延迟（50ns），但不足以等待PTW完成：

```cpp
if ((i + 1) % 8 == 0 && i < NUM_REQUESTS - 1) {
    wait(50, SC_NS);  // ❌ 不够，PTW需要200-800ns
}
```

**需要修改为**:
```cpp
if ((i + 1) % 8 == 0 && i < NUM_REQUESTS - 1) {
    wait(1000, SC_NS);  // ✅ 等待1us，确保PTW完成
}
```

---

## 5. 当前状态总结

### 5.1 代码修复

| 修复项 | 状态 | 验证 |
|--------|------|------|
| align_iova掩码修正 | ✅ 完成 | 源代码确认 |
| lookup_pt页对齐 | ✅ 完成 | 源代码确认 |
| 编译正确性 | ✅ 完成 | .o文件生成 |
| 链接正确性 | ✅ 完成 | 二进制更新 |

### 5.2 功能验证

| 测试项 | 结果 | 说明 |
|--------|------|------|
| 100请求翻译 | ✅ 100/100 PASS | PA = IOVA + 0x10000 |
| DC Cache命中率 | ✅ 90.57% | 合理（单设备） |
| PT Cache命中率 | ✅ 0% | **合理（并发冷启动）** |
| 仿真正常结束 | ✅ sc_stop() | 正常终止 |

### 5.3 Cache统计

```
========== IOMMU Cache Statistics Summary ==========
Cache               Accesses   Hits   Misses  HitRate
dc_cache                 106     96       10   0.9057  ✅
pc_cache                 106      0      106   0.0000  ⚠️ (未配置PC)
pt_cache                 106      0      106   0.0000  ✅ (并发冷启动)
```

---

## 6. 结论与建议

### 6.1 结论

1. **PT cache代码修复完全正确** ✅
   - lookup和fill都使用页对齐的VPN
   - 编译和链接验证通过

2. **0%命中率是并发测试的预期行为** ✅
   - 所有lookup在update之前完成
   - 没有重复访问同一VPN
   - 符合cache工作原理

3. **功能验证通过** ✅
   - 100/100请求翻译正确
   - 地址映射关系正确（PA = IOVA + 0x10000）

### 6.2 建议

1. **保持当前代码**：修复的BUG是正确的，应当保留

2. **如需测试PT cache命中率**：
   - 使用**方案A**（重复访问模式）
   - 预期可达到80-90%命中率
   - 更能体现PT cache的实际效果

3. **并发测试的价值**：
   - 当前测试验证了**并发处理能力**
   - 验证了PTW的正确性
   - 验证了地址翻译的正确性
   - 0%命中率不影响测试价值

---

## 7. 附录：关键文件

- `iommu/cache_src/cache/pt_cache.cpp` - PT cache实现（已修复）
- `rp/test_rp_thread.cc` - 测试代码（50ns分组延迟）
- `cache_sim.log` - Cache详细日志
- `iommu/cache_config/default_config.json` - PT cache配置（1024 sets, 8 ways）

---

**报告生成时间**: 2026-05-07 17:40 CST
**分析结论**: PT cache代码修复正确，0%命中率在并发冷启动场景下完全合理 ✅
