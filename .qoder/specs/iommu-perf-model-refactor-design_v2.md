# IOMMU性能模型重构设计方案（v2）

## Context

当前IOMMU功能模型采用阻塞传输（`b_transport`）的单线程同步架构，所有地址翻译逻辑在单次`axi_slave_b_transport`回调中一步完成。这无法模拟真实硬件的流水线行为、并发处理和时序特征。

本方案将功能模型重构为基于TLM2.0 AT编码风格、非阻塞端口、21线程并行流水线的性能模型，支持任务并发，各子模块配备Context Buffer跟踪飞行中任务。

**核心约束**：
- SystemC标准协议 + TLM2.0协议
- AT（Approximately Timed）编码风格
- 非阻塞端口（nb_transport_fw / nb_transport_bw）
- 支持多任务并发
- 保持与现有功能模型的翻译正确性一致

**v2变更摘要**：
- AXI ID分配器改为单AXI ID + 全局FIFO顺序队列
- 各Walker模块增加独立的DDR请求/响应FIFO，req/rsp线程解耦
- 新增ddr_arbiter_thread进行DDR请求仲裁
- Context Buffer以task_id为key，新增outstanding_task_count参数
- DDR Module仅保留nb_transport，移除b_transport
- 控制路径DDR访问通过ctrl_path_req_ddr_fifo路由

---

## 一、总体架构

### 1.1 流水线架构总览

```
PCIe/AXI Inbound
     |
     v
[inbound_fifo] --> [Parser] --> DC/PC Cache查询(并行) + collector_fifo
                                      |
                                [Collector] (汇聚DC/PC结果,路由决策)
                                   / | \
                                  /  |  \
                [xDTW]        [PT Cache]   [MSIPT Cache]
              (DC/PC Miss)   (IOTLB查询)  (MSI页表查询)
                  |              |              |
                  |         [PTW+WalkerCache]  [MSIPTW]
                  |              |              |
                  v              v              v
              回到Collector  [Forwarder]   [MSI Forwarder]
                            --> AXI Master/Stream输出
                            
              [DDR Arbiter] <-- xDTW/PTW/MSIPTW的DDR请求FIFO
                  |
                  v
              [DDR Module] (nb_transport only)
                  |
                  v
              DDR响应 --> 路由到对应模块的rsp_ddr_fifo

              [Fault/CQ Proc] <-- 各模块故障路径
```

### 1.2 线程分配（21个SC_THREAD）

| 模块 | 线程名 | 数量 | 职责 |
|------|--------|------|------|
| Parser | `parser_thread` | 1 | 解析入站请求,分类TTYP,提取DDI,分发到DC/PC Cache和Collector |
| DC Cache | `dc_cache_query_thread` | 1 | DC缓存查询 |
| DC Cache | `dc_cache_update_thread` | 1 | DC缓存更新（xDTW返回后填充） |
| PC Cache | `pc_cache_query_thread` | 1 | PC缓存查询 |
| PC Cache | `pc_cache_update_thread` | 1 | PC缓存更新（xDTW返回后填充） |
| Collector | `collector_cache_lookup_result_thread` | 1 | 汇聚DC/PC结果,路由到PT/MSIPT/xDTW |
| Collector | `collector_xdtw_response_thread` | 1 | 处理xDTW返回结果,更新DC/PC Cache |
| xDTW | `xdtw_req_thread` | 1 | 接收walk任务,计算DDR地址,发送请求到xdtw_req_ddr_fifo |
| xDTW | `xdtw_rsp_thread` | 1 | 处理xdtw_rsp_ddr_fifo的DDR响应,解析数据,决定继续walk或完成 |
| PT Cache | `pt_cache_query_thread` | 1 | IOTLB查询 |
| PT Cache | `pt_cache_result_thread` | 1 | 处理TLB命中/未命中路由 |
| PT Cache | `pt_cache_ptw_rsp_thread` | 1 | 处理PTW返回,更新IOTLB |
| PTW | `ptw_req_thread` | 1 | 接收walk任务,计算VS/G-stage DDR地址,发送请求到ptw_req_ddr_fifo |
| PTW | `ptw_rsp_thread` | 1 | 处理ptw_rsp_ddr_fifo的DDR响应,解析PTE,更新Walker Cache,决定继续或完成 |
| MSIPT Cache | `msipt_cache_query_thread` | 1 | MSI页表缓存查询 |
| MSIPT Cache | `msipt_cache_result_thread` | 1 | 处理MSI缓存命中/未命中 |
| MSIPTW | `msiptw_req_thread` | 1 | 接收MSI walk任务,计算DDR地址,发送请求到msiptw_req_ddr_fifo |
| MSIPTW | `msiptw_rsp_thread` | 1 | 处理msiptw_rsp_ddr_fifo的DDR响应,解析MSI PTE,决定继续或完成 |
| Forwarder | `forwarder_thread` | 1 | DMA转发/MSI转发/ATS响应 |
| Fault/CQ | `fault_cq_proc_thread` | 1 | 故障记录+命令队列处理+缓存失效 |
| **DDR Arbiter** | **`ddr_arbiter_thread`** | **1** | **从各模块DDR请求FIFO仲裁,发送nb_transport_fw到DDR,管理ddr_pending_queue** |

---

## 二、功能模型到流水线的映射

`iommu_translate_iova()`（638行，20+步骤）的分解映射：

| 功能模型步骤 | 源文件 | 流水线阶段 |
|---|---|---|
| 步骤1-5: 模式检查、DDI提取、device_id宽度校验、TTYP分类 | `iommu_translate.cc` L46-182 | **Parser** |
| 步骤6: DC缓存查找 `lookup_ioatc_dc()` | `iommu_atc.cc` | **DC Cache** |
| 步骤6: DC Miss→DDT radix walk | `iommu_device_context.cc` | **xDTW** |
| 步骤7-13: DC配置检查、EN_ATS/PDTV校验 | `iommu_translate.cc` L214-302 | **Collector** |
| 步骤14: PC缓存查找 `lookup_ioatc_pc()` | `iommu_atc.cc` | **PC Cache** |
| 步骤14: PC Miss→PDT radix walk | `iommu_process_context.cc` | **xDTW** |
| 步骤15-16: PC配置检查、页表参数设置 | `iommu_translate.cc` L312-330 | **Collector** |
| 步骤17: IOTLB查找 `lookup_ioatc_iotlb()` | `iommu_atc.cc` | **PT Cache** |
| 步骤17: VS/S-stage walk | `iommu_two_stage_trans.cc` | **PTW** |
| 步骤18: MSI翻译 `msi_address_translation()` | `iommu_msi_trans.cc` | **MSIPT Cache + MSIPTW** |
| 步骤19: G-stage walk | `iommu_second_stage_trans.cc` | **PTW**（嵌套G-stage子walk） |
| 步骤20: PA计算、ATS响应、IOATC填充 | `iommu_translate.cc` L494-613 | **Forwarder** |
| 故障处理: `report_fault()` | `iommu_faults.cc` | **Fault/CQ Proc** |

**关键原则**：现有功能模型C代码（`iommu_translate.cc`、`iommu_device_context.cc`等）不替换，而是被各流水线阶段分段调用。

---

## 三、核心数据结构设计

### 3.1 统一任务上下文 `iommu_task_t`

新建文件：`iommu/iommu_task.hh`（约280行）

任务上下文封装一次翻译请求的完整生命周期，从`iommu_translate_iova()`的栈变量提取而来：

```
iommu_task_t
├── 身份字段（从PayloadExtention提取）
│   ├── task_id (uint32_t)        -- 单调递增，全局唯一
│   ├── device_id, process_id, pid_valid
│   ├── iova, length, at (addr_type_t)
│   ├── exec_req, priv_req, no_write, is_cxl_dev
│   └── timestamp (sc_time)       -- 延迟测量
│
├── 分类字段（Parser计算）
│   ├── TTYP (uint8_t)
│   ├── is_read, is_write, is_exec, priv
│   └── DDI[3] (uint8_t)
│
├── 上下文字段（Collector填充）
│   ├── DC (device_context_t)     -- 完整设备上下文
│   ├── PC (process_context_t)    -- 完整进程上下文
│   ├── iosatp, iohgatp           -- 页表根指针
│   ├── PSCV, GV, PSCID, GSCID, DID, PID
│   └── DTF, SUM, check_access_perms
│
├── 翻译结果字段（PTW/Forwarder填充）
│   ├── pa, gpa, page_sz, gst_page_sz
│   ├── vs_pte (spte_t), g_pte (gpte_t)
│   ├── is_msi, is_mrif, mrif_nid, dest_mrif_addr
│   ├── cause, iotval, iotval2     -- 故障信息
│   └── is_bare_translation
│
├── 流水线状态
│   ├── state (task_state_t枚举)
│   │   TASK_INIT → TASK_PARSE_DONE → TASK_DC_HIT/MISS →
│   │   TASK_PC_HIT/MISS → TASK_XDTW_REQ/DONE →
│   │   TASK_TLB_HIT/MISS → TASK_PTW_REQ/DONE →
│   │   TASK_MSI_HIT/MISS → TASK_FORWARD → TASK_FAULT → TASK_DONE
│   ├── dc_valid, pc_valid, need_pc (bool) -- Collector同步
│   └── tlm_trans_ptr (tlm_generic_payload*) -- 原始payload指针
│
├── Walker上下文 (walk_context_t) —— v2增强版
│   ├── walk_type (DDT/PDT/VS_PT/G_PT/MSI_PT)
│   ├── walk_phase (枚举，跟踪多步walk的当前阶段)
│   ├── level, max_levels (int8_t)
│   ├── base_addr (uint64_t)      -- 当前层级基地址
│   ├── indexes[5] (uint16_t)     -- VPN/DDI/PDI索引
│   ├── read_addr, read_size      -- 当前DDR访问目标
│   ├── read_buf[64] (uint8_t)    -- DDR响应数据缓冲
│   ├── ptesize (uint8_t)         -- PTE大小
│   ├── vpn[5] (uint16_t)         -- VS-stage VPN（PTW用）
│   ├── vs_level (int8_t)         -- VS-stage当前层级（PTW嵌套walk用）
│   ├── gs_level (int8_t)         -- G-stage当前层级（PTW嵌套walk用）
│   ├── gs_base_addr (uint64_t)   -- G-stage基地址
│   ├── gs_pte_addr (uint64_t)    -- G-stage PTE地址
│   ├── pending_vs_pte_addr (uint64_t) -- 待G-stage翻译的VS PTE地址
│   └── gs_vpn[5] (uint16_t)     -- G-stage VPN
│
└── 完成事件
    └── completion_event (sc_event) -- Forwarder通知Parser/RP
```

### 3.2 DDR请求顺序队列（v2：替代AXI ID分配器）

**v2设计**：所有DDR请求共用单一AXI ID（所有请求发往同一DDR目标）。DDR请求→响应严格按FIFO顺序返回。用全局顺序队列替代AXI ID池。

```
// DDR请求条目（各Walker模块的req_ddr_fifo中传递）
ddr_req_entry_t
├── task_id (uint32_t)           -- 所属任务ID
├── addr (uint64_t)              -- DDR读/写地址
├── size (uint32_t)              -- 读/写大小（字节）
├── is_write (bool)              -- false=读, true=写
└── write_data[64] (uint8_t)     -- 写数据（仅写操作使用）

// DDR响应条目（各Walker模块的rsp_ddr_fifo中传递）
ddr_rsp_entry_t
├── task_id (uint32_t)           -- 所属任务ID
├── data[64] (uint8_t)           -- DDR响应数据
├── data_length (uint32_t)       -- 响应数据长度
└── error (bool)                 -- DDR访问错误标志

// DDR待决队列条目（ddr_arbiter维护的全局FIFO）
ddr_pending_entry_t
├── task_id (uint32_t)           -- 任务ID
├── source_module (uint8_t)      -- 发起模块: XDTW=0, PTW=1, MSIPTW=2, CTRL_PATH=3
├── addr (uint64_t)              -- DDR地址（调试用）
├── size (uint32_t)              -- 读取大小
└── trans_ptr (tlm_generic_payload*) -- TLM payload指针（用于DDR回调中拷贝数据）
```

### 3.3 DDR待决顺序队列 `ddr_pending_queue`

**v2设计**：替代v1的`outstanding_table`（std::map以axi_id为key）。

```
ddr_pending_queue: std::queue<ddr_pending_entry_t>  -- 全局FIFO队列
sc_mutex ddr_queue_mtx                              -- 队列互斥锁

工作原理：
1. ddr_arbiter_thread从各模块req_ddr_fifo取出请求后，
   构造ddr_pending_entry_t并push到ddr_pending_queue尾部
2. 同时通过nb_transport_fw发送DDR请求（共用单一AXI ID）
3. DDR响应严格按发送顺序返回（FIFO保序）
4. ddr_nb_transport_bw回调中从ddr_pending_queue头部pop，
   根据source_module将响应数据路由到对应的rsp_ddr_fifo

保序保证：
- 单AXI ID意味着DDR端不会乱序响应
- ddr_pending_queue的push/pop顺序与DDR请求/响应顺序一致
- 无需axi_id关联，仅需FIFO顺序匹配
```

### 3.4 Walker Cache `walker_cache_t`（三级缓存）

```
walker_cache_t
├── ptwc_1[64]                      -- 直接映射，缓存VPN最高段
├── ptwc_2[2][64]                   -- 2路组相联，缓存VPN[3:2]
├── ptwc_3[4][64]                   -- 4路组相联，缓存VPN[3:1]（Sv48）
├── lookup(gscid,pscid,iova,mode,&ppn) → hit_level/miss
├── update_ptwc1/2/3(...)
├── invalidate_by_gscid/pscid/iova/all()
└── SRRIP替换（2-bit RRPV）
```

---

## 四、各子模块Context Buffer设计

### 4.1 设计原则

- **FIFO传递task指针**：所有`sc_fifo`使用`sc_fifo<iommu_task_t*>`直接传递任务指针，无需集中式task_store，无需查找开销
- **所有权传递语义**：task指针从一个FIFO读出后，所有权转移到读取方；写入下一个FIFO时所有权再次转移。同一时刻只有一个模块持有某task的所有权
- **任务生命周期**：Parser中`new`分配 → 各模块直接通过指针读写 → Forwarder/Fault中`delete`释放
- **背压机制**：FIFO满时SC_THREAD自然阻塞，模拟流水线停顿
- **特殊情况**：Collector需要同时从Parser、DC Cache、PC Cache接收同一任务的不同信息，采用pending_tasks map以task_id为key，存储iommu_task_t*指针及同步状态
- **outstanding_task_count**（v2新增）：各Walker模块维护飞行中任务计数，作为性能模型关键调参参数

### 4.2 各模块Context Buffer详细设计

| 模块 | Context Buffer类型 | 说明 |
|------|-------------------|------|
| **Parser** | `sc_fifo<iommu_task_t*> inbound_fifo` | 从nb_transport_fw接收的任务指针入队，深度16 |
| **DC Cache** | 无显式buffer；FIFO直传 | 从parser_to_dc_cache_query_fifo读取task*，查询后写入dc_cache_to_collector_fifo |
| **PC Cache** | 无显式buffer；FIFO直传 | 同DC Cache模式 |
| **Collector** | `std::map<uint32_t, collector_entry_t> pending_tasks` | 以task_id为key聚合DC/PC结果。collector_entry_t含：iommu_task_t* task, bool dc_done, bool pc_done, bool need_pc |
| **xDTW** | `std::map<uint32_t, iommu_task_t*> active_walks` | **v2：以task_id为key**跟踪飞行中的DDR读请求。`int outstanding_task_count` 记录当前飞行中任务数，上限 `XDTW_MAX_OUTSTANDING_TASKS`（默认4） |
| **PT Cache** | FIFO三级流水线直传 | query→result→ptw_rsp线性管道，task指针依次传递 |
| **PTW** | `std::map<uint32_t, iommu_task_t*> active_walks` | **v2：以task_id为key**跟踪多步页表walk的DDR请求。`int outstanding_task_count` 上限 `PTW_MAX_OUTSTANDING_TASKS`（默认4） |
| **MSIPT Cache** | FIFO流水线 + `std::map<uint32_t, iommu_task_t*>` | **v2：以task_id为key**。`int outstanding_task_count` 上限 `MSIPTW_MAX_OUTSTANDING_TASKS`（默认2） |
| **Forwarder** | `sc_fifo<iommu_task_t*>` 直传 | 接收完成任务，转发后delete释放 |
| **Fault/CQ** | `sc_fifo<iommu_task_t*>` 直传 | 接收故障任务，记录故障后delete释放 |

**v2 outstanding_task_count语义**：
- 各Walker模块从上游FIFO接收任务时 `outstanding_task_count++`
- 任务在模块内完全处理完毕（发送到下游FIFO）时 `outstanding_task_count--`
- 当 `outstanding_task_count >= MAX_OUTSTANDING_TASKS` 时，req_thread不再从上游FIFO读取新任务（自然背压）
- 此参数直接影响模块并发度，是性能调优的关键旋钮

**性能参数**（定义于 `iommu_perf_params.hh`）：
```
XDTW_MAX_OUTSTANDING_TASKS  = 4   // xDTW最大并发walk数
PTW_MAX_OUTSTANDING_TASKS   = 4   // PTW最大并发walk数
MSIPTW_MAX_OUTSTANDING_TASKS = 2  // MSIPTW最大并发walk数
DDR_MAX_OUTSTANDING         = 8   // DDR全局最大飞行请求数
```

### 4.3 Collector的Context Buffer详细逻辑

Collector是流水线的"大脑"，需要等待DC和PC两个异步结果到齐后才能做路由决策。

**注意**：Parser会将同一个task指针同时写入3个FIFO（parser_to_collector、parser_to_dc_cache_query、parser_to_pc_cache_query）。DC/PC Cache各自操作task中对应的字段后，将指针写入collector FIFO。Collector通过task_id匹配同一任务的三份到达。

```
collector_entry_t {
    iommu_task_t* task; // 从parser_to_collector_fifo首先到达
    bool dc_done;       // DC结果已到达（dc_cache_to_collector_fifo）
    bool pc_done;       // PC结果已到达或不需要
    bool need_pc;       // 是否需要PC查询（取决于DC.tc.PDTV）
}

处理逻辑（collector_cache_lookup_result_thread）：
1. 使用sc_event多FIFO轮询：对parser_to_collector/dc_cache_to_collector/pc_cache_to_collector
   三个FIFO使用data_written_event()做OR-list等待
2. parser_to_collector → 创建pending_tasks[task->task_id]条目，保存task指针
3. dc_cache_to_collector → 通过task->task_id查找pending_tasks，标记dc_done
   检查DC.tc.PDTV决定need_pc
4. pc_cache_to_collector → 查找pending_tasks，标记pc_done
5. 当dc_done && pc_done → 执行路由决策：
   - DC_MISS → collector_to_xdtw_fifo（发送task*）
   - DC_HIT + PC不需要/PC_HIT → 配置页表参数 → is_msi_address()判断
     → MSI → collector_to_msipt_cache_query_fifo（发送task*）
     → 非MSI → collector_to_pt_cache_query_fifo（发送task*）
   - PC_MISS → collector_to_xdtw_fifo（发送task*进行PDT walk）
   - 故障 → collector_to_fault_fifo（发送task*）
6. 从pending_tasks中erase条目
```

### 4.4 task指针并发安全说明

由于Parser将同一task*同时推入3个FIFO，DC/PC Cache线程可能并发访问同一task的不同字段。安全性保证：
- **字段隔离**：DC Cache只写DC相关字段（task->DC, task->dc_valid, task->state中DC部分），PC Cache只写PC相关字段。无写-写冲突。
- **SystemC协作式调度**：同一仿真时间点只有一个SC_THREAD运行，delta cycle内不会真正并发。
- **Collector做最终汇聚**：Collector在确认dc_done && pc_done后才读取DC/PC结果，保证读-写顺序。

---

## 五、完整AT传输实现策略

采用**完整TLM2.0 AT（Approximately Timed）方案**，所有性能关键路径使用`nb_transport_fw`/`nb_transport_bw`非阻塞传输。

### 5.1 Socket类型升级总览

| 接口 | 当前Socket类型 | 目标Socket类型 | AT传输方式 |
|------|---------------|---------------|-----------|
| **入站** (RP→IOMMU) slave | `simple_target_socket` + b_transport | `tlm_target_socket<BUS_WIDTH>` | nb_transport_fw (BEGIN_REQ/END_RESP) |
| **DDR** (IOMMU→DDR) master_1 | `simple_initiator_socket` + b_transport | `tlm_initiator_socket<BUS_WIDTH>` | nb_transport_fw→DDR; nb_transport_bw←DDR |
| **DMA转发** master_0 | `simple_initiator_socket` + b_transport | `tlm_initiator_socket<BUS_WIDTH>` | nb_transport_fw (Forwarder发起) |
| **MSI转发** stream socket | `simple_initiator_socket` + b_transport | `tlm_initiator_socket<BUS_WIDTH>` | nb_transport_fw (MSI Forwarder发起) |
| **ATS响应** master_2 | `simple_initiator_socket` + b_transport | `tlm_initiator_socket<BUS_WIDTH>` | nb_transport_fw |
| **AHB寄存器** slave | `simple_target_socket` + b_transport | 保留b_transport | 配置路径，非性能关键 |

### 5.2 入站AT传输时序

```
Initiator (RP)                           Target (IOMMU)
     |                                        |
     |── nb_transport_fw(BEGIN_REQ, payload) ─►|
     |                                        | 提取PayloadExtention
     |                                        | new iommu_task_t, 填充字段
     |                                        | push task* to inbound_fifo
     |◄── return TLM_UPDATED ────────────────| （同步返回END_REQ阶段）
     |    phase = END_REQ                     |
     |                                        |
     |    ... 流水线处理中（21线程并行）...     |
     |                                        |
     |◄── nb_transport_bw(BEGIN_RESP, payload)|  Forwarder完成翻译
     |    payload中已设置翻译后地址和状态      |
     |                                        |
     |── nb_transport_fw(END_RESP) ──────────►|  delete task
     |                                        |
```

**IOMMU入站nb_transport_fw回调实现**：
```cpp
tlm::tlm_sync_enum iommu_top::axi_slave_nb_transport_fw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_REQ) {
        // 1. 提取PayloadExtention
        PayloadExtention* ext = nullptr;
        trans.get_extension(ext);
        // 2. 创建task, 填充字段
        iommu_task_t* task = new iommu_task_t();
        task->task_id = next_task_id++;
        task->tlm_trans_ptr = &trans;
        // ... 填充device_id, iova等
        // 3. 推入inbound_fifo
        inbound_fifo.nb_write(task);  // 非阻塞写
        // 4. 返回END_REQ（告知initiator请求已接受）
        phase = tlm::END_REQ;
        return tlm::TLM_UPDATED;
    }
    else if (phase == tlm::END_RESP) {
        // Initiator确认收到响应，可释放资源
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}
```

**Forwarder发送响应（nb_transport_bw）**：
```cpp
// forwarder_thread中：翻译完成后
void iommu_top::send_response_to_initiator(iommu_task_t* task) {
    tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;
    // 设置翻译结果到payload
    trans->set_address(task->pa);
    trans->set_response_status(tlm::TLM_OK_RESPONSE);
    // 通过nb_transport_bw回调通知Initiator
    tlm::tlm_phase phase = tlm::BEGIN_RESP;
    sc_time delay = SC_ZERO_TIME;
    axi_slave_from_pcie_noc_0_socket->nb_transport_bw(*trans, phase, delay);
    // task在收到END_RESP后delete
}
```

### 5.3 DDR AT传输时序（v2：DDR请求FIFO + 仲裁器模型）

**v2设计**：Walker线程不再直接调用ddr_nb_read等待响应，而是将请求写入模块专属的req_ddr_fifo，由ddr_arbiter_thread统一仲裁后通过nb_transport_fw发送到DDR。DDR响应通过nb_transport_bw回调，从ddr_pending_queue匹配并路由到对应模块的rsp_ddr_fifo。

```
Walker req线程          ddr_req_fifo    DDR Arbiter        DDR Module          rsp_ddr_fifo    Walker rsp线程
     |                      |               |                   |                   |               |
  计算DDR地址               |               |                   |                   |               |
  构造ddr_req_entry         |               |                   |                   |               |
     |── write(req_entry)──►|               |                   |                   |               |
     |  (线程继续处理下一个   |               |                   |                   |               |
     |   任务或阻塞在FIFO)   |               |                   |                   |               |
     |                      |◄── read() ───|                   |                   |               |
     |                      |               | 构造pending_entry  |                   |               |
     |                      |               | push到pending_queue|                   |               |
     |                      |               |                   |                   |               |
     |                      |               |── nb_transport_fw ►|                   |               |
     |                      |               |   (BEGIN_REQ)      |                   |               |
     |                      |               |◄── TLM_UPDATED ───|                   |               |
     |                      |               |   (END_REQ)        |                   |               |
     |                      |               |                   |                   |               |
     |                      |               |                   | ... DDR处理延迟 ... |               |
     |                      |               |                   |                   |               |
     |                      |               |◄── nb_transport_bw |                   |               |
     |                      |               |   (BEGIN_RESP)     |                   |               |
     |                      |               | pop pending_queue  |                   |               |
     |                      |               | 根据source_module  |                   |               |
     |                      |               | 路由到对应rsp_fifo |                   |               |
     |                      |               |── END_RESP ───────►|                   |               |
     |                      |               |                   |                   |               |
     |                      |               |──write(rsp_entry)─────────────────────►|               |
     |                      |               |                   |                   |◄── read() ───|
     |                      |               |                   |                   |               |
     |                      |               |                   |                   |  解析数据      |
     |                      |               |                   |                   |  查active_walks|
     |                      |               |                   |                   |  继续或完成    |
```

**DDR Arbiter核心逻辑**：
```cpp
void iommu_top::ddr_arbiter_thread() {
    uint8_t rr_index = 0;  // round-robin轮转索引
    while (true) {
        // 等待任意req_ddr_fifo有数据
        wait(ctrl_path_req_ddr_fifo.data_written_event() |
             xdtw_req_ddr_fifo.data_written_event() |
             ptw_req_ddr_fifo.data_written_event() |
             msiptw_req_ddr_fifo.data_written_event());

        // 流控：检查全局飞行请求数
        while (ddr_pending_queue.size() >= DDR_MAX_OUTSTANDING) {
            wait(ddr_pending_freed_event);  // 等待有请求完成
        }

        // 优先级：ctrl_path > round-robin(xDTW, PTW, MSIPTW)
        ddr_req_entry_t req;
        uint8_t source_module;

        if (ctrl_path_req_ddr_fifo.num_available() > 0) {
            // 控制路径最高优先级（report_fault等）
            ctrl_path_ddr_req_t ctrl_req = ctrl_path_req_ddr_fifo.read();
            req.task_id = 0;  // 控制路径无task_id
            req.addr = ctrl_req.addr;
            req.size = ctrl_req.size;
            req.is_write = ctrl_req.is_write;
            memcpy(req.write_data, ctrl_req.write_data, ctrl_req.size);
            source_module = 3;  // CTRL_PATH
        } else {
            // Round-robin仲裁：xDTW(0), PTW(1), MSIPTW(2)
            sc_fifo<ddr_req_entry_t>* fifos[3] = {
                &xdtw_req_ddr_fifo, &ptw_req_ddr_fifo, &msiptw_req_ddr_fifo
            };
            bool found = false;
            for (int k = 0; k < 3; k++) {
                uint8_t idx = (rr_index + k) % 3;
                if (fifos[idx]->num_available() > 0) {
                    req = fifos[idx]->read();
                    source_module = idx;
                    rr_index = (idx + 1) % 3;
                    found = true;
                    break;
                }
            }
            if (!found) continue;
        }

        // 构造pending条目并入队
        ddr_pending_entry_t pending;
        pending.task_id = req.task_id;
        pending.source_module = source_module;
        pending.addr = req.addr;
        pending.size = req.size;

        // 分配TLM payload并发送
        tlm_generic_payload* trans = new tlm_generic_payload();
        trans->set_address(req.addr);
        trans->set_data_length(req.size);
        if (req.is_write) {
            trans->set_command(tlm::TLM_WRITE_COMMAND);
            trans->set_data_ptr(req.write_data);
        } else {
            trans->set_command(tlm::TLM_READ_COMMAND);
            uint8_t* data_buf = new uint8_t[req.size];
            trans->set_data_ptr(data_buf);
        }
        pending.trans_ptr = trans;

        ddr_queue_mtx.lock();
        ddr_pending_queue.push(pending);
        ddr_queue_mtx.unlock();

        // 发送nb_transport_fw到DDR
        tlm::tlm_phase phase = tlm::BEGIN_REQ;
        sc_time delay = SC_ZERO_TIME;
        axi_master_1_to_cmn_rnd_socket->nb_transport_fw(*trans, phase, delay);
    }
}
```

**IOMMU DDR nb_transport_bw回调（v2）**：
```cpp
tlm::tlm_sync_enum iommu_top::ddr_nb_transport_bw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_RESP) {
        // 从ddr_pending_queue头部取出（FIFO保序）
        ddr_queue_mtx.lock();
        ddr_pending_entry_t pending = ddr_pending_queue.front();
        ddr_pending_queue.pop();
        ddr_queue_mtx.unlock();

        // 构造响应条目
        ddr_rsp_entry_t rsp;
        rsp.task_id = pending.task_id;
        rsp.data_length = trans.get_data_length();
        memcpy(rsp.data, trans.get_data_ptr(), rsp.data_length);
        rsp.error = (trans.get_response_status() != tlm::TLM_OK_RESPONSE);

        // 根据source_module路由到对应rsp_ddr_fifo
        switch (pending.source_module) {
            case 0: xdtw_rsp_ddr_fifo.write(rsp); break;
            case 1: ptw_rsp_ddr_fifo.write(rsp); break;
            case 2: msiptw_rsp_ddr_fifo.write(rsp); break;
            case 3:
                // 控制路径：拷贝数据到ctrl_path缓冲并通知
                memcpy(ctrl_path_rsp_buf, rsp.data, rsp.data_length);
                ctrl_path_rsp_event.notify(SC_ZERO_TIME);
                break;
        }

        // 释放TLM payload
        if (trans.get_command() == tlm::TLM_READ_COMMAND) {
            delete[] trans.get_data_ptr();
        }
        delete pending.trans_ptr;

        // 通知arbiter有空间可发新请求
        ddr_pending_freed_event.notify(SC_ZERO_TIME);

        // 发送END_RESP确认
        phase = tlm::END_RESP;
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}
```

### 5.4 DDR Module（v2：仅nb_transport）

DDR Module (`ddr/test_ddr.hh`) **仅支持nb_transport**，移除b_transport注册：

```cpp
// DDR Module构造函数 —— v2：仅注册nb_transport_fw
DDR_Module::DDR_Module(sc_module_name name) : sc_module(name),
    axi_slave_from_cmn_rnd_1_socket("axi_slave_from_cmn_rnd_1_socket")
{
    // 仅注册nb_transport_fw（移除b_transport注册）
    axi_slave_from_cmn_rnd_1_socket.register_nb_transport_fw(
        this, &DDR_Module::nb_transport_fw);
    SC_THREAD(ddr_process_thread);
}

// DDR Module nb_transport_fw回调
tlm::tlm_sync_enum DDR_Module::nb_transport_fw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_REQ) {
        // 将请求payload指针入队（注意：不拷贝payload）
        ddr_req_fifo.write(&trans);
        phase = tlm::END_REQ;
        return tlm::TLM_UPDATED;
    }
    else if (phase == tlm::END_RESP) {
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

// DDR处理线程：模拟DDR延迟
void DDR_Module::ddr_process_thread() {
    while(true) {
        tlm_generic_payload* trans = ddr_req_fifo.read();
        // 模拟DDR读/写延迟
        if (trans->get_command() == tlm::TLM_READ_COMMAND) {
            wait(DDR_READ_LATENCY, SC_NS);
        } else {
            wait(DDR_WRITE_LATENCY, SC_NS);
        }
        // 执行内存读写
        process_memory_access(*trans);
        // 通过nb_transport_bw返回响应
        tlm::tlm_phase phase = tlm::BEGIN_RESP;
        sc_time delay = SC_ZERO_TIME;
        target_socket->nb_transport_bw(*trans, phase, delay);
    }
}
```

### 5.5 RP Module AT升级

RP Module (`rp/test_rp.hh`)需升级为AT initiator：

```cpp
// RP发送翻译请求（AT方式）
void RP_Module::send_translation_request_at(tlm_generic_payload& trans) {
    tlm::tlm_phase phase = tlm::BEGIN_REQ;
    sc_time delay = SC_ZERO_TIME;
    // 发送非阻塞请求
    tlm::tlm_sync_enum status = 
        axi_master_to_pcie_noc_0_socket->nb_transport_fw(trans, phase, delay);
    if (status == TLM_UPDATED && phase == END_REQ) {
        // 请求已接受，等待响应
        wait(response_event);  // 由nb_transport_bw触发
    }
}

// RP接收响应回调
tlm::tlm_sync_enum RP_Module::nb_transport_bw(
    tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_RESP) {
        // 保存响应，通知测试线程
        response_event.notify(SC_ZERO_TIME);
        // 发送END_RESP确认
        phase = tlm::END_RESP;
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}
```

### 5.6 PayloadExtention扩展

`param_trans_def.hh`中PayloadExtention添加AT所需字段：

```cpp
// 新增字段
uint8_t  iommu_internal;   // 标记IOMMU内部DDR访问（vs DMA转发）
```

**v2注意**：由于所有DDR请求共用单一AXI ID，不再需要在PayloadExtention中传递`axi_id`字段。DDR请求/响应的关联通过`ddr_pending_queue`的FIFO顺序保证。

### 5.7 控制路径DDR访问（v2新增）

**问题**：功能模型中`report_fault()`（`iommu_faults.cc`）调用`write_memory()`→通过`iommu_inst->top`的b_transport路径写DDR故障队列。v2中DDR仅支持nb_transport，需要兼容方案。

**方案**：通过`ctrl_path_req_ddr_fifo`路由控制路径DDR访问，DDR Arbiter给予最高优先级处理。

```
ctrl_path_ddr_req_t
├── addr (uint64_t)              -- DDR读/写地址
├── size (uint32_t)              -- 读/写大小
├── is_write (bool)              -- 读/写标志
├── write_data[64] (uint8_t)     -- 写数据
└── completion_event (sc_event*) -- 完成通知事件指针

// 修改 iommu_ref_api.cc 中的 write_memory_test / read_memory_test：
void write_memory_test(iommu_t* iommu, uint64_t addr, uint32_t size, uint8_t* data) {
    ctrl_path_ddr_req_t req;
    req.addr = addr;
    req.size = size;
    req.is_write = true;
    memcpy(req.write_data, data, size);
    sc_event done_event;
    req.completion_event = &done_event;
    iommu->top->ctrl_path_req_ddr_fifo.write(req);
    wait(done_event);  // 同步等待DDR完成
}

void read_memory_test(iommu_t* iommu, uint64_t addr, uint32_t size, uint8_t* data) {
    ctrl_path_ddr_req_t req;
    req.addr = addr;
    req.size = size;
    req.is_write = false;
    sc_event done_event;
    req.completion_event = &done_event;
    iommu->top->ctrl_path_req_ddr_fifo.write(req);
    wait(done_event);
    memcpy(data, iommu->top->ctrl_path_rsp_buf, size);
}
```

**关键**：`report_fault()` API本身无需修改，仅修改底层`write_memory_test`/`read_memory_test`的实现。DDR Arbiter对`ctrl_path_req_ddr_fifo`给予最高优先级，确保控制路径不被数据路径饿死。

---

## 六、FIFO通道配置

所有FIFO使用`sc_fifo<iommu_task_t*>`传递任务指针（DDR FIFO除外），深度取自`iommu_perf_params.hh`：

### 6.1 流水线数据FIFO

| FIFO名称 | 深度 | 源模块 | 目标模块 |
|----------|------|--------|----------|
| `inbound_fifo` | 16 | nb_transport_fw回调 | Parser |
| `parser_to_collector_fifo` | 16 | Parser | Collector |
| `parser_to_dc_cache_query_fifo` | 8 | Parser | DC Cache |
| `parser_to_pc_cache_query_fifo` | 8 | Parser | PC Cache |
| `dc_cache_to_collector_fifo` | 8 | DC Cache | Collector |
| `pc_cache_to_collector_fifo` | 8 | PC Cache | Collector |
| `collector_to_xdtw_fifo` | 4 | Collector | xDTW |
| `collector_to_dc_cache_update_fifo` | 8 | Collector | DC Cache |
| `collector_to_pc_cache_update_fifo` | 8 | Collector | PC Cache |
| `collector_to_pt_cache_query_fifo` | 8 | Collector | PT Cache |
| `collector_to_msipt_cache_query_fifo` | 8 | Collector | MSIPT Cache |
| `collector_to_fault_fifo` | 8 | Collector/各模块 | Fault Proc |
| `xdtw_to_collector_fifo` | 4 | xDTW | Collector |
| `pt_cache_to_ptw_fifo` | 4 | PT Cache | PTW |
| `ptw_to_pt_cache_fifo` | 4 | PTW | PT Cache |
| `pt_cache_to_fwd_fifo` | 8 | PT Cache | Forwarder |
| `msipt_cache_to_msiptw_fifo` | 4 | MSIPT Cache | MSIPTW |
| `msiptw_to_msipt_cache_fifo` | 4 | MSIPTW | MSIPT Cache |
| `msipt_cache_to_fwd_fifo` | 8 | MSIPT Cache | Forwarder |

### 6.2 DDR请求/响应FIFO（v2新增）

| FIFO名称 | 类型 | 深度 | 源模块 | 目标模块 |
|----------|------|------|--------|----------|
| `xdtw_req_ddr_fifo` | `sc_fifo<ddr_req_entry_t>` | 4 | xDTW req_thread / rsp_thread | DDR Arbiter |
| `xdtw_rsp_ddr_fifo` | `sc_fifo<ddr_rsp_entry_t>` | 4 | DDR bw回调 | xDTW rsp_thread |
| `ptw_req_ddr_fifo` | `sc_fifo<ddr_req_entry_t>` | 8 | PTW req_thread / rsp_thread | DDR Arbiter |
| `ptw_rsp_ddr_fifo` | `sc_fifo<ddr_rsp_entry_t>` | 8 | DDR bw回调 | PTW rsp_thread |
| `msiptw_req_ddr_fifo` | `sc_fifo<ddr_req_entry_t>` | 2 | MSIPTW req_thread / rsp_thread | DDR Arbiter |
| `msiptw_rsp_ddr_fifo` | `sc_fifo<ddr_rsp_entry_t>` | 2 | DDR bw回调 | MSIPTW rsp_thread |
| `ctrl_path_req_ddr_fifo` | `sc_fifo<ctrl_path_ddr_req_t>` | 2 | write_memory/read_memory | DDR Arbiter |

**DDR FIFO深度说明**：
- xDTW：DDT最多3层+1次DC读取=4次DDR访问，深度4
- PTW：VS-stage最多5层 × G-stage每层最多5次=25次DDR访问（最坏情况），深度8兼顾吞吐
- MSIPTW：通常1-2次DDR访问，深度2
- ctrl_path：故障写入低频，深度2

---

## 七、互斥与同步设计

### 7.1 需要互斥保护的资源

| 资源 | 互斥锁 | 访问方 | 原因 |
|------|--------|--------|------|
| DDR待决队列 | `sc_mutex ddr_queue_mtx` | DDR Arbiter + DDR响应回调 | 并发push/pop |
| Walker Cache | `sc_mutex walker_cache_mtx` | PTW线程 | 并发查找/填充 |
| iommu_inst缓存 | `sc_mutex iommu_cache_mtx` | DC/PC/PT Cache线程 | 并发缓存操作 |
| Collector pending_tasks | `sc_mutex collector_mtx` | Collector线程 | 多FIFO并发更新同一map |
| next_task_id计数器 | `sc_mutex task_id_mtx` | nb_transport_fw回调 | 原子递增 |
| xDTW active_walks | `sc_mutex xdtw_walks_mtx` | xDTW req/rsp线程 | 并发读写active_walks |
| PTW active_walks | `sc_mutex ptw_walks_mtx` | PTW req/rsp线程 | 并发读写active_walks |
| MSIPTW active_walks | `sc_mutex msiptw_walks_mtx` | MSIPTW req/rsp线程 | 并发读写active_walks |

**v2变更**：移除v1的`axi_id_mtx`和`outstanding_mtx`（AXI ID池和Outstanding表已废弃）。新增`ddr_queue_mtx`（DDR待决队列）和各Walker模块的`walks_mtx`。

**注意**：采用task指针传递后，无需全局task_store及其mutex。task指针的所有权在模块间明确传递。

### 7.2 死锁避免

**锁序规则**（从低到高）：
```
task_id_mtx < collector_mtx < iommu_cache_mtx < walker_cache_mtx < *_walks_mtx < ddr_queue_mtx
```
任何模块不得在持有高序锁时获取低序锁。

**无嵌套锁**：每个临界区最多获取一个mutex。各walker线程对active_walks的操作独立于ddr_queue的操作。

---

## 八、文件创建/修改清单

### 新建文件（10个）

| 文件 | 行数估算 | 内容 |
|------|----------|------|
| `iommu/iommu_task.hh` | ~300 | iommu_task_t, task_state_t, walk_context_t, ddr_req_entry_t, ddr_rsp_entry_t, ddr_pending_entry_t, ctrl_path_ddr_req_t, walker_cache_t及其三级子结构 |
| `iommu/iommu_perf_model.hh` | ~150 | 工具函数：extract_DDI, classify_ttype, is_msi_address, calculate_pa_from_ppn等 |
| `iommu/iommu_perf_parser.cc` | ~200 | parser_thread实现 |
| `iommu/iommu_perf_collector.cc` | ~350 | collector_cache_lookup_result_thread + collector_xdtw_response_thread |
| `iommu/iommu_perf_dc_pc_cache.cc` | ~150 | DC/PC Cache的4个线程 |
| `iommu/iommu_perf_pt_cache.cc` | ~200 | PT Cache的3个线程 |
| `iommu/iommu_perf_msipt_cache.cc` | ~250 | MSIPT Cache的4个线程 |
| `iommu/iommu_perf_xdtw.cc` | ~350 | xDTW的2个线程（v2：req/rsp解耦+状态机） |
| `iommu/iommu_perf_ptw.cc` | ~600 | PTW的2个线程（v2：req/rsp解耦+嵌套walk状态机）+ Walker Cache集成 |
| `iommu/iommu_perf_forwarder_fault_cq.cc` | ~350 | Forwarder + Fault/CQ的2个线程 + ddr_arbiter_thread |

### 修改文件（6个）

| 文件 | 修改内容 |
|------|----------|
| `iommu/iommu_top.hh` | 从57行扩展到~350行：声明21个SC_THREAD、~26个sc_fifo（含7个DDR FIFO）、Walker Cache实例、ddr_pending_queue、各Walker active_walks和outstanding_task_count、所有sc_mutex、Collector pending_tasks；替换simple_target_socket为tlm_target_socket支持nb_transport_fw；替换simple_initiator_socket为tlm_initiator_socket支持nb_transport_bw |
| `iommu/iommu_top.cc` | 实现axi_slave_nb_transport_fw（入站AT回调）；实现ddr_nb_transport_bw（DDR响应AT回调，路由到rsp_ddr_fifo）；实现send_response_to_initiator()辅助方法；实现ddr_arbiter_thread；移除CQ_Monitor_Process_Thread；保留before_end_of_elaboration和ahb_slave_b_transport不变 |
| `iommu/param_trans_def.hh` | PayloadExtention添加iommu_internal(uint8_t)字段；clone()和copy_from()方法同步更新 |
| `ddr/test_ddr.hh` + `ddr/test_ddr.cc` | DDR Module升级AT：**仅注册nb_transport_fw**（移除b_transport注册）、DDR请求FIFO、ddr_process_thread处理线程（模拟延迟后通过nb_transport_bw返回响应） |
| `rp/test_rp.hh` + `rp/test_rp_thread.cc` | RP Module升级AT：添加nb_transport_bw回调接收响应；修改send_translation_request为AT方式（nb_transport_fw发送BEGIN_REQ，等待response_event） |
| `iommu/iommu_ref_api.cc` | 修改write_memory_test/read_memory_test实现：通过ctrl_path_req_ddr_fifo路由DDR访问（替代直接b_transport） |

### 不修改的文件（保持功能模型代码不变）

- `iommu/iommu_translate.cc` -- 翻译算法参考（被流水线分段调用）
- `iommu/iommu_device_context.cc` -- DC定位（被xDTW调用）
- `iommu/iommu_process_context.cc` -- PC定位（被xDTW调用）
- `iommu/iommu_two_stage_trans.cc` -- VS-stage walk（被PTW调用）
- `iommu/iommu_second_stage_trans.cc` -- G-stage walk（被PTW调用）
- `iommu/iommu_msi_trans.cc` -- MSI翻译（被MSIPTW调用）
- `iommu/iommu_atc.cc` -- 缓存操作（被DC/PC/PT Cache调用）
- `iommu/iommu_faults.cc` -- 故障报告（被Fault Proc调用，底层DDR访问通过ctrl_path路由）
- `iommu/iommu_command_queue.cc` -- 命令处理（被CQ Proc调用）
- `main.cpp` -- socket绑定保持（socket名称不变，但需确认nb_transport注册兼容性）

---

## 九、实现步骤

### Phase 1：基础数据结构
1. 创建`iommu/iommu_task.hh` -- 定义iommu_task_t、task_state_t、walk_context_t（v2增强：walk_phase/vs_level/gs_level等）、ddr_req_entry_t、ddr_rsp_entry_t、ddr_pending_entry_t、ctrl_path_ddr_req_t、walker_cache_t
2. 创建`iommu/iommu_perf_model.hh` -- 提取工具函数（extract_DDI、classify_ttype、is_msi_address等）

### Phase 2：AT传输基础设施
3. 修改`iommu/param_trans_def.hh` -- PayloadExtention添加iommu_internal字段
4. 修改`ddr/test_ddr.hh` + `ddr/test_ddr.cc` -- DDR Module仅nb_transport（移除b_transport、添加nb_transport_fw回调、ddr_process_thread）
5. 修改`rp/test_rp.hh` + `rp/test_rp_thread.cc` -- RP Module升级AT
6. 修改`iommu/iommu_ref_api.cc` -- write_memory_test/read_memory_test改为ctrl_path_req_ddr_fifo路由

### Phase 3：顶层模块重构
7. 修改`iommu/iommu_top.hh` -- 声明所有FIFO（含7个DDR FIFO）、21个SC_THREAD、mutex、AT socket类型替换、Walker Cache、ddr_pending_queue、各Walker active_walks/outstanding_task_count
8. 修改`iommu/iommu_top.cc` -- 实现axi_slave_nb_transport_fw、ddr_nb_transport_bw（路由到rsp_ddr_fifo）、ddr_arbiter_thread、send_response_to_initiator

### Phase 4：流水线模块实现（可并行开发）
9. `iommu_perf_parser.cc` -- Parser线程
10. `iommu_perf_dc_pc_cache.cc` -- DC/PC Cache的4个线程
11. `iommu_perf_collector.cc` -- Collector的2个线程
12. `iommu_perf_xdtw.cc` -- xDTW的2个线程（v2：req写req_ddr_fifo，rsp读rsp_ddr_fifo+状态机）
13. `iommu_perf_pt_cache.cc` -- PT Cache的3个线程
14. `iommu_perf_ptw.cc` -- PTW的2个线程（v2：req/rsp解耦+嵌套walk状态机）+ Walker Cache集成
15. `iommu_perf_msipt_cache.cc` -- MSIPT Cache + MSIPTW的4个线程（v2：req/rsp解耦）
16. `iommu_perf_forwarder_fault_cq.cc` -- Forwarder + Fault/CQ线程

### Phase 5：构建集成
17. 修改`Makefile`添加8个新源文件
18. 确认`main.cpp`中socket绑定与新AT socket类型的兼容性

---

## 十、验证方案

### 10.1 编译验证
```bash
# WSL环境下编译
bash -c "cd /mnt/d/Qoder_proj/iommu_model_20260401_v1 && make clean && make DEBUG=1"
```

### 10.2 功能等价验证
- 使用现有`test_rp_thread.cc`中的测试用例（多设备、多模式翻译）
- 对比新旧模型的翻译结果（PA、fault code、response status）
- 验证Bare模式、DDT_1LVL、两级翻译、MSI翻译路径

### 10.3 并发验证
- 注入多个设备的并发翻译请求
- 验证无死锁（FIFO阻塞+线程饥饿）
- 验证任务隔离（一个任务故障不影响其他任务）
- DDR请求/响应FIFO保序正确

### 10.4 时序验证
- 插桩任务时间戳，验证：
  - Cache命中路径延迟 ≈ PARSER(1ns) + DC_CACHE(2ns) + COLLECTOR(1ns) + PT_CACHE(3ns) + FORWARDER(2ns) = 9ns
  - Cache未命中路径包含预期的DDR延迟
- 验证多任务流水线重叠执行
- 验证outstanding_task_count对并发度的限制效果

### 10.5 回归测试
- 所有现有测试用例在新模型下通过
- 无内存泄漏（任务分配/释放平衡）

---

## 十一、风险与应对

| 风险 | 应对策略 |
|------|----------|
| 环形FIFO依赖导致死锁 | 数据流为非循环DAG；xDTW→Collector→xDTW通过独立线程避免自死锁 |
| iommu_inst共享状态竞争 | 所有缓存访问用sc_mutex保护；功能模型代码非线程安全，需mutex包装 |
| 任务内存泄漏 | 严格所有权传递：Parser中new，Forwarder/Fault中delete；task指针在FIFO间单一所有权转移 |
| 功能正确性退化 | 保留所有功能.cc不变；流水线调用相同函数+相同参数 |
| RP/DDR AT升级复杂度 | DDR Module仅保留nb_transport简化实现；RP升级主要改发送方式和加nb_transport_bw回调 |
| nb_transport phase时序错误 | 严格遵循TLM2.0 base protocol规则：BEGIN_REQ→END_REQ→BEGIN_RESP→END_RESP |
| task指针在多FIFO共享安全性 | Parser将同一task*写入3个FIFO时，利用SystemC协作式调度保证不真正并发；DC/PC Cache操作互不重叠的字段 |
| DDR请求FIFO满导致arbiter阻塞 | req_ddr_fifo深度根据模块最大DDR访问次数设计；DDR_MAX_OUTSTANDING限制全局飞行数 |
| 控制路径DDR访问延迟 | ctrl_path在arbiter中享有最高优先级；实际场景中故障写入频率远低于数据路径 |
| Walker rsp线程状态机复杂度 | PTW嵌套walk（VS×G）需多阶段状态机；通过walk_phase枚举严格管理状态转换 |

---

## 十二、各线程内部执行流程详细设计

本章细化21个SC_THREAD的内部伪代码执行流程。每个线程的描述包含：输入/输出FIFO、调用的功能模型函数（含源文件行号引用）、处理延迟、task字段读写、故障分支。

**约定**：
- `wait(DELAY, SC_NS)` 表示模拟处理延迟
- `fifo.read()` / `fifo.write(task)` 表示阻塞式FIFO操作
- 所有对 `iommu_inst` 缓存的访问需用 `iommu_cache_mtx` 保护
- `task->state` 状态转换在每个阶段入口/出口更新
- **v2变更**：Walker线程不再直接调用`ddr_nb_read()+wait(rsp_event)`，而是通过req_ddr_fifo/rsp_ddr_fifo解耦

---

### 12.1 parser_thread（Parser线程）

**输入**：`inbound_fifo`（深度16，由nb_transport_fw回调写入）
**输出**：`parser_to_collector_fifo`、`parser_to_dc_cache_query_fifo`、`parser_to_pc_cache_query_fifo`、`collector_to_fault_fifo`、`pt_cache_to_fwd_fifo`
**延迟**：PARSER_DELAY = 1ns
**对应功能模型**：`iommu_translate_iova()` 步骤1-5（`iommu_translate.cc` L39-182）

```
void iommu_top::parser_thread() {
    while (true) {
        // ========== 1. 从入站FIFO读取任务 ==========
        iommu_task_t* task = inbound_fifo.read();
        task->state = TASK_PARSING;
        wait(PARSER_DELAY, SC_NS);

        // ========== 2. 事件计数与TTYP分类 ==========
        // 对应 iommu_translate.cc L46-89
        task->TTYP = classify_ttype(task);
        extract_access_attributes(task);

        // ========== 3. 步骤1：检查 ddtp.iommu_mode == Off ==========
        // 对应 iommu_translate.cc L97-113
        if (iommu_inst.reg_file.ddtp.iommu_mode == Off) {
            task->cause = 256;
            task->state = TASK_FAULT;
            if (task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) {
                task->rsp_status = UNSUPPORTED_REQUEST;
            }
            collector_to_fault_fifo.write(task);
            continue;
        }

        // ========== 4. 步骤2：检查 ddtp.iommu_mode == Bare ==========
        // 对应 iommu_translate.cc L120-141
        if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_Bare) {
            if (task->at == ADDR_TYPE_TRANSLATED ||
                task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) {
                task->cause = 260;
                task->state = TASK_FAULT;
                collector_to_fault_fifo.write(task);
                continue;
            }
            task->pa = task->iova;
            task->page_sz = PAGESIZE;
            task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = 1;
            task->vs_pte.PBMT = PMA;
            task->g_pte.X = task->g_pte.W = task->g_pte.R = 1;
            task->is_msi = 0;
            task->state = TASK_FORWARD;
            pt_cache_to_fwd_fifo.write(task);
            continue;
        }

        // ========== 5. 步骤3-4：DDI提取 ==========
        // 对应 iommu_translate.cc L143-158
        extract_DDI(task, &iommu_inst);

        // ========== 6. 步骤5：设备ID宽度验证 ==========
        // 对应 iommu_translate.cc L162-182
        if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_2LVL && task->DDI[2] != 0) {
            task->cause = 260;
            task->state = TASK_FAULT;
            collector_to_fault_fifo.write(task);
            continue;
        }
        if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_1LVL &&
            (task->DDI[2] != 0 || task->DDI[1] != 0)) {
            task->cause = 260;
            task->state = TASK_FAULT;
            collector_to_fault_fifo.write(task);
            continue;
        }

        // ========== 7. 分发到DC/PC Cache查询和Collector ==========
        task->state = TASK_PARSE_DONE;
        parser_to_collector_fifo.write(task);
        parser_to_dc_cache_query_fifo.write(task);
        parser_to_pc_cache_query_fifo.write(task);
    }
}
```

**task字段写入**：`TTYP`, `is_read/is_write/is_exec/priv`, `DDI[3]`, `cause`, `pa`(Bare), `page_sz`(Bare), `vs_pte/g_pte`(Bare), `state`
**task字段读取**：`at`, `iova`, `device_id`, `exec_req`, `pid_valid`, `process_id`

---

### 12.2 dc_cache_query_thread（DC Cache查询线程）

**输入**：`parser_to_dc_cache_query_fifo`（深度8）
**输出**：`dc_cache_to_collector_fifo`（深度8）
**延迟**：DC_CACHE_HIT_DELAY = 2ns
**对应功能模型**：`lookup_ioatc_dc()`（`iommu_atc.cc` L36-49）

```
void iommu_top::dc_cache_query_thread() {
    while (true) {
        iommu_task_t* task = parser_to_dc_cache_query_fifo.read();
        wait(DC_CACHE_HIT_DELAY, SC_NS);

        iommu_cache_mtx.lock();
        uint8_t status = lookup_ioatc_dc(&iommu_inst, task->device_id, &task->DC);
        iommu_cache_mtx.unlock();

        if (status == IOATC_HIT) {
            task->dc_valid = true;
            task->dc_hit = true;
            task->DTF = task->DC.tc.DTF;
        } else {
            task->dc_valid = false;
            task->dc_hit = false;
        }

        dc_cache_to_collector_fifo.write(task);
    }
}
```

---

### 12.3 dc_cache_update_thread（DC Cache更新线程）

**输入**：`collector_to_dc_cache_update_fifo`（深度8）
**输出**：无
**延迟**：DC_CACHE_HIT_DELAY = 2ns
**对应功能模型**：`cache_ioatc_dc()`（`iommu_atc.cc` L8-33）

```
void iommu_top::dc_cache_update_thread() {
    while (true) {
        iommu_task_t* task = collector_to_dc_cache_update_fifo.read();
        wait(DC_CACHE_HIT_DELAY, SC_NS);

        iommu_cache_mtx.lock();
        cache_ioatc_dc(&iommu_inst, task->device_id, &task->DC);
        iommu_cache_mtx.unlock();
    }
}
```

---

### 12.4 pc_cache_query_thread（PC Cache查询线程）

**输入**：`parser_to_pc_cache_query_fifo`（深度8）
**输出**：`pc_cache_to_collector_fifo`（深度8）
**延迟**：PC_CACHE_HIT_DELAY = 2ns
**对应功能模型**：`lookup_ioatc_pc()`（`iommu_atc.cc` L80-94）

```
void iommu_top::pc_cache_query_thread() {
    while (true) {
        iommu_task_t* task = parser_to_pc_cache_query_fifo.read();
        wait(PC_CACHE_HIT_DELAY, SC_NS);

        iommu_cache_mtx.lock();
        uint8_t status = lookup_ioatc_pc(&iommu_inst, task->device_id,
                                          task->process_id, &task->PC);
        iommu_cache_mtx.unlock();

        if (status == IOATC_HIT) {
            task->pc_valid = true;
            task->pc_hit = true;
        } else {
            task->pc_valid = false;
            task->pc_hit = false;
        }

        pc_cache_to_collector_fifo.write(task);
    }
}
```

---

### 12.5 pc_cache_update_thread（PC Cache更新线程）

**输入**：`collector_to_pc_cache_update_fifo`（深度8）
**输出**：无
**延迟**：PC_CACHE_HIT_DELAY = 2ns

```
void iommu_top::pc_cache_update_thread() {
    while (true) {
        iommu_task_t* task = collector_to_pc_cache_update_fifo.read();
        wait(PC_CACHE_HIT_DELAY, SC_NS);

        iommu_cache_mtx.lock();
        cache_ioatc_pc(&iommu_inst, task->device_id, task->process_id, &task->PC);
        iommu_cache_mtx.unlock();
    }
}
```

---

### 12.6 collector_cache_lookup_result_thread（Collector-1：缓存结果汇聚线程）

**输入**：`parser_to_collector_fifo`、`dc_cache_to_collector_fifo`、`pc_cache_to_collector_fifo`
**输出**：`collector_to_xdtw_fifo`、`collector_to_pt_cache_query_fifo`、`collector_to_msipt_cache_query_fifo`、`collector_to_fault_fifo`
**延迟**：COLLECTOR_DELAY = 1ns
**对应功能模型**：`iommu_translate_iova()` 步骤7-17前半

```
void iommu_top::collector_cache_lookup_result_thread() {
    while (true) {
        wait(parser_to_collector_fifo.data_written_event() |
             dc_cache_to_collector_fifo.data_written_event() |
             pc_cache_to_collector_fifo.data_written_event());

        // --- 处理 parser_to_collector_fifo ---
        while (parser_to_collector_fifo.num_available() > 0) {
            iommu_task_t* task = parser_to_collector_fifo.read();
            collector_mtx.lock();
            pending_tasks[task->task_id].task = task;
            pending_tasks[task->task_id].parser_arrived = true;
            collector_mtx.unlock();
        }

        // --- 处理 dc_cache_to_collector_fifo ---
        while (dc_cache_to_collector_fifo.num_available() > 0) {
            iommu_task_t* task = dc_cache_to_collector_fifo.read();
            collector_mtx.lock();
            pending_tasks[task->task_id].dc_done = true;
            collector_mtx.unlock();
        }

        // --- 处理 pc_cache_to_collector_fifo ---
        while (pc_cache_to_collector_fifo.num_available() > 0) {
            iommu_task_t* task = pc_cache_to_collector_fifo.read();
            collector_mtx.lock();
            pending_tasks[task->task_id].pc_done = true;
            collector_mtx.unlock();
        }

        // ========== 遍历pending_tasks，处理三份都到齐的任务 ==========
        collector_mtx.lock();
        for (auto it = pending_tasks.begin(); it != pending_tasks.end(); ) {
            auto& entry = it->second;
            if (!entry.parser_arrived || !entry.dc_done || !entry.pc_done) {
                ++it; continue;
            }
            iommu_task_t* task = entry.task;
            it = pending_tasks.erase(it);
            collector_mtx.unlock();

            wait(COLLECTOR_DELAY, SC_NS);
            task->state = TASK_COLLECTING;

            // 步骤7：DC验证 + 步骤8-16：翻译参数配置
            // （完整逻辑同v1 Section 12.6，此处省略重复）
            // DC命中 → EN_ATS/PDTV/ENS等检查 → 配置iosatp/iohgatp
            // DC未命中 → collector_to_xdtw_fifo
            // PC未命中 → collector_to_xdtw_fifo (WALK_PDT)
            // 配置完成 → collector_to_pt_cache_query_fifo

            // ... 同v1 Section 12.6完整逻辑 ...

            collector_mtx.lock();
        }
        collector_mtx.unlock();
    }
}
```

---

### 12.7 collector_xdtw_response_thread（Collector-2：xDTW响应处理线程）

**输入**：`xdtw_to_collector_fifo`（深度4）
**输出**：`collector_to_dc_cache_update_fifo`、`collector_to_pc_cache_update_fifo`、`collector_to_pt_cache_query_fifo`、`collector_to_fault_fifo`、`collector_to_xdtw_fifo`
**延迟**：COLLECTOR_DELAY = 1ns

```
void iommu_top::collector_xdtw_response_thread() {
    while (true) {
        iommu_task_t* task = xdtw_to_collector_fifo.read();
        wait(COLLECTOR_DELAY, SC_NS);

        if (task->state == TASK_FAULT) {
            collector_to_fault_fifo.write(task);
            continue;
        }

        if (task->walk_ctx.walk_type == WALK_DDT) {
            task->dc_valid = true;
            task->dc_hit = true;
            task->DTF = task->DC.tc.DTF;
            collector_to_dc_cache_update_fifo.write(task);
            // DC验证 + 需要PC时发PDT walk + 配置翻译参数
            // ... 同v1 Section 12.7完整逻辑 ...
            configure_and_route(task);
        }
        else if (task->walk_ctx.walk_type == WALK_PDT) {
            task->pc_valid = true;
            task->pc_hit = true;
            collector_to_pc_cache_update_fifo.write(task);
            // ENS检查 + 从PC提取iosatp + 配置翻译参数
            // ... 同v1 Section 12.7完整逻辑 ...
            configure_and_route(task);
        }
    }
}

void iommu_top::configure_and_route(iommu_task_t* task) {
    task->PSCV = (task->iosatp.MODE == IOSATP_Bare) ? 0 : 1;
    task->GV = (task->iohgatp.MODE == IOHGATP_Bare) ? 0 : 1;
    task->GSCID = (task->GV == 0) ? 0 : task->iohgatp.GSCID;
    task->PSCID = (task->PSCV == 0) ? 0 : task->PSCID;
    task->check_access_perms =
        (task->TTYP != PCIE_ATS_TRANSLATION_REQUEST) ? 1 : 0;
    task->state = TASK_ROUTE_DECISION;
    collector_to_pt_cache_query_fifo.write(task);
}
```

---

### 12.8 xdtw_req_thread（v2：xDTW请求线程 —— 异步发请求）

**输入**：`collector_to_xdtw_fifo`（深度4）
**输出**：`xdtw_req_ddr_fifo`（DDR请求）
**延迟**：XDTW_COMPUTE_DELAY = 1ns（地址计算延迟）
**对应功能模型**：`locate_device_context()`（`iommu_device_context.cc` L9-228）、`locate_process_context()`（`iommu_process_context.cc` L11-196）

**v2设计**：xdtw_req_thread仅负责初始化walk上下文、计算第一次DDR读取地址、将请求发送到xdtw_req_ddr_fifo。后续的DDR响应处理和walk推进由xdtw_rsp_thread完成。这使得xDTW可以同时处理多个飞行中的walk任务。

```
// Walk阶段枚举
enum xdtw_walk_phase_t {
    XDTW_DDT_NON_LEAF,    // DDT非叶层级遍历
    XDTW_DDT_READ_DC,     // 读取最终DC条目
    XDTW_PDT_NON_LEAF,    // PDT非叶层级遍历
    XDTW_PDT_GS_IMPLICIT, // PDT的G-stage隐式翻译（每层地址转换）
    XDTW_PDT_READ_PC,     // 读取最终PC条目
};

void iommu_top::xdtw_req_thread() {
    while (true) {
        // 流控：等待outstanding_task_count低于上限
        while (xdtw_outstanding_task_count >= XDTW_MAX_OUTSTANDING_TASKS) {
            wait(xdtw_task_completed_event);
        }

        iommu_task_t* task = collector_to_xdtw_fifo.read();
        task->state = TASK_XDTW_REQ;
        xdtw_outstanding_task_count++;

        wait(XDTW_COMPUTE_DELAY, SC_NS);

        if (task->walk_ctx.walk_type == WALK_DDT) {
            // ========== DDT Walk初始化 ==========
            // 对应 iommu_device_context.cc L96-109
            uint64_t a = iommu_inst.reg_file.ddtp.ppn * PAGESIZE;
            uint8_t LEVELS;
            if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_3LVL) LEVELS = 3;
            else if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_2LVL) LEVELS = 2;
            else LEVELS = 1;

            task->walk_ctx.max_levels = LEVELS;
            task->walk_ctx.level = LEVELS - 1;
            task->walk_ctx.base_addr = a;
            task->walk_ctx.indexes[0] = task->DDI[0];
            task->walk_ctx.indexes[1] = task->DDI[1];
            task->walk_ctx.indexes[2] = task->DDI[2];

            if (LEVELS > 1) {
                // 从最高层级开始walk
                task->walk_ctx.walk_phase = XDTW_DDT_NON_LEAF;
                uint64_t read_addr = a + (task->DDI[LEVELS - 1] * 8);
                task->walk_ctx.read_addr = read_addr;
                task->walk_ctx.read_size = 8;
            } else {
                // 1级DDT：直接读DC
                task->walk_ctx.walk_phase = XDTW_DDT_READ_DC;
                uint8_t DC_SIZE = (iommu_inst.reg_file.capabilities.msi_flat == 1) ?
                                  EXT_FORMAT_DC_SIZE : BASE_FORMAT_DC_SIZE;
                uint64_t dc_addr = a + (task->DDI[0] * DC_SIZE);
                task->walk_ctx.read_addr = dc_addr;
                task->walk_ctx.read_size = DC_SIZE;
            }
        }
        else if (task->walk_ctx.walk_type == WALK_PDT) {
            // ========== PDT Walk初始化 ==========
            // 对应 iommu_process_context.cc L35-78
            uint16_t PDI[3];
            PDI[0] = get_bits(7,  0, task->process_id);
            PDI[1] = get_bits(16, 8, task->process_id);
            PDI[2] = get_bits(19, 17, task->process_id);

            uint64_t a = task->DC.fsc.pdtp.PPN * PAGESIZE;
            uint8_t LEVELS;
            if (task->DC.fsc.pdtp.MODE == PD20) LEVELS = 3;
            else if (task->DC.fsc.pdtp.MODE == PD17) LEVELS = 2;
            else LEVELS = 1;

            task->walk_ctx.max_levels = LEVELS;
            task->walk_ctx.level = LEVELS - 1;
            task->walk_ctx.base_addr = a;
            task->walk_ctx.indexes[0] = PDI[0];
            task->walk_ctx.indexes[1] = PDI[1];
            task->walk_ctx.indexes[2] = PDI[2];

            if (LEVELS > 1) {
                task->walk_ctx.walk_phase = XDTW_PDT_NON_LEAF;
                uint64_t read_addr = a + PDI[LEVELS - 1] * 8;
                task->walk_ctx.read_addr = read_addr;
                task->walk_ctx.read_size = 8;
            } else {
                task->walk_ctx.walk_phase = XDTW_PDT_READ_PC;
                uint64_t pc_addr = a + PDI[0] * 16;
                task->walk_ctx.read_addr = pc_addr;
                task->walk_ctx.read_size = 16;
            }
        }

        // 将task注册到active_walks
        xdtw_walks_mtx.lock();
        xdtw_active_walks[task->task_id] = task;
        xdtw_walks_mtx.unlock();

        // 发送第一个DDR请求到xdtw_req_ddr_fifo
        ddr_req_entry_t req;
        req.task_id = task->task_id;
        req.addr = task->walk_ctx.read_addr;
        req.size = task->walk_ctx.read_size;
        req.is_write = false;
        xdtw_req_ddr_fifo.write(req);
    }
}
```

**task字段写入**：`state`, `walk_ctx.*`（walk_phase, level, base_addr, indexes, read_addr, read_size）
**task字段读取**：`walk_ctx.walk_type`, `DDI[3]`, `process_id`, `DC.fsc.pdtp.*`
**性能特征**：线程在发送DDR请求后立即返回循环顶部，可接收下一个walk任务

---

### 12.9 xdtw_rsp_thread（v2：xDTW响应线程 —— 状态机驱动）

**输入**：`xdtw_rsp_ddr_fifo`（深度4，由DDR bw回调写入）
**输出**：`xdtw_to_collector_fifo`（walk完成）、`xdtw_req_ddr_fifo`（继续walk的下一次DDR请求）
**延迟**：XDTW_PARSE_DELAY = 1ns（数据解析延迟）
**对应功能模型**：`locate_device_context()` L112-228 + `locate_process_context()` L81-196

**v2设计**：xdtw_rsp_thread从xdtw_rsp_ddr_fifo读取DDR响应，通过task_id查找active_walks中的task，根据walk_phase状态机决定下一步操作。

```
void iommu_top::xdtw_rsp_thread() {
    while (true) {
        ddr_rsp_entry_t rsp = xdtw_rsp_ddr_fifo.read();
        wait(XDTW_PARSE_DELAY, SC_NS);

        // 查找对应的task
        xdtw_walks_mtx.lock();
        auto it = xdtw_active_walks.find(rsp.task_id);
        iommu_task_t* task = it->second;
        xdtw_walks_mtx.unlock();

        // 拷贝DDR响应数据
        memcpy(task->walk_ctx.read_buf, rsp.data, rsp.data_length);

        bool walk_complete = false;
        bool walk_fault = false;
        bool need_next_ddr = false;

        switch (task->walk_ctx.walk_phase) {

        case XDTW_DDT_NON_LEAF: {
            // ========== DDT非叶层级处理 ==========
            // 对应 iommu_device_context.cc L112-169
            ddte_t ddte;
            memcpy(&ddte.raw, task->walk_ctx.read_buf, 8);

            // 验证V bit
            if (ddte.V == 0) {
                task->cause = 258;  // DDT entry not valid
                walk_fault = true; break;
            }
            // 验证reserved bits
            if (ddte.reserved0 != 0 || ddte.reserved1 != 0) {
                task->cause = 259;  // DDT entry misconfigured
                walk_fault = true; break;
            }

            // 更新基地址并降级
            task->walk_ctx.base_addr = ddte.PPN * PAGESIZE;
            task->walk_ctx.level--;

            if (task->walk_ctx.level > 0) {
                // 还有非叶层级：继续walk
                uint64_t next_addr = task->walk_ctx.base_addr +
                    (task->walk_ctx.indexes[task->walk_ctx.level] * 8);
                task->walk_ctx.read_addr = next_addr;
                task->walk_ctx.read_size = 8;
                need_next_ddr = true;
            } else {
                // 到达叶层级：下一步读DC
                task->walk_ctx.walk_phase = XDTW_DDT_READ_DC;
                uint8_t DC_SIZE = (iommu_inst.reg_file.capabilities.msi_flat == 1) ?
                                  EXT_FORMAT_DC_SIZE : BASE_FORMAT_DC_SIZE;
                uint64_t dc_addr = task->walk_ctx.base_addr +
                    (task->walk_ctx.indexes[0] * DC_SIZE);
                task->walk_ctx.read_addr = dc_addr;
                task->walk_ctx.read_size = DC_SIZE;
                need_next_ddr = true;
            }
            break;
        }

        case XDTW_DDT_READ_DC: {
            // ========== 读取并验证DC ==========
            // 对应 iommu_device_context.cc L182-228
            uint8_t DC_SIZE = task->walk_ctx.read_size;
            memcpy(&task->DC, task->walk_ctx.read_buf, DC_SIZE);

            if (task->DC.tc.V == 0) {
                task->cause = 258;
                walk_fault = true;
            } else if (do_device_context_configuration_checks(&iommu_inst, &task->DC)) {
                task->cause = 259;
                walk_fault = true;
            } else {
                task->state = TASK_XDTW_DONE;
                walk_complete = true;
            }
            break;
        }

        case XDTW_PDT_NON_LEAF: {
            // ========== PDT非叶层级处理 ==========
            // 对应 iommu_process_context.cc L81-157
            pdte_t pdte;
            memcpy(&pdte.raw, task->walk_ctx.read_buf, 8);

            if (pdte.V == 0) {
                task->cause = 266; walk_fault = true; break;
            }
            if (pdte.reserved0 || pdte.reserved1) {
                task->cause = 267; walk_fault = true; break;
            }

            task->walk_ctx.base_addr = pdte.PPN * PAGESIZE;
            task->walk_ctx.level--;

            if (task->walk_ctx.level > 0) {
                uint64_t next_addr = task->walk_ctx.base_addr +
                    task->walk_ctx.indexes[task->walk_ctx.level] * 8;
                // 若需G-stage隐式翻译（DC.iohgatp.MODE != Bare）
                // → 切换到XDTW_PDT_GS_IMPLICIT阶段
                // 简化实现：此处直接使用GPA（不做G-stage子walk）
                task->walk_ctx.read_addr = next_addr;
                task->walk_ctx.read_size = 8;
                need_next_ddr = true;
            } else {
                // 读取最终PC
                task->walk_ctx.walk_phase = XDTW_PDT_READ_PC;
                uint64_t pc_addr = task->walk_ctx.base_addr +
                    task->walk_ctx.indexes[0] * 16;
                task->walk_ctx.read_addr = pc_addr;
                task->walk_ctx.read_size = 16;
                need_next_ddr = true;
            }
            break;
        }

        case XDTW_PDT_GS_IMPLICIT: {
            // ========== PDT G-stage隐式翻译 ==========
            // 对应 iommu_process_context.cc L93-108
            // G-stage翻译完成后回到PDT_NON_LEAF继续
            // 此阶段处理G-stage page table walk的DDR响应
            // （详细实现类似PTW的G-stage walk状态机）
            // 完成后恢复PDT walk上下文，计算翻译后的SPA
            task->walk_ctx.walk_phase = XDTW_PDT_NON_LEAF;
            // ... G-stage响应处理 ...
            need_next_ddr = true;
            break;
        }

        case XDTW_PDT_READ_PC: {
            // ========== 读取并验证PC ==========
            // 对应 iommu_process_context.cc L159-196
            memcpy(&task->PC, task->walk_ctx.read_buf, 16);

            if (task->PC.ta.V == 0) {
                task->cause = 266;
                walk_fault = true;
            } else if (do_process_context_configuration_checks(&iommu_inst, &task->DC, &task->PC)) {
                task->cause = 267;
                walk_fault = true;
            } else {
                task->state = TASK_XDTW_DONE;
                walk_complete = true;
            }
            break;
        }
        } // end switch

        if (walk_fault) {
            // Walk失败：发送到Collector处理故障
            task->state = TASK_FAULT;
            xdtw_walks_mtx.lock();
            xdtw_active_walks.erase(rsp.task_id);
            xdtw_walks_mtx.unlock();
            xdtw_outstanding_task_count--;
            xdtw_task_completed_event.notify(SC_ZERO_TIME);
            xdtw_to_collector_fifo.write(task);
        }
        else if (walk_complete) {
            // Walk成功完成：发送到Collector
            xdtw_walks_mtx.lock();
            xdtw_active_walks.erase(rsp.task_id);
            xdtw_walks_mtx.unlock();
            xdtw_outstanding_task_count--;
            xdtw_task_completed_event.notify(SC_ZERO_TIME);
            xdtw_to_collector_fifo.write(task);
        }
        else if (need_next_ddr) {
            // 继续walk：发送下一个DDR请求
            ddr_req_entry_t req;
            req.task_id = task->task_id;
            req.addr = task->walk_ctx.read_addr;
            req.size = task->walk_ctx.read_size;
            req.is_write = false;
            xdtw_req_ddr_fifo.write(req);
        }
    }
}
```

**DDR访问模式**：每层DDT/PDT需1次DDR读取；DDT最多3层+1次DC读取=4次；PDT最多3层+1次PC读取=4次。多个walk任务可交叉访问DDR（受XDTW_MAX_OUTSTANDING_TASKS限制）。

---

### 12.10 pt_cache_query_thread（PT Cache查询线程）

**输入**：`collector_to_pt_cache_query_fifo`（深度8）
**输出**：`pt_cache_to_ptw_fifo`（MISS）、`pt_cache_to_fwd_fifo`（HIT）、`collector_to_fault_fifo`（FAULT）
**延迟**：PT_CACHE_HIT_DELAY = 3ns
**对应功能模型**：`lookup_ioatc_iotlb()`（`iommu_atc.cc` L150-232）

```
void iommu_top::pt_cache_query_thread() {
    while (true) {
        iommu_task_t* task = collector_to_pt_cache_query_fifo.read();
        task->state = TASK_TLB_QUERY;
        wait(PT_CACHE_HIT_DELAY, SC_NS);

        iommu_cache_mtx.lock();
        uint8_t ioatc_status = lookup_ioatc_iotlb(
            &iommu_inst, task->iova, task->check_access_perms,
            task->priv, task->is_read, task->is_write, task->is_exec,
            task->SUM, task->PSCV, task->PSCID, task->GV, task->GSCID,
            &task->cause, &task->pa, &task->page_sz,
            &task->vs_pte, &task->g_pte, &task->is_msi);
        iommu_cache_mtx.unlock();

        if (ioatc_status == IOATC_HIT) {
            task->is_mrif = 0;
            task->state = TASK_FORWARD;
            pt_cache_to_fwd_fifo.write(task);
        }
        else if (ioatc_status == IOATC_FAULT) {
            task->state = TASK_FAULT;
            collector_to_fault_fifo.write(task);
        }
        else {
            task->state = TASK_TLB_MISS;
            pt_cache_to_ptw_fifo.write(task);
        }
    }
}
```

---

### 12.11 pt_cache_result_thread（PT Cache结果线程）

**说明**：此线程的功能已合并到 pt_cache_query_thread 中（查询后立即路由）。

---

### 12.12 pt_cache_ptw_rsp_thread（PT Cache PTW响应处理线程）

**输入**：`ptw_to_pt_cache_fifo`（深度4）
**输出**：`pt_cache_to_fwd_fifo`（深度8）、`collector_to_fault_fifo`（深度8）、`collector_to_msipt_cache_query_fifo`
**延迟**：PT_CACHE_HIT_DELAY = 3ns

```
void iommu_top::pt_cache_ptw_rsp_thread() {
    while (true) {
        iommu_task_t* task = ptw_to_pt_cache_fifo.read();

        if (task->state == TASK_FAULT) {
            collector_to_fault_fifo.write(task);
            continue;
        }

        wait(PT_CACHE_HIT_DELAY, SC_NS);

        // 步骤18：MSI地址翻译判断
        if (task->DC.msiptp.MODE != MSIPTP_Off) {
            uint8_t is_msi_addr = check_is_msi_address(task->gpa, &task->DC, &iommu_inst);
            if (is_msi_addr) {
                task->is_msi = 1;
                task->state = TASK_MSI_QUERY;
                collector_to_msipt_cache_query_fifo.write(task);
                continue;
            }
        }

        // PBMT聚合 & 页面大小聚合
        task->vs_pte.PBMT = (task->vs_pte.PBMT != PMA) ?
                             task->vs_pte.PBMT : task->g_pte.PBMT;
        task->page_sz = (task->gst_page_sz < task->page_sz) ?
                         task->gst_page_sz : task->page_sz;
        task->pa = (task->pa & ~(task->page_sz - 1)) |
                   (task->iova & (task->page_sz - 1));

        // IOTLB缓存填充
        uint64_t napot_ppn = (((task->pa & ~(task->page_sz - 1)) |
                              ((task->page_sz/2) - 1)) / PAGESIZE);
        uint64_t napot_iova = (((task->iova & ~(task->page_sz - 1)) |
                               ((task->page_sz/2) - 1)) / PAGESIZE);

        if (task->at == ADDR_TYPE_UNTRANSLATED &&
            (task->is_msi == 0 || (task->is_msi == 1 && task->is_mrif == 0))) {
            iommu_cache_mtx.lock();
            cache_ioatc_iotlb(&iommu_inst, napot_iova, task->GV, task->PSCV,
                              task->iohgatp.GSCID, task->PSCID,
                              &task->vs_pte, &task->g_pte, napot_ppn,
                              ((task->page_sz > PAGESIZE) ? 1 : 0), task->is_msi);
            iommu_cache_mtx.unlock();
        }

        task->state = TASK_FORWARD;
        pt_cache_to_fwd_fifo.write(task);
    }
}
```

---

### 12.13 ptw_req_thread（v2：PTW请求线程 —— 异步发请求）

**输入**：`pt_cache_to_ptw_fifo`（深度4）
**输出**：`ptw_req_ddr_fifo`（DDR请求）
**延迟**：PTW_COMPUTE_DELAY = 1ns
**对应功能模型**：`two_stage_address_translation()`（`iommu_two_stage_trans.cc` L8-599）+ `second_stage_address_translation()`（`iommu_second_stage_trans.cc` L7-423）

**v2设计**：ptw_req_thread仅负责初始化walk上下文（VS-stage + G-stage参数）、计算第一次DDR读取地址、发送请求到ptw_req_ddr_fifo。后续的多步walk推进（含嵌套G-stage）由ptw_rsp_thread的状态机完成。

```
// PTW Walk阶段枚举
enum ptw_walk_phase_t {
    PTW_VS_WALK,       // VS-stage页表walk（每层读PTE）
    PTW_GS_IMPLICIT,   // G-stage隐式翻译（翻译VS PTE地址为SPA）
    PTW_GS_EXPLICIT,   // G-stage显式翻译（最终GPA→SPA）
    PTW_AD_UPDATE,     // A/D bit AMO更新
};

void iommu_top::ptw_req_thread() {
    while (true) {
        // 流控
        while (ptw_outstanding_task_count >= PTW_MAX_OUTSTANDING_TASKS) {
            wait(ptw_task_completed_event);
        }

        iommu_task_t* task = pt_cache_to_ptw_fifo.read();
        task->state = TASK_PTW_REQ;
        ptw_outstanding_task_count++;

        wait(PTW_COMPUTE_DELAY, SC_NS);

        // ========== 1. Bare模式快速路径 ==========
        // 对应 iommu_two_stage_trans.cc L39-82
        if (task->iosatp.MODE == IOSATP_Bare) {
            task->vs_pte.raw = 0;
            task->vs_pte.D = task->vs_pte.A = task->vs_pte.G = task->vs_pte.U = 1;
            task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = task->vs_pte.V = 1;
            task->vs_pte.PBMT = PMA;
            task->gpa = task->iova;
            task->page_sz = get_bare_page_size(&iommu_inst);

            // 直接进入G-stage显式翻译
            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                // 初始化G-stage walk
                init_gstage_walk(task, task->gpa);
                task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                // 注册到active_walks并发送DDR请求
                ptw_walks_mtx.lock();
                ptw_active_walks[task->task_id] = task;
                ptw_walks_mtx.unlock();

                ddr_req_entry_t req;
                req.task_id = task->task_id;
                req.addr = task->walk_ctx.read_addr;
                req.size = task->walk_ctx.read_size;
                req.is_write = false;
                ptw_req_ddr_fifo.write(req);
                continue;
            } else {
                // G-stage也是Bare：直接完成
                task->pa = task->gpa;
                task->gst_page_sz = get_gstage_bare_page_size(&iommu_inst);
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.X = task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.PBMT = PMA;
                task->state = TASK_PTW_DONE;
                ptw_outstanding_task_count--;
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                ptw_to_pt_cache_fifo.write(task);
                continue;
            }
        }

        // ========== 2. VS-stage Walk初始化 ==========
        {
            uint16_t vpn[5];
            uint8_t LEVELS, PTESIZE;
            extract_vpn(task->iova, task->iosatp.MODE, task->DC.tc.SXL,
                        vpn, &LEVELS, &PTESIZE);

            // canonical检查
            if (!check_canonical(task->iova, task->iosatp.MODE, task->DC.tc.SXL)) {
                task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                task->state = TASK_FAULT;
                ptw_outstanding_task_count--;
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                ptw_to_pt_cache_fifo.write(task);
                continue;
            }

            int8_t i = LEVELS - 1;
            uint64_t a = task->iosatp.PPN * PAGESIZE;

            // 保存VPN到walk上下文
            for (int k = 0; k < 5; k++) task->walk_ctx.vpn[k] = vpn[k];
            task->walk_ctx.max_levels = LEVELS;
            task->walk_ctx.ptesize = PTESIZE;

            // Walker Cache查询
            walker_cache_mtx.lock();
            int hit_level = walker_cache.lookup(task->GSCID, task->PSCID,
                                                 task->iova, task->iosatp.MODE);
            if (hit_level >= 0) {
                a = walker_cache.get_ppn(hit_level) * PAGESIZE;
                i = hit_level - 1;
            }
            walker_cache_mtx.unlock();

            task->walk_ctx.level = i;
            task->walk_ctx.base_addr = a;

            // 计算第一次PTE地址
            uint64_t pte_addr = a + vpn[i] * PTESIZE;

            // 是否需要G-stage隐式翻译
            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                // 初始化G-stage隐式walk来翻译pte_addr
                task->walk_ctx.pending_vs_pte_addr = pte_addr;
                task->walk_ctx.vs_level = i;
                init_gstage_walk(task, pte_addr);
                task->walk_ctx.walk_phase = PTW_GS_IMPLICIT;
            } else {
                // 无G-stage：直接读PTE
                task->walk_ctx.read_addr = pte_addr;
                task->walk_ctx.read_size = PTESIZE;
                task->walk_ctx.walk_phase = PTW_VS_WALK;
            }

            // 注册到active_walks并发送DDR请求
            ptw_walks_mtx.lock();
            ptw_active_walks[task->task_id] = task;
            ptw_walks_mtx.unlock();

            ddr_req_entry_t req;
            req.task_id = task->task_id;
            req.addr = task->walk_ctx.read_addr;
            req.size = task->walk_ctx.read_size;
            req.is_write = false;
            ptw_req_ddr_fifo.write(req);
        }
    }
}

// 辅助函数：初始化G-stage walk上下文
void iommu_top::init_gstage_walk(iommu_task_t* task, uint64_t gpa) {
    // 对应 iommu_second_stage_trans.cc L80-139
    uint16_t gs_vpn[5];
    uint8_t GS_LEVELS;
    extract_gs_vpn(gpa, task->iohgatp.MODE, gs_vpn, &GS_LEVELS);
    for (int k = 0; k < 5; k++) task->walk_ctx.gs_vpn[k] = gs_vpn[k];
    task->walk_ctx.gs_level = GS_LEVELS - 1;
    task->walk_ctx.gs_base_addr = task->iohgatp.PPN * PAGESIZE;
    uint64_t gs_pte_addr = task->walk_ctx.gs_base_addr +
                           gs_vpn[GS_LEVELS - 1] * 8; // G-stage PTESIZE=8
    task->walk_ctx.read_addr = gs_pte_addr;
    task->walk_ctx.read_size = 8;
}
```

**task字段写入**：`state`, `walk_ctx.*`（所有walk相关状态）, `vs_pte`(Bare), `gpa`(Bare), `page_sz`(Bare), `g_pte`(Bare), `pa`(Bare)
**性能特征**：线程在发送DDR请求后立即返回循环顶部，支持最多PTW_MAX_OUTSTANDING_TASKS个并发walk

---

### 12.14 ptw_rsp_thread（v2：PTW响应线程 —— 多阶段状态机）

**输入**：`ptw_rsp_ddr_fifo`（深度8，由DDR bw回调写入）
**输出**：`ptw_to_pt_cache_fifo`（walk完成）、`ptw_req_ddr_fifo`（继续walk的下一次DDR请求）
**延迟**：PTW_PARSE_DELAY = 1ns
**对应功能模型**：`iommu_two_stage_trans.cc` L163-599 + `iommu_second_stage_trans.cc` L139-423

**v2设计**：ptw_rsp_thread处理三种walk阶段的DDR响应：
- **PTW_VS_WALK**：VS-stage PTE读取→解析→叶/非叶判断
- **PTW_GS_IMPLICIT**：G-stage隐式翻译PTE读取→解析→翻译VS PTE地址
- **PTW_GS_EXPLICIT**：G-stage显式翻译PTE读取→解析→最终PA计算

```
void iommu_top::ptw_rsp_thread() {
    while (true) {
        ddr_rsp_entry_t rsp = ptw_rsp_ddr_fifo.read();
        wait(PTW_PARSE_DELAY, SC_NS);

        // 查找对应的task
        ptw_walks_mtx.lock();
        iommu_task_t* task = ptw_active_walks[rsp.task_id];
        ptw_walks_mtx.unlock();

        memcpy(task->walk_ctx.read_buf, rsp.data, rsp.data_length);

        bool walk_complete = false;
        bool walk_fault = false;
        bool need_next_ddr = false;

        switch (task->walk_ctx.walk_phase) {

        case PTW_VS_WALK: {
            // ========== VS-stage PTE解析 ==========
            // 对应 iommu_two_stage_trans.cc L163-471
            spte_t pte;
            pte.raw = 0;
            memcpy(&pte.raw, task->walk_ctx.read_buf, task->walk_ctx.ptesize);

            // 步骤3: PTE有效性检查 (L227-237)
            if ((pte.V == 0) || (pte.R == 0 && pte.W == 1) ||
                (pte.PBMT == 3) || (pte.reserved != 0)) {
                task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                walk_fault = true; break;
            }

            // NAPOT检查：非叶级别的N bit必须为0 (L258)
            if (task->walk_ctx.level != 0 && pte.N) {
                task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                walk_fault = true; break;
            }

            // 步骤4: 叶/非叶判断
            if (pte.R == 1 || pte.X == 1) {
                // ===== 叶节点（步骤5-7） =====
                // 权限检查 (L329-365)
                if (task->check_access_perms) {
                    if ((task->is_exec && pte.X == 0) ||
                        (task->is_read && pte.R == 0) ||
                        (task->is_write && pte.W == 0)) {
                        task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                        walk_fault = true; break;
                    }
                }
                if (task->priv == U_MODE && pte.U == 0) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true; break;
                }
                if (task->is_exec && task->priv == S_MODE && pte.U == 1) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true; break;
                }
                if (task->priv == S_MODE && !task->is_exec &&
                    task->SUM == 0 && pte.U == 1) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true; break;
                }

                // 超级页面对齐检查 + page_sz计算 (L394-408)
                task->page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.level; j++) {
                    task->page_sz *= (task->walk_ctx.ptesize == 8) ? 512 : 1024;
                }
                // 对齐检查：低位PPN必须为0
                // ... superpage alignment verification ...

                // A/D bit处理 (L437-471)
                if (pte.A == 0 || (task->is_write && pte.D == 0)) {
                    if (task->DC.tc.SADE == 1) {
                        // 硬件A/D更新：需要AMO DDR写
                        pte.A = 1;
                        if (task->is_write) pte.D = 1;
                        task->walk_ctx.ad_pte = pte;
                        task->walk_ctx.ad_pte_addr = task->walk_ctx.base_addr +
                            task->walk_ctx.vpn[task->walk_ctx.level] * task->walk_ctx.ptesize;
                        // 保存叶PTE结果，A/D更新后再进入G-stage或完成
                        task->vs_pte = pte;
                        task->gpa = ((pte.PPN * PAGESIZE) & ~(task->page_sz - 1)) |
                                    (task->iova & (task->page_sz - 1));
                        task->walk_ctx.walk_phase = PTW_AD_UPDATE;
                        task->walk_ctx.read_addr = task->walk_ctx.ad_pte_addr;
                        task->walk_ctx.read_size = task->walk_ctx.ptesize;
                        need_next_ddr = true;
                        break;
                    } else {
                        // 软件A/D管理：触发page fault
                        task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                        walk_fault = true; break;
                    }
                }

                // NAPOT PPN调整 (L481-486)
                if (pte.N) {
                    pte.PPN = (pte.PPN & ~0xF) |
                              ((task->iova / PAGESIZE) & 0xF);
                }

                // GPA计算 (L497)
                task->gpa = ((pte.PPN * PAGESIZE) & ~(task->page_sz - 1)) |
                            (task->iova & (task->page_sz - 1));
                task->vs_pte = pte;

                // Walker Cache更新（叶PTE）
                walker_cache_mtx.lock();
                walker_cache.update(task->GSCID, task->PSCID, task->iova,
                                     task->iosatp.MODE, task->walk_ctx.level, pte.PPN);
                walker_cache_mtx.unlock();

                // 步骤19: G-stage显式翻译
                if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                    init_gstage_walk(task, task->gpa);
                    task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                    need_next_ddr = true;
                } else {
                    // G-stage Bare → 直接完成
                    task->pa = task->gpa;
                    task->gst_page_sz = get_gstage_bare_page_size(&iommu_inst);
                    task->g_pte.raw = 0;
                    task->g_pte.PPN = task->gpa / PAGESIZE;
                    task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                    task->g_pte.X = task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                    task->g_pte.PBMT = PMA;
                    task->state = TASK_PTW_DONE;
                    walk_complete = true;
                }
            } else {
                // ===== 非叶节点 =====
                // 非叶PTE reserved bits检查 (L280-293)
                if (pte.PBMT != 0 || pte.D != 0 || pte.A != 0 || pte.U != 0) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true; break;
                }

                // Walker Cache更新（非叶PPN）
                walker_cache_mtx.lock();
                walker_cache.update(task->GSCID, task->PSCID, task->iova,
                                     task->iosatp.MODE, task->walk_ctx.level, pte.PPN);
                walker_cache_mtx.unlock();

                task->walk_ctx.level--;
                if (task->walk_ctx.level < 0) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true; break;
                }

                task->walk_ctx.base_addr = pte.PPN * PAGESIZE;
                uint64_t next_pte_addr = task->walk_ctx.base_addr +
                    task->walk_ctx.vpn[task->walk_ctx.level] * task->walk_ctx.ptesize;

                // 是否需要G-stage隐式翻译
                if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                    task->walk_ctx.pending_vs_pte_addr = next_pte_addr;
                    init_gstage_walk(task, next_pte_addr);
                    task->walk_ctx.walk_phase = PTW_GS_IMPLICIT;
                } else {
                    task->walk_ctx.read_addr = next_pte_addr;
                    task->walk_ctx.read_size = task->walk_ctx.ptesize;
                    // walk_phase保持PTW_VS_WALK
                }
                need_next_ddr = true;
            }
            break;
        }

        case PTW_GS_IMPLICIT: {
            // ========== G-stage隐式翻译：翻译VS PTE地址为SPA ==========
            // 对应 iommu_second_stage_trans.cc L139-255
            // 此阶段将VS-stage PTE的GPA地址翻译为物理地址(SPA)
            gpte_t gs_pte;
            gs_pte.raw = 0;
            memcpy(&gs_pte.raw, task->walk_ctx.read_buf, 8);

            // G-stage PTE有效性检查
            if (gs_pte.V == 0 || (gs_pte.R == 0 && gs_pte.W == 1)) {
                task->cause = 21;  // Guest load page fault (implicit)
                task->iotval2 = task->walk_ctx.pending_vs_pte_addr;
                walk_fault = true; break;
            }

            if (gs_pte.R == 1 || gs_pte.X == 1) {
                // G-stage叶节点：计算翻译后的SPA
                // 权限检查（隐式翻译：只需要R权限）
                if (gs_pte.R == 0) {
                    task->cause = 21;
                    walk_fault = true; break;
                }

                uint64_t gs_page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.gs_level; j++)
                    gs_page_sz *= 512;

                // 超级页面对齐检查
                // ... alignment verification ...

                uint64_t spa = ((gs_pte.PPN * PAGESIZE) & ~(gs_page_sz - 1)) |
                               (task->walk_ctx.pending_vs_pte_addr & (gs_page_sz - 1));

                // G-stage A/D bit检查（隐式翻译仅需A=1）
                if (gs_pte.A == 0) {
                    if (task->DC.tc.GADE == 1) {
                        gs_pte.A = 1;
                        // 简化处理：直接标记更新完成
                    } else {
                        task->cause = 21;
                        walk_fault = true; break;
                    }
                }

                // 回到VS-stage walk：读取翻译后地址的VS PTE
                task->walk_ctx.read_addr = spa;
                task->walk_ctx.read_size = task->walk_ctx.ptesize;
                task->walk_ctx.walk_phase = PTW_VS_WALK;
            } else {
                // G-stage非叶：继续walk
                if (gs_pte.PBMT != 0 || gs_pte.D != 0 || gs_pte.A != 0 ||
                    gs_pte.U != 0) {
                    task->cause = 21;
                    walk_fault = true; break;
                }
                task->walk_ctx.gs_level--;
                if (task->walk_ctx.gs_level < 0) {
                    task->cause = 21;
                    walk_fault = true; break;
                }
                task->walk_ctx.gs_base_addr = gs_pte.PPN * PAGESIZE;
                uint64_t gs_next = task->walk_ctx.gs_base_addr +
                    task->walk_ctx.gs_vpn[task->walk_ctx.gs_level] * 8;
                task->walk_ctx.read_addr = gs_next;
                task->walk_ctx.read_size = 8;
                // walk_phase保持PTW_GS_IMPLICIT
            }
            need_next_ddr = true;
            break;
        }

        case PTW_GS_EXPLICIT: {
            // ========== G-stage显式翻译：翻译GPA到最终PA ==========
            // 对应 iommu_second_stage_trans.cc L139-423
            gpte_t gs_pte;
            gs_pte.raw = 0;
            memcpy(&gs_pte.raw, task->walk_ctx.read_buf, 8);

            // G-stage PTE有效性检查
            if (gs_pte.V == 0 || (gs_pte.R == 0 && gs_pte.W == 1)) {
                set_guest_fault_cause(task, GST_PAGE_FAULT);
                walk_fault = true; break;
            }

            if (gs_pte.R == 1 || gs_pte.X == 1) {
                // G-stage叶节点：权限检查 + PA计算
                // 权限检查 (L278-283)
                if (task->check_access_perms) {
                    if (task->is_read && gs_pte.R == 0) {
                        set_guest_fault_cause(task, GST_PAGE_FAULT);
                        walk_fault = true; break;
                    }
                    if (task->is_write && gs_pte.W == 0) {
                        set_guest_fault_cause(task, GST_PAGE_FAULT);
                        walk_fault = true; break;
                    }
                    if (task->is_exec && gs_pte.X == 0) {
                        set_guest_fault_cause(task, GST_PAGE_FAULT);
                        walk_fault = true; break;
                    }
                }

                // A/D bit检查 (L364-396)
                if (gs_pte.A == 0 || (task->is_write && gs_pte.D == 0)) {
                    if (task->DC.tc.GADE == 1) {
                        gs_pte.A = 1;
                        if (task->is_write) gs_pte.D = 1;
                        // 简化处理：直接标记更新完成
                    } else {
                        set_guest_fault_cause(task, GST_PAGE_FAULT);
                        walk_fault = true; break;
                    }
                }

                // PA计算 (L417)
                task->gst_page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.gs_level; j++)
                    task->gst_page_sz *= 512;
                task->pa = ((gs_pte.PPN * PAGESIZE) & ~(task->gst_page_sz - 1)) |
                           (task->gpa & (task->gst_page_sz - 1));
                task->g_pte = gs_pte;

                // handle_virtual_interrupt_file_overlap (L440)
                // 调整gst_page_sz避免跨越VIF边界

                task->state = TASK_PTW_DONE;
                walk_complete = true;
            } else {
                // G-stage非叶：继续walk
                if (gs_pte.PBMT != 0 || gs_pte.D != 0 || gs_pte.A != 0 ||
                    gs_pte.U != 0) {
                    set_guest_fault_cause(task, GST_PAGE_FAULT);
                    walk_fault = true; break;
                }
                task->walk_ctx.gs_level--;
                if (task->walk_ctx.gs_level < 0) {
                    set_guest_fault_cause(task, GST_PAGE_FAULT);
                    walk_fault = true; break;
                }
                task->walk_ctx.gs_base_addr = gs_pte.PPN * PAGESIZE;
                uint64_t gs_next = task->walk_ctx.gs_base_addr +
                    task->walk_ctx.gs_vpn[task->walk_ctx.gs_level] * 8;
                task->walk_ctx.read_addr = gs_next;
                task->walk_ctx.read_size = 8;
                // walk_phase保持PTW_GS_EXPLICIT
                need_next_ddr = true;
            }
            break;
        }

        case PTW_AD_UPDATE: {
            // ========== A/D bit AMO更新完成 ==========
            // DDR写响应已收到，A/D更新成功
            // 继续G-stage翻译或直接完成
            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                init_gstage_walk(task, task->gpa);
                task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                need_next_ddr = true;
            } else {
                task->pa = task->gpa;
                task->gst_page_sz = get_gstage_bare_page_size(&iommu_inst);
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.X = task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.PBMT = PMA;
                task->state = TASK_PTW_DONE;
                walk_complete = true;
            }
            break;
        }

        } // end switch

        // ========== 统一处理walk结果 ==========
        if (walk_fault) {
            task->state = TASK_FAULT;
            ptw_walks_mtx.lock();
            ptw_active_walks.erase(rsp.task_id);
            ptw_walks_mtx.unlock();
            ptw_outstanding_task_count--;
            ptw_task_completed_event.notify(SC_ZERO_TIME);
            ptw_to_pt_cache_fifo.write(task);
        }
        else if (walk_complete) {
            ptw_walks_mtx.lock();
            ptw_active_walks.erase(rsp.task_id);
            ptw_walks_mtx.unlock();
            ptw_outstanding_task_count--;
            ptw_task_completed_event.notify(SC_ZERO_TIME);
            ptw_to_pt_cache_fifo.write(task);
        }
        else if (need_next_ddr) {
            ddr_req_entry_t req;
            req.task_id = task->task_id;
            req.addr = task->walk_ctx.read_addr;
            req.size = task->walk_ctx.read_size;
            req.is_write = (task->walk_ctx.walk_phase == PTW_AD_UPDATE);
            if (req.is_write) {
                memcpy(req.write_data, &task->walk_ctx.ad_pte,
                       task->walk_ctx.ptesize);
            }
            ptw_req_ddr_fifo.write(req);
        }
    }
}
```

**Walker Cache交互**：
- 查询：ptw_req_thread中walk开始前查Walker Cache，命中则跳过前序层级
- 更新：ptw_rsp_thread中每遇到非叶PTE时更新对应级别的Walker Cache条目
- 三级结构：PTWc_1（直接映射，最高级PPN）、PTWc_2（2路，中间级）、PTWc_3（4路，最低级）

**DDR访问模式**：VS-stage每层1次DDR读（含可能的G-stage隐式翻译子walk）；G-stage最多Sv48x4=4层。最坏情况：Sv48(4层VS×Sv48x4(4层GS隐式)+1次GS显式(4层))=20+次DDR读。多个walk任务可交叉访问DDR（受PTW_MAX_OUTSTANDING_TASKS限制）。

---

### 12.15 msipt_cache_query_thread（MSIPT Cache查询线程）

**输入**：`collector_to_msipt_cache_query_fifo`（深度8）
**输出**：`msipt_cache_to_msiptw_fifo`（MISS）、`msipt_cache_to_fwd_fifo`（HIT）、`collector_to_fault_fifo`（FAULT）
**延迟**：MSIPT_CACHE_HIT_DELAY = 3ns
**对应功能模型**：MSI PTE缓存查找（类似IOTLB但针对MSI地址）

```
void iommu_top::msipt_cache_query_thread() {
    while (true) {
        iommu_task_t* task = collector_to_msipt_cache_query_fifo.read();
        task->state = TASK_MSI_QUERY;
        wait(MSIPT_CACHE_HIT_DELAY, SC_NS);

        // 查询MSIPT缓存
        // 对应功能模型中msi_address_translation()的缓存查找部分
        // 注：功能模型没有显式的MSIPT缓存，性能模型新增此缓存层
        iommu_cache_mtx.lock();
        uint8_t hit = lookup_msipt_cache(task->gpa, &task->DC, task);
        iommu_cache_mtx.unlock();

        if (hit) {
            // MSI缓存命中：结果已填充到task
            task->state = TASK_FORWARD;
            msipt_cache_to_fwd_fifo.write(task);
        } else {
            // MSI缓存未命中：提交MSIPTW
            task->state = TASK_MSI_MISS;
            msipt_cache_to_msiptw_fifo.write(task);
        }
    }
}
```

---

### 12.16 msipt_cache_result_thread（MSIPT Cache结果线程）

**输入**：`msiptw_to_msipt_cache_fifo`（深度4）
**输出**：`msipt_cache_to_fwd_fifo`（深度8）、`collector_to_fault_fifo`
**延迟**：MSIPT_CACHE_HIT_DELAY = 3ns
**功能**：接收MSIPTW返回结果，更新MSIPT缓存，转发到Forwarder

```
void iommu_top::msipt_cache_result_thread() {
    while (true) {
        iommu_task_t* task = msiptw_to_msipt_cache_fifo.read();

        if (task->state == TASK_FAULT) {
            collector_to_fault_fifo.write(task);
            continue;
        }

        wait(MSIPT_CACHE_HIT_DELAY, SC_NS);

        // 更新MSIPT缓存
        iommu_cache_mtx.lock();
        update_msipt_cache(task->gpa, &task->DC, task);
        iommu_cache_mtx.unlock();

        // PBMT聚合 + 页面大小计算（同pt_cache_ptw_rsp_thread）
        task->vs_pte.PBMT = (task->vs_pte.PBMT != PMA) ?
                             task->vs_pte.PBMT : task->g_pte.PBMT;

        task->state = TASK_FORWARD;
        msipt_cache_to_fwd_fifo.write(task);
    }
}
```

---

### 12.17 msiptw_req_thread（v2：MSIPTW请求线程 —— 异步发请求）

**输入**：`msipt_cache_to_msiptw_fifo`（深度4）
**输出**：`msiptw_req_ddr_fifo`（DDR请求）
**延迟**：MSIPTW_COMPUTE_DELAY = 1ns
**对应功能模型**：`msi_address_translation()`（`iommu_msi_trans.cc` L19-293）

**v2设计**：msiptw_req_thread仅负责初始化MSI walk上下文（提取interrupt file number、计算MSI PTE地址），将DDR读请求发送到msiptw_req_ddr_fifo。MSI PTE只需1次16字节DDR读取，后续解析由msiptw_rsp_thread完成。

```
// MSIPTW Walk阶段枚举
enum msiptw_walk_phase_t {
    MSIPTW_READ_MSIPTE,    // 读取16字节MSI PTE
};

void iommu_top::msiptw_req_thread() {
    while (true) {
        // 流控
        while (msiptw_outstanding_task_count >= MSIPTW_MAX_OUTSTANDING_TASKS) {
            wait(msiptw_task_completed_event);
        }

        iommu_task_t* task = msipt_cache_to_msiptw_fifo.read();
        task->state = TASK_MSIPTW_REQ;
        msiptw_outstanding_task_count++;

        wait(MSIPTW_COMPUTE_DELAY, SC_NS);

        // ========== MSI PTE地址计算 ==========
        // 对应 iommu_msi_trans.cc L57-115

        // 1. MGPAW计算 (L57-63)
        uint64_t mgpaw = calculate_mgpaw(&iommu_inst);
        uint64_t mgpaw_mask = (1ULL << mgpaw) - 1;

        // 2. 提取interrupt file number I (L108)
        uint64_t I = extract((task->gpa >> 12),
                             (task->DC.msi_addr_mask.mask & mgpaw_mask));

        // 3. 计算MSI PTE地址 (L115)
        uint64_t m = task->DC.msiptp.PPN * PAGESIZE;
        uint64_t msipte_addr = m | (I * 16);

        // 保存walk上下文
        task->walk_ctx.walk_phase = MSIPTW_READ_MSIPTE;
        task->walk_ctx.read_addr = msipte_addr;
        task->walk_ctx.read_size = 16;  // MSI PTE = 128 bits = 16 bytes

        // 注册到active_walks
        msiptw_walks_mtx.lock();
        msiptw_active_walks[task->task_id] = task;
        msiptw_walks_mtx.unlock();

        // 发送DDR请求到msiptw_req_ddr_fifo
        ddr_req_entry_t req;
        req.task_id = task->task_id;
        req.addr = msipte_addr;
        req.size = 16;
        req.is_write = false;
        msiptw_req_ddr_fifo.write(req);
    }
}
```

**task字段写入**：`state`, `walk_ctx.*`（walk_phase, read_addr, read_size）
**性能特征**：线程在发送DDR请求后立即返回循环顶部，支持最多MSIPTW_MAX_OUTSTANDING_TASKS个并发walk

---

### 12.18 msiptw_rsp_thread（v2：MSIPTW响应线程 —— MSI PTE解析）

**输入**：`msiptw_rsp_ddr_fifo`（深度4，由DDR bw回调写入）
**输出**：`msiptw_to_msipt_cache_fifo`（walk完成/故障）
**延迟**：MSIPTW_PARSE_DELAY = 1ns
**对应功能模型**：`iommu_msi_trans.cc` L127-293

**v2设计**：msiptw_rsp_thread从msiptw_rsp_ddr_fifo读取DDR响应，解析16字节MSI PTE，根据V/M字段完成翻译或报告故障。由于MSI PTE只需1次DDR读取，此线程的状态机只有一个阶段（MSIPTW_READ_MSIPTE），每次响应处理即为最终结果。

```
void iommu_top::msiptw_rsp_thread() {
    while (true) {
        ddr_rsp_entry_t rsp = msiptw_rsp_ddr_fifo.read();
        wait(MSIPTW_PARSE_DELAY, SC_NS);

        // 查找对应的task
        msiptw_walks_mtx.lock();
        iommu_task_t* task = msiptw_active_walks[rsp.task_id];
        msiptw_walks_mtx.unlock();

        memcpy(task->walk_ctx.read_buf, rsp.data, rsp.data_length);

        bool walk_complete = false;
        bool walk_fault = false;

        // ========== 解析16字节MSI PTE ==========
        // 对应 iommu_msi_trans.cc L127-293
        msipte_t msipte;
        memcpy(&msipte.raw, task->walk_ctx.read_buf, 16);

        // 验证MSI PTE (L156-184)
        if (msipte.V == 0) {
            task->cause = 262;  // MSI PTE not valid
            walk_fault = true;
        }
        else if (msipte.C == 1 || msipte.M == 0 || msipte.M == 2) {
            task->cause = 263;  // MSI PTE misconfigured
            walk_fault = true;
        }
        else if (msipte.M == 3) {
            // ===== M=3: Basic Translate/RW模式 (L191-217) =====
            if (msipte.translate_rw.reserved != 0) {
                task->cause = 263;
                walk_fault = true;
            } else {
                task->pa = (msipte.translate_rw.PPN * PAGESIZE) |
                           (task->gpa & 0xFFF);
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.X = 0;
                task->g_pte.PBMT = PMA;
                task->page_sz = PAGESIZE;
                task->is_mrif = 0;
                task->state = TASK_MSIPTW_DONE;
                walk_complete = true;
            }
        }
        else if (msipte.M == 1) {
            // ===== M=1: MRIF模式 (L219-272) =====
            if (iommu_inst.reg_file.capabilities.msi_mrif == 0) {
                task->cause = 263;
                walk_fault = true;
            } else {
                task->dest_mrif_addr = msipte.mrif.MRIF_ADDR_55_9 << 9;
                task->pa = msipte.mrif.NPPN * PAGESIZE;
                task->mrif_nid = (msipte.mrif.N10 << 10) | msipte.mrif.N90;
                task->is_mrif = 1;
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.X = 0;
                task->g_pte.PBMT = PMA;
                task->page_sz = PAGESIZE;
                task->state = TASK_MSIPTW_DONE;
                walk_complete = true;
            }
        }

        // 步骤15：执行权限检查 (L281-287)
        if (walk_complete && task->is_exec == 1 &&
            task->check_access_perms == 1) {
            task->cause = 1;  // Instruction access fault
            walk_complete = false;
            walk_fault = true;
        }

        // ========== 统一处理walk结果 ==========
        if (walk_fault) {
            task->state = TASK_FAULT;
        }

        // 清理active_walks并释放outstanding计数
        msiptw_walks_mtx.lock();
        msiptw_active_walks.erase(rsp.task_id);
        msiptw_walks_mtx.unlock();
        msiptw_outstanding_task_count--;
        msiptw_task_completed_event.notify(SC_ZERO_TIME);

        // 发送到MSIPT Cache结果线程
        msiptw_to_msipt_cache_fifo.write(task);
    }
}
```

**DDR访问模式**：MSI翻译只需1次16字节DDR读取（读MSI PTE），因此每次msiptw_rsp_thread处理一个DDR响应即为最终结果，无需循环或状态转移。多个MSIPTW任务可交叉访问DDR（受MSIPTW_MAX_OUTSTANDING_TASKS限制）。

---

### 12.19 forwarder_thread（Forwarder转发线程）

**输入**：`pt_cache_to_fwd_fifo`（深度8）、`msipt_cache_to_fwd_fifo`（深度8）
**输出**：通过nb_transport_fw转发到AXI Master/Stream socket
**延迟**：FORWARDER_DELAY = 2ns
**对应功能模型**：步骤20（`iommu_translate.cc` L494-613）+ iommu_top.cc L147-202的转发逻辑

```
void iommu_top::forwarder_thread() {
    while (true) {
        // 使用OR-list等待两个输入FIFO
        wait(pt_cache_to_fwd_fifo.data_written_event() |
             msipt_cache_to_fwd_fifo.data_written_event());

        iommu_task_t* task = nullptr;

        // 优先处理pt_cache_to_fwd_fifo（常规翻译路径）
        if (pt_cache_to_fwd_fifo.num_available() > 0) {
            task = pt_cache_to_fwd_fifo.read();
        } else if (msipt_cache_to_fwd_fifo.num_available() > 0) {
            task = msipt_cache_to_fwd_fifo.read();
        }
        if (!task) continue;

        wait(FORWARDER_DELAY, SC_NS);
        task->state = TASK_FORWARD;

        // ========== 步骤20：响应生成 ==========
        // 对应 iommu_translate.cc L494-613

        // 1. 设置翻译结果到原始payload
        tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;
        trans->set_response_status(tlm::TLM_OK_RESPONSE);

        // 2. Bare模式PPN计算 (L502-524)
        uint8_t is_bare = (task->PSCV == 0 && task->GV == 0) ? 1 : 0;
        uint64_t final_pa;
        if (is_bare) {
            final_pa = task->pa;
        } else {
            // NAPOT格式PA计算
            uint64_t napot_mask = (task->page_sz/2/PAGESIZE) - 1;
            uint64_t actual_ppn = (task->pa >> 12) & ~napot_mask;
            final_pa = (actual_ppn << 12) | (task->iova & 0xFFF);
        }

        // 3. ATS响应字段填充 (L533-612)
        // 如果TTYP == PCIE_ATS_TRANSLATION_REQUEST:
        //   rsp.Priv = (pasid_valid && priv_req) ? 1 : 0
        //   rsp.R = (vs_pte.R & g_pte.R)
        //   rsp.W = (vs_pte.W & g_pte.W & vs_pte.D & g_pte.D)
        //   rsp.Exe = (vs_pte.X & g_pte.X & vs_pte.R & g_pte.R) & exec_req

        // 4. 转发到下游
        if (task->is_msi == 1 && task->is_mrif == 0) {
            // MSI转发到IMSIC (对应 iommu_top.cc L152-157)
            trans->set_address(task->pa);
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            sc_time delay = SC_ZERO_TIME;
            axi_stream_to_cmn_rnd_socket->nb_transport_fw(*trans, phase, delay);
        } else if (task->TTYP == PCIE_ATS_TRANSLATION_REQUEST) {
            // ATS响应：通过nb_transport_bw返回给Initiator
            send_response_to_initiator(task);
        } else {
            // DMA转发 (对应 iommu_top.cc L158-202)
            trans->set_address(final_pa);
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            sc_time delay = SC_ZERO_TIME;
            axi_master_0_to_pcie_noc_socket->nb_transport_fw(*trans, phase, delay);
        }

        // 5. 通知Initiator翻译完成（非ATS情况）
        if (task->TTYP != PCIE_ATS_TRANSLATION_REQUEST) {
            send_response_to_initiator(task);
        }

        // 6. 释放任务
        task->state = TASK_DONE;
        delete task;  // 所有权终结点
    }
}
```

**task字段读取**：`pa`, `page_sz`, `is_msi`, `is_mrif`, `PSCV`, `GV`, `TTYP`, `vs_pte`, `g_pte`, `iova`, `tlm_trans_ptr`, `dest_mrif_addr`
**task生命周期**：Forwarder是任务的`delete`终结点（正常完成路径）

---

### 12.20 fault_cq_proc_thread（Fault/CQ处理线程）

**输入**：`collector_to_fault_fifo`（深度8）
**输出**：通过nb_transport_bw返回故障响应给Initiator
**延迟**：FORWARDER_DELAY = 2ns
**对应功能模型**：`report_fault()`（`iommu_faults.cc` L8-159）+ 故障分类（`iommu_translate.cc` L654-730）

```
void iommu_top::fault_cq_proc_thread() {
    while (true) {
        iommu_task_t* task = collector_to_fault_fifo.read();
        wait(FORWARDER_DELAY, SC_NS);

        // ========== 1. 调用report_fault记录故障 ==========
        // 对应 iommu_faults.cc L8-159
        report_fault(&iommu_inst, task->cause, task->iova, task->iotval2,
                     task->TTYP, task->DTF, task->device_id,
                     task->pid_valid, task->process_id, task->priv);

        // ========== 2. 确定响应类型 ==========
        // 对应 iommu_translate.cc L654-730

        tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;

        if (task->at != ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) {
            // 非ATS请求：UNSUPPORTED_REQUEST
            // 对应 L657-661
            trans->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        } else {
            // ATS请求：根据cause分类
            // CA类 (L679-685): cause ∈ {1,5,7,261,263,265,267,268-272,274}
            // UR类 (L696-700): cause ∈ {256-260}
            // Success with R=W=0 (L722-730): cause ∈ {12,13,15,20,21,23,266,262}

            if (task->cause == 12 || task->cause == 13 || task->cause == 15 ||
                task->cause == 20 || task->cause == 21 || task->cause == 23 ||
                task->cause == 266 || task->cause == 262) {
                // ATS Success with R=W=0
                // 对应 L722-730: 不记录到fault queue
                trans->set_response_status(tlm::TLM_OK_RESPONSE);
                // 返回的ATS completion中R=W=0
            } else if (task->cause == 1 || task->cause == 5 || task->cause == 7 ||
                       task->cause == 261 || task->cause == 263 || task->cause == 265 ||
                       task->cause == 267 || task->cause >= 268) {
                // Completer Abort
                trans->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            } else {
                // Unsupported Request
                trans->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            }
        }

        // ========== 3. 通过nb_transport_bw返回响应 ==========
        send_response_to_initiator(task);

        // ========== 4. 释放任务 ==========
        task->state = TASK_DONE;
        delete task;  // 所有权终结点（故障路径）
    }
}
```

**task生命周期**：Fault/CQ Proc是任务的`delete`终结点（故障路径）

**注意**：`report_fault()`内部通过`write_memory()`写故障队列到DDR。在v2中，此DDR写走`ctrl_path_req_ddr_fifo`路径，由`ddr_arbiter_thread`统一仲裁。`report_fault()`自身为同步调用，内部等待`ctrl_path_rsp_event`完成后返回。

---

### 12.21 ddr_arbiter_thread（v2新增：DDR请求仲裁线程）

**输入**：7个DDR请求FIFO：`ctrl_path_req_ddr_fifo`、`xdtw_req_ddr_fifo`、`ptw_req_ddr_fifo`、`msiptw_req_ddr_fifo`
**输出**：通过`nb_transport_fw`发送到DDR Module；DDR响应路由到对应的`rsp_ddr_fifo`
**延迟**：DDR_ARBITER_DELAY = 0ns（仲裁逻辑为纯组合逻辑延迟）
**DDR延迟**：由DDR Module在`ddr_process_thread`中模拟（DDR_ACCESS_DELAY_NS）

**v2设计**：ddr_arbiter_thread是DDR访问的唯一出口。所有Walker模块的DDR请求通过各自的req_ddr_fifo提交，arbiter按优先级从中取出请求，构造TLM payload发送到DDR，同时在ddr_pending_queue中记录pending条目。DDR响应通过nb_transport_bw回调按FIFO顺序匹配pending条目，将响应数据路由到对应模块的rsp_ddr_fifo。

**仲裁优先级**：
1. `ctrl_path_req_ddr_fifo`（最高优先级：report_fault等控制路径DDR写）
2. Round-robin轮转：`xdtw_req_ddr_fifo` → `ptw_req_ddr_fifo` → `msiptw_req_ddr_fifo`

**详细伪代码**已在Section 5.3中给出。此处补充完整的线程规格：

```
void iommu_top::ddr_arbiter_thread() {
    uint8_t rr_index = 0;  // round-robin轮转索引
    while (true) {
        // 等待任意req_ddr_fifo有数据
        wait(ctrl_path_req_ddr_fifo.data_written_event() |
             xdtw_req_ddr_fifo.data_written_event() |
             ptw_req_ddr_fifo.data_written_event() |
             msiptw_req_ddr_fifo.data_written_event());

        // 流控：检查全局飞行请求数
        while (ddr_pending_queue.size() >= DDR_MAX_OUTSTANDING) {
            wait(ddr_pending_freed_event);
        }

        // 优先级仲裁：ctrl_path > round-robin(xDTW, PTW, MSIPTW)
        ddr_req_entry_t req;
        uint8_t source_module;

        if (ctrl_path_req_ddr_fifo.num_available() > 0) {
            ctrl_path_ddr_req_t ctrl_req = ctrl_path_req_ddr_fifo.read();
            req.task_id = 0;
            req.addr = ctrl_req.addr;
            req.size = ctrl_req.size;
            req.is_write = ctrl_req.is_write;
            memcpy(req.write_data, ctrl_req.write_data, ctrl_req.size);
            source_module = 3;  // CTRL_PATH
        } else {
            sc_fifo<ddr_req_entry_t>* fifos[3] = {
                &xdtw_req_ddr_fifo, &ptw_req_ddr_fifo, &msiptw_req_ddr_fifo
            };
            bool found = false;
            for (int k = 0; k < 3; k++) {
                uint8_t idx = (rr_index + k) % 3;
                if (fifos[idx]->num_available() > 0) {
                    req = fifos[idx]->read();
                    source_module = idx;  // 0=XDTW, 1=PTW, 2=MSIPTW
                    rr_index = (idx + 1) % 3;
                    found = true;
                    break;
                }
            }
            if (!found) continue;
        }

        // 构造ddr_pending_entry并入队
        ddr_pending_entry_t pending;
        pending.task_id = req.task_id;
        pending.source_module = source_module;
        pending.addr = req.addr;
        pending.size = req.size;

        // 分配TLM payload
        tlm_generic_payload* trans = new tlm_generic_payload();
        trans->set_address(req.addr);
        trans->set_data_length(req.size);
        if (req.is_write) {
            trans->set_command(tlm::TLM_WRITE_COMMAND);
            uint8_t* wdata = new uint8_t[req.size];
            memcpy(wdata, req.write_data, req.size);
            trans->set_data_ptr(wdata);
        } else {
            trans->set_command(tlm::TLM_READ_COMMAND);
            uint8_t* rdata = new uint8_t[req.size];
            trans->set_data_ptr(rdata);
        }
        pending.trans_ptr = trans;

        ddr_queue_mtx.lock();
        ddr_pending_queue.push(pending);
        ddr_queue_mtx.unlock();

        // 发送nb_transport_fw到DDR
        tlm::tlm_phase phase = tlm::BEGIN_REQ;
        sc_time delay = SC_ZERO_TIME;
        axi_master_1_to_cmn_rnd_socket->nb_transport_fw(*trans, phase, delay);
    }
}
```

**nb_transport_bw回调**（DDR响应路由，已在Section 5.3定义）：
- 从`ddr_pending_queue`头部pop（FIFO保序保证）
- 根据`source_module`路由到`xdtw_rsp_ddr_fifo`(0) / `ptw_rsp_ddr_fifo`(1) / `msiptw_rsp_ddr_fifo`(2) / `ctrl_path_rsp_event`(3)
- 释放TLM payload内存
- 通知`ddr_pending_freed_event`让arbiter继续发送

**全局DDR流控**：`DDR_MAX_OUTSTANDING`（默认8）限制全局飞行DDR请求数。此参数与各Walker模块的`MAX_OUTSTANDING_TASKS`共同决定DDR带宽利用率。

---

## 十三、实现关键注意事项（代码验证后补充）

基于对功能模型代码的完整验证，补充以下实现要点：

### 13.1 Socket类型：保留simple_*_socket即可

**发现**：`tlm_utils::simple_target_socket` 和 `tlm_utils::simple_initiator_socket` 已原生支持 nb_transport 注册。无需替换为 `tlm_target_socket`/`tlm_initiator_socket`。

**修正**：`iommu_top.hh` 中保留现有 socket 类型声明不变，仅追加 nb_transport 回调注册：
```cpp
// 构造函数中注册nb_transport（与现有b_transport并存）
axi_slave_from_pcie_noc_0_socket.register_nb_transport_fw(this, &iommu_top::axi_slave_nb_transport_fw);
axi_master_1_to_cmn_rnd_socket.register_nb_transport_bw(this, &iommu_top::ddr_nb_transport_bw);
```

**优势**：`main.cpp` 中所有 socket 绑定代码完全不需修改。

### 13.2 DDR Module：仅nb_transport（v2变更）

**v2设计**：DDR Module **移除b_transport注册**，仅支持nb_transport_fw。所有DDR访问（包括性能路径和控制路径）统一通过`ddr_arbiter_thread` → `nb_transport_fw`发送。

**v1差异**：v1中DDR Module同时注册b_transport和nb_transport_fw，控制路径（report_fault → write_memory）直接走b_transport。v2中控制路径改为通过`ctrl_path_req_ddr_fifo`提交到ddr_arbiter，由arbiter统一发送nb_transport_fw。

**方案**：
```cpp
// DDR Module构造函数 —— v2：仅注册nb_transport_fw
DDR_Module::DDR_Module(sc_module_name name) : sc_module(name) {
    axi_slave_from_cmn_rnd_1_socket.register_nb_transport_fw(
        this, &DDR_Module::nb_transport_fw);
    SC_THREAD(ddr_process_thread);
}
```

**控制路径DDR写**（report_fault等）：
```cpp
// write_memory()改造：不再直接调用b_transport，改为推入ctrl_path_req_ddr_fifo
void iommu_top::write_memory_via_arbiter(uint64_t addr, uint8_t* data, uint32_t size) {
    ctrl_path_ddr_req_t req;
    req.addr = addr;
    req.size = size;
    req.is_write = true;
    memcpy(req.write_data, data, size);
    ctrl_path_req_ddr_fifo.write(req);
    wait(ctrl_path_rsp_event);  // 同步等待DDR完成
}
```

### 13.3 功能模型缓存大小与性能模型参数

**发现**：
- 功能模型缓存大小极小：`RVI_IOMMU_DDT_CACHE_SIZE=2`, `RVI_IOMMU_PDT_CACHE_SIZE=2`, `RVI_IOMMU_TLB_SIZE=2`（定义于 `iommu_data_structures.hh`）
- 性能模型参数定义较大缓存：`DC_CACHE_SIZE=64`, `PC_CACHE_SIZE=32`, `PT_CACHE_SIZE=256`（定义于 `iommu_perf_params.hh`）
- 设计文档 Section 12 中 DC/PC/PT Cache线程调用 `lookup_ioatc_dc/pc/iotlb()` 等功能模型API，这些函数操作 `iommu_inst.ddt_cache[2]` 等2-entry数组

**Phase 1方案**（保持功能正确性）：
- 直接使用功能模型的缓存API和缓存大小（2-entry）
- 缓存命中率低但翻译结果正确
- 性能参数中的缓存大小作为未来扩展目标

**Phase 2方案**（可选，后续优化）：
- 将 `RVI_IOMMU_DDT_CACHE_SIZE` 等宏定义改为引用 `iommu_perf_params.hh` 中的值
- 或在 `iommu_struct.hh` 中将缓存数组改为动态分配

### 13.4 Walker线程req/rsp解耦设计（v2核心变更）

**v2设计确认**：Section 12 中各Walker模块（xDTW、PTW、MSIPTW）的req/rsp线程完全解耦：

**req线程职责**：
1. 从上游FIFO读取任务（带outstanding_task_count流控）
2. 初始化walk上下文（计算起始DDR地址、设置walk_phase）
3. 注册到`active_walks[task_id]`
4. 将`ddr_req_entry_t`推入`*_req_ddr_fifo`
5. 立即返回循环顶部（不等待DDR响应）

**rsp线程职责**：
1. 从`*_rsp_ddr_fifo`读取DDR响应
2. 通过`task_id`查找`active_walks`中的task
3. 根据`walk_phase`状态机解析响应数据
4. 决定：继续walk（推入新DDR请求到`*_req_ddr_fifo`）或完成（推到下游FIFO）
5. 完成时清理`active_walks`并递减`outstanding_task_count`

**与v1差异**：v1中req/rsp在同一线程内（`ddr_nb_read → wait(rsp_event)` 同步模式）。v2将等待DDR响应的逻辑移到独立的rsp线程中，使得req线程可以在一个DDR请求发出后立即处理下一个任务。

**可直接调用的功能模型函数**（纯计算，不做DDR访问）：
- `lookup_ioatc_dc/pc/iotlb()` — 缓存查询
- `cache_ioatc_dc/pc/iotlb()` — 缓存更新
- `do_device_context_configuration_checks()` — DC配置验证
- `do_process_context_configuration_checks()` — PC配置验证
- `report_fault()` — 故障报告（内部DDR写通过ctrl_path_req_ddr_fifo路由，见13.2）

**不可直接调用的功能模型函数**（内部有DDR读取）：
- `locate_device_context()` — 改为xdtw_req_thread + xdtw_rsp_thread
- `locate_process_context()` — 改为xdtw_req_thread + xdtw_rsp_thread
- `two_stage_address_translation()` — 改为ptw_req_thread + ptw_rsp_thread
- `second_stage_address_translation()` — 改为ptw_rsp_thread的PTW_GS_IMPLICIT/PTW_GS_EXPLICIT阶段
- `msi_address_translation()` — 改为msiptw_req_thread + msiptw_rsp_thread

### 13.5 iommu_task_t walk_context_t v2字段（与v1差异）

**v2移除**：
- ~~`rsp_event (sc_event)`~~ — v2中rsp线程通过rsp_ddr_fifo读取响应，不再需要事件通知
- ~~`axi_id (uint16_t)`~~ — v2使用单一AXI ID，DDR关联通过ddr_pending_queue FIFO顺序保证

**v2新增**：
```
walk_context_t:
    walk_phase (枚举)            -- 跟踪多步walk的当前阶段（PTW_VS_WALK/GS_IMPLICIT/GS_EXPLICIT/AD_UPDATE等）
    pending_vs_pte_addr (uint64_t) -- G-stage隐式翻译中待翻译的VS PTE地址
    vs_level (int8_t)            -- VS-stage当前层级（PTW嵌套walk中暂存）
    gs_level (int8_t)            -- G-stage当前层级
    gs_base_addr (uint64_t)      -- G-stage walk基地址
    gs_vpn[5] (uint16_t)         -- G-stage VPN索引
    ad_pte (spte_t)              -- A/D更新时的PTE值
    ad_pte_addr (uint64_t)       -- A/D更新时的PTE DDR地址
```

**原因**：v1的walk_context仅需跟踪单步DDR访问（发请求→等rsp_event→继续），v2的walk_context需要跟踪整个多步walk的完整状态，因为req/rsp解耦后rsp线程需要根据walk_phase恢复walk进度。

### 13.6 关键常量引用（含RVI_IOMMU_前缀）

功能模型中常量均带 `RVI_IOMMU_` 前缀，性能模型代码中需使用相同名称：

```
// IOATC状态
RVI_IOMMU_IOATC_MISS (0), RVI_IOMMU_IOATC_HIT (1), RVI_IOMMU_IOATC_FAULT (2)

// DDT模式
RVI_IOMMU_Off (0), RVI_IOMMU_DDT_Bare (1), RVI_IOMMU_DDT_1LVL (2), RVI_IOMMU_DDT_2LVL (3), RVI_IOMMU_DDT_3LVL (4)

// 页表模式
RVI_IOMMU_IOSATP_Bare (0), RVI_IOMMU_IOHGATP_Bare (0)
RVI_IOMMU_PDTP_Bare (0), RVI_IOMMU_PD8 (1), RVI_IOMMU_PD17 (2), RVI_IOMMU_PD20 (3)
RVI_IOMMU_MSIPTP_Off (0), RVI_IOMMU_MSIPTP_Flat (1)

// TTYP
RVI_IOMMU_PCIE_ATS_TRANSLATION_REQUEST (8)

// DC格式大小
RVI_IOMMU_BASE_FORMAT_DC_SIZE (32), RVI_IOMMU_EXT_FORMAT_DC_SIZE (64)

// 页面大小
RVI_IOMMU_PAGESIZE (4096)
```

**注意**：设计文档Section 12伪代码中的常量名（如`Off`、`DDT_Bare`、`PAGESIZE`等）在实际代码中需加 `RVI_IOMMU_` 前缀。

### 13.7 iommu_t结构中的top指针

`iommu_t` 结构（`iommu_struct.hh`）包含 `class iommu_top *top` 成员。此指针用于功能模型代码回调SystemC层（如 `read_memory()` 通过 `iommu->top` 发起DDR事务）。

性能模型中此指针继续有效：
- `before_end_of_elaboration()` 中设置 `iommu_inst.top = this`
- report_fault() 等控制路径函数通过此指针访问DDR（v2中改走ctrl_path_req_ddr_fifo，见13.2）
- 性能路径的Walker线程通过req_ddr_fifo → ddr_arbiter间接访问DDR

### 13.8 PayloadExtention精确字段列表（v2更新）

当前 `PayloadExtention`（`param_trans_def.hh` 130行）关键字段：
- `requester_id:16` — 对应 device_id
- `at:2` — addr_type_t (0=UNTRANSLATED, 1=TRANSLATED, 2=ATS)
- `pid_valid:1`, `process_id:28` — PASID
- `exec_req:1`, `priv_req:1`, `no_write:1` — 访问属性
- `msg_type:8` — PRI消息类型标识
- `segment_num:16`, `ds_valid:1` — 段号

**v2变更**：
- ~~`axi_id (uint16_t)`~~ — **移除**：v2使用单一AXI ID，不需要在PayloadExtention中携带axi_id
- `iommu_internal (uint8_t)` — **保留但含义微调**：标识IOMMU内部DDR访问(1) vs DMA转发(0)。仅在DDR Arbiter构造TLM payload时由arbiter自行设置，不通过PayloadExtention传递。

**净变更**：PayloadExtention无需新增字段。DDR arbiter使用独立的TLM payload（非原始入站payload），因此DDR关联信息完全由ddr_pending_queue管理。

同时更新 `clone()` 和 `operator=()` 方法（如有新增字段则拷贝）。

---

## 十四、实现执行计划（按文件排序）

基于Section 9的Phase划分，细化为可直接执行的步骤：

### Step 1: 创建 `iommu/iommu_task.hh`（新文件，~300行）

定义：
- `enum task_state_t` — 所有状态枚举
- `struct walk_context_t` — v2增强版Walker上下文（含walk_phase, gs_level, gs_vpn, gs_base_addr, pending_vs_pte_addr, ad_pte等新字段）
- `struct iommu_task_t` — 完整任务结构（Section 3.1所有字段，使用13.5中的精确类型）
- `struct collector_entry_t` — Collector pending_tasks条目
- `struct ddr_req_entry_t` — DDR请求条目（Section 3.2）
- `struct ddr_rsp_entry_t` — DDR响应条目（Section 3.2）
- `struct ddr_pending_entry_t` — DDR待决队列条目（Section 3.2）
- `class walker_cache_t` — Walker Cache三级缓存（lookup/update/invalidate + SRRIP替换）

### Step 2: 创建 `iommu/iommu_perf_model.hh`（新文件，~180行）

提取纯计算工具函数（不做DDR访问）：
- `classify_ttype()` — 从at/is_read/is_write/exec_req推导TTYP
- `extract_access_attributes()` — 从PayloadExtention提取访问属性
- `extract_DDI()` — 根据DDT模式计算DDI[0/1/2]
- `check_is_msi_address()` — MSI地址模式匹配
- `extract_vpn()` — 从IOVA提取VPN和确定LEVELS/PTESIZE
- `extract_gs_vpn()` — 从GPA提取G-stage VPN和确定GS_LEVELS
- `check_canonical()` — 地址canonical检查
- `get_bare_page_size()` / `get_gstage_bare_page_size()` — Bare模式页面大小
- `calculate_mgpaw()` / `extract()` — MSI翻译辅助函数

### Step 3: 修改 `iommu/param_trans_def.hh`

- PayloadExtention：v2无需新增axi_id字段（DDR关联由ddr_pending_queue管理）
- 若需要iommu_internal标记：在DDR Arbiter内部TLM payload中设置，不修改PayloadExtention

### Step 4: 修改 `ddr/test_ddr.hh`

- **移除**现有 b_transport 注册（v2仅保留nb_transport）
- **保留** `nb_transport_fw()` 回调注册
- 新增 `sc_fifo<tlm_generic_payload*> ddr_req_fifo`
- 新增 `SC_THREAD(ddr_process_thread)` — 模拟DDR延迟后通过 nb_transport_bw 返回

### Step 5: 修改 `rp/test_rp.hh` + `rp/test_rp_thread.cc`

- 添加 `nb_transport_bw()` 回调
- 添加 `sc_event response_event`
- 修改 `send_translation_request` 使用 nb_transport_fw (AT方式)

### Step 6: 修改 `iommu/iommu_top.hh`（57行 → ~370行）

新增声明：
- 20个 `sc_fifo<iommu_task_t*>` FIFO + 7个 `sc_fifo<ddr_req_entry_t>` / `sc_fifo<ddr_rsp_entry_t>` DDR FIFO（使用iommu_perf_params.hh深度）
- 21个 SC_THREAD 函数声明（含ddr_arbiter_thread）
- `sc_mutex` 声明（5个：ddr_queue/walker_cache/iommu_cache/collector/task_id）
- 3个 `sc_mutex` Walker专用（xdtw_walks/ptw_walks/msiptw_walks）
- `walker_cache_t` 实例
- `std::map<uint32_t, iommu_task_t*>` — xdtw/ptw/msiptw_active_walks
- `int outstanding_task_count` — xdtw/ptw/msiptw各一个
- `sc_event task_completed_event` — xdtw/ptw/msiptw各一个
- `std::queue<ddr_pending_entry_t> ddr_pending_queue` + `sc_event ddr_pending_freed_event`
- `std::map<uint32_t, collector_entry_t> pending_tasks`
- `uint32_t next_task_id`
- `nb_transport_fw/bw` 回调声明
- `init_gstage_walk()`、`send_response_to_initiator()`、`configure_and_route()`、`write_memory_via_arbiter()` 辅助方法声明

### Step 7: 修改 `iommu/iommu_top.cc`

- 构造函数：注册nb_transport回调，初始化FIFO/mutex/walker_cache，注册21个SC_THREAD
- 实现 `axi_slave_nb_transport_fw()` — 入站AT回调
- 实现 `ddr_nb_transport_bw()` — DDR响应AT回调（从ddr_pending_queue pop → 路由到rsp_ddr_fifo）
- 实现 `write_memory_via_arbiter()` — 控制路径DDR写封装
- 实现 `send_response_to_initiator()` — 通过nb_transport_bw返回响应
- 实现 `configure_and_route()` — 公共路由方法
- 保留 `before_end_of_elaboration()` 和 `ahb_slave_b_transport()` 不变
- 保留 `axi_slave_b_transport()` 但标记为deprecated/可选删除

### Step 8-16: 创建9个流水线实现文件

按Section 12的伪代码实现（每个文件是iommu_top的成员函数实现）：
- `iommu_perf_parser.cc` — parser_thread (Step 8)
- `iommu_perf_dc_pc_cache.cc` — 4个cache线程 (Step 9)
- `iommu_perf_collector.cc` — 2个collector线程 (Step 10)
- `iommu_perf_xdtw.cc` — xdtw_req_thread + xdtw_rsp_thread (Step 11)
- `iommu_perf_pt_cache.cc` — 3个pt_cache线程 (Step 12)
- `iommu_perf_ptw.cc` — ptw_req_thread + ptw_rsp_thread + init_gstage_walk (Step 13)
- `iommu_perf_msipt_cache.cc` — msipt_cache_query/result线程 (Step 14)
- `iommu_perf_msiptw.cc` — msiptw_req_thread + msiptw_rsp_thread (Step 15)
- `iommu_perf_forwarder_fault_cq.cc` — forwarder + fault线程 (Step 16)

**注意**：v2中ddr_arbiter_thread的实现在`iommu_top.cc`中（与ddr_nb_transport_bw回调紧密关联），不单独建文件。

### Step 17: 修改 `Makefile`

CXX_SOURCES 追加9个新.cc文件。

### Step 18: 编译验证

```bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1 && make clean && make DEBUG=1
```

修复编译错误直到成功。

### Step 19: 功能验证

运行现有测试用例，验证翻译结果正确（PA、fault code、response status与功能模型一致）。

---

## 十五、验证方案

### 15.1 编译验证
```bash
bash -c "cd /mnt/d/Qoder_proj/iommu_model_20260401_v1 && make clean && make DEBUG=1"
```

### 15.2 功能等价验证
- 运行 `./iommu_model`
- 对比3个测试设备（device 0x05/0x06/0x07）的翻译结果
- Bare模式（device 0x05）：PA = IOVA
- G-stage（device 0x06）：PA = 通过Sv48x4页表翻译
- S-stage（device 0x07）：PA = 通过Sv39页表翻译
- 验证故障路径（无效device_id、错误模式等）

### 15.3 时序验证
- 在task中插入时间戳（创建时、各阶段完成时）
- Cache命中路径总延迟 ≈ 1+2+1+3+2 = 9ns
- Cache未命中路径包含DDR延迟（每次DDR_ACCESS_DELAY_NS，由DDR Module配置）
- DDR请求到达arbiter的排队延迟取决于飞行请求数和DDR带宽

### 15.4 并发验证
- 修改RP测试线程注入多个并发请求（不同device_id）
- 验证多任务流水线重叠执行
- 验证outstanding_task_count正确限制各Walker模块的并发度
- 验证ddr_pending_queue的FIFO保序：响应路由到正确模块
- 验证无死锁（FIFO背压正确传递 + outstanding_task_count流控不产生循环等待）

### 15.5 DDR Arbiter验证
- 验证优先级仲裁：ctrl_path优先级最高，xDTW/PTW/MSIPTW round-robin公平
- 验证DDR_MAX_OUTSTANDING流控：飞行请求数不超过限制
- 验证ddr_pending_queue保序：push/pop顺序与DDR请求/响应顺序一致
- 验证TLM payload内存正确释放：无内存泄漏

### 15.6 内存泄漏检查
- 验证每个new出来的task最终被delete（Forwarder正常路径 + Fault故障路径）
- 统计task创建数和释放数一致
- 验证DDR Arbiter分配的TLM payload在nb_transport_bw回调中正确释放
- 验证ddr_req_entry中write_data的堆分配正确释放

### 15.7 Outstanding参数调优测试
- 修改XDTW/PTW/MSIPTW_MAX_OUTSTANDING_TASKS参数
- 观察DDR带宽利用率变化
- 修改DDR_MAX_OUTSTANDING参数
- 观察全局DDR排队延迟变化
- 目标：找到各参数的最优配置使DDR带宽利用率最大化同时避免过度排队