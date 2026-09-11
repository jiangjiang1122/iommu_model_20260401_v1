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
            // [STAT] PT Cache 输出间隔统计
            pt_cache_out_total_count++;
            {
                const double out_ns = sc_time_stamp().to_seconds() * 1e9;
                if (pt_cache_out_last_ns > 0.0) {
                    double interval = out_ns - pt_cache_out_last_ns;
                    pt_cache_out_interval_total_ns += interval;
                    if (interval > pt_cache_out_interval_max_ns)
                        pt_cache_out_interval_max_ns = interval;
                    if (interval < pt_cache_out_interval_min_ns)
                        pt_cache_out_interval_min_ns = interval;
                    pt_cache_out_interval_count++;
                }
                pt_cache_out_last_ns = out_ns;
            }
            
            pt_cache_mtx.lock();
            if (pt_cache_pending_tasks.find(resp.task_id) != pt_cache_pending_tasks.end()) {
                iommu_task_t* task = pt_cache_pending_tasks[resp.task_id];
                pt_cache_pending_tasks.erase(resp.task_id);
                pt_cache_mtx.unlock();

                // [STAT] 控制包(SQ/CQ/MSI) PT Cache 命中统计
                if (task->is_ctrl) {
                    ctrl_pt_cache_hit_count++;
                }

                // [重构] 区分两类 pt_hit_response:
                //   dedup_suspended=false: PT Cache 常规CL HIT -> 转换并转发 Forwarder
                //   dedup_suspended=true : dedup 占位CL HIT -> 任务已挂 dedup Buffer, 不转发
                bool is_placeholder = resp.dedup_suspended;
            
                if (!is_placeholder) {
                    // 常规CL HIT: 转换响应并直接转发到Forwarder
                    // [前置] PT HIT直接输出: 先移除walker前置pending表项,
                    // 后到的前置响应将被丢弃, 防止响应线程触碰已释放的task
                    if (PTW_WALKER_CACHE_ENABLED && WALKER_FRONT_ENABLED) {
                        walker_front_mtx.lock();
                        walker_front_pending.erase(task->task_id);
                        walker_front_mtx.unlock();
                    }
                    pt_hit_response_to_task(resp, task);

                    // [MSI] 点8: HIT条目is_msi=1 -> 保存的是iova->gpa映射,
                    // 恢复GPA后派发MSIPT大模块执行; is_msi=0 -> 直接forward(写保序)
                    if (resp.pt_data.reserved.is_msi) {
                        task->gpa = (resp.pt_data.vs_pte.PPN << 12) |
                                    (task->iova & 0xFFFULL);
                        task->is_msi = 1;
                        task->walk_ctx.msi_index =
                            msi_extract(task->gpa >> 12,
                                        task->DC.msi_addr_mask.mask &
                                        ((1ULL << (calculate_mgpaw(&iommu_inst) - 12)) - 1));
                        printf("[PT_CACHE] task_id=%u -> HIT is_msi=1 (gpa=0x%lx, msi_index=%llu), dispatch to MSIPT module\n",
                               task->task_id, task->gpa,
                               (unsigned long long)task->walk_ctx.msi_index);
                        fflush(stdout);
                        route_to_msipt(task);
                    } else {
                        printf("[PT_CACHE] task_id=%u -> HIT regular CL, routing to fwd_fifo (pa=0x%lx)\n",
                               task->task_id, task->pa);
                        fflush(stdout);
                        pt_cache_to_fwd_fifo.write(task);
                    }
                } else {
                    // dedup 占位CL HIT: 任务已在 execute_dedup_request 中挂接到 dedup Buffer
                    // 不发PTW, 等待 dedup_update 刷新 Buffer 时转发
                    printf("[PT_CACHE] task_id=%u -> HIT dedup placeholder, task suspended in Buffer (no PTW)\n",
                           task->task_id);
                    fflush(stdout);
                    // 注意：不能delete task, dedup flush回调会转发任务
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
            // [STAT] PT Cache 输出间隔统计
            pt_cache_out_total_count++;
            {
                const double out_ns = sc_time_stamp().to_seconds() * 1e9;
                if (pt_cache_out_last_ns > 0.0) {
                    double interval = out_ns - pt_cache_out_last_ns;
                    pt_cache_out_interval_total_ns += interval;
                    if (interval > pt_cache_out_interval_max_ns)
                        pt_cache_out_interval_max_ns = interval;
                    if (interval < pt_cache_out_interval_min_ns)
                        pt_cache_out_interval_min_ns = interval;
                    pt_cache_out_interval_count++;
                }
                pt_cache_out_last_ns = out_ns;
            }
            
            printf("[PT_CACHE] task_id=%u -> MISS response received\n", resp.task_id);
            fflush(stdout);

            pt_cache_mtx.lock();
            if (pt_cache_pending_tasks.find(resp.task_id) != pt_cache_pending_tasks.end()) {
                iommu_task_t* task = pt_cache_pending_tasks[resp.task_id];
                pt_cache_pending_tasks.erase(resp.task_id);
                pt_cache_mtx.unlock();

                // [STAT] 控制包(SQ/CQ/MSI) PT Cache 缺失统计
                if (task->is_ctrl) {
                    ctrl_pt_cache_miss_count++;
                    printf("[STAT_CTRL] task_id=%u is_ctrl=%u PT Cache MISS -> entering dedup/PTW\n",
                           task->task_id, task->is_ctrl);
                    fflush(stdout);
                }

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
