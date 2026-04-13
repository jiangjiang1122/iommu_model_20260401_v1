//
// IOMMU 性能模型 - DC Cache 和 PC Cache 模块实现
// 按照 SPEC v4 第 5.3 和 5.4 节定义实现
//

#include "iommu_top.hh"
#include "iommu_perf_model.hh"

// ============================================================================
// DC Cache 线程实现
// ============================================================================

void iommu_top::dc_cache_thread() {
    printf("[DC Cache] Thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        bool got_data = false;
        
        // 检查查询请求 FIFO
        if (parser_to_dc_cache_query_fifo.num_available() > 0) {
            // 查询请求
            parser_to_dc_cache_query_fifo.read(task);
            
#ifdef DEBUG_DC_CACHE
            printf("[DC Cache] Query for device_id %d (task %d)\n", task->device_id, task->task_id);
#endif
            
            device_context_t DC;
            memset(&DC, 0, sizeof(DC));
            uint8_t result = lookup_dc_cache(task->device_id, &DC);
            
            if (result) {
                // Hit
                task->state = TASK_DC_HIT;
                task->DC = DC;
#ifdef DEBUG_DC_CACHE
                printf("[DC Cache] HIT for device_id %d\n", task->device_id);
#endif
            } else {
                // Miss
                task->state = TASK_DC_MISS;
#ifdef DEBUG_DC_CACHE
                printf("[DC Cache] MISS for device_id %d\n", task->device_id);
#endif
            }
            
            dc_cache_to_collector_fifo.write(task);
            got_data = true;
            wait(DC_CACHE_HIT_DELAY, SC_NS);
        }
        
        // 检查更新请求 FIFO
        if (collector_to_dc_cache_update_fifo.num_available() > 0) {
            // 更新请求
            collector_to_dc_cache_update_fifo.read(task);
            
#ifdef DEBUG_DC_CACHE
            printf("[DC Cache] Update for device_id %d\n", task->device_id);
#endif
            
            update_dc_cache(task->device_id, &task->DC);
            got_data = true;
            wait(DC_CACHE_HIT_DELAY, SC_NS);
        }
        
        // 如果两个 FIFO 都空，等待
        if (!got_data) {
            wait(SC_ZERO_TIME);
        }
    }
}

// ============================================================================
// PC Cache 线程实现
// ============================================================================

void iommu_top::pc_cache_thread() {
    printf("[PC Cache] Thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        bool got_data = false;
        
        // 检查查询请求 FIFO
        if (parser_to_pc_cache_query_fifo.num_available() > 0) {
            // 查询请求
            parser_to_pc_cache_query_fifo.read(task);
            
#ifdef DEBUG_PC_CACHE
            printf("[PC Cache] Query for device_id %d, process_id %d (task %d)\n", 
                   task->device_id, task->process_id, task->task_id);
#endif
            
            process_context_t PC;
            memset(&PC, 0, sizeof(PC));
            uint8_t result = lookup_pc_cache(task->device_id, task->process_id, &PC);
            
            if (result) {
                // Hit
                task->state = TASK_PC_HIT;
                task->PC = PC;
#ifdef DEBUG_PC_CACHE
                printf("[PC Cache] HIT for device_id %d, process_id %d\n", 
                       task->device_id, task->process_id);
#endif
            } else {
                // Miss
                task->state = TASK_PC_MISS;
#ifdef DEBUG_PC_CACHE
                printf("[PC Cache] MISS for device_id %d, process_id %d\n", 
                       task->device_id, task->process_id);
#endif
            }
            
            pc_cache_to_collector_fifo.write(task);
            got_data = true;
            wait(PC_CACHE_HIT_DELAY, SC_NS);
        }
        
        // 检查更新请求 FIFO
        if (collector_to_pc_cache_update_fifo.num_available() > 0) {
            // 更新请求
            collector_to_pc_cache_update_fifo.read(task);
            
#ifdef DEBUG_PC_CACHE
            printf("[PC Cache] Update for device_id %d, process_id %d\n", 
                   task->device_id, task->process_id);
#endif
            
            update_pc_cache(task->device_id, task->process_id, &task->PC);
            got_data = true;
            wait(PC_CACHE_HIT_DELAY, SC_NS);
        }
        
        // 如果两个 FIFO 都空，等待
        if (!got_data) {
            wait(SC_ZERO_TIME);
        }
    }
}
