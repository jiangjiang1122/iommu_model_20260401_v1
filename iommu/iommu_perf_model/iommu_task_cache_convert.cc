// IOMMU Performance Model - Task to CacheMessage Conversion Utilities
// Convert between iommu_task_t and CacheMessage data structures

#include "iommu_top.hh"
#include <cstdio>

// Don't use 'using namespace iommu;' to avoid namespace pollution

// ============================================================
// Convert iommu_task_t to CacheMessage for DC lookup request
// ============================================================
iommu::CacheMessage task_to_dc_request(iommu_task_t* task) {
    iommu::CacheMessage req;
    
    req.msg_type  = iommu::CacheMsgType::DC_LOOKUP;
    req.task_id   = task->task_id;
    req.device_id = task->device_id;
    req.iova      = task->iova;
    
    printf("[CONVERT] task_id=%u -> DC_LOOKUP request (device_id=0x%x)\n",
           task->task_id, task->device_id);
    fflush(stdout);
    
    return req;
}

// ============================================================
// Convert iommu_task_t to CacheMessage for PC lookup request
// ============================================================
iommu::CacheMessage task_to_pc_request(iommu_task_t* task) {
    iommu::CacheMessage req;
    
    req.msg_type   = iommu::CacheMsgType::PC_LOOKUP;
    req.task_id    = task->task_id;
    req.device_id  = task->device_id;
    req.process_id = task->process_id;
    req.iova       = task->iova;
    
    printf("[CONVERT] task_id=%u -> PC_LOOKUP request (process_id=%u)\n",
           task->task_id, task->process_id);
    fflush(stdout);
    
    return req;
}

// ============================================================
// Convert DC CacheMessage response back to iommu_task_t
// Populates task fields from resp.dc_data
// ============================================================
void dc_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task) {
    if (resp.hit) {
        // DC hit: mark as hit and copy full DC data
        task->dc_hit = 1;
        
        // Now types are unified, can do full struct assignment
        task->DC = resp.dc_data;
        
        // Extract key fields
        task->GSCID = resp.dc_data.iohgatp.GSCID;
        
        printf("[CONVERT] task_id=%u <- DC_LOOKUP response (HIT, gscid=%u)\n",
               task->task_id, task->GSCID);
        fflush(stdout);
    } else {
        // DC miss
        task->dc_hit = 0;
        
        printf("[CONVERT] task_id=%u <- DC_LOOKUP response (MISS)\n",
               task->task_id);
        fflush(stdout);
    }
}

// ============================================================
// Convert PC CacheMessage response back to iommu_task_t
// Populates task fields from resp.pc_data
// ============================================================
void pc_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task) {
    if (resp.hit) {
        // PC hit: mark as hit and copy full PC data
        task->pc_hit = 1;
        
        // Now types are unified, can do full struct assignment
        task->PC = resp.pc_data;
        
        // Extract key fields
        task->PSCID = resp.pc_data.ta.PSCID;
        
        printf("[CONVERT] task_id=%u <- PC_LOOKUP response (HIT, pscid=%u)\n",
               task->task_id, task->PSCID);
        fflush(stdout);
    } else {
        // PC miss
        task->pc_hit = 0;
        
        printf("[CONVERT] task_id=%u <- PC_LOOKUP response (MISS)\n",
               task->task_id);
        fflush(stdout);
    }
}

// ============================================================
// Convert iommu_task_t to CacheMessage for DC update
// ============================================================
iommu::CacheMessage task_to_dc_update(iommu_task_t* task) {
    iommu::CacheMessage req;
    
    req.msg_type  = iommu::CacheMsgType::DC_UPDATE;
    req.task_id   = task->task_id;
    req.device_id = task->device_id;
    // Now types are unified, can do full struct assignment
    req.dc_data = task->DC;
    
    printf("[CONVERT] task_id=%u -> DC_UPDATE request\n", task->task_id);
    fflush(stdout);
    
    return req;
}

// ============================================================
// Convert iommu_task_t to CacheMessage for PC update
// ============================================================
iommu::CacheMessage task_to_pc_update(iommu_task_t* task) {
    iommu::CacheMessage req;
    
    req.msg_type   = iommu::CacheMsgType::PC_UPDATE;
    req.task_id    = task->task_id;
    req.device_id  = task->device_id;
    req.process_id = task->process_id;
    // Now types are unified, can do full struct assignment
    req.pc_data = task->PC;
    
    printf("[CONVERT] task_id=%u -> PC_UPDATE request\n", task->task_id);
    fflush(stdout);
    
    return req;
}

// ============================================================
// Convert iommu_task_t to CacheMessage for PT lookup request
// ============================================================
iommu::CacheMessage task_to_pt_request(iommu_task_t* task) {
    iommu::CacheMessage req;
    
    req.msg_type   = iommu::CacheMsgType::PT_LOOKUP;
    req.task_id    = task->task_id;
    req.gscid      = task->GSCID;
    req.pscid      = task->PSCID;
    req.iova       = task->iova;
    
    // (1) 根据iohgatp.MODE和iosatp.MODE判断翻译阶段
    bool stage1_bare = (task->iosatp.MODE == RVI_IOMMU_IOSATP_Bare);
    bool stage2_bare = (task->iohgatp.MODE == RVI_IOMMU_IOHGATP_Bare);
    
    if (stage1_bare && !stage2_bare) {
        // S-stage: Bare, VS-stage: 非Bare -> STAGE2_ONLY
        req.stage = iommu::TransStage::STAGE2_ONLY;
    } else if (!stage1_bare && !stage2_bare) {
        // S-stage: 非Bare, VS-stage: 非Bare -> STAGE1_AND_2
        req.stage = iommu::TransStage::STAGE1_AND_2;
    } else if (!stage1_bare && stage2_bare) {
        // S-stage: 非Bare, VS-stage: Bare -> STAGE1_ONLY
        req.stage = iommu::TransStage::STAGE1_ONLY;
    } else {
        // 两者都Bare: 不应该发生，默认STAGE1_ONLY
        req.stage = iommu::TransStage::STAGE1_ONLY;
    }
    
    // (2) pt_sv48: 表示Sv39/Sv48标志位，指明iova地址类型
    // 如果iosatp.MODE == Sv48，则为true，否则为false（Sv39或Bare）
    req.pt_sv48 = (task->iosatp.MODE == RVI_IOMMU_IOSATP_Sv48);
    
    // (3) pt_gstage_x4: 表示G-stage阶段GPA是Sv39/Sv48还是Sv39x4/Sv48x4
    // 如果iohgatp.MODE == Sv48x4或Sv39x4，则为true，否则为false
    req.pt_gstage_x4 = (task->iohgatp.MODE == RVI_IOMMU_IOHGATP_Sv48x4 || 
                        task->iohgatp.MODE == RVI_IOMMU_IOHGATP_Sv39x4);
    
    printf("[CONVERT] task_id=%u -> PT_LOOKUP request (gscid=%u, pscid=%u, iova=0x%lx, stage=%d, sv48=%d, x4=%d)\n",
           task->task_id, task->GSCID, task->PSCID, task->iova, 
           static_cast<int>(req.stage), req.pt_sv48, req.pt_gstage_x4);
    fflush(stdout);
    
    return req;
}

// ============================================================
// Convert PT CacheMessage hit response back to iommu_task_t
// ============================================================
void pt_hit_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task) {
    // PT hit: extract PA from PT cache response
    if (resp.hit && resp.pt_data.reserved.valid) {
        // Get the SPA (Supervisor Physical Address) from PT data
        uint64_t ppn = 0;
        if (resp.stage == iommu::TransStage::STAGE1_ONLY) {
            ppn = resp.pt_data.vs_pte.PPN;
        } else if (resp.stage == iommu::TransStage::STAGE2_ONLY) {
            ppn = resp.pt_data.g_pte.PPN;
        } else {
            // STAGE1_AND_2: use the final translation result
            ppn = resp.pt_data.g_pte.PPN;
        }
        
        // Calculate PA: PPN * PAGESIZE + offset
        uint64_t page_size = 4096;  // Default 4K
        uint64_t offset = task->iova & (page_size - 1);
        task->pa = (ppn << 12) | offset;
        task->state = TASK_DONE;
        
        printf("[CONVERT] task_id=%u <- PT_LOOKUP response (HIT, pa=0x%lx)\n",
               task->task_id, task->pa);
        fflush(stdout);
    }
}

// ============================================================
// Convert PT CacheMessage miss response back to iommu_task_t
// ============================================================
void pt_miss_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task) {
    // PT miss: need to walk page table
    task->state = TASK_PTW_REQ;
    
    printf("[CONVERT] task_id=%u <- PT_LOOKUP response (MISS)\n",
           task->task_id);
    fflush(stdout);
}

// ============================================================
// Convert iommu_task_t to CacheMessage for PT update
// ============================================================
iommu::CacheMessage task_to_pt_update(iommu_task_t* task) {
    iommu::CacheMessage req;
    
    req.msg_type   = iommu::CacheMsgType::PT_UPDATE;
    req.task_id    = task->task_id;
    req.gscid      = task->GSCID;
    req.pscid      = task->PSCID;
    req.iova       = task->iova;
    
    // (1) 根据iohgatp.MODE和iosatp.MODE判断翻译阶段
    bool stage1_bare = (task->iosatp.MODE == RVI_IOMMU_IOSATP_Bare);
    bool stage2_bare = (task->iohgatp.MODE == RVI_IOMMU_IOHGATP_Bare);
    
    if (stage1_bare && !stage2_bare) {
        // S-stage: Bare, VS-stage: 非Bare -> STAGE2_ONLY
        req.stage = iommu::TransStage::STAGE2_ONLY;
    } else if (!stage1_bare && !stage2_bare) {
        // S-stage: 非Bare, VS-stage: 非Bare -> STAGE1_AND_2
        req.stage = iommu::TransStage::STAGE1_AND_2;
    } else if (!stage1_bare && stage2_bare) {
        // S-stage: 非Bare, VS-stage: Bare -> STAGE1_ONLY
        req.stage = iommu::TransStage::STAGE1_ONLY;
    } else {
        // 两者都Bare: 不应该发生，默认STAGE1_ONLY
        req.stage = iommu::TransStage::STAGE1_ONLY;
    }
    
    // (2) pt_sv48: 表示Sv39/Sv48标志位，指明iova地址类型
    req.pt_sv48 = (task->iosatp.MODE == RVI_IOMMU_IOSATP_Sv48);
    
    // (3) pt_gstage_x4: 表示G-stage阶段GPA是Sv39/Sv48还是Sv39x4/Sv48x4
    req.pt_gstage_x4 = (task->iohgatp.MODE == RVI_IOMMU_IOHGATP_Sv48x4 || 
                        task->iohgatp.MODE == RVI_IOMMU_IOHGATP_Sv39x4);
    
    req.from_prefetch = false;
    
    // (4) 使用make_pt_data构造PTData
    // 根据task->pa和req.stage构造完整的PTData
    req.pt_data = iommu::make_pt_data(
        task->pa,                          // SPA (System Physical Address)
        iommu::PageSize::PAGE_4K,          // 页大小 (默认4KB)
        0x07,                              // permissions (R|W|X)
        req.stage,                         // 翻译阶段
        iommu::PageSize::PAGE_4K,          // input_page_size
        (req.stage == iommu::TransStage::STAGE2_ONLY) ? false : true,  // iova_is_va
        req.pt_sv48,                       // sv48标志
        req.pt_gstage_x4                   // gstage_x4标志
    );
    
    printf("[CONVERT] task_id=%u -> PT_UPDATE request (gscid=%u, pscid=%u, iova=0x%lx, pa=0x%lx, stage=%d, sv48=%d, x4=%d)\n",
           task->task_id, task->GSCID, task->PSCID, task->iova, task->pa,
           static_cast<int>(req.stage), req.pt_sv48, req.pt_gstage_x4);
    fflush(stdout);
    
    return req;
}

// ============================================================
// Convert iommu_task_t to CacheMessage for Walker Cache lookup request
// ============================================================
iommu::CacheMessage task_to_walker_request(iommu_task_t* task) {
    iommu::CacheMessage req;
    
    req.msg_type           = iommu::CacheMsgType::WALKER_LOOKUP;
    req.task_id            = task->task_id;
    
    // 从 task 解析路由键
    req.gscid              = task->GSCID;
    req.pscid              = task->PSCID;
    req.iova               = task->iova;
    
    // 解析翻译模式标志位
    req.walker_addr_is_va  = (task->iosatp.MODE != IOSATP_Bare);
    req.walker_from_two_stage = (task->iohgatp.MODE != IOHGATP_Bare);
    req.walker_sv48        = (task->iosatp.MODE == IOSATP_Sv48);
    req.walker_x4_mode     = (task->iohgatp.MODE == IOHGATP_Sv39x4 || 
                              task->iohgatp.MODE == IOHGATP_Sv48x4);
    
    printf("[CONVERT] task_id=%u -> WALKER_LOOKUP request (gscid=%u, pscid=%u, iova=0x%lx, sv48=%d, x4=%d)\n",
           task->task_id, task->GSCID, task->PSCID, task->iova,
           req.walker_sv48, req.walker_x4_mode);
    fflush(stdout);
    
    return req;
}

// ============================================================
// Convert Walker CacheMessage response back to iommu_task_t
// Populates task walk_ctx from resp.walker_data
// ============================================================
void walker_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task) {
    if (resp.hit) {
        // Walker Cache hit: extract hit level and next PPN
        uint8_t hit_level = resp.walker_level;
        iommu::ppn_t next_ppn = resp.walker_data.next_ppn;
        
        // 设置walk starting point从命中层级的下一级开始
        task->walk_ctx.level = hit_level - 1;
        task->walk_ctx.base_addr = next_ppn * PAGESIZE;
        
        printf("[CONVERT] task_id=%u <- WALKER_LOOKUP response (HIT at level=%d, next_ppn=0x%lx, start from level=%d)\n",
               task->task_id, hit_level, next_ppn, task->walk_ctx.level);
        fflush(stdout);
    } else {
        // Walker Cache miss: will start full walk from highest level
        printf("[CONVERT] task_id=%u <- WALKER_LOOKUP response (MISS)\n",
               task->task_id);
        fflush(stdout);
    }
}

// ============================================================
// Convert iommu_task_t to CacheMessage for Walker Cache update
// ============================================================
iommu::CacheMessage task_to_walker_update(iommu_task_t* task) {
    iommu::CacheMessage req;
    
    req.msg_type           = iommu::CacheMsgType::WALKER_UPDATE;
    req.task_id            = task->task_id;
    req.gscid              = task->GSCID;
    req.pscid              = task->PSCID;
    req.iova               = task->iova;
    
    // 路由键标志位（与lookup保持一致）
    req.walker_addr_is_va  = (task->iosatp.MODE != IOSATP_Bare);
    req.walker_from_two_stage = (task->iohgatp.MODE != IOHGATP_Bare);
    req.walker_sv48        = (task->iosatp.MODE == IOSATP_Sv48);
    req.walker_x4_mode     = (task->iohgatp.MODE == IOHGATP_Sv39x4 || 
                              task->iohgatp.MODE == IOHGATP_Sv48x4);
    
    // 根据保存的中间结果构造三级数据
    // 默认值：valid=false
    req.walker_data_ptwc1 = iommu::make_walker_data(0, false, 
        req.walker_addr_is_va, req.walker_from_two_stage,
        req.walker_sv48, req.walker_x4_mode);
    req.walker_data_ptwc2 = iommu::make_walker_data(0, false,
        req.walker_addr_is_va, req.walker_from_two_stage,
        req.walker_sv48, req.walker_x4_mode);
    req.walker_data_ptwc3 = iommu::make_walker_data(0, false,
        req.walker_addr_is_va, req.walker_from_two_stage,
        req.walker_sv48, req.walker_x4_mode);
    
    // 填充实际命中的层级
    bool has_update = false;
    if (task->walk_ctx.walker_cache_entries.valid_level2) {
        req.walker_data_ptwc3 = iommu::make_walker_data(
            task->walk_ctx.walker_cache_entries.ppn_level2, true,
            req.walker_addr_is_va, req.walker_from_two_stage,
            req.walker_sv48, req.walker_x4_mode);
        has_update = true;
    }
    if (task->walk_ctx.walker_cache_entries.valid_level1) {
        req.walker_data_ptwc2 = iommu::make_walker_data(
            task->walk_ctx.walker_cache_entries.ppn_level1, true,
            req.walker_addr_is_va, req.walker_from_two_stage,
            req.walker_sv48, req.walker_x4_mode);
        has_update = true;
    }
    if (task->walk_ctx.walker_cache_entries.valid_level0) {
        req.walker_data_ptwc1 = iommu::make_walker_data(
            task->walk_ctx.walker_cache_entries.ppn_level0, true,
            req.walker_addr_is_va, req.walker_from_two_stage,
            req.walker_sv48, req.walker_x4_mode);
        has_update = true;
    }
    
    // 设置update kind
    if (has_update) {
        req.walker_update_kind = iommu::WalkerUpdateKind::PTWC_1_2_3;
    }
    
    printf("[CONVERT] task_id=%u -> WALKER_UPDATE request (gscid=%u, pscid=%u, iova=0x%lx, L2=%d, L1=%d, L0=%d)\n",
           task->task_id, task->GSCID, task->PSCID, task->iova,
           task->walk_ctx.walker_cache_entries.valid_level2,
           task->walk_ctx.walker_cache_entries.valid_level1,
           task->walk_ctx.walker_cache_entries.valid_level0);
    fflush(stdout);
    
    return req;
}
