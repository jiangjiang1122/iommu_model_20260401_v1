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
#if TEST_CFG_MULTI_DEVICE_SCENE
    iommu_md::BusyGuard md_busy(md_trace.active);
    std::set<uint16_t> visited;
    // MSI邻页占位清理没有翻译结果；若已有等待者，交回PTW自行翻译，不能返回PA=0。
    const bool retry_without_result = upd.dedup_pa_base == 0 &&
        upd.pt_data.vs_pte.raw == 0 && upd.pt_data.g_pte.raw == 0;
#endif

    while (cur != DEDUP_BUFFER_INVALID_IDX) {
#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::require(cur < PT_DEDUP_BUFFER_SIZE && visited.insert(cur).second,
                          "去重刷新索引越界或链成环");
        const auto& checked = dedup_buf->entries[cur];
        iommu_md::require(checked.is_valid() && checked.task_ptr && checked.gscid == upd.gscid &&
            checked.pscid == upd.pscid && (checked.iova & ~0xFFFULL) == (upd.iova & ~0xFFFULL),
            "去重刷新entry身份不匹配");
        iommu_md::require(checked.task_ptr->GSCID == upd.gscid && checked.task_ptr->PSCID == upd.pscid &&
            (checked.task_ptr->iova & ~0xFFFULL) == (upd.iova & ~0xFFFULL), "去重刷新task身份不匹配");
        iommu_md::require(checked.next_index == DEDUP_BUFFER_INVALID_IDX || checked.next_index < PT_DEDUP_BUFFER_SIZE,
                          "去重刷新next索引越界");
#endif
        auto& entry = dedup_buf->entries[cur];
        uint16_t next_idx = entry.next_index;
        iommu_task_t* pending_task = entry.task_ptr;  // free 前保存

#if TEST_CFG_MULTI_DEVICE_SCENE
        if (retry_without_result) {
            pending_task->walk_ctx.dedup_bypass = true;
            pending_task->walk_ctx.prefetch_enabled = false;
            pending_task->walk_ctx.prefetch_depth = 0;
            pending_task->dedup_head_index = DEDUP_BUFFER_INVALID_IDX;
            pending_task->state = TASK_PTW_REQ;
            dedup_buf->free_entry(cur);
            md_trace.operation(pending_task->task_id, "dedup", "cleanup_rewalk",
                               pending_task->device_id, iommu_md::now_ns());
            pt_cache_to_ptw_fifo.write(pending_task);
            ++flushed_count;
            cur = next_idx;
            continue;
        }
#endif
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
#if TEST_CFG_MULTI_DEVICE_SCENE
        // [STAT] 持有延时细分: W1=alloc->组完成, W2=组完成->聚合刷新执行, W3=聚合刷新->entry释放
        if (upd.md_agg_flush_ns > 0.0 && entry.alloc_time_ns > 0.0) {
            const double t0 = entry.alloc_time_ns;
            const double t1 = upd.md_group_complete_ns > t0 ? upd.md_group_complete_ns : t0;
            const double t2 = upd.md_agg_flush_ns > t1 ? upd.md_agg_flush_ns : t1;
            const double t3 = iommu_md::now_ns();
            const double w0 = t1 - t0, w1 = t2 - t1, w2 = t3 - t2;
            const bool is_main = (cur == head_index);
            if (is_main) md_hold_main_cnt++; else md_hold_susp_cnt++;
            double* sum = is_main ? md_hold_main_sum : md_hold_susp_sum;
            double* mx  = is_main ? md_hold_main_max : md_hold_susp_max;
            sum[0] += w0; sum[1] += w1; sum[2] += w2;
            if (w0 > mx[0]) mx[0] = w0;
            if (w1 > mx[1]) mx[1] = w1;
            if (w2 > mx[2]) mx[2] = w2;
        }
        md_trace.operation(pending_task->task_id, "dedup", "flush", pending_task->device_id,
                           entry.alloc_time_ns, true, -1, 0, 0);
#endif
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
