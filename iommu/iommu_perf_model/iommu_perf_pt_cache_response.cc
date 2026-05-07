// IOMMU Performance Model - PT Cache Response Handler Thread
// Handles PT cache hit/miss responses from CacheSubsystem

#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>

// ============================================================
// collector_pt_response_thread - Process PT cache hit/miss responses
// Reads from cache_sub.pt_hit_response_fifo and pt_miss_response_fifo
// Routes tasks to forwarder (hit) or PTW (miss)
// ============================================================
void iommu_top::collector_pt_response_thread() {
    while (true) {
        // Wait for either hit or miss response
        wait(cache_sub.pt_hit_response_fifo.data_written_event() |
             cache_sub.pt_miss_response_fifo.data_written_event());

        // =====================================================================
        // Process PT Hit Response
        // =====================================================================
        iommu::CacheMessage resp;
        while (cache_sub.pt_hit_response_fifo.nb_read(resp)) {
            printf("[PT_CACHE] task_id=%u -> HIT response received\n", resp.task_id);
            fflush(stdout);

            pt_cache_mtx.lock();
            if (pt_cache_pending_tasks.find(resp.task_id) != pt_cache_pending_tasks.end()) {
                iommu_task_t* task = pt_cache_pending_tasks[resp.task_id];
                pt_cache_pending_tasks.erase(resp.task_id);
                pt_cache_mtx.unlock();

                // Convert CacheMessage response to task
                pt_hit_response_to_task(resp, task);

                // Route to forwarder
                printf("[PT_CACHE] task_id=%u -> HIT, routing to fwd_fifo (pa=0x%lx)\n",
                       task->task_id, task->pa);
                fflush(stdout);
                pt_cache_to_fwd_fifo.write(task);
            } else {
                pt_cache_mtx.unlock();
                printf("[PT_CACHE] WARNING: task_id=%u not found in pending map!\n", resp.task_id);
                fflush(stdout);
            }
        }

        // =====================================================================
        // Process PT Miss Response
        // =====================================================================
        while (cache_sub.pt_miss_response_fifo.nb_read(resp)) {
            printf("[PT_CACHE] task_id=%u -> MISS response received\n", resp.task_id);
            fflush(stdout);

            pt_cache_mtx.lock();
            if (pt_cache_pending_tasks.find(resp.task_id) != pt_cache_pending_tasks.end()) {
                iommu_task_t* task = pt_cache_pending_tasks[resp.task_id];
                pt_cache_pending_tasks.erase(resp.task_id);
                pt_cache_mtx.unlock();

                // Convert CacheMessage response to task
                pt_miss_response_to_task(resp, task);

                // Route to PTW for page table walk
                printf("[PT_CACHE] task_id=%u -> MISS, routing to ptw_fifo\n",
                       task->task_id);
                fflush(stdout);
                pt_cache_to_ptw_fifo.write(task);
            } else {
                pt_cache_mtx.unlock();
                printf("[PT_CACHE] WARNING: task_id=%u not found in pending map!\n", resp.task_id);
                fflush(stdout);
            }
        }
    }
}
