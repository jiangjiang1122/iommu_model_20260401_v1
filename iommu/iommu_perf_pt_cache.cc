//
// IOMMU 性能模型 - PT Cache (IOTLB) 模块实现
// 按照 SPEC v4 第 5.5 节定义实现
//

#include "iommu_top.hh"
#include "iommu_perf_model.hh"

// ============================================================================
// PT Cache 线程 1: 查询
// ============================================================================

void iommu_top::pt_cache_query_thread() {
    printf("[PT Cache-1] Query thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        collector_to_pt_cache_query_fifo.read(task);
        
#ifdef DEBUG_PT_CACHE
        printf("[PT Cache-1] Query for task %d: iova=0x%lx, PSCV=%d, PSCID=%d, GV=%d, GSCID=%d\n",
               task->task_id, task->iova, task->PSCV, task->PSCID, task->GV, task->GSCID);
#endif

        uint32_t cause = 0;
        uint64_t pa = 0;
        uint64_t page_sz = 0;
        spte_t vs_pte;
        gpte_t g_pte;
        vs_pte.raw = 0;
        g_pte.raw = 0;
        
        // 调用 IOTLB 查找函数
        // 参数：iova, check_access_perms, priv, is_read, is_write, is_exec, SUM, 
        //      PSCV, PSCID, GV, GSCID, cause, pa, page_sz, vs_pte, g_pte
        uint8_t result = lookup_iotlb(
            task->iova, 
            0,  // check_access_perms = 0 (在 Cache 查询时不检查权限)
            task->priv, 
            task->is_read, 
            task->is_write, 
            task->is_exec, 
            task->SUM, 
            task->PSCV, 
            task->PSCID, 
            task->GV, 
            task->GSCID,
            &cause, 
            &pa, 
            &page_sz, 
            &vs_pte, 
            &g_pte
        );
        
        task->vs_pte = vs_pte;
        task->g_pte = g_pte;
        
        if (result == IOATC_HIT) {
            // TLB Hit
            task->state = TASK_TLB_HIT;
            task->pa = pa;
            task->page_sz = page_sz;
#ifdef DEBUG_PT_CACHE
            printf("[PT Cache-1] Task %d: TLB HIT, pa=0x%lx, page_sz=%lu\n", 
                   task->task_id, pa, page_sz);
#endif
        } else if (result == IOATC_MISS) {
            // TLB Miss
            task->state = TASK_TLB_MISS;
#ifdef DEBUG_PT_CACHE
            printf("[PT Cache-1] Task %d: TLB MISS\n", task->task_id);
#endif
        } else {
            // Fault
            task->state = TASK_FAULT;
            task->cause = cause;
#ifdef DEBUG_PT_CACHE
            printf("[PT Cache-1] Task %d: FAULT, cause=%d\n", task->task_id, cause);
#endif
        }
        
        // 发送到内部结果 FIFO
        pt_cache_lookup_result_fifo.write(task);
        wait(PT_CACHE_HIT_DELAY, SC_NS);
    }
}

// ============================================================================
// PT Cache 线程 2: 结果处理
// ============================================================================

void iommu_top::pt_cache_result_thread() {
    printf("[PT Cache-2] Result thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        pt_cache_lookup_result_fifo.read(task);
        
#ifdef DEBUG_PT_CACHE
        printf("[PT Cache-2] Processing task %d, state=%d\n", task->task_id, task->state);
#endif
        
        if (task->state == TASK_TLB_HIT) {
            // 命中，直接转发
            task->state = TASK_FORWARD;
#ifdef DEBUG_PT_CACHE
            printf("[PT Cache-2] Task %d: Forwarding (TLB HIT)\n", task->task_id);
#endif
            pt_cache_to_fwd_fifo.write(task);
            
        } else if (task->state == TASK_TLB_MISS) {
            // 未命中，提交 PTW 进行页表 walk
#ifdef DEBUG_PT_CACHE
            printf("[PT Cache-2] Task %d: Sending to PTW (TLB MISS)\n", task->task_id);
#endif
            pt_cache_to_ptw_fifo.write(task);
            
        } else if (task->state == TASK_FAULT) {
            // 错误上报
#ifdef DEBUG_PT_CACHE
            printf("[PT Cache-2] Task %d: Reporting fault (cause=%d)\n", task->task_id, task->cause);
#endif
            fault_fifo.write(task);
        }
        
        wait(PT_CACHE_HIT_DELAY, SC_NS);
    }
}

// ============================================================================
// PT Cache 线程 3: PTW 响应处理
// ============================================================================

void iommu_top::pt_cache_ptw_rsp_thread() {
    printf("[PT Cache-3] PTW response thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        ptw_to_pt_cache_fifo.read(task);
        
#ifdef DEBUG_PT_CACHE
        printf("[PT Cache-3] Received PTW response for task %d, pa=0x%lx, page_sz=%lu\n", 
               task->task_id, task->pa, task->page_sz);
#endif

        // 更新 IOTLB
        update_iotlb(
            task->iova, 
            task->vs_pte, 
            task->g_pte, 
            task->pa, 
            task->page_sz,
            task->PSCV, 
            task->PSCID, 
            task->GV, 
            task->GSCID
        );
        
#ifdef DEBUG_PT_CACHE
        printf("[PT Cache-3] Updated IOTLB for task %d\n", task->task_id);
#endif
        
        // 转发
        task->state = TASK_FORWARD;
        pt_cache_to_fwd_fifo.write(task);
        
        wait(PT_CACHE_HIT_DELAY, SC_NS);
    }
}
