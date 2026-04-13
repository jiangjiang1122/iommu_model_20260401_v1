# IOMMU 性能模型重构设计方案 (V3)

## Context

当前 IOMMU 模型是一个纯功能级(Functional Level)的 SystemC TLM-2.0 模型，所有通信使用阻塞式 `b_transport`，一次只能处理一个翻译请求。本方案将其重构为近似时序级(Approximately Timed, AT)性能模型。

**设计原则**：
- 现有 Rivos 参考模型代码（`iommu_translate.cc`、`iommu_device_context.cc` 等）**保留不修改**，作为功能参考模型
- 各硬件子模块按照实际硬件功能**重新实现代码**，可借鉴参考模型中的数据结构定义、地址计算逻辑、验证检查逻辑等
- 非阻塞 socket 通信支持并发请求
- 按硬件子模块划分 SC_THREAD，每个子模块有独立的输入 buffer
- 异步 DDR 访问，通过 AXI ID 标识请求，上下文暂存本地
- DDR 端口同时支持阻塞和非阻塞两种模式（原子操作必须使用阻塞接口）

**V3 相对 V2 的核心变更**：
1. Parser 直接向 DC Cache 和 PC Cache 发送查询请求（不再由 Collector 转发）
2. 各子模块 thread 拆分为更细粒度（请求/响应分离）
3. 新增 msiptw_thread 和全局 fault_fifo + fault_proc_thread
4. FIFO 定义与 thread 严格一一对应

---

## 一、现有架构分析

### 1.1 当前通信模式（全阻塞，串行处理）

```
RP::send_request()
  -> axi_master->b_transport()           [阻塞]
    -> iommu_top::axi_slave_b_transport()
      -> iommu_translate_iova()           [同步翻译，约700行goto状态机]
        -> locate_device_context()
          -> read_memory_test() -> b_transport() [阻塞DDR读]
        -> locate_process_context()
          -> read_memory_test() -> b_transport() [阻塞DDR读]
        -> two_stage_address_translation()
          -> read_memory() -> b_transport()      [阻塞DDR读]
        -> msi_address_translation()
          -> read_memory() -> b_transport()      [阻塞DDR读]
      -> axi_master_0->b_transport()      [阻塞转发]
```

**核心问题**：一次只处理一笔请求，无并发能力。

### 1.2 关键文件清单

| 文件 | 作用 | 性能模型中定位 |
|------|------|--------------|
| `iommu/iommu_top.hh/.cc` | 顶层SC_MODULE | **重写**为性能模型核心 |
| `iommu/iommu_struct.hh` | `iommu_t`主结构体 | 部分复用数据结构 |
| `iommu/iommu_translate.cc/.hh` | `iommu_translate_iova()` | **保留为功能参考** |
| `iommu/iommu_device_context.cc` | `locate_device_context()` | **保留为功能参考** |
| `iommu/iommu_process_context.cc` | `locate_process_context()` | **保留为功能参考** |
| `iommu/iommu_two_stage_trans.cc` | `two_stage_address_translation()` | **保留为功能参考** |
| `iommu/iommu_second_stage_trans.cc` | `second_stage_address_translation()` | **保留为功能参考** |
| `iommu/iommu_msi_trans.cc` | `msi_address_translation()` | **保留为功能参考** |
| `iommu/iommu_atc.cc/.hh` | 缓存lookup/insert | 借鉴并扩展缓存逻辑 |
| `iommu/iommu_ref_api.cc/.hh` | DDR访问封装 | **重写**为双模式(阻塞+非阻塞) |
| `iommu/iommu_data_structures.hh` | DC/PC/PTE数据结构 | **直接复用** |
| `iommu/iommu_registers.hh` | 寄存器定义 | **直接复用** |
| `iommu/param_trans_def.hh` | PayloadExtention | 扩展axi_id字段 |
| `ddr/test_ddr.hh` | DDR模型 | **重写**支持并发 |
| `rp/test_rp*.cc/.hh` | RP测试模型 | **重写**支持并发 |

### 1.3 现有接口与硬件对应

| 代码中Socket名称 | 硬件对应 | 作用 |
|------|------|------|
| `axi_slave_from_pcie_noc_0_socket` (Target) | AXI Slave 0 (512b@1GHz) | 接收翻译请求+Page Request |
| `ahb_slave_from_pcie_noc_1_socket` (Target) | AHB Slave (32b@100MHz) | 固件寄存器访问 |
| `axi_master_0_to_pcie_noc_socket` (Initiator) | AXI Master 0 (512b@1GHz) | 翻译后DMA数据转发 |
| `axi_master_1_to_cmn_rnd_socket` (Initiator) | AXI Master 2 (64b@1GHz) | DDT/PDT/页表/CQ/FQ/PQ/MRIF读写 |
| `axi_master_2_to_pcie_noc_socket` (Initiator) | AXI Master 1 (128b@1GHz) | ATS消息回传RP |
| `axi_stream_to_cmn_rnd_socket` (Initiator) | AXI Stream (@450MHz) | IMSIC中断文件访问 |

---

## 二、性能模型整体架构

### 2.1 子模块划分与数据流

所有子模块作为 `iommu_top` 内部的 SC_THREAD，通过 `sc_fifo<iommu_task_t*>` 传递任务指针。

```
+====================================================================================+
|                              iommu_top (SC_MODULE)                                  |
|                                                                                     |
|  AXI Slave 0 -----> [inbound_fifo]                                                 |
|  (nb_transport)           |                                                         |
|                           v                                                         |
|                      [Parser] --parser_to_pq_fifo--> [PQ Proc]                     |
|                       |  |  |                                                       |
|          +------------+  |  +------------+                                          |
|          |               |               |                                          |
|  parser_to_dc_cache  parser_to       parser_to_pc_cache                             |
|  _query_fifo        _collector_fifo  _query_fifo                                    |
|          |          (iova+原始请求)       |                                          |
|          v               |               v                                          |
|   [DC Cache]             |         [PC Cache]                                       |
|   (查询+更新)             |         (查询+更新)                                       |
|          |               |               |                                          |
|  dc_cache_to_            |       pc_cache_to_                                       |
|  collector_fifo          |       collector_fifo                                     |
|          |               |               |                                          |
|          v               v               v                                          |
|        +----------[Collector]------------+                                          |
|        | 进程1: 接收Parser任务+DC/PC结果   |                                          |
|        |   (pending_tasks按task_id匹配)   |                                          |
|        | 进程2: xDTW响应处理               |                                          |
|        +---------+---------+-------------+                                          |
|                  |         |         |                                               |
|   collector_to_ /          |          \ collector_to_                               |
|   xdtw_fifo   /           |           \ pt/msipt_cache                              |
|               v            |            v                                           |
|          [xDTW]            |    collector_to_pt_cache_query_fifo                    |
|     (2进程:请求+响应)       |    collector_to_msipt_cache_query_fifo                  |
|     DDR(nb)-->             |            |              |                             |
|          |                 |            v              v                             |
|   xdtw_to_collector_fifo  |   [PT Cache]       [MSIPT Cache]                       |
|          |                 |   (3进程:            (4进程:                             |
|          v                 |    查询/结果/PTW响应)  查询/结果/MSIPTW请求/MSIPTW响应)    |
|        Collector           |      |                    |                             |
|                            |   miss:                miss:                            |
|  collector_to_dc/pc_       |   pt_cache_to_ptw_fifo  msipt_cache_to_msiptw_fifo     |
|  cache_update_fifo         |      |                    |                             |
|     |          |           |      v                    v                             |
|     v          v           |   [PTW]               [MSIPTW]                         |
|  DC Cache   PC Cache       |   (2进程:请求+响应)    (2进程:请求+响应)                   |
|  (更新)     (更新)          |   DDR(nb)-->           DDR(nb)-->                       |
|                            |      |                    |                             |
|                            |   ptw_to_pt_cache_fifo  msiptw_to_msipt_cache_fifo     |
|                            |      |                    |                             |
|                            |      v                    v                             |
|                            |   PT Cache更新缓存     MSIPT Cache更新缓存              |
|                            |      |                    |                             |
|                            | pt_cache_to_fwd_fifo  msipt_cache_to_fwd_fifo          |
|                            |      |                    |                             |
|                            |      v                    v                             |
|                            | --> AXI Master 0     --> AXI Stream (M==3)             |
|                            | (翻译后DMA数据)      --> AXI Master 2 (M==1, MRIF)     |
|                            |                                                        |
|  ====== 错误处理通道 ======                                                          |
|  各模块 --fault_fifo--> [Fault Proc] --> [FQ Proc] --DDR-->                         |
|                                                                                     |
|  [CQ Proc]---DDR--->                                                                |
|                                                                                     |
|  [Reg Access] (保持b_transport)                                                     |
|  AHB Slave -------->                                                                |
+====================================================================================+
```

---

## 三、核心数据结构设计

### 3.1 统一事务上下文 `iommu_task_t`

新建文件 `iommu/iommu_task.hh`：

```cpp
// 任务状态枚举
enum task_state_t {
    // Parser阶段
    TASK_PARSE_DONE,           // Parser完成解析，已发起DC+PC查询

    // DC/PC Cache查询结果
    TASK_DC_HIT,               // DC Cache命中
    TASK_DC_MISS,              // DC Cache未命中
    TASK_PC_HIT,               // PC Cache命中
    TASK_PC_MISS,              // PC Cache未命中

    // xDTW完成
    TASK_DC_WALK_DONE,         // xDTW完成DDT walk，得到DC
    TASK_PC_WALK_DONE,         // xDTW完成PDT walk，得到PC

    // PT Cache查询
    TASK_TLB_HIT,              // PT Cache(TLB)命中
    TASK_TLB_MISS,             // PT Cache(TLB)未命中

    // PTW完成
    TASK_PT_WALK_DONE,         // PTW完成页表walk

    // MSIPT Cache查询
    TASK_MSIPT_HIT,            // MSIPT Cache命中
    TASK_MSIPT_MISS,           // MSIPT Cache未命中
    TASK_MSIPT_WALK_DONE,      // MSIPTW完成MSI页表walk

    // 最终状态
    TASK_FORWARD,              // 地址翻译完成，准备转发
    TASK_FAULT,                // 出错
    TASK_DONE                  // 完成
};

// Walk类型枚举（用于DDR请求标识）
enum walk_type_t {
    WALK_DDT,                  // DDT radix tree walk
    WALK_PDT,                  // PDT radix tree walk
    WALK_VS_PT,                // VS-stage 页表walk
    WALK_G_PT,                 // G-stage 页表walk
    WALK_G_PT_IMPLICIT,        // G-stage 隐式翻译(VS PTE地址翻译)
    WALK_MSI_PT,               // MSI 页表walk
    WALK_AD_UPDATE,            // A/D位原子更新（blocking）
    WALK_FQ_WRITE,             // FQ写入
    WALK_PQ_WRITE,             // PQ写入
    WALK_CQ_READ,              // CQ读取
    WALK_MRIF_WRITE            // MRIF写入（blocking原子OR）
};

// Walk上下文 - DDR异步回调恢复用
struct walk_context_t {
    walk_type_t walk_type;
    int8_t      level;          // 当前walk层级
    uint8_t     max_levels;     // 总层级数
    uint64_t    base_addr;      // 当前层基地址
    uint16_t    index[4];       // DDI/PDI/VPN索引数组
    uint8_t     pte_size;       // PTE大小(4或8字节)
    uint64_t    read_addr;      // 当前DDR读取地址
    uint8_t     read_size;      // 当前DDR读取大小
    uint64_t    vs_a;           // VS walk的当前地址a
    uint64_t    g_a;            // G walk的当前地址a
};

// 统一事务上下文
struct iommu_task_t {
    // === 任务标识 ===
    uint32_t    task_id;             // 全局唯一ID（由Parser分配）
    sc_time     timestamp;           // 进入时间戳

    // === 原始请求（从PayloadExtention提取） ===
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
    bool        dc_valid;            // DC查询是否已返回
    bool        pc_valid;            // PC查询是否已返回
    bool        need_pc;             // 是否需要PC（由Collector根据DC.tc.PDTV判断）
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
    walk_context_t walk_ctx;         // 当前活跃walk
    walk_context_t saved_vs_walk;    // 嵌套G-stage时保存的VS walk

    // === DDR请求跟踪 ===
    uint16_t    current_axi_id;      // 当前DDR请求的AXI ID
};
```

### 3.2 DDR响应结构

```cpp
struct ddr_response_t {
    uint16_t    axi_id;
    walk_type_t walk_type;       // 请求类型，用于路由
    uint32_t    task_id;         // 关联的任务ID
    uint8_t     status;          // 0=OK, 1=ERROR
    uint8_t     data[64];        // 读回数据
    uint32_t    data_size;
};
```

### 3.3 AXI ID 分配器与Outstanding表

```cpp
// AXI ID分配器
struct axi_id_allocator_t {
    std::queue<uint16_t> free_ids;
    sc_event             id_freed_evt;
    uint16_t             max_ids;       // 如64或256
    void init(uint16_t max);
    uint16_t alloc_id();               // 分配，池空则wait
    void free_id(uint16_t id);         // 归还，触发event
};

// Outstanding DDR请求表（全局唯一，在iommu_top中）
struct ddr_outstanding_entry_t {
    iommu_task_t*   task;
    walk_type_t     walk_type;
    uint64_t        expected_addr;
    uint8_t         expected_size;
};
std::unordered_map<uint16_t, ddr_outstanding_entry_t> ddr_outstanding_table;
```

---

## 四、FIFO定义与SC_THREAD声明

### 4.1 全部FIFO声明

```cpp
// ===================== 入站缓冲 =====================
sc_fifo<tlm::tlm_generic_payload*> inbound_fifo;              // 深度16

// ===================== Parser输出 =====================
// Parser发送iova+原始请求信息给Collector，Collector建立pending_tasks表按task_id跟踪
sc_fifo<iommu_task_t*> parser_to_collector_fifo;              // 深度16
sc_fifo<iommu_task_t*> parser_to_pq_fifo;                     // 深度4
// Parser直接向DC Cache和PC Cache发起查询（携带task_id）
sc_fifo<iommu_task_t*> parser_to_dc_cache_query_fifo;         // 深度8
sc_fifo<iommu_task_t*> parser_to_pc_cache_query_fifo;         // 深度8

// ===================== Cache查询结果返回Collector =====================
// DC/PC Cache查询完成后，将结果（含task_id）输出给Collector
sc_fifo<iommu_task_t*> dc_cache_to_collector_fifo;            // 深度8
sc_fifo<iommu_task_t*> pc_cache_to_collector_fifo;            // 深度8

// ===================== Collector到Cache (更新操作) =====================
// xDTW返回结果后Collector触发缓存更新
sc_fifo<iommu_task_t*> collector_to_dc_cache_update_fifo;     // 深度8
sc_fifo<iommu_task_t*> collector_to_pc_cache_update_fifo;     // 深度8

// ===================== Collector到Walker =====================
sc_fifo<iommu_task_t*> collector_to_xdtw_fifo;                // 深度4

// ===================== Collector到PT/MSIPT Cache (查询操作) =====================
sc_fifo<iommu_task_t*> collector_to_pt_cache_query_fifo;      // 深度8
sc_fifo<iommu_task_t*> collector_to_msipt_cache_query_fifo;   // 深度8

// ===================== xDTW返回 =====================
sc_fifo<iommu_task_t*> xdtw_to_collector_fifo;                // 深度4

// ===================== PT Cache 内部+外部FIFO =====================
sc_fifo<iommu_task_t*> pt_cache_lookup_result_fifo;           // 深度4 (内部: 查询进程 -> 结果处理进程)
sc_fifo<iommu_task_t*> pt_cache_to_ptw_fifo;                  // 深度4 (PT Cache miss -> PTW)
sc_fifo<iommu_task_t*> ptw_to_pt_cache_fifo;                  // 深度4 (PTW结果返回PT Cache)
sc_fifo<iommu_task_t*> pt_cache_to_fwd_fifo;                  // 深度8 (地址翻译完成 -> AXI Master 0)

// ===================== MSIPT Cache 内部+外部FIFO =====================
sc_fifo<iommu_task_t*> msipt_cache_lookup_result_fifo;        // 深度4 (内部: 查询进程 -> 结果处理进程)
sc_fifo<iommu_task_t*> msipt_cache_to_msiptw_fifo;            // 深度4 (MSIPT Cache miss -> 内置MSIPTW)
sc_fifo<iommu_task_t*> msiptw_to_msipt_cache_fifo;            // 深度4 (MSIPTW结果返回MSIPT Cache)
sc_fifo<iommu_task_t*> msipt_cache_to_fwd_fifo;               // 深度8 (MSI翻译完成 -> AXI Stream / AXI Master 2)

// ===================== 全局错误处理 =====================
// 各模块（Collector/PT Cache/MSIPT Cache/xDTW/PTW/MSIPTW）报错请求统一写入此FIFO
sc_fifo<iommu_task_t*> fault_fifo;                             // 深度8

// ===================== DDR响应FIFO（统一） =====================
// 所有DDR响应通过一个统一FIFO返回，通过ddr_response_t.walk_type区分路由
sc_fifo<ddr_response_t> ddr_rsp_fifo;                          // 深度16
```

### 4.2 DDR响应路由线程

由于DDR只有一个请求FIFO和一个响应FIFO，需要一个路由线程将DDR响应分发到各子模块：

```cpp
SC_THREAD(ddr_rsp_router_thread);
```

```
行为:
  loop:
    rsp = ddr_rsp_fifo.read()   // 等待DDR响应
    根据 rsp.walk_type 路由:
      WALK_DDT / WALK_PDT                          -> 放入 xdtw内部响应队列 (通过sc_event通知xdtw_rsp_thread)
      WALK_VS_PT / WALK_G_PT / WALK_G_PT_IMPLICIT  -> 放入 ptw内部响应队列 (通过sc_event通知ptw_rsp_thread)
      WALK_MSI_PT                                   -> 放入 msiptw内部响应队列 (通过sc_event通知msiptw_rsp_thread)
```

> **注意**: 各子模块内部维护自己的响应队列（`std::queue` + `sc_event`），DDR响应路由线程通过push+notify分发。

### 4.3 SC_THREAD声明

```cpp
// ==================== Parser ====================
SC_THREAD(parser_thread);                          // 解析入站请求，发起DC/PC Cache查询

// ==================== Collector (2个进程) ====================
SC_THREAD(collector_cache_lookup_result_thread);   // 进程1: 接收Parser任务+DC/PC Cache查询结果，按task_id匹配
SC_THREAD(collector_xdtw_response_thread);          // 进程2: 处理xDTW返回的DC/PC walk结果

// ==================== DC/PC Cache ====================
SC_THREAD(dc_cache_thread);                        // DC Cache: 处理Parser查询请求 + Collector更新请求
SC_THREAD(pc_cache_thread);                        // PC Cache: 处理Parser查询请求 + Collector更新请求

// ==================== PT Cache (3个进程) ====================
SC_THREAD(pt_cache_query_thread);                  // 进程1: IOTLB查询
SC_THREAD(pt_cache_result_thread);                 // 进程2: 查询结果处理(命中->转发, 未命中->PTW)
SC_THREAD(pt_cache_ptw_rsp_thread);                // 进程3: PTW响应处理，更新IOTLB缓存并转发

// ==================== MSIPT Cache (4个进程，含内置MSIPTW) ====================
SC_THREAD(msipt_cache_query_thread);               // 进程1: MSI Cache查询
SC_THREAD(msipt_cache_result_thread);              // 进程2: 查询结果处理(命中->路由输出, 未命中->MSIPTW)
SC_THREAD(msiptw_req_thread);                      // 进程3: 内置MSIPTW请求，执行向DDR的MSI页表查询
SC_THREAD(msiptw_rsp_thread);                      // 进程4: 内置MSIPTW响应，接收DDR响应，更新缓存并路由输出

// ==================== Walker模块 ====================
SC_THREAD(xdtw_req_thread);                        // xDTW请求进程: 处理Collector请求，发起DDR读取
SC_THREAD(xdtw_rsp_thread);                        // xDTW响应进程: 处理DDR响应，叶/非叶判断
SC_THREAD(ptw_req_thread);                         // PTW请求进程: 处理PT Cache miss请求，发起DDR读取
SC_THREAD(ptw_rsp_thread);                         // PTW响应进程: 处理DDR响应，权限检查，A/D位更新

// ==================== DDR响应路由 ====================
SC_THREAD(ddr_rsp_router_thread);                  // DDR响应统一路由分发

// ==================== 输出模块 ====================
SC_THREAD(forwarder_thread);                       // AXI Master 0 转发 (pt_cache_to_fwd_fifo)
SC_THREAD(msi_forwarder_thread);                   // AXI Stream / AXI Master 2 转发 (msipt_cache_to_fwd_fifo)

// ==================== Fault/Queue处理 ====================
SC_THREAD(fault_proc_thread);                      // 全局错误处理: 读取fault_fifo, 生成fault record, 写入FQ
SC_THREAD(fq_proc_thread);                         // Fault Queue处理
SC_THREAD(pq_proc_thread);                         // Page Request Queue处理
SC_THREAD(cq_proc_thread);                         // Command Queue处理
```

---

## 五、各子模块详细设计

### 5.1 Parser（解析器）

**职责**: 接收AXI Slave 0的入站请求，解析请求类型，创建`iommu_task_t`，直接向DC Cache和PC Cache发起查询，同时将任务信息发送给Collector。

**内部进程**: 1个（`parser_thread`）

**不涉及DDR访问，全部是纯逻辑。**

```
parser_thread:
  loop:
    payload = inbound_fifo.read()             // 阻塞等待入站请求
    提取PayloadExtention -> device_id, at, pid_valid, process_id等

    创建 iommu_task_t* task:
      task->task_id = ++global_task_counter
      task->timestamp = sc_time_stamp()
      填充所有原始请求字段

    (1) 判断请求类型:
    如果是PRI消息 (ext->msg_type == 0b0010 && ext->msg_code == PAGE_REQ):
      -> parser_to_pq_fifo.write(task)
      -> continue

    (2) 地址翻译请求处理:
    执行纯逻辑预处理（借鉴iommu_translate_iova steps 1~5）:
      - 检查 ddtp.iommu_mode:
        - Off -> task->state = TASK_FAULT, task->cause = 256
        - Bare -> task->is_bare_mode = 1, task->pa = iova, task->state = TASK_FORWARD
      - 提取DDI索引(用于后续DDT walk)
      - 推导TTYP和访问属性(is_read, is_write, is_exec, priv)
      - 检查device_id宽度是否超过max_devid_mask

    如果Bare模式:
      直接发往 pt_cache_to_fwd_fifo (无需翻译，直通转发)
    如果错误:
      -> fault_fifo
    否则（正常地址翻译流程）:
      task->state = TASK_PARSE_DONE
      task->dc_valid = false
      task->pc_valid = false
      // 同时执行3个写操作:
      -> parser_to_collector_fifo.write(task)          // 发送iova+原始请求信息给Collector
      -> parser_to_dc_cache_query_fifo.write(task)     // 直接向DC Cache发起查询（携带task_id）
      -> parser_to_pc_cache_query_fifo.write(task)     // 直接向PC Cache发起查询（携带task_id）

    wait(1, SC_NS)  // 模拟解析延迟
```

> **V3变更说明**: 与V2的关键区别是Parser不再将查询请求统一发给Collector转发，而是直接向DC Cache和PC Cache发送查询请求。Collector仅接收任务信息用于建立pending_tasks跟踪表。这样DC/PC Cache查询可以与Collector建表并行执行，减少延迟。

### 5.2 Collector（收集调度器）

**职责**: 核心调度模块，负责：
1. 接收Parser的任务信息，建立pending_tasks跟踪表
2. 收集DC/PC Cache查询结果，根据命中/未命中决定后续流程
3. 收集xDTW返回的DC/PC结果，更新缓存并继续流程
4. 当获取了DC和PC信息后，判断是发往PT Cache(页表翻译)还是MSIPT Cache(MSI翻译)

**内部进程**: 2个

**不涉及DDR访问，全部是纯逻辑判断。**

#### 进程1: collector_cache_lookup_result_thread

单线程监听3个FIFO：`parser_to_collector_fifo`、`dc_cache_to_collector_fifo`、`pc_cache_to_collector_fifo`。通过 `pending_tasks` 表按 `task_id` 匹配任务和Cache查询结果。

```
collector_cache_lookup_result_thread:

  // 内部维护: pending_tasks表 (task_id -> task)
  // 记录每个任务的DC和PC查询结果是否已到达

  loop:
    等待 parser_to_collector_fifo 或 dc_cache_to_collector_fifo 或 pc_cache_to_collector_fifo 非空

    如果 parser_to_collector_fifo 有数据:
      task = parser_to_collector_fifo.read()
      // 新任务到达，存入pending_tasks表
      pending_tasks[task->task_id] = task
      // 此时DC/PC Cache查询可能尚未返回，继续等待
      continue

    如果 dc_cache_to_collector_fifo 有数据:
      task = dc_cache_to_collector_fifo.read()
      // 按task_id查找pending_tasks
      pending_task = pending_tasks[task->task_id]
      pending_task->dc_valid = true
      pending_task->DC = task->DC   // 更新DC查询结果
      pending_task->state(dc) = task->state  // TASK_DC_HIT or TASK_DC_MISS
      // 检查PC结果是否也已到达
      如果 pending_task->pc_valid == false:
        continue  // 等待PC结果

    如果 pc_cache_to_collector_fifo 有数据:
      task = pc_cache_to_collector_fifo.read()
      pending_task = pending_tasks[task->task_id]
      pending_task->pc_valid = true
      pending_task->PC = task->PC
      pending_task->state(pc) = task->state  // TASK_PC_HIT or TASK_PC_MISS
      // 检查DC结果是否也已到达
      如果 pending_task->dc_valid == false:
        continue  // 等待DC结果

    // DC和PC结果都已到齐，从pending_tasks中取出任务，执行决策逻辑:
    task = pending_task
    从pending_tasks表中移除task->task_id

    (1) DC_HIT + PC_HIT:
      执行DC配置验证（借鉴iommu_translate_iova steps 7~13纯逻辑）:
        - 检查EN_ATS/T2GPA/PDTV等
        - 设置iosatp/iohgatp
      如果验证失败 -> fault_fifo
      判断是否需要PC:
        如果 DC.tc.PDTV == 1 && task->pid_valid:
          使用PC中的iosatp/SUM/PSCID（步骤15~16）
        否则:
          使用DC中的iosatp，忽略PC结果
      执行MSI地址判断并路由（见下方MSI判断逻辑）

    (2) DC_HIT + PC_MISS:
      判断是否真的需要PC:
        如果 DC.tc.PDTV == 1 && task->pid_valid && DC.fsc.pdtp.MODE != PDTP_Bare:
          确实需要PC -> collector_to_xdtw_fifo (发起PDT walk)
        否则:
          实际不需要PC，按DC_HIT + PC_HIT逻辑处理（忽略PC_MISS）

    (3) DC_MISS (无论PC结果如何):
      -> collector_to_xdtw_fifo (发起DDT walk)

    (4) 校验失败:
      -> fault_fifo

    wait(1, SC_NS)

  // ===== MSI地址判断与路由逻辑 =====
  MSI地址判断(task):
    如果 DC.msiptp.MODE != MSIPTP_Off:
      // 启用了MSI功能，使用DC中的掩码和模式信息判断当前地址是否为MSI地址
      // 借鉴 iommu_msi_trans.cc 的地址检测逻辑:
      //   检查gpa是否在MSI地址范围内（根据DC.msi_addr_mask和DC.msi_addr_pattern）
      is_msi = (task->gpa & DC.msi_addr_mask) == DC.msi_addr_pattern
      如果 is_msi:
        task->is_msi = 1
        -> collector_to_msipt_cache_query_fifo  // 执行MSI翻译流程
        return
    // 不是MSI地址或未启用MSI功能，执行常规地址翻译
    -> collector_to_pt_cache_query_fifo
```

#### 进程2: collector_xdtw_response_thread

处理xDTW返回的DC/PC walk结果。

```
collector_xdtw_response_thread:
  loop:
    task = xdtw_to_collector_fifo.read()

    如果 task->state == TASK_DC_WALK_DONE:
      // xDTW返回了DC
      // 触发DC Cache更新
      collector_to_dc_cache_update_fifo.write(task)

      判断是否需要查询PC:
        条件: task->pid_valid == 1 && task->DC.tc.PDTV == 1
              && task->DC.fsc.pdtp.MODE != PDTP_Bare
        如果需要PC:
          // 检查PC是否已经在前面的Cache查询中命中了
          如果 task->pc_valid && task->state(pc) == PC_HIT:
            直接使用已有PC -> 执行步骤15~16 -> MSI判断并路由到PT/MSIPT Cache
          否则:
            // 需要PDT walk获取PC
            -> collector_to_xdtw_fifo (PDT walk)
        如果不需要PC:
          执行DC配置验证(步骤7~13)
          设置iosatp/iohgatp（从DC）
          执行MSI地址判断并路由到 collector_to_pt_cache_query_fifo 或
                collector_to_msipt_cache_query_fifo

    如果 task->state == TASK_PC_WALK_DONE:
      // xDTW返回了PC（其关联的DC是前面Cache命中或walk得到的）
      // 触发PC Cache更新
      collector_to_pc_cache_update_fifo.write(task)
      // 执行DC+PC配置验证
      设置iosatp(从PC)/iohgatp(从DC)/SUM/PSCID
      执行MSI地址判断并路由到 collector_to_pt_cache_query_fifo 或
            collector_to_msipt_cache_query_fifo

    如果 task->state == TASK_FAULT:
      -> fault_fifo

    wait(1, SC_NS)
```

### 5.3 DC Cache

**职责**: 设备上下文缓存，提供**查询**和**更新**两个接口。容量扩展到64条。

**内部进程**: 1个（`dc_cache_thread`），处理两个输入FIFO。

**查询请求来自Parser（携带task_id），查询结果输出给Collector。**

```
dc_cache_thread:
  loop:
    等待 (parser_to_dc_cache_query_fifo 有数据 OR collector_to_dc_cache_update_fifo 有数据)

    如果是查询请求（parser_to_dc_cache_query_fifo）:
      task = parser_to_dc_cache_query_fifo.read()
      // 查询请求携带task_id，用于Collector后续匹配
      result = lookup_dc_cache(task->device_id, &task->DC)
      task->state = (result == HIT) ? TASK_DC_HIT : TASK_DC_MISS
      // 将查询结果（含task_id和DC信息）输出给Collector
      dc_cache_to_collector_fifo.write(task)
      wait(1, SC_NS)

    如果是更新请求（collector_to_dc_cache_update_fifo）:
      task = collector_to_dc_cache_update_fifo.read()
      update_dc_cache(task->device_id, &task->DC)
      wait(1, SC_NS)

接口函数:
  uint8_t lookup_dc_cache(uint32_t device_id, device_context_t* DC)
    // 64条目LRU查找，借鉴iommu_atc.cc的lookup_ioatc_dc逻辑
  void update_dc_cache(uint32_t device_id, device_context_t* DC)
    // LRU替换写入
```

### 5.4 PC Cache

**设计与DC Cache同构**，查询键为(device_id, process_id)。

**查询请求来自Parser（`parser_to_pc_cache_query_fifo`），查询结果输出给Collector（`pc_cache_to_collector_fifo`）。更新请求来自Collector（`collector_to_pc_cache_update_fifo`）。**

### 5.5 PT Cache（含转发逻辑）

**职责**: 
1. **查询**: 接收Collector的查询请求，查找IOTLB
2. **命中时转发**: 直接将翻译结果写入 `pt_cache_to_fwd_fifo` -> AXI Master 0
3. **未命中时**: 向PTW提交页表查询请求 (`pt_cache_to_ptw_fifo`)
4. **更新**: 接收PTW返回的结果 (`ptw_to_pt_cache_fifo`)，更新缓存，再转发

**内部进程**: 3个

#### 进程1: pt_cache_query_thread

执行IOTLB查询，将查询结果写入内部FIFO。

```
pt_cache_query_thread:
  loop:
    task = collector_to_pt_cache_query_fifo.read()

    result = lookup_iotlb(task->iova, task->PSCV, task->PSCID, task->GV, task->GSCID, ...)

    如果 HIT:
      填充task的pa/page_sz/vs_pte/g_pte
      task->state = TASK_TLB_HIT

    如果 MISS:
      task->state = TASK_TLB_MISS

    如果 FAULT (权限检查失败):
      task->state = TASK_FAULT

    pt_cache_lookup_result_fifo.write(task)    // 写入内部结果FIFO
    wait(1, SC_NS)
```

#### 进程2: pt_cache_result_thread

处理查询结果，根据命中/未命中决定路由。

```
pt_cache_result_thread:
  loop:
    task = pt_cache_lookup_result_fifo.read()

    如果 task->state == TASK_TLB_HIT:
      // 缓存命中，翻译完成，直接转发
      task->state = TASK_FORWARD
      pt_cache_to_fwd_fifo.write(task)

    如果 task->state == TASK_TLB_MISS:
      // 缓存未命中，发给PTW做页表walk
      pt_cache_to_ptw_fifo.write(task)

    如果 task->state == TASK_FAULT:
      fault_fifo.write(task)

    wait(1, SC_NS)
```

#### 进程3: pt_cache_ptw_rsp_thread

处理PTW返回的页表walk结果，更新IOTLB缓存并转发。

```
pt_cache_ptw_rsp_thread:
  loop:
    task = ptw_to_pt_cache_fifo.read()

    // 更新IOTLB缓存
    update_iotlb(task->iova, task->vs_pte, task->g_pte, task->pa, task->page_sz, ...)

    // 翻译完成，转发
    task->state = TASK_FORWARD
    pt_cache_to_fwd_fifo.write(task)

    wait(1, SC_NS)
```

### 5.6 MSIPT Cache（含内置MSIPTW）

**职责**:
1. **查询**: 接收Collector的MSI翻译查询请求
2. **命中时**: 根据msipte.M字段决定输出路径
3. **未命中时**: 将请求发送给内置MSIPTW从DDR获取MSI页表
4. **MSIPTW请求**: 向DDR发起MSI页表查询
5. **MSIPTW响应**: 接收DDR响应，更新缓存，根据msipte.M字段决定输出路径
6. **输出路由**:
   - `msipte.M == 1` (MRIF模式): 从msipte提取MRIF地址，写入MRIF -> **AXI Master 2**
   - `msipte.M == 3` (Basic Translate模式): 翻译MSI地址 -> **AXI Stream Master** (IMSIC)

**内部进程**: 4个

#### 进程1: msipt_cache_query_thread

执行MSI Cache查询，将查询结果写入内部FIFO。

```
msipt_cache_query_thread:
  loop:
    task = collector_to_msipt_cache_query_fifo.read()

    // MSI地址检测: 检查gpa是否匹配MSI pattern
    // 借鉴 iommu_msi_trans.cc 的地址检测逻辑
    // 计算interrupt_file_num
    result = lookup_msipt_cache(task->DC.msiptp, interrupt_file_num)

    如果 HIT:
      task->state = TASK_MSIPT_HIT
      // 将msipte数据附带在task中

    如果 MISS:
      task->state = TASK_MSIPT_MISS

    msipt_cache_lookup_result_fifo.write(task)
    wait(1, SC_NS)
```

#### 进程2: msipt_cache_result_thread

处理查询结果，命中时根据msipte.M字段决定输出路径；未命中时将请求发送给内置MSIPTW。

```
msipt_cache_result_thread:
  loop:
    task = msipt_cache_lookup_result_fifo.read()

    如果 task->state == TASK_MSIPT_HIT:
      // 命中，根据msipte.M字段决定输出路径
      执行MSI输出路由(task, msipte)  // 见下方输出路由逻辑

    如果 task->state == TASK_MSIPT_MISS:
      // 未命中，将请求发送给内置MSIPTW
      msipt_cache_to_msiptw_fifo.write(task)

    wait(1, SC_NS)

  // ===== MSI输出路由逻辑 =====
  MSI输出路由(task, msipte):
    检查 msipte.V == 0 -> task->state = TASK_FAULT, fault_fifo.write(task), return (cause=261)
    检查 msipte.M == 0 或 M == 2 -> task->state = TASK_FAULT, fault_fifo.write(task), return (cause=263)

    如果 msipte.M == 3 (Basic Translate模式):
      task->pa = msipte.translate_rw.PPN * PAGESIZE | (task->gpa & 0xFFF)
      task->is_msi = 1
      task->is_mrif = 0
      -> msipt_cache_to_fwd_fifo (路由到AXI Stream -> IMSIC)

    如果 msipte.M == 1 (MRIF模式):
      task->dest_mrif_addr = msipte.mrif.MRIF_ADDR_55_9 << 9
      task->pa = msipte.mrif.NPPN * PAGESIZE
      task->mrif_nid = (msipte.mrif.N10 << 10) | msipte.mrif.N90
      task->is_msi = 1
      task->is_mrif = 1
      -> msipt_cache_to_fwd_fifo (路由到AXI Master 2写入MRIF)
```

#### 进程3: msiptw_req_thread

内置MSIPTW请求进程，执行向DDR的MSI页表查询。

```
msiptw_req_thread:
  loop:
    task = msipt_cache_to_msiptw_fifo.read()

    // 计算MSI页表DDR读取地址
    // 借鉴 iommu_msi_trans.cc 的地址计算逻辑
    addr = task->DC.msiptp.PPN * PAGESIZE | (interrupt_file_num * 16)

    // 初始化walk_ctx
    task->walk_ctx.walk_type = WALK_MSI_PT
    task->walk_ctx.read_addr = addr
    task->walk_ctx.read_size = 16    // msipte大小为16字节

    // 分配AXI ID, 记录outstanding表
    axi_id = axi_id_allocator.alloc_id()
    task->current_axi_id = axi_id
    ddr_outstanding_table[axi_id] = {task, WALK_MSI_PT, addr, 16}

    // 发起非阻塞DDR读请求
    send_ddr_nb_read(addr, 16, axi_id, WALK_MSI_PT)

    wait(1, SC_NS)
```

#### 进程4: msiptw_rsp_thread

内置MSIPTW响应进程，接收DDR的响应信息，执行MSI Cache更新，然后根据msipte.M字段决定输出路径。

```
msiptw_rsp_thread:
  loop:
    等待内部DDR响应队列非空 (通过sc_event)

    rsp = 内部响应队列.pop()
    // 从outstanding表恢复task
    entry = ddr_outstanding_table[rsp.axi_id]
    task = entry.task
    释放AXI ID

    如果 rsp.status != OK:
      task->state = TASK_FAULT
      task->cause = xxx  // DDR访问错误
      fault_fifo.write(task)
      continue

    // 解析msipte(16字节)
    解析 rsp.data -> msipte

    // 更新MSIPT Cache
    update_msipt_cache(task->DC.msiptp, interrupt_file_num, msipte)

    // 根据msipte.M字段决定输出路径
    task->state = TASK_MSIPT_WALK_DONE
    执行MSI输出路由(task, msipte)  // 同进程2中的输出路由逻辑

    wait(1, SC_NS)
```

### 5.7 xDTW（DDT/PDT Walker）

**职责**: 执行DDT radix tree walk获取DC，执行PDT radix tree walk获取PC。

**重写实现**，借鉴`iommu_device_context.cc`和`iommu_process_context.cc`中的地址计算和验证逻辑。

**内部进程**: 2个

#### 进程1: xdtw_req_thread

处理来自Collector的请求（`collector_to_xdtw_fifo`），初始化walk上下文并发起DDR读取。

```
xdtw_req_thread:
  loop:
    task = collector_to_xdtw_fifo.read()

    判断walk类型:
      如果 task需要DDT walk (DC_MISS):
        初始化walk_ctx: walk_type=WALK_DDT
        根据ddtp.iommu_mode确定level数(1LVL/2LVL/3LVL)
        计算DDI索引: DDI[0]=device_id[6:0], DDI[1]=device_id[15:7], DDI[2]=device_id[23:16]
        base = ddtp.PPN * PAGESIZE
        addr = base + DDI[max_level-1] * 8

      如果 task需要PDT walk (PC_MISS):
        初始化walk_ctx: walk_type=WALK_PDT
        根据DC.fsc.pdtp.MODE确定level数(PD8/PD17/PD20)
        计算PDI索引
        base = DC.fsc.pdtp.PPN * PAGESIZE
        addr = base + PDI[max_level-1] * 8

    分配AXI ID, 记录outstanding表
    发起非阻塞DDR读请求

    wait(1, SC_NS)
```

#### 进程2: xdtw_rsp_thread

处理DDR响应（内部响应队列），判断返回类型、叶/非叶节点。

```
xdtw_rsp_thread:
  loop:
    等待内部DDR响应队列非空 (通过sc_event)

    rsp = 内部响应队列.pop()
    // 从outstanding表恢复task
    entry = ddr_outstanding_table[rsp.axi_id]
    task = entry.task
    释放AXI ID

    // 判断返回类型
    解析DDR返回数据:

      DDT walk (task->walk_ctx.walk_type == WALK_DDT):
        解析ddte (8字节)
        检查V位、reserved位

        如果是非叶节点 (level > 0):
          // 继续发起下一级请求
          level--
          计算下一级地址
          分配AXI ID, 记录outstanding表
          发起新DDR读
          continue

        如果是叶节点:
          读取完整DC (base格式32字节, ext格式64字节)
          执行DC配置检查（借鉴iommu_device_context.cc的验证逻辑）
          task->DC = 读到的DC
          task->state = TASK_DC_WALK_DONE
          xdtw_to_collector_fifo.write(task)

        如果错误:
          task->cause = xxx
          task->state = TASK_FAULT
          fault_fifo.write(task)

      PDT walk (task->walk_ctx.walk_type == WALK_PDT):
        解析pdte (8字节)
        类似DDT walk逻辑

        如果是非叶节点 (level > 0):
          level--, 计算下一级地址, 发起新DDR读
          continue

        如果是叶节点:
          读取PC (16字节)
          执行PC配置检查
          task->PC = 读到的PC
          task->state = TASK_PC_WALK_DONE
          xdtw_to_collector_fifo.write(task)

        如果错误:
          task->cause = xxx
          task->state = TASK_FAULT
          fault_fifo.write(task)

    wait(1, SC_NS)
```

### 5.8 PTW（Page Table Walker）

**最复杂的子模块**，重写实现两阶段翻译的页表walk。借鉴`iommu_two_stage_trans.cc`和`iommu_second_stage_trans.cc`的逻辑。

**关键特征**: 
- 支持VS-stage和G-stage嵌套walk
- A/D位更新必须使用**阻塞DDR接口**（原子操作）

**内部进程**: 2个

#### 进程1: ptw_req_thread

处理来自PT Cache的请求（`pt_cache_to_ptw_fifo`），初始化walk策略并发起DDR读取。

```
ptw_req_thread:
  loop:
    task = pt_cache_to_ptw_fifo.read()

    根据task中的iosatp/iohgatp确定walk策略:
      如果 iosatp.MODE == Bare && iohgatp.MODE == Bare:
        pa = iova, 直接完成 -> ptw_to_pt_cache_fifo
        continue
      如果 iosatp.MODE == Bare && iohgatp.MODE != Bare:
        仅需G-stage walk (GPA=IOVA -> SPA)
      如果 iosatp.MODE != Bare:
        需要VS-stage walk, 可能嵌套G-stage

    初始化walk_ctx:
      计算VPN索引, 设置level, PTESIZE
      计算PTE地址 = a + vpn[i] * PTESIZE
      如果G-stage活跃: 先保存VS ctx, 发起G隐式翻译
      否则: 直接发起PTE读取

    分配AXI ID, 记录outstanding表
    发起非阻塞DDR读

    wait(1, SC_NS)
```

#### 进程2: ptw_rsp_thread

处理DDR响应（内部响应队列），执行叶/非叶判断、权限检查、A/D位更新。

```
ptw_rsp_thread:
  loop:
    等待内部DDR响应队列非空 (通过sc_event)

    rsp = 内部响应队列.pop()
    // 从outstanding表恢复task
    entry = ddr_outstanding_table[rsp.axi_id]
    task = entry.task
    释放AXI ID

    根据walk_type分发处理:

      WALK_G_PT_IMPLICIT (VS PTE地址的G-stage隐式翻译):
        解析G PTE, 权限检查
        如果非叶: level--, 继续G walk, 发起新DDR读
        如果叶: 得到SPA, 恢复saved_vs_walk
          用SPA作为地址发起VS PTE读取

      WALK_VS_PT:
        解析VS PTE
        权限检查(V/R/W/X), 叶/非叶判断
        如果非叶: level--, 新地址
          如果G-stage活跃: 先做隐式G翻译
          否则: 直接读下一级PTE
        如果叶:
          权限检查, 超级页对齐检查
          **A/D位处理（阻塞原子操作）**:
            如果需要更新A/D位(SADE==1):
              使用 b_transport 发起原子读-改-写:
              1. read_memory_for_AMO (blocking)
              2. 比较PTE是否变化
              3. 设置A=1, 条件设置D=1
              4. write_memory (blocking, 原子写回)
              5. 如果PTE变化则重试
          如果需要G-stage显式翻译(GPA->SPA):
            发起G walk
          否则完成:
            -> ptw_to_pt_cache_fifo

      WALK_G_PT (显式G-stage翻译):
        解析G PTE, 处理类似VS
        **G-stage A/D位处理也需要阻塞原子操作**
        完成后 -> ptw_to_pt_cache_fifo

      如果错误:
        task->cause = xxx
        task->state = TASK_FAULT
        fault_fifo.write(task)

    wait(1, SC_NS)
```

### 5.9 Forwarder（DMA数据转发）

```
forwarder_thread:
  loop:
    task = pt_cache_to_fwd_fifo.read()
    // 修改原始TLM payload地址为翻译后的PA
    task->original_trans->set_address(task->pa)
    // 通过AXI Master 0发起nb_transport_fw转发
    // 等待nb_transport_bw响应（确保DMA实际完成）
    // 通过AXI Slave 0的nb_transport_bw返回响应给RP
    释放iommu_task_t
```

### 5.10 MSI Forwarder（MSI结果转发）

```
msi_forwarder_thread:
  loop:
    task = msipt_cache_to_fwd_fifo.read()

    如果 task->is_mrif == 0 (M==3, Basic Translate):
      // 走AXI Stream Master -> IMSIC
      设置地址为翻译后的MSI物理地址
      通过 axi_stream_to_cmn_rnd_socket 发送

    如果 task->is_mrif == 1 (M==1, MRIF模式):
      // 走AXI Master 2 -> 写入MRIF内存
      // **MRIF写入需要原子OR操作（blocking）**
      设置地址为 task->dest_mrif_addr
      通过 axi_master_1_to_cmn_rnd_socket 使用 b_transport 发送（原子OR写）

    释放iommu_task_t
```

### 5.11 Fault Proc（全局错误处理）

**职责**: 统一处理各模块上报的错误请求，生成fault record并写入FQ。

**内部进程**: 1个（`fault_proc_thread`）

```
fault_proc_thread:
  loop:
    task = fault_fifo.read()

    // 根据task中的错误信息生成fault record
    // 借鉴 iommu_faults.cc 的逻辑
    生成fault_record:
      fault_record.CAUSE = task->cause
      fault_record.TTYP  = task->TTYP
      fault_record.iotval = task->iotval
      fault_record.iotval2 = task->iotval2
      fault_record.DID   = task->device_id
      fault_record.PID   = task->process_id
      fault_record.PV    = task->pid_valid

    // 写入FQ（阻塞DDR写入）
    // 检查FQ是否已满
    如果FQ已满:
      设置fqof (FQ overflow)
    否则:
      使用b_transport写入FQ内存（32字节fault record）
      更新fqt (FQ tail指针)

    // 触发中断（如果配置了fie.FIE）
    如果中断使能:
      触发fault中断

    // 返回响应给RP（如果需要）
    如果 task->original_trans != nullptr:
      设置错误响应
      通过AXI Slave 0的nb_transport_bw返回

    释放iommu_task_t

    wait(1, SC_NS)
```

---

## 六、DDR端口设计：阻塞+非阻塞双模式

### 6.1 需要阻塞(blocking)接口的操作

经代码分析，以下操作**必须使用b_transport阻塞接口**：

| 操作 | 原因 | 代码位置 |
|------|------|---------|
| **A/D位更新(S/VS-stage)** | CAS原子操作：read_for_AMO + compare + write_back + retry | `iommu_two_stage_trans.cc:437-471` |
| **A/D位更新(G-stage)** | 同上 | `iommu_second_stage_trans.cc:364-396` |
| **MRIF写入** | 原子OR写操作，将中断标识位原子设置到MRIF内存 | `iommu_msi_trans.cc:242` (硬件规范要求) |
| **FQ写入** | 32字节fault record必须原子写入，写失败需设置fqmf | `iommu_faults.cc:126-156` |
| **PQ写入** | 16字节page request record原子写入 | `iommu_ats.cc:282-293` |

> **A/D位更新详解**: 硬件使用原子OR操作更新A/D位。在功能模型中实现为CAS循环：
> 1. `read_memory_for_AMO()` 读取当前PTE
> 2. 与之前读到的PTE比较，如果不一致则重试整个walk
> 3. 设置A=1（如果是写操作且W==1则D=1）
> 4. `write_memory()` 原子写回
> 这必须是阻塞操作以保证原子性。

### 6.2 可以使用非阻塞(nb_transport)接口的操作

| 操作 | 说明 |
|------|------|
| DDT entry读取 | xDTW读DDT radix tree节点(8字节) |
| PDT entry读取 | xDTW读PDT radix tree节点(8字节) |
| DC读取 | xDTW读设备上下文(32/64字节) |
| PC读取 | xDTW读进程上下文(16字节) |
| VS-stage PTE读取 | PTW读VS页表项(8字节) |
| G-stage PTE读取 | PTW读G页表项(8字节) |
| MSI PTE读取 | MSIPTW读MSI页表项(16字节) |
| CQ读取 | CQ Proc读命令队列条目(16字节) |
| 翻译后DMA数据转发 | Forwarder转发到AXI Master 0 |

### 6.3 DDR端口实现

AXI Master 2（代码中`axi_master_1_to_cmn_rnd_socket`）需要**同时注册b_transport和nb_transport**：

```cpp
// 非阻塞DDR读请求（xDTW/PTW/MSIPTW使用）
void send_ddr_nb_read(uint64_t addr, uint8_t size, uint16_t axi_id, walk_type_t type) {
    auto* trans = create_ddr_payload(TLM_READ_COMMAND, addr, size, axi_id);
    tlm::tlm_phase phase = tlm::BEGIN_REQ;
    sc_time delay = SC_ZERO_TIME;
    axi_master_1_to_cmn_rnd_socket->nb_transport_fw(*trans, phase, delay);
}

// 阻塞DDR读写（A/D更新、MRIF写入、FQ/PQ写入使用）
void send_ddr_blocking_read(uint64_t addr, uint8_t size, char* data) {
    tlm::tlm_generic_payload trans;
    sc_time delay = SC_ZERO_TIME;
    // 设置payload...
    axi_master_1_to_cmn_rnd_socket->b_transport(trans, delay);
    memcpy(data, trans.get_data_ptr(), size);
}

void send_ddr_blocking_write(uint64_t addr, uint8_t size, char* data) {
    tlm::tlm_generic_payload trans;
    sc_time delay = SC_ZERO_TIME;
    // 设置payload...
    axi_master_1_to_cmn_rnd_socket->b_transport(trans, delay);
}
```

### 6.4 DDR模型改造（`ddr/test_ddr.hh`）

DDR模型需要**同时支持b_transport和nb_transport**：

```
DDR_Module:
  // 保留b_transport处理函数（用于原子操作）
  void b_transport(tlm_generic_payload& trans, sc_time& delay) {
    // 同步执行读写 + 添加固定延迟
    执行内存读写
    delay += (cmd==READ) ? read_latency : write_latency;
  }

  // 新增nb_transport处理函数（用于并发非阻塞访问）
  tlm_sync_enum nb_transport_fw(tlm_generic_payload& trans, tlm_phase& phase, sc_time& delay) {
    if (phase == BEGIN_REQ) {
      执行内存读写
      // 使用SC_THREAD或payload event queue延迟响应
      sc_spawn 一个延迟线程:
        wait(read_latency)
        nb_transport_bw(trans, BEGIN_RESP, delay)
      return TLM_ACCEPTED;
    }
  }

  可配置参数:
    sc_time read_latency  = 10ns
    sc_time write_latency = 8ns
    uint32_t max_outstanding = 16
```

---

## 七、数据排序与一致性

### 7.1 同设备请求排序

规范要求：AXI Slave 0的响应必须等到AXI Master 0的响应后才能返回。

**方案**：Forwarder中按device_id维护per-device排序队列：
```cpp
std::map<uint32_t, std::queue<iommu_task_t*>> device_order_queues;
```
每个设备的响应严格按请求到达顺序返回。

### 7.2 Cache Invalidation同步

使用 `sc_mutex` 保护缓存数据结构：
- DC/PC/TLB/MSIPT Cache各用一个 `sc_mutex`
- 查询/更新时 lock -> 操作 -> unlock
- CQ Proc执行invalidation时 lock -> invalidate -> unlock

IOFENCE命令处理时等待所有in-flight翻译完成。

---

## 八、文件修改清单

### 需要新建的文件

| 文件 | 说明 |
|------|------|
| `iommu/iommu_task.hh` | `iommu_task_t`, `walk_context_t`, `ddr_response_t`, `axi_id_allocator_t`, 状态枚举 |
| `iommu/iommu_perf_parser.cc` | Parser子模块实现 |
| `iommu/iommu_perf_collector.cc` | Collector子模块实现（2个进程） |
| `iommu/iommu_perf_dc_cache.cc` | DC Cache子模块实现 |
| `iommu/iommu_perf_pc_cache.cc` | PC Cache子模块实现 |
| `iommu/iommu_perf_pt_cache.cc` | PT Cache子模块实现（3个进程） |
| `iommu/iommu_perf_msipt_cache.cc` | MSIPT Cache子模块实现（4个进程，含内置MSIPTW） |
| `iommu/iommu_perf_xdtw.cc` | xDTW子模块实现（2个进程） |
| `iommu/iommu_perf_ptw.cc` | PTW子模块实现（2个进程） |
| `iommu/iommu_perf_forwarder.cc` | Forwarder + MSI Forwarder实现 |
| `iommu/iommu_perf_fault_proc.cc` | Fault Proc子模块实现 |

### 需要重大修改的文件

| 文件 | 修改内容 |
|------|---------|
| `iommu/iommu_top.hh` | 添加全部sc_fifo声明（含新增的parser_to_dc/pc_cache_query_fifo、fault_fifo等）、23个SC_THREAD声明、nb_transport回调、AXI ID分配器、outstanding表 |
| `iommu/iommu_top.cc` | 注册所有SC_THREAD和nb_transport回调；before_end_of_elaboration初始化 |
| `ddr/test_ddr.hh` | 同时支持b_transport和nb_transport、延迟响应、多outstanding |
| `iommu/iommu_ref_api.cc` | 提供阻塞和非阻塞两套DDR访问接口 |

### 需要中等修改的文件

| 文件 | 修改内容 |
|------|---------|
| `iommu/param_trans_def.hh` | `PayloadExtention`添加`axi_id`和`walk_type`字段 |
| `rp/test_rp.hh` | socket改nb_transport, 添加nb_transport_bw回调 |
| `rp/test_rp_func.cc` | 发送函数改为非阻塞 |
| `rp/test_rp_thread.cc` | 测试线程支持并发请求 |
| `iommu/iommu_struct.hh` | 扩大缓存大小常量 |
| `iommu/iommu_atc.hh` | DDT_CACHE_SIZE=64, 扩大TLB_SIZE, 添加MSIPT缓存结构 |

### 需要小修改的文件

| 文件 | 修改内容 |
|------|---------|
| `main.cpp` | 增大仿真时间 |
| `Makefile` | 添加新源文件编译 |

### 保留为功能参考（不修改）

| 文件 | 原因 |
|------|------|
| `iommu/iommu_translate.cc/.hh` | 功能参考模型：翻译核心逻辑 |
| `iommu/iommu_device_context.cc` | 功能参考模型：DC定位和验证 |
| `iommu/iommu_process_context.cc` | 功能参考模型：PC定位和验证 |
| `iommu/iommu_two_stage_trans.cc` | 功能参考模型：VS-stage walk |
| `iommu/iommu_second_stage_trans.cc` | 功能参考模型：G-stage walk |
| `iommu/iommu_msi_trans.cc` | 功能参考模型：MSI翻译 |
| `iommu/iommu_registers.hh` | 纯数据结构定义 |
| `iommu/iommu_data_structures.hh` | 纯数据结构定义 |
| `iommu/iommu_fault.hh` | 纯数据结构定义 |
| `iommu/iommu_utils.hh/.cc` | 工具函数 |
| `iommu/iommu_hpm.hh/.cc` | 性能计数器 |
| `iommu/iommu_interrupt.hh/.cc` | 中断生成 |

---

## 九、实施步骤

### Phase 1: 基础设施
1. 创建 `iommu_task.hh` - 定义所有数据结构和枚举
2. 修改 `iommu_top.hh` - 添加全部FIFO（含新增FIFO）、23个SC_THREAD声明、AXI ID分配器
3. 修改 `param_trans_def.hh` 添加 axi_id/walk_type
4. 修改 `iommu_atc.hh` 扩大缓存容量

### Phase 2: DDR模型改造
1. DDR模型添加 nb_transport_fw 并保留 b_transport
2. 实现可配置延迟响应
3. 验证阻塞和非阻塞双模式

### Phase 3: 核心管线实现
1. Parser + Forwarder (Bare mode直通验证)
2. DC Cache + PC Cache (查询来自Parser，结果输出到Collector，更新来自Collector)
3. xDTW (2个进程: 请求+响应，DDT/PDT walk异步化)
4. Collector (2个进程: 单线程监听3个FIFO按task_id匹配 + xDTW响应处理)
5. PT Cache (3个进程: 查询/结果处理/PTW响应) + PTW (2个进程: 请求/响应，含A/D位阻塞操作)
6. MSIPT Cache (4个进程: 查询/结果处理/MSIPTW请求/MSIPTW响应)
7. DDR响应路由线程
8. Fault Proc (全局错误处理)

### Phase 4: RP模型改造与并发测试
1. RP改为nb_transport
2. 并发请求发送测试
3. 多设备并发翻译验证

### Phase 5: 排序、一致性、错误路径
1. per-device响应排序
2. cache invalidation同步
3. IOFENCE处理
4. FQ/PQ写入
5. 错误路径测试（验证fault_fifo全链路）

---

## 十、验证方法

### 10.1 编译验证
```bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
make clean && make DEBUG=1
```

### 10.2 功能正确性验证
- 复用现有3设备测试用例，确保翻译结果与功能参考模型一致
- Bare mode直通 / Sv48x4 G-stage / Sv39 S-stage
- MSI翻译(M==1 MRIF + M==3 Basic Translate)
- A/D位更新正确性

### 10.3 并发性验证
- 多设备同时发送翻译请求
- DC/PC Cache命中/未命中混合场景
- DDR outstanding请求数压力测试
- per-device响应顺序验证

### 10.4 性能指标观测
- 每笔翻译端到端延迟
- DDR访问次数和Cache命中率
- 管线利用率

---

## 十一、风险与注意事项

1. **各模块重写代码量大**: 各子模块需要重新实现硬件逻辑，而不仅仅是封装现有函数。需要深入理解参考模型中每一步的含义。

2. **嵌套Walk复杂性**: VS-stage每级PTE可能触发完整G-stage walk（最多5x5=25次DDR访问），PTW中需维护双层walk上下文栈。

3. **阻塞/非阻塞混用**: 同一个DDR端口混用b_transport和nb_transport时，TLM-2.0协议要求特别注意线程安全和时序。建议使用sc_mutex保护DDR端口的阻塞调用。

4. **DC+PC并行查询一致性**: Parser同时触发DC和PC查询，但PC查询可能在DC_MISS时无意义。Collector需要在DC_MISS时忽略PC结果，避免竞态。

5. **SystemC协作式线程**: `sc_fifo::write()`在FIFO满时阻塞当前线程。所有FIFO深度需仔细评估，或使用`nb_write()`+重试。

6. **TLM Payload生命周期**: nb_transport模式下payload不能提前释放。DDR请求需使用独立payload实例，避免与原始请求混用。

7. **调试复杂度**: 异步后调试困难。所有子模块的关键步骤应打印task_id用于端到端追踪。

8. **fault_fifo并发写入**: 多个模块可能同时向fault_fifo写入错误请求，sc_fifo本身支持多生产者单消费者模式，但需确保FIFO深度足够。

---

## 附录A：Thread ↔ FIFO 完整映射表

| Thread名称 | 所属模块 | 输入FIFO | 输出FIFO |
|---|---|---|---|
| parser_thread | Parser | inbound_fifo | parser_to_collector_fifo, parser_to_dc_cache_query_fifo, parser_to_pc_cache_query_fifo, parser_to_pq_fifo, pt_cache_to_fwd_fifo(bare), fault_fifo(error) |
| dc_cache_thread | DC Cache | parser_to_dc_cache_query_fifo, collector_to_dc_cache_update_fifo | dc_cache_to_collector_fifo |
| pc_cache_thread | PC Cache | parser_to_pc_cache_query_fifo, collector_to_pc_cache_update_fifo | pc_cache_to_collector_fifo |
| collector_cache_lookup_result_thread | Collector | parser_to_collector_fifo, dc_cache_to_collector_fifo, pc_cache_to_collector_fifo | collector_to_xdtw_fifo, collector_to_pt_cache_query_fifo, collector_to_msipt_cache_query_fifo, fault_fifo |
| collector_xdtw_response_thread | Collector | xdtw_to_collector_fifo | collector_to_dc_cache_update_fifo, collector_to_pc_cache_update_fifo, collector_to_xdtw_fifo, collector_to_pt_cache_query_fifo, collector_to_msipt_cache_query_fifo, fault_fifo |
| xdtw_req_thread | xDTW | collector_to_xdtw_fifo | DDR(nb) |
| xdtw_rsp_thread | xDTW | DDR响应(内部队列) | xdtw_to_collector_fifo, fault_fifo |
| pt_cache_query_thread | PT Cache | collector_to_pt_cache_query_fifo | pt_cache_lookup_result_fifo |
| pt_cache_result_thread | PT Cache | pt_cache_lookup_result_fifo | pt_cache_to_fwd_fifo, pt_cache_to_ptw_fifo, fault_fifo |
| pt_cache_ptw_rsp_thread | PT Cache | ptw_to_pt_cache_fifo | pt_cache_to_fwd_fifo |
| ptw_req_thread | PTW | pt_cache_to_ptw_fifo | DDR(nb) |
| ptw_rsp_thread | PTW | DDR响应(内部队列) | ptw_to_pt_cache_fifo, fault_fifo |
| msipt_cache_query_thread | MSIPT Cache | collector_to_msipt_cache_query_fifo | msipt_cache_lookup_result_fifo |
| msipt_cache_result_thread | MSIPT Cache | msipt_cache_lookup_result_fifo | msipt_cache_to_fwd_fifo, msipt_cache_to_msiptw_fifo |
| msiptw_req_thread | MSIPT(内置MSIPTW) | msipt_cache_to_msiptw_fifo | DDR(nb) |
| msiptw_rsp_thread | MSIPT(内置MSIPTW) | DDR响应(内部队列) | msipt_cache_to_fwd_fifo, fault_fifo |
| ddr_rsp_router_thread | DDR路由 | ddr_rsp_fifo | xdtw/ptw/msiptw内部响应队列 |
| forwarder_thread | Forwarder | pt_cache_to_fwd_fifo | AXI Master 0 |
| msi_forwarder_thread | MSI Forwarder | msipt_cache_to_fwd_fifo | AXI Stream / AXI Master 2 |
| fault_proc_thread | Fault处理 | fault_fifo | FQ(DDR blocking) |
| fq_proc_thread | FQ | fq内部 | DDR |
| pq_proc_thread | PQ | parser_to_pq_fifo | DDR |
| cq_proc_thread | CQ | cq内部 | DDR |
