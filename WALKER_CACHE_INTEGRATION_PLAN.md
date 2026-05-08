# Walker Cache集成方案

## 一、任务概述

在PTW（Page Table Walker）中集成Walker Cache功能，缓存页表遍历过程中的中间地址转换结果，加速后续的页表遍历。

### 核心目标
1. **开关控制**：支持整体开启/关闭Walker Cache功能
2. **Lookup优化**：PTW请求先查询Walker Cache，命中后从命中层级继续walk
3. **Update优化**：PTW完成后一次性更新所有中间结果到Walker Cache

---

## 二、架构理解

### 2.1 Walker Cache结构（来自cache_src）

Walker Cache包含**3个独立子表**：

| 子表 | 名称 | 相联度 | 缓存内容 |
|------|------|--------|----------|
| PTWc_1 | PTW Cache 1 | 直接映射(1-way) | 第一级页表中间结果(VPN[3]级) |
| PTWc_2 | PTW Cache 2 | 2-way组相连 | 第二级页表中间结果(VPN[2]级) |
| PTWc_3 | PTW Cache 3 | 4-way组相连 | 第三级页表中间结果(VPN[1]级) |

### 2.2 Walker Cache Line数据结构

```cpp
struct WalkerTag {
    gscid_t    gscid;          // GSCID
    pscid_t    pscid;          // PSCID
    iova_t     va_segment;     // VPN分段（根据level不同）
    uint8_t    level;          // 层级(1/2/3)
    bool       va_pa_flag;     // VA/PA标志位
    bool       stage_flag;     // 单阶段/两阶段标志
    bool       sv48_flag;      // Sv39/Sv48标志
    bool       x4_mode_flag;   // Sv39x4/Sv48x4标志
};

struct WalkerData {
    ppn_t      next_ppn;       // 下一级页表基址PPN
    struct {
        uint32_t valid:1;           // 有效位
        uint32_t va_pa_flag:1;      // VA/PA标志
        uint32_t stage_flag:1;      // 翻译阶段标志
        uint32_t sv48_flag:1;       // Sv39/Sv48标志
        uint32_t x4_mode_flag:1;    // x4模式标志
    } reserved;
};
```

### 2.3 地址分段规则

**Sv39模式**（3级页表）：
```
IOVA[63:39] = Sign extension
IOVA[38:30] = VPN[2] (9 bits) → PTWc_3
IOVA[29:21] = VPN[1] (9 bits) → PTWc_2
IOVA[20:12] = VPN[0] (9 bits) → PTWc_1
IOVA[11:0]  = Page Offset
```

**Sv48模式**（4级页表）：
```
IOVA[63:48] = Sign extension
IOVA[47:39] = VPN[3] (9 bits) → PTWc_3 (最高级)
IOVA[38:30] = VPN[2] (9 bits) → PTWc_2
IOVA[29:21] = VPN[1] (9 bits) → PTWc_1
IOVA[20:12] = VPN[0] (9 bits) → (不使用)
IOVA[11:0]  = Page Offset
```

**Sv39x4/Sv48x4模式**（GPA地址）：
- 与Sv39/Sv48类似，但x4_mode_flag=true
- 实际使用长度不固定，但40bit足够

### 2.4 CacheSubsystem接口

Walker Cache已通过CacheSubsystem暴露FIFO接口：

```cpp
// Lookup请求/响应
sc_fifo<CacheMessage> walker_request_fifo;      // PTW → Walker Cache
sc_fifo<CacheMessage> walker_response_fifo;     // Walker Cache → PTW

// Update请求
sc_fifo<CacheMessage> walker_update_fifo;       // PTW → Walker Cache
```

---

## 三、修改方案设计

### 3.1 开关控制

#### 方案A：配置文件开关（推荐）

在`iommu/cache_config/default_config.json`中添加：

```json
{
  "walker_cache": {
    "enabled": true,
    "ptw_c1": { "sets": 64, "ways": 1, "latency_cycles": 1 },
    "ptw_c2": { "sets": 128, "ways": 2, "latency_cycles": 2 },
    "ptw_c3": { "sets": 256, "ways": 4, "latency_cycles": 2 }
  }
}
```

#### 方案B：编译期宏开关（备选）

在`iommu_perf_params.hh`中定义：

```cpp
#define WALKER_CACHE_ENABLED 1  // 1=启用, 0=禁用
```

**推荐方案A**：支持运行时动态配置，更灵活。

---

### 3.2 PTW请求线程修改（ptw_req_thread）

#### 当前流程（无Walker Cache）

```cpp
void iommu_top::ptw_req_thread() {
    // 1. 读取任务
    iommu_task_t* task = pt_cache_to_ptw_fifo.read();
    
    // 2. 初始化walk context
    extract_vpn(...);
    task->walk_ctx.level = LEVELS - 1;
    task->walk_ctx.base_addr = iosatp.PPN * PAGESIZE;
    
    // 3. 直接发送DDR请求（第一级页表）
    uint64_t pte_addr = base_addr + vpn[level] * PTESIZE;
    ddr_req_entry_t req;
    req.addr = pte_addr;
    ptw_req_ddr_fifo.write(req);
}
```

#### 新流程（有Walker Cache）

```cpp
void iommu_top::ptw_req_thread() {
    // 1. 读取任务
    iommu_task_t* task = pt_cache_to_ptw_fifo.read();
    
    // 2. 检查Walker Cache开关
    bool walker_cache_enabled = cfg.walker_cache.enabled; // 从配置读取
    
    if (!walker_cache_enabled) {
        // 关闭：走原流程
        send_first_ddr_request(task);
        return;
    }
    
    // 3. 构造Walker Cache Lookup请求
    iommu::CacheMessage req;
    req.msg_type           = iommu::CacheMsgType::WALKER_LOOKUP;
    req.task_id            = task->task_id;
    
    // 从task解析路由键
    req.gscid              = task->GSCID;
    req.pscid              = task->PSCID;
    req.iova               = task->iova;
    
    // 解析翻译模式标志位
    req.walker_addr_is_va  = (task->iosatp.MODE != IOSATP_Bare);  // VA还是PA
    req.walker_from_two_stage = (task->iohgatp.MODE != IOHGATP_Bare);  // 两阶段
    req.walker_sv48        = (task->iosatp.MODE == IOSATP_Sv48);  // Sv39/Sv48
    req.walker_x4_mode     = (task->iohgatp.MODE == IOHGATP_Sv39x4 || 
                              task->iohgatp.MODE == IOHGATP_Sv48x4);  // x4模式
    
    // 4. 发送Lookup请求到Walker Cache
    cache_sub.walker_request_fifo.write(req);
    
    // 5. 阻塞等待响应
    iommu::CacheMessage resp = cache_sub.walker_response_fifo.read();
    
    // 6. 处理命中/未命中
    if (resp.hit) {
        // ===== 命中：从命中层级继续walk =====
        uint8_t hit_level = resp.walker_level;  // 例如：level=2 (PTWc_2命中)
        ppn_t next_ppn = resp.walker_data.next_ppn;  // 下一级页表基址
        
        printf("[PTW_REQ] task_id=%u, Walker Cache HIT at level=%d, next_ppn=0x%lx\n",
               task->task_id, hit_level, next_ppn);
        
        // 初始化walk context（从命中层级开始）
        task->walk_ctx.level = hit_level - 1;  // 从下一层级开始
        task->walk_ctx.base_addr = next_ppn * PAGESIZE;
        task->walk_ctx.walk_phase = PTW_VS_WALK;
        task->walk_ctx.ddr_read_count = 0;  // 从命中点开始计数
        
        // 发送下一级DDR请求
        uint64_t pte_addr = task->walk_ctx.base_addr + 
                           task->walk_ctx.vpn[task->walk_ctx.level] * task->walk_ctx.ptesize;
        task->walk_ctx.read_addr = pte_addr;
        task->walk_ctx.read_size = task->walk_ctx.ptesize;
        task->walk_ctx.ddr_read_count = 1;
        
        ddr_req_entry_t ddr_req;
        ddr_req.task_id = task->task_id;
        ddr_req.addr = pte_addr;
        ddr_req.size = task->walk_ctx.ptesize;
        ddr_req.is_write = false;
        ptw_req_ddr_fifo.write(ddr_req);
        
    } else {
        // ===== 未命中：走完整walk流程 =====
        printf("[PTW_REQ] task_id=%u, Walker Cache MISS, starting full walk\n",
               task->task_id);
        
        // 原流程：初始化完整walk
        send_first_ddr_request(task);
    }
}
```

#### 关键逻辑说明

**1. 路由键解析规则**：

| 字段 | 来源 | 逻辑 |
|------|------|------|
| gscid | task->GSCID | 直接使用 |
| pscid | task->PSCID | 直接使用 |
| iova | task->iova | 原始IOVA |
| walker_addr_is_va | iosatp.MODE | != Bare则为true |
| walker_from_two_stage | iohgatp.MODE | != Bare则为true |
| walker_sv48 | iosatp.MODE | == Sv48则为true |
| walker_x4_mode | iohgatp.MODE | == Sv39x4/Sv48x4则为true |

**2. 命中后的处理**：

```
假设Sv39模式，3级页表（level=2,1,0）：
- PTWc_3缓存VPN[2] → 如果命中，next_ppn指向level=1的页表基址
- PTWc_2缓存VPN[1] → 如果命中，next_ppn指向level=0的页表基址
- PTWc_1缓存VPN[0] → 如果命中，next_ppn指向最终数据页

命中level=2 (PTWc_3)：
  → 跳过level=2的DDR访问
  → 从level=1开始walk
  → 需要访问level=1和level=0（2次DDR）

命中level=1 (PTWc_2)：
  → 跳过level=2和level=1的DDR访问
  → 从level=0开始walk
  → 需要访问level=0（1次DDR）

命中level=0 (PTWc_1)：
  → 跳过所有页表访问
  → next_ppn就是最终数据页PPN
  → 0次DDR访问，直接计算PA
```

**3. 未命中处理**：

完全走原有流程，不影响正确性。

---

### 3.3 PTW响应线程修改（ptw_rsp_thread）

#### 当前流程（无Walker Cache）

```cpp
void iommu_top::ptw_rsp_thread() {
    // 1. 读取DDR响应
    ddr_rsp_entry_t rsp = ptw_rsp_ddr_fifo.read();
    
    // 2. 解析PTE
    spte_t pte;
    memcpy(&pte, rsp.data, PTESIZE);
    
    // 3. 多级walk循环
    while (!walk_complete && !walk_fault) {
        if (leaf_node) {
            // 叶子节点：计算GPA/PA
            walk_complete = true;
        } else {
            // 非叶子节点：发送下一级DDR请求
            send_next_ddr_request();
        }
    }
    
    // 4. Walk完成后更新PT Cache
    cache_sub.pt_update_fifo.write(pt_update_req);
}
```

#### 新流程（有Walker Cache）

```cpp
void iommu_top::ptw_rsp_thread() {
    // 新增：保存中间结果
    struct WalkerCacheEntry {
        uint8_t  level;
        ppn_t    next_ppn;
        bool     valid;
    };
    
    std::map<uint32_t, std::vector<WalkerCacheEntry>> walker_cache_updates;
    
    // ... 原有流程 ...
    
    switch (task->walk_ctx.walk_phase) {
    case PTW_VS_WALK: {
        // 解析PTE
        spte_t pte;
        memcpy(&pte, rsp.data, PTESIZE);
        
        if (non_leaf_node) {
            // ===== 非叶子节点：记录中间结果 =====
            ppn_t next_ppn = pte.PPN;  // 下一级页表基址
            uint8_t current_level = task->walk_ctx.level;
            
            // 保存到临时结构（用于最后update）
            walker_cache_updates[task->task_id].push_back({
                current_level, next_ppn, true
            });
            
            printf("[PTW_RSP] task_id=%u, VS_WALK non-leaf: level=%d, next_ppn=0x%lx (saved for walker cache)\n",
                   task->task_id, current_level, next_ppn);
            
            // 发送下一级DDR请求
            send_next_ddr_request();
            
        } else if (leaf_node) {
            // ===== 叶子节点：walk完成 =====
            walk_complete = true;
        }
        break;
    }
    
    // ... 其他case (GS_IMPLICIT, GS_EXPLICIT, AD_UPDATE) ...
    
    } // end switch
    
    // ========== Walk完成后的处理 ==========
    if (walk_complete) {
        // 1. 更新PT Cache（原有逻辑）
        cache_sub.pt_update_fifo.write(task_to_pt_update(task));
        
        // 2. 更新Walker Cache（新增逻辑）
        if (walker_cache_enabled) {
            update_walker_cache(task, walker_cache_updates);
        }
        
        // 3. 路由到forwarder（原有逻辑）
        pt_cache_to_fwd_fifo.write(task);
    }
}
```

#### Walker Cache Update逻辑

```cpp
void iommu_top::update_walker_cache(iommu_task_t* task,
    std::map<uint32_t, std::vector<WalkerCacheEntry>>& updates) {
    
    auto it = updates.find(task->task_id);
    if (it == updates.end() || it->second.empty()) {
        return;  // 没有中间结果
    }
    
    // 构造Walker Cache Update请求
    iommu::CacheMessage req;
    req.msg_type           = iommu::CacheMsgType::WALKER_UPDATE;
    req.task_id            = task->task_id;
    req.gscid              = task->GSCID;
    req.pscid              = task->PSCID;
    req.iova               = task->iova;
    
    // 路由键标志位（与lookup保持一致）
    req.walker_addr_is_va  = (task->iosatp.MODE != IOSATP_Bare);
    req.walker_from_two_stage = (task->iohgatp.MODE != IOHGATP_Bare);
    req.walker_sv48        = (task->iosatp.MODE == IOSATP_Sv48);
    req.walker_x4_mode     = (task->iohgatp.MODE == IOHGATP_Sv39x4 || 
                              task->iohgatp.MODE == IOHGATP_Sv48x4);
    
    // 根据保存的中间结果构造三级数据
    req.walker_update_kind = iommu::WalkerUpdateKind::PTWC_1_2_3;  // 同时更新三级
    
    // 初始化默认值（valid=false）
    req.walker_data_ptwc1 = iommu::make_walker_data(0, false, 
        req.walker_addr_is_va, req.walker_from_two_stage,
        req.walker_sv48, req.walker_x4_mode);
    req.walker_data_ptwc2 = iommu::make_walker_data(0, false,
        req.walker_addr_is_va, req.walker_from_two_stage,
        req.walker_sv48, req.walker_x4_mode);
    req.walker_data_ptwc3 = iommu::make_walker_data(0, false,
        req.walker_addr_is_va, req.walker_from_two_stage,
        req.walker_sv48, req.walker_x4_mode);
    
    // 填充实际命中的层级
    for (const auto& entry : it->second) {
        switch (entry.level) {
        case 0:  // PTWc_1
            req.walker_data_ptwc1 = iommu::make_walker_data(
                entry.next_ppn, true,
                req.walker_addr_is_va, req.walker_from_two_stage,
                req.walker_sv48, req.walker_x4_mode);
            break;
        case 1:  // PTWc_2
            req.walker_data_ptwc2 = iommu::make_walker_data(
                entry.next_ppn, true,
                req.walker_addr_is_va, req.walker_from_two_stage,
                req.walker_sv48, req.walker_x4_mode);
            break;
        case 2:  // PTWc_3 (Sv39最高级 / Sv48次高级)
            req.walker_data_ptwc3 = iommu::make_walker_data(
                entry.next_ppn, true,
                req.walker_addr_is_va, req.walker_from_two_stage,
                req.walker_sv48, req.walker_x4_mode);
            break;
        case 3:  // PTWc_3 (Sv48最高级)
            req.walker_data_ptwc3 = iommu::make_walker_data(
                entry.next_ppn, true,
                req.walker_addr_is_va, req.walker_from_two_stage,
                req.walker_sv48, req.walker_x4_mode);
            break;
        }
    }
    
    // 发送到walker_update_fifo
    cache_sub.walker_update_fifo.write(req);
    
    printf("[PTW_RSP] task_id=%u -> Walker Cache UPDATE (levels:", task->task_id);
    for (const auto& entry : it->second) {
        printf(" %d", entry.level);
    }
    printf(")\n");
}
```

---

### 3.4 数据结构修改

#### 3.4.1 新增配置结构

**文件**: `iommu/cache_config/default_config.json`

```json
{
  "walker_cache": {
    "enabled": true,
    "ptw_c1": {
      "sets": 64,
      "ways": 1,
      "latency_cycles": 1,
      "write_latency_cycles": 1
    },
    "ptw_c2": {
      "sets": 128,
      "ways": 2,
      "latency_cycles": 2,
      "write_latency_cycles": 2
    },
    "ptw_c3": {
      "sets": 256,
      "ways": 4,
      "latency_cycles": 2,
      "write_latency_cycles": 2
    }
  }
}
```

#### 3.4.2 GlobalConfig扩展

**文件**: `iommu/cache_src/common/json_config.h`

```cpp
struct GlobalConfig {
    // ... 现有字段 ...
    
    // Walker Cache配置
    struct {
        bool enabled;
        CacheConfig ptw_c1;
        CacheConfig ptw_c2;
        CacheConfig ptw_c3;
    } walker_cache;
};
```

---

### 3.5 时序分析

#### 3.5.1 命中场景时序

```
Timeline (假设Sv39, PTWc_2命中):

PTW_REQ Thread:
  t=0ns:    读取task
  t=5ns:    发送walker_request_fifo (Walker Lookup)
  
Walker Cache:
  t=5ns:    接收请求
  t=7ns:    查询PTWc_3 (miss)
  t=9ns:    查询PTWc_2 (hit!)
  t=10ns:   发送walker_response_fifo (hit, level=1, next_ppn=0x12345)
  
PTW_REQ Thread:
  t=10ns:   接收响应
  t=12ns:   初始化walk context (level=0)
  t=15ns:   发送DDR请求 (level=0页表)
  
DDR:
  t=15ns:   接收请求
  t=215ns:  返回响应 (200ns DDR latency)
  
PTW_RSP Thread:
  t=215ns:  解析PTE (叶子节点)
  t=220ns:  计算PA
  t=225ns:  更新PT Cache + Walker Cache
  t=230ns:  发送到forwarder

总延迟: 230ns
DDR访问次数: 1次 (原需3次)
加速比: 3x
```

#### 3.5.2 未命中场景时序

```
Timeline (Walker Cache未命中):

PTW_REQ Thread:
  t=0ns:    读取task
  t=5ns:    发送walker_request_fifo
  t=15ns:   接收响应 (miss)
  t=18ns:   发送第一级DDR请求

DDR:
  t=18ns:   level=2请求
  t=218ns:  level=2响应

PTW_RSP Thread:
  t=218ns:  解析PTE, 发送level=1 DDR请求

DDR:
  t=220ns:  level=1请求
  t=420ns:  level=1响应

PTW_RSP Thread:
  t=420ns:  解析PTE, 发送level=0 DDR请求

DDR:
  t=422ns:  level=0请求
  t=622ns:  level=0响应

PTW_RSP Thread:
  t=622ns:  解析PTE (叶子节点)
  t=630ns:  更新PT Cache + Walker Cache

总延迟: 630ns
DDR访问次数: 3次
额外开销: 15ns (Walker Lookup miss latency)
```

---

## 四、修改文件清单

### 4.1 核心修改文件

| 文件 | 修改内容 | 预估行数 |
|------|----------|----------|
| `iommu/iommu_perf_model/iommu_perf_ptw.cc` | ptw_req_thread添加Walker Lookup逻辑 | +80行 |
| `iommu/iommu_perf_model/iommu_perf_ptw.cc` | ptw_rsp_thread添加中间结果保存和Update逻辑 | +100行 |
| `iommu/iommu_top.hh` | 新增walker_cache_enabled成员变量 | +5行 |
| `iommu/iommu_top.hh` | 新增update_walker_cache辅助函数声明 | +3行 |
| `iommu/cache_config/default_config.json` | 添加walker_cache配置段 | +20行 |

### 4.2 配置文件修改

| 文件 | 修改内容 |
|------|----------|
| `iommu/cache_src/common/json_config.h` | GlobalConfig添加walker_cache结构 |
| `iommu/cache_src/common/json_config.cpp` | 解析walker_cache配置 |

### 4.3 无需修改的文件

- ✅ `iommu/cache_src/cache/walker_cache.cpp` - 已实现完整
- ✅ `iommu/cache_src/cache/walker_cache.h` - 接口已定义
- ✅ `iommu/cache_src/subsystem/cache_subsystem.cpp` - FIFO已暴露
- ✅ `iommu/cache_src/common/types.h` - CacheMessage/WalkerData已定义

---

## 五、实现步骤

### Step 1: 配置文件扩展（1小时）

1. 修改`default_config.json`添加walker_cache配置
2. 修改`json_config.h/cpp`解析配置
3. 测试配置加载

### Step 2: PTW请求线程修改（3小时）

1. 在`ptw_req_thread`中添加Walker Cache开关检查
2. 构造`CacheMessage`并发送到`walker_request_fifo`
3. 阻塞读取`walker_response_fifo`
4. 处理命中/未命中分支
5. 单元测试：验证命中后的walk初始化正确

### Step 3: PTW响应线程修改（4小时）

1. 在`ptw_rsp_thread`中添加中间结果保存逻辑
2. 在walk完成后调用`update_walker_cache`
3. 实现`update_walker_cache`函数
4. 构造三级WalkerData并写入`walker_update_fifo`
5. 单元测试：验证Update请求格式正确

### Step 4: 集成测试（2小时）

1. 测试开关关闭场景（原流程不变）
2. 测试开关开启+全未命中场景
3. 测试开关开启+部分命中场景
4. 测试开关开启+全部命中场景
5. 对比DDR访问次数和总延迟

### Step 5: 回归测试（1小时）

1. 运行100请求测试
2. 验证地址翻译正确性
3. 检查Cache命中率统计
4. Git备份

---

## 六、风险与应对

### 6.1 潜在风险

| 风险 | 影响 | 应对措施 |
|------|------|----------|
| Walker Cache配置错误 | 功能异常 | 默认配置已验证，提供配置文件示例 |
| Lookup阻塞导致死锁 | 系统挂起 | walker_response_fifo depth设为足够大(≥16) |
| 中间结果保存内存泄漏 | 内存耗尽 | walk完成后立即清理map |
| 路由键解析错误 | Tag不匹配 | 与task_to_pt_request保持一致的解析逻辑 |
| Update请求格式错误 | Cache填充失败 | 使用make_walker_data辅助函数 |

### 6.2 调试策略

1. **第一阶段**：关闭Walker Cache，验证原流程不受影响
2. **第二阶段**：开启Walker Cache，强制所有lookup miss，验证update逻辑
3. **第三阶段**：注入预填充数据，强制lookup hit，验证命中逻辑
4. **第四阶段**：正常运行，观察命中率

---

## 七、预期效果

### 7.1 性能提升

**场景1：重复访问相同IOVA**
- 首次访问：3次DDR (600ns)
- 第二次访问：0次DDR (命中PTWc_1/2/3)，~15ns
- **加速比: 40x**

**场景2：访问同一VPN的不同地址**
- 首次访问：3次DDR
- 第二次访问：1次DDR (PTWc_3/2命中)，~220ns
- **加速比: 2.7x**

**场景3：随机访问（冷启动）**
- 首次访问：3次DDR + 15ns lookup = 615ns
- **额外开销: 2.5%**

### 7.2 Cache命中率预估

| 测试场景 | 预估命中率 | 说明 |
|----------|-----------|------|
| 重复访问 | 80-90% | 相同IOVA重复访问 |
| 顺序扫描 | 50-70% | 同一页表内的连续访问 |
| 随机访问 | 10-30% | 冷启动后逐渐提升 |
| 100请求测试 | 0-5% | 并发首次访问（与PT cache类似） |

---

## 八、后续优化方向

1. **预取机制**：根据访问模式预取相邻页表项
2. **自适应配置**：根据命中率动态调整cache大小
3. **多级预热**：系统启动时预加载常用页表
4. **失效优化**：IOTINVAL.VMA时精确失效相关entry
5. **统计增强**：记录各级子表的独立命中率

---

## 九、关键注意事项

### 9.1 路由键一致性

**Lookup和Update必须使用完全相同的路由键**：

```cpp
// Lookup时
req.gscid = task->GSCID;
req.pscid = task->PSCID;
req.iova = task->iova;
req.walker_addr_is_va = (task->iosatp.MODE != IOSATP_Bare);
req.walker_from_two_stage = (task->iohgatp.MODE != IOHGATP_Bare);
req.walker_sv48 = (task->iosatp.MODE == IOSATP_Sv48);
req.walker_x4_mode = (task->iohgatp.MODE == IOHGATP_Sv39x4 || 
                      task->iohgatp.MODE == IOHGATP_Sv48x4);

// Update时必须完全相同！
req.gscid = task->GSCID;  // ✅ 相同
req.pscid = task->PSCID;  // ✅ 相同
req.iova = task->iova;    // ✅ 相同
req.walker_addr_is_va = ...;  // ✅ 相同
// ...
```

### 9.2 Level映射规则

| PTWc子表 | Level | VPN分段 | 说明 |
|----------|-------|---------|------|
| PTWc_3 | 2 (Sv39) / 3 (Sv48) | VPN[2]/VPN[3] | 最高级页表 |
| PTWc_2 | 1 (Sv39) / 2 (Sv48) | VPN[1]/VPN[2] | 中间级页表 |
| PTWc_1 | 0 (Sv39) / 1 (Sv48) | VPN[0]/VPN[1] | 最低级页表 |

**注意**：Sv39和Sv48的level编号不同！

### 9.3 并发安全

```cpp
// 中间结果保存需要线程安全
std::map<uint32_t, std::vector<WalkerCacheEntry>> walker_cache_updates;
sc_mutex walker_cache_updates_mtx;  // 保护map访问

// 在ptw_rsp_thread中
walker_cache_updates_mtx.lock();
walker_cache_updates[task->task_id].push_back(entry);
walker_cache_updates_mtx.unlock();

// walk完成后
walker_cache_updates_mtx.lock();
update_walker_cache(task, walker_cache_updates);
walker_cache_updates.erase(task->task_id);  // 清理
walker_cache_updates_mtx.unlock();
```

---

## 十、总结

本方案通过以下步骤在PTW中集成Walker Cache：

1. ✅ **开关控制**：配置文件控制，支持动态开启/关闭
2. ✅ **Lookup优化**：PTW请求先查Walker Cache，命中后跳过已缓存层级
3. ✅ **Update优化**：walk完成后一次性更新所有中间结果
4. ✅ **正确性保证**：未命中时走原流程，不影响功能
5. ✅ **性能提升**：重复访问场景可达40x加速

**预估工作量**：约11小时（开发8h + 测试3h）

**风险等级**：低（开关控制，可随时回退）

**下一步**：评审方案 → 开始Step 1实现
