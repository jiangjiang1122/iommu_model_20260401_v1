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

                // NEW: 检查是否为占位CL命中
                if (resp.pt_data.reserved.is_ph == 1) {
                    // ===== 占位CL命中：挂入Buffer链表 =====
                    printf("[DEDUP] task_id=%u -> Placeholder HIT (head_index=%u)\n",
                           task->task_id, resp.pt_data.reserved.head_index);
                    fflush(stdout);
                    
                    // 步骤1: 分配新Buffer Entry
                    pt_dedup_buffer_mtx.lock();
                    uint8_t new_idx = pt_dedup_buffer.allocate_entry();
                    if (new_idx == DEDUP_BUFFER_INVALID_IDX) {
                        pt_dedup_buffer_mtx.unlock();
                        printf("[DEDUP] ERROR: Buffer full! Cannot allocate entry for task_id=%u\n", task->task_id);
                        fflush(stdout);
                        // Buffer满，直接转发到PTW（降级处理）
                        task->state = TASK_PTW_REQ;
                        pt_cache_to_ptw_fifo.write(task);
                        continue;
                    }
                    
                    // 步骤2: 填充Buffer Entry
                    auto& new_entry = pt_dedup_buffer.entries[new_idx];
                    new_entry.gscid = task->GSCID;
                    new_entry.pscid = task->PSCID;
                    new_entry.iova = task->iova;
                    new_entry.stage = task->walk_ctx.walk_type == WALK_VS_PT ? 
                                      iommu::TransStage::STAGE1_AND_2 : iommu::TransStage::STAGE2_ONLY;
                    new_entry.sv48 = (task->iosatp.MODE == IOSATP_Sv48);
                    new_entry.task_ptr = task;
                    new_entry.next_index = DEDUP_BUFFER_INVALID_IDX;
                    
                    // 步骤3: 获取链头索引
                    uint8_t head_idx = resp.pt_data.reserved.head_index;
                    
                    // 步骤4: 挂接到链表 (更新PT Cache的tail_index)
                    uint8_t old_tail = resp.pt_data.reserved.tail_index;
                    
                    printf("[DEDUP] task_id=%u -> HIT placeholder: head=%u, old_tail=%u, new=%u\n",
                           task->task_id, head_idx, old_tail, new_idx);
                    fflush(stdout);
                    
                    // 更新PT Cache的tail_index (通过batch_update_placeholders)
                    // 注意: tail_index存储在PT Cache中,不在Buffer中
                    
                    pt_dedup_buffer_mtx.unlock();
                    
                    // 步骤5: 任务挂起
                    task->state = TASK_PTW_REQ;
                    task->dedup_head_index = head_idx;
                    
                    printf("[DEDUP] task_id=%u -> Buffered at entry[%u], chain: [%u]...[%u] -> [%u]\n",
                           task->task_id, new_idx, head_idx, old_tail, new_idx);
                    fflush(stdout);
                    
                } else {
                    // ===== 常规CL命中：直接转发 =====
                    // [AD] 检查 A/D 位是否需要更新
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
            // DEBUG: 打印MISS的详细信息
            printf("[PT_CACHE] task_id=%u -> MISS response received (iova=0x%lx, aligned=0x%lx)\n",
                   resp.task_id, resp.iova, resp.iova & ~0xFFFULL);
            fflush(stdout);

            pt_cache_mtx.lock();
            if (pt_cache_pending_tasks.find(resp.task_id) != pt_cache_pending_tasks.end()) {
                iommu_task_t* task = pt_cache_pending_tasks[resp.task_id];
                pt_cache_pending_tasks.erase(resp.task_id);
                pt_cache_mtx.unlock();

                // Convert CacheMessage response to task
                pt_miss_response_to_task(resp, task);

                // =====================================================================
                // NEW: PT Cache去重+预取逻辑（4KB页简化版）
                // =====================================================================
                if (PT_CACHE_DEDUP_ENABLED) {
                    // 步骤1: 分配Buffer Entry（链头）
                    pt_dedup_buffer_mtx.lock();
                    uint8_t head_idx = pt_dedup_buffer.allocate_entry();
                    if (head_idx == DEDUP_BUFFER_INVALID_IDX) {
                        pt_dedup_buffer_mtx.unlock();
                        printf("[DEDUP] ERROR: Buffer full! Cannot allocate entry for task_id=%u\n", task->task_id);
                        fflush(stdout);
                        // Buffer满，降级处理：直接发PTW
                        task->state = TASK_PTW_REQ;
                        pt_cache_to_ptw_fifo.write(task);
                        continue;
                    }
                    
                    // 步骤2: 填充链头Buffer Entry
                    auto& head_entry = pt_dedup_buffer.entries[head_idx];
                    head_entry.gscid = task->GSCID;
                    head_entry.pscid = task->PSCID;
                    head_entry.iova = task->iova;
                    head_entry.stage = (task->walk_ctx.walk_type == WALK_VS_PT || 
                                       task->walk_ctx.walk_type == WALK_G_PT) ? 
                                      iommu::TransStage::STAGE1_AND_2 : iommu::TransStage::STAGE2_ONLY;
                    head_entry.sv48 = (task->iosatp.MODE == IOSATP_Sv48);
                    head_entry.gstage_x4 = (task->iohgatp.MODE == IOHGATP_Sv48x4);
                    head_entry.task_ptr = task;
                    head_entry.next_index = DEDUP_BUFFER_INVALID_IDX;
                    pt_dedup_buffer_mtx.unlock();
                    
                    // 步骤3: 插入主占位CL到PT Cache
                    iommu::TransStage stage = head_entry.stage;
                    bool sv48 = head_entry.sv48;
                    bool gstage_x4 = head_entry.gstage_x4;
                    uint64_t page_iova = task->iova & ~0xFFFULL;  // 4KB页对齐
                                        
                    sc_time latency;
                    bool insert_success = cache_sub.pt_cache().insert_placeholder(task->GSCID, task->PSCID, page_iova,
                                                           stage, sv48, gstage_x4, head_idx, head_idx, true, &latency);
                    
                    // [兜底逻辑] 检查主占位CL插入是否成功
                    if (!insert_success) {
                        // 替换失败：所有way都是is_req=1的占位CL
                        // 兜底策略：不记录到Buffer和Cache，直接转发到PTW
                        printf("[DEDUP_FALLBACK] task_id=%u -> Main placeholder insert FAILED, fallback to direct PTW\n",
                               task->task_id);
                        fflush(stdout);
                        
                        // 释放刚才分配的Buffer entry
                        pt_dedup_buffer.free_entry(head_idx);
                        pt_dedup_buffer_mtx.unlock();
                        
                        // 直接发送PTW，不设置dedup相关字段
                        task->walk_ctx.prefetch_enabled = false;  // 不使用预取
                        task->walk_ctx.prefetch_depth = 0;
                        task->state = TASK_PTW_REQ;
                        
                        pt_cache_to_ptw_fifo.write(task);
                        continue;  // 任务已发送，继续处理下一个响应
                    }
                                        
                    printf("[DEDUP] task_id=%u -> Main placeholder created (head_index=%u, iova=0x%lx)\n",
                           task->task_id, head_idx, page_iova);
                    fflush(stdout);
                    pt_dedup_buffer_mtx.unlock();  // 插入成功，释放Buffer锁
                    
                    // 步骤4: 预读D个占位CL（D=PT_DEDUP_PREFETCH_DEPTH）
                    uint32_t prefetch_depth = PT_DEDUP_PREFETCH_DEPTH;
                    bool prefetch_insert_failed = false;
                    
                    if (prefetch_depth > 0) {
                        std::vector<uint64_t> prefetch_iovas;
                        for (uint32_t d = 0; d < prefetch_depth; d++) {
                            uint64_t prefetch_iova = page_iova + (d + 1) * 0x1000;  // 4KB间隔
                            
                            // 插入预取占位CL (is_req=false, head_index=0xFF)
                            bool prefetch_success = cache_sub.pt_cache().insert_placeholder(task->GSCID, task->PSCID, prefetch_iova,
                                                                   stage, sv48, gstage_x4, 
                                                                   DEDUP_BUFFER_INVALID_IDX, DEDUP_BUFFER_INVALID_IDX, false, &latency);
                            
                            if (!prefetch_success) {
                                // [兜底逻辑] 预取占位CL插入失败，直接丢弃，不占用Cache资源
                                printf("[DEDUP_FALLBACK] Prefetch placeholder[%u] iova=0x%lx insert FAILED, discarded\n",
                                       d, prefetch_iova);
                                fflush(stdout);
                                prefetch_insert_failed = true;
                                // 继续尝试插入后续的预取占位CL
                            } else {
                                prefetch_iovas.push_back(prefetch_iova);
                                
                                printf("[DEDUP] Prefetch placeholder[%u]: iova=0x%lx, head_index=%u\n",
                                       d, prefetch_iova, head_idx);
                            }
                        }
                        fflush(stdout);
                        
                        // 步骤5: 发送PTW请求（带预取标记）
                        task->walk_ctx.prefetch_enabled = true;
                        task->walk_ctx.prefetch_depth = prefetch_depth;
                        task->walk_ctx.prefetch_base_iova = page_iova;
                        task->walk_ctx.prefetch_count = 0;
                        task->walk_ctx.pt_update_count = 1 + prefetch_depth;  // 1主 + D预取
                        
                        // 填充预取IOVA列表
                        for (uint32_t d = 0; d < prefetch_depth; d++) {
                            task->walk_ctx.prefetch_iovas[d] = prefetch_iovas[d];
                        }
                        
                        // 填充主任务更新信息
                        task->walk_ctx.pt_updates[0].iova = page_iova;
                        task->walk_ctx.pt_updates[0].vs_pte.raw = 0;  // 待PTW填充
                        task->walk_ctx.pt_updates[0].g_pte.raw = 0;
                        task->walk_ctx.pt_updates[0].pa = 0;
                        task->walk_ctx.pt_updates[0].page_sz = 0x1000;  // 4KB
                        
                        task->dedup_head_index = head_idx;
                        task->state = TASK_PTW_REQ;
                        
                        printf("[DEDUP] task_id=%u -> Send PTW with prefetch (depth=%u, total_iovas=%u)\n",
                               task->task_id, prefetch_depth, task->walk_ctx.pt_update_count);
                        fflush(stdout);
                    } else {
                        // D=0：无预取模式，发送普通PTW
                        task->walk_ctx.prefetch_enabled = false;
                        task->walk_ctx.prefetch_depth = 0;
                        task->walk_ctx.pt_update_count = 1;
                        task->walk_ctx.pt_updates[0].iova = page_iova;
                        task->walk_ctx.pt_updates[0].vs_pte.raw = 0;
                        task->walk_ctx.pt_updates[0].g_pte.raw = 0;
                        task->walk_ctx.pt_updates[0].pa = 0;
                        task->walk_ctx.pt_updates[0].page_sz = 0x1000;
                        
                        task->dedup_head_index = head_idx;
                        task->state = TASK_PTW_REQ;
                        
                        printf("[DEDUP] task_id=%u -> Send PTW without prefetch (D=0)\n",
                               task->task_id);
                        fflush(stdout);
                    }
                    
                    pt_cache_to_ptw_fifo.write(task);
                    continue;  // 任务已发送，继续处理下一个响应
                }

                // ===== 旧VA Dedup Check（保留但功能已关闭） =====
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
