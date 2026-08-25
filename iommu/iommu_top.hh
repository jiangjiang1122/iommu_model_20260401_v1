#ifndef IOMMU_TOP_HH
#define IOMMU_TOP_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/peq_with_get.h"
#include <queue>
#include <vector>
#include <map>
#include "iommu_struct.hh"
#include "param_trans_def.hh"
#include "iommu_task.hh"
#include "iommu_perf_params.hh"
#include "iommu_perf_model.hh"

#include "json_config.h"
#include "cache_subsystem.h"
#include "dedup_buffer.h"  // NEW: Dedup Buffer for PT Cache deduplication
#include <iostream>
#include <cassert>
// TODO: 暂时禁用set，待修复崩溃问题
// #include <set>

using namespace std;
using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;
// using namespace iommu;  // Avoid namespace pollution, use iommu:: prefix explicitly

#define IOMMU_BASE_ADDR 0x80000000ULL

// ===================== Cache命中率统计全局变量 =====================
extern uint64_t g_dc_cache_hit_count;
extern uint64_t g_dc_cache_miss_count;
extern uint64_t g_pc_cache_hit_count;
extern uint64_t g_pc_cache_miss_count;
extern uint64_t g_pt_cache_hit_count;
extern uint64_t g_pt_cache_miss_count;
extern uint64_t g_msipt_cache_hit_count;
extern uint64_t g_msipt_cache_miss_count;

class iommu_top : public sc_module
{
public:
    // ===================== Sockets (unchanged names for main.cpp compatibility) =====================
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_stream_socket;      // MSI data to IMSIC
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_master_0_to_pcie_noc_to_cmn_rni_socket;    // DMA data / access RP
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_master_1_to_cmn_rnd_socket;     // DDR access for DDT/PDT/CQ/FQ/PQ/MRIF
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_master_2_ats_msg_to_pcie_noc_socket;    // ATS msg back to RP

    tlm_utils::simple_target_socket<iommu_top,BUS_WIDTH>  axi_slave_from_pcie_noc_0_socket;
    tlm_utils::simple_target_socket<iommu_top,BUS_WIDTH>  ahb_slave_from_pcie_noc_1_socket;

    // ===================== IOMMU Instance =====================
    iommu_t iommu_inst;

    // 从 JSON 配置文件加载（推荐，参数已与架构对齐）
    iommu::GlobalConfig cfg = iommu::load_config("iommu/cache_config/default_config.json");

    // 创建 CacheSubsystem
    iommu::CacheSubsystem cache_sub{"cache_sub", cfg};

    // 打印Cache命中率统计信息
    void print_cache_statistics();

    // ===================== Task ID Counter =====================
    uint32_t next_task_id;
    sc_mutex task_id_mtx;

    // ===================== Pipeline Data FIFOs (sc_fifo<iommu_task_t*>) =====================
    sc_fifo<iommu_task_t*> inbound_fifo;
    sc_fifo<iommu_task_t*> parser_to_collector_fifo;
    // sc_fifo<iommu_task_t*> parser_to_dc_cache_query_fifo;  // Replaced by cache_sub.dc_request_fifo
    // sc_fifo<iommu_task_t*> parser_to_pc_cache_query_fifo;  // Replaced by cache_sub.pc_request_fifo
    // sc_fifo<iommu_task_t*> dc_cache_to_collector_fifo;     // Replaced by cache_sub.dc_response_fifo
    // sc_fifo<iommu_task_t*> pc_cache_to_collector_fifo;     // Replaced by cache_sub.pc_response_fifo
    sc_fifo<iommu_task_t*> collector_to_xdtw_dc_fifo;
    sc_fifo<iommu_task_t*> collector_to_xdtw_pc_fifo;
    // sc_fifo<iommu_task_t*> collector_to_dc_cache_update_fifo;  // Replaced by cache_sub.dc_update_fifo
    // sc_fifo<iommu_task_t*> collector_to_pc_cache_update_fifo;  // Replaced by cache_sub.pc_update_fifo
    // sc_fifo<iommu_task_t*> collector_to_pt_cache_query_fifo;   // Replaced by cache_sub.pt_request_fifo
    sc_fifo<iommu_task_t*> collector_to_msipt_cache_query_fifo;
    sc_fifo<iommu_task_t*> collector_to_fault_fifo;
    sc_fifo<iommu_task_t*> xdtw_to_collector_fifo;
    sc_fifo<iommu_task_t*> pt_cache_to_ptw_fifo;
    sc_fifo<iommu_task_t*> ptw_to_pt_cache_fifo;
    sc_fifo<iommu_task_t*> pt_cache_to_fwd_fifo;
    sc_fifo<iommu_task_t*> msipt_cache_to_msiptw_fifo;
    sc_fifo<iommu_task_t*> msiptw_to_msipt_cache_fifo;
    sc_fifo<iommu_task_t*> msipt_cache_to_fwd_fifo;

    // ===================== DDR Request/Response FIFOs =====================
    sc_fifo<ddr_req_entry_t> xdtw_req_ddr_fifo;
    sc_fifo<ddr_rsp_entry_t> xdtw_rsp_ddr_fifo;
    sc_fifo<ddr_req_entry_t> ptw_req_ddr_fifo;
    sc_fifo<ddr_rsp_entry_t> ptw_rsp_ddr_fifo;
    sc_fifo<ddr_req_entry_t> msiptw_req_ddr_fifo;
    sc_fifo<ddr_rsp_entry_t> msiptw_rsp_ddr_fifo;
        // [MSI] MRIF pending word写请求通道(经Master2/DDR端口, 响应静默丢弃)
        sc_fifo<ddr_req_entry_t> msi_mrif_req_ddr_fifo;
    sc_fifo<ctrl_path_ddr_req_t> ctrl_path_req_ddr_fifo;

    // ===================== DDR Pending Queue =====================
    std::queue<ddr_pending_entry_t> ddr_pending_queue;
    sc_mutex ddr_queue_mtx;
    sc_event ddr_pending_freed_event;

    // ===================== AXI Master端口Outstanding计数器 =====================
    int axi_master_1_to_cmn_rnd_outstanding;  // axi_master_1_to_cmn_rnd_socket并发计数
    sc_event axi_master_1_slot_freed_event;   // slot释放事件
    int axi_master_0_to_pcie_noc_outstanding; // axi_master_0_to_pcie_noc_socket并发计数
    sc_event axi_master_0_slot_freed_event;   // slot释放事件

    // ===================== Control Path Response =====================
    uint8_t ctrl_path_rsp_buf[64];
    sc_event ctrl_path_rsp_event;

    // ===================== Collector Pending Tasks =====================
    std::map<uint32_t, collector_entry_t> pending_tasks;
    sc_mutex collector_mtx;

    // ===================== PT Cache Pending Tasks =====================
    std::map<uint32_t, iommu_task_t*> pt_cache_pending_tasks;  // Save original task for PT lookup
    sc_mutex pt_cache_mtx;

    // ===================== [前置] Walker Front Pending Tasks =====================
    // 前置查询待响应任务表: configure_and_route注册, 响应线程写回后移除;
    // PT HIT直接输出路径会先移除表项(防响应线程触碰已释放的task)
    std::map<uint32_t, iommu_task_t*> walker_front_pending;
    sc_mutex walker_front_mtx;
    sc_event walker_front_ready_event;   // 前置结果写回通知(PTW兜底等待用)

    // ===================== VA Dedup Table (PT Cache VA去重) =====================
    // ... 保留旧去重代码（暂时不删除，后续清理） ...
    struct va_dedup_key_t {
        uint32_t gscid;
        uint32_t pscid;
        uint64_t page_iova;  // iova & ~0xFFF (4KB页对齐)
        bool operator<(const va_dedup_key_t& other) const {
            if (gscid != other.gscid) return gscid < other.gscid;
            if (pscid != other.pscid) return pscid < other.pscid;
            return page_iova < other.page_iova;
        }
    };
    struct va_dedup_entry_t {
        bool valid;
        uint32_t first_task_id;                    // 首笔(dedup miss)task_id
        std::vector<iommu_task_t*> pending_tasks;  // 挂起的dedup hit任务
    };
    std::map<va_dedup_key_t, va_dedup_entry_t> va_dedup_table;
    sc_mutex va_dedup_mtx;
    uint64_t va_dedup_hit_count;   // 统计：去重命中次数
    uint64_t va_dedup_miss_count;  // 统计：去重未命中次数

    // ===================== NEW: Dedup Buffer for PT Cache =====================
    iommu::DedupBuffer pt_dedup_buffer;  // Buffer for deduplication (256 entries)
    sc_mutex pt_dedup_buffer_mtx;  // Mutex for Buffer concurrent access protection

    // [重构] dedup Buffer 刷新回调: 由 CacheSubsystem::execute_dedup_update 触发
    // 遍历 head_index 链, 算 PA, 转发挂起任务到 pt_cache_to_fwd_fifo
    void dedup_flush_chain_cb(uint16_t head_index, const iommu::CacheMessage& upd);

    // ===================== NEW: PTW Response FIFO (顺序处理) =====================
    // [重构] 使用FIFO替代prefetch_groups map,保证响应顺序处理
    sc_fifo<iommu_task_t*> ptw_response_fifo;  // PTW完成响应FIFO
    sc_event ptw_response_event;               // PTW响应事件
    
    // [保留] 预取组状态追踪 (兼容现有PTW逻辑,后续可移除)
    struct PrefetchGroupState {
        iommu_task_t* main_task = nullptr;    // 主任务指针
        uint32_t pending_tasks = 0;           // 待完成walk数
        uint32_t total_tasks = 0;             // 总任务数(1+D)
        bool     completed = false;           // 组完成标志
        bool     main_task_done = false;      // 主任务walk完成标志(pt_updates[0]已填充)
        bool     has_fault = false;           // 组中有任务fault
        // [大页] 大页组: 不写PT Cache, 仅按D+1个构造4KB iova刷新去重Cache;
        // slot_invalid标注D个无效结果槽(未真实预取, 仅用于占位清除)
        bool     is_hugepage = false;
        bool     slot_invalid[17] = {false};
        double   group_start_ns = 0.0;        // [STAT] 组起始时间(主任务进入PTW时刻, ns)
        
        // 收集所有walk结果
        spte_t   vs_ptes[17];
        gpte_t   g_ptes[17];
        uint64_t pas[17];
        uint64_t page_szs[17];
        uint64_t group_iovas[17];             // 组内所有IOVA
    };
    
    std::map<uint32_t, PrefetchGroupState> prefetch_groups;  // key=group_id
    sc_mutex prefetch_group_mtx;
    sc_event prefetch_group_completed_event;  // [保留] 用于触发Monitor

    // ===================== IOMMU/PTW IOPS Counters =====================
    uint64_t iommu_total_completed;  // IOMMU 出口完成翻译总数
    uint64_t ptw_total_completed;    // PTW 完成任务总数

    // ===================== Port Byte Counters =====================
    uint64_t slave_0_total_bytes;    // axi_slave_0  入口接收总字节数
    uint64_t master_0_total_bytes;   // axi_master_0 出口发送总字节数
    uint64_t master_1_total_bytes;   // axi_master_1 DDR 访问总字节数
    
    // ===================== Outstanding Peak Trackers =====================
    int peak_iommu_global_outstanding;     // IOMMU全局outstanding峰值
    int peak_ptw_outstanding;              // PTW outstanding峰值
    int peak_xdtw_dc_outstanding;          // xDTW DC walk outstanding峰值
    int peak_xdtw_pc_outstanding;          // xDTW PC walk outstanding峰值
    int peak_collector_dc_walk_outstanding; // Collector DC walk outstanding峰值
    int peak_collector_pc_walk_outstanding; // Collector PC walk outstanding峰值
    int peak_axi_master_1_outstanding;     // DDR端口outstanding峰值
    int peak_axi_master_0_outstanding;     // 出口端口outstanding峰值

    // ===================== PTW Per-Task Statistics =====================
    uint64_t ptw_total_ddr_reads;         // 所有PTW任务DDR读次数总和
    double   ptw_total_exec_ns;           // 所有PTW任务执行延时总和(ns)
    // [STAT] PTW DDR访问详细统计
    uint32_t ptw_max_ddr_reads;           // 单笔任务最大DDR访问次数
    uint32_t ptw_min_ddr_reads;           // 单笔任务最小DDR访问次数
    double   ptw_max_ddr_latency_ns;      // 单次DDR访问最大延时
    double   ptw_min_ddr_latency_ns;      // 单次DDR访问最小延时
    double   ptw_total_ddr_latency_ns;    // 单次DDR访问总延时（用于平均）
    uint64_t ptw_ddr_latency_count;       // DDR延时采样次数
    // [STAT] PTW任务延时统计
    double   ptw_max_task_latency_ns;     // 单笔任务最大延时
    double   ptw_min_task_latency_ns;     // 单笔任务最小延时
    std::map<uint32_t, double> ptw_task_start_ns; // task_id -> PTW入口时刻(ns)
    // [NEW] PTW任务注入/输出间隔统计
    double   ptw_last_inject_ns;          // 上一次任务注入时刻(ns)
    double   ptw_inject_interval_total_ns; // 注入间隔总和(ns)
    double   ptw_inject_interval_max_ns;  // 最大注入间隔(ns)
    double   ptw_inject_interval_min_ns;  // 最小注入间隔(ns)
    uint64_t ptw_inject_interval_count;   // 注入间隔采样次数
    double   ptw_last_output_ns;          // 上一次任务输出时刻(ns)
    double   ptw_output_interval_total_ns; // 输出间隔总和(ns)
    double   ptw_output_interval_max_ns;  // 最大输出间隔(ns)
    double   ptw_output_interval_min_ns;  // 最小输出间隔(ns)
    uint64_t ptw_output_interval_count;   // 输出间隔采样次数
    // [STAT] PTW DDR访问次数分布与任务分类统计
    std::map<uint32_t, uint32_t> ptw_ddr_reads_distribution;  // DDR reads -> count (all tasks)
    std::map<uint32_t, uint32_t> ptw_main_ddr_reads_distribution;   // 主任务DDR reads分布
    std::map<uint32_t, uint32_t> ptw_prefetch_ddr_reads_distribution; // 预取任务DDR reads分布
    uint64_t ptw_main_task_count;          // 主任务数(非prefetch)
    uint64_t ptw_prefetch_task_count;      // 预取任务数
    // [STAT] 主任务/预取任务DDR reads汇总
    uint64_t ptw_main_total_ddr_reads;     // 主任务DDR读总数
    uint64_t ptw_prefetch_total_ddr_reads; // 预取任务DDR读总数
    // [大页][STAT] 大页任务统计
    uint64_t ptw_hugepage_main_count;      // 大页主任务数(page_sz>4KB)
    uint64_t ptw_hugepage_pf_skipped;      // 大页跳过的预取spawn数
    uint64_t ptw_hugepage_invalid_slots;   // 返回的无效结果槽总数(D个/组)
    uint64_t ptw_front_leaf_hits;          // 前置leaf命中短路完成数(0次DDR)
    std::vector<double> ptw_output_interval_values;  // 所有输出间隔值(ns), 用于波动曲线
    std::vector<double> ptw_task_latency_values;     // 所有任务执行延时(ns), 用于分布统计
    // [STAT] PTW任务组执行时间统计 (1主+D预取为一组)
    double   ptw_group_exec_total_ns;        // 所有组执行时间总和(ns)
    double   ptw_group_exec_max_ns;          // 单组最大执行时间(ns)
    double   ptw_group_exec_min_ns;          // 单组最小执行时间(ns)
    uint64_t ptw_group_exec_count;           // 完成的组数
    std::vector<double> ptw_group_exec_values; // 每组执行时间(ns), 用于分布统计

    // ===================== End-to-End Latency Statistics =====================
    double   iommu_total_e2e_latency_ns;  // 所有IO端到端延时总和(ns)
    double   iommu_e2e_max_ns;            // 单笔IO最大端到端延时(ns)
    double   iommu_e2e_min_ns;            // 单笔IO最小端到端延时(ns)
    std::vector<double> iommu_e2e_values; // 每笔IO端到端延时(ns), 用于分布统计

    // ===================== IOMMU Input/Output Port Interval Statistics =====================
    // 输入端口(inbound_fifo)任务间隔
    double   iommu_in_last_ns;              // 上一次输入任务时刻(ns)
    double   iommu_in_interval_total_ns;    // 输入间隔总和(ns)
    double   iommu_in_interval_max_ns;      // 最大输入间隔(ns)
    double   iommu_in_interval_min_ns;      // 最小输入间隔(ns)
    uint64_t iommu_in_interval_count;       // 输入间隔采样次数
    std::vector<double> iommu_in_interval_values;  // 所有输入间隔值(ns)
    // 输出端口(master_0)任务间隔
    double   iommu_out_last_ns;             // 上一次输出任务时刻(ns)
    double   iommu_out_interval_total_ns;   // 输出间隔总和(ns)
    double   iommu_out_interval_max_ns;     // 最大输出间隔(ns)
    double   iommu_out_interval_min_ns;     // 最小输出间隔(ns)
    uint64_t iommu_out_interval_count;      // 输出间隔采样次数
    std::vector<double> iommu_out_interval_values; // 所有输出间隔值(ns)

    // ===================== [MONITOR] 阻塞点监控统计 =====================
    // 监控点1: flush_dedup_buffer_by_iova 阻塞统计
    uint64_t monitor_flush_total_count;           // flush调用总次数
    double   monitor_flush_total_ns;              // flush总耗时(ns)
    double   monitor_flush_max_ns;                // 单次flush最大耗时(ns)
    uint64_t monitor_flush_fifo_block_count;      // FIFO阻塞次数(pt_cache_to_fwd_fifo满)
    double   monitor_flush_fifo_block_total_ns;   // FIFO阻塞总耗时(ns)
    double   monitor_flush_fifo_block_max_ns;     // 单次FIFO阻塞最大耗时(ns)
    // [NEW] 监控点1增强: 扫描时间与FIFO阻塞时间分离
    double   monitor_flush_scan_total_ns;         // 256-entry扫描总耗时(ns)(不含FIFO阻塞)
    double   monitor_flush_scan_max_ns;           // 单次扫描最大耗时(ns)
    uint64_t monitor_flush_matched_entries;       // 匹配的buffer entry总数
    uint64_t monitor_flush_scanned_entries;       // 扫描的buffer entry总数(=调用次数×256)
    // 监控点2: Monitor线程PT Cache UPDATE阻塞统计
    uint64_t monitor_pt_update_total_count;       // PT UPDATE写入总次数
    double   monitor_pt_update_total_ns;          // PT UPDATE写入总耗时(ns)
    double   monitor_pt_update_max_ns;            // 单次PT UPDATE最大耗时(ns)
    uint64_t monitor_pt_update_fifo_block_count;  // PT UPDATE FIFO阻塞次数
    double   monitor_pt_update_fifo_block_ns;     // PT UPDATE FIFO阻塞总耗时(ns)
    // [NEW] 监控点2增强: PT UPDATE在pt_update_fifo中的排队等待时间
    double   monitor_pt_update_queue_wait_total_ns; // UPDATE消息在FIFO中排队等待总时间(ns)
    double   monitor_pt_update_queue_wait_max_ns;   // UPDATE消息最大排队等待时间(ns)
    uint64_t monitor_pt_update_queue_wait_count;    // 有排队等待的UPDATE次数
    // 监控点3: Monitor线程整体处理统计
    uint64_t monitor_group_total_count;           // 处理的group总数
    double   monitor_group_total_process_ns;      // group总处理耗时(ns)(含flush+PT UPDATE)
    double   monitor_group_max_process_ns;        // 单个group最大处理耗时(ns)
    // 监控点4: 重排序乱序暂存统计
    uint64_t reorder_out_of_order_count;          // 乱序到达的任务数(所有类型)
    uint64_t reorder_total_marked_count;          // 标记ready的总任务数(用于计算乱序比例)
    uint64_t reorder_write_blocked_count;         // 写请求因队头未ready被阻塞的次数
    double   reorder_write_blocked_total_ns;      // 写请求阻塞总耗时(ns)
    double   reorder_write_blocked_max_ns;        // 写请求单次最大阻塞耗时(ns)
    uint64_t reorder_wait_for_head_count;         // reorder_output_thread等待队头的次数
    double   reorder_wait_for_head_total_ns;      // reorder_output_thread等待队头总耗时(ns)
    // [NEW] 监控点4增强: 读请求reorder等待延时(ready->实际发送)
    double   reorder_read_total_wait_ns;          // 读请求等待发送总耗时(ns)
    double   reorder_read_max_wait_ns;            // 读请求单次最大等待耗时(ns)
    uint64_t reorder_read_waited_count;           // 读请求等待>0的次数
    uint64_t reorder_read_total_sent_count;       // 读请求总发送次数

    // ===================== Steady-State IOPS Measurement =====================
    // 跳过前10%和后10%，只统计中间80%稳定段
    double   steady_start_ns;             // 稳态开始时刻(ns)
    double   steady_end_ns;               // 稳态结束时刻(ns)
    uint64_t steady_start_count;          // 稳态开始时的完成数
    uint64_t steady_end_count;            // 稳态结束时的完成数
    uint64_t ptw_steady_start_completed;  // 稳态开始时PTW完成数
    uint64_t ptw_steady_end_completed;    // 稳态结束时PTW完成数

    // ===================== Walker Active Walks =====================
    std::map<uint32_t, iommu_task_t*> xdtw_active_walks;
    sc_mutex xdtw_walks_mtx;
    int xdtw_dc_outstanding_task_count;
    int xdtw_pc_outstanding_task_count;
    sc_event xdtw_dc_task_completed_event;
    sc_event xdtw_pc_task_completed_event;

    // Collector walk outstanding counters
    int collector_dc_walk_outstanding;
    int collector_pc_walk_outstanding;
    sc_event collector_dc_walk_completed_event;
    sc_event collector_pc_walk_completed_event;

    std::map<uint32_t, iommu_task_t*> ptw_active_walks;
    sc_mutex ptw_walks_mtx;
    int ptw_outstanding_task_count;
    sc_event ptw_task_completed_event;

    // PTW PEQ for pipeline delay (non-blocking, parallel timing)
    tlm_utils::peq_with_get<iommu_task_t> ptw_req_peq;
    tlm_utils::peq_with_get<ddr_rsp_entry_t> ptw_rsp_peq;

    std::map<uint32_t, iommu_task_t*> msiptw_active_walks;
    sc_mutex msiptw_walks_mtx;
    int msiptw_outstanding_task_count;
    sc_event msiptw_task_completed_event;

    // ===================== Cache Mutex =====================
    sc_mutex iommu_cache_mtx;
    sc_mutex walker_cache_mtx;

    // ===================== Output Reorder Buffer =====================
    // 出口重排序：写请求按 task_id 单调保序输出；读请求到达即输出；读写互不影响。
    // 任意 task 在重排序输出之后才释放 IOMMU 全局 outstanding。
    struct reorder_entry_t {
        iommu_task_t* task;
        bool ready;       // 翻译完成（含 fault），已可输出
        bool is_write;    // 1=写请求，需保序；0=读请求，可乱序
        double ready_ns;  // [MONITOR] 标记ready的时刻(ns)，用于计算乱序等待时间
    };
    std::map<uint32_t, reorder_entry_t> reorder_buf;   // key = task_id
    std::queue<uint32_t> reorder_write_order;          // 写请求到达顺序（task_id）
    sc_mutex reorder_mtx;
    sc_event reorder_ready_event;

    // 全局 outstanding：parser 入侧申请，reorder 出口释放
    int iommu_global_outstanding;
    sc_event iommu_global_outstanding_freed_event;

    // 重排序辅助方法
    void reorder_register_task(iommu_task_t* task);    // 入口注册 + outstanding++
    void reorder_mark_ready(iommu_task_t* task);        // 出口（forwarder/fault）标记就绪
    void reorder_output_thread();                       // 重排序输出线程

    // ===================== Legacy CQ Event =====================
    sc_event cq_process_evt;
    std::queue<uint8_t> Ini_Process_queue;

    // ===================== 21 SC_THREAD Declarations =====================
    // Parser (1 thread)
    void parser_thread();

    // DC Cache (2 threads)
    void dc_cache_query_thread();
    void dc_cache_update_thread();

    // PC Cache (2 threads)
    void pc_cache_query_thread();
    void pc_cache_update_thread();

    // Collector (3 threads)
    void collector_cache_lookup_result_thread();
    void collector_pt_response_thread();  // New: handle PT cache responses
    void collector_xdtw_response_thread();

    // [前置] Walker Cache前置查询响应线程: 读walker_front_response_fifo,
    // 将结果写入task->walk_ctx.walker_front_*字段并通知PTW
    void walker_front_response_thread();

    // xDTW (2 threads)
    void xdtw_req_thread();
    void xdtw_rsp_thread();

    // PT Cache (3 threads)
    void pt_cache_query_thread();
    void pt_cache_result_thread();
    void pt_cache_ptw_rsp_thread();

    // PTW (4 threads: dispatch + process for req/rsp)
    void ptw_req_thread();            // dispatch: flow control + PEQ notify
    void ptw_req_process_thread();    // process: PEQ get + walk init + DDR send
    void ptw_rsp_thread();            // dispatch: FIFO read + PEQ notify
    void ptw_rsp_process_thread();    // process: PEQ get + PTE parse + state machine
    
    // NEW: 预取组监控线程
    void prefetch_group_monitor_thread();  // monitor prefetch group completion

    // MSIPT Cache (2 threads)
    void msipt_cache_query_thread();
    void msipt_cache_result_thread();

    // MSIPTW (2 threads)
    void msiptw_req_thread();
    void msiptw_rsp_thread();

    // [MSI] MSI处理辅助函数 (实现于 iommu_perf_msipt_cache.cc)
    // msi_id_check: 按功能模型 step3-5 识别地址是否命中虚拟 interrupt file页,
    //               命中时填充 task->gpa/is_msi/walk_ctx.msi_index
    bool msi_id_check(iommu_task_t* task, uint64_t gpa);
    // route_to_msipt: 将命中MSI的任务改道到独立的MSIPT路径
    void route_to_msipt(iommu_task_t* task);
    // msipte_decode: 解析16B MSI PTE并填充翻译结果(Flat/MRIF)或fault,
    //                返回true表示fault(task->cause已设置)
    bool msipte_decode(iommu_task_t* task, const msipte_t& msipte);

    // [MSI] MSI专项统计
    uint64_t msipt_flat_count;      // Flat模式MSI完成数
    uint64_t msipt_mrif_count;      // MRIF模式MSI完成数
    uint64_t msi_atomic_or_count;   // MRIF pending bit 原子OR写次数
    uint64_t msi_notice_count;      // notice MSI发出次数

    // Forwarder (2 threads - split for PT and MSIPT paths)
    void pt_forwarder_thread();      // PT Cache -> axi_master_0_to_pcie_noc_socket
    void msipt_forwarder_thread();   // MSIPT Cache -> axi_stream or axi_master_1

    // Fault/CQ (1 thread)
    void fault_cq_proc_thread();

    // DDR Arbiter (1 thread)
    void ddr_arbiter_thread();

    // ===================== AT Transport Callbacks =====================
    // Inbound AT callback (RP -> IOMMU)
    tlm::tlm_sync_enum axi_slave_nb_transport_fw(
        tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay);

    // DDR response AT callback (DDR -> IOMMU)
    tlm::tlm_sync_enum ddr_nb_transport_bw(
        tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay);

    // ===================== Helper Methods =====================
    void send_response_to_initiator(iommu_task_t* task);
    void configure_and_route(iommu_task_t* task);
    void init_gstage_walk(iommu_task_t* task, uint64_t gpa);
    void va_dedup_recover(iommu_task_t* completed_task, bool is_fault);

    // Legacy b_transport (kept for AHB register access)
    void axi_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay);
    void ahb_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay);

    void before_end_of_elaboration();

    SC_HAS_PROCESS(iommu_top);
    iommu_top(sc_core::sc_module_name name) : sc_module(name),
        // Initialize FIFOs with SPEC-defined depths
        inbound_fifo("inbound_fifo", FIFO_DEPTH_INBOUND),
        parser_to_collector_fifo("parser_to_collector_fifo", FIFO_DEPTH_PARSER_TO_COLLECTOR),
        // parser_to_dc_cache_query_fifo("parser_to_dc_cache_query_fifo", FIFO_DEPTH_PARSER_TO_DC_CACHE_QUERY),  // Replaced by cache_sub
        // parser_to_pc_cache_query_fifo("parser_to_pc_cache_query_fifo", FIFO_DEPTH_PARSER_TO_PC_CACHE_QUERY),  // Replaced by cache_sub
        // dc_cache_to_collector_fifo("dc_cache_to_collector_fifo", FIFO_DEPTH_DC_CACHE_TO_COLLECTOR),           // Replaced by cache_sub
        // pc_cache_to_collector_fifo("pc_cache_to_collector_fifo", FIFO_DEPTH_PC_CACHE_TO_COLLECTOR),           // Replaced by cache_sub
        collector_to_xdtw_dc_fifo("collector_to_xdtw_dc_fifo", FIFO_DEPTH_COLLECTOR_TO_XDTW),
        collector_to_xdtw_pc_fifo("collector_to_xdtw_pc_fifo", FIFO_DEPTH_COLLECTOR_TO_XDTW),
        // collector_to_dc_cache_update_fifo("collector_to_dc_cache_update_fifo", FIFO_DEPTH_COLLECTOR_TO_DC_CACHE_UPDATE),  // Replaced by cache_sub
        // collector_to_pc_cache_update_fifo("collector_to_pc_cache_update_fifo", FIFO_DEPTH_COLLECTOR_TO_PC_CACHE_UPDATE),  // Replaced by cache_sub
        // collector_to_pt_cache_query_fifo("collector_to_pt_cache_query_fifo", FIFO_DEPTH_COLLECTOR_TO_PT_CACHE_QUERY),     // Replaced by cache_sub
        collector_to_msipt_cache_query_fifo("collector_to_msipt_cache_query_fifo", FIFO_DEPTH_COLLECTOR_TO_MSIPT_CACHE_QUERY),
        collector_to_fault_fifo("collector_to_fault_fifo", FIFO_DEPTH_COLLECTOR_TO_FAULT),
        xdtw_to_collector_fifo("xdtw_to_collector_fifo", FIFO_DEPTH_XDTW_TO_COLLECTOR),
        pt_cache_to_ptw_fifo("pt_cache_to_ptw_fifo", FIFO_DEPTH_PT_CACHE_TO_PTW),
        ptw_to_pt_cache_fifo("ptw_to_pt_cache_fifo", FIFO_DEPTH_PTW_TO_PT_CACHE),
        pt_cache_to_fwd_fifo("pt_cache_to_fwd_fifo", FIFO_DEPTH_PT_CACHE_TO_FWD),
        msipt_cache_to_msiptw_fifo("msipt_cache_to_msiptw_fifo", FIFO_DEPTH_MSIPT_CACHE_TO_MSIPTW),
        msiptw_to_msipt_cache_fifo("msiptw_to_msipt_cache_fifo", FIFO_DEPTH_MSIPTW_TO_MSIPT_CACHE),
        msipt_cache_to_fwd_fifo("msipt_cache_to_fwd_fifo", FIFO_DEPTH_MSIPT_CACHE_TO_FWD),
        // DDR FIFOs
        xdtw_req_ddr_fifo("xdtw_req_ddr_fifo", FIFO_DEPTH_XDTW_REQ_DDR),
        xdtw_rsp_ddr_fifo("xdtw_rsp_ddr_fifo", FIFO_DEPTH_XDTW_RSP_DDR),
        ptw_req_ddr_fifo("ptw_req_ddr_fifo", FIFO_DEPTH_PTW_REQ_DDR),
        ptw_rsp_ddr_fifo("ptw_rsp_ddr_fifo", FIFO_DEPTH_PTW_RSP_DDR),
        msiptw_req_ddr_fifo("msiptw_req_ddr_fifo", FIFO_DEPTH_MSIPTW_REQ_DDR),
        msiptw_rsp_ddr_fifo("msiptw_rsp_ddr_fifo", FIFO_DEPTH_MSIPTW_RSP_DDR),
        msi_mrif_req_ddr_fifo("msi_mrif_req_ddr_fifo", FIFO_DEPTH_MSIPTW_REQ_DDR),
        ctrl_path_req_ddr_fifo("ctrl_path_req_ddr_fifo", FIFO_DEPTH_CTRL_PATH_REQ_DDR),
        // State initialization
        next_task_id(1),
        xdtw_dc_outstanding_task_count(0),
        xdtw_pc_outstanding_task_count(0),
        collector_dc_walk_outstanding(0),
        collector_pc_walk_outstanding(0),
        ptw_outstanding_task_count(0),
        ptw_req_peq("ptw_req_peq"),
        ptw_rsp_peq("ptw_rsp_peq"),
        msiptw_outstanding_task_count(0),
        axi_master_1_to_cmn_rnd_outstanding(0),
        axi_master_0_to_pcie_noc_outstanding(0),
        iommu_global_outstanding(0),
        va_dedup_hit_count(0),
        va_dedup_miss_count(0),
        iommu_total_completed(0),
        ptw_total_completed(0),
        slave_0_total_bytes(0),
        master_0_total_bytes(0),
        master_1_total_bytes(0),
        peak_iommu_global_outstanding(0),
        peak_ptw_outstanding(0),
        peak_xdtw_dc_outstanding(0),
        peak_xdtw_pc_outstanding(0),
        peak_collector_dc_walk_outstanding(0),
        peak_collector_pc_walk_outstanding(0),
        peak_axi_master_1_outstanding(0),
        peak_axi_master_0_outstanding(0),
        ptw_total_ddr_reads(0),
        ptw_total_exec_ns(0.0),
        ptw_max_ddr_reads(0),
        ptw_min_ddr_reads(999999),
        ptw_max_ddr_latency_ns(0.0),
        ptw_min_ddr_latency_ns(999999999.0),
        ptw_total_ddr_latency_ns(0.0),
        ptw_ddr_latency_count(0),
        ptw_max_task_latency_ns(0.0),
        ptw_min_task_latency_ns(999999999.0),
        ptw_last_inject_ns(0.0),
        ptw_inject_interval_total_ns(0.0),
        ptw_inject_interval_max_ns(0.0),
        ptw_inject_interval_min_ns(999999999.0),
        ptw_inject_interval_count(0),
        ptw_last_output_ns(0.0),
        ptw_output_interval_total_ns(0.0),
        ptw_output_interval_max_ns(0.0),
        ptw_output_interval_min_ns(999999999.0),
        ptw_output_interval_count(0),
        ptw_main_task_count(0),
        ptw_prefetch_task_count(0),
        ptw_main_total_ddr_reads(0),
        ptw_prefetch_total_ddr_reads(0),
        ptw_hugepage_main_count(0),
        ptw_hugepage_pf_skipped(0),
        ptw_hugepage_invalid_slots(0),
        ptw_front_leaf_hits(0),
        ptw_group_exec_total_ns(0.0),
        ptw_group_exec_max_ns(0.0),
        ptw_group_exec_min_ns(999999999.0),
        ptw_group_exec_count(0),
        iommu_total_e2e_latency_ns(0.0),
        iommu_e2e_max_ns(0.0),
        iommu_e2e_min_ns(999999999.0),
        iommu_in_last_ns(0.0),
        iommu_in_interval_total_ns(0.0),
        iommu_in_interval_max_ns(0.0),
        iommu_in_interval_min_ns(999999999.0),
        iommu_in_interval_count(0),
        iommu_out_last_ns(0.0),
        iommu_out_interval_total_ns(0.0),
        iommu_out_interval_max_ns(0.0),
        iommu_out_interval_min_ns(999999999.0),
        iommu_out_interval_count(0),
        monitor_flush_total_count(0),
        monitor_flush_total_ns(0.0),
        monitor_flush_max_ns(0.0),
        monitor_flush_fifo_block_count(0),
        monitor_flush_fifo_block_total_ns(0.0),
        monitor_flush_fifo_block_max_ns(0.0),
        monitor_flush_scan_total_ns(0.0),
        monitor_flush_scan_max_ns(0.0),
        monitor_flush_matched_entries(0),
        monitor_flush_scanned_entries(0),
        monitor_pt_update_total_count(0),
        monitor_pt_update_total_ns(0.0),
        monitor_pt_update_max_ns(0.0),
        monitor_pt_update_fifo_block_count(0),
        monitor_pt_update_fifo_block_ns(0.0),
        monitor_pt_update_queue_wait_total_ns(0.0),
        monitor_pt_update_queue_wait_max_ns(0.0),
        monitor_pt_update_queue_wait_count(0),
        monitor_group_total_count(0),
        monitor_group_total_process_ns(0.0),
        monitor_group_max_process_ns(0.0),
        reorder_out_of_order_count(0),
        reorder_total_marked_count(0),
        reorder_write_blocked_count(0),
        reorder_write_blocked_total_ns(0.0),
        reorder_write_blocked_max_ns(0.0),
        reorder_wait_for_head_count(0),
        reorder_wait_for_head_total_ns(0.0),
        reorder_read_total_wait_ns(0.0),
        reorder_read_max_wait_ns(0.0),
        reorder_read_waited_count(0),
        reorder_read_total_sent_count(0),
        steady_start_ns(0.0),
        steady_end_ns(0.0),
        steady_start_count(0),
        steady_end_count(0),
        ptw_steady_start_completed(0),
        ptw_steady_end_completed(0),
        msipt_flat_count(0),
        msipt_mrif_count(0),
        msi_atomic_or_count(0),
        msi_notice_count(0)
    {
        iommu_inst.top = this;

        // Register AT nb_transport callbacks for inbound and DDR
        axi_slave_from_pcie_noc_0_socket.register_nb_transport_fw(
            this, &iommu_top::axi_slave_nb_transport_fw);
        axi_master_1_to_cmn_rnd_socket.register_nb_transport_bw(
            this, &iommu_top::ddr_nb_transport_bw);

        // Keep AHB register access as b_transport (non-performance path)
        ahb_slave_from_pcie_noc_1_socket.register_b_transport(
            this, &iommu_top::ahb_slave_b_transport);

        // Register all SC_THREADs
        SC_THREAD(parser_thread);
        // SC_THREAD(dc_cache_query_thread);   // Replaced by CacheSubsystem
        // SC_THREAD(dc_cache_update_thread);  // Replaced by CacheSubsystem
        // SC_THREAD(pc_cache_query_thread);   // Replaced by CacheSubsystem
        // SC_THREAD(pc_cache_update_thread);  // Replaced by CacheSubsystem
        SC_THREAD(collector_cache_lookup_result_thread);
        SC_THREAD(collector_pt_response_thread);  // New: handle PT cache hit/miss responses
        SC_THREAD(collector_xdtw_response_thread);
        SC_THREAD(walker_front_response_thread);   // [前置] Walker前置查询响应处理
        SC_THREAD(xdtw_req_thread);
        SC_THREAD(xdtw_rsp_thread);
        // SC_THREAD(pt_cache_query_thread);    // Replaced by CacheSubsystem
        // SC_THREAD(pt_cache_result_thread);   // Replaced by CacheSubsystem
        // SC_THREAD(pt_cache_ptw_rsp_thread);  // Replaced by CacheSubsystem
        SC_THREAD(ptw_req_thread);
        SC_THREAD(ptw_req_process_thread);
        SC_THREAD(ptw_rsp_thread);
        SC_THREAD(ptw_rsp_process_thread);
        SC_THREAD(prefetch_group_monitor_thread);  // NEW: 预取组监控
        SC_THREAD(msipt_cache_query_thread);
        SC_THREAD(msipt_cache_result_thread);
        SC_THREAD(msiptw_req_thread);
        SC_THREAD(msiptw_rsp_thread);
        SC_THREAD(pt_forwarder_thread);
        SC_THREAD(msipt_forwarder_thread);
        SC_THREAD(fault_cq_proc_thread);
        SC_THREAD(ddr_arbiter_thread);
        SC_THREAD(reorder_output_thread);   // 出口重排序线程

        // [重构] 绑定去重模块回调: 转发 FIFO + Buffer 刷新回调
        // dedup_scheduler_thread 处理 dedup_update 时经此回调刷新 Buffer 并转发任务
        cache_sub.set_forward_fifo(&pt_cache_to_fwd_fifo);
        cache_sub.set_dedup_flush_callback(
            [this](uint16_t head_index, const iommu::CacheMessage& upd) {
                this->dedup_flush_chain_cb(head_index, upd);
            });

        memset(ctrl_path_rsp_buf, 0, sizeof(ctrl_path_rsp_buf));
    }

    ~iommu_top(){
    }
};

#endif
