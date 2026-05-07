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
