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
    req.stage      = (task->iohgatp.MODE == 0) ? 
                     iommu::TransStage::STAGE1_ONLY : 
                     iommu::TransStage::STAGE1_AND_2;
    req.pt_sv48    = (task->iosatp.MODE >= 9);  // Sv48 or Sv57
    req.pt_gstage_x4 = (task->iohgatp.MODE >= 8);  // Sv39x4 or Sv48x4
    
    printf("[CONVERT] task_id=%u -> PT_LOOKUP request (gscid=%u, pscid=%u, iova=0x%lx)\n",
           task->task_id, task->GSCID, task->PSCID, task->iova);
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
    req.stage      = (task->iohgatp.MODE == 0) ? 
                     iommu::TransStage::STAGE1_ONLY : 
                     iommu::TransStage::STAGE1_AND_2;
    req.pt_sv48    = (task->iosatp.MODE >= 9);
    req.pt_gstage_x4 = (task->iohgatp.MODE >= 8);
    req.from_prefetch = false;
    
    // Copy PT data from task to CacheMessage
    req.pt_data.reserved.valid = 1;
    // Note: Skipping vs_pte and g_pte assignment due to type mismatch (gpte_t)
    // PT cache update will use the PA directly from task->pa
    req.pt_data.vs_pte.PPN = task->pa >> 12;
    req.pt_data.g_pte.PPN = task->pa >> 12;
    
    printf("[CONVERT] task_id=%u -> PT_UPDATE request\n", task->task_id);
    fflush(stdout);
    
    return req;
}
