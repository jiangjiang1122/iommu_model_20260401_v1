#ifndef IOMMU_CACHE_TYPES_H
#define IOMMU_CACHE_TYPES_H

#include <systemc.h>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>
#include <optional>

namespace iommu {

// ============================================================
// 基础类型定义
// ============================================================

using device_id_t  = uint32_t;  // 24bit: segment(8) + bus(8) + device(5) + function(3)
using process_id_t = uint32_t;  // 20bit
using gscid_t      = uint16_t;  // 16bit
using pscid_t      = uint32_t;  // 20bit
using iova_t       = uint64_t;  // 44bit (高位截断)
using spa_t        = uint64_t;  // 44bit 物理地址
using ppn_t        = uint64_t;  // PPN

// device_id 字段提取辅助函数
inline uint8_t  get_segment(device_id_t did)  { return (did >> 16) & 0xFF; }
inline uint8_t  get_bus(device_id_t did)      { return (did >> 8) & 0xFF; }
inline uint8_t  get_device(device_id_t did)   { return (did >> 3) & 0x1F; }
inline uint8_t  get_function(device_id_t did) { return did & 0x07; }
inline device_id_t make_device_id(uint8_t seg, uint8_t bus, uint8_t dev, uint8_t func) {
    return (static_cast<uint32_t>(seg) << 16) |
           (static_cast<uint32_t>(bus) << 8) |
           (static_cast<uint32_t>(dev & 0x1F) << 3) |
           (func & 0x07);
}

// iova 字段提取 (Sv48)
inline uint16_t get_vpn0(iova_t va) { return (va >> 12) & 0x1FF; }   // bits[20:12]
inline uint16_t get_vpn1(iova_t va) { return (va >> 21) & 0x1FF; }   // bits[29:21]
inline uint16_t get_vpn2(iova_t va) { return (va >> 30) & 0x1FF; }   // bits[38:30]
inline uint16_t get_vpn3(iova_t va) { return (va >> 39) & 0x1FF; }   // bits[47:39]

// 页大小枚举
enum class PageSize : uint8_t {
    PAGE_4K   = 0,  // 12bit offset
    PAGE_2M   = 1,  // 21bit offset
    PAGE_1G   = 2,  // 30bit offset
    PAGE_512G = 3   // 39bit offset
};

inline uint64_t page_size_bytes(PageSize ps) {
    switch (ps) {
        case PageSize::PAGE_4K:   return 4096ULL;
        case PageSize::PAGE_2M:   return 2ULL * 1024 * 1024;
        case PageSize::PAGE_1G:   return 1ULL * 1024 * 1024 * 1024;
        case PageSize::PAGE_512G: return 512ULL * 1024 * 1024 * 1024;
        default: return 4096ULL;
    }
}

// 翻译阶段标志
enum class TransStage : uint8_t {
    STAGE1_ONLY = 0,
    STAGE2_ONLY = 1,
    STAGE1_AND_2 = 2
};

// ============================================================
// Cache 消息类型
// 说明：
// - lookup / update / invalidate / response 统一复用一个消息结构。
// - 为兼容现有代码，保留 `CacheRequest` / `CacheResponse` 等名字作为别名。
// ============================================================

enum class CacheMsgType : uint8_t {
    TRANSLATE = 0,
    DC_LOOKUP,
    PC_LOOKUP,
    PT_LOOKUP,
    WALKER_LOOKUP,
    MSI_LOOKUP,
    PREFETCH,
    DC_UPDATE,
    PC_UPDATE,
    PT_UPDATE,
    WALKER_UPDATE,
    MSI_UPDATE,
    DC_INVALIDATE,
    PC_INVALIDATE,
    PT_INVALIDATE,
    WALKER_INVALIDATE,
    MSI_INVALIDATE,
    CACHE_RESPONSE,
    CACHE_UPDATE_RESPONSE,
    CACHE_INVALIDATE_RESPONSE
};

enum class WalkerUpdateKind : uint8_t {
    PTWC_1_2_3,
    PTWC_2_3,
    PTWC_3
};

struct CacheMessage;
using CacheReqType = CacheMsgType;
using CacheUpdateType = CacheMsgType;
using CacheInvalidateType = CacheMsgType;
using CacheRequest = CacheMessage;
using CacheResponse = CacheMessage;

// ============================================================
// 失效命令类型
// ============================================================

enum class InvalidCmdType : uint8_t {
    GLOBAL_INVAL,       // 全局失效，所有 cache 全清
    IODIR_INVAL_DDT,    // DC失效
    IODIR_INVAL_PDT,    // PC失效
    IOTINVAL_VMA,       // 第一阶段页表失效
    IOTINVAL_GVMA       // 第二阶段页表失效
};

enum class CacheInvalidateMode : uint8_t {
    PRECISE,    // hash 定位 set 后比较 way
    SCAN,       // 逐 set 扫描比较
    GLOBAL      // 全表清空
};

struct InvalidationCmd {
    InvalidCmdType  cmd_type = InvalidCmdType::IODIR_INVAL_DDT;
    CacheInvalidateMode invalidate_mode = CacheInvalidateMode::PRECISE;
    device_id_t     device_id   = 0;
    process_id_t    process_id  = 0;
    gscid_t         gscid       = 0;
    pscid_t         pscid       = 0;
    iova_t          iova        = 0;
    bool            has_device_id  = false;
    bool            has_process_id = false;
    bool            has_gscid      = false;
    bool            has_pscid      = false;
    bool            has_iova       = false;
};

// ============================================================
// 参考模型对齐的 DC/PC/PT 数据结构
// 说明：
// - `iommu_model_20260401_v1` 中 DC/PC/PT cache 使用更贴近规范的位域结构。
// - 当前 `iommu_cache_sim` 保留独立 tag 与 replacement 组织方式，
//   但 data 载荷改为优先贴合参考模型结构。
// - 为降低迁移成本，下面同时提供少量桥接辅助函数，供子系统与测试读取。
// ============================================================

union tc_t {
    struct {
        uint64_t V:1;
        uint64_t EN_ATS:1;
        uint64_t EN_PRI:1;
        uint64_t T2GPA:1;
        uint64_t DTF:1;
        uint64_t PDTV:1;
        uint64_t PRPR:1;
        uint64_t GADE:1;
        uint64_t SADE:1;
        uint64_t DPE:1;
        uint64_t SBE:1;
        uint64_t SXL:1;
        uint64_t reserved0:12;
        uint64_t custom:8;
        uint64_t reserved1:32;
    };
    uint64_t raw = 0;
};

union iohgatp_t {
    struct {
        uint64_t PPN:44;
        uint64_t GSCID:16;
        uint64_t MODE:4;
    };
    uint64_t raw = 0;
};

union ta_t {
    struct {
        uint64_t reserved0:12;
        uint64_t PSCID:20;
        uint64_t reserved1:8;
        uint64_t rcid:12;
        uint64_t mcid:12;
    };
    uint64_t raw = 0;
};

union iosatp_t {
    struct {
        uint64_t PPN:44;
        uint64_t reserved:16;
        uint64_t MODE:4;
    };
    uint64_t raw = 0;
};

union fsc_t {
    iosatp_t iosatp;
    struct {
        uint64_t PPN:44;
        uint64_t reserved:16;
        uint64_t MODE:4;
    } pdtp;
    uint64_t raw;

    fsc_t() : raw(0) {}
};

union msiptp_t {
    struct {
        uint64_t MODE:4;
        uint64_t reserved0:8;
        uint64_t PPN:44;
        uint64_t reserved1:8;
    };
    uint64_t raw = 0;
};

union msi_addr_mask_t {
    struct {
        uint64_t mask:52;
        uint64_t reserved:12;
    };
    uint64_t raw = 0;
};

union msi_addr_pattern_t {
    struct {
        uint64_t pattern:52;
        uint64_t reserved:12;
    };
    uint64_t raw = 0;
};

typedef struct {
    tc_t               tc{};
    iohgatp_t          iohgatp{};
    ta_t               ta{};
    fsc_t              fsc{};
    msiptp_t           msiptp{};
    msi_addr_mask_t    msi_addr_mask{};
    msi_addr_pattern_t msi_addr_pattern{};
    uint64_t           reserved = 0;
} device_context_t;

union pc_ta_t {
    struct {
        uint64_t V:1;
        uint64_t ENS:1;
        uint64_t SUM:1;
        uint64_t reserved0:9;
        uint64_t PSCID:20;
        uint64_t reserved1:32;
    };
    uint64_t raw = 0;
};

union pc_fsc_t {
    iosatp_t iosatp;
    uint64_t raw;

    pc_fsc_t() : raw(0) {}
};

typedef struct {
    pc_ta_t  ta{};
    pc_fsc_t fsc{};
} process_context_t;

struct MSIPTData {
    bool        valid       = false;
    uint64_t    pte         = 0;    // MSI page table entry
    spa_t       spa         = 0;
    bool        mrif_mode   = false;
};

union spte_t {
    struct {
        uint64_t V:1;
        uint64_t R:1;
        uint64_t W:1;
        uint64_t X:1;
        uint64_t U:1;
        uint64_t G:1;
        uint64_t A:1;
        uint64_t D:1;
        uint64_t RSW:2;
        uint64_t PPN:44;
        uint64_t reserved:5;
        uint64_t rsw60t59b:2;
        uint64_t PBMT:2;
        uint64_t N:1;
    };
    uint64_t raw = 0;
};

union gpte_t {
    struct {
        uint64_t V:1;
        uint64_t R:1;
        uint64_t W:1;
        uint64_t X:1;
        uint64_t U:1;
        uint64_t G:1;
        uint64_t A:1;
        uint64_t D:1;
        uint64_t RSW:2;
        uint64_t PPN:44;
        uint64_t reserved:5;
        uint64_t rsw60t59b:2;
        uint64_t PBMT:2;
        uint64_t N:1;
    };
    uint64_t raw = 0;
};

union pt_reserved_t {
    struct {
        uint32_t valid:1;
        uint32_t trans_type:2;
        uint32_t input_page_size:2;
        uint32_t result_page_size:2;
        uint32_t iova_is_va:1;
        uint32_t sv48:1;
        uint32_t gstage_x4:1;
        uint32_t replacement_info:2;
        uint32_t reserved:16;
    };
    uint32_t raw = 0;
};

// PT Cache 原始 payload：缓存 `spte_t/gpte_t + reserved`，
// 由 cache 外部 IOMMU 逻辑根据 `trans_type` 决定如何解释并转换为 `tlb_t`。
typedef struct {
    spte_t        vs_pte{};
    gpte_t        g_pte{};
    pt_reserved_t reserved{};
} pt_cache_data_t;

// `tlb_t` 保留为 cache 外部逻辑使用的翻译结果结构。
typedef struct {
    uint64_t    vpn         = 0;
    uint8_t     GV          = 0;
    uint8_t     PSCV        = 0;
    uint32_t    GSCID       = 0;
    uint32_t    PSCID       = 0;
    uint8_t     VS_R        = 0;
    uint8_t     VS_W        = 0;
    uint8_t     VS_X        = 0;
    uint8_t     PBMT        = 0;
    uint8_t     G           = 0;
    uint8_t     U           = 0;
    uint8_t     VS_D        = 0;
    uint8_t     G_R         = 0;
    uint8_t     G_W         = 0;
    uint8_t     G_X         = 0;
    uint8_t     G_D         = 0;
    uint64_t    PPN         = 0;
    uint8_t     S           = 0;
    uint32_t    lru         = 0;
    uint8_t     valid       = 0;
    uint8_t     IS_MSI      = 0;
} tlb_t;

using DCData = device_context_t;
using PCData = process_context_t;
using PTData = pt_cache_data_t;

inline bool dc_valid(const DCData& data) { return data.tc.V != 0; }
inline gscid_t dc_gscid(const DCData& data) {
    return static_cast<gscid_t>(data.iohgatp.GSCID);
}
inline pscid_t dc_pscid(const DCData& data) {
    return static_cast<pscid_t>(data.ta.PSCID);
}
inline bool dc_en_ats(const DCData& data) { return data.tc.EN_ATS != 0; }
inline bool dc_en_pri(const DCData& data) { return data.tc.EN_PRI != 0; }
inline bool dc_t2gpa(const DCData& data) { return data.tc.T2GPA != 0; }
inline bool dc_pdtv(const DCData& data) { return data.tc.PDTV != 0; }
inline TransStage dc_stage(const DCData& data) {
    const bool has_s_stage = dc_pdtv(data) || data.fsc.iosatp.MODE != 0;
    const bool has_g_stage = data.iohgatp.MODE != 0;
    if (has_s_stage && has_g_stage) return TransStage::STAGE1_AND_2;
    if (has_s_stage) return TransStage::STAGE1_ONLY;
    if (has_g_stage) return TransStage::STAGE2_ONLY;
    return TransStage::STAGE1_AND_2;
}

inline bool pc_valid(const PCData& data) { return data.ta.V != 0; }
inline pscid_t pc_pscid(const PCData& data) {
    return static_cast<pscid_t>(data.ta.PSCID);
}

inline TransStage pt_stage(const PTData& data) {
    switch (data.reserved.trans_type) {
        case 0: return TransStage::STAGE1_ONLY;
        case 1: return TransStage::STAGE2_ONLY;
        case 2: return TransStage::STAGE1_AND_2;
        default: return TransStage::STAGE1_AND_2;
    }
}

inline PageSize pt_input_page_size(const PTData& data) {
    switch (data.reserved.input_page_size) {
        case 0: return PageSize::PAGE_4K;
        case 1: return PageSize::PAGE_2M;
        case 2: return PageSize::PAGE_1G;
        case 3: return PageSize::PAGE_512G;
        default: return PageSize::PAGE_4K;
    }
}

inline PageSize pt_page_size(const PTData& data) {
    switch (data.reserved.result_page_size) {
        case 0: return PageSize::PAGE_4K;
        case 1: return PageSize::PAGE_2M;
        case 2: return PageSize::PAGE_1G;
        case 3: return PageSize::PAGE_512G;
        default: return PageSize::PAGE_4K;
    }
}

inline uint64_t pt_page_size_bytes(const PTData& data) {
    return page_size_bytes(pt_page_size(data));
}

inline bool pt_valid(const PTData& data) { return data.reserved.valid != 0; }
inline bool pt_iova_is_va(const PTData& data) { return data.reserved.iova_is_va != 0; }
inline bool pt_sv48_mode(const PTData& data) { return data.reserved.sv48 != 0; }
inline bool pt_gstage_x4_mode(const PTData& data) { return data.reserved.gstage_x4 != 0; }
inline bool pt_from_prefetch(const PTData&) { return false; }

inline DCData make_dc_data(gscid_t gscid, bool en_ats = false,
                           bool en_pri = false, bool t2gpa = false,
                           bool pdtv = false,
                           TransStage stage = TransStage::STAGE1_AND_2) {
    DCData data;
    data.tc.V = 1;
    data.tc.EN_ATS = en_ats ? 1 : 0;
    data.tc.EN_PRI = en_pri ? 1 : 0;
    data.tc.T2GPA = t2gpa ? 1 : 0;
    data.tc.PDTV = pdtv ? 1 : 0;
    data.iohgatp.GSCID = gscid;

    switch (stage) {
        case TransStage::STAGE1_ONLY:
            data.iohgatp.MODE = 0;
            data.fsc.iosatp.MODE = 8;
            break;
        case TransStage::STAGE2_ONLY:
            data.iohgatp.MODE = 8;
            data.fsc.iosatp.MODE = 0;
            break;
        case TransStage::STAGE1_AND_2:
        default:
            data.iohgatp.MODE = 8;
            data.fsc.iosatp.MODE = 8;
            break;
    }
    return data;
}

inline PCData make_pc_data(pscid_t pscid, bool ens = true, bool sum = false) {
    PCData data;
    data.ta.V = 1;
    data.ta.ENS = ens ? 1 : 0;
    data.ta.SUM = sum ? 1 : 0;
    data.ta.PSCID = pscid;
    data.fsc.iosatp.MODE = 8;
    return data;
}

inline PTData make_pt_data(spa_t spa, PageSize page_size,
                           uint8_t permissions = 0x07,
                           TransStage stage = TransStage::STAGE1_AND_2,
                           PageSize input_page_size = PageSize::PAGE_4K,
                           bool iova_is_va = true,
                           bool sv48 = true,
                           bool gstage_x4 = false) {
    PTData data;
    data.reserved.valid = 1;
    data.reserved.trans_type = static_cast<uint32_t>(stage);
    data.reserved.input_page_size = static_cast<uint32_t>(input_page_size);
    data.reserved.result_page_size = static_cast<uint32_t>(page_size);
    data.reserved.iova_is_va = iova_is_va ? 1U : 0U;
    data.reserved.sv48 = sv48 ? 1U : 0U;
    data.reserved.gstage_x4 = gstage_x4 ? 1U : 0U;

    data.vs_pte.V = 1;
    data.vs_pte.R = (permissions & 0x1U) ? 1U : 0U;
    data.vs_pte.W = (permissions & 0x2U) ? 1U : 0U;
    data.vs_pte.X = (permissions & 0x4U) ? 1U : 0U;
    data.vs_pte.A = 1;
    data.vs_pte.D = data.vs_pte.W;

    data.g_pte.V = 1;
    data.g_pte.R = data.vs_pte.R;
    data.g_pte.W = data.vs_pte.W;
    data.g_pte.X = data.vs_pte.X;
    data.g_pte.A = 1;
    data.g_pte.D = data.g_pte.W;

    const spa_t base_spa = spa & ~(page_size_bytes(page_size) - 1ULL);
    const uint64_t ppn = base_spa >> 12;

    switch (page_size) {
        case PageSize::PAGE_4K:
            data.vs_pte.PPN = ppn;
            data.g_pte.PPN = ppn;
            break;
        case PageSize::PAGE_2M:
            data.vs_pte.PPN = ppn;
            data.g_pte.PPN = ppn;
            break;
        case PageSize::PAGE_1G:
            data.vs_pte.PPN = ppn;
            data.g_pte.PPN = ppn;
            break;
        case PageSize::PAGE_512G:
            data.vs_pte.PPN = ppn;
            data.g_pte.PPN = ppn;
            break;
    }
    return data;
}

// Walker Cache 中间结果。
// 说明：
// - `gscid` / `pscid` / `walker_level` 由外层 `CacheMessage` 携带，不在这里重复。
// - `reserved` 对齐架构定义中的 8bit 保留区，其中已知低 5bit 语义如下：
//   bit0: valid
//   bit1: VA/PA 类型标志
//   bit2: 单阶段/双阶段来源标志
//   bit3: Sv39/Sv48 标志
//   bit4: x4 模式标志（Sv39/Sv48 与 Sv39x4/Sv48x4 区分）
union walker_reserved_t {
    struct {
        uint8_t valid:1;
        uint8_t va_pa_flag:1;
        uint8_t stage_flag:1;
        uint8_t sv48_flag:1;
        uint8_t x4_mode_flag:1;
        uint8_t reserved:3;
    };
    uint8_t raw = 0;
};

struct WalkerData {
    walker_reserved_t reserved{};
    ppn_t             next_ppn = 0;
};

inline bool walker_valid(const WalkerData& data) { return data.reserved.valid != 0; }
inline bool walker_va_pa_flag(const WalkerData& data) { return data.reserved.va_pa_flag != 0; }
inline bool walker_stage_flag(const WalkerData& data) { return data.reserved.stage_flag != 0; }
inline bool walker_sv48_flag(const WalkerData& data) { return data.reserved.sv48_flag != 0; }
inline bool walker_x4_mode_flag(const WalkerData& data) { return data.reserved.x4_mode_flag != 0; }

inline void walker_set_valid(WalkerData& data, bool valid) {
    data.reserved.valid = valid ? 1U : 0U;
}

inline WalkerData make_walker_data(ppn_t next_ppn,
                                   bool valid = true,
                                   bool is_va = true,
                                   bool is_stage1_and_2 = true,
                                   bool is_sv48 = true,
                                   bool is_x4_mode = false) {
    WalkerData data;
    data.next_ppn = next_ppn;
    data.reserved.valid = valid ? 1U : 0U;
    data.reserved.va_pa_flag = is_va ? 1U : 0U;
    data.reserved.stage_flag = is_stage1_and_2 ? 1U : 0U;
    data.reserved.sv48_flag = is_sv48 ? 1U : 0U;
    data.reserved.x4_mode_flag = is_x4_mode ? 1U : 0U;
    return data;
}

struct CacheMessage {
    // 统一消息类别。  输入/输出共用
    // 同一底层字段复用为 lookup/update/invalidate 三种接口名字。
    union {
        CacheMsgType msg_type; //cache外部模块使用，通用消息类型接口
        CacheMsgType req_type; //cache内部使用，请求类型接口
        CacheMsgType update_type; //cache内部使用，更新类型接口    
        CacheMsgType invalidate_type; //cache内部使用，失效类型接口
    };

    // 通用元信息：任务标记、输入/输出共用
    uint64_t        task_id = 0;

    // 路由键：不同 cache 按需使用其中一部分字段做查找/更新/失效定位。 查找/更新/失效输入
    device_id_t     device_id = 0;
    process_id_t    process_id = 0;
    uint32_t        msi_index = 0; // 暂定
    gscid_t         gscid = 0;
    pscid_t         pscid = 0;
    iova_t          iova = 0;

    // 外部 IOMMU 模块在 update 时同步该条目是否来自预取，用于统计预取命中率。 更新输入
    bool            from_prefetch = false;

    // PT lookup 参与 set 内 way compare 的上下文字段。 查找输入
    // `stage` 已表示一阶段/二阶段/两阶段转换类型。
    TransStage      stage = TransStage::STAGE1_AND_2;
    bool            pt_sv48 = true;
    bool            pt_gstage_x4 = false;

    // Walker lookup 参与 set 内 way compare 的上下文字段。 查找输入
    bool            walker_addr_is_va = true;
    bool            walker_from_two_stage = true;
    bool            walker_sv48 = true;
    bool            walker_x4_mode = false;

    // Walker update 请求类型和各 level 独立 payload。 更新输入
    WalkerUpdateKind walker_update_kind = WalkerUpdateKind::PTWC_3;
    WalkerData      walker_data_ptwc1{};
    WalkerData      walker_data_ptwc2{};
    WalkerData      walker_data_ptwc3{};

    // invalidate 相关控制字段。 失效输入
    InvalidCmdType  cmd_type = InvalidCmdType::IODIR_INVAL_DDT;
    CacheInvalidateMode invalidate_mode = CacheInvalidateMode::PRECISE;
    bool            has_device_id = false;
    bool            has_process_id = false;
    bool            has_gscid = false;
    bool            has_pscid = false;
    bool            has_iova = false;


    // 各 cache 的 payload 区。 输入输出共用
    // 统一消息体会带上所有可能的数据载荷，具体由 msg_type 决定哪一块有效。
    // Walker: lookup 响应中表示实际命中 level；update 目标由 walker_update_kind
    // 与 walker_data_ptwc1/2/3 表达。
    // Walker lookup / update / invalidate 请求不使用该字段作为输入。
    bool            hit = false;
    uint8_t         walker_level = 0;
    device_context_t dc_data{};
    process_context_t pc_data{};
    PTData           pt_data{};
    WalkerData      walker_data{};
    MSIPTData       msi_data{};

    // invalidate 实际影响条目数。 cache内部使用
    uint32_t        affected_entries = 0;

    // 时序信息：用于性能建模和统计。cache内部使用
    sc_time         timestamp = SC_ZERO_TIME;
    sc_time         latency = SC_ZERO_TIME;

    CacheMessage() : msg_type(CacheMsgType::TRANSLATE) {}
};

inline std::ostream& operator<<(std::ostream& os, const CacheMessage& msg) {
    os << "CacheMessage{type=" << static_cast<unsigned>(msg.msg_type)
       << ", task_id=" << msg.task_id
       << ", device_id=" << msg.device_id
       << ", process_id=" << msg.process_id
       << ", gscid=" << msg.gscid
       << ", pscid=" << msg.pscid
       << ", iova=0x" << std::hex << msg.iova
       << ", hit=" << std::dec << msg.hit
       << "}";
    return os;
}

using CacheUpdateRequest = CacheMessage;
using CacheUpdateResponse = CacheMessage;
using CacheInvalidateRequest = CacheMessage;
using CacheInvalidateResponse = CacheMessage;

// ============================================================
// 缓存配置结构体
// ============================================================

struct CacheConfig {
    uint32_t    num_sets    = 64;
    uint32_t    num_ways    = 4;
    std::string replacement = "plru";
    uint32_t    srrip_m_bits = 2;
    uint32_t    arbiter_latency_cycles = 0;
    uint32_t    hash_latency_cycles = 1;
    uint32_t    read_set_latency_cycles = 1;
    uint32_t    compare_latency_cycles = 1;
    uint32_t    update_way_select_latency_cycles = 1;
    // Fill/update 计算index延时：三种路径各有不同拍数
    uint32_t    fill_compute_index_hit_cycles = 1;          // 直接命中（tag已存在）
    uint32_t    fill_compute_index_invalid_cycles = 2;      // 有 invalid way 可以填充
    uint32_t    fill_compute_index_replacement_cycles = 4;  // 需要替换算法
    uint32_t    write_way_latency_cycles = 1;
    uint32_t    invalidation_compare_per_way_cycles = 1;  // 失效时逐 way 串行比较，每 way 耗时
};

struct StatsConfig {
    std::string output_file = "cache_sim.log";  // 统一输出文件（运行日志 + task trace + 统计汇总）
    bool        enable_latency_histogram = true;
    bool        enable_task_trace = false;
    std::string task_trace_level = "off";  // off/basic/detail
    double      histogram_bin_width_ns = 1.0;
};

struct GlobalConfig {
    double      clock_period_ns = 1.0;
    uint32_t    random_seed = 42;
    CacheConfig dc_cache;
    CacheConfig pc_cache;
    CacheConfig msipt_cache;
    CacheConfig pt_cache;
    // Walker Cache 3个子表各自的配置
    uint32_t    walker_base_sets = 64;
    CacheConfig walker_ptw_c1;
    CacheConfig walker_ptw_c2;
    CacheConfig walker_ptw_c3;
    StatsConfig statistics;
};

} // namespace iommu

#endif // IOMMU_CACHE_TYPES_H
