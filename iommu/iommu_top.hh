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
#include "iommu_multidev_trace.hh"

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

    // [读写端口分离] 入站两个独立 slave 端口:
    //   ..._0_socket  = axi slave R port(只收读请求, 注册 axi_slave_r_nb_transport_fw)
    //   ..._0w_socket = axi slave W port(只收写请求, 注册 axi_slave_w_nb_transport_fw)
    //   两端口各自独立做本端口节拍计时(512bit@1000MHz: 512B=8ns, 128B=2ns, ctrl=1ns)
    //   与读/写 320 并发准入门控, 互不影响。
    tlm_utils::simple_target_socket<iommu_top,BUS_WIDTH>  axi_slave_from_pcie_noc_0_socket;
#if TEST_CFG_SPLIT_RW_PORTS
    tlm_utils::simple_target_socket<iommu_top,BUS_WIDTH>  axi_slave_from_pcie_noc_0w_socket;
#endif
    tlm_utils::simple_target_socket<iommu_top,BUS_WIDTH>  ahb_slave_from_pcie_noc_1_socket;

    // ===================== IOMMU Instance =====================
    iommu_t iommu_inst;

    // 从 JSON 配置文件加载（推荐，参数已与架构对齐）
    iommu::GlobalConfig cfg = iommu::load_config("iommu/cache_config/default_config.json");

#if TEST_CFG_MULTI_DEVICE_SCENE
    iommu_md::Trace md_trace;
    bool md_quiescent() const;
    void md_update_write_hol();
    // [STAT] 去重Buffer持有延时细分: W1=alloc->组完成, W2=组完成->聚合刷新执行, W3=聚合刷新->entry释放
    //   三档按 entry 来源拆分: main=主任务链头(MISS新建), pf=主任务链头(由预取占位行升级),
    //   susp=被去重挂起的任务。PTW 预取任务不申请 Buffer entry。
    uint64_t md_hold_main_cnt = 0, md_hold_susp_cnt = 0, md_hold_pf_cnt = 0;
    double md_hold_main_sum[3] = {0, 0, 0}, md_hold_susp_sum[3] = {0, 0, 0};
    double md_hold_main_max[3] = {0, 0, 0}, md_hold_susp_max[3] = {0, 0, 0};
    double md_hold_pf_sum[3] = {0, 0, 0}, md_hold_pf_max[3] = {0, 0, 0};

    // ============ [STAT] 端口带宽的"稳态窗口 / 注入期"口径 ============
    //   日志原有 "Port Average Bandwidth" 用【全仿真时长】作分母, 包含了页表构建/warmup
    //   (本场景 ~14.2us) 与注入结束后的 drain 段, 因此明显低于稳态值(属口径问题)。
    //   下组快照用于算出两个无稀释的口径:
    //     稳态窗口 = [steady_start_ns, steady_end_ns](完成数 10%~90%)
    //     注入期   = 本端口[首次准入, 末次准入]
    uint64_t bw_snap_win_r_bytes  = 0;   // 注入窗口打开时读口字节快照
    uint64_t bw_snap_win_w_bytes = 0;   // 注入窗口打开时写口字节快照
    uint64_t bw_snap_win_m0_bytes = 0;  // 注入窗口打开时出口字节快照
    uint64_t steady_start_slave_r_bytes = 0, steady_end_slave_r_bytes = 0;
    uint64_t steady_start_slave_w_bytes = 0, steady_end_slave_w_bytes = 0;
    uint64_t steady_start_master0_bytes = 0, steady_end_master0_bytes = 0;
#endif

    // 创建 CacheSubsystem
    iommu::CacheSubsystem cache_sub{"cache_sub", cfg};

    // 打印Cache命中率统计信息
    void print_cache_statistics();

    // ===================== [STAT] IOMMU 内部并发 4KB 页统计 =====================
    // 需求: 在接收请求端口入口处统计 IOMMU 模块平均并发的 4KB 页数目(按 device 分开统计)。
    //   只统计 512Byte 数据翻译任务(is_ctrl=0 && length=512), SQ/CQ/MSI 等控制包不记录。
    //   入口: 按 (device_id, iova 的 4KB 页值) 查表 -> 无匹配则插入并置 valid, count=1;
    //         有匹配则 count++。出口: count--, 减到 0 时清除该表项(valid=0, value=0)。
    //   并发数 = 当前 valid 表项数; 平均值用时间加权(Σ count×Δt / 窗口)求得。
    static constexpr unsigned KB4_DATA_LEN = 512;   // 512Byte 数据任务长度门控120120

    struct kb4_page_info_t {                 // 对应需求中的 struct 4kb_info
        bool     page_is_valid = false;      // 该 4KB 页是否正在被 IOMMU 处理
        uint64_t page_value    = 0;          // iova 对应的 4KB 页值
        uint32_t task_count    = 0;          // 属于该 4KB 页的在途 512B 任务数
    };
    struct kb4_dev_stat_t {                  // 单个 device 的 4KB 页跟踪表与统计
        std::map<uint64_t, kb4_page_info_t> pages;  // key = 4KB 页值(iova & ~0xFFF)
        uint32_t cur_conc  = 0;              // 当前并发(valid)4KB 页数
        uint32_t peak_conc = 0;              // 峰值并发 4KB 页数
        double   area_ns   = 0.0;            // Σ(cur_conc × Δt), 单位 page·ns
        double   last_change_ns = 0.0;       // 上次并发数变化时刻(ns)
        uint64_t in_tasks  = 0;              // 入口记录的 512B 数据任务数
        uint64_t out_tasks = 0;              // 出口记录的 512B 数据任务数
        uint64_t distinct_pages = 0;         // 曾经出现过的不同 4KB 页数
        uint64_t out_no_match   = 0;         // 出口查不到表项的异常次数
        uint64_t bad_state_cnt  = 0;         // 状态异常(命中表项但 valid=0)次数
    };
    std::map<uint32_t, kb4_dev_stat_t> kb4_dev_stats;  // key = device_id
    double   kb4_total_area_ns      = 0.0;   // 全设备 Σ(总并发 × Δt)
    uint32_t kb4_total_peak         = 0;     // 全设备总并发峰值
    double   kb4_win_first_ns       = -1.0;  // 采样窗口起点(ns), -1=未开始
    double   kb4_win_last_change_ns = 0.0;   // 全设备总并发上次变化时刻(ns)

    void track_4kb_page_in(iommu_task_t* task);    // 入口: 512B 数据任务到达
    void track_4kb_page_out(iommu_task_t* task);   // 出口: 512B 数据任务完成
    void reset_4kb_concurrency_window();           // 测量 epoch 前重置窗口
    void print_4kb_concurrency_report() const;     // 打印并发 4KB 页统计报告

    // ===================== Task ID Counter =====================
    uint32_t next_task_id;
    sc_mutex task_id_mtx;

    // ===================== Pipeline Data FIFOs (sc_fifo<iommu_task_t*>) =====================
    // [读写端口分离] 入口 FIFO 按方向拆为两条, parser 轮询(RR)弹出,
    //   避免单 FIFO 下"读满堵写"的队头阻塞(旧 inbound_fifo 已移除)。
    sc_fifo<iommu_task_t*> inbound_fifo_r;
    sc_fifo<iommu_task_t*> inbound_fifo_w;
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
        // [bypass预取] bypass组建组标记: 主任务未挂dedup buffer, 刷新时跳过dedup_update并直接转发主任务
        bool     is_bypass = false;
        bool     slot_invalid[17] = {false};
        double   group_start_ns = 0.0;        // [STAT] 组起始时间(主任务进入PTW时刻, ns)
        double   group_complete_ns = 0.0;     // [STAT] 组完成时间(pending=0且main_done时刻, ns)
        uint32_t group_ddr_reads = 0;         // [STAT] 组内所有任务DDR读次数累加
        
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

    // [聚合刷新PEQ] 待处理预取组数据: 持锁阶段收集, 经200ns PEQ延时后由
    //   agg_flush_process_thread 刷新去重buffer/去重cache/PT Cache。
    //   自包含字段(task_id/gscid/pscid/pa_bases)在收集时从main_task拷贝,
    //   避免延时后解引用main_task导致use-after-free。
    struct pending_group_update {
        uint32_t group_id = 0;
        iommu_task_t* main_task = nullptr;
        iommu::TransStage stage = iommu::TransStage::STAGE1_AND_2;
        bool sv48 = false;
        bool gstage_x4 = false;
        bool has_fault = false;
        bool is_hugepage = false;  // [大页] 大页组: 不写PT Cache, 仅刷去重Cache
        // [bypass预取] bypass组: 跳过dedup_update(不碰去重cache/buffer), 写PT Cache, 直接转发主任务
        bool is_bypass = false;
        std::vector<std::pair<uint64_t, iommu::PTData>> batch_updates;
        std::vector<uint64_t> group_iovas;
        uint32_t total_tasks = 0;
        // [200ns PEQ 自包含] flush所需字段, 收集时从main_task拷贝
        uint32_t task_id = 0;
        uint32_t gscid = 0;
        uint32_t pscid = 0;
        std::vector<uint64_t> pa_bases;  // 每iova的PA基址(pt_updates[i].pa & ~0xFFF)
        // [STAT] 去重Buffer持有延时细分: 组完成/聚合刷新时刻随消息传递
        double group_start_ns = 0.0;
        double group_complete_ns = 0.0;
        double agg_flush_ns = 0.0;
    };

    // ===================== IOMMU/PTW IOPS Counters =====================
    uint64_t iommu_total_completed;  // IOMMU 出口完成翻译总数
    uint64_t ptw_total_completed;    // PTW 完成任务总数

    // ===================== Port Byte Counters =====================
    uint64_t slave_0_total_bytes;    // axi_slave_0  入口接收总字节数(SPLIT下=读口)
    uint64_t slave_0w_total_bytes = 0;   // [读写端口分离] 写口接收总字节数(带宽分开统计)
    uint64_t master_0_total_bytes;   // axi_master_0 出口发送总字节数
    uint64_t master_1_total_bytes;   // axi_master_1 DDR 访问总字节数
    
    // ===================== Outstanding Peak Trackers =====================
    int peak_iommu_global_outstanding;     // IOMMU全局outstanding峰值

    // ============ [STAT需求1-诊断] 入口未决队列(input_delay_queue)积压 ============
    //   位置: 端口准入(outstanding++)之后、parser 弹出之前。
    //   [读写端口分离] 门控已前移到端口, 此队列积压 = 已过准入但尚未被 parser
    //   取走的任务, 按读/写方向分开统计。
    uint32_t input_q_count    = 0;      // 当前 input_delay_queue 待处理任务数(读+写)
    uint32_t input_q_peak     = 0;      // 峰值
    double   input_q_area_ns  = 0.0;    // Σ(count × Δt), 时间加权
    double   input_q_last_ns  = 0.0;    // 上次变化时刻(ns)
    double   input_q_first_ns = -1.0;   // 首次采样时刻(ns), -1=未开始
    // 按方向拆分的积压统计
    uint32_t input_q_r_count = 0, input_q_w_count = 0;
    uint32_t input_q_r_peak = 0, input_q_w_peak = 0;
    double   input_q_r_area_ns = 0.0, input_q_w_area_ns = 0.0;
    double   input_q_dir_last_ns = 0.0;
    double   input_q_dir_first_ns = -1.0;
    // 变化前采样: 把"旧count × 距上次变化的Δt"累加进面积
    void input_q_sample_before_change() {
        const double now = sc_time_stamp().to_seconds() * 1e9;
        if (input_q_first_ns < 0.0) { input_q_first_ns = now; input_q_last_ns = now; }
        if (now > input_q_last_ns) {
            input_q_area_ns += (double)input_q_count * (now - input_q_last_ns);
            input_q_last_ns = now;
        }
        if (input_q_dir_first_ns < 0.0) { input_q_dir_first_ns = now; input_q_dir_last_ns = now; }
        if (now > input_q_dir_last_ns) {
            input_q_r_area_ns += (double)input_q_r_count * (now - input_q_dir_last_ns);
            input_q_w_area_ns += (double)input_q_w_count * (now - input_q_dir_last_ns);
            input_q_dir_last_ns = now;
        }
    }
    int peak_ptw_outstanding;              // PTW outstanding峰值
    int peak_xdtw_dc_outstanding;          // xDTW DC walk outstanding峰值
    int peak_xdtw_pc_outstanding;          // xDTW PC walk outstanding峰值
    int peak_collector_dc_walk_outstanding; // Collector DC walk outstanding峰值
    int peak_collector_pc_walk_outstanding; // Collector PC walk outstanding峰值
    int peak_axi_master_1_outstanding;     // DDR端口outstanding峰值
    // [STAT] DDR端口(axi_master_1, 访问页表/目录表)读写分离并发统计
    //   并发定义: 每发一笔请求++, 每收一组response--; 读=页表/目录表walk, 写=A/D位更新+MSI MRIF
    int      axi_master_1_read_outstanding = 0;       // 当前读并发
    int      axi_master_1_write_outstanding = 0;      // 当前写并发
    int      peak_axi_master_1_read_outstanding = 0;  // 读并发峰值
    int      peak_axi_master_1_write_outstanding = 0; // 写并发峰值
    uint64_t axi_master_1_read_req_total = 0;         // 读请求总数
    uint64_t axi_master_1_write_req_total = 0;        // 写请求总数
    int peak_axi_master_0_outstanding;     // 出口端口outstanding峰值

    // ===================== [STAT] 平均并发统计(Little's Law) =====================
    // avg_concurrency = 驻留时间总和 / 仿真总时间
    // IOMMU: 用e2e延时总和; PTW: 用ptw_total_exec_ns
    double   iommu_read_e2e_total_ns;      // 读任务e2e延时总和(ns)
    double   iommu_write_e2e_total_ns;     // 写任务e2e延时总和(ns)
    uint64_t iommu_read_completed;         // 完成的读任务数
    uint64_t iommu_write_completed;        // 完成的写任务数

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
    uint32_t ptw_main_max_ddr_reads;       // 主任务最大DDR读次数
    uint32_t ptw_main_min_ddr_reads;       // 主任务最小DDR读次数
    uint32_t ptw_prefetch_max_ddr_reads;   // 预取任务最大DDR读次数
    uint32_t ptw_prefetch_min_ddr_reads;   // 预取任务最小DDR读次数
    // [STAT] 预取分析统计
    uint64_t ptw_late_spawn_triggered;     // 触发late-spawn的主任务数
    uint64_t ptw_late_spawn_skipped_msi;   // 因MSI跳过late-spawn的主任务数
    uint64_t ptw_late_spawn_skipped_bypass; // 因bypass跳过late-spawn的主任务数
    uint64_t ptw_prefetch_spawned_total;   // 实际spawn的预取任务总数
    // [STAT需求3] 主任务两阶段DDR分解(iova->gpa / gpa->spa) + 预取启动前DDR次数
    uint64_t ptw_main_vs_ddr_sum = 0;       // 主任务第一阶段(VS_WALK)DDR读总和
    uint64_t ptw_main_gs_ddr_sum = 0;       // 主任务第二阶段(GS_IMPLICIT+GS_EXPLICIT)DDR读总和
    uint64_t ptw_main_ad_ddr_sum = 0;       // 主任务AD_UPDATE DDR访问总和
    uint64_t ptw_main_stage_count = 0;      // 参与两阶段分解统计的主任务数
    uint64_t ptw_spawn_start_ddr_sum = 0;   // 预取启动时主任务已访问DDR次数总和
    uint64_t ptw_spawn_start_ddr_count = 0; // 预取启动次数(early+late)
    uint64_t ptw_early_spawn_ddr_sum = 0;   // early-spawn启动前DDR总和
    uint64_t ptw_early_spawn_cnt = 0;       // early-spawn次数
    uint64_t ptw_late_spawn_ddr_sum = 0;    // late-spawn启动前DDR总和
    uint64_t ptw_late_spawn_cnt = 0;        // late-spawn次数
    // [大页][STAT] 大页任务统计
    uint64_t ptw_hugepage_main_count;      // 大页主任务数(page_sz>4KB)
    uint64_t ptw_hugepage_pf_skipped;      // 大页跳过的预取spawn数
    uint64_t ptw_hugepage_invalid_slots;   // 返回的无效结果槽总数(D个/组)
    uint64_t ptw_front_leaf_hits;          // 前置leaf命中短路完成数(0次DDR)
    std::vector<double> ptw_output_interval_values;  // 所有输出间隔值(ns), 用于波动曲线
    std::vector<double> ptw_task_latency_values;     // 所有任务执行延时(ns), 用于分布统计
    // [STAT] PTW主任务/预取任务执行延时拆分统计 (主任务+预取任务口径)
    //   主任务: 由ptw_req_thread入口, walk_ctx.is_prefetch_task=false
    //   预取任务: 由late/early-spawn生成, walk_ctx.is_prefetch_task=true
    double   ptw_main_total_exec_ns = 0.0;         // 主任务执行延时总和(ns)
    double   ptw_main_max_task_latency_ns = 0.0;   // 主任务最大延时(ns)
    double   ptw_main_min_task_latency_ns = 999999999.0;  // 主任务最小延时(ns)
    std::vector<double> ptw_main_task_latency_values;    // 主任务延时序列
    double   ptw_prefetch_total_exec_ns = 0.0;     // 预取任务执行延时总和(ns)
    double   ptw_prefetch_max_task_latency_ns = 0.0;    // 预取任务最大延时(ns)
    double   ptw_prefetch_min_task_latency_ns = 999999999.0; // 预取任务最小延时(ns)
    std::vector<double> ptw_prefetch_task_latency_values; // 预取任务延时序列
    // [STAT] PTW并发任务数时间加权平均(Little's Law精确版):
    //   每次ptw_outstanding_task_count变化前, 累加 (旧值 × 距上次变化的时长);
    //   输出时: 平均并发 = 累计面积 / (末次采样时刻 - 首次采样时刻)
    double   ptw_concurrency_area_ns = 0.0;        // Σ(count × Δt), 单位 task·ns
    double   ptw_concurrency_last_change_ns = 0.0; // 上一次count变化时刻(ns)
    double   ptw_concurrency_first_sample_ns = -1.0; // 首次采样时刻(ns), -1表示未开始
    double   ptw_concurrency_last_sample_ns = 0.0;   // 末次采样时刻(ns), 用于计算窗口
    // [STAT] PTW任务组执行时间统计 (1主+D预取为一组)
    double   ptw_group_exec_total_ns;        // 所有组执行时间总和(ns)
    double   ptw_group_exec_max_ns;          // 单组最大执行时间(ns)
    double   ptw_group_exec_min_ns;          // 单组最小执行时间(ns)
    uint64_t ptw_group_exec_count;           // 完成的组数
    std::vector<double> ptw_group_exec_values; // 每组执行时间(ns), 用于分布统计
    // [STAT] PTW任务组DDR读次数统计 (1主+D预取为一组)
    uint64_t ptw_group_total_ddr_reads;      // 所有组DDR读次数总和
    uint32_t ptw_group_max_ddr_reads;        // 单组最大DDR读次数
    uint32_t ptw_group_min_ddr_reads;        // 单组最小DDR读次数
    std::vector<uint32_t> ptw_group_ddr_values; // 每组DDR读次数, 用于分布统计

    // ===================== PT Cache 输出间隔统计 =====================
    double   pt_cache_out_last_ns;          // 上一次PT Cache输出时刻(ns)
    double   pt_cache_out_interval_total_ns; // 输出间隔总和(ns)
    double   pt_cache_out_interval_max_ns;   // 最大输出间隔(ns)
    double   pt_cache_out_interval_min_ns;   // 最小输出间隔(ns)
    uint64_t pt_cache_out_interval_count;    // 输出间隔采样次数
    uint64_t pt_cache_out_total_count;       // PT Cache输出总次数(HIT+MISS)

    // ===================== SQ/CQ/MSI PT Cache命中路径统计 =====================
    // 追踪is_ctrl=1的控制包(SQ/CQ/MSI)是否全部PT Cache命中,未进入dedup/PTW
    uint64_t ctrl_total_count;             // 控制包总数(SQ+CQ+MSI)
    uint64_t ctrl_pt_cache_hit_count;      // 控制包PT Cache命中(常规CL HIT,直接转发)
    uint64_t ctrl_pt_cache_miss_count;     // 控制包PT Cache缺失(进入dedup/PTW)
    uint64_t ctrl_sq_count;                // SQ请求数
    uint64_t ctrl_cq_count;                // CQ请求数
    uint64_t ctrl_msi_count;               // MSI请求数

    // ===================== End-to-End Latency Statistics =====================
    double   iommu_total_e2e_latency_ns;  // 所有IO端到端延时总和(ns)
    double   iommu_e2e_max_ns;            // 单笔IO最大端到端延时(ns)
    double   iommu_e2e_min_ns;            // 单笔IO最小端到端延时(ns)
    std::vector<double> iommu_e2e_values; // 每笔IO端到端延时(ns), 用于分布统计

    // ===================== IOMMU Input/Output Port Interval Statistics =====================
    // 输入端口(inbound_fifo_r/w)任务间隔(合计口径)
    double   iommu_in_last_ns;              // 上一次输入任务时刻(ns)
    double   iommu_in_interval_total_ns;    // 输入间隔总和(ns)
    double   iommu_in_interval_max_ns;      // 最大输入间隔(ns)
    double   iommu_in_interval_min_ns;      // 最小输入间隔(ns)
    uint64_t iommu_in_interval_count;       // 输入间隔采样次数
    std::vector<double> iommu_in_interval_values;  // 所有输入间隔值(ns)
    // [读写端口分离] 读口/写口各自的输入间隔统计
    double   iommu_in_r_last_ns = 0.0, iommu_in_w_last_ns = 0.0;
    double   iommu_in_r_interval_total_ns = 0.0, iommu_in_w_interval_total_ns = 0.0;
    double   iommu_in_r_interval_max_ns = 0.0, iommu_in_w_interval_max_ns = 0.0;
    double   iommu_in_r_interval_min_ns = 999999999.0, iommu_in_w_interval_min_ns = 999999999.0;
    uint64_t iommu_in_r_interval_count = 0, iommu_in_w_interval_count = 0;
    std::vector<double> iommu_in_r_interval_values, iommu_in_w_interval_values;
    std::vector<double> iommu_in_r_interval_end_ns, iommu_in_w_interval_end_ns;
    // 输出端口(master_0)任务间隔
    double   iommu_out_last_ns;             // 上一次输出任务时刻(ns)
    double   iommu_out_interval_total_ns;   // 输出间隔总和(ns)
    double   iommu_out_interval_max_ns;     // 最大输出间隔(ns)
    double   iommu_out_interval_min_ns;     // 最小输出间隔(ns)
    uint64_t iommu_out_interval_count;      // 输出间隔采样次数
    std::vector<double> iommu_out_interval_values; // 所有输出间隔值(ns)
    // [STAT需求1] 每个间隔对应的结束时刻(ns), 用于稳态窗口[#1000~#9000]内筛选
    std::vector<double> iommu_in_interval_end_ns;
    std::vector<double> iommu_out_interval_end_ns;

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
    // [聚合刷新] PEQ: PTW完成后聚合任务(刷新去重buffer/去重cache/PT Cache)200ns非阻塞延时
    tlm_utils::peq_with_get<pending_group_update> agg_flush_peq;

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
#if TEST_CFG_MULTI_DEVICE_SCENE && TEST_CFG_PERDEV_WRITE_ORDER
    // [写保序=每设备] 队列数=设备数: 设备内写严格保序, 设备间写互不阻塞
    std::queue<uint32_t> reorder_write_order_dev[iommu_md::MAX_DEVICE_COUNT];
#endif
    sc_mutex reorder_mtx;
    sc_event reorder_ready_event;

    // ===================== 流水线延时静态延时线 =====================
    // [静态化改造] 入口(200ns)/出口(400ns)流水线延时原先对每个 task 调用 sc_spawn
    //   派生独立协程；在多设备(N=16)大事务量(16万包)下会累积数十万动态进程, 触发
    //   SystemC 协程栈分配断言 (F4 assertion failed: ret == 0 @ sc_cor_qt.cpp) 而崩溃。
    //   改为两条静态 SC_THREAD 延时线: 到达侧仅入队(task + 到期时刻), 由常驻线程按
    //   FIFO 到期顺序统一执行"写 inbound_fifo"/"发送响应+统计+释放"。因入队不阻塞且
    //   延时固定, 到期顺序即到达顺序, 与原并行协程的时序语义等价, 且进程数恒定。
    struct pipeline_delay_item_t {
        iommu_task_t* task;
        double        due_ns;   // 到期时刻(ns): 入队时刻 + 流水线延时
    };
    // [读写端口分离] 入口延时线按方向拆为两条, 读写互不阻塞;
    //   出口延时线保持单条(reorder 输出线程本身串行发送, 无方向间HOL)。
    std::queue<pipeline_delay_item_t> input_delay_queue_r;  // 读方向入口延时线队列
    sc_mutex input_delay_mtx_r;
    sc_event input_delay_event_r;
    std::queue<pipeline_delay_item_t> input_delay_queue_w;  // 写方向入口延时线队列
    sc_mutex input_delay_mtx_w;
    sc_event input_delay_event_w;
    std::queue<pipeline_delay_item_t> output_delay_queue;  // 出口延时线队列
    sc_mutex output_delay_mtx;
    sc_event output_delay_event;
    // 入口延时线通用实现(读/写各一条常驻 SC_THREAD 调用)
    void input_delay_line(std::queue<pipeline_delay_item_t>& q, sc_mutex& mtx,
                          sc_event& evt, sc_fifo<iommu_task_t*>& out_fifo,
                          uint32_t& dir_q_count, uint32_t& dir_q_peak);
    void input_delay_thread_r();  // 常驻: 读方向到期后写入 inbound_fifo_r
    void input_delay_thread_w();  // 常驻: 写方向到期后写入 inbound_fifo_w
    void output_delay_thread();   // 常驻: 到期后发送响应并统计/释放 task

    // 全局 outstanding：端口准入申请，reorder 出口释放
    int iommu_global_outstanding;
    sc_event iommu_global_outstanding_freed_event;

    // [场景13] 读写分离 outstanding 计数器与峰值跟踪
    int iommu_read_outstanding;
    int iommu_write_outstanding;
    int peak_iommu_read_outstanding;
    int peak_iommu_write_outstanding;
    sc_event iommu_read_outstanding_freed_event;
    sc_event iommu_write_outstanding_freed_event;

    // ============ [STAT] 端口准入门控阻塞统计(读写端口分离) ============
    //   门控已从 parser 前移到端口: 读口只因"读满/全局满"阻塞,
    //   写口只因"写满/全局满"阻塞, 两方向互不影响(无交叉队头阻塞)。
    uint64_t port_blk_read_cnt = 0;      // 读口因额度满而阻塞的次数
    uint64_t port_blk_write_cnt = 0;     // 写口因额度满而阻塞的次数
    double   port_blk_read_ns = 0.0;     // 读口阻塞总时长(ns)
    double   port_blk_write_ns = 0.0;    // 写口阻塞总时长(ns)
    double   port_blk_read_max_ns = 0.0; // 读口单次最大阻塞(ns)
    double   port_blk_write_max_ns = 0.0;// 写口单次最大阻塞(ns)
    uint64_t port_admit_read_total = 0;  // 读口准入任务总数
    uint64_t port_admit_write_total = 0; // 写口准入任务总数

    // ============ [STAT] 读写端口独立节拍/供给侧验证(读写端口分离) ============
    //   port_beat_*_ns : 各端口【带宽节拍】累计占用时长(Σ 逐包 wait), 除以注入窗口
    //                  即端口线速占用率; ≈100% 表示该口一直在按自身节拍收包。
    //   port_dir_mismatch_cnt: 端口收到与本方向不符的命令(绑定错误), 必须为 0。
    double   port_beat_read_ns = 0.0;    // 读口节拍累计时长(ns)
    double   port_beat_write_ns = 0.0;   // 写口节拍累计时长(ns)
    uint64_t port_dir_mismatch_cnt = 0;  // R口收到写/W口收到读 的次数
    double   port_first_admit_r_ns = -1.0;  // 注入窗口内读口首次准入时刻(ns)
    double   port_first_admit_w_ns = -1.0;  // 注入窗口内写口首次准入时刻(ns)

    // ============ [STAT] 注入窗口内的读写并发顶满程度 ============
    //   窗口定义: RP 测量 epoch 开始注入(与 reset_4kb_concurrency_window 同点)打开,
    //   到 RP 把全部请求都送完端口后关闭。窗口内 RP 供给侧必然有请求可发,
    //   因此该窗口内的时间加权平均并发才是"读写是否顶满 320"的正确口径
    //   (全运行窗口会把注入结束后的 drain 段计入而大幅拉低平均值)。
    //   full_*_ns : 窗口内该计数器【等于上限】的时长, 除以窗口长度即顶满时长占比。
    bool     conc_win_open = false;             // 窗口是否打开
    double   conc_win_start_ns = 0.0;           // 窗口起点(ns)
    double   conc_win_last_ns = 0.0;            // 窗口内上次采样时刻(ns)
    double   conc_win_end_ns = 0.0;             // 窗口终点(关闭时刻, 0=未关闭)
    double   conc_win_area_read_ns = 0.0;       // Σ read_outstanding × Δt
    double   conc_win_area_write_ns = 0.0;      // Σ write_outstanding × Δt
    double   conc_win_area_global_ns = 0.0;     // Σ global_outstanding × Δt
    double   conc_win_full_read_ns = 0.0;       // read 顶满额度时长(ns)
    double   conc_win_full_write_ns = 0.0;      // write 顶满额度时长(ns)
    double   conc_win_full_global_ns = 0.0;     // global 顶满额度时长(ns)
    uint64_t conc_win_sample_cnt = 0;           // 窗口内采样次数
    // 窗口内分段历史: 每段记录【起止时刻 + 该段内生效的 read/write/global 计数】。
    //   用途: 关闭后可任意切片重算子窗口(如"双口同时在注入")的时间加权均值,
    //   避免把"某一口已注完、只剩另一口"的尾巴计入而低估该方向顶满程度。
    struct conc_seg_t { double s, e, r, w, g; };
    std::vector<conc_seg_t> conc_win_hist;
    // 在 [t0,t1] 内按分段常数积分, 输出面积与顶满时长
    void conc_window_slice(double t0, double t1, double& avg_r, double& avg_w, double& avg_g,
                           double& full_r, double& full_w, double& full_g) const;

    void conc_window_open();                    // 开始注入时调用
    void conc_window_close();                   // RP 全部请求已送达端口后调用
    //   在 outstanding 变化前采样窗口面积与顶满时长(与 ++/-- 严格配对)
    void conc_window_sample() {
        if (!conc_win_open) return;
        const double now_ns = sc_core::sc_time_stamp().to_seconds() * 1e9;
        const double delta  = now_ns - conc_win_last_ns;
        if (delta > 0.0) {
            const double vr = (double)iommu_read_outstanding;
            const double vw = (double)iommu_write_outstanding;
            const double vg = (double)iommu_global_outstanding;
            conc_win_area_read_ns   += vr * delta;
            conc_win_area_write_ns  += vw * delta;
            conc_win_area_global_ns += vg * delta;
            if (iommu_read_outstanding   >= (int)IOMMU_READ_MAX_OUTSTANDING)   conc_win_full_read_ns   += delta;
            if (iommu_write_outstanding  >= (int)IOMMU_WRITE_MAX_OUTSTANDING)  conc_win_full_write_ns  += delta;
            if (iommu_global_outstanding >= (int)IOMMU_GLOBAL_MAX_OUTSTANDING) conc_win_full_global_ns += delta;
            // 本段 [上次采样, 本次采样) 内旧计数恒定, 直接存入显式区间
            conc_win_hist.push_back(conc_seg_t{conc_win_last_ns, now_ns, vr, vw, vg});
        }
        conc_win_last_ns = now_ns;
        conc_win_sample_cnt++;
    }

    // ============ [STAT] Parser 门控快照监控 ============
    //   目的: 验证"读满或写满 -> 后续任务(含另一方向)全部进不来"的队头阻塞是否
    //         在稳态下持续成立。采样点 = parser_thread 每次从 inbound_fifo 弹出
    //         task 之后、方向门控 wait 之前, 记录当时 read/write/global outstanding。
    //   判据: SELF_FULL=1 表示该 task 自身方向已满(必然进入 wait);
    //         EITHER_FULL=1 表示读或写至少一个满(此时另一方向的请求也被堵住)。
    uint64_t parser_recv_total = 0;         // 弹出的请求总数
    uint64_t parser_recv_read = 0;          // 其中读请求
    uint64_t parser_recv_write = 0;         // 其中写请求
    uint64_t parser_snap_read_full = 0;     // 快照时 read_outstanding 已满
    uint64_t parser_snap_write_full = 0;    // 快照时 write_outstanding 已满
    uint64_t parser_snap_either_full = 0;   // 快照时 读满 或 写满
    uint64_t parser_snap_both_full = 0;     // 快照时 读写同时满
    uint64_t parser_snap_global_full = 0;   // 快照时 global 已满
    uint64_t parser_snap_selfdir_full = 0;  // 快照时【自身方向】已满(会真阻塞)
    uint64_t parser_snap_otherdir_full = 0; // 快照时【另一方向】满而自身未满(纯HOL受害)
    uint64_t parser_blk_by_read = 0;        // 实际因读满 wait 的次数
    uint64_t parser_blk_by_write = 0;       // 实际因写满 wait 的次数
    uint64_t parser_blk_by_global = 0;      // 实际因全局满 wait 的次数
    double   parser_blk_dir_ns = 0.0;       // 方向门控阻塞总时长(ns)
    double   parser_blk_dir_max_ns = 0.0;   // 方向门控单次最大阻塞(ns)
    double   parser_blk_global_ns = 0.0;    // 全局门控阻塞总时长(ns)
    double   parser_snap_read_sum = 0.0;    // Σ read_outstanding  (算 parser 视角平均并发)
    double   parser_snap_write_sum = 0.0;   // Σ write_outstanding
    double   parser_snap_global_sum = 0.0;  // Σ global_outstanding

    // ============ [STAT] IOMMU 内部实际并发的时间加权统计(面积法) ============
    //   与 PTW 的 ptw_sample_concurrency_before_change() 同法:
    //   在 read/write/global outstanding 每次变化【之前】采样, 累加
    //   「旧计数 × 距上次变化的时长」得到面积; 输出时
    //   时间加权平均并发 = 累计面积 / (末次采样时刻 - 首次采样时刻)。
    //   采样点必须与 ++/-- 严格配对:
    //     入口: reorder_register_task()  中 outstanding++ 之前
    //     出口: reorder_output_thread()  中 outstanding-- 之前
    double   iommu_conc_area_read_ns = 0.0;      // Σ read_outstanding × Δt   (task·ns)
    double   iommu_conc_area_write_ns = 0.0;     // Σ write_outstanding × Δt
    double   iommu_conc_area_global_ns = 0.0;    // Σ global_outstanding × Δt
    double   iommu_conc_first_sample_ns = -1.0;  // 首次采样时刻(窗口起点), -1=未开始
    double   iommu_conc_last_change_ns = 0.0;    // 上次计数变化时刻
    double   iommu_conc_last_sample_ns = 0.0;    // 末次采样时刻(窗口终点)

    // [STAT] IOMMU 内部并发采样: 在 read/write/global outstanding 变化前调用
    void iommu_sample_concurrency_before_change() {
        const double now_ns = sc_core::sc_time_stamp().to_seconds() * 1e9;
        if (iommu_conc_first_sample_ns < 0.0) {
            iommu_conc_first_sample_ns = now_ns;
            iommu_conc_last_change_ns  = now_ns;
        } else {
            const double delta = now_ns - iommu_conc_last_change_ns;
            if (delta > 0.0) {
                iommu_conc_area_read_ns   += (double)iommu_read_outstanding   * delta;
                iommu_conc_area_write_ns  += (double)iommu_write_outstanding  * delta;
                iommu_conc_area_global_ns += (double)iommu_global_outstanding * delta;
            }
            iommu_conc_last_change_ns = now_ns;
        }
        iommu_conc_last_sample_ns = now_ns;
    }

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
    // [聚合刷新] PEQ process线程: 200ns延时后执行去重buffer/去重cache/PT Cache刷新
    void agg_flush_process_thread();

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
    // [读写端口分离] 入站 AT 回调按端口拆成两个【独立注册函数】, 且两份实现完全独立:
    //   axi slave R port -> axi_slave_r_nb_transport_fw
    //       只处理读请求, 节拍 wait(512B=8ns / 128B=2ns / SQ,CQ,MSI=1ns) 在本函数内部,
    //       只按 Read(320)+Global(640) 门控, 准入后入 inbound_fifo_r
    //   axi slave W port -> axi_slave_w_nb_transport_fw
    //       只处理写请求, 节拍 wait(512B=8ns / 128B=2ns / SQ,CQ,MSI=1ns) 在本函数内部,
    //       只按 Write(320)+Global(640) 门控, 准入后入 inbound_fifo_w
    //   两者不共用任何入口/准入函数, 互不影响; 方向由【端口】决定(不依赖命令);
    //   收到方向不符的命令即 FATAL。非 SPLIT 场景(单端口同时收读写)仍用
    //   axi_slave_nb_transport_fw, 方向由 trans.get_command() 判定。
    //   下一级 parser_thread 从 inbound_fifo_r / inbound_fifo_w 轮询(RR)取请求。
    tlm::tlm_sync_enum axi_slave_r_nb_transport_fw(
        tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay);
    tlm::tlm_sync_enum axi_slave_w_nb_transport_fw(
        tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay);
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
        inbound_fifo_r("inbound_fifo_r", FIFO_DEPTH_INBOUND),
        inbound_fifo_w("inbound_fifo_w", FIFO_DEPTH_INBOUND),
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
        agg_flush_peq("agg_flush_peq"),
        msiptw_outstanding_task_count(0),
        axi_master_1_to_cmn_rnd_outstanding(0),
        axi_master_0_to_pcie_noc_outstanding(0),
        iommu_global_outstanding(0),
        iommu_read_outstanding(0),
        iommu_write_outstanding(0),
        peak_iommu_read_outstanding(0),
        peak_iommu_write_outstanding(0),
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
        ptw_main_max_ddr_reads(0),
        ptw_main_min_ddr_reads(999999),
        ptw_prefetch_max_ddr_reads(0),
        ptw_prefetch_min_ddr_reads(999999),
        ptw_late_spawn_triggered(0),
        ptw_late_spawn_skipped_msi(0),
        ptw_late_spawn_skipped_bypass(0),
        ptw_prefetch_spawned_total(0),
        ptw_hugepage_main_count(0),
        ptw_hugepage_pf_skipped(0),
        ptw_hugepage_invalid_slots(0),
        ptw_front_leaf_hits(0),
        ptw_group_exec_total_ns(0.0),
        ptw_group_exec_max_ns(0.0),
        ptw_group_exec_min_ns(999999999.0),
        ptw_group_exec_count(0),
        ptw_group_total_ddr_reads(0),
        ptw_group_max_ddr_reads(0),
        ptw_group_min_ddr_reads(999999),
        pt_cache_out_last_ns(0.0),
        pt_cache_out_interval_total_ns(0.0),
        pt_cache_out_interval_max_ns(0.0),
        pt_cache_out_interval_min_ns(999999999.0),
        pt_cache_out_interval_count(0),
        pt_cache_out_total_count(0),
        ctrl_total_count(0),
        ctrl_pt_cache_hit_count(0),
        ctrl_pt_cache_miss_count(0),
        ctrl_sq_count(0),
        ctrl_cq_count(0),
        ctrl_msi_count(0),
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
#if TEST_CFG_MULTI_DEVICE_SCENE
        md_trace.configure(TEST_CFG_NUM_DEVICES);
        cache_sub.set_md_trace(&md_trace);
#endif

        // Register AT nb_transport callbacks for inbound and DDR
        // [读写端口分离] SPLIT: R port 注册 axi_slave_r_nb_transport_fw,
        //   W port 注册 axi_slave_w_nb_transport_fw —— 两个【不同】的注册函数,
        //   各自只处理本方向请求; 非 SPLIT: 单端口保留按命令分发的旧回调。
#if TEST_CFG_SPLIT_RW_PORTS
        axi_slave_from_pcie_noc_0_socket.register_nb_transport_fw(
            this, &iommu_top::axi_slave_r_nb_transport_fw);
        axi_slave_from_pcie_noc_0w_socket.register_nb_transport_fw(
            this, &iommu_top::axi_slave_w_nb_transport_fw);
#else
        axi_slave_from_pcie_noc_0_socket.register_nb_transport_fw(
            this, &iommu_top::axi_slave_nb_transport_fw);
#endif
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
        SC_THREAD(agg_flush_process_thread);       // [聚合刷新] PEQ 200ns延时刷新
        SC_THREAD(msipt_cache_query_thread);
        SC_THREAD(msipt_cache_result_thread);
        SC_THREAD(msiptw_req_thread);
        SC_THREAD(msiptw_rsp_thread);
        SC_THREAD(pt_forwarder_thread);
        SC_THREAD(msipt_forwarder_thread);
        SC_THREAD(fault_cq_proc_thread);
        SC_THREAD(ddr_arbiter_thread);
        SC_THREAD(reorder_output_thread);   // 出口重排序线程
        // [静态延时线] 入口(读/写各一条)/出口流水线延时常驻线程(替代 per-task sc_spawn)
        SC_THREAD(input_delay_thread_r);
        SC_THREAD(input_delay_thread_w);
        SC_THREAD(output_delay_thread);

        // [重构] 绑定去重模块回调: 转发 FIFO + Buffer 刷新回调
        // dedup_scheduler_thread 处理 dedup_update 时经此回调刷新 Buffer 并转发任务
        cache_sub.set_forward_fifo(&pt_cache_to_fwd_fifo);
        cache_sub.set_dedup_flush_callback(
            [this](uint16_t head_index, const iommu::CacheMessage& upd) {
                this->dedup_flush_chain_cb(head_index, upd);
            });
        // [FIX] 绑定PTW并发容量检查回调: Buffer满时dedup worker通过此回调检查PTW是否有空闲槽位
        cache_sub.set_ptw_capacity_check(
            [this]() -> bool {
                return this->ptw_outstanding_task_count < (int)PTW_MAX_OUTSTANDING_TASKS;
            });

        memset(ctrl_path_rsp_buf, 0, sizeof(ctrl_path_rsp_buf));
    }

    ~iommu_top(){
    }

    // PTW完成后释放并发计数（无延时）
    void ptw_release_outstanding() {
        ptw_sample_concurrency_before_change();  // [STAT] 采样当前count×时长
        ptw_outstanding_task_count--;
        assert(ptw_outstanding_task_count >= 0);
        ptw_task_completed_event.notify(SC_ZERO_TIME);
        cache_sub.get_dedup_buffer_full_event().notify(SC_ZERO_TIME);
    }

    // [STAT] PTW并发数采样: 在ptw_outstanding_task_count变化前调用,
    //   累计 "旧count × 距上次变化时长" 得到时间加权面积
    void ptw_sample_concurrency_before_change() {
        const double now_ns = sc_core::sc_time_stamp().to_seconds() * 1e9;
        if (ptw_concurrency_first_sample_ns < 0.0) {
            ptw_concurrency_first_sample_ns = now_ns;
            ptw_concurrency_last_change_ns = now_ns;
        } else {
            const double delta = now_ns - ptw_concurrency_last_change_ns;
            if (delta > 0.0) {
                ptw_concurrency_area_ns += (double)ptw_outstanding_task_count * delta;
            }
            ptw_concurrency_last_change_ns = now_ns;
        }
        ptw_concurrency_last_sample_ns = now_ns;
    }

    // [STAT] PTW任务执行延时统一记录入口: 同时更新
    //   - 合并口径(ptw_total_exec_ns / ptw_task_latency_values / max/min)
    //   - 主任务或预取任务拆分口径(按is_prefetch_task分类)
    // 调用位置需与原有 ptw_task_start_ns 查找/擦除代码配套使用
    void ptw_record_task_latency(uint32_t task_id, bool is_prefetch) {
        auto it = ptw_task_start_ns.find(task_id);
        if (it == ptw_task_start_ns.end()) return;
        const double now_ns = sc_core::sc_time_stamp().to_seconds() * 1e9;
        const double lat = now_ns - it->second;
        ptw_task_start_ns.erase(it);

        // 合并口径(向后兼容既有报表)
        ptw_total_exec_ns += lat;
        ptw_task_latency_values.push_back(lat);
        if (lat > ptw_max_task_latency_ns) ptw_max_task_latency_ns = lat;
        if (lat < ptw_min_task_latency_ns) ptw_min_task_latency_ns = lat;

        // 主/预取拆分口径
        if (is_prefetch) {
            ptw_prefetch_total_exec_ns += lat;
            ptw_prefetch_task_latency_values.push_back(lat);
            if (lat > ptw_prefetch_max_task_latency_ns) ptw_prefetch_max_task_latency_ns = lat;
            if (lat < ptw_prefetch_min_task_latency_ns) ptw_prefetch_min_task_latency_ns = lat;
        } else {
            ptw_main_total_exec_ns += lat;
            ptw_main_task_latency_values.push_back(lat);
            if (lat > ptw_main_max_task_latency_ns) ptw_main_max_task_latency_ns = lat;
            if (lat < ptw_main_min_task_latency_ns) ptw_main_min_task_latency_ns = lat;
        }
    }
};

#endif
