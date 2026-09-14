#ifndef __IOMMU_TASK_HH__
#define __IOMMU_TASK_HH__

#include "systemc.h"
#include "tlm.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include "iommu_data_structures.hh"
#include "iommu_translate.hh"
#include "iommu_req_rsp.hh"
#include "iommu_perf_params.hh"
#include "iommu_dedup_params.hh"

// Forward declaration for iommu namespace types
namespace iommu {
    using iova_t = uint64_t;
}

// ===================== Task State Enum =====================
enum task_state_t {
    TASK_INIT = 0,
    TASK_PARSING,
    TASK_PARSE_DONE,
    TASK_DC_QUERY,
    TASK_DC_HIT,
    TASK_DC_MISS,
    TASK_PC_QUERY,
    TASK_PC_HIT,
    TASK_PC_MISS,
    TASK_COLLECTING,
    TASK_ROUTE_DECISION,
    TASK_XDTW_REQ,
    TASK_XDTW_DONE,
    TASK_TLB_QUERY,
    TASK_TLB_HIT,
    TASK_TLB_MISS,
    TASK_PTW_REQ,
    TASK_PTW_DONE,
    TASK_MSI_QUERY,
    TASK_MSI_HIT,
    TASK_MSI_MISS,
    TASK_MSIPTW_REQ,
    TASK_MSIPTW_DONE,
    TASK_FORWARD,
    TASK_FAULT,
    TASK_DONE
};

// ===================== Walk Type Enum =====================
enum walk_type_t {
    WALK_DDT = 0,
    WALK_PDT,
    WALK_VS_PT,
    WALK_G_PT,
    WALK_MSI_PT
};

// ===================== xDTW Walk Phase Enum =====================
enum xdtw_walk_phase_t {
    XDTW_DDT_NON_LEAF = 0,
    XDTW_DDT_READ_DC,
    XDTW_PDT_NON_LEAF,
    XDTW_PDT_GS_IMPLICIT,
    XDTW_PDT_READ_PC
};

// ===================== PTW Walk Phase Enum =====================
enum ptw_walk_phase_t {
    PTW_VS_WALK = 0,
    PTW_GS_IMPLICIT,
    PTW_GS_EXPLICIT,
    PTW_AD_UPDATE,
    PTW_PREFETCH_WAIT,  // NEW: 等待预取DDR响应
    PTW_VS_PTE_READ     // NEW: 两阶段预取 - 读取VS L0 PTE
};

// ===================== MSIPTW Walk Phase Enum =====================
enum msiptw_walk_phase_t {
    MSIPTW_READ_MSIPTE = 0
};

// ===================== Walk Context =====================
struct walk_context_t {
    walk_type_t walk_type = WALK_DDT;
    int walk_phase = 0;             // xdtw_walk_phase_t / ptw_walk_phase_t / msiptw_walk_phase_t
    int8_t level = 0;
    int8_t max_levels = 0;
    uint64_t base_addr = 0;
    uint16_t indexes[5] = {0};
    uint64_t read_addr = 0;
    uint32_t read_size = 0;
    uint8_t read_buf[WALK_CTX_READ_BUF_SIZE] = {0};  // 支持 DC/PC 读取及 combined burst
    uint8_t ptesize = 0;

    // VS-stage walk context (for PTW)
    uint16_t vpn[5] = {0};
    int8_t vs_level = 0;

    // G-stage walk context (for PTW nested walk)
    int8_t gs_level = 0;
    uint64_t gs_base_addr = 0;
    uint64_t gs_pte_addr = 0;
    uint64_t pending_vs_pte_addr = 0;
    uint16_t gs_vpn[5] = {0};

    // Two-stage prefetch: VS L0 table info (for reading sequential VS L0 PTEs)
    uint64_t vs_l0_spa_ppn = 0;    // VS L0表所在物理页号 (from GS_IMPLICIT)
    uint64_t vs_l0_gpa_base = 0;   // VS L0表GPA基址 (from VS walk)

    // A/D bit update context
    spte_t ad_pte{};
    uint64_t ad_pte_addr = 0;

    // DDR read count for walk progress tracking
    uint32_t ddr_read_count = 0;

    // Walker Cache intermediate results (for PTW)
    // 保存walk过程中的中间PPN，用于最后update walker cache
    struct {
        uint64_t ppn_level2 = 0;  // Level 2 PPN (PTWc_3)
        uint64_t ppn_level1 = 0;  // Level 1 PPN (PTWc_2)
        uint64_t ppn_level0 = 0;  // Level 0 PPN (PTWc_1)
        bool valid_level2 = false;
        bool valid_level1 = false;
        bool valid_level0 = false;
        // 两阶段专用: GS_IMPLICIT leaf 翻译后的 SPA，供 VS_WALK non-leaf 保存
        uint64_t last_gs_implicit_spa = 0;
        bool has_pending_spa = false;
    } walker_cache_entries;

    // Walker Cache lookup命中层级（用于update时避免冗余更新）
    // 0=全部miss, 3=c3 hit, 2=c2 hit, 1=c1 hit
    uint8_t walker_hit_level = 0;

    // [前置] Walker Cache前置查询结果 (configure_and_route处发起, 随任务透传至PTW)
    //   walker_front_valid: 前置查询结果已写回(PTW输入侧兜底等待此标志)
    //   walker_front_hit/level/next_ppn: 查询结果原始值, PTW在walk init时解释
    bool     walker_front_valid = false;
    bool     walker_front_hit = false;
    uint8_t  walker_front_level = 0;
    uint64_t walker_front_next_ppn = 0;
    // [大页] 前置查询命中端到端大页leaf PTE: next_ppn即最终物理页帧PPN,
    // PTW可完全短路(0次DDR); leaf_page_sz由命中子表级推导(C3=2MB/C2=1GB/C1=512GB)
    bool     walker_front_is_leaf = false;
    uint64_t walker_front_leaf_page_sz = 0;

    // [S2] S2 Walker Cache 命中层级 (0=全miss, 3=c3 hit, 2=c2 hit, 1=c1 hit)
    // 用于GS_EXPLICIT阶段的G-stage walker cache
    uint8_t s2_walker_hit_level = 0;

    // [S2] S2 Walker Cache 中间结果 (GS_EXPLICIT walk过程中保存，供update使用)
    // 命名与WalkerCache ptwc1/ptwc2/ptwc3对齐:
    //   level1 = gs_level=3的中间结果 → 存入ptwc1
    //   level2 = gs_level=2的中间结果 → 存入ptwc2
    //   level3 = gs_level=1的中间结果 → 存入ptwc3
    struct {
        uint64_t s2_ppn_level1 = 0;  // gs_level=3 result → ptwc1
        uint64_t s2_ppn_level2 = 0;  // gs_level=2 result → ptwc2
        uint64_t s2_ppn_level3 = 0;  // gs_level=1 result → ptwc3
        bool s2_valid_level1 = false;
        bool s2_valid_level2 = false;
        bool s2_valid_level3 = false;
    } s2_walker_cache_entries;

    // NEW: 预取相关
    bool      prefetch_enabled = false;     // 是否启用预取
    uint32_t  prefetch_depth = 0;           // 预取深度（页数量，默认=8）
    uint64_t  prefetch_base_iova = 0;       // 预取起始IOVA
    uint32_t  prefetch_count = 0;           // 已预取的页数量
    uint64_t  prefetch_iovas[16];           // 预取的IOVA列表（最大16页）

    // [重构] dedup 降级标志: dedup_cache 插入失败, 任务直接转发 PTW,
    // PTW 完成后不写 dedup_update_fifo(dedup_cache 无占位), 由 PTW 路径直接转发
    bool      dedup_bypass = false;

    // NEW: 主任务特有: 预取组状态追踪
    struct {
        uint32_t completed_count = 0;       // 已完成的walk数
        bool     all_completed = false;     // 全部完成标志
        uint64_t group_iovas[17];           // 组内所有IOVA
        spte_t   group_vs_ptes[17];         // 组内所有VS-stage PTE
        gpte_t   group_g_ptes[17];          // 组内所有G-stage PTE
        uint64_t group_pas[17];             // 组内所有PA
        uint64_t group_page_szs[17];        // 组内所有页大小
    } prefetch_group;
    
    // NEW: PT Cache批量更新(保留原有字段)
    uint32_t  pt_update_count = 0;          // 需要更新的PT Cache条目数
    struct {
        uint64_t iova;                        // 目标IOVA
        spte_t vs_pte;                      // VS-stage PTE
        gpte_t g_pte;                       // G-stage PTE
        uint64_t pa;                        // 物理地址
        uint64_t page_sz;                   // 页大小
    } pt_updates[17];                       // 1个主任务 + 16个预取

    // NEW: 预取等待状态
    int8_t prefetch_ddr_pending = 0;        // 待完成的预取DDR响应数
    
    // NEW: 预取组管理(Burst预取方案)
    uint32_t  prefetch_group_id = 0;        // 预取组ID(同一组共享,使用主task_id)
    bool      is_prefetch_task = false;     // 是否为预取任务(非主任务)
    bool      is_two_stage_prefetch = false; // 是否为两阶段预取主任务(monitor读取group arrays)
    uint32_t  prefetch_idx = 0;             // 预取索引(0=主任务, 1~D=预取)
    uint32_t  prefetch_total = 0;           // 预取组总任务数(1+D)
    uint64_t  leaf_pt_base_addr = 0;        // Leaf页表基址(用于地址计算)
    uint8_t   leaf_ptesize = 8;             // Leaf页表PTE大小
    
    // NEW: Burst预取专用字段
    uint64_t  burst_start_addr = 0;         // Burst DDR读起始地址
    uint32_t  burst_size = 0;               // Burst DDR读大小(字节)
    bool      prefetch_burst_pending = false;  // Burst DDR请求是否未完成
    bool      combined_burst_ready = false;     // 合并Burst已完成DDR读(leaf+prefetch在同一次读中)

    // [STAT] DDR访问日志 - 记录每次DDR访问的类型和信息
    // type: 0=VS_non_leaf, 1=VS_leaf, 2=VS_leaf+burst, 3=GS_implicit, 4=GS_explicit, 5=AD_update, 6=Bare_explicit, 7=Burst_wait
    uint8_t  ddr_log_type[8] = {0};
    uint8_t  ddr_log_level[8] = {0};      // Page table level (0,1,2)
    uint64_t ddr_log_addr[8] = {0};       // DDR访问地址
    uint32_t ddr_log_size[8] = {0};       // DDR访问大小(字节)
    uint8_t  ddr_log_count = 0;           // 当前日志条目数

    // [STAT需求3] 两阶段DDR访问分解计数(在DDR响应侧按walk_phase累加, 不受ddr_log 8条上限约束)
    uint32_t vs_stage_ddr_reads = 0;      // 第一阶段(iova->gpa, PTW_VS_WALK)DDR读次数
    uint32_t gs_stage_ddr_reads = 0;      // 第二阶段(gpa->spa, GS_IMPLICIT+GS_EXPLICIT)DDR读次数
    uint32_t ad_update_ddr_writes = 0;    // AD_UPDATE阶段DDR访问次数

    // [MSI] MSI地址识别结果 (msi_id_check命中时填充, 供MSIPTW/MSIPT Cache使用)
    uint64_t msi_index = 0;             // interrupt file number I = extract(A>>12, mask)

    // [MSI] Walker前置查询发起标志: configure_and_route发起前置查询时置true;
    // MSI使能设备不发起前置查询, PTW据此走同步查询分支(避免死等前置结果)
    bool     walker_front_requested = false;

    walk_context_t() = default;
};

// ===================== Unified Task Context =====================
struct iommu_task_t {
    // Identity fields (from PayloadExtention)
    uint32_t task_id;
    uint32_t device_id;
    uint32_t process_id;
    uint8_t pid_valid;
    uint64_t iova;
    uint32_t length;
    addr_type_t at;
    uint8_t exec_req;
    uint8_t priv_req;
    uint8_t no_write;
    uint8_t is_cxl_dev;
    uint8_t read_writeAMO;
    sc_time timestamp;

    // Classification fields (Parser)
    uint8_t TTYP;
    uint8_t is_read;
    uint8_t is_write;
    uint8_t is_exec;
    uint8_t priv;
    uint8_t SUM;
    uint8_t DDI[3];

    // Context fields (Collector)
    device_context_t DC;
    process_context_t PC;
    iosatp_t iosatp;
    iohgatp_t iohgatp;
    uint8_t PSCV;
    uint8_t GV;
    uint32_t PSCID;
    uint32_t GSCID;
    uint32_t DID;
    uint32_t PID;
    uint8_t PV;
    uint8_t DTF;
    uint8_t check_access_perms;
    uint8_t SXL;
    uint8_t SADE;
    uint8_t GADE;

    // Translation result fields (PTW/Forwarder)
    uint64_t pa;
    uint64_t gpa;
    uint64_t page_sz;
    uint64_t gst_page_sz;
    spte_t vs_pte;
    gpte_t g_pte;
    uint8_t is_msi;
    uint8_t is_mrif;
    uint32_t mrif_nid;
    uint64_t dest_mrif_addr;
    uint32_t cause;
    uint64_t iotval;
    uint64_t iotval2;
    uint8_t is_bare_translation;
    uint8_t is_b_transport;         // 1 if task created via b_transport (don't delete in forwarder)
    uint8_t is_ctrl;                // [场景13] 1=SQ/CQ/MSI控制包(不计入IOPS), 0=Data数据包

    // NEW: 去重模块相关
    uint8_t dedup_head_index = 0xFF;  // Buffer任务链头指针（0-255，0xFF=无效）

    // Pipeline state
    task_state_t state;
    uint8_t dc_valid;
    uint8_t dc_hit;
    uint8_t pc_valid;
    uint8_t pc_hit;
    uint8_t need_pc;
    tlm::tlm_generic_payload* tlm_trans_ptr;

    // Walker context
    walk_context_t walk_ctx;

    // Constructor
    iommu_task_t()
        : task_id(0), device_id(0), process_id(0), pid_valid(0),
          iova(0), length(0), at(ADDR_TYPE_UNTRANSLATED),
          exec_req(0), priv_req(0), no_write(0), is_cxl_dev(0),
          read_writeAMO(0), timestamp(),
          TTYP(0), is_read(0), is_write(0), is_exec(0), priv(0), SUM(0),
          DDI{0},
          DC{}, PC{}, iosatp{}, iohgatp{},
          PSCV(0), GV(0), PSCID(0), GSCID(0), DID(0), PID(0),
          PV(0), DTF(0), check_access_perms(0), SXL(0), SADE(0), GADE(0),
          pa(0), gpa(0), page_sz(0), gst_page_sz(0),
          vs_pte{}, g_pte{},
          is_msi(0), is_mrif(0), mrif_nid(0), dest_mrif_addr(0),
          cause(0), iotval(0), iotval2(0),
          is_bare_translation(0), is_b_transport(0), is_ctrl(0),
          dedup_head_index(0xFF),
          state(TASK_INIT), dc_valid(0), dc_hit(0), pc_valid(0), pc_hit(0),
          need_pc(0), tlm_trans_ptr(nullptr), walk_ctx() {}

};

// ===================== Collector Entry =====================
struct collector_entry_t {
    iommu_task_t* task;
    bool parser_arrived;
    bool dc_done;
    bool pc_done;
    bool need_pc;

    collector_entry_t() : task(nullptr), parser_arrived(false),
                          dc_done(false), pc_done(false), need_pc(false) {}
};

// ===================== DDR Request Entry =====================
struct ddr_req_entry_t {
    uint32_t task_id;
    uint64_t addr;
    uint32_t size;
    bool is_write;
    uint8_t write_data[64];
    double submit_time_ns;  // [STAT] DDR请求提交时间戳(ns)

    ddr_req_entry_t() : task_id(0), addr(0), size(0), is_write(false), submit_time_ns(0.0) {
        memset(write_data, 0, sizeof(write_data));
    }
};

// ===================== DDR Response Entry =====================
struct ddr_rsp_entry_t {
    uint32_t task_id;
    uint8_t data[WALK_CTX_READ_BUF_SIZE];  // 支持 DC/PC 读取及 combined burst
    uint32_t data_length;
    bool error;
    double submit_time_ns;  // [STAT] DDR请求提交时间戳(ns)，从req传递到rsp

    ddr_rsp_entry_t() : task_id(0), data_length(0), error(false), submit_time_ns(0.0) {
        memset(data, 0, sizeof(data));
    }
};

// ===================== DDR Pending Queue Entry =====================
struct ddr_pending_entry_t {
    uint32_t task_id;
    uint8_t source_module;      // 0=XDTW, 1=PTW, 2=MSIPTW, 3=CTRL_PATH
    uint64_t addr;
    uint32_t size;
    tlm::tlm_generic_payload* trans_ptr;
    double submit_time_ns;  // [STAT] DDR请求提交时间戳(ns)
    bool is_write = false;  // [STAT] DDR读/写标记: 响应时据此分离读写outstanding--

    ddr_pending_entry_t() : task_id(0), source_module(0), addr(0),
                            size(0), trans_ptr(nullptr), submit_time_ns(0.0) {}
};

// ===================== Control Path DDR Request =====================
struct ctrl_path_ddr_req_t {
    uint64_t addr;
    uint32_t size;
    bool is_write;
    uint8_t write_data[64];

    ctrl_path_ddr_req_t() : addr(0), size(0), is_write(false) {
        memset(write_data, 0, sizeof(write_data));
    }
};

// ===================== sc_fifo required operator<< overloads =====================
inline std::ostream& operator<<(std::ostream& os, const ddr_req_entry_t& e) {
    os << "ddr_req{id=" << e.task_id << ",addr=0x" << std::hex << e.addr << std::dec
       << ",sz=" << e.size << ",wr=" << e.is_write << "}";
    return os;
}
inline std::ostream& operator<<(std::ostream& os, const ddr_rsp_entry_t& e) {
    os << "ddr_rsp{id=" << e.task_id << ",len=" << e.data_length
       << ",err=" << e.error << "}";
    return os;
}
inline std::ostream& operator<<(std::ostream& os, const ctrl_path_ddr_req_t& e) {
    os << "ctrl_req{addr=0x" << std::hex << e.addr << std::dec
       << ",sz=" << e.size << ",wr=" << e.is_write << "}";
    return os;
}

// ===================== Source Module Defines =====================
#define DDR_SRC_XDTW      0
#define DDR_SRC_PTW        1
#define DDR_SRC_MSIPTW     2
#define DDR_SRC_CTRL_PATH  3
#define DDR_SRC_MSI_MRIF   4  // [MSI] MRIF pending word写(响应静默丢弃)

// ===================== Processing Delay Params =====================
static const int XDTW_COMPUTE_DELAY  = 1;  // ns
static const int XDTW_PARSE_DELAY    = 1;  // ns
static const int PTW_COMPUTE_DELAY   = 1;  // ns
static const int PTW_PARSE_DELAY     = 1;  // ns
static const int MSIPTW_COMPUTE_DELAY = 1; // ns
static const int MSIPTW_PARSE_DELAY   = 1; // ns

#endif // __IOMMU_TASK_HH__
