#ifndef __IOMMU_PERF_MODEL_HH__
#define __IOMMU_PERF_MODEL_HH__

#include "iommu_struct.hh"
#include "iommu_task.hh"

// ===================== TTYP Classification =====================
// Corresponds to iommu_translate.cc L46-89
static inline void classify_ttype_and_attribs(iommu_task_t* task) {
    // Classify TTYP
    task->TTYP = TTYPE_NONE;
    if (task->at == ADDR_TYPE_UNTRANSLATED && task->read_writeAMO == READ) {
        if (task->exec_req)
            task->TTYP = UNTRANSLATED_READ_FOR_EXECUTE_TRANSACTION;
        else
            task->TTYP = UNTRANSLATED_READ_TRANSACTION;
    }
    if (task->at == ADDR_TYPE_UNTRANSLATED && task->read_writeAMO == WRITE)
        task->TTYP = UNTRANSLATED_WRITE_AMO_TRANSACTION;
    if (task->at == ADDR_TYPE_TRANSLATED && task->read_writeAMO == READ) {
        if (task->pid_valid && task->exec_req)
            task->TTYP = TRANSLATED_READ_FOR_EXECUTE_TRANSACTION;
        else
            task->TTYP = TRANSLATED_READ_TRANSACTION;
    }
    if (task->at == ADDR_TYPE_TRANSLATED && task->read_writeAMO == WRITE)
        task->TTYP = TRANSLATED_WRITE_AMO_TRANSACTION;
    if (task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST)
        task->TTYP = PCIE_ATS_TRANSLATION_REQUEST;

    // Extract access attributes (from get_attribs_from_req logic)
    task->is_read = (task->read_writeAMO == READ && task->exec_req &&
                     task->at == ADDR_TYPE_UNTRANSLATED) ?
                    0 : (task->read_writeAMO == READ) ? 1 : 0;
    task->is_write = (task->read_writeAMO == WRITE) ? 1 : 0;
    task->is_write = ((task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) &&
                      (task->no_write == 0)) ? 1 : task->is_write;
    task->is_exec = (task->read_writeAMO == READ && (task->exec_req &&
                     (task->at == ADDR_TYPE_UNTRANSLATED || task->pid_valid))) ? 1 : 0;
    task->priv = (task->pid_valid && task->priv_req) ? S_MODE : U_MODE;
}

// ===================== DDI Extraction =====================
// Corresponds to iommu_translate.cc L143-158
static inline void extract_DDI(iommu_task_t* task, iommu_t* iommu) {
    if (iommu->reg_file.capabilities.msi_flat == 0) {
        task->DDI[0] = get_bits(6,  0, task->device_id);
        task->DDI[1] = get_bits(15, 7, task->device_id);
        task->DDI[2] = get_bits(23, 16, task->device_id);
    } else {
        task->DDI[0] = get_bits(5,  0, task->device_id);
        task->DDI[1] = get_bits(14, 6, task->device_id);
        task->DDI[2] = get_bits(23, 15, task->device_id);
    }
}

// ===================== VPN Extraction =====================
// Extract VPN fields and determine LEVELS/PTESIZE based on iosatp mode and SXL
static inline void extract_vpn(uint64_t iova, uint8_t mode, uint8_t SXL,
                                uint16_t vpn[5], uint8_t* LEVELS, uint8_t* PTESIZE) {
    if (SXL == 1) {
        // RV32: Sv32
        vpn[0] = get_bits(21, 12, iova);
        vpn[1] = get_bits(31, 22, iova);
        *LEVELS = 2;
        *PTESIZE = 4;
    } else {
        // RV64 modes
        vpn[0] = get_bits(20, 12, iova);
        vpn[1] = get_bits(29, 21, iova);
        vpn[2] = get_bits(38, 30, iova);
        vpn[3] = get_bits(47, 39, iova);
        vpn[4] = get_bits(56, 48, iova);
        *PTESIZE = 8;

        if (mode == IOSATP_Sv39) {
            *LEVELS = 3;
        } else if (mode == IOSATP_Sv48) {
            *LEVELS = 4;
        } else if (mode == IOSATP_Sv57) {
            *LEVELS = 5;
        } else {
            *LEVELS = 3; // default to Sv39
        }
    }
}

// ===================== G-stage VPN Extraction =====================
static inline void extract_gs_vpn(uint64_t gpa, uint8_t mode,
                                   uint16_t gs_vpn[5], uint8_t* GS_LEVELS) {
    gs_vpn[0] = get_bits(20, 12, gpa);
    gs_vpn[1] = get_bits(29, 21, gpa);
    gs_vpn[2] = get_bits(38, 30, gpa);
    gs_vpn[3] = get_bits(47, 39, gpa);
    gs_vpn[4] = get_bits(56, 48, gpa);

    if (mode == IOHGATP_Sv39x4) {
        *GS_LEVELS = 3;
    } else if (mode == IOHGATP_Sv48x4) {
        *GS_LEVELS = 4;
    } else if (mode == IOHGATP_Sv57x4) {
        *GS_LEVELS = 5;
    } else if (mode == IOHGATP_Sv32x4) {
        *GS_LEVELS = 2;
    } else {
        *GS_LEVELS = 3;
    }
}

// ===================== Canonical Address Check =====================
static inline bool check_canonical(uint64_t iova, uint8_t mode, uint8_t SXL) {
    if (SXL == 1) {
        // Sv32: check bits above 31
        return (iova >> 32) == 0;
    }
    if (mode == IOSATP_Sv39) {
        // Check bits 63:39 are all same as bit 38
        uint64_t sign = (iova >> 38) & 1;
        uint64_t upper = iova >> 39;
        return (sign == 0) ? (upper == 0) : (upper == 0x1FFFFFF);
    }
    if (mode == IOSATP_Sv48) {
        uint64_t sign = (iova >> 47) & 1;
        uint64_t upper = iova >> 48;
        return (sign == 0) ? (upper == 0) : (upper == 0xFFFF);
    }
    if (mode == IOSATP_Sv57) {
        uint64_t sign = (iova >> 56) & 1;
        uint64_t upper = iova >> 57;
        return (sign == 0) ? (upper == 0) : (upper == 0x7F);
    }
    return true;
}

// ===================== Bare Mode Page Size =====================
static inline uint64_t get_bare_page_size(iommu_t* iommu) {
    // Use the configured bare page size based on iohgatp mode
    return iommu->sv39_bare_pg_sz ? iommu->sv39_bare_pg_sz : PAGESIZE;
}

static inline uint64_t get_gstage_bare_page_size(iommu_t* iommu) {
    return iommu->sv39x4_bare_pg_sz ? iommu->sv39x4_bare_pg_sz : PAGESIZE;
}

// ===================== MSI Address Check =====================
// Check if a GPA is an MSI address for the given device context
static inline uint8_t check_is_msi_address(uint64_t gpa, device_context_t* DC, iommu_t* iommu) {
    if (DC->msiptp.MODE == MSIPTP_Off) return 0;
    // An incoming write to GPA is recognized as MSI if:
    // (A >> 12) & ~msi_addr_mask = (msi_addr_pattern & ~msi_addr_mask)
    uint64_t a_shifted = gpa >> 12;
    uint64_t mask = DC->msi_addr_mask.mask;
    uint64_t pattern = DC->msi_addr_pattern.pattern;
    return ((a_shifted & ~mask) == (pattern & ~mask)) ? 1 : 0;
}

// ===================== Guest Fault Cause Helper =====================
static inline void set_guest_fault_cause(iommu_task_t* task, uint8_t base_fault) {
    if (task->is_exec) {
        task->cause = 20; // Instruction guest-page fault
    } else if (task->is_read) {
        task->cause = 21; // Load guest-page fault
    } else {
        task->cause = 23; // Store/AMO guest-page fault
    }
}

// ===================== DC Configuration Checks =====================
// These are declared extern in the functional model headers
extern uint8_t do_device_context_configuration_checks(iommu_t *iommu, device_context_t *DC);
extern uint8_t do_process_context_configuration_checks(iommu_t *iommu, device_context_t *DC, process_context_t *PC);

// ===================== MGPAW Calculation for MSI =====================
static inline uint64_t calculate_mgpaw(iommu_t* iommu) {
    // MGPAW = capabilities.pas (physical address size in bits)
    return iommu->reg_file.capabilities.pas;
}

// ===================== MSI Extract Function =====================
// Extract bits from a value using a mask pattern
// This implements the extract() function from iommu_msi_trans.cc
static inline uint64_t msi_extract(uint64_t value, uint64_t mask) {
    uint64_t result = 0;
    uint8_t bit_pos = 0;
    for (int i = 0; i < 52; i++) {
        if (mask & (1ULL << i)) {
            if (value & (1ULL << i)) {
                result |= (1ULL << bit_pos);
            }
            bit_pos++;
        }
    }
    return result;
}

#endif // __IOMMU_PERF_MODEL_HH__
