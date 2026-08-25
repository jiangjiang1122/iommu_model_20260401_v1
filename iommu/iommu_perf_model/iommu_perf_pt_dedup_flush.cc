// IOMMU Performance Model - PT Dedup Buffer Flush (重构版)
// 提供 dedup_flush_chain_cb: 由 CacheSubsystem::execute_dedup_update 通过回调触发
// 遍历 dedup Buffer 任务链, 计算 PA, 转发挂起任务到 pt_cache_to_fwd_fifo。
//
// [重构说明]
// - 去重/预取占位不再存于 PT Cache, 而在独立的 dedup_cache 中(CacheSubsystem)。
// - PTW 完成后 Monitor 写 dedup_update_fifo, dedup_scheduler_thread 查 dedup_cache
//   取 head_index, 再调用本回调刷新 Buffer 链。
// - 一条 Buffer 链中的所有任务同属一个 4KB 页(命中同一主占位), 共享同一 PTE,
//   仅页内 offset 不同。

#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>

// ============================================================
// dedup_flush_chain_cb - 刷新 dedup Buffer 任务链(回调)
//
// 参数:
//   head_index: Buffer 链头索引(由 dedup_cache line 提供)
//   upd:        dedup_update 消息, 携带该 iova 已解析的 PTE 与 dedup_pa_base;
//               [MSI] pt_data.reserved.is_msi=1 时 dedup_pa_base 为 GPA 页基址,
//               链上任务均为MSI, 改道MSIPT大模块(点4/点6)
//
// 流程(对应设计"任务2: 刷新 dedup buffer"):
//   1. 从 head_index 沿 next_index 遍历链
//   2. 普通: PA = pa_base | (iova & 0xFFF) -> pt_cache_to_fwd_fifo
//      MSI : GPA = gpa_base | (iova & 0xFFF) -> route_to_msipt
//   3. 置 task->state = TASK_PTW_DONE
//   4. 先 free_entry(释放 Buffer, 避免 FIFO 阻塞占用), 再转发
// ============================================================
void iommu_top::dedup_flush_chain_cb(uint16_t head_index,
                                     const iommu::CacheMessage& upd) {
    if (head_index == DEDUP_BUFFER_INVALID_IDX) {
        return;
    }

    auto* dedup_buf = cache_sub.get_pt_dedup_buffer();
    if (dedup_buf == nullptr) {
        printf("[DEDUP_FLUSH_CB] ERROR: dedup_buffer is nullptr!\n");
        fflush(stdout);
        return;
    }

    const bool is_msi_flush = (upd.pt_data.reserved.is_msi != 0);
    const uint64_t pa_base = upd.dedup_pa_base & ~0xFFFULL;  // MSI时为GPA页基址
    uint16_t cur = head_index;
    uint32_t flushed_count = 0;

    while (cur != DEDUP_BUFFER_INVALID_IDX) {
        auto& entry = dedup_buf->entries[cur];
        uint16_t next_idx = entry.next_index;
        iommu_task_t* pending_task = entry.task_ptr;  // free 前保存

        if (pending_task != nullptr) {
            uint64_t offset = pending_task->iova & 0xFFFULL;
            pending_task->pa = pa_base | offset;
            pending_task->vs_pte.raw = upd.pt_data.vs_pte.raw;
            pending_task->g_pte.raw = upd.pt_data.g_pte.raw;
            pending_task->page_sz = 0x1000ULL;  // 4KB(大页暂不实现)
            pending_task->state = TASK_PTW_DONE;
            if (is_msi_flush) {
                // [MSI] 同页任务均为MSI(点11保证), 置GPA后改道MSIPT模块
                pending_task->gpa = pa_base | offset;
                pending_task->is_msi = 1;
                pending_task->walk_ctx.msi_index =
                    msi_extract(pending_task->gpa >> 12,
                                pending_task->DC.msi_addr_mask.mask &
                                ((1ULL << (calculate_mgpaw(&iommu_inst) - 12)) - 1));
            }

            printf("[DEDUP_FLUSH_CB] task_id=%u -> %s=0x%lx (iova=0x%lx, page_base=0x%lx, offset=0x%lx) [buf_idx=%u]\n",
                   pending_task->task_id, is_msi_flush ? "GPA(MSI)" : "PA",
                   pending_task->pa, pending_task->iova,
                   pa_base, offset, cur);
            fflush(stdout);
        }

        // 先释放 Buffer Entry(在 FIFO write 之前), 确保即使 FIFO 阻塞 entry 也已释放
        dedup_buf->free_entry(cur);
        flushed_count++;

        // 转发(可能阻塞, 但 entry 已释放): MSI改道MSIPT, 普通转发forwarder
        if (pending_task != nullptr) {
            if (is_msi_flush) {
                route_to_msipt(pending_task);
            } else {
                pt_cache_to_fwd_fifo.write(pending_task);
            }
        }

        cur = next_idx;
    }

    printf("[DEDUP_FLUSH_CB] head=%u -> chain flush completed (%u tasks flushed%s)\n",
           head_index, flushed_count, is_msi_flush ? ", MSI routed" : "");
    fflush(stdout);
}
