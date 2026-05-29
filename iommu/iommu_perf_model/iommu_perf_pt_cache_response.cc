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

                // [AD] 检查 A/D 位是否需要更新
                // 如果 Cache 中的 PTE 的 A/D 未设置，仍需送 PTW 进行硬件更新
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
                    printf("[PT_CACHE] task_id=%u -> HIT, routing to fwd_fifo (pa=0x%lx, A=%d, D=%d)\n",
                           task->task_id, task->pa, task->vs_pte.A, task->vs_pte.D);
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

                // ===== VA Dedup Check =====
                if (PT_CACHE_VA_DEDUP_ENABLED) {
                    va_dedup_key_t key;
                    key.gscid = task->GSCID;
                    key.pscid = task->PSCID;
                    key.page_iova = task->iova & ~0xFFFULL;

                    va_dedup_mtx.lock();
                    auto dedup_it = va_dedup_table.find(key);
                    if (dedup_it != va_dedup_table.end() && dedup_it->second.valid) {
                        // Dedup HIT: 挂入等待列表，不发PTW
                        dedup_it->second.pending_tasks.push_back(task);
                        va_dedup_hit_count++;
                        va_dedup_mtx.unlock();
                        printf("[VA_DEDUP] task_id=%u -> HIT (page=0x%lx, pending=%zu)\n",
                               task->task_id, key.page_iova, dedup_it->second.pending_tasks.size());
                        fflush(stdout);
                        continue;  // 跳过发PTW
                    } else {
                        // Dedup MISS: 建新表项
                        va_dedup_entry_t entry;
                        entry.valid = true;
                        entry.first_task_id = task->task_id;
                        va_dedup_table[key] = entry;
                        va_dedup_miss_count++;
                        va_dedup_mtx.unlock();
                        printf("[VA_DEDUP] task_id=%u -> MISS (new page=0x%lx)\n",
                               task->task_id, key.page_iova);
                        fflush(stdout);
                    }
                }

                // Route to PTW for page table walk (仅 dedup miss 或功能关闭时到达此处)
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
