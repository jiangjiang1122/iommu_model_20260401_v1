# IOMMU 性能模型重构设计方案

## Context

当前 IOMMU 模型是一个纯功能级(Functional Level)的 SystemC TLM-2.0 模型，所有通信使用阻塞式 `b_transport`，一次只能处理一个翻译请求。整个翻译管线在同一个 SystemC 线程上下文中同步执行，无法模拟硬件的并发处理行为。

本方案将其重构为近似时序级(Approximately Timed, AT)性能模型，实现：
- 非阻塞 socket 通信，支持并发请求
- 按硬件子模块划分 SC_THREAD，每个子模块有独立的输入 buffer
- 异步 DDR 访问，通过 AXI ID 标识请求，上下文暂存本地
- 保留现有 Rivos 参考翻译核心逻辑，封装为各流水级调用的子函数

---

## 一、现有架构分析

### 1.1 当前通信模式（全阻塞，串行处理）

```
RP::send_request()
  -> axi_master->b_transport()           [阻塞]
    -> iommu_top::axi_slave_b_transport()
      -> iommu_translate_iova()           [同步翻译，约700行goto状态机]
        -> locate_device_context()
          -> read_memory_test() -> axi_master_1->b_transport() [阻塞DDR读]
        -> locate_process_context()
          -> read_memory_test() -> b_transport()               [阻塞DDR读]
        -> two_stage_address_translation()
          -> second_stage_address_translation()
            -> read_memory() -> b_transport()                  [阻塞DDR读]
        -> msi_address_translation()
      -> axi_master_0->b_transport()      [阻塞转发]
```

**核心问题**：一次只处理一笔请求，无并发能力。

### 1.2 关键文件清单

| 文件 | 作用 |
|------|------|
| `iommu/iommu_top.hh/.cc` | 顶层 SC_MODULE，4个initiator socket + 2个target socket |
| `iommu/iommu_struct.hh` | `iommu_t` 主结构体：寄存器、缓存(DC/PC/TLB各2条)、状态 |
| `iommu/iommu_translate.cc/.hh` | 主翻译函数 `iommu_translate_iova()` |
| `iommu/iommu_device_context.cc` | `locate_device_context()` DDT radix tree walk |
| `iommu/iommu_process_context.cc` | `locate_process_context()` PDT walk |
| `iommu/iommu_two_stage_trans.cc` | `two_stage_address_translation()` VS-stage walk |
| `iommu/iommu_second_stage_trans.cc` | `second_stage_address_translation()` G-stage walk |
| `iommu/iommu_msi_trans.cc` | `msi_address_translation()` MSI翻译 |
| `iommu/iommu_atc.cc/.hh` | 缓存 lookup/insert (DC/PC/TLB各2条LRU) |
| `iommu/iommu_ref_api.cc/.hh` | DDR访问封装：`read_memory_test()`/`write_memory_test()` 使用 `b_transport` |
| `iommu/iommu_command_queue.cc/.hh` | CQ 命令处理 `process_commands()` |
| `iommu/iommu_faults.cc` / `iommu_fault.hh` | Fault 上报 |
| `iommu/iommu_ats.cc/.hh` | ATS/PRI 处理 |
| `iommu/param_trans_def.hh` | `PayloadExtention` TLM扩展 |
| `ddr/test_ddr.hh` | DDR模型(1MB, 3个target socket, b_transport) |
| `rp/test_rp.hh`, `test_rp_func.cc`, `test_rp_thread.cc` | RP测试模型 |

### 1.3 现有接口与硬件对应

| 代码中Socket名称 | 硬件对应 | 作用 |
|------|------|------|
| `axi_slave_from_pcie_noc_0_socket` (Target) | AXI Slave 0 (512b@1GHz) | 接收翻译请求 |
| `ahb_slave_from_pcie_noc_1_socket` (Target) | AHB Slave (32b@100MHz) | 固件寄存器访问 |
| `axi_master_0_to_pcie_noc_socket` (Initiator) | AXI Master 0 (512b@1GHz) | 翻译后DMA数据转发 |
| `axi_master_1_to_cmn_rnd_socket` (Initiator) | AXI Master 2 (64b@1GHz) | DDT/PDT/页表/CQ/FQ读写 |
| `axi_master_2_to_pcie_noc_socket` (Initiator) | AXI Master 1 (128b@1GHz) | ATS消息回传RP |
| `axi_stream_to_cmn_rnd_socket` (Initiator) | AXI Stream (@450MHz) | IMSIC中断文件 |

---

## 二、性能模型整体架构

### 2.1 子模块划分与数据流

所有子模块作为 `iommu_top` 内部的 SC_THREAD，通过 `sc_fifo<iommu_task_t*>` 传递任务指针。

```
                    +----------------------------------------------------------+
                    |                  iommu_top (SC_MODULE)                    |
                    |                                                          |
 AXI Slave 0 ----->| [Parser]--fifo-->[Collector]--+-->[DC Cache]             |
  (nb_transport)    |    |                          |     | hit/miss           |
                    |    | PRI msg                  |     v                    |
                    |    v                          |   [xDTW]---DDR--->       |
                    | [PQ Proc]                     |     |                    |
                    |                               |   (DC/PC result)        |
                    |             [Collector]<-------+                         |
                    |                |                                         |
                    |                +-->[PC Cache]                            |
                    |                |     | miss->[xDTW]                      |
                    |                |                                         |
                    |                +-->[PT Cache]--miss-->[PTW]---DDR--->    |--> AXI Master 2
                    |                |                        |                |    (nb_transport)
                    |                |                  [MSIPT Walker]         |
                    |                v                                         |
 AXI Master 0 <----| [Forwarder] <-- 翻译成功转发DMA数据                      |
  (nb_transport)    |                                                          |
                    | [CQ Proc]---DDR--->                                      |
 AXI Master 1 <----| [FQ Proc]---DDR--->                                      |
  (nb_transport)    |                                                          |
                    | [Reg Access] (保持b_transport, 低频无需重构)             |
 AHB Slave -------->|                                                          |
  (b_transport)     +----------------------------------------------------------+
```

---

## 三、核心数据结构设计

### 3.1 统一事务上下文 `iommu_task_t`

新建文件 `iommu/iommu_task.hh`，定义贯穿整个管线的任务载体：

```cpp
// 任务状态枚举
enum task_state_t {
    TASK_PARSE_DONE,        // Parser完成解析
    TASK_DC_LOOKUP,         // 等待DC Cache查询
    TASK_DC_HIT,            // DC Cache命中
    TASK_DC_MISS,           // DC Cache未命中
    TASK_DC_WALK_DONE,      // xDTW完成DDT walk
    TASK_PC_LOOKUP,         // 等待PC Cache查询
    TASK_PC_HIT,            // PC Cache命中
    TASK_PC_MISS,           // PC Cache未命中
    TASK_PC_WALK_DONE,      // xDTW完成PDT walk
    TASK_TLB_LOOKUP,        // 等待TLB查询
    TASK_TLB_HIT,           // TLB命中
    TASK_TLB_MISS,          // TLB未命中
    TASK_PT_WALK_DONE,      // PTW完成页表walk
    TASK_MSI_DONE,          // MSI翻译完成
    TASK_FORWARD,           // 准备转发
    TASK_FAULT,             // 出错
    TASK_DONE               // 完成
};

// Walk类型枚举
enum walk_type_t {
    WALK_DDT,               // DDT radix tree walk
    WALK_PDT,               // PDT radix tree walk
    WALK_VS_PT,             // VS-stage 页表walk
    WALK_G_PT,              // G-stage 页表walk
    WALK_G_PT_IMPLICIT,     // G-stage 隐式翻译(VS PTE地址翻译)
    WALK_MSI_PT             // MSI 页表walk
};

// Walk上下文 - DDR异步回调恢复用
struct walk_context_t {
    walk_type_t walk_type;
    int8_t      level;          // 当前walk层级
    uint8_t     max_levels;     // 总层级数
    uint64_t    base_addr;      // 当前层基地址
    uint16_t    index[4];       // DDI/PDI/VPN索引数组
    uint8_t     pte_size;       // PTE大小(4或8)
    uint64_t    read_addr;      // 当前DDR读取地址
    uint8_t     read_size;      // 当前DDR读取大小
    // VS-stage特有
    uint64_t    vs_a;           // VS walk的当前地址a
    // G-stage特有
    uint64_t    g_a;            // G walk的当前地址a
    // 嵌套walk相关
    walk_context_t* saved_vs_ctx; // VS->G隐式翻译时保存的VS上下文
};

// 统一事务上下文
struct iommu_task_t {
    // === 任务标识 ===
    uint32_t    task_id;             // 全局唯一ID
    sc_time     timestamp;           // 进入时间戳

    // === 原始请求 ===
    uint32_t    device_id;
    uint32_t    process_id;
    uint8_t     pid_valid;
    uint8_t     exec_req, priv_req, no_write, is_cxl_dev;
    addr_type_t at;
    uint64_t    iova;
    uint32_t    length;
    uint8_t     read_writeAMO;
    tlm::tlm_generic_payload* original_trans; // 原始TLM payload指针

    // === 状态机 ===
    task_state_t state;
    uint8_t     TTYP;
    uint8_t     is_read, is_write, is_exec, priv;

    // === DC/PC 查询结果 ===
    device_context_t  DC;
    process_context_t PC;
    uint8_t     DTF, PSCV, GV, PV, SUM, SXL;
    uint32_t    GSCID, PSCID, DID, PID;
    iosatp_t    iosatp;
    iohgatp_t   iohgatp;

    // === 翻译结果 ===
    uint64_t    pa, gpa;
    uint64_t    page_sz, gst_page_sz;
    spte_t      vs_pte;
    gpte_t      g_pte;
    uint8_t     is_msi, is_mrif, is_bare_mode;
    uint32_t    mrif_nid;
    uint64_t    dest_mrif_addr;
    uint32_t    cause;
    uint64_t    iotval, iotval2;

    // === Walk上下文 ===
    walk_context_t walk_ctx;       // 当前walk状态
    walk_context_t saved_vs_walk;  // 嵌套G-stage时保存的VS walk

    // === DDR请求跟踪 ===
    uint16_t    current_axi_id;    // 当前DDR请求的AXI ID
};
```

### 3.2 DDR响应结构

```cpp
struct ddr_response_t {
    uint16_t    axi_id;
    uint8_t     status;      // 0=OK, 1=ERROR
    uint8_t     data[64];    // 读回数据（最大64字节，DC大小）
    uint32_t    data_size;
};
```

### 3.3 AXI ID 分配器

```cpp
struct axi_id_allocator_t {
    std::queue<uint16_t> free_ids;
    sc_event             id_freed_evt;
    uint16_t             max_ids;

    void init(uint16_t max);      // 初始化所有ID到空闲池
    uint16_t alloc_id();          // 分配ID，池空则wait(id_freed_evt)
    void free_id(uint16_t id);    // 归还ID，触发id_freed_evt
};
```

### 3.4 Outstanding DDR请求表

```cpp
// 在iommu_top中维护
struct ddr_outstanding_entry_t {
    iommu_task_t*   task;
    walk_type_t     walk_type;
    uint64_t        expected_addr;
    uint8_t         expected_size;
};
std::unordered_map<uint16_t, ddr_outstanding_entry_t> ddr_outstanding_table;
```

---

## 四、各子模块SC_THREAD设计

### 4.1 FIFO声明（在iommu_top.hh中）

```cpp
// Parser输出
sc_fifo<iommu_task_t*> parser_to_collector_fifo;   // 深度16
sc_fifo<iommu_task_t*> parser_to_pq_fifo;          // 深度4

// Collector到各Cache
sc_fifo<iommu_task_t*> collector_to_dc_cache_fifo;  // 深度8
sc_fifo<iommu_task_t*> collector_to_pc_cache_fifo;  // 深度8
sc_fifo<iommu_task_t*> collector_to_pt_cache_fifo;  // 深度8

// Collector到Walker
sc_fifo<iommu_task_t*> collector_to_xdtw_fifo;     // 深度4
sc_fifo<iommu_task_t*> collector_to_ptw_fifo;       // 深度4
sc_fifo<iommu_task_t*> collector_to_msi_fifo;       // 深度4

// Walker返回Collector
sc_fifo<iommu_task_t*> xdtw_to_collector_fifo;     // 深度4
sc_fifo<iommu_task_t*> ptw_to_collector_fifo;       // 深度4
sc_fifo<iommu_task_t*> msi_to_collector_fifo;       // 深度4

// Cache返回Collector
sc_fifo<iommu_task_t*> dc_cache_to_collector_fifo;  // 深度8
sc_fifo<iommu_task_t*> pc_cache_to_collector_fifo;  // 深度8
sc_fifo<iommu_task_t*> pt_cache_to_collector_fifo;  // 深度8

// 最终路径
sc_fifo<iommu_task_t*> collector_to_fwd_fifo;       // 深度8
sc_fifo<iommu_task_t*> collector_to_fault_fifo;     // 深度8

// DDR响应FIFO
sc_fifo<ddr_response_t> xdtw_ddr_rsp_fifo;         // 深度8
sc_fifo<ddr_response_t> ptw_ddr_rsp_fifo;           // 深度8
sc_fifo<ddr_response_t> msi_ddr_rsp_fifo;           // 深度4

// 入站缓冲
sc_fifo<tlm::tlm_generic_payload*> inbound_fifo;    // 深度16
```

### 4.2 SC_THREAD声明（在构造函数中注册）

```cpp
SC_THREAD(parser_thread);
SC_THREAD(collector_thread);
SC_THREAD(dc_cache_thread);
SC_THREAD(pc_cache_thread);
SC_THREAD(pt_cache_thread);
SC_THREAD(xdtw_thread);
SC_THREAD(ptw_thread);
SC_THREAD(msi_walker_thread);
SC_THREAD(forwarder_thread);
SC_THREAD(fq_proc_thread);
SC_THREAD(pq_proc_thread);
SC_THREAD(cq_proc_thread);
```

### 4.3 Parser（解析器）

**输入**: `inbound_fifo`（由 `nb_transport_fw` 回调填充）
**输出**: `parser_to_collector_fifo` 或 `parser_to_pq_fifo`

```
行为:
  loop:
    task_payload = inbound_fifo.read()  // 阻塞等待
    分配 task_id (原子递增)
    从PayloadExtention提取: device_id, at, pid_valid, process_id等
    执行iommu_translate_iova中step 1~5的纯计算逻辑:
      - 检查 ddtp.iommu_mode (Off/Bare/DDT_xLVL)
      - 提取DDI索引
      - 推导TTYP和访问属性
    如果是PRI消息 -> parser_to_pq_fifo
    否则 -> parser_to_collector_fifo
    wait(1, SC_NS)  // 模拟解析延迟
```

**不涉及DDR访问，全部是纯逻辑。**

### 4.4 Collector（收集调度器）

**输入**: 多个FIFO（新请求、各Cache返回、各Walker返回）
**输出**: 各Cache查询FIFO、各Walker FIFO、转发/错误FIFO

这是翻译管线的核心调度点，执行 `iommu_translate_iova()` 中步骤6~20的纯逻辑判断（不含DDR访问部分）。

```
行为:
  loop:
    等待任一输入FIFO非空 (使用sc_event_or_list)
    按优先级取出task:
      1. xdtw_to_collector_fifo  (walk完成，优先处理)
      2. ptw_to_collector_fifo   (页表walk完成)
      3. msi_to_collector_fifo   (MSI翻译完成)
      4. dc_cache_to_collector_fifo (DC查询结果)
      5. pc_cache_to_collector_fifo (PC查询结果)
      6. pt_cache_to_collector_fifo (TLB查询结果)
      7. parser_to_collector_fifo   (新请求)

    根据 task->state 决定下一步:

    TASK_PARSE_DONE:
      -> 发往 collector_to_dc_cache_fifo (查DC)

    TASK_DC_HIT:
      -> 执行step 7~13纯逻辑检查(DC配置验证, EN_ATS, T2GPA等)
      -> 如果PDTV==1 -> 发往 collector_to_pc_cache_fifo (查PC)
      -> 否则 -> 设置iosatp/iohgatp -> 发往 collector_to_pt_cache_fifo (查TLB)

    TASK_DC_MISS:
      -> 发往 collector_to_xdtw_fifo (DDT walk)

    TASK_DC_WALK_DONE:
      -> 缓存DC -> 执行step 7~13 -> 继续

    TASK_PC_HIT:
      -> 执行step 15~16逻辑 -> 发往 collector_to_pt_cache_fifo (查TLB)

    TASK_PC_MISS:
      -> 发往 collector_to_xdtw_fifo (PDT walk)

    TASK_PC_WALK_DONE:
      -> 缓存PC -> 继续

    TASK_TLB_HIT:
      -> 直接跳到step 20 -> 发往 collector_to_fwd_fifo

    TASK_TLB_MISS:
      -> 发往 collector_to_ptw_fifo (页表walk)

    TASK_PT_WALK_DONE:
      -> 如果需要MSI翻译 -> 发往 collector_to_msi_fifo
      -> 否则 -> 缓存IOTLB -> 发往 collector_to_fwd_fifo

    TASK_MSI_DONE:
      -> 处理MSI结果 -> 发往 collector_to_fwd_fifo

    TASK_FAULT:
      -> 发往 collector_to_fault_fifo

    wait(1, SC_NS)  // 每次调度1ns延迟
```

### 4.5 DC Cache / PC Cache / PT Cache

**结构统一**：每个Cache子模块是一个简单的SC_THREAD。

```
行为 (以DC Cache为例):
  loop:
    task = collector_to_dc_cache_fifo.read()
    result = lookup_ioatc_dc(&iommu_inst, task->device_id, &task->DC)
    task->state = (result == IOATC_HIT) ? TASK_DC_HIT : TASK_DC_MISS
    dc_cache_to_collector_fifo.write(task)
    wait(1, SC_NS)  // 模拟1周期查询延迟
```

**缓存容量扩展**：
- DC Cache: 从2条扩大到64条（`RVI_IOMMU_DDT_CACHE_SIZE = 64`）
- PC Cache: 从2条扩大到可配置大小
- TLB: 从2条扩大到可配置大小

### 4.6 xDTW（DDT/PDT Walker）

**核心子模块**，需要异步DDR访问。

```
行为:
  loop:
    等待(新请求 OR DDR响应)

    如果有新请求 (collector_to_xdtw_fifo非空):
      task = fifo.read()
      初始化walk_ctx:
        - DDT walk: level = max_level-1, base = ddtp.PPN * PAGESIZE
        - PDT walk: level = max_level-1, base = DC.fsc.PPN * PAGESIZE
      计算第一级读取地址: addr = base + index[level] * entry_size
      分配AXI ID: axi_id = ddr_id_alloc.alloc_id()
      记录到outstanding表: ddr_outstanding_table[axi_id] = {task, walk_type, addr, size}
      发起非阻塞DDR读: nb_transport_fw(BEGIN_REQ) on AXI Master 2
      task->current_axi_id = axi_id
      wait(1, SC_NS)

    如果有DDR响应 (xdtw_ddr_rsp_fifo非空):
      rsp = fifo.read()
      从outstanding表查找: entry = ddr_outstanding_table[rsp.axi_id]
      task = entry.task
      释放AXI ID: ddr_id_alloc.free_id(rsp.axi_id)
      从outstanding表删除

      将rsp.data解析为ddte/DC/pdte/PC
      执行验证逻辑(V位检查, reserved位检查)

      如果非叶节点 && level > 0:
        level--, 计算下一级地址, 发起新DDR读
      如果叶节点:
        DDT: 读取完整DC(可能需要多次8字节读), 执行DC配置检查
        PDT: 读取PC, 执行PC配置检查
        缓存结果
        task->state = TASK_DC_WALK_DONE / TASK_PC_WALK_DONE
        xdtw_to_collector_fifo.write(task)
      如果错误:
        task->cause = xxx, task->state = TASK_FAULT
        xdtw_to_collector_fifo.write(task)
```

**并发能力**: 多个task的walk可以交错执行（一个等DDR响应时处理另一个的响应或新请求）。

### 4.7 PTW（Page Table Walker）

最复杂的子模块，需要处理两阶段翻译的嵌套walk。

```
行为:
  loop:
    等待(新请求 OR DDR响应)

    新请求处理:
      task = collector_to_ptw_fifo.read()
      根据iosatp/iohgatp确定walk类型:
        - iosatp.MODE == Bare: pa = iova, 直接返回
        - 否则: 初始化VS-stage walk_ctx
      计算PTE地址 = a + vpn[i] * PTESIZE
      如果G-stage活跃(iohgatp.MODE != Bare):
        保存VS walk上下文到task->saved_vs_walk
        初始化G-stage walk用于隐式PTE地址翻译
        发起G-stage的DDR读
      否则:
        直接发起VS-stage的DDR读

    DDR响应处理:
      rsp = ptw_ddr_rsp_fifo.read()
      task = outstanding表查找
      根据walk_type:

        WALK_G_PT_IMPLICIT (VS PTE地址的G-stage翻译):
          处理G-stage PTE -> 如果非叶则继续G walk
          如果G walk完成:
            得到翻译后的SPA
            恢复VS walk上下文(task->saved_vs_walk)
            用SPA作为地址发起VS PTE读取

        WALK_VS_PT:
          读取VS PTE
          执行验证(V位, 权限, 叶/非叶判断)
          如果非叶: level--, 计算下一级地址
            如果G-stage活跃: 先做隐式G翻译再读PTE
          如果叶: 权限检查, 超级页对齐, A/D位处理
            如果需要G-stage显式翻译(GPA->SPA):
              初始化G walk, 发起DDR读
            否则完成: task->state = TASK_PT_WALK_DONE

        WALK_G_PT (显式G-stage翻译):
          处理G PTE -> 完成后设置最终PA
          task->state = TASK_PT_WALK_DONE
          ptw_to_collector_fifo.write(task)
```

**嵌套Walk设计要点**：
- VS-stage每级PTE读取可能需要完整的G-stage walk（最多5级）
- `iommu_task_t`中同时维护 `walk_ctx`(当前活跃walk) 和 `saved_vs_walk`(保存的VS walk)
- G-stage隐式翻译完成后恢复VS walk继续

### 4.8 Forwarder（转发器）

```
行为:
  loop:
    task = collector_to_fwd_fifo.read()
    修改原始TLM payload地址为翻译后的PA
    通过AXI Master 0发起nb_transport_fw(BEGIN_REQ)
    等待nb_transport_bw(BEGIN_RESP) -- 确保DMA数据实际到达DDR
    通过AXI Slave 0的nb_transport_bw(BEGIN_RESP)返回响应给RP
    释放iommu_task_t
```

### 4.9 CQ Proc / FQ Proc / PQ Proc

封装现有 `process_commands()`、`report_fault()`、`handle_page_request()` 逻辑。DDR访问改为异步，但这些模块的DDR访问频率低，可以使用简化的异步方案（或暂保持同步）。

---

## 五、非阻塞Socket改造

### 5.1 改造范围

| Socket | 改造方式 | 说明 |
|--------|---------|------|
| AXI Slave 0 | b_transport -> nb_transport_fw/bw | 翻译请求入口，必须非阻塞 |
| AXI Master 0 | b_transport -> nb_transport_fw/bw | 翻译后DMA转发 |
| AXI Master 2 (代码中叫master_1) | b_transport -> nb_transport_fw/bw | DDR访问，最关键改造点 |
| AXI Master 1 (代码中叫master_2) | b_transport -> nb_transport_fw/bw | ATS消息 |
| AHB Slave | **保持b_transport** | 低频寄存器访问，无需改造 |
| AXI Stream | **保持b_transport** | 低频中断操作 |

### 5.2 AXI Slave 0 非阻塞处理

```cpp
// 注册nb_transport_fw替代b_transport
tlm::tlm_sync_enum axi_slave_nb_transport_fw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_REQ) {
        // 将payload指针放入入站FIFO
        trans.acquire();  // 增加引用计数
        inbound_fifo.nb_write(&trans);
        phase = tlm::END_REQ;
        return tlm::TLM_UPDATED;
    }
    return tlm::TLM_ACCEPTED;
}
```

### 5.3 DDR端口(AXI Master 2) 非阻塞处理

```cpp
// 发起DDR读请求 (在xDTW/PTW中调用)
void send_ddr_read_request(uint64_t addr, uint8_t size, uint16_t axi_id) {
    auto* trans = new tlm::tlm_generic_payload();
    auto* ext = new PayloadExtention();
    ext->axi_id = axi_id;
    trans->set_extension(ext);
    trans->set_command(tlm::TLM_READ_COMMAND);
    trans->set_address(addr);
    trans->set_data_length(size);
    // ... 设置其他字段
    tlm::tlm_phase phase = tlm::BEGIN_REQ;
    sc_time delay = SC_ZERO_TIME;
    axi_master_1_to_cmn_rnd_socket->nb_transport_fw(*trans, phase, delay);
}

// DDR响应回调
tlm::tlm_sync_enum ddr_nb_transport_bw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_RESP) {
        PayloadExtention* ext;
        trans.get_extension(ext);
        uint16_t axi_id = ext->axi_id;

        // 构建响应
        ddr_response_t rsp;
        rsp.axi_id = axi_id;
        rsp.status = (trans.get_response_status() == tlm::TLM_OK_RESPONSE) ? 0 : 1;
        memcpy(rsp.data, trans.get_data_ptr(), trans.get_data_length());
        rsp.data_size = trans.get_data_length();

        // 根据outstanding表中的walk_type路由到对应FIFO
        auto it = ddr_outstanding_table.find(axi_id);
        if (it != ddr_outstanding_table.end()) {
            switch (it->second.walk_type) {
                case WALK_DDT: case WALK_PDT:
                    xdtw_ddr_rsp_fifo.nb_write(rsp); break;
                case WALK_VS_PT: case WALK_G_PT: case WALK_G_PT_IMPLICIT:
                    ptw_ddr_rsp_fifo.nb_write(rsp); break;
                case WALK_MSI_PT:
                    msi_ddr_rsp_fifo.nb_write(rsp); break;
            }
        }
        phase = tlm::END_RESP;
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}
```

---

## 六、数据排序与一致性

### 6.1 同设备请求排序

规范要求：AXI Slave 0的响应必须等到AXI Master 0的响应后才能返回。

**方案**：Forwarder中按device_id维护per-device排序队列：
```cpp
std::map<uint32_t, std::queue<iommu_task_t*>> device_order_queues;
```
每个设备的响应严格按请求到达顺序返回。

### 6.2 Cache Invalidation同步

使用 `sc_mutex` 保护缓存数据结构：
- DC/PC/TLB Cache各用一个 `sc_mutex`
- Cache SC_THREAD查询时 lock -> unlock
- CQ Proc执行invalidation时 lock -> invalidate -> unlock

IOFENCE命令处理时等待所有in-flight翻译完成（通过检查outstanding表）。

---

## 七、外部模型改造

### 7.1 DDR模型 (`ddr/test_ddr.hh`)

变更内容：
1. 3个target socket注册 `nb_transport_fw` 回调
2. 接收 `BEGIN_REQ` 时执行内存读写，使用SC_THREAD + `wait(read_latency)` 模拟DDR延迟
3. 延迟到期后通过 `nb_transport_bw` 发送 `BEGIN_RESP`
4. 支持多outstanding请求（并发处理不同AXI ID）
5. 保留b_transport注册用于AHB端口兼容

新增可配置参数：
- `sc_time read_latency` (默认10ns)
- `sc_time write_latency` (默认8ns)
- `uint32_t max_outstanding` (默认16)

### 7.2 RP模型 (`rp/test_rp.hh`, `test_rp_func.cc`, `test_rp_thread.cc`)

变更内容：
1. initiator socket改为 `nb_transport_fw` 发送请求
2. 注册 `nb_transport_bw` 接收IOMMU翻译响应
3. `send_translation_request_rp()` 改为非阻塞：发送后等待`sc_event`
4. 测试线程改为可发送并发请求

### 7.3 main.cpp

- 增大仿真时间（从1000ns增大到满足并发测试需求）
- Socket绑定无需变化（simple_socket同时支持b/nb transport）

---

## 八、文件修改清单

### 需要新建的文件

| 文件 | 说明 |
|------|------|
| `iommu/iommu_task.hh` | `iommu_task_t`, `walk_context_t`, `ddr_response_t`, `axi_id_allocator_t`, 状态枚举 |
| `iommu/iommu_pipeline.hh` | FIFO类型alias, 管线常量(深度/延迟等), 子函数声明 |
| `iommu/iommu_pipeline.cc` | 从Rivos参考代码提取的纯逻辑验证子函数(DC验证, PC验证, PTE验证等) |

### 需要重大修改的文件

| 文件 | 修改内容 |
|------|---------|
| `iommu/iommu_top.hh` | 添加全部sc_fifo声明、SC_THREAD声明、nb_transport回调、AXI ID分配器、outstanding表 |
| `iommu/iommu_top.cc` | 实现全部SC_THREAD(12个)、nb_transport_fw/bw回调；重构axi_slave处理逻辑 |
| `ddr/test_ddr.hh` | 添加nb_transport_fw回调、延迟响应机制、多outstanding支持 |

### 需要中等修改的文件

| 文件 | 修改内容 |
|------|---------|
| `iommu/iommu_ref_api.cc` | `read_memory_test`/`write_memory_test`提供异步版本或保留同步版给CQ/FQ等低频路径 |
| `iommu/iommu_command_queue.cc` | DDR访问改为异步；添加cache invalidation同步逻辑 |
| `iommu/param_trans_def.hh` | `PayloadExtention`添加`axi_id`字段 |
| `rp/test_rp.hh` | socket改nb_transport, 添加nb_transport_bw回调 |
| `rp/test_rp_func.cc` | `send_translation_request_rp`改为非阻塞 |
| `rp/test_rp_thread.cc` | 测试线程支持并发请求 |

### 需要小修改的文件

| 文件 | 修改内容 |
|------|---------|
| `iommu/iommu_struct.hh` | 扩大缓存大小常量 |
| `iommu/iommu_atc.hh` | `DDT_CACHE_SIZE`=64, 扩大TLB_SIZE |
| `iommu/iommu_atc.cc` | 适配更大缓存，可选添加hash查找优化 |
| `iommu/iommu_faults.cc` | `report_fault`中`write_memory`改为通过FQ Proc封装 |
| `main.cpp` | 增大仿真时间, Makefile添加新文件 |
| `Makefile` | 添加新源文件编译 |

### 保持不变的文件

| 文件 | 原因 |
|------|------|
| `iommu/iommu_translate.cc` | 核心翻译逻辑保留，各子模块调用其验证子函数 |
| `iommu/iommu_device_context.cc` | 验证逻辑保留，DDR访问由xDTW接管 |
| `iommu/iommu_process_context.cc` | 同上 |
| `iommu/iommu_two_stage_trans.cc` | 同上 |
| `iommu/iommu_second_stage_trans.cc` | 同上 |
| `iommu/iommu_msi_trans.cc` | 同上 |
| `iommu/iommu_registers.hh` | 纯数据结构定义 |
| `iommu/iommu_data_structures.hh` | 纯数据结构定义 |
| `iommu/iommu_utils.hh/.cc` | 工具函数 |
| `iommu/iommu_hpm.hh/.cc` | 性能计数器 |
| `iommu/iommu_interrupt.hh/.cc` | 中断生成 |

---

## 九、实施步骤

### Phase 1: 基础设施
1. 创建 `iommu_task.hh` - 定义所有数据结构
2. 创建 `iommu_pipeline.hh/.cc` - 管线参数和提取的验证子函数
3. 修改 `iommu_top.hh` - 添加FIFO、SC_THREAD声明
4. 实现 AXI ID 分配器
5. 修改 `param_trans_def.hh` 添加 axi_id

### Phase 2: DDR模型改造
1. DDR模型添加 nb_transport_fw/bw
2. 实现可配置延迟响应
3. 单笔nb_transport通信验证

### Phase 3: 翻译管线实现
1. Parser + Forwarder (Bare mode直通验证)
2. DC Cache + xDTW (DDT walk异步化)
3. Collector状态机 (DC查询全流程)
4. PT Cache + PTW (页表walk异步化)
5. MSI walker
6. PC Cache

### Phase 4: RP模型改造与并发测试
1. RP改为nb_transport
2. 并发请求发送测试
3. 多设备并发翻译验证

### Phase 5: 排序、一致性、错误路径
1. per-device响应排序
2. cache invalidation同步
3. IOFENCE处理
4. 错误路径测试

---

## 十、验证方法

### 10.1 编译验证
```bash
# 在WSL中
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
make clean && make DEBUG=1
```

### 10.2 功能正确性验证
- 复用现有测试用例（3个设备的翻译测试），确保翻译结果与功能模型一致
- Bare mode直通测试
- Sv48x4 G-stage翻译测试
- Sv39 S-stage翻译测试

### 10.3 并发性验证
- 多设备同时发送翻译请求，观察并发执行
- DC Cache命中/未命中的并发场景
- DDR outstanding请求数验证
- per-device响应顺序验证

### 10.4 性能指标观测
- 打印每笔翻译的端到端延迟
- 统计DDR访问次数和Cache命中率
- 观察管线利用率

---

## 十一、风险与注意事项

1. **Rivos参考代码的goto密集结构**: `iommu_translate_iova()` 700+行使用大量goto，不宜直接修改。方案是保留原函数，从中提取纯逻辑验证子函数供各子模块调用。

2. **嵌套Walk复杂性**: VS-stage每级PTE可能触发完整G-stage walk（最多5x5=25次DDR访问），需仔细设计上下文栈。

3. **SystemC协作式线程**: `sc_fifo::write()`在FIFO满时阻塞，需合理设置深度或使用`nb_write()`。

4. **TLM Payload生命周期**: nb_transport模式下payload不能提前释放。DDR请求使用独立payload实例。

5. **调试复杂度**: 异步后调试困难，所有子模块关键步骤应打印task_id用于追踪。
