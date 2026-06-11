// IOMMU Performance Model - PT Dedup Buffer Flush Logic
// Implements flush_dedup_buffer_chain() for PT Cache deduplication + prefetch
// v3.0: tail_index已移至Buffer entry, 批量更新占位CL

#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>

// ============================================================
// flush_single_pt_cache - 刷新单个PT Cache条目
// 
// 功能:
// 1. 查询PT Cache (iova_aligned)
// 2. MISS: 新建常规CL
// 3. HIT常规CL: 更新PTE
// 4. HIT占位CL: 
//    - 4KB页: 占位CL → 常规CL (is_ph: 1→0)
//    - 大页: 删除占位CL (valid: 1→0)
//    - 处理Buffer链表 (仅is_req=1时)
// ============================================================
void iommu_top::flush_single_pt_cache(uint64_t iova, 
                                       const iommu::PTData& pt_data,
                                       uint64_t page_size,
                                       iommu::gscid_t gscid,
                                       iommu::pscid_t pscid,
                                       iommu::TransStage stage,
                                       bool sv48,
                                       bool gstage_x4) {
    uint64_t iova_aligned = iova & ~0xFFFULL;  // 4KB对齐
    
    // 查询PT Cache
    iommu::PTData existing_data;
    sc_time latency;
    bool hit = cache_sub.pt_cache().lookup_pt(gscid, pscid, iova_aligned, 
                                               stage, sv48, gstage_x4,
                                               existing_data, latency);
    
    if (!hit) {
        // [MISS]: 未命中
        if (page_size > 0x1000) {
            // 大页: 不写入PT Cache,直接结束
            printf("[PT_FLUSH] iova=0x%lx -> MISS, large page (0x%lx), skip PT Cache\n",
                   iova, page_size);
            fflush(stdout);
            return;
        } else {
            // 4KB页: 新建常规CL
            cache_sub.pt_cache().fill_pt(gscid, pscid, iova_aligned, stage, pt_data, false);
            printf("[PT_FLUSH] iova=0x%lx -> MISS, new regular CL\n", iova_aligned);
            fflush(stdout);
            return;
        }
    }
    
    // [HIT]: 命中
    if (existing_data.reserved.is_ph == 0) {
        // [HIT 常规CL]: 更新Cache line信息
        cache_sub.pt_cache().fill_pt(gscid, pscid, iova_aligned, stage, pt_data, false);
        printf("[PT_FLUSH] iova=0x%lx -> HIT regular CL, updated\n", iova_aligned);
        fflush(stdout);
        return;
    }
    
    // [HIT 占位CL]: 复杂处理
    uint16_t head_index = existing_data.reserved.head_index;
    uint8_t is_req = existing_data.reserved.is_req;
    
    printf("[PT_FLUSH] iova=0x%lx -> HIT placeholder CL (head=%u, is_req=%u)\n",
           iova_aligned, head_index, is_req);
    fflush(stdout);
    
    // Step 1: 更新占位CL状态
    if (page_size == 0x1000) {
        // 4KB页: 占位CL → 常规CL
        iommu::PTData regular_data = pt_data;
        regular_data.reserved.is_ph = 0;
        regular_data.reserved.head_index = 0xFFFF;
        regular_data.reserved.is_req = 0;
        
        cache_sub.pt_cache().fill_pt(gscid, pscid, iova_aligned, stage, regular_data, false);
        printf("[PT_FLUSH] iova=0x%lx -> Placeholder -> Regular CL\n", iova_aligned);
        fflush(stdout);
    } else {
        // 大页: 删除占位CL
        // 通过invalidate实现
        cache_sub.pt_cache().invalidate_vma(gscid, pscid, iova_aligned,
                                            true, true, true,
                                            iommu::CacheInvalidateMode::PRECISE);
        printf("[PT_FLUSH] iova=0x%lx -> Large page, placeholder deleted\n", iova_aligned);
        fflush(stdout);
        return;  // 大页不处理Buffer链表
    }
    
    // Step 2: 处理Buffer链表 (仅is_req=1时)
    if (is_req == 0) {
        printf("[PT_FLUSH] iova=0x%lx -> Prefetch placeholder, no buffer chain\n", iova_aligned);
        fflush(stdout);
        return;  // 预取占位CL,无Buffer链表
    }
    
    // is_req=1: 主任务占位CL,Buffer链表已在flush_dedup_buffer_chain中处理
    printf("[PT_FLUSH] iova=0x%lx -> Main task placeholder, buffer chain handled separately\n",
           iova_aligned);
    fflush(stdout);
}

// ============================================================
// flush_dedup_buffer_chain - 刷新Dedup Buffer链表
// 
// 功能:
// 1. 遍历Buffer链表(从head_index开始)
// 2. 为每个挂起任务计算PA (vs_pte.PPN << 12 | iova_offset)
// 3. 唤醒所有挂起任务(发送到pt_cache_to_fwd_fifo)
// 4. 释放所有Buffer Entry
//
// 参数:
// - head_index: Buffer链头索引
// - group_id: 预取组ID(用于日志)
// - main_task: 主任务指针(通过walk_ctx.pt_updates访问PTE)
// - group_iovas: 预取组IOVA列表
// - total_tasks: 预取组总任务数(1+D)
// ============================================================
void iommu_top::flush_dedup_buffer_chain(uint16_t head_index, uint32_t group_id,
                                         iommu_task_t* main_task,
                                         const uint64_t* group_iovas,
                                         uint32_t total_tasks) {
    if (head_index == DEDUP_BUFFER_INVALID_IDX) {
        printf("[DEDUP_FLUSH] group_id=%u -> Invalid head_index, skip flush\n", group_id);
        fflush(stdout);
        return;
    }
    
    printf("[DEDUP_FLUSH] group_id=%u -> Flushing buffer chain from head[%u]\n",
           group_id, head_index);
    fflush(stdout);
    
    // 注意: batch_update_placeholders已在Monitor中调用,此处不再重复
    
    // [FIX] 使用CacheSubsystem的dedup_buffer_（而非iommu_top::pt_dedup_buffer）
    auto* dedup_buf = cache_sub.get_pt_dedup_buffer();
    if (dedup_buf == nullptr) {
        printf("[DEDUP_FLUSH] group_id=%u -> ERROR: dedup_buffer is nullptr!\n", group_id);
        fflush(stdout);
        return;
    }
    
    uint16_t cur = head_index;
    uint32_t flushed_count = 0;
    
    // 遍历Buffer链表
    // [FIX] 核心优化: 先释放Buffer Entry, 再写FIFO
    // 这样即使FIFO阻塞(depth=4), buffer entry已经释放,
    // 其他等待分配entry的任务(pt_worker_thread)可以立即使用
    while (cur != DEDUP_BUFFER_INVALID_IDX) {
        auto& entry = dedup_buf->entries[cur];
        uint16_t next_idx = entry.next_index;
        iommu_task_t* pending_task = entry.task_ptr;  // 保存task_ptr(free后会清空)
        
        if (pending_task != nullptr) {
            // 计算PA：使用对应IOVA的PTE.PPN + task的offset
            uint64_t page_iova = pending_task->iova & ~0xFFFULL;  // 4KB页对齐
            uint64_t offset = pending_task->iova & 0xFFFL;         // 页内偏移
            
            // [FIX] 使用main_task->walk_ctx.pt_updates访问PTE数据
            bool found = false;
            for (uint32_t i = 0; i < total_tasks; i++) {
                if (group_iovas[i] == page_iova) {
                    // 找到匹配的IOVA，计算PA
                    uint64_t pa = (main_task->walk_ctx.pt_updates[i].vs_pte.PPN << 12) | offset;
                    pending_task->pa = pa;
                    pending_task->vs_pte = main_task->walk_ctx.pt_updates[i].vs_pte;
                    pending_task->g_pte = main_task->walk_ctx.pt_updates[i].g_pte;
                    pending_task->page_sz = main_task->walk_ctx.pt_updates[i].page_sz;
                    found = true;
                    
                    printf("[DEDUP_FLUSH] task_id=%u -> PA=0x%lx (iova=0x%lx, PPN=0x%lx, offset=0x%lx)\n",
                           pending_task->task_id, pa, pending_task->iova,
                           main_task->walk_ctx.pt_updates[i].vs_pte.PPN, offset);
                    fflush(stdout);
                    break;
                }
            }
            
            if (!found) {
                // 未找到对应PTE，使用主任务的PPN（同页场景）
                uint64_t pa = (main_task->walk_ctx.pt_updates[0].vs_pte.PPN << 12) | offset;
                pending_task->pa = pa;
                pending_task->vs_pte = main_task->walk_ctx.pt_updates[0].vs_pte;
                pending_task->g_pte = main_task->walk_ctx.pt_updates[0].g_pte;
                pending_task->page_sz = main_task->walk_ctx.pt_updates[0].page_sz;
                
                printf("[DEDUP_FLUSH] task_id=%u -> PA=0x%lx (using main PPN=0x%lx, iova=0x%lx)\n",
                       pending_task->task_id, pa, main_task->walk_ctx.pt_updates[0].vs_pte.PPN, pending_task->iova);
                fflush(stdout);
            }
            
            // 设置任务状态为PTW完成
            pending_task->state = TASK_PTW_DONE;
        }
        
        // [FIX] 先释放Buffer Entry（在FIFO write之前!）
        // 确保即使FIFO阻塞, entry也已释放, pt_worker_thread可以立即分配
        dedup_buf->free_entry(cur);
        flushed_count++;
        
        printf("[DEDUP_FLUSH] entry[%u] freed first (task_ptr=%p, next=%u) -> forwarding\n",
               cur, (void*)pending_task, next_idx);
        fflush(stdout);
        
        // 发送到forwarder（可能阻塞, 但entry已释放）
        if (pending_task != nullptr) {
            pt_cache_to_fwd_fifo.write(pending_task);
        }
        
        // 移动到下一个
        cur = next_idx;
    }
    
    printf("[DEDUP_FLUSH] group_id=%u -> Chain flush completed (%u tasks flushed)\n",
           group_id, flushed_count);
    fflush(stdout);
}
