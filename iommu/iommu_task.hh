#ifndef __IOMMU_TASK_HH__
#define __IOMMU_TASK_HH__

#include "systemc.h"
#include "tlm.h"
#include "iommu_data_structures.hh"
#include "iommu_perf_params.hh"
#include <map>
#include <queue>

using namespace std;

// ==================== 任务状态枚举 ====================
enum task_state_t {
    // Parser 阶段
    TASK_PARSE_DONE,           // Parser 完成解析，已发起 DC+PC 查询

    // DC/PC Cache 查询结果
    TASK_DC_HIT,               // DC Cache 命中
    TASK_DC_MISS,              // DC Cache 未命中
    TASK_PC_HIT,               // PC Cache 命中
    TASK_PC_MISS,              // PC Cache 未命中

    // xDTW 完成
    TASK_DC_WALK_DONE,         // xDTW 完成 DDT walk，得到 DC
    TASK_PC_WALK_DONE,         // xDTW 完成 PDT walk，得到 PC

    // PT Cache 查询
    TASK_TLB_HIT,              // PT Cache(TLB) 命中
    TASK_TLB_MISS,             // PT Cache(TLB) 未命中

    // PTW 完成
    TASK_PT_WALK_DONE,         // PTW 完成页表 walk

    // MSIPT Cache 查询
    TASK_MSIPT_HIT,            // MSIPT Cache 命中
    TASK_MSIPT_MISS,           // MSIPT Cache 未命中
    TASK_MSIPT_WALK_DONE,      // MSIPTW 完成 MSI 页表 walk

    // 最终状态
    TASK_FORWARD,              // 地址翻译完成，准备转发
    TASK_FAULT,                // 出错
    TASK_DONE                  // 完成
};

// ==================== Walk 类型枚举 ====================
enum walk_type_t {
    WALK_DDT,                  // DDT radix tree walk
    WALK_PDT,                  // PDT radix tree walk
    WALK_VS_PT,                // VS-stage 页表 walk
    WALK_G_PT,                 // G-stage 页表 walk
    WALK_G_PT_IMPLICIT,        // G-stage 隐式翻译 (VS PTE 地址翻译)
    WALK_MSI_PT,               // MSI 页表 walk
    WALK_AD_UPDATE,            // A/D 位原子更新（blocking）
    WALK_FQ_WRITE,             // FQ 写入
    WALK_PQ_WRITE,             // PQ 写入
    WALK_CQ_READ,              // CQ 读取
    WALK_MRIF_WRITE,           // MRIF 写入（blocking 原子 OR）
    WALK_DDT_DC_READ,          // DDT后读取DC内容
    WALK_PDT_PC_READ           // PDT后读取PC内容
};

// ==================== Walk 上下文 - DDR 异步回调恢复用 ====================
struct walk_context_t {
    walk_type_t walk_type;
    int8_t      level;          // 当前 walk 层级
    uint8_t     max_levels;     // 总层级数
    uint64_t    base_addr;      // 当前层基地址
    uint16_t    index[4];       // DDI/PDI/VPN 索引数组
    uint8_t     pte_size;       // PTE 大小 (4 或 8 字节)
    uint64_t    read_addr;      // 当前 DDR 读取地址
    uint8_t     read_size;      // 当前 DDR 读取大小
    uint64_t    vs_a;           // VS walk 的当前地址 a
    uint64_t    g_a;            // G walk 的当前地址 a
    
    // 嵌套 G-stage 时保存的 VS walk 上下文
    uint64_t    saved_vs_ppn;
    uint8_t     saved_vs_level;
    uint64_t    saved_vs_iova;
    
    walk_context_t() {
        walk_type = WALK_DDT;
        level = 0;
        max_levels = 0;
        base_addr = 0;
        for (int i = 0; i < 4; i++) index[i] = 0;
        pte_size = 8;
        read_addr = 0;
        read_size = 8;
        vs_a = 0;
        g_a = 0;
        saved_vs_ppn = 0;
        saved_vs_level = 0;
        saved_vs_iova = 0;
    }
};

// ==================== 统一事务上下文 ====================
struct iommu_task_t {
    // === 任务标识 ===
    uint32_t    task_id;             // 全局唯一 ID（由 Parser 分配）
    sc_time     timestamp;           // 进入时间戳

    // === 原始请求（从 PayloadExtention 提取） ===
    uint32_t    device_id;
    uint32_t    process_id;
    uint8_t     pid_valid;
    uint8_t     exec_req, priv_req, no_write, is_cxl_dev;
    addr_type_t at;
    uint64_t    iova;
    uint32_t    length;
    uint8_t     read_writeAMO;
    tlm::tlm_generic_payload* original_trans; // 原始 TLM payload 指针

    // === 状态机 ===
    task_state_t state;
    uint8_t     TTYP;
    uint8_t     is_read, is_write, is_exec, priv;

    // === DC/PC 查询结果 ===
    device_context_t  DC;
    process_context_t PC;
    bool        dc_valid;            // DC 查询是否已返回
    bool        pc_valid;            // PC 查询是否已返回
    bool        need_pc;             // 是否需要 PC（由 Collector 根据 DC.tc.PDTV 判断）
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

    // === Walk 上下文 ===
    walk_context_t walk_ctx;         // 当前活跃 walk
    walk_context_t saved_vs_walk;    // 嵌套 G-stage 时保存的 VS walk

    // === DDR 请求跟踪 ===
    uint16_t    current_axi_id;      // 当前 DDR 请求的 AXI ID

    // === Walker Cache 命中信息 (V4 新增) ===
    int8_t      wc_hit_level;        // Walker Cache 命中的层级 (-1=未命中，1/2/3=PTWc_x 命中)
    uint64_t    wc_hit_ppn;          // Walker Cache 命中时的中间 PPN

    // === 构造函数 ===
    iommu_task_t() {
        task_id = 0;
        timestamp = sc_time(0, SC_NS);
        device_id = 0;
        process_id = 0;
        pid_valid = 0;
        exec_req = 0;
        priv_req = 0;
        no_write = 0;
        is_cxl_dev = 0;
        at = ADDR_TYPE_UNTRANSLATED;
        iova = 0;
        length = 0;
        read_writeAMO = 0;
        original_trans = nullptr;
        state = TASK_PARSE_DONE;
        TTYP = 0;
        is_read = 0;
        is_write = 0;
        is_exec = 0;
        priv = 0;
        dc_valid = false;
        pc_valid = false;
        need_pc = false;
        DTF = 0;
        PSCV = 0;
        GV = 0;
        PV = 0;
        SUM = 0;
        SXL = 0;
        GSCID = 0;
        PSCID = 0;
        DID = 0;
        PID = 0;
        iosatp.raw = 0;
        iohgatp.raw = 0;
        pa = 0;
        gpa = 0;
        page_sz = 0;
        gst_page_sz = 0;
        vs_pte.raw = 0;
        g_pte.raw = 0;
        is_msi = 0;
        is_mrif = 0;
        is_bare_mode = 0;
        mrif_nid = 0;
        dest_mrif_addr = 0;
        cause = 0;
        iotval = 0;
        iotval2 = 0;
        current_axi_id = 0;
        wc_hit_level = -1;
        wc_hit_ppn = 0;
    }
};

// ==================== DDR 响应结构 ====================
struct ddr_response_t {
    uint16_t    axi_id;
    walk_type_t walk_type;       // 请求类型，用于路由
    uint32_t    task_id;         // 关联的任务 ID
    uint8_t     status;          // 0=OK, 1=ERROR
    uint8_t     data[64];        // 读回数据
    uint32_t    data_size;

    ddr_response_t() {
        axi_id = 0;
        walk_type = WALK_DDT;
        task_id = 0;
        status = 0;
        memset(data, 0, 64);
        data_size = 0;
    }
};

// ==================== DDR 响应流输出运算符 ====================
inline std::ostream& operator<<(std::ostream& os, const ddr_response_t& rsp) {
    os << "ddr_response_t{axi_id=" << rsp.axi_id
       << ", walk_type=" << rsp.walk_type
       << ", task_id=" << rsp.task_id
       << ", status=" << rsp.status
       << ", data_size=" << rsp.data_size << "}";
    return os;
}

// ==================== AXI ID 分配器 ====================
class axi_id_allocator_t {
public:
    std::queue<uint16_t> free_ids;
    sc_event             id_freed_evt;
    uint16_t             max_ids;

    void init(uint16_t max) {
        max_ids = max;
        while (!free_ids.empty()) free_ids.pop();
        for (uint16_t i = 0; i < max; i++) {
            free_ids.push(i);
        }
    }

    uint16_t alloc_id() {
        if (free_ids.empty()) {
            // 等待 ID 释放
            wait(id_freed_evt);
        }
        uint16_t id = free_ids.front();
        free_ids.pop();
        return id;
    }

    void free_id(uint16_t id) {
        free_ids.push(id);
        id_freed_evt.notify();
    }
};

// ==================== Outstanding DDR 请求表条目 ====================
struct ddr_outstanding_entry_t {
    iommu_task_t*   task;
    walk_type_t     walk_type;
    uint64_t        expected_addr;
    uint8_t         expected_size;
};

// ==================== Cache Invalidation 命令类型 ====================
enum cache_inv_type_t {
    INV_DDT,            // IOTINVAL.DDT - 失效 DC Cache + 关联的 PT/Walker Cache
    INV_VMA,            // IOTINVAL.VMA - 失效 PT Cache + Walker Cache
    INV_IOFENCE,        // IOFENCE.C - 等待所有 in-flight 翻译完成
    INV_ATS_INVAL,      // ATS.INVAL - 向 RP 发送 ATS Invalidation Request
    INV_ATS_PRGR,       // ATS.PRGR - 向 RP 发送 Page Request Group Response
};

// ==================== Cache Invalidation 命令结构 ====================
struct cache_inv_cmd_t {
    cache_inv_type_t type;
    uint32_t    device_id;       // DDT 失效时使用
    uint32_t    process_id;      // VMA 失效时使用
    uint16_t    gscid;           // 用于 PT/Walker Cache 失效
    uint32_t    pscid;
    uint64_t    addr;            // VMA 失效时的地址
    bool        gv;              // gscid 有效
    bool        pscidv;          // pscid 有效
    bool        av;              // addr 有效
    uint8_t     func3;           // IOFENCE 功能码
    // ATS Msg 相关字段
    uint32_t    dseg;
    uint32_t    rid;
    uint64_t    payload;

    cache_inv_cmd_t() {
        type = INV_DDT;
        device_id = 0;
        process_id = 0;
        gscid = 0;
        pscid = 0;
        addr = 0;
        gv = false;
        pscidv = false;
        av = false;
        func3 = 0;
        dseg = 0;
        rid = 0;
        payload = 0;
    }
};

// ==================== Cache Invalidation 命令流输出运算符 ====================
inline std::ostream& operator<<(std::ostream& os, const cache_inv_cmd_t& cmd) {
    os << "cache_inv_cmd_t{type=" << cmd.type 
       << ", device_id=" << cmd.device_id
       << ", process_id=" << cmd.process_id
       << ", gscid=" << cmd.gscid
       << ", pscid=" << cmd.pscid
       << ", addr=0x" << std::hex << cmd.addr << std::dec
       << ", gv=" << cmd.gv
       << ", pscidv=" << cmd.pscidv
       << ", av=" << cmd.av
       << ", func3=" << cmd.func3
       << ", dseg=" << cmd.dseg
       << ", rid=" << cmd.rid
       << ", payload=0x" << std::hex << cmd.payload << std::dec << "}";
    return os;
}

// ==================== Walker Cache 数据结构 ====================

// PTWc_1: 缓存 VPN[3] 级中间结果（Sv48 专用，Sv39 时对应 VPN[2]）
// 直接映射，s=64 条目
struct walker_cache_entry_1_t {
    bool        valid;
    uint16_t    gscid;           // 16 比特
    uint32_t    pscid;           // 20 比特
    uint16_t    vpn_high;        // VPN 最高段 (VPN[3] for Sv48, VPN[2] for Sv39)
    uint64_t    ppn;             // 中间结果 PPN (44 比特)
    uint8_t     addr_mode;       // 地址模式标志 (Sv39/Sv48, VS/G-stage)

    walker_cache_entry_1_t() {
        valid = false;
        gscid = 0;
        pscid = 0;
        vpn_high = 0;
        ppn = 0;
        addr_mode = 0;
    }
};

// PTWc_2: 缓存{VPN[3],VPN[2]} 级中间结果
// 2-way 组相连，s=64 条目/way
struct walker_cache_entry_2_t {
    bool        valid;
    uint16_t    gscid;
    uint32_t    pscid;
    uint16_t    vpn_high;        // VPN 最高段
    uint16_t    vpn_mid;         // VPN 次高段 (VPN[2] for Sv48, VPN[1] for Sv39)
    uint64_t    ppn;
    uint8_t     addr_mode;
    uint8_t     rrpv;            // SRRIP 替换算法值 (M=2 比特)

    walker_cache_entry_2_t() {
        valid = false;
        gscid = 0;
        pscid = 0;
        vpn_high = 0;
        vpn_mid = 0;
        ppn = 0;
        addr_mode = 0;
        rrpv = 0;
    }
};

// PTWc_3: 缓存{VPN[3],VPN[2],VPN[1]} 级中间结果
// 4-way 组相连，s=64 条目/way (仅 Sv48 启用，Sv39 关闭)
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

    walker_cache_entry_3_t() {
        valid = false;
        gscid = 0;
        pscid = 0;
        vpn_high = 0;
        vpn_mid = 0;
        vpn_low = 0;
        ppn = 0;
        addr_mode = 0;
        rrpv = 0;
    }
};

// Walker Cache 容量配置
#define WALKER_CACHE_SET_SIZE   64
#define PTWC1_WAYS              1   // 直接映射
#define PTWC2_WAYS              2   // 2-way 组相连
#define PTWC3_WAYS              4   // 4-way 组相连

// Walker Cache 管理结构（内置于 PTW 模块中）
class walker_cache_t {
public:
    walker_cache_entry_1_t ptwc_1[WALKER_CACHE_SET_SIZE];
    walker_cache_entry_2_t ptwc_2[PTWC2_WAYS][WALKER_CACHE_SET_SIZE];
    walker_cache_entry_3_t ptwc_3[PTWC3_WAYS][WALKER_CACHE_SET_SIZE];
    sc_mutex               wc_mutex;    // 保护并发访问

    // 索引散列函数 (参考架构文档 4.5.4)
    uint16_t hash_1(uint16_t gscid, uint32_t pscid, uint16_t vpn_high) {
        return (gscid ^ pscid ^ vpn_high) & (WALKER_CACHE_SET_SIZE - 1);
    }

    uint16_t hash_2(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint16_t vpn_mid) {
        return (gscid ^ pscid ^ vpn_high ^ vpn_mid) & (WALKER_CACHE_SET_SIZE - 1);
    }

    uint16_t hash_3(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint16_t vpn_mid, uint16_t vpn_low) {
        return (gscid ^ pscid ^ vpn_high ^ vpn_mid ^ vpn_low) & (WALKER_CACHE_SET_SIZE - 1);
    }

    // 查询接口 (按优先级：PTWc_3 > PTWc_2 > PTWc_1)
    // 返回：命中级别 (1/2/3), 命中的 PPN; 0=未命中
    int lookup(uint16_t gscid, uint32_t pscid, uint64_t iova, uint8_t addr_mode, uint64_t* ppn) {
        wc_mutex.lock();

        // 提取 VPN 字段 (假设 Sv48，Sv39 时 vpn[3]=0)
        uint16_t vpn[4];
        vpn[3] = (iova >> 39) & 0x1FF;  // VPN[3] - bits 47:39
        vpn[2] = (iova >> 30) & 0x1FF;  // VPN[2] - bits 38:30
        vpn[1] = (iova >> 21) & 0x1FF;  // VPN[1] - bits 29:21
        vpn[0] = (iova >> 12) & 0x1FF;  // VPN[0] - bits 20:12

        // 按优先级查询 PTWc_3 (仅 Sv48)
        if (addr_mode == IOSATP_Sv48 || addr_mode == IOHGATP_Sv48x4) {
            uint16_t idx = hash_3(gscid, pscid, vpn[3], vpn[2], vpn[1]);
            for (int way = 0; way < PTWC3_WAYS; way++) {
                if (ptwc_3[way][idx].valid &&
                    ptwc_3[way][idx].gscid == gscid &&
                    ptwc_3[way][idx].pscid == pscid &&
                    ptwc_3[way][idx].vpn_high == vpn[3] &&
                    ptwc_3[way][idx].vpn_mid == vpn[2] &&
                    ptwc_3[way][idx].vpn_low == vpn[1] &&
                    ptwc_3[way][idx].addr_mode == addr_mode) {
                    *ppn = ptwc_3[way][idx].ppn;
                    wc_mutex.unlock();
                    return 3;  // PTWc_3 命中
                }
            }
        }

        // 查询 PTWc_2
        uint16_t idx2 = hash_2(gscid, pscid, vpn[3], vpn[2]);
        for (int way = 0; way < PTWC2_WAYS; way++) {
            if (ptwc_2[way][idx2].valid &&
                ptwc_2[way][idx2].gscid == gscid &&
                ptwc_2[way][idx2].pscid == pscid &&
                ptwc_2[way][idx2].vpn_high == vpn[3] &&
                ptwc_2[way][idx2].vpn_mid == vpn[2] &&
                ptwc_2[way][idx2].addr_mode == addr_mode) {
                *ppn = ptwc_2[way][idx2].ppn;
                wc_mutex.unlock();
                return 2;  // PTWc_2 命中
            }
        }

        // 查询 PTWc_1
        uint16_t idx1 = hash_1(gscid, pscid, vpn[3]);
        if (ptwc_1[idx1].valid &&
            ptwc_1[idx1].gscid == gscid &&
            ptwc_1[idx1].pscid == pscid &&
            ptwc_1[idx1].vpn_high == vpn[3] &&
            ptwc_1[idx1].addr_mode == addr_mode) {
            *ppn = ptwc_1[idx1].ppn;
            wc_mutex.unlock();
            return 1;  // PTWc_1 命中
        }

        wc_mutex.unlock();
        return 0;  // 未命中
    }

    // 更新接口
    void update_ptwc1(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint64_t ppn, uint8_t mode) {
        wc_mutex.lock();
        uint16_t idx = hash_1(gscid, pscid, vpn_high);
        ptwc_1[idx].valid = true;
        ptwc_1[idx].gscid = gscid;
        ptwc_1[idx].pscid = pscid;
        ptwc_1[idx].vpn_high = vpn_high;
        ptwc_1[idx].ppn = ppn;
        ptwc_1[idx].addr_mode = mode;
        wc_mutex.unlock();
    }

    void update_ptwc2(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint16_t vpn_mid, uint64_t ppn, uint8_t mode) {
        wc_mutex.lock();
        uint16_t idx = hash_2(gscid, pscid, vpn_high, vpn_mid);
        // SRRIP 替换：选择 RRPV 最大的 way
        int victim_way = 0;
        uint8_t max_rrpv = 0;
        for (int way = 0; way < PTWC2_WAYS; way++) {
            if (!ptwc_2[way][idx].valid) {
                victim_way = way;
                break;
            }
            if (ptwc_2[way][idx].rrpv >= max_rrpv) {
                max_rrpv = ptwc_2[way][idx].rrpv;
                victim_way = way;
            }
        }
        ptwc_2[victim_way][idx].valid = true;
        ptwc_2[victim_way][idx].gscid = gscid;
        ptwc_2[victim_way][idx].pscid = pscid;
        ptwc_2[victim_way][idx].vpn_high = vpn_high;
        ptwc_2[victim_way][idx].vpn_mid = vpn_mid;
        ptwc_2[victim_way][idx].ppn = ppn;
        ptwc_2[victim_way][idx].addr_mode = mode;
        ptwc_2[victim_way][idx].rrpv = 0;  // 新条目 RRPV=0
        wc_mutex.unlock();
    }

    void update_ptwc3(uint16_t gscid, uint32_t pscid, uint16_t vpn_high, uint16_t vpn_mid, uint16_t vpn_low, uint64_t ppn, uint8_t mode) {
        wc_mutex.lock();
        uint16_t idx = hash_3(gscid, pscid, vpn_high, vpn_mid, vpn_low);
        // SRRIP 替换
        int victim_way = 0;
        uint8_t max_rrpv = 0;
        for (int way = 0; way < PTWC3_WAYS; way++) {
            if (!ptwc_3[way][idx].valid) {
                victim_way = way;
                break;
            }
            if (ptwc_3[way][idx].rrpv >= max_rrpv) {
                max_rrpv = ptwc_3[way][idx].rrpv;
                victim_way = way;
            }
        }
        ptwc_3[victim_way][idx].valid = true;
        ptwc_3[victim_way][idx].gscid = gscid;
        ptwc_3[victim_way][idx].pscid = pscid;
        ptwc_3[victim_way][idx].vpn_high = vpn_high;
        ptwc_3[victim_way][idx].vpn_mid = vpn_mid;
        ptwc_3[victim_way][idx].vpn_low = vpn_low;
        ptwc_3[victim_way][idx].ppn = ppn;
        ptwc_3[victim_way][idx].addr_mode = mode;
        ptwc_3[victim_way][idx].rrpv = 0;
        wc_mutex.unlock();
    }

    // 失效接口
    void invalidate_by_gscid(uint16_t gscid) {
        wc_mutex.lock();
        for (int i = 0; i < WALKER_CACHE_SET_SIZE; i++) {
            if (ptwc_1[i].valid && ptwc_1[i].gscid == gscid) {
                ptwc_1[i].valid = false;
            }
            for (int way = 0; way < PTWC2_WAYS; way++) {
                if (ptwc_2[way][i].valid && ptwc_2[way][i].gscid == gscid) {
                    ptwc_2[way][i].valid = false;
                }
            }
            for (int way = 0; way < PTWC3_WAYS; way++) {
                if (ptwc_3[way][i].valid && ptwc_3[way][i].gscid == gscid) {
                    ptwc_3[way][i].valid = false;
                }
            }
        }
        wc_mutex.unlock();
    }

    void invalidate_by_gscid_pscid(uint16_t gscid, uint32_t pscid) {
        wc_mutex.lock();
        for (int i = 0; i < WALKER_CACHE_SET_SIZE; i++) {
            if (ptwc_1[i].valid && ptwc_1[i].gscid == gscid && ptwc_1[i].pscid == pscid) {
                ptwc_1[i].valid = false;
            }
            for (int way = 0; way < PTWC2_WAYS; way++) {
                if (ptwc_2[way][i].valid && ptwc_2[way][i].gscid == gscid && ptwc_2[way][i].pscid == pscid) {
                    ptwc_2[way][i].valid = false;
                }
            }
            for (int way = 0; way < PTWC3_WAYS; way++) {
                if (ptwc_3[way][i].valid && ptwc_3[way][i].gscid == gscid && ptwc_3[way][i].pscid == pscid) {
                    ptwc_3[way][i].valid = false;
                }
            }
        }
        wc_mutex.unlock();
    }

    void invalidate_by_iova(uint16_t gscid, uint32_t pscid, uint64_t iova) {
        // 简化实现：按 (gscid, pscid) 失效
        invalidate_by_gscid_pscid(gscid, pscid);
    }

    void invalidate_all() {
        wc_mutex.lock();
        for (int i = 0; i < WALKER_CACHE_SET_SIZE; i++) {
            ptwc_1[i].valid = false;
            for (int way = 0; way < PTWC2_WAYS; way++) {
                ptwc_2[way][i].valid = false;
            }
            for (int way = 0; way < PTWC3_WAYS; way++) {
                ptwc_3[way][i].valid = false;
            }
        }
        wc_mutex.unlock();
    }

    walker_cache_t() {
        for (int i = 0; i < WALKER_CACHE_SET_SIZE; i++) {
            ptwc_1[i].valid = false;
            for (int way = 0; way < PTWC2_WAYS; way++) {
                ptwc_2[way][i].valid = false;
            }
            for (int way = 0; way < PTWC3_WAYS; way++) {
                ptwc_3[way][i].valid = false;
            }
        }
    }
};

#endif // __IOMMU_TASK_HH__
