// IOMMU Performance Model - DC/PC Cache Threads (4 threads)
// SPEC Section 12.2-12.5

#include "iommu_top.hh"
#include <cstdio>

// ============================================================
// 12.2 dc_cache_query_thread - DC Cache Lookup
// Corresponds to lookup_ioatc_dc() in iommu_atc.cc
// ============================================================
void iommu_top::dc_cache_query_thread() {
    while (true) {
        iommu_task_t* task = parser_to_dc_cache_query_fifo.read();
        printf("[DC_CACHE_ENTRY] task_id=%u, time=%s, device_id=0x%x\n", 
               task->task_id, sc_time_stamp().to_string().c_str(), task->device_id);
        fflush(stdout);
        
        wait(DC_CACHE_HIT_DELAY, SC_NS);

        iommu_cache_mtx.lock();
        uint8_t status = lookup_ioatc_dc(&iommu_inst, task->device_id, &task->DC);
        iommu_cache_mtx.unlock();

        if (status == IOATC_HIT) {
            task->dc_valid = 1;
            task->dc_hit = 1;
            task->DTF = task->DC.tc.DTF;
            g_dc_cache_hit_count++;
            printf("[DC_CACHE_EXIT] task_id=%u, time=%s, device_id=0x%x -> HIT (total_hit=%lu)\n", 
                   task->task_id, sc_time_stamp().to_string().c_str(), task->device_id, g_dc_cache_hit_count);
        } else {
            task->dc_valid = 0;
            task->dc_hit = 0;
            g_dc_cache_miss_count++;
            printf("[DC_CACHE_EXIT] task_id=%u, time=%s, device_id=0x%x -> MISS (total_miss=%lu)\n", 
                   task->task_id, sc_time_stamp().to_string().c_str(), task->device_id, g_dc_cache_miss_count);
        }
        fflush(stdout);

        dc_cache_to_collector_fifo.write(task);
    }
}

// ============================================================
// 12.3 dc_cache_update_thread - DC Cache Update (after xDTW)
// Corresponds to cache_ioatc_dc() in iommu_atc.cc
// ============================================================
void iommu_top::dc_cache_update_thread() {
    while (true) {
        iommu_task_t* task = collector_to_dc_cache_update_fifo.read();
        wait(DC_CACHE_HIT_DELAY, SC_NS);

        printf("[DC_CACHE_UPD] task_id=%u, device_id=0x%x -> update DC cache\n",
               task->task_id, task->device_id);
        fflush(stdout);

        iommu_cache_mtx.lock();
        cache_ioatc_dc(&iommu_inst, task->device_id, &task->DC);
        iommu_cache_mtx.unlock();

        // Note: task pointer ownership remains with Collector
        // dc_cache_update_thread does NOT delete or forward the task
    }
}

// ============================================================
// 12.4 pc_cache_query_thread - PC Cache Lookup
// Corresponds to lookup_ioatc_pc() in iommu_atc.cc
// ============================================================
void iommu_top::pc_cache_query_thread() {
    while (true) {
        iommu_task_t* task = parser_to_pc_cache_query_fifo.read();
        printf("[PC_CACHE_ENTRY] task_id=%u, time=%s, device_id=0x%x, pid=%u\n", 
               task->task_id, sc_time_stamp().to_string().c_str(), task->device_id, task->process_id);
        fflush(stdout);
        
        wait(PC_CACHE_HIT_DELAY, SC_NS);

        iommu_cache_mtx.lock();
        uint8_t status = lookup_ioatc_pc(&iommu_inst, task->device_id,
                                          task->process_id, &task->PC);
        iommu_cache_mtx.unlock();

        if (status == IOATC_HIT) {
            task->pc_valid = 1;
            task->pc_hit = 1;
            g_pc_cache_hit_count++;
            printf("[PC_CACHE_EXIT] task_id=%u, time=%s, device_id=0x%x, pid=%u -> HIT (total_hit=%lu)\n",
                   task->task_id, sc_time_stamp().to_string().c_str(), task->device_id, task->process_id, g_pc_cache_hit_count);
        } else {
            task->pc_valid = 0;
            task->pc_hit = 0;
            g_pc_cache_miss_count++;
            printf("[PC_CACHE_EXIT] task_id=%u, time=%s, device_id=0x%x, pid=%u -> MISS (total_miss=%lu)\n",
                   task->task_id, sc_time_stamp().to_string().c_str(), task->device_id, task->process_id, g_pc_cache_miss_count);
        }
        fflush(stdout);

        pc_cache_to_collector_fifo.write(task);
    }
}

// ============================================================
// 12.5 pc_cache_update_thread - PC Cache Update (after xDTW)
// Corresponds to cache_ioatc_pc() in iommu_atc.cc
// ============================================================
void iommu_top::pc_cache_update_thread() {
    while (true) {
        iommu_task_t* task = collector_to_pc_cache_update_fifo.read();
        wait(PC_CACHE_HIT_DELAY, SC_NS);

        printf("[PC_CACHE_UPD] task_id=%u, device_id=0x%x, pid=%u -> update PC cache\n",
               task->task_id, task->device_id, task->process_id);
        fflush(stdout);

        iommu_cache_mtx.lock();
        cache_ioatc_pc(&iommu_inst, task->device_id, task->process_id, &task->PC);
        iommu_cache_mtx.unlock();

        // Note: task pointer ownership remains with Collector
    }
}
