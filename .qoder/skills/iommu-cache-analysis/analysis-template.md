# [Cache名称] 技术分析报告

## 一、访问类型

| 访问类型 | 函数入口 | 触发场景 | 延时计算 |
|---------|---------|---------|---------|
| LOOKUP | `lookup_*()` | ... | ... |
| FILL | `fill_*()` | ... | ... |
| INVALIDATE | `invalidate_*()` | ... | ... |

### FILL 子类型

| 子类型 | 条件 | 延时公式 |
|-------|-----|---------|
| HIT | 更新已存在的 cacheline | hash + read_set + compute_hit + write |
| INVALID WAY | 写入空闲 way | hash + read_set + compute_invalid + write |
| REPLACEMENT | 替换已有 cacheline | hash + read_set + compute_replace + write |

## 二、内部执行流程

### 2.1 REQUEST 阶段流程

```
fifo.read()
    ↓
execute_*_request()
    ↓
cache_->lookup()
    ↓
arbitrate_ram_access(LOOKUP, ...)
    ↓
┌─ HIT: 返回数据 → response_fifo
└─ MISS: 转发到下一处理阶段
```

### 2.2 UPDATE 阶段流程

```
update_fifo.read()
    ↓
execute_*_update()
    ↓
cache_->fill()
    ↓
arbitrate_ram_access(FILL, ...)
    ↓
┌─ 已存在: fill_hit
├─ 空闲 way: fill_invalid
└─ 需替换: fill_replacement
```

### 2.3 调度策略

```
调度线程 {
    while (true) {
        if (has_req && has_upd) {
            // 乒乓选择
            do_request = next_is_request_;
        } else {
            do_request = has_req;
        }
        
        if (do_request) {
            // 处理 REQUEST
            next_is_request_ = false;
        } else {
            // 处理 UPDATE
            next_is_request_ = true;
        }
    }
}
```

## 三、延时计算

### 3.1 配置参数

```json
{
  "cache_timing": {
    "arbiter_latency_cycles": 1,
    "hash_latency_cycles": 1,
    "read_set_latency_cycles": 2,
    "compare_latency_cycles": 1,
    "fill_compute_index_hit_cycles": 1,
    "fill_compute_index_invalid_cycles": 2,
    "fill_compute_index_replacement_cycles": 4,
    "write_way_latency_cycles": 1
  }
}
```

### 3.2 计算公式

| 操作 | 公式 | Cycles | 延时 (1GHz) |
|-----|------|--------|-------------|
| LOOKUP HIT/MISS | arbiter + hash + read_set + compare | 1+1+2+1=5 | 5 ns |
| FILL HIT | arbiter + hash + read_set + compute_hit + write | 1+1+2+1+1=6 | 6 ns |
| FILL INVALID | arbiter + hash + read_set + compute_invalid + write | 1+1+2+2+1=7 | 7 ns |
| FILL REPLACE | arbiter + hash + read_set + compute_replace + write | 1+1+2+4+1=9 | 9 ns |
| INVALIDATE (per set) | arbiter + read_set + compare + write | 1+2+1+1=5 | 5 ns |

## 四、原子阶段

### 4.1 互斥机制

```cpp
// arbitrate_ram_access() 实现
template <typename Fn>
auto arbitrate_ram_access(CacheOpType op, Fn&& fn) {
    // 1. 请求 RAM 端口（可能等待）
    const sc_time queue_latency = request_ram_port(op);
    
    // 2. 设置 ram_port_busy_ = true（互斥锁）
    // 3. 执行操作 fn()
    // 4. 释放端口 ram_port_busy_ = false
}
```

### 4.2 原子操作列表

| 操作 | 原子阶段内容 | 典型延时 |
|-----|-------------|---------|
| LOOKUP | hash → read_set → compare | 5 ns |
| FILL (HIT) | hash → read_set → compute_index → write_way | 6 ns |
| FILL (INVALID) | hash → read_set → compute_index → write_way | 7 ns |
| FILL (REPLACE) | hash → read_set → compute_index → write_way | 9 ns |
| INVALIDATE (per set) | read_set → compare → write_way | 5 ns |

### 4.3 关键约束

1. **RAM 端口互斥**：同一时刻只有一个 LOOKUP/FILL/INVALIDATE 操作可以访问 RAM
2. **调度线程串行**：单线程处理 REQUEST 和 UPDATE，天然串行
3. **去重调度串行**：单线程处理去重请求和更新，天然串行

## 五、调度策略

| 属性 | 值 |
|-----|-----|
| 调度线程 | `*_scheduler_thread` |
| 调度算法 | 乒乓调度（REQUEST/UPDATE 交替） |
| FIFO 队列 | `*_request_fifo`, `*_update_fifo` |
| 流控机制 | FIFO blocking write |

## 六、性能统计

### 6.1 统计维度

- 按阶段：REQUEST (phase=0) / UPDATE (phase=1)
- 按操作类型：LOOKUP / FILL_HIT / FILLINVALID / FILLREPLACE
- 按区间：每 1000 任务统计命中率

### 6.2 关键指标

| 指标 | 说明 |
|-----|-----|
| `ExecAvg` | 平均执行延时 |
| `QueueAvg` | 平均排队延时 |
| `TotalAvg` | 平均端到端延时 |
| `HitRate` | 命中率 |
| `RAM_Access` | RAM 访问次数 |

## 七、总结

| 维度 | 说明 |
|-----|-----|
| **访问类型** | LOOKUP、FILL（3种子类型）、INVALIDATE |
| **原子阶段** | 每次 `arbitrate_ram_access()` 调用（RAM 端口互斥） |
| **调度方式** | 乒乓调度（REQUEST/UPDATE 交替） |
| **典型延时** | LOOKUP: 5ns, FILL: 6-9ns, INVALIDATE: 5ns |
| **串行约束** | RAM 端口互斥 + 单线程调度 |
