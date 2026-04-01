// Copyright (c) 2022 by Rivos Inc.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
// Author: ved@rivosinc.com
#ifndef __IOMMU_STRUCT_H__
#define __IOMMU_STRUCT_H__
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Endian definitions for Windows compatibility
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

typedef struct iommu_t iommu_t;

#include "iommu_registers.hh"
#include "iommu_data_structures.hh"
#include "iommu_req_rsp.hh"
#include "iommu_fault.hh"
#include "iommu_translate.hh"
#include "iommu_utils.hh"
#include "iommu_interrupt.hh"
#include "iommu_command_queue.hh"
#include "iommu_ats.hh"
#include "iommu_atc.hh"
#include "iommu_hpm.hh"
#include "iommu_ref_api.hh"

// Forward declaration for SystemC module
class iommu_top;

typedef struct iommu_t {
    // from iommu_command_queue.c
    uint8_t command_queue_stall_for_itag;
    uint8_t ats_inv_req_timeout;
    uint8_t iofence_wait_pending_inv;
    uint8_t iofence_pending_PR, iofence_pending_PW, iofence_pending_AV, iofence_pending_WSI_BIT;
    uint64_t iofence_pending_ADDR;
    uint32_t iofence_pending_DATA;

    uint8_t pending_inval_req_DSV;
    uint8_t pending_inval_req_DSEG;
    uint16_t pending_inval_req_RID;
    uint8_t pending_inval_req_PV;
    uint32_t pending_inval_req_PID;
    uint64_t pending_inval_req_PAYLOAD;

    // Pointer to SystemC top module
    class iommu_top *top;

    // iommu_reg.c
    // IOMMU register file
    iommu_regs_t reg_file;
    iommu_internal_regs_t internal_reg_file;
    
    // Register offset to size mapping
    uint8_t offset_to_size[4096];
    // Global parameters of the design
    uint8_t num_hpm;
    uint8_t hpmctr_bits;
    uint8_t eventID_limit;
    uint8_t num_vec_bits;
    uint8_t gxl_writeable;
    uint8_t fctl_be_writeable;
    uint8_t max_iommu_mode;
    uint8_t fill_ats_trans_in_ioatc;
    uint32_t max_devid_mask;
    uint8_t trans_for_debug;
    uint64_t sv57_bare_pg_sz;
    uint64_t sv48_bare_pg_sz;
    uint64_t sv39_bare_pg_sz;
    uint64_t sv32_bare_pg_sz;
    uint64_t sv57x4_bare_pg_sz;
    uint64_t sv48x4_bare_pg_sz;
    uint64_t sv39x4_bare_pg_sz;
    uint64_t sv32x4_bare_pg_sz;
    iommu_qosid_t iommu_qosid_mask;

    // from iommu_atc.
    ddt_cache_t ddt_cache[RVI_IOMMU_DDT_CACHE_SIZE];
    pdt_cache_t pdt_cache[RVI_IOMMU_PDT_CACHE_SIZE];
    tlb_t       tlb[RVI_IOMMU_TLB_SIZE];
    uint32_t    dc_lru_time;
    uint32_t    pc_lru_time;
    uint32_t    tlb_lru_time;

    itag_tracker_t itag_tracker[RVI_IOMMU_MAX_ITAGS];
    uint8_t msi_pending[16];
} iommu_t;

#endif
