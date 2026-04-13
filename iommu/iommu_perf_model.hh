#ifndef __IOMMU_PERF_MODEL_HH__
#define __IOMMU_PERF_MODEL_HH__

#include "iommu_top.hh"
#include "iommu_task.hh"
#include "iommu_translate.hh"
#include "iommu_struct.hh"
#include "iommu_struct.hh"

using namespace std;

// ============================================================================
// 辅助函数声明
// ============================================================================

// 从请求中提取访问属性
inline void get_attribs_from_req(iommu_task_t* task, uint8_t* is_read, uint8_t* is_write, uint8_t* is_exec, uint8_t* priv) {
    *is_read = (task->read_writeAMO == READ) ? 1 : 0;
    *is_write = (task->read_writeAMO == WRITE) ? 1 : 0;
    *is_exec = task->exec_req;
    *priv = task->priv_req;
}

// 提取 DDI 索引
inline void extract_ddi(iommu_t* iommu, uint32_t device_id, uint8_t* DDI) {
    if (iommu->reg_file.capabilities.msi_flat == 0) {
        DDI[0] = get_bits(6,  0, device_id);
        DDI[1] = get_bits(15, 7, device_id);
        DDI[2] = get_bits(23, 16, device_id);
    } else {
        DDI[0] = get_bits(5,  0, device_id);
        DDI[1] = get_bits(14, 6, device_id);
        DDI[2] = get_bits(23, 15, device_id);
    }
}

// 检查设备 ID 宽度
inline bool check_device_id_width(iommu_t* iommu, uint8_t* DDI) {
    if (iommu->reg_file.ddtp.iommu_mode == DDT_2LVL && DDI[2] != 0) {
        return false;
    }
    if (iommu->reg_file.ddtp.iommu_mode == DDT_1LVL && (DDI[2] != 0 || DDI[1] != 0)) {
        return false;
    }
    return true;
}

// 计算 DDT 索引
inline uint8_t calculate_ddt_index(iommu_t* iommu, uint32_t device_id) {
    if (iommu->reg_file.capabilities.msi_flat == 0) {
        return (device_id >> 0) & 0x7F;
    } else {
        return (device_id >> 0) & 0x3F;
    }
}

// 计算 PDT 索引
inline uint16_t calculate_pdt_index(fsc_t fsc, uint32_t process_id) {
    if (fsc.pdtp.MODE == PD8) {
        return process_id & 0xFF;
    } else if (fsc.pdtp.MODE == PD17) {
        return (process_id >> 8) & 0x1FF;
    } else {
        return (process_id >> 11) & 0x1FF;
    }
}

// 计算 VPN 索引（根据页表模式）
inline void calculate_vpn_indices(uint64_t iova, uint8_t mode, uint16_t* vpn) {
    // iova bits: [63:12] = VPN + offset
    if (mode == IOSATP_Sv39 || mode == IOHGATP_Sv39x4) {
        // Sv39: 3 级页表 VPN[2:0]
        vpn[3] = 0;
        vpn[2] = (iova >> 30) & 0x1FF;  // bits 38:30
        vpn[1] = (iova >> 21) & 0x1FF;  // bits 29:21
        vpn[0] = (iova >> 12) & 0x1FF;  // bits 20:12
    } else if (mode == IOSATP_Sv48 || mode == IOHGATP_Sv48x4) {
        // Sv48: 4 级页表 VPN[3:0]
        vpn[3] = (iova >> 39) & 0x1FF;  // bits 47:39
        vpn[2] = (iova >> 30) & 0x1FF;  // bits 38:30
        vpn[1] = (iova >> 21) & 0x1FF;  // bits 29:21
        vpn[0] = (iova >> 12) & 0x1FF;  // bits 20:12
    } else {
        // Bare or Sv32
        vpn[0] = vpn[1] = vpn[2] = vpn[3] = 0;
    }
}

// 获取页表 walk 层级数
inline uint8_t get_page_table_levels(uint8_t mode) {
    switch (mode) {
        case 3: // IOSATP_Sv39
        case 8: // IOHGATP_Sv39x4
            return 3;
        case 4: // IOSATP_Sv48
        case 9: // IOHGATP_Sv48x4
            return 4;
        default:
            return 0;
    }
}

// MSI 地址判断
inline bool is_msi_address(uint64_t gpa, device_context_t* DC) {
    if (DC->msiptp.MODE == MSIPTP_Off) {
        return false;
    }
    
    uint64_t mask = DC->msi_addr_mask.mask;
    uint64_t pattern = DC->msi_addr_pattern.pattern;
    
    // (A >> 12) & ~mask == pattern & ~mask
    uint64_t gpa_page = gpa >> 12;
    uint64_t not_mask = ~mask;
    
    return (gpa_page & not_mask) == (pattern & not_mask);
}

// PTE 解析辅助函数
inline uint64_t get_pte_ppn(uint64_t pte_raw) {
    return (pte_raw >> 10) & 0xFFFFFFFFFULL; // bits 53:10
}

inline bool is_pte_leaf(uint64_t pte_raw) {
    // Leaf PTE has R, W, or X bit set
    return (pte_raw >> 1) & 0x7; // bits 3:1 = R, W, X
}

inline bool is_pte_valid(uint64_t pte_raw) {
    return pte_raw & 0x1; // V bit
}

// 虚拟中断文件重叠处理
inline void handle_virtual_interrupt_file_overlap(device_context_t* DC, uint64_t gpa, uint64_t* gst_page_sz) {
    // 检查GPA是否落在MSI虚拟中断文件范围内
    if (DC->msiptp.MODE != MSIPTP_Off) {
        uint64_t gpa_page = gpa >> 12;
        uint64_t mask = DC->msi_addr_mask.mask;
        uint64_t pattern = DC->msi_addr_pattern.pattern;
        uint64_t not_mask = ~mask;
        
        // 检查是否匹配MSI地址模式
        if ((gpa_page & not_mask) == (pattern & not_mask)) {
            // 如果页面大小大于4KB，限制为4KB以避免与其他页面重叠
            if (*gst_page_sz > 4096) {
                *gst_page_sz = 4096;
            }
        }
    }
}

#endif // __IOMMU_PERF_MODEL_HH__
