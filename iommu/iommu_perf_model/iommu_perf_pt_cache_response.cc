// IOMMU Performance Model - PT Cache Response Handler Thread
// Handles PT cache hit/miss responses from CacheSubsystem

#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>

// ============================================================
// collector_pt_response_thread - Process PT cache hit/miss responses
// Reads from cache_sub.pt_hit_response_fifo and pt_miss_response_fifo
// Routes tasks to forwarder (hit) or PTW (miss)
//
// 设计文档流程:
// - HIT常规CL (is_ph=0): 直接转发到Forwarder
// - HIT占位CL (is_ph=1): 任务已在execute_pt_request中挂接到Buffer,
//   这里只记录日志,不发PTW,等待Monitor处理
// - MISS: 占位CL已在execute_pt_request中插入,
//   这里发送PTW请求(prefetch_enabled=true, depth=D)
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
            pt_cache_mtx.lock();
            if (pt_cache_pending_tasks.find(resp.task_id) != pt_cache_pending_tasks.end()) {
                iommu_task_t* task = pt_cache_pending_tasks[resp.task_id];
                pt_cache_pending_tasks.erase(resp.task_id);
                pt_cache_mtx.unlock();

                // Convert CacheMessage response to task
                pt_hit_response_to_task(resp, task);

                bool is_placeholder = (resp.pt_data.reserved.is_ph == 1);
                
                if (!is_placeholder) {
                    // 常规CL HIT: 直接转发到Forwarder
                    printf("[PT_CACHE] task_id=%u -> HIT regular CL, routing to fwd_fifo (pa=0x%lx)\n",
                           task->task_id, task->pa);
                    fflush(stdout);
                    pt_cache_to_fwd_fifo.write(task);
                } else {
                    // 占位CL HIT: 任务已在execute_pt_request中挂接到Buffer
                    // 不发PTW,等待Monitor处理
                    printf("[PT_CACHE] task_id=%u -> HIT placeholder CL (is_ph=1), task suspended in Buffer (no PTW)\n",
                           task->task_id);
                    fflush(stdout);
                    // 注意：不能delete task，Monitor会flush Buffer并转发任务
                }
            } else {
                pt_cache_mtx.unlock();
                printf("[PT_CACHE] WARNING: task_id=%u not found in pending map!\n", resp.task_id);
                fflush(stdout);
            }
        }

        // =====================================================================
        // Process PT Miss Response
        // MISS时占位CL已在execute_pt_request中插入,任务已挂接到Buffer
        // 这里发送PTW请求,触发PTW walk + Burst预取
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

                // 发送PTW请求 (带预取参数)
                // 任务已在Buffer中(execute_pt_request中分配),PTW完成后Monitor会flush
                bool prefetch_en = task->walk_ctx.prefetch_enabled;
                uint32_t prefetch_d = task->walk_ctx.prefetch_depth;
                
                printf("[PT_CACHE] task_id=%u -> MISS, sending PTW request (prefetch_enabled=%d, depth=%u)\n",
                       task->task_id, prefetch_en, prefetch_d);
                fflush(stdout);
                
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
