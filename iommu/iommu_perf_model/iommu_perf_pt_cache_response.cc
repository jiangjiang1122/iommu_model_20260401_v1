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

                // [SIMPLIFIED] 占位CL和常规CL统一处理：直接转发
                // Buffer操作已在execute_pt_request中完成，collector不关心Buffer细节
                
                // 检查 A/D 位是否需要更新
                bool need_ad_update = (task->vs_pte.A == 0 || (task->is_write && task->vs_pte.D == 0));
                
                if (need_ad_update && task->SADE == 1) {
                    // A/D 未设置且 SADE=1，需要送 PTW 进行硬件更新
                    printf("[PT_CACHE] task_id=%u -> HIT but A/D update needed (A=%d, D=%d), routing to PTW\n",
                           task->task_id, task->vs_pte.A, task->vs_pte.D);
                    fflush(stdout);
                    
                    task->state = TASK_PTW_REQ;
                    pt_cache_to_ptw_fifo.write(task);
                } else {
                    // A/D 已设置或 SADE=0，直接转发
                    printf("[PT_CACHE] task_id=%u -> HIT, routing to fwd_fifo (pa=0x%lx, A=%d, D=%d, is_ph=%d)\n",
                           task->task_id, task->pa, task->vs_pte.A, task->vs_pte.D,
                           resp.pt_data.reserved.is_ph);
                    fflush(stdout);
                    pt_cache_to_fwd_fifo.write(task);
                }
            } else {
                pt_cache_mtx.unlock();
                printf("[PT_CACHE] WARNING: task_id=%u not found in pending map!\n", resp.task_id);
                fflush(stdout);
            }
        }

        // =====================================================================
        // Process PT Miss Response
        // [NOTE] 正常情况下不会到达这里，因为execute_pt_request会在MISS时插入占位CL并返回HIT
        // 保留此分支作为兜底处理
        // =====================================================================
        while (cache_sub.pt_miss_response_fifo.nb_read(resp)) {
            // DEBUG: 打印MISS的详细信息
            printf("[PT_CACHE] task_id=%u -> MISS response received (UNEXPECTED, should be handled by execute_pt_request)\n",
                   resp.task_id);
            fflush(stdout);

            pt_cache_mtx.lock();
            if (pt_cache_pending_tasks.find(resp.task_id) != pt_cache_pending_tasks.end()) {
                iommu_task_t* task = pt_cache_pending_tasks[resp.task_id];
                pt_cache_pending_tasks.erase(resp.task_id);
                pt_cache_mtx.unlock();

                // Convert CacheMessage response to task
                pt_miss_response_to_task(resp, task);

                // [FALLBACK] 直接发送PTW，不进行去重
                printf("[PT_CACHE_FALLBACK] task_id=%u -> Direct PTW (dedup not available)\n",
                       task->task_id);
                fflush(stdout);
                
                task->walk_ctx.prefetch_enabled = false;
                task->walk_ctx.prefetch_depth = 0;
                task->state = TASK_PTW_REQ;
                
                pt_cache_to_ptw_fifo.write(task);
            } else {
                pt_cache_mtx.unlock();
                printf("[PT_CACHE] WARNING: task_id=%u not found in pending map!\n", resp.task_id);
                fflush(stdout);
            }
        }
    }
}
