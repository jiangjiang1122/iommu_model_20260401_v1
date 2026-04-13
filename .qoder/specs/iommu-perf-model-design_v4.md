# IOMMU 性能模型重构设计方案 (V4)

## Context

当前 IOMMU 模型是一个纯功能级(Functional Level)的 SystemC TLM-2.0 模型，所有通信使用阻塞式 `b_transport`，一次只能处理一个翻译请求。本方案将其重构为近似时序级(Approximately Timed, AT)性能模型。

**设计原则**：
- 现有 Rivos 参考模型代码（`iommu_translate.cc`、`iommu_device_context.cc` 等）**保留不修改**，作为功能参考模型
- 各硬件子模块按照实际硬件功能**重新实现代码**，可借鉴参考模型中的数据结构定义、地址计算逻辑、验证检查逻辑等
- 非阻塞 socket 通信支持并发请求
- 按硬件子模块划分 SC_THREAD，每个子模块有独立的输入 buffer
- 异步 DDR 访问，通过 AXI ID 标识请求，上下文暂存本地
- DDR 端口同时支持阻塞和非阻塞两种模式（原子操作必须使用阻塞接口）

**V4 相对 V3 的核心变更**：
1. **新增 Walker Cache 子模块**：参考架构文档 4.5 章节，在 PTW 模块内集成 Walker Cache（含 PTWc_1/PTWc_2/PTWc_3 三级中间结果缓存），减少页表 walk 的 DDR 访问次数
2. **新增 Cache Invalidation 通道**：CQ Proc 通过 `cq_to_cache_inv_fifo` 向各 Cache 模块（DC/PC/PT/Walker Cache）发送失效命令
3. **对照架构文档 5.1 概述校验**：补充 CQ Proc 通过 AXI Master 1 向 RP 发送 ATS Msg 的描述

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
|  collector_to_dc/pc_       |   pt_cache_to_ptw_fifo        |
|  cache_update_fifo         |      |                    |                             |
|     |          |           |      v                    v                             |
|     v          v           |   [PTW + Walker Cache] [MSIPTW]                        |
|  DC Cache   PC Cache       |   (2进程:请求+响应)    (2进程:请求+响应)                   |
|  (更新)     (更新)          |   内置Walker Cache     DDR(nb)-->                       |
|                            |   (PTWc_1/2/3)            |                             |
|                            |   DDR(nb)-->              |                             |
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
|  ====== Cache失效通道 ======                                                         |
|  [CQ Proc] --cq_to_cache_inv_fifo--> DC/PC/PT/Walker Cache                         |
|  [CQ Proc] --AXI Master 1--> RP (ATS Invalidation/Page Rsp)                        |
|  [CQ Proc] --DDR(AXI Master 2)--> CQ读取                                            |
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

    // === Walker Cache命中信息 (V4新增) ===
    int8_t      wc_hit_level;        // Walker Cache命中的层级(-1=未命中, 1/2/3=PTWc_x命中)
    uint64_t    wc_hit_ppn;          // Walker Cache命中时的中间PPN
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

### 3.4 Walker Cache 数据结构 (V4新增)

参考架构文档 4.5 章节设计。Walker Cache 缓存页表 walk 两阶段/一阶段地址翻译过程中 VA/GPA 到下一级页表物理地址 PPN 的中间结果，目标是减少 DDR 访问次数。

```cpp
// ==================== Walker Cache缓存条目 ====================

// PTWc_1: 缓存VPN[3]级中间结果（Sv48专用，Sv39时对应VPN[2]）
// 直接映射, s=64条目
struct walker_cache_entry_1_t {
    bool        valid;
    uint16_t    gscid;           // 16比特
    uint32_t    pscid;           // 20比特
    uint16_t    vpn_high;        // VPN最高段 (VPN[3] for Sv48, VPN[2] for Sv39)
    uint64_t    ppn;             // 中间结果PPN (44比特)
    uint8_t     addr_mode;       // 地址模式标志 (Sv39/Sv48, VS/G-stage)
};

// PTWc_2: 缓存{VPN[3],VPN[2]}级中间结果
// 2-way组相连, s=64条目/way
struct walker_cache_entry_2_t {
    bool        valid;
    uint16_t    gscid;
    uint32_t    pscid;
    uint16_t    vpn_high;        // VPN最高段
    uint16_t    vpn_mid;         // VPN次高段 (VPN[2] for Sv48, VPN[1] for Sv39)
    uint64_t    ppn;
    uint8_t     addr_mode;
    uint8_t     rrpv;            // SRRIP替换算法值 (M=2比特)
};

// PTWc_3: 缓存{VPN[3],VPN[2],VPN[1]}级中间结果
// 4-way组相连, s=64条目/way (仅Sv48启用, Sv39关闭)
struct walker_cache_entry_3_t {
    bool        valid;
    uint16_t    gscid;
    uint32_t    pscid;
    uint16_t    vpn_high;
    uint16_t    vpn_mid;
    uint16_t    vpn_low;         // VPN[1] for Sv48
    uint64_t    ppn;
    uint8_t     addr_mode;
    uint8_t     rrpv;
};

// Walker Cache容量配置
#define WALKER_CACHE_SET_SIZE   64
#define PTWC1_WAYS              1   // 直接映射
#define PTWC2_WAYS              2   // 2-way组相连
#define PTWC3_WAYS              4   // 4-way组相连

// Walker Cache管理结构（内置于PTW模块中）
struct walker_cache_t {
    walker_cache_entry_1_t ptwc_1[WALKER_CACHE_SET_SIZE];
    walker_cache_entry_2_t ptwc_2[PTWC2_WAYS][WALKER_CACHE_SET_SIZE];
    walker_cache_entry_3_t ptwc_3[PTWC3_WAYS][WALKER_CACHE_SET_SIZE];
    sc_mutex               wc_mutex;    // 保护并发访问

    // 索引散列函数 (参考架构文档4.5.4)
    // PTWc_1: hash_1 = (gscid ^ pscid ^ vpn_high) & (s-1)
    // PTWc_2: hash_2 = (gscid ^ pscid ^ vpn_high ^ vpn_mid) & (s-1)
    // PTWc_3: hash_3 = (gscid ^ pscid ^ vpn_high ^ vpn_mid ^ vpn_low) & (s-1)
    uint16_t hash_1(uint16_t gscid, uint32_t pscid, uint16_t vpn_high);
    uint16_t hash_2(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint16_t vpn_mid);
    uint16_t hash_3(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint16_t vpn_mid, uint16_t vpn_low);

    // 查询接口 (按优先级: PTWc_3 > PTWc_2 > PTWc_1)
    // 返回: 命中级别(1/2/3), 命中的PPN; 0=未命中
    int lookup(uint16_t gscid, uint32_t pscid, uint64_t iova, uint8_t addr_mode, uint64_t* ppn);

    // 更新接口 (PTW walk完成后更新中间结果)
    void update_ptwc1(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint64_t ppn, uint8_t mode);
    void update_ptwc2(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint16_t vpn_mid, uint64_t ppn, uint8_t mode);
    void update_ptwc3(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint16_t vpn_mid, uint16_t vpn_low, uint64_t ppn, uint8_t mode);

    // 失效接口
    void invalidate_by_gscid(uint16_t gscid);                              // DC失效触发
    void invalidate_by_gscid_pscid(uint16_t gscid, uint32_t pscid);       // PC失效触发
    void invalidate_by_iova(uint16_t gscid, uint32_t pscid, uint64_t iova); // 页表修改触发
    void invalidate_all();                                                   // 全部失效
};
```

### 3.5 Cache Invalidation 命令结构 (V4新增)

```cpp
// Cache失效命令类型
enum cache_inv_type_t {
    INV_DDT,            // IOTINVAL.DDT - 失效DC Cache + 关联的PT/Walker Cache
    INV_VMA,            // IOTINVAL.VMA - 失效PT Cache + Walker Cache
    INV_IOFENCE,        // IOFENCE.C - 等待所有in-flight翻译完成
    INV_ATS_INVAL,      // ATS.INVAL - 向RP发送ATS Invalidation Request
    INV_ATS_PRGR,       // ATS.PRGR - 向RP发送Page Request Group Response
};

// Cache失效命令结构
struct cache_inv_cmd_t {
    cache_inv_type_t type;
    uint32_t    device_id;       // DDT失效时使用
    uint32_t    process_id;      // VMA失效时使用
    uint16_t    gscid;           // 用于PT/Walker Cache失效
    uint32_t    pscid;
    uint64_t    addr;            // VMA失效时的地址
    bool        gv;              // gscid有效
    bool        pscidv;          // pscid有效
    bool        av;              // addr有效
    uint8_t     func3;           // IOFENCE功能码
    // ATS Msg相关字段
    uint32_t    dseg;
    uint32_t    rid;
    uint64_t    payload;
};
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
sc_fifo<iommu_task_t*> dc_cache_to_collector_fifo;            // 深度8
sc_fifo<iommu_task_t*> pc_cache_to_collector_fifo;            // 深度8

// ===================== Collector到Cache (更新操作) =====================
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
sc_fifo<iommu_task_t*> fault_fifo;                             // 深度8

// ===================== Cache Invalidation (V4新增) =====================
// CQ Proc向各Cache模块发送失效命令
sc_fifo<cache_inv_cmd_t> cq_to_cache_inv_fifo;                // 深度8

// ===================== DDR响应FIFO（统一） =====================
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
      WALK_DDT / WALK_PDT                          -> 放入 xdtw内部响应队列 (通知xdtw_rsp_thread)
      WALK_VS_PT / WALK_G_PT / WALK_G_PT_IMPLICIT  -> 放入 ptw内部响应队列 (通知ptw_rsp_thread)
      WALK_MSI_PT                                   -> 放入 msiptw内部响应队列 (通知msiptw_rsp_thread)
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
SC_THREAD(xdtw_req_thread);                        // xDTW请求进程
SC_THREAD(xdtw_rsp_thread);                        // xDTW响应进程
SC_THREAD(ptw_req_thread);                         // PTW请求进程(含Walker Cache查询)
SC_THREAD(ptw_rsp_thread);                         // PTW响应进程(含Walker Cache更新)

// ==================== DDR响应路由 ====================
SC_THREAD(ddr_rsp_router_thread);

// ==================== 输出模块 ====================
SC_THREAD(forwarder_thread);                       // AXI Master 0 转发
SC_THREAD(msi_forwarder_thread);                   // AXI Stream / AXI Master 2 转发

// ==================== Fault/Queue处理 ====================
SC_THREAD(fault_proc_thread);                      // 全局错误处理
SC_THREAD(fq_proc_thread);                         // Fault Queue处理
SC_THREAD(pq_proc_thread);                         // Page Request Queue处理
SC_THREAD(cq_proc_thread);                         // Command Queue处理 + Cache Invalidation分发
```

---

## 五、各子模块详细设计

### 5.1 Parser（解析器）

**职责**: 接收AXI Slave 0的入站请求，解析请求类型，创建`iommu_task_t`，直接向DC Cache和PC Cache发起查询，同时将任务信息发送给Collector。对无法处理的错误包进行丢弃和错误上报。

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
      task->wc_hit_level = -1                 // Walker Cache初始化为未命中

    (1) 判断请求类型:
    如果是PRI消息 (ext->msg_type == 0b0010 && ext->msg_code == PAGE_REQ):
      -> parser_to_pq_fifo.write(task)
      -> continue

    如果是无法处理的错误包:
      丢弃，生成错误上报 -> fault_fifo
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
      直接发往 pt_cache_to_fwd_fifo
    如果错误:
      -> fault_fifo
    否则（正常地址翻译流程）:
      task->state = TASK_PARSE_DONE
      task->dc_valid = false
      task->pc_valid = false
      -> parser_to_collector_fifo.write(task)
      -> parser_to_dc_cache_query_fifo.write(task)
      -> parser_to_pc_cache_query_fifo.write(task)

    wait(1, SC_NS)
```

### 5.2 Collector（收集调度器）

**职责**: 核心调度模块，负责：
1. 接收Parser的任务信息，建立pending_tasks跟踪表
2. 收集DC/PC Cache查询结果，根据命中/未命中决定后续流程
3. 收集xDTW返回的DC/PC结果，更新缓存并继续流程
4. 当获取了DC和PC信息后，判断是发往PT Cache(页表翻译)还是MSIPT Cache(MSI翻译)

**内部进程**: 2个

**不涉及DDR访问，全部是纯逻辑判断。**

#### 进程1: collector_cache_lookup_result_thread

单线程监听3个FIFO，通过 `pending_tasks` 表按 `task_id` 匹配。

```
collector_cache_lookup_result_thread:

  // 内部维护: pending_tasks表 (task_id -> task)

  loop:
    等待 parser_to_collector_fifo 或 dc_cache_to_collector_fifo 或 pc_cache_to_collector_fifo 非空

    如果 parser_to_collector_fifo 有数据:
      task = parser_to_collector_fifo.read()
      pending_tasks[task->task_id] = task
      continue

    如果 dc_cache_to_collector_fifo 有数据:
      task = dc_cache_to_collector_fifo.read()
      pending_task = pending_tasks[task->task_id]
      pending_task->dc_valid = true
      pending_task->DC = task->DC
      如果 pending_task->pc_valid == false: continue

    如果 pc_cache_to_collector_fifo 有数据:
      task = pc_cache_to_collector_fifo.read()
      pending_task = pending_tasks[task->task_id]
      pending_task->pc_valid = true
      pending_task->PC = task->PC
      如果 pending_task->dc_valid == false: continue

    // DC和PC结果都已到齐
    task = pending_task
    从pending_tasks表中移除task->task_id

    (1) DC_HIT + PC_HIT:
      执行DC配置验证（步骤7~13纯逻辑）
      如果验证失败 -> fault_fifo
      判断是否需要PC -> 设置iosatp/iohgatp
      执行MSI地址判断并路由

    (2) DC_HIT + PC_MISS:
      判断是否真的需要PC:
        如果需要 -> collector_to_xdtw_fifo (PDT walk)
        否则按DC_HIT + PC_HIT逻辑处理

    (3) DC_MISS:
      -> collector_to_xdtw_fifo (DDT walk)

    (4) 校验失败:
      -> fault_fifo

    wait(1, SC_NS)

  // MSI地址判断逻辑:
  如果 DC.msiptp.MODE != MSIPTP_Off:
    is_msi = (task->gpa & DC.msi_addr_mask) == DC.msi_addr_pattern
    如果 is_msi -> collector_to_msipt_cache_query_fifo
  否则 -> collector_to_pt_cache_query_fifo
```

#### 进程2: collector_xdtw_response_thread

```
collector_xdtw_response_thread:
  loop:
    task = xdtw_to_collector_fifo.read()

    如果 task->state == TASK_DC_WALK_DONE:
      collector_to_dc_cache_update_fifo.write(task)
      判断是否需要PC:
        如果需要且PC未命中 -> collector_to_xdtw_fifo (PDT walk)
        如果需要且PC已命中 -> 执行配置验证 -> MSI判断 -> 路由
        如果不需要 -> 执行配置验证 -> MSI判断 -> 路由

    如果 task->state == TASK_PC_WALK_DONE:
      collector_to_pc_cache_update_fifo.write(task)
      设置iosatp/iohgatp/SUM/PSCID
      执行MSI判断 -> 路由

    如果 task->state == TASK_FAULT:
      -> fault_fifo

    wait(1, SC_NS)
```

### 5.3 DC Cache

**职责**: 设备上下文缓存。查询请求来自Parser（携带task_id），查询结果输出给Collector。容量64条。

**内部进程**: 1个（`dc_cache_thread`）

```
dc_cache_thread:
  loop:
    等待 (parser_to_dc_cache_query_fifo 或 collector_to_dc_cache_update_fifo 有数据)

    如果是查询请求（parser_to_dc_cache_query_fifo）:
      task = fifo.read()
      result = lookup_dc_cache(task->device_id, &task->DC)
      task->state = (result == HIT) ? TASK_DC_HIT : TASK_DC_MISS
      dc_cache_to_collector_fifo.write(task)
      wait(1, SC_NS)

    如果是更新请求（collector_to_dc_cache_update_fifo）:
      task = fifo.read()
      update_dc_cache(task->device_id, &task->DC)
      wait(1, SC_NS)
```

### 5.4 PC Cache

**设计与DC Cache同构**，查询键为(device_id, process_id)。查询请求来自Parser（`parser_to_pc_cache_query_fifo`），查询结果输出给Collector（`pc_cache_to_collector_fifo`）。

### 5.5 PT Cache（含转发逻辑）

**职责**: IOTLB查询、命中转发、未命中提交PTW、接收PTW结果更新缓存。

**内部进程**: 3个

#### 进程1: pt_cache_query_thread

```
pt_cache_query_thread:
  loop:
    task = collector_to_pt_cache_query_fifo.read()
    result = lookup_iotlb(task->iova, task->PSCV, task->PSCID, task->GV, task->GSCID, ...)
    如果 HIT: 填充pa/page_sz, task->state = TASK_TLB_HIT
    如果 MISS: task->state = TASK_TLB_MISS
    如果 FAULT: task->state = TASK_FAULT
    pt_cache_lookup_result_fifo.write(task)
    wait(1, SC_NS)
```

#### 进程2: pt_cache_result_thread

```
pt_cache_result_thread:
  loop:
    task = pt_cache_lookup_result_fifo.read()
    如果 TASK_TLB_HIT: task->state = TASK_FORWARD -> pt_cache_to_fwd_fifo
    如果 TASK_TLB_MISS: -> pt_cache_to_ptw_fifo
    如果 TASK_FAULT: -> fault_fifo
    wait(1, SC_NS)
```

#### 进程3: pt_cache_ptw_rsp_thread

```
pt_cache_ptw_rsp_thread:
  loop:
    task = ptw_to_pt_cache_fifo.read()
    update_iotlb(task->iova, task->vs_pte, task->g_pte, task->pa, task->page_sz, ...)
    task->state = TASK_FORWARD
    pt_cache_to_fwd_fifo.write(task)
    wait(1, SC_NS)
```

### 5.6 MSIPT Cache（含内置MSIPTW）

**职责**: MSI页表缓存查询与翻译，内置MSIPTW。

**内部进程**: 4个

#### 进程1: msipt_cache_query_thread

```
msipt_cache_query_thread:
  loop:
    task = collector_to_msipt_cache_query_fifo.read()
    result = lookup_msipt_cache(task->DC.msiptp, interrupt_file_num)
    如果 HIT: task->state = TASK_MSIPT_HIT
    如果 MISS: task->state = TASK_MSIPT_MISS
    msipt_cache_lookup_result_fifo.write(task)
    wait(1, SC_NS)
```

#### 进程2: msipt_cache_result_thread

```
msipt_cache_result_thread:
  loop:
    task = msipt_cache_lookup_result_fifo.read()
    如果 TASK_MSIPT_HIT:
      执行MSI输出路由(task, msipte)
    如果 TASK_MSIPT_MISS:
      msipt_cache_to_msiptw_fifo.write(task)
    wait(1, SC_NS)

  MSI输出路由(task, msipte):
    检查 msipte.V == 0 -> FAULT (cause=261)
    检查 msipte.M == 0 或 M == 2 -> FAULT (cause=263)
    如果 msipte.M == 3: task->pa = PPN<<12 | gpa[11:0], is_mrif=0 -> msipt_cache_to_fwd_fifo
    如果 msipte.M == 1: 提取MRIF信息, is_mrif=1 -> msipt_cache_to_fwd_fifo
```

#### 进程3: msiptw_req_thread

```
msiptw_req_thread:
  loop:
    task = msipt_cache_to_msiptw_fifo.read()
    addr = task->DC.msiptp.PPN * PAGESIZE | (interrupt_file_num * 16)
    task->walk_ctx.walk_type = WALK_MSI_PT
    分配AXI ID, 记录outstanding表, 发起非阻塞DDR读
    wait(1, SC_NS)
```

#### 进程4: msiptw_rsp_thread

```
msiptw_rsp_thread:
  loop:
    等待内部DDR响应队列非空
    rsp = 内部响应队列.pop()
    恢复task, 释放AXI ID
    如果错误: -> fault_fifo, continue
    解析msipte(16字节), 更新MSIPT Cache
    task->state = TASK_MSIPT_WALK_DONE
    执行MSI输出路由(task, msipte)
    wait(1, SC_NS)
```

### 5.7 xDTW（DDT/PDT Walker）

**职责**: 执行DDT/PDT radix tree walk获取DC/PC。

**内部进程**: 2个

#### 进程1: xdtw_req_thread

```
xdtw_req_thread:
  loop:
    task = collector_to_xdtw_fifo.read()
    判断walk类型(DDT/PDT), 初始化walk_ctx
    计算DDR读取地址, 分配AXI ID, 发起非阻塞DDR读
    wait(1, SC_NS)
```

#### 进程2: xdtw_rsp_thread

```
xdtw_rsp_thread:
  loop:
    等待内部DDR响应队列非空
    rsp = 内部响应队列.pop()
    恢复task, 释放AXI ID

    DDT walk:
      解析ddte, 检查V位
      非叶节点: level--, 计算下一级地址, 发起新DDR读, continue
      叶节点: 读取完整DC, 配置检查
        task->state = TASK_DC_WALK_DONE -> xdtw_to_collector_fifo
      错误: -> fault_fifo

    PDT walk:
      类似DDT walk逻辑
      叶节点: 读取PC, 配置检查
        task->state = TASK_PC_WALK_DONE -> xdtw_to_collector_fifo
      错误: -> fault_fifo

    wait(1, SC_NS)
```

### 5.8 PTW（Page Table Walker，含内置Walker Cache）

**最复杂的子模块**，重写实现两阶段翻译的页表walk。借鉴`iommu_two_stage_trans.cc`和`iommu_second_stage_trans.cc`的逻辑。

**关键特征**:
- 支持VS-stage和G-stage嵌套walk
- A/D位更新必须使用**阻塞DDR接口**（原子操作）
- **内置Walker Cache**：三级中间结果缓存（PTWc_1/PTWc_2/PTWc_3），减少DDR访问

**内部进程**: 2个

**内置数据结构**: `walker_cache_t`（见3.4节）

#### 5.8.1 Walker Cache 设计（参考架构文档4.5章节）

Walker Cache 缓存页表 walk 过程中 VA/GPA 到下一级页表物理地址 PPN 的中间结果。

**三级缓存表结构**:

| 缓存表 | 缓存内容 | 组织方式 | 容量 |
|--------|----------|----------|------|
| PTWc_1 | VPN最高段对应的中间PPN | 直接映射 | 64条目 |
| PTWc_2 | VPN最高两段对应的中间PPN | 2-way组相连 | 2×64条目 |
| PTWc_3 | VPN最高三段对应的中间PPN | 4-way组相连 | 4×64条目 (仅Sv48) |

**地址模式兼容**：
- Sv48: 4级页表 (VPN[3]/VPN[2]/VPN[1]/VPN[0])，PTWc_1/2/3 全部启用
- Sv39: 3级页表 (VPN[2]/VPN[1]/VPN[0])，PTWc_3 关闭，PTWc_1 缓存 VPN[2]，PTWc_2 缓存 {VPN[2],VPN[1]}

**散列函数** (参考架构文档4.5.4):
```
PTWc_1: hash_1 = (gscid[5:0] ^ pscid[5:0] ^ vpn_high[5:0]) & 0x3F
PTWc_2: hash_2 = (gscid[5:0] ^ pscid[5:0] ^ vpn_high[5:0] ^ vpn_mid[5:0]) & 0x3F
PTWc_3: hash_3 = (gscid[5:0] ^ pscid[5:0] ^ vpn_high[5:0] ^ vpn_mid[5:0] ^ vpn_low[5:0]) & 0x3F
```

**缓存替换**: PTWc_1 直接映射无替换；PTWc_2/PTWc_3 使用 SRRIP 算法（2比特RRPV）。

**查询优先级**: PTWc_3 > PTWc_2 > PTWc_1。命中更高级别的缓存可以跳过更多walk层级。

**Walker Cache 查询逻辑**（在 ptw_req_thread 中执行）:
```
walker_cache_lookup(task):
  从iova中提取VPN字段:
    Sv48: vpn[3], vpn[2], vpn[1], vpn[0]
    Sv39: vpn[2], vpn[1], vpn[0]

  // 按优先级查询 (同时查询3个缓存表，取最优命中)
  如果 Sv48 且 PTWc_3命中(gscid, pscid, vpn[3], vpn[2], vpn[1]):
    // 跳过3级walk，直接从level 0开始
    task->wc_hit_level = 3
    task->wc_hit_ppn = ptwc3_result.ppn
    起始PTE地址 = ppn * PAGESIZE + vpn[0] * PTESIZE
    return

  如果 PTWc_2命中(gscid, pscid, vpn_high, vpn_mid):
    // 跳过2级walk
    task->wc_hit_level = 2
    task->wc_hit_ppn = ptwc2_result.ppn
    起始PTE地址 = ppn * PAGESIZE + vpn_next * PTESIZE
    return

  如果 PTWc_1命中(gscid, pscid, vpn_high):
    // 跳过1级walk
    task->wc_hit_level = 1
    task->wc_hit_ppn = ptwc1_result.ppn
    起始PTE地址 = ppn * PAGESIZE + vpn_next * PTESIZE
    return

  // 全部未命中，从根开始walk
  task->wc_hit_level = -1
```

**Walker Cache 更新逻辑**（在 ptw_rsp_thread 中执行）:
```
walker_cache_update(task, level, ppn):
  // 在页表walk过程中，每完成一级非叶PTE读取，更新对应的Walker Cache
  // level表示刚完成的层级

  如果完成了最高层(root) -> 更新PTWc_1
  如果完成了第二层 -> 更新PTWc_2
  如果完成了第三层(仅Sv48) -> 更新PTWc_3
```

**Walker Cache 失效逻辑**（参考架构文档4.5.5）:
```
失效触发源:
  1. DC Cache失效时: 按gscid遍历Walker Cache，失效匹配条目
  2. PC Cache失效时: 按(gscid, pscid)遍历Walker Cache，失效匹配条目
  3. 页表修改时(IOTINVAL.VMA): 按(gscid, pscid, iova)失效匹配条目
  4. 全局失效(IOTINVAL.VMA without GV/PSCIDV/AV): 失效全部条目

失效命令通过 cq_to_cache_inv_fifo 接收，在PTW模块中处理。
```

#### 5.8.2 进程1: ptw_req_thread

处理来自PT Cache的请求，**先查询Walker Cache**，根据命中情况确定walk起始层级，再发起DDR读取。

```
ptw_req_thread:
  loop:
    // === 优先处理Cache失效命令 ===
    如果 cq_to_cache_inv_fifo 有数据 (非阻塞检查):
      cmd = cq_to_cache_inv_fifo.nb_read()
      如果 cmd.type == INV_VMA:
        执行Walker Cache失效:
          如果 cmd.gv && cmd.pscidv && cmd.av:
            walker_cache.invalidate_by_iova(cmd.gscid, cmd.pscid, cmd.addr)
          如果 cmd.gv && cmd.pscidv:
            walker_cache.invalidate_by_gscid_pscid(cmd.gscid, cmd.pscid)
          如果 cmd.gv:
            walker_cache.invalidate_by_gscid(cmd.gscid)
          否则:
            walker_cache.invalidate_all()
      如果 cmd.type == INV_DDT:
        walker_cache.invalidate_by_gscid(cmd.gscid)
      continue

    // === 处理页表walk请求 ===
    task = pt_cache_to_ptw_fifo.read()

    根据task中的iosatp/iohgatp确定walk策略:
      如果 iosatp.MODE == Bare && iohgatp.MODE == Bare:
        pa = iova, 直接完成 -> ptw_to_pt_cache_fifo
        continue
      如果 iosatp.MODE == Bare && iohgatp.MODE != Bare:
        仅需G-stage walk
      如果 iosatp.MODE != Bare:
        需要VS-stage walk, 可能嵌套G-stage

    // ===== V4新增: 查询Walker Cache =====
    wc_result = walker_cache.lookup(task->GSCID, task->PSCID, task->iova, addr_mode, &ppn)

    如果 wc_result > 0 (Walker Cache命中):
      // 从命中的中间PPN开始walk，跳过已缓存的层级
      task->wc_hit_level = wc_result
      task->wc_hit_ppn = ppn
      根据命中级别计算起始PTE地址:
        remaining_level = max_level - wc_result
        addr = ppn * PAGESIZE + vpn[remaining_level] * PTESIZE
      如果G-stage活跃且需要隐式翻译: 先做隐式G翻译
      否则: 直接发起PTE读取
    否则 (Walker Cache未命中):
      // 从根PPN开始完整walk
      task->wc_hit_level = -1
      计算VPN索引, 设置level
      addr = root_ppn * PAGESIZE + vpn[max_level-1] * PTESIZE
      如果G-stage活跃: 先做隐式G翻译
      否则: 直接发起PTE读取

    初始化walk_ctx
    分配AXI ID, 记录outstanding表, 发起非阻塞DDR读

    wait(1, SC_NS)
```

#### 5.8.3 进程2: ptw_rsp_thread

处理DDR响应，执行叶/非叶判断、权限检查、A/D位更新。**在非叶节点处理时更新Walker Cache**。

```
ptw_rsp_thread:
  loop:
    等待内部DDR响应队列非空 (通过sc_event)

    rsp = 内部响应队列.pop()
    entry = ddr_outstanding_table[rsp.axi_id]
    task = entry.task
    释放AXI ID

    根据walk_type分发处理:

      WALK_G_PT_IMPLICIT (VS PTE地址的G-stage隐式翻译):
        解析G PTE, 权限检查
        如果非叶: level--, 继续G walk
        如果叶: 得到SPA, 恢复saved_vs_walk
          用SPA作为地址发起VS PTE读取

      WALK_VS_PT:
        解析VS PTE
        权限检查(V/R/W/X), 叶/非叶判断

        如果非叶:
          // ===== V4新增: 更新Walker Cache =====
          // 将当前非叶PTE的PPN作为中间结果更新到Walker Cache
          已完成的层级数 = max_level - current_level
          如果 已完成的层级数 == 1:
            walker_cache.update_ptwc1(gscid, pscid, vpn_high, pte.ppn, mode)
          如果 已完成的层级数 == 2:
            walker_cache.update_ptwc2(gscid, pscid, vpn_high, vpn_mid, pte.ppn, mode)
          如果 已完成的层级数 == 3 && Sv48:
            walker_cache.update_ptwc3(gscid, pscid, vpn_high, vpn_mid, vpn_low, pte.ppn, mode)

          level--, 计算新地址
          如果G-stage活跃: 先做隐式G翻译
          否则: 直接读下一级PTE

        如果叶:
          权限检查, 超级页对齐检查
          **A/D位处理（阻塞原子操作）**:
            如果需要更新A/D位(SADE==1):
              使用 b_transport 发起原子读-改-写
          如果需要G-stage显式翻译: 发起G walk
          否则完成: -> ptw_to_pt_cache_fifo

      WALK_G_PT (显式G-stage翻译):
        解析G PTE, 类似VS处理
        // G-stage walk也可以利用Walker Cache (用于缓存G-stage中间PPN)
        **G-stage A/D位处理也需要阻塞原子操作**
        完成后 -> ptw_to_pt_cache_fifo

      如果错误:
        task->state = TASK_FAULT -> fault_fifo

    wait(1, SC_NS)
```

### 5.9 Forwarder（DMA数据转发）

```
forwarder_thread:
  loop:
    task = pt_cache_to_fwd_fifo.read()
    task->original_trans->set_address(task->pa)
    // 通过AXI Master 0发起nb_transport_fw转发
    // 等待nb_transport_bw响应后，通过AXI Slave 0回复
    释放iommu_task_t
```

### 5.10 MSI Forwarder（MSI结果转发）

```
msi_forwarder_thread:
  loop:
    task = msipt_cache_to_fwd_fifo.read()
    如果 task->is_mrif == 0 (M==3, Basic Translate):
      通过 axi_stream_to_cmn_rnd_socket -> IMSIC
    如果 task->is_mrif == 1 (M==1, MRIF模式):
      通过 axi_master_1_to_cmn_rnd_socket 使用 b_transport (原子OR写入MRIF)
    释放iommu_task_t
```

### 5.11 Fault Proc（全局错误处理）

```
fault_proc_thread:
  loop:
    task = fault_fifo.read()
    生成fault_record (CAUSE, TTYP, iotval, iotval2, DID, PID, PV)
    如果FQ已满: 设置fqof
    否则: 使用b_transport写入FQ内存（32字节）, 更新fqt
    如果中断使能: 触发fault中断
    如果 task->original_trans != nullptr: 设置错误响应
    释放iommu_task_t
    wait(1, SC_NS)
```

### 5.12 CQ Proc（Command Queue处理）(V4补充)

**职责**（参考架构文档5.1）:
1. 处理A核通过Command Queue下发的各种指令
2. 通过 AXI Master 2 接口读取CQ实体
3. 通过 AXI Master 1 接口向RP发送ATS Invalidation Request和Page Request Group Response等Msg
4. 向内部各Cache模块发送Cache Invalidation命令

**内部进程**: 1个（`cq_proc_thread`）

```
cq_proc_thread:
  loop:
    // 检查CQ是否有新的命令（cqh != cqt）
    如果 cqh == cqt: wait(cq_event), continue

    // 通过AXI Master 2读取CQ条目（16字节）
    使用b_transport从DDR读取CQ条目

    解析CQ命令 (参考RISC-V IOMMU Spec第4章):

      IOTINVAL.VMA:
        // 失效PT Cache和Walker Cache中的页表条目
        构建 cache_inv_cmd_t: type=INV_VMA, 填充gscid/pscid/addr/gv/pscidv/av
        cq_to_cache_inv_fifo.write(cmd)
        // PT Cache失效: 根据(gscid, pscid, iova)失效匹配条目
        // Walker Cache失效: 同步在PTW模块中处理

      IOTINVAL.DDT:
        // 失效DC Cache + 关联的PC Cache + PT Cache + Walker Cache
        构建 cache_inv_cmd_t: type=INV_DDT, 填充device_id
        cq_to_cache_inv_fifo.write(cmd)

      IOFENCE.C:
        // 等待所有in-flight翻译请求完成
        等待所有outstanding任务完成
        更新cqh

      ATS.INVAL:
        // 通过AXI Master 1向指定RP发送ATS Invalidation Request
        根据DSEG/RID确定目标RP (参考RPx路由配置)
        通过 axi_master_2_to_pcie_noc_socket 发送ATS Invalidation Msg

      ATS.PRGR:
        // 通过AXI Master 1向指定RP发送Page Request Group Response
        通过 axi_master_2_to_pcie_noc_socket 发送ATS Page Group Response Msg

    更新cqh (CQ Head指针)

    wait(1, SC_NS)
```

---

## 六、DDR端口设计：阻塞+非阻塞双模式

### 6.1 需要阻塞(blocking)接口的操作

| 操作 | 原因 |
|------|------|
| **A/D位更新(S/VS-stage)** | CAS原子操作：read_for_AMO + compare + write_back + retry |
| **A/D位更新(G-stage)** | 同上 |
| **MRIF写入** | 原子OR写操作 |
| **FQ写入** | 32字节fault record原子写入 |
| **PQ写入** | 16字节page request record原子写入 |

### 6.2 可以使用非阻塞(nb_transport)接口的操作

| 操作 | 说明 |
|------|------|
| DDT/PDT entry读取 | xDTW (8字节) |
| DC/PC读取 | xDTW (32/64/16字节) |
| VS/G-stage PTE读取 | PTW (8字节) |
| MSI PTE读取 | MSIPTW (16字节) |
| CQ读取 | CQ Proc (16字节) |
| 翻译后DMA数据转发 | Forwarder -> AXI Master 0 |

### 6.3 DDR端口实现

```cpp
void send_ddr_nb_read(uint64_t addr, uint8_t size, uint16_t axi_id, walk_type_t type);
void send_ddr_blocking_read(uint64_t addr, uint8_t size, char* data);
void send_ddr_blocking_write(uint64_t addr, uint8_t size, char* data);
```

### 6.4 DDR模型改造

DDR模型同时支持b_transport和nb_transport，可配置延迟参数。

---

## 七、数据排序与一致性

### 7.1 同设备请求排序

Forwarder中按device_id维护per-device排序队列。

### 7.2 Cache Invalidation同步

使用 `sc_mutex` 保护缓存数据结构（DC/PC/TLB/MSIPT/Walker Cache各用一个）。

CQ Proc执行invalidation时通过 `cq_to_cache_inv_fifo` 向各模块发送失效命令。IOFENCE命令处理时等待所有in-flight翻译完成。

**Walker Cache失效传播链**（参考架构文档4.5.5）:
- DC Cache失效 → 按gscid失效Walker Cache条目
- PC Cache失效 → 按(gscid, pscid)失效Walker Cache条目
- 页表修改(IOTINVAL.VMA) → 按(gscid, pscid, iova)失效Walker Cache条目

---

## 八、文件修改清单

### 需要新建的文件

| 文件 | 说明 |
|------|------|
| `iommu/iommu_task.hh` | `iommu_task_t`, `walk_context_t`, `ddr_response_t`, `cache_inv_cmd_t`, Walker Cache数据结构 |
| `iommu/iommu_perf_parser.cc` | Parser子模块实现 |
| `iommu/iommu_perf_collector.cc` | Collector子模块实现（2个进程） |
| `iommu/iommu_perf_dc_cache.cc` | DC Cache子模块实现 |
| `iommu/iommu_perf_pc_cache.cc` | PC Cache子模块实现 |
| `iommu/iommu_perf_pt_cache.cc` | PT Cache子模块实现（3个进程） |
| `iommu/iommu_perf_msipt_cache.cc` | MSIPT Cache子模块实现（4个进程，含内置MSIPTW） |
| `iommu/iommu_perf_xdtw.cc` | xDTW子模块实现（2个进程） |
| `iommu/iommu_perf_ptw.cc` | PTW子模块实现（2个进程，含Walker Cache） |
| `iommu/iommu_perf_walker_cache.cc` | Walker Cache实现（PTWc_1/2/3, 散列函数, 查询/更新/失效） |
| `iommu/iommu_perf_forwarder.cc` | Forwarder + MSI Forwarder实现 |
| `iommu/iommu_perf_fault_proc.cc` | Fault Proc子模块实现 |
| `iommu/iommu_perf_cq_proc.cc` | CQ Proc子模块实现（含Cache Invalidation分发） |

### 需要重大修改的文件

| 文件 | 修改内容 |
|------|---------|
| `iommu/iommu_top.hh` | 全部FIFO（含新增cq_to_cache_inv_fifo）、SC_THREAD、Walker Cache实例、AXI ID分配器 |
| `iommu/iommu_top.cc` | 注册SC_THREAD和nb_transport回调 |
| `ddr/test_ddr.hh` | 同时支持b_transport和nb_transport |
| `iommu/iommu_ref_api.cc` | 阻塞+非阻塞双套DDR访问接口 |

### 需要中等修改的文件

| 文件 | 修改内容 |
|------|---------|
| `iommu/param_trans_def.hh` | 添加axi_id/walk_type字段 |
| `rp/test_rp.hh` | socket改nb_transport |
| `rp/test_rp_func.cc` | 发送函数改非阻塞 |
| `rp/test_rp_thread.cc` | 并发请求 |
| `iommu/iommu_struct.hh` | 扩大缓存大小常量 |
| `iommu/iommu_atc.hh` | 扩大Cache容量, 添加MSIPT/Walker Cache结构 |

### 保留为功能参考（不修改）

`iommu_translate.cc/.hh`、`iommu_device_context.cc`、`iommu_process_context.cc`、`iommu_two_stage_trans.cc`、`iommu_second_stage_trans.cc`、`iommu_msi_trans.cc`、`iommu_registers.hh`、`iommu_data_structures.hh`、`iommu_fault.hh`、`iommu_utils.hh/.cc`、`iommu_hpm.hh/.cc`、`iommu_interrupt.hh/.cc`

---

## 九、实施步骤

### Phase 1: 基础设施
1. 创建 `iommu_task.hh` - 所有数据结构（含Walker Cache、cache_inv_cmd_t）
2. 修改 `iommu_top.hh` - 全部FIFO、SC_THREAD声明、Walker Cache实例
3. 修改 `param_trans_def.hh` 添加 axi_id/walk_type
4. 修改 `iommu_atc.hh` 扩大缓存容量

### Phase 2: DDR模型改造
1. DDR模型添加 nb_transport_fw 并保留 b_transport
2. 实现可配置延迟响应

### Phase 3: 核心管线实现
1. Parser + Forwarder (Bare mode直通)
2. DC Cache + PC Cache
3. xDTW (2进程)
4. Collector (2进程)
5. PT Cache (3进程) + PTW (2进程, 含Walker Cache)
6. Walker Cache (PTWc_1/2/3, 散列/查询/更新/失效)
7. MSIPT Cache (4进程)
8. DDR响应路由线程
9. Fault Proc + CQ Proc (含Cache Invalidation分发)

### Phase 4: RP模型改造与并发测试
1. RP改为nb_transport
2. 并发请求发送测试

### Phase 5: 排序、一致性、错误路径
1. per-device响应排序
2. Cache Invalidation全链路 (CQ Proc -> DC/PC/PT/Walker Cache)
3. IOFENCE处理
4. FQ/PQ写入
5. Walker Cache命中率统计与验证

---

## 十、验证方法

### 10.1 编译验证
```bash
make clean && make DEBUG=1
```

### 10.2 功能正确性验证
- Bare mode直通 / Sv48x4 G-stage / Sv39 S-stage
- MSI翻译(M==1 MRIF + M==3 Basic Translate)
- A/D位更新正确性
- **Walker Cache命中路径验证**: 连续相同VPN前缀的翻译请求，第二次应命中Walker Cache

### 10.3 Walker Cache专项验证
- **命中率测试**: 同一设备连续递增IOVA翻译，验证PTWc_1/2/3命中率
- **失效正确性**: IOTINVAL.VMA后Walker Cache条目正确失效
- **Sv39/Sv48兼容**: Sv39模式下PTWc_3自动关闭
- **DDR访问减少**: 对比启用/禁用Walker Cache的DDR访问次数

### 10.4 并发性验证
- 多设备同时发送翻译请求
- DC/PC Cache命中/未命中混合场景
- DDR outstanding请求数压力测试
- per-device响应顺序验证

### 10.5 Cache Invalidation全链路验证
- CQ下发IOTINVAL.DDT -> DC Cache + Walker Cache失效
- CQ下发IOTINVAL.VMA -> PT Cache + Walker Cache失效
- CQ下发IOFENCE.C -> 等待所有in-flight翻译完成

### 10.6 性能指标观测
- 每笔翻译端到端延迟
- DDR访问次数和各级Cache命中率（DC/PC/PT/Walker Cache）
- 管线利用率

---

## 十一、风险与注意事项

1. **各模块重写代码量大**: 各子模块需要重新实现硬件逻辑。

2. **嵌套Walk复杂性**: VS-stage每级PTE可能触发完整G-stage walk（最多5x5=25次DDR访问），PTW中需维护双层walk上下文栈。

3. **阻塞/非阻塞混用**: 同一个DDR端口混用b_transport和nb_transport时需注意线程安全。建议使用sc_mutex保护阻塞调用。

4. **DC+PC并行查询一致性**: Parser同时触发DC和PC查询，Collector需在DC_MISS时忽略PC结果。

5. **SystemC协作式线程**: `sc_fifo::write()`在FIFO满时阻塞。所有FIFO深度需仔细评估。

6. **TLM Payload生命周期**: DDR请求需使用独立payload实例。

7. **调试复杂度**: 所有子模块的关键步骤应打印task_id用于端到端追踪。

8. **fault_fifo并发写入**: 多个模块同时写入，需确保FIFO深度足够。

9. **Walker Cache一致性**: Walker Cache更新和失效操作需通过sc_mutex保护，避免与PTW的查询/更新操作产生竞态。Walker Cache失效必须在页表walk开始前生效。

10. **Walker Cache与超级页**: 超级页（如2MB/1GB页）在非叶节点即可确定最终映射，Walker Cache中缓存的中间PPN在超级页场景下需要正确处理叶节点标识。

---

## 附录A：与架构文档5.1概述对照校验

| 架构文档5.1模块 | 性能模型对应 | 校验状态 |
|---|---|---|
| Parser: Memory Request分流至地址转换逻辑 | parser_thread → parser_to_collector/dc/pc_cache_fifo | 覆盖 |
| Parser: Page Request分流至PQ处理逻辑 | parser_thread → parser_to_pq_fifo | 覆盖 |
| Parser: 错误包丢弃和错误上报 | parser_thread → fault_fifo | V4补充 |
| DC Cache: 缓存DDT信息，最大64条 | dc_cache_thread (64条LRU) | 覆盖 |
| DC Cache: 哈希映射可通过寄存器配置 | lookup_dc_cache散列函数 | 覆盖 |
| PC Cache: 缓存PDT信息 | pc_cache_thread | 覆盖 |
| PT Cache: 缓存页表，miss提交PTW | pt_cache_query/result/ptw_rsp_thread | 覆盖 |
| MSIPT Cache: 缓存MSI页表，内置MSI PTW | msipt_cache_query/result/msiptw_req/rsp_thread | 覆盖 |
| Collector: 收集DC/PC信息，选择PT Cache或xDTW | collector两个进程 | 覆盖 |
| xDTW: 通过AXI Master 2访问DDT/PDT | xdtw_req/rsp_thread | 覆盖 |
| PTW: 通过AXI Master 2访问页表 | ptw_req/rsp_thread | 覆盖 |
| Walker Cache: 页表walk中间结果缓存 | walker_cache_t (PTWc_1/2/3), 内置于PTW | **V4新增** |
| CQ Proc: 通过AXI Master 2读CQ | cq_proc_thread | V4补充 |
| CQ Proc: 通过AXI Master 1向RP发Msg | cq_proc_thread → AXI Master 1 | **V4补充** |
| CQ Proc: 向各Cache发送Invalidation | cq_to_cache_inv_fifo | **V4新增** |
| FQ Proc: 收集错误，与A核交互 | fault_proc_thread + fq_proc_thread | 覆盖 |
| PQ Proc: 从RP接收ATS Msg | pq_proc_thread | 覆盖 |

---

## 附录B：Thread ↔ FIFO 完整映射表

| Thread名称 | 所属模块 | 输入FIFO | 输出FIFO |
|---|---|---|---|
| parser_thread | Parser | inbound_fifo | parser_to_collector_fifo, parser_to_dc/pc_cache_query_fifo, parser_to_pq_fifo, fault_fifo |
| dc_cache_thread | DC Cache | parser_to_dc_cache_query_fifo, collector_to_dc_cache_update_fifo | dc_cache_to_collector_fifo |
| pc_cache_thread | PC Cache | parser_to_pc_cache_query_fifo, collector_to_pc_cache_update_fifo | pc_cache_to_collector_fifo |
| collector_cache_lookup_result_thread | Collector | parser_to_collector_fifo, dc/pc_cache_to_collector_fifo | collector_to_xdtw/pt_cache/msipt_cache_fifo, fault_fifo |
| collector_xdtw_response_thread | Collector | xdtw_to_collector_fifo | collector_to_dc/pc_cache_update_fifo, collector_to_xdtw/pt_cache/msipt_cache_fifo, fault_fifo |
| xdtw_req_thread | xDTW | collector_to_xdtw_fifo | DDR(nb) |
| xdtw_rsp_thread | xDTW | DDR响应(内部队列) | xdtw_to_collector_fifo, fault_fifo |
| pt_cache_query_thread | PT Cache | collector_to_pt_cache_query_fifo | pt_cache_lookup_result_fifo |
| pt_cache_result_thread | PT Cache | pt_cache_lookup_result_fifo | pt_cache_to_fwd_fifo, pt_cache_to_ptw_fifo, fault_fifo |
| pt_cache_ptw_rsp_thread | PT Cache | ptw_to_pt_cache_fifo | pt_cache_to_fwd_fifo |
| ptw_req_thread | PTW+Walker Cache | pt_cache_to_ptw_fifo, cq_to_cache_inv_fifo | DDR(nb), ptw_to_pt_cache_fifo(bare) |
| ptw_rsp_thread | PTW+Walker Cache | DDR响应(内部队列) | ptw_to_pt_cache_fifo, fault_fifo |
| msipt_cache_query_thread | MSIPT Cache | collector_to_msipt_cache_query_fifo | msipt_cache_lookup_result_fifo |
| msipt_cache_result_thread | MSIPT Cache | msipt_cache_lookup_result_fifo | msipt_cache_to_fwd_fifo, msipt_cache_to_msiptw_fifo |
| msiptw_req_thread | MSIPT(MSIPTW) | msipt_cache_to_msiptw_fifo | DDR(nb) |
| msiptw_rsp_thread | MSIPT(MSIPTW) | DDR响应(内部队列) | msipt_cache_to_fwd_fifo, fault_fifo |
| ddr_rsp_router_thread | DDR路由 | ddr_rsp_fifo | xdtw/ptw/msiptw内部响应队列 |
| forwarder_thread | Forwarder | pt_cache_to_fwd_fifo | AXI Master 0 |
| msi_forwarder_thread | MSI Forwarder | msipt_cache_to_fwd_fifo | AXI Stream / AXI Master 2 |
| fault_proc_thread | Fault处理 | fault_fifo | FQ(DDR blocking) |
| fq_proc_thread | FQ | fq内部 | DDR |
| pq_proc_thread | PQ | parser_to_pq_fifo | DDR |
| cq_proc_thread | CQ | cq内部 | DDR(AXI Master 2), AXI Master 1, cq_to_cache_inv_fifo |
