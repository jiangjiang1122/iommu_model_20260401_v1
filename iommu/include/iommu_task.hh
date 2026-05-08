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
    PTW_AD_UPDATE
};

// ===================== MSIPTW Walk Phase Enum =====================
enum msiptw_walk_phase_t {
    MSIPTW_READ_MSIPTE = 0
};

// ===================== Walk Context =====================
struct walk_context_t {
    walk_type_t walk_type;
    int walk_phase;             // xdtw_walk_phase_t / ptw_walk_phase_t / msiptw_walk_phase_t
    int8_t level;
    int8_t max_levels;
    uint64_t base_addr;
    uint16_t indexes[5];
    uint64_t read_addr;
    uint32_t read_size;
    uint8_t read_buf[64];
    uint8_t ptesize;

    // VS-stage walk context (for PTW)
    uint16_t vpn[5];
    int8_t vs_level;

    // G-stage walk context (for PTW nested walk)
    int8_t gs_level;
    uint64_t gs_base_addr;
    uint64_t gs_pte_addr;
    uint64_t pending_vs_pte_addr;
    uint16_t gs_vpn[5];

    // A/D bit update context
    spte_t ad_pte;
    uint64_t ad_pte_addr;

    // DDR read count for walk progress tracking
    uint32_t ddr_read_count;

    // Walker Cache intermediate results (for PTW)
    // 保存walk过程中的中间PPN，用于最后update walker cache
    struct {
        uint64_t ppn_level2;  // Level 2 PPN (PTWc_3)
        uint64_t ppn_level1;  // Level 1 PPN (PTWc_2)
        uint64_t ppn_level0;  // Level 0 PPN (PTWc_1)
        bool valid_level2;
        bool valid_level1;
        bool valid_level0;
    } walker_cache_entries;

    walk_context_t() {
        memset(this, 0, sizeof(walk_context_t));
    }
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
    iommu_task_t() {
        memset(this, 0, sizeof(iommu_task_t));
        state = TASK_INIT;
        vs_pte.raw = 0;
        g_pte.raw = 0;
        DC.tc.raw = 0;
        DC.iohgatp.raw = 0;
        DC.ta.raw = 0;
        DC.fsc.raw = 0;
        DC.msiptp.raw = 0;
        DC.msi_addr_mask.raw = 0;
        DC.msi_addr_pattern.raw = 0;
        DC.reserved = 0;
        PC.ta.raw = 0;
        PC.fsc.raw = 0;
        iosatp.raw = 0;
        iohgatp.raw = 0;
        at = ADDR_TYPE_UNTRANSLATED;
        tlm_trans_ptr = nullptr;
    }
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

    ddr_req_entry_t() : task_id(0), addr(0), size(0), is_write(false) {
        memset(write_data, 0, sizeof(write_data));
    }
};

// ===================== DDR Response Entry =====================
struct ddr_rsp_entry_t {
    uint32_t task_id;
    uint8_t data[64];
    uint32_t data_length;
    bool error;

    ddr_rsp_entry_t() : task_id(0), data_length(0), error(false) {
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

    ddr_pending_entry_t() : task_id(0), source_module(0), addr(0),
                            size(0), trans_ptr(nullptr) {}
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

// ===================== Processing Delay Params =====================
static const int XDTW_COMPUTE_DELAY  = 1;  // ns
static const int XDTW_PARSE_DELAY    = 1;  // ns
static const int PTW_COMPUTE_DELAY   = 1;  // ns
static const int PTW_PARSE_DELAY     = 1;  // ns
static const int MSIPTW_COMPUTE_DELAY = 1; // ns
static const int MSIPTW_PARSE_DELAY   = 1; // ns

#endif // __IOMMU_TASK_HH__
