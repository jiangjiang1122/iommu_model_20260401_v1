// IOMMU Performance Model - PTW Threads (2 threads)
// SPEC Section 12.13-12.14
// Corresponds to two_stage_address_translation() + second_stage_address_translation()

#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>
#include <vector>

// ============================================================
// 12.13 ptw_req_thread - PTW Request Dispatch (PEQ-based pipeline)
// Reads from FIFO, applies outstanding control, schedules via PEQ
// ============================================================
void iommu_top::ptw_req_thread() {
    while (true) {
        // Flow control: limit total PTW outstanding tasks
        while (ptw_outstanding_task_count >= PTW_MAX_OUTSTANDING_TASKS) {
            wait(ptw_task_completed_event);
        }

        iommu_task_t* task = pt_cache_to_ptw_fifo.read();
        // [STAT] 记录PTW任务入口时刻
        ptw_task_start_ns[task->task_id] = sc_time_stamp().to_seconds() * 1e9;
        task->state = TASK_PTW_REQ;
        ptw_outstanding_task_count++;
        if (ptw_outstanding_task_count > peak_ptw_outstanding) {
            peak_ptw_outstanding = ptw_outstanding_task_count;
            printf("[PTW_STAT] New peak outstanding=%d at task_id=%u, time=%s\n",
                   peak_ptw_outstanding, task->task_id, sc_time_stamp().to_string().c_str());
            fflush(stdout);
        }

        printf("[PTW_REQ] task_id=%u, iova=0x%lx, iosatp.MODE=%d, GV=%d -> dispatch to PEQ\n",
               task->task_id, task->iova, task->iosatp.MODE, task->GV);
        fflush(stdout);

        // Pipeline delay via PEQ (non-blocking, parallel timing)
        ptw_req_peq.notify(*task, sc_time(PTW_REQ_PIPELINE_DELAY_NS, SC_NS));
    }
}

// ============================================================
// 12.13b ptw_req_process_thread - PTW Request Processing
// Fires after PEQ delay, performs walk init and sends DDR request
// ============================================================
void iommu_top::ptw_req_process_thread() {
    while (true) {
        iommu_task_t* task = ptw_req_peq.get_next_transaction();
        if (!task) { wait(ptw_req_peq.get_event()); continue; }

        // ========== 1. Bare mode fast path ==========
        // Corresponds to iommu_two_stage_trans.cc L39-82
        if (task->iosatp.MODE == IOSATP_Bare) {
            printf("[PTW_REQ] task_id=%u -> Bare mode fast path, no page table reads\n", task->task_id);
            fflush(stdout);
            task->vs_pte.raw = 0;
            task->vs_pte.D = task->vs_pte.A = task->vs_pte.G = task->vs_pte.U = 1;
            task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = task->vs_pte.V = 1;
            task->vs_pte.PBMT = PMA;
            task->gpa = task->iova;
            task->page_sz = get_bare_page_size(&iommu_inst);

            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                // Need G-stage explicit translation
                init_gstage_walk(task, task->gpa);
                task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                task->walk_ctx.ddr_read_count = 1;

                ptw_walks_mtx.lock();
                ptw_active_walks[task->task_id] = task;
                ptw_walks_mtx.unlock();

                ddr_req_entry_t req;
                req.task_id = task->task_id;
                req.addr = task->walk_ctx.read_addr;
                req.size = task->walk_ctx.read_size;
                req.is_write = false;
                req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;  // [STAT] 记录DDR请求提交时间
                printf("[PTW_REQ] task_id=%u, Bare mode -> GS_EXPLICIT, addr=0x%lx, read_count=1 -> ddr_req\n",
                       task->task_id, req.addr);
                fflush(stdout);
                ptw_req_ddr_fifo.write(req);
                continue;
            } else {
                // G-stage also Bare: complete immediately
                task->pa = task->gpa;
                task->gst_page_sz = get_gstage_bare_page_size(&iommu_inst);
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.X = task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.PBMT = PMA;
                task->state = TASK_PTW_DONE;
                ptw_outstanding_task_count--;
                ptw_total_completed++;  // [STAT]
                // [STAT] 累加PTW完成统计（Bare直通路径）
                ptw_total_ddr_reads += task->walk_ctx.ddr_read_count;
                { auto _s = ptw_task_start_ns.find(task->task_id);
                  if (_s != ptw_task_start_ns.end()) {
                      ptw_total_exec_ns += sc_time_stamp().to_seconds()*1e9 - _s->second;
                      ptw_task_start_ns.erase(_s); } }
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                
                // Convert task to CacheMessage and write to cache_sub.pt_update_fifo
                iommu::CacheMessage pt_update_req = task_to_pt_update(task);
                cache_sub.pt_update_fifo.write(pt_update_req);
                
                // Also route to forwarder
                pt_cache_to_fwd_fifo.write(task);
                // VA Dedup: 恢复挂起的同页任务
                va_dedup_recover(task, false);
                continue;
            }
        }

        // ========== 2. VS-stage Walk Initialization ==========
        {
            // =====================================================================
            // Walker Cache Lookup (如果启用)
            // =====================================================================
            bool walker_cache_enabled = PTW_WALKER_CACHE_ENABLED;  // 从PTW模块参数读取
            bool walker_hit = false;
            
            if (walker_cache_enabled) {
                // 使用转换函数构造Walker Cache Lookup请求
                iommu::CacheMessage req = task_to_walker_request(task);
                
                // 发送Lookup请求
                cache_sub.walker_request_fifo.write(req);
                
                // 阻塞等待响应
                iommu::CacheMessage resp = cache_sub.walker_response_fifo.read();
                
                // 使用转换函数将响应写回task
                walker_response_to_task(resp, task);
                
                walker_hit = resp.hit;
                
                // 保存lookup命中层级，用于update时避免冗余更新
                if (walker_hit) {
                    task->walk_ctx.walker_hit_level = resp.walker_level;
                } else {
                    task->walk_ctx.walker_hit_level = 0;  // 全部miss
                }
            }
            
            uint16_t vpn[5] = {0};
            uint8_t LEVELS = 0, PTESIZE = 0;
            extract_vpn(task->iova, task->iosatp.MODE, task->SXL,
                        vpn, &LEVELS, &PTESIZE);

            // Canonical address check
            if (!check_canonical(task->iova, task->iosatp.MODE, task->SXL)) {
                printf("[PTW_REQ] task_id=%u -> FAULT: canonical check failed\n", task->task_id);
                fflush(stdout);
                task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                task->state = TASK_FAULT;
                ptw_outstanding_task_count--;
                ptw_total_completed++;  // [STAT]
                // [STAT] 累加PTW完成统计（canonical fault）
                ptw_total_ddr_reads += task->walk_ctx.ddr_read_count;
                { auto _s = ptw_task_start_ns.find(task->task_id);
                  if (_s != ptw_task_start_ns.end()) {
                      ptw_total_exec_ns += sc_time_stamp().to_seconds()*1e9 - _s->second;
                      ptw_task_start_ns.erase(_s); } }
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                
                // Convert task to CacheMessage and write to cache_sub.pt_update_fifo (for fault recording)
                iommu::CacheMessage pt_update_req = task_to_pt_update(task);
                cache_sub.pt_update_fifo.write(pt_update_req);
                
                // Route to fault FIFO
                collector_to_fault_fifo.write(task);
                // VA Dedup: 恢复挂起的同页任务(fault)
                va_dedup_recover(task, true);
                continue;
            }

            int8_t i = LEVELS - 1;
            uint64_t a = task->iosatp.PPN * PAGESIZE;

            // Save VPN to walk context
            for (int k = 0; k < 5; k++) task->walk_ctx.vpn[k] = vpn[k];
            task->walk_ctx.max_levels = LEVELS;
            task->walk_ctx.ptesize = PTESIZE;

            // =====================================================================
            // 根据 Walker Cache 命中情况初始化 walk
            // =====================================================================
            if (walker_cache_enabled && walker_hit) {
                // 命中：walker_response_to_task已经设置了walk starting point
                // task->walk_ctx.level 和 task->walk_ctx.base_addr 已经被设置
                
                printf("[PTW_REQ] task_id=%u, Walker hit: start from level=%d, base_addr=0x%lx\n",
                       task->task_id, task->walk_ctx.level, task->walk_ctx.base_addr);
                fflush(stdout);
            } else {
                // 未命中：从最高级开始完整 walk
                task->walk_ctx.level = i;
                task->walk_ctx.base_addr = a;
            }

            // Calculate first PTE address
            uint64_t pte_addr = task->walk_ctx.base_addr + 
                               task->walk_ctx.vpn[task->walk_ctx.level] * PTESIZE;

            // Check if G-stage implicit translation needed
            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                task->walk_ctx.pending_vs_pte_addr = pte_addr;
                task->walk_ctx.vs_level = task->walk_ctx.level;
                init_gstage_walk(task, pte_addr);
                task->walk_ctx.walk_phase = PTW_GS_IMPLICIT;
            } else {
                task->walk_ctx.read_addr = pte_addr;
                task->walk_ctx.read_size = PTESIZE;
                task->walk_ctx.walk_phase = PTW_VS_WALK;
            }

            // Register in active_walks and send DDR request
            ptw_walks_mtx.lock();
            ptw_active_walks[task->task_id] = task;
            ptw_walks_mtx.unlock();

            ddr_req_entry_t req;
            req.task_id = task->task_id;
            req.addr = task->walk_ctx.read_addr;
            req.size = task->walk_ctx.read_size;
            req.is_write = false;
            req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;  // [STAT]
            task->walk_ctx.ddr_read_count = 1;
            printf("[PTW_REQ] task_id=%u, walk_phase=%d, addr=0x%lx, size=%d, read_count=1 -> ddr_req\n",
                   task->task_id, task->walk_ctx.walk_phase, req.addr, req.size);
            fflush(stdout);
            ptw_req_ddr_fifo.write(req);
        }
    }
}

// ============================================================
// 12.14 ptw_rsp_thread - PTW Response Dispatch (PEQ-based pipeline)
// Reads DDR responses, schedules via PEQ with pipeline delay
// ============================================================
void iommu_top::ptw_rsp_thread() {
    while (true) {
        ddr_rsp_entry_t rsp = ptw_rsp_ddr_fifo.read();
        // Heap-allocate to ensure lifetime across PEQ delay
        ddr_rsp_entry_t* rsp_ptr = new ddr_rsp_entry_t(rsp);
        ptw_rsp_peq.notify(*rsp_ptr, sc_time(PTW_RSP_PIPELINE_DELAY_NS, SC_NS));
    }
}

// ============================================================
// 12.14b ptw_rsp_process_thread - PTW Response Processing
// Fires after PEQ delay, processes DDR responses for VS/GS walk
// ============================================================
void iommu_top::ptw_rsp_process_thread() {
    while (true) {
        ddr_rsp_entry_t* rsp_ptr = ptw_rsp_peq.get_next_transaction();
        if (!rsp_ptr) { wait(ptw_rsp_peq.get_event()); continue; }
        ddr_rsp_entry_t rsp = *rsp_ptr;
        delete rsp_ptr;

        printf("[PTW_RSP] task_id=%u, data_len=%d, error=%d -> processing\n",
               rsp.task_id, rsp.data_length, rsp.error);
        fflush(stdout);

        // Find corresponding task
        ptw_walks_mtx.lock();
        auto it = ptw_active_walks.find(rsp.task_id);
        if (it == ptw_active_walks.end()) {
            ptw_walks_mtx.unlock();
            printf("[PTW] ERROR: task_id %u not found in active_walks\n", rsp.task_id);
            continue;
        }
        iommu_task_t* task = it->second;
        ptw_walks_mtx.unlock();

        memcpy(task->walk_ctx.read_buf, rsp.data, rsp.data_length);

        // [STAT] 计算单次DDR访问延时
        double ddr_latency_ns = sc_time_stamp().to_seconds() * 1e9 - rsp.submit_time_ns;
        if (ddr_latency_ns > ptw_max_ddr_latency_ns)
            ptw_max_ddr_latency_ns = ddr_latency_ns;
        if (ddr_latency_ns < ptw_min_ddr_latency_ns)
            ptw_min_ddr_latency_ns = ddr_latency_ns;
        ptw_total_ddr_latency_ns += ddr_latency_ns;
        ptw_ddr_latency_count++;

        bool walk_complete = false;
        bool walk_fault = false;
        bool need_next_ddr = false;

        printf("[PTW_RSP] task_id=%u, walk_phase=%d (VS_WALK=0, GS_IMPLICIT=1, GS_EXPLICIT=2, AD_UPDATE=3), read_count=%u\n",
               task->task_id, task->walk_ctx.walk_phase, task->walk_ctx.ddr_read_count);
        fflush(stdout);

        switch (task->walk_ctx.walk_phase) {

        case PTW_PREFETCH_WAIT: {
            // =====================================================================
            // Burst预取DDR响应处理 (v2.0)
            // 接收Burst DDR响应,解析D个PTE,构造pt_updates
            // =====================================================================
            printf("[PTW_PREFETCH] task_id=%u -> Burst DDR response received\n",
                   task->task_id);
            fflush(stdout);
            
            // 从Burst响应数据中解析D个PTE
            uint32_t prefetch_depth = task->walk_ctx.prefetch_depth;
            uint32_t ptesize = task->walk_ctx.leaf_ptesize;
            uint64_t base_iova = task->iova & ~0xFFFULL;  // 4KB页对齐
            
            // 解析Burst数据 (每个PTE=8字节)
            for (uint32_t d = 0; d < prefetch_depth; d++) {
                // 从read_buf中提取第d个PTE
                uint64_t pte_raw = 0;
                memcpy(&pte_raw, &task->walk_ctx.read_buf[d * ptesize], ptesize);
                
                spte_t vs_pte;
                vs_pte.raw = pte_raw;
                
                gpte_t g_pte;
                g_pte.raw = pte_raw;  // G-stage PTE与VS-stage相同
                
                // 计算预取IOVA对应的PA
                uint64_t prefetch_ppn = vs_pte.PPN;
                uint64_t prefetch_iova = base_iova + (d + 1) * 0x1000;  // 4KB步长
                uint64_t prefetch_pa = (prefetch_ppn << 12) | (prefetch_iova & 0xFFF);
                
                // 保存到pt_updates[d+1]
                task->walk_ctx.pt_updates[d + 1].vs_pte = vs_pte;
                task->walk_ctx.pt_updates[d + 1].g_pte = g_pte;
                task->walk_ctx.pt_updates[d + 1].pa = prefetch_pa;
                task->walk_ctx.pt_updates[d + 1].iova = prefetch_iova;
                task->walk_ctx.pt_updates[d + 1].page_sz = 0x1000;  // 4KB
                
                printf("[PTW_PREFETCH] PTE[%u]: iova=0x%lx, PPN=0x%lx, pa=0x%lx\n",
                       d+1, prefetch_iova, prefetch_ppn, prefetch_pa);
            }
            
            fflush(stdout);
            
            // 标记Burst预取完成
            task->walk_ctx.prefetch_burst_pending = false;
            
            // [FIX] 减少pending_tasks并通知monitor
            prefetch_group_mtx.lock();
            auto& group = prefetch_groups[task->task_id];
            group.pending_tasks--;
            
            printf("[PTW_PREFETCH] task_id=%u -> Burst complete, pending_tasks=%u\n",
                   task->task_id, group.pending_tasks);
            fflush(stdout);
            
            if (group.pending_tasks == 0 && group.completed) {
                printf("[PTW_PREFETCH] task_id=%u -> Notifying monitor (all pending done)\n",
                       task->task_id);
                fflush(stdout);
                prefetch_group_completed_event.notify(SC_ZERO_TIME);
            }
            prefetch_group_mtx.unlock();
            
            break;
        }

        case PTW_VS_WALK: {
            // ========== VS-stage PTE parsing ==========
            // Corresponds to iommu_two_stage_trans.cc L163-471
            spte_t pte;
            pte.raw = 0;
            memcpy(&pte.raw, task->walk_ctx.read_buf, task->walk_ctx.ptesize);

            // Step 3: PTE validity check
            if ((pte.V == 0) || (pte.R == 0 && pte.W == 1) ||
                (pte.PBMT == 3) || (pte.reserved != 0)) {
                task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                walk_fault = true;
                break;
            }

            // NAPOT check: non-leaf N bit must be 0
            if (task->walk_ctx.level != 0 && pte.N) {
                task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                walk_fault = true;
                break;
            }

            // Step 4: Leaf/non-leaf determination
            if (pte.R == 1 || pte.X == 1) {
                // ===== Leaf node (steps 5-7) =====
                // Permission checks
                if (task->check_access_perms) {
                    if ((task->is_exec && pte.X == 0) ||
                        (task->is_read && pte.R == 0) ||
                        (task->is_write && pte.W == 0)) {
                        task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                        walk_fault = true;
                        break;
                    }
                }
                if (task->priv == U_MODE && pte.U == 0) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }
                if (task->is_exec && task->priv == S_MODE && pte.U == 1) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }
                if (task->priv == S_MODE && !task->is_exec &&
                    task->SUM == 0 && pte.U == 1) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }

                // Superpage size calculation
                task->page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.level; j++) {
                    task->page_sz *= (task->walk_ctx.ptesize == 8) ? 512 : 1024;
                }

                // Step 6: Misaligned superpage check
                // If i > 0 and pte.ppn[i-1:0] != 0, this is a misaligned superpage
                if (task->walk_ctx.level > 0) {
                    uint8_t misaligned = 0;
                    if (task->iosatp.MODE == IOSATP_Sv32 && task->SXL == 1) {
                        // Sv32: PPN[0] is 10 bits
                        if ((pte.PPN & 0x3FF) != 0) misaligned = 1;
                    } else {
                        // Sv39/Sv48/Sv57: PPN subfields are 9 bits each
                        if ((pte.PPN & 0x1FF) != 0) misaligned = 1;
                        if (task->walk_ctx.level >= 2 && ((pte.PPN >> 9) & 0x1FF) != 0) misaligned = 1;
                        if (task->walk_ctx.level >= 3 && ((pte.PPN >> 18) & 0x1FF) != 0) misaligned = 1;
                        if (task->walk_ctx.level >= 4 && ((pte.PPN >> 27) & 0x1FF) != 0) misaligned = 1;
                    }
                    if (misaligned) {
                        task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                        walk_fault = true;
                        printf("[PTW_RSP] task_id=%u -> FAULT: misaligned superpage at level=%d, PPN=0x%lx\n",
                               task->task_id, task->walk_ctx.level, (uint64_t)pte.PPN);
                        fflush(stdout);
                        break;
                    }
                }

                printf("[PTW_RSP] task_id=%u, VS_WALK leaf: level=%d, page_sz=0x%lx, PPN=0x%lx, total_reads=%u\n",
                       task->task_id, task->walk_ctx.level, task->page_sz, pte.PPN, task->walk_ctx.ddr_read_count);
                fflush(stdout);

                // A/D bit processing
                if (pte.A == 0 || (task->is_write && pte.D == 0)) {
                    printf("[PTW_RSP] task_id=%u, A/D update needed: A=%d, D=%d, is_write=%d, SADE=%d, read_count=%u\n",
                           task->task_id, pte.A, pte.D, task->is_write, task->SADE, task->walk_ctx.ddr_read_count);
                    fflush(stdout);
                    if (task->SADE == 1) {
                        // Hardware A/D update: need AMO DDR write
                        pte.A = 1;
                        if (task->is_write) pte.D = 1;
                        task->walk_ctx.ad_pte = pte;
                        task->walk_ctx.ad_pte_addr = task->walk_ctx.base_addr +
                            task->walk_ctx.vpn[task->walk_ctx.level] * task->walk_ctx.ptesize;
                        task->vs_pte = pte;
                        task->gpa = ((pte.PPN * PAGESIZE) & ~(task->page_sz - 1)) |
                                    (task->iova & (task->page_sz - 1));
                        task->walk_ctx.walk_phase = PTW_AD_UPDATE;
                        task->walk_ctx.read_addr = task->walk_ctx.ad_pte_addr;
                        task->walk_ctx.read_size = task->walk_ctx.ptesize;
                        task->walk_ctx.ddr_read_count++;
                        need_next_ddr = true;
                        printf("[PTW_RSP] task_id=%u, -> AD_UPDATE (read_count=%u), addr=0x%lx\n",
                               task->task_id, task->walk_ctx.ddr_read_count, task->walk_ctx.read_addr);
                        fflush(stdout);
                        break;
                    } else {
                        task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                        walk_fault = true;
                        break;
                    }
                }

                // NAPOT PPN adjustment
                if (pte.N) {
                    pte.PPN = (pte.PPN & ~0xFULL) |
                              ((task->iova / PAGESIZE) & 0xF);
                }

                // GPA calculation
                task->gpa = ((pte.PPN * PAGESIZE) & ~(task->page_sz - 1)) |
                            (task->iova & (task->page_sz - 1));
                task->vs_pte = pte;

                // Step 19: G-stage explicit translation
                if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                    init_gstage_walk(task, task->gpa);
                    task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                    task->walk_ctx.ddr_read_count++;
                    need_next_ddr = true;
                    printf("[PTW_RSP] task_id=%u, -> GS_EXPLICIT (read_count=%u), gpa=0x%lx\n",
                           task->task_id, task->walk_ctx.ddr_read_count, task->gpa);
                    fflush(stdout);
                } else {
                    // G-stage Bare: complete
                    task->pa = task->gpa;
                    task->gst_page_sz = get_gstage_bare_page_size(&iommu_inst);
                    task->g_pte.raw = 0;
                    task->g_pte.PPN = task->gpa / PAGESIZE;
                    task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                    task->g_pte.X = task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                    task->g_pte.PBMT = PMA;
                    task->state = TASK_PTW_DONE;
                    walk_complete = true;
                    printf("[PTW_RSP] task_id=%u, VS_WALK leaf -> DONE, total_reads=%u, pa=0x%lx\n",
                           task->task_id, task->walk_ctx.ddr_read_count, task->pa);
                    fflush(stdout);
                }
            } else {
                // ===== Non-leaf node =====
                printf("[PTW_RSP] task_id=%u, VS_WALK non-leaf: level=%d -> next level %d, read_count=%u\n",
                       task->task_id, task->walk_ctx.level, task->walk_ctx.level - 1, task->walk_ctx.ddr_read_count);
                fflush(stdout);
                if (pte.PBMT != 0 || pte.D != 0 || pte.A != 0 || pte.U != 0) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }

                // =====================================================================
                // 保存中间结果到 Walker Cache entries（按 PTWc_X tag 含义对齐）
                // =====================================================================
                // PTWc_X 的 tag = 前 X 段 VPN 合并，其 data 是"读完前 X 段后的 PTE.PPN"：
                //   Sv39: walk level=2 非叶子 -> PTWc_2（存 valid_level1）
                //   Sv39: walk level=1 非叶子 -> PTWc_3（存 valid_level2）
                //   Sv48: walk level=3 非叶子 -> PTWc_1（存 valid_level0）
                //   Sv48: walk level=2 非叶子 -> PTWc_2（存 valid_level1）
                //   Sv48: walk level=1 非叶子 -> PTWc_3（存 valid_level2）
                //   level=0 为叶子节点 -> 不缓存
                //
                // 注意：task_to_walker_update 映射为
                //   valid_level0 -> PTWc_1, valid_level1 -> PTWc_2, valid_level2 -> PTWc_3
                if (task->iosatp.MODE == IOSATP_Sv48) {
                    if (task->walk_ctx.level == 3) {
                        task->walk_ctx.walker_cache_entries.ppn_level0 = pte.PPN;
                        task->walk_ctx.walker_cache_entries.valid_level0 = true;
                    } else if (task->walk_ctx.level == 2) {
                        task->walk_ctx.walker_cache_entries.ppn_level1 = pte.PPN;
                        task->walk_ctx.walker_cache_entries.valid_level1 = true;
                    } else if (task->walk_ctx.level == 1) {
                        task->walk_ctx.walker_cache_entries.ppn_level2 = pte.PPN;
                        task->walk_ctx.walker_cache_entries.valid_level2 = true;
                    }
                } else {  // Sv39
                    if (task->walk_ctx.level == 2) {
                        task->walk_ctx.walker_cache_entries.ppn_level1 = pte.PPN;
                        task->walk_ctx.walker_cache_entries.valid_level1 = true;
                    } else if (task->walk_ctx.level == 1) {
                        task->walk_ctx.walker_cache_entries.ppn_level2 = pte.PPN;
                        task->walk_ctx.walker_cache_entries.valid_level2 = true;
                    }
                }
                // level==0 为叶子节点，Walker Cache 不缓存叶子
                
                printf("[PTW_RSP] task_id=%u, saved walker cache: level=%d, PPN=0x%lx, valid=[L2=%d,L1=%d,L0=%d]\n",
                       task->task_id, task->walk_ctx.level, pte.PPN,
                       task->walk_ctx.walker_cache_entries.valid_level2,
                       task->walk_ctx.walker_cache_entries.valid_level1,
                       task->walk_ctx.walker_cache_entries.valid_level0);
                fflush(stdout);

                task->walk_ctx.level--;
                if (task->walk_ctx.level < 0) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }

                task->walk_ctx.base_addr = pte.PPN * PAGESIZE;
                uint64_t next_pte_addr = task->walk_ctx.base_addr +
                    task->walk_ctx.vpn[task->walk_ctx.level] * task->walk_ctx.ptesize;

                if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                    task->walk_ctx.pending_vs_pte_addr = next_pte_addr;
                    init_gstage_walk(task, next_pte_addr);
                    task->walk_ctx.walk_phase = PTW_GS_IMPLICIT;
                } else {
                    task->walk_ctx.read_addr = next_pte_addr;
                    task->walk_ctx.read_size = task->walk_ctx.ptesize;
                    // walk_phase stays PTW_VS_WALK
                }
                task->walk_ctx.ddr_read_count++;
                need_next_ddr = true;
                printf("[PTW_RSP] task_id=%u, VS_WALK non-leaf -> next addr=0x%lx, read_count=%u\n",
                       task->task_id, task->walk_ctx.read_addr, task->walk_ctx.ddr_read_count);
                fflush(stdout);
            }
            break;
        }

        case PTW_GS_IMPLICIT: {
            // ========== G-stage implicit translation ==========
            // Translates VS PTE address (GPA) to SPA
            // Corresponds to iommu_second_stage_trans.cc L139-255
            gpte_t gs_pte;
            gs_pte.raw = 0;
            memcpy(&gs_pte.raw, task->walk_ctx.read_buf, 8);

            if (gs_pte.V == 0 || (gs_pte.R == 0 && gs_pte.W == 1)) {
                task->cause = 21;  // Guest load page fault (implicit)
                task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                walk_fault = true;
                break;
            }

            if (gs_pte.R == 1 || gs_pte.X == 1) {
                // G-stage leaf: calculate translated SPA
                if (gs_pte.R == 0) {
                    task->cause = 21;
                    task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                    walk_fault = true;
                    break;
                }

                uint64_t gs_page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.gs_level; j++)
                    gs_page_sz *= 512;

                uint64_t spa = ((gs_pte.PPN * PAGESIZE) & ~(gs_page_sz - 1)) |
                               (task->walk_ctx.pending_vs_pte_addr & (gs_page_sz - 1));

                // G-stage A/D check (implicit: only need A=1)
                if (gs_pte.A == 0) {
                    if (task->GADE == 1) {
                        gs_pte.A = 1;
                    } else {
                        task->cause = 21;
                        task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                        walk_fault = true;
                        break;
                    }
                }

                // Return to VS-stage walk with translated address
                task->walk_ctx.read_addr = spa;
                task->walk_ctx.read_size = task->walk_ctx.ptesize;
                task->walk_ctx.walk_phase = PTW_VS_WALK;
                task->walk_ctx.ddr_read_count++;
                printf("[PTW_RSP] task_id=%u, GS_IMPLICIT leaf -> VS_WALK (read_count=%u), spa=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, spa);
                fflush(stdout);
            } else {
                // G-stage non-leaf: continue walk
                if (gs_pte.PBMT != 0 || gs_pte.D != 0 || gs_pte.A != 0 ||
                    gs_pte.U != 0) {
                    task->cause = 21;
                    task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                    walk_fault = true;
                    break;
                }
                task->walk_ctx.gs_level--;
                if (task->walk_ctx.gs_level < 0) {
                    task->cause = 21;
                    task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                    walk_fault = true;
                    break;
                }
                task->walk_ctx.gs_base_addr = gs_pte.PPN * PAGESIZE;
                uint64_t gs_next = task->walk_ctx.gs_base_addr +
                    task->walk_ctx.gs_vpn[task->walk_ctx.gs_level] * 8;
                task->walk_ctx.read_addr = gs_next;
                task->walk_ctx.read_size = 8;
                task->walk_ctx.ddr_read_count++;
                printf("[PTW_RSP] task_id=%u, GS_IMPLICIT non-leaf -> next (read_count=%u), addr=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, gs_next);
                fflush(stdout);
                // walk_phase stays PTW_GS_IMPLICIT
            }
            need_next_ddr = true;
            break;
        }

        case PTW_GS_EXPLICIT: {
            // ========== G-stage explicit translation: GPA → final PA ==========
            // Corresponds to iommu_second_stage_trans.cc L139-423
            gpte_t gs_pte;
            gs_pte.raw = 0;
            memcpy(&gs_pte.raw, task->walk_ctx.read_buf, 8);

            if (gs_pte.V == 0 || (gs_pte.R == 0 && gs_pte.W == 1)) {
                set_guest_fault_cause(task, 0);
                task->iotval2 = (task->gpa & ~0x3ULL);
                walk_fault = true;
                break;
            }

            if (gs_pte.R == 1 || gs_pte.X == 1) {
                // G-stage leaf: permission check + PA calculation
                if (task->check_access_perms) {
                    if (task->is_read && gs_pte.R == 0) {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        break;
                    }
                    if (task->is_write && gs_pte.W == 0) {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        break;
                    }
                    if (task->is_exec && gs_pte.X == 0) {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        break;
                    }
                }

                // A/D bit check
                if (gs_pte.A == 0 || (task->is_write && gs_pte.D == 0)) {
                    if (task->GADE == 1) {
                        gs_pte.A = 1;
                        if (task->is_write) gs_pte.D = 1;
                    } else {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        break;
                    }
                }

                // G-stage superpage size calculation
                task->gst_page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.gs_level; j++)
                    task->gst_page_sz *= 512;

                // Misaligned G-stage superpage check
                if (task->walk_ctx.gs_level > 0) {
                    uint8_t gs_misaligned = 0;
                    if ((gs_pte.PPN & 0x1FF) != 0) gs_misaligned = 1;
                    if (task->walk_ctx.gs_level >= 2 && ((gs_pte.PPN >> 9) & 0x1FF) != 0) gs_misaligned = 1;
                    if (task->walk_ctx.gs_level >= 3 && ((gs_pte.PPN >> 18) & 0x1FF) != 0) gs_misaligned = 1;
                    if (task->walk_ctx.gs_level >= 4 && ((gs_pte.PPN >> 27) & 0x1FF) != 0) gs_misaligned = 1;
                    if (gs_misaligned) {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        printf("[PTW_RSP] task_id=%u -> FAULT: misaligned G-stage superpage at level=%d, PPN=0x%lx\n",
                               task->task_id, task->walk_ctx.gs_level, (uint64_t)gs_pte.PPN);
                        fflush(stdout);
                        break;
                    }
                }

                task->pa = ((gs_pte.PPN * PAGESIZE) & ~(task->gst_page_sz - 1)) |
                           (task->gpa & (task->gst_page_sz - 1));
                task->g_pte = gs_pte;

                // Handle virtual interrupt file overlap
                handle_virtual_interrupt_file_overlap(&task->DC, task->gpa,
                                                     &task->gst_page_sz);

                task->state = TASK_PTW_DONE;
                walk_complete = true;
                printf("[PTW_RSP] task_id=%u, GS_EXPLICIT leaf -> DONE, total_reads=%u, pa=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, task->pa);
                fflush(stdout);
            } else {
                // G-stage non-leaf: continue walk
                if (gs_pte.PBMT != 0 || gs_pte.D != 0 || gs_pte.A != 0 ||
                    gs_pte.U != 0) {
                    set_guest_fault_cause(task, 0);
                    task->iotval2 = (task->gpa & ~0x3ULL);
                    walk_fault = true;
                    break;
                }
                task->walk_ctx.gs_level--;
                if (task->walk_ctx.gs_level < 0) {
                    set_guest_fault_cause(task, 0);
                    task->iotval2 = (task->gpa & ~0x3ULL);
                    walk_fault = true;
                    break;
                }
                task->walk_ctx.gs_base_addr = gs_pte.PPN * PAGESIZE;
                uint64_t gs_next = task->walk_ctx.gs_base_addr +
                    task->walk_ctx.gs_vpn[task->walk_ctx.gs_level] * 8;
                task->walk_ctx.read_addr = gs_next;
                task->walk_ctx.read_size = 8;
                task->walk_ctx.ddr_read_count++;
                printf("[PTW_RSP] task_id=%u, GS_EXPLICIT non-leaf -> next (read_count=%u), addr=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, gs_next);
                fflush(stdout);
                // walk_phase stays PTW_GS_EXPLICIT
                need_next_ddr = true;
            }
            break;
        }

        case PTW_AD_UPDATE: {
            // A/D bit AMO update completed
            // Continue to G-stage translation or complete
            printf("[PTW_RSP] task_id=%u, AD_UPDATE completed, read_count=%u\n",
                   task->task_id, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                init_gstage_walk(task, task->gpa);
                task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                task->walk_ctx.ddr_read_count++;
                need_next_ddr = true;
                printf("[PTW_RSP] task_id=%u, AD_UPDATE -> GS_EXPLICIT (read_count=%u)\n",
                       task->task_id, task->walk_ctx.ddr_read_count);
                fflush(stdout);
            } else {
                task->pa = task->gpa;
                task->gst_page_sz = get_gstage_bare_page_size(&iommu_inst);
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.X = task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.PBMT = PMA;
                task->state = TASK_PTW_DONE;
                walk_complete = true;
                printf("[PTW_RSP] task_id=%u, AD_UPDATE -> DONE, total_reads=%u, pa=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, task->pa);
                fflush(stdout);
            }
            break;
        }

        } // end switch

        // ========== Unified walk result handling ==========
        if (walk_fault) {
            printf("[PTW_RSP] task_id=%u -> WALK FAULT, cause=%d, total_reads=%u\n",
                   task->task_id, task->cause, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            task->state = TASK_FAULT;
            ptw_walks_mtx.lock();
            ptw_active_walks.erase(rsp.task_id);
            ptw_walks_mtx.unlock();
            ptw_outstanding_task_count--;
            ptw_total_completed++;  // [STAT]
            // [STAT] 累加PTW完成统计（walk fault）
            ptw_total_ddr_reads += task->walk_ctx.ddr_read_count;
            { auto _s = ptw_task_start_ns.find(task->task_id);
              if (_s != ptw_task_start_ns.end()) {
                  ptw_total_exec_ns += sc_time_stamp().to_seconds()*1e9 - _s->second;
                  ptw_task_start_ns.erase(_s); } }
            ptw_task_completed_event.notify(SC_ZERO_TIME);
            
            // Convert task to CacheMessage and write to cache_sub.pt_update_fifo
            iommu::CacheMessage pt_update_req = task_to_pt_update(task);
            cache_sub.pt_update_fifo.write(pt_update_req);
            
            // Also route to forwarder after PT update
            pt_cache_to_fwd_fifo.write(task);
            // VA Dedup: 恢复挂起的同页任务(fault)
            va_dedup_recover(task, true);
        }
        else if (walk_complete) {
            walk_complete_handler:  // 用于goto跳转
            printf("[PTW_RSP] task_id=%u -> WALK COMPLETE, pa=0x%lx, total_reads=%u\n",
                   task->task_id, task->pa, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            
            // =====================================================================
            // 预取组结果收集(完整9笔独立walk方案)
            // =====================================================================
            if (task->walk_ctx.prefetch_group_id != 0) {
                // 这是预取组的一员
                uint32_t group_id = task->walk_ctx.prefetch_group_id;
                uint32_t task_idx = task->walk_ctx.prefetch_idx;
                
                prefetch_group_mtx.lock();
                auto& group = prefetch_groups[group_id];
                
                // 保存当前walk结果
                group.vs_ptes[task_idx] = task->vs_pte;
                group.g_ptes[task_idx] = task->g_pte;
                group.pas[task_idx] = task->pa;
                group.page_szs[task_idx] = task->page_sz;
                
                // 减少pending计数
                group.pending_tasks--;
                
                printf("[PTW_PREFETCH] Walk completed: group=%u, idx=%u, pending=%u\n",
                       group_id, task_idx, group.pending_tasks);
                fflush(stdout);
                
                // 检查是否全部完成
                if (group.pending_tasks == 0 && !group.completed) {
                    group.completed = true;
                    printf("[PTW_PREFETCH] Group %u ALL COMPLETED! Trigger batch update.\n", group_id);
                    fflush(stdout);
                    
                    // 触发批量返回
                    prefetch_group_completed_event.notify(SC_ZERO_TIME);
                }
                
                prefetch_group_mtx.unlock();
                
                // 预取任务直接释放(不经过后续流程)
                if (task->walk_ctx.is_prefetch_task) {
                    printf("[PTW_PREFETCH] Prefetch task %u completed, freeing.\n", task->task_id);
                    fflush(stdout);
                    delete task;
                    goto skip_walk_cleanup;
                }
                // 主任务继续后续流程(由monitor thread释放)
            }
            
            // =====================================================================
            // NEW: Burst预取方案 (v2.0)
            // 利用L0页表连续性,1次Burst读代替D次独立walk
            // =====================================================================
            if (task->walk_ctx.prefetch_enabled && task->walk_ctx.prefetch_depth > 0) {
                printf("[PTW_PREFETCH] task_id=%u -> Burst prefetch mode, depth=%u\n",
                       task->task_id, task->walk_ctx.prefetch_depth);
                fflush(stdout);
                
                // 保存主任务信息到pt_updates[0]
                task->walk_ctx.pt_updates[0].vs_pte = task->vs_pte;
                task->walk_ctx.pt_updates[0].g_pte = task->g_pte;
                task->walk_ctx.pt_updates[0].pa = task->pa;
                task->walk_ctx.pt_updates[0].iova = task->iova;
                task->walk_ctx.pt_updates[0].page_sz = task->page_sz;
                
                // 计算Burst参数
                uint64_t leaf_pt_base = task->walk_ctx.base_addr;  // L0页表基址
                uint16_t current_vpn0 = task->walk_ctx.vpn[0];     // 当前VPN[0]
                uint32_t ptesize = task->walk_ctx.ptesize;         // PTE大小 (Sv39=8字节)
                uint32_t prefetch_depth = task->walk_ctx.prefetch_depth;
                
                // [FIX] 初始化prefetch_iovas数组
                uint64_t base_iova = task->iova & ~0xFFFULL;  // 4KB页对齐
                for (uint32_t d = 0; d < prefetch_depth; d++) {
                    task->walk_ctx.prefetch_iovas[d] = base_iova + (d + 1) * 0x1000;
                }
                
                // Burst起始地址: L0_base + (VPN[0]+1) * PTE_size
                uint64_t burst_start_addr = leaf_pt_base + (current_vpn0 + 1) * ptesize;
                uint32_t burst_size = prefetch_depth * ptesize;  // D * 8字节
                
                // [边界检查] 确保Burst不跨越4KB页表页
                uint64_t pt_page_mask = 0xFFF;  // 4KB页表页
                uint64_t burst_end_addr = burst_start_addr + burst_size - 1;
                
                if ((burst_start_addr & ~pt_page_mask) != (burst_end_addr & ~pt_page_mask)) {
                    // Burst跨越页表页边界，截断
                    uint64_t page_boundary = (burst_start_addr | pt_page_mask) + 1;
                    burst_size = page_boundary - burst_start_addr;
                    prefetch_depth = burst_size / ptesize;
                    
                    printf("[PTW_PREFETCH] Burst truncated at page boundary, depth=%u, size=%u bytes\n",
                           prefetch_depth, burst_size);
                    fflush(stdout);
                    
                    task->walk_ctx.prefetch_depth = prefetch_depth;  // 更新实际预取深度
                }
                
                task->walk_ctx.pt_update_count = 1 + prefetch_depth;  // 1主 + D预取
                
                // 保存leaf页表信息
                task->walk_ctx.leaf_pt_base_addr = leaf_pt_base;
                task->walk_ctx.leaf_ptesize = ptesize;
                task->walk_ctx.burst_start_addr = burst_start_addr;
                task->walk_ctx.burst_size = burst_size;
                task->walk_ctx.prefetch_burst_pending = true;
                
                // 初始化预取组
                uint32_t group_id = task->task_id;
                task->walk_ctx.prefetch_group_id = group_id;
                task->walk_ctx.prefetch_total = 1 + prefetch_depth;
                task->walk_ctx.prefetch_idx = 0;  // 0=主任务
                
                // 保存所有IOVA到组上下文
                task->walk_ctx.prefetch_group.group_iovas[0] = task->iova;
                for (uint32_t d = 0; d < prefetch_depth; d++) {
                    task->walk_ctx.prefetch_group.group_iovas[1+d] = task->walk_ctx.prefetch_iovas[d];
                }
                
                // 注册到预取组追踪表
                prefetch_group_mtx.lock();
                auto& group = prefetch_groups[group_id];
                group.main_task = task;
                group.pending_tasks = 1;  // [FIX] 仅等待1次Burst DDR响应 (主任务已完成)
                group.total_tasks = task->walk_ctx.prefetch_total;
                group.completed = true;  // [FIX] Burst方案: 主任务已完成,设置completed=true
                
                // 复制IOVA列表
                for (uint32_t i = 0; i < group.total_tasks; i++) {
                    group.group_iovas[i] = task->walk_ctx.prefetch_group.group_iovas[i];
                }
                
                prefetch_group_mtx.unlock();
                
                printf("[PTW_PREFETCH] task_id=%u -> Burst DDR: addr=0x%lx, size=%u bytes, depth=%u\n",
                       task->task_id, burst_start_addr, burst_size, prefetch_depth);
                fflush(stdout);
                
                // 发送Burst DDR读请求
                ddr_req_entry_t burst_req;
                burst_req.task_id = task->task_id;
                burst_req.addr = burst_start_addr;
                burst_req.size = burst_size;
                burst_req.is_write = false;
                burst_req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;
                
                task->walk_ctx.ddr_read_count++;
                task->walk_ctx.walk_phase = PTW_PREFETCH_WAIT;
                
                // 注册到active_walks
                ptw_walks_mtx.lock();
                ptw_active_walks[task->task_id] = task;
                ptw_walks_mtx.unlock();
                
                ptw_req_ddr_fifo.write(burst_req);
                
                // 跳过后续处理，等待Burst响应
                goto skip_walk_cleanup;
            }
            
            ptw_walks_mtx.lock();
            ptw_active_walks.erase(rsp.task_id);
            ptw_walks_mtx.unlock();
            ptw_outstanding_task_count--;
            printf("[PTW_STAT] task_id=%u completed, outstanding now=%d\n",
                   task->task_id, ptw_outstanding_task_count);
            fflush(stdout);
            ptw_total_completed++;  // [STAT]
            // [STAT] 累加PTW完成统计（walk complete）
            ptw_total_ddr_reads += task->walk_ctx.ddr_read_count;
            
            // [STAT] 更新DDR访问次数最大/最小值
            if (task->walk_ctx.ddr_read_count > ptw_max_ddr_reads)
                ptw_max_ddr_reads = task->walk_ctx.ddr_read_count;
            if (task->walk_ctx.ddr_read_count < ptw_min_ddr_reads)
                ptw_min_ddr_reads = task->walk_ctx.ddr_read_count;
            
            // [STAT] 计算任务总延时
            { auto _s = ptw_task_start_ns.find(task->task_id);
              if (_s != ptw_task_start_ns.end()) {
                  double task_latency = sc_time_stamp().to_seconds()*1e9 - _s->second;
                  ptw_total_exec_ns += task_latency;
                  if (task_latency > ptw_max_task_latency_ns)
                      ptw_max_task_latency_ns = task_latency;
                  if (task_latency < ptw_min_task_latency_ns)
                      ptw_min_task_latency_ns = task_latency;
                  ptw_task_start_ns.erase(_s);
              } }
            ptw_task_completed_event.notify(SC_ZERO_TIME);
            
            // Convert task to CacheMessage and write to cache_sub.pt_update_fifo
            iommu::CacheMessage pt_update_req = task_to_pt_update(task);
            cache_sub.pt_update_fifo.write(pt_update_req);
            
            // =====================================================================
            // Update Walker Cache (如果启用)
            // =====================================================================
            bool walker_cache_enabled = PTW_WALKER_CACHE_ENABLED;  // 从PTW模块参数读取
            if (walker_cache_enabled) {
                // 使用转换函数构造Walker Cache Update请求
                iommu::CacheMessage walker_req = task_to_walker_update(task);
                
                // PTWC_1_2_3 / PTWC_2_3 / PTWC_3 三种更新类型都需要写入 walker_update_fifo
                cache_sub.walker_update_fifo.write(walker_req);
                
                printf("[t=%llu ns][PTW_RSP] task_id=%u -> Walker Cache UPDATE enqueue (iova=0x%lx, kind=%d, L2=%d, L1=%d, L0=%d)\n",
                       (unsigned long long)sc_core::sc_time_stamp().value()/1000,
                       task->task_id, task->iova,
                       static_cast<int>(walker_req.walker_update_kind),
                       task->walk_ctx.walker_cache_entries.valid_level2,
                       task->walk_ctx.walker_cache_entries.valid_level1,
                       task->walk_ctx.walker_cache_entries.valid_level0);
                fflush(stdout);
            }
            
            // Also route to forwarder after PT update
            pt_cache_to_fwd_fifo.write(task);
            // VA Dedup: 恢复挂起的同页任务
            va_dedup_recover(task, false);
        }
        else if (need_next_ddr) {
            printf("[PTW_RSP] task_id=%u -> need_next_ddr, addr=0x%lx, size=%d, phase=%d, read_count=%u\n",
                   task->task_id, task->walk_ctx.read_addr, task->walk_ctx.read_size,
                   task->walk_ctx.walk_phase, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            ddr_req_entry_t req;
            req.task_id = task->task_id;
            req.addr = task->walk_ctx.read_addr;
            req.size = task->walk_ctx.read_size;
            req.is_write = (task->walk_ctx.walk_phase == PTW_AD_UPDATE);
            req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;  // [STAT]
            if (req.is_write) {
                memcpy(req.write_data, &task->walk_ctx.ad_pte,
                       task->walk_ctx.ptesize);
            }
            ptw_req_ddr_fifo.write(req);
        }
        
        skip_walk_cleanup:
        ;  // 空语句，用于goto跳转
    }
}

// ============================================================
// NEW: prefetch_group_monitor_thread - 预取组监控线程
// 监控预取组完成状态,执行批量PT Cache更新和Dedup Buffer刷新
// ============================================================
void iommu_top::prefetch_group_monitor_thread() {
    while (true) {
        // 等待预取组完成事件
        wait(prefetch_group_completed_event);
        
        prefetch_group_mtx.lock();
        
        // 查找已完成的组 (Burst预取方案)
        for (auto it = prefetch_groups.begin(); it != prefetch_groups.end(); ) {
            uint32_t group_id = it->first;
            auto& group = it->second;
            
            // Burst方案: pending_tasks=0 表示Burst DDR响应已处理
            if (group.completed && group.pending_tasks == 0) {
                printf("[PTW_PREFETCH_MONITOR] Processing completed group %u (total=%u, Burst mode)\n",
                       group_id, group.total_tasks);
                fflush(stdout);
                
                // ========== 批量更新PT Cache ==========
                // 注意: pt_updates已在PTW_PREFETCH_WAIT状态中填充
                auto* main_task = group.main_task;
                
                // [FIX] 安全检查
                if (main_task == nullptr) {
                    printf("[PTW_PREFETCH_MONITOR] ERROR: main_task is nullptr for group %u!\n", group_id);
                    fflush(stdout);
                    it = prefetch_groups.erase(it);
                    continue;
                }
                
                // [FIX] 动态计算stage（与task_to_pt_request保持一致）
                bool sv48 = (main_task->iosatp.MODE == IOSATP_Sv48);
                bool gstage_x4 = (main_task->iohgatp.MODE == IOHGATP_Sv48x4);
                bool stage1_bare = (main_task->iosatp.MODE == RVI_IOMMU_IOSATP_Bare);
                bool stage2_bare = (main_task->iohgatp.MODE == RVI_IOMMU_IOHGATP_Bare);
                iommu::TransStage stage;
                if (stage1_bare && !stage2_bare) {
                    stage = iommu::TransStage::STAGE2_ONLY;
                } else if (!stage1_bare && !stage2_bare) {
                    stage = iommu::TransStage::STAGE1_AND_2;
                } else {
                    stage = iommu::TransStage::STAGE1_ONLY;
                }
                
                std::vector<std::pair<uint64_t, iommu::PTData>> batch_updates;
                
                for (uint32_t i = 0; i < group.total_tasks; i++) {
                    uint64_t iova = group.group_iovas[i];
                    
                    iommu::PTData pt_data;
                    pt_data.vs_pte.raw = main_task->walk_ctx.pt_updates[i].vs_pte.raw;
                    pt_data.g_pte.raw = main_task->walk_ctx.pt_updates[i].g_pte.raw;
                    pt_data.reserved.raw = 0;
                    pt_data.reserved.valid = 1;
                    pt_data.reserved.trans_type = static_cast<uint32_t>(stage);
                    pt_data.reserved.input_page_size = 0;  // 4KB
                    pt_data.reserved.result_page_size = 0;  // 4KB
                    pt_data.reserved.iova_is_va = 1;
                    pt_data.reserved.sv48 = (main_task->iosatp.MODE == IOSATP_Sv48) ? 1U : 0U;
                    pt_data.reserved.gstage_x4 = (main_task->iohgatp.MODE == IOHGATP_Sv48x4) ? 1U : 0U;
                    pt_data.reserved.is_ph = 0;  // 常规CL
                    
                    batch_updates.push_back({iova, pt_data});
                    
                    printf("[PTW_PREFETCH_MONITOR] Batch update[%u]: iova=0x%lx, vs_ppn=0x%lx\n",
                           i, iova, main_task->walk_ctx.pt_updates[i].vs_pte.PPN);
                    fflush(stdout);
                }
                
                // ========== 收集所有Buffer链头（batch_update前，占位CL还保留head_index） ==========
                std::vector<uint8_t> chain_heads;
                for (uint32_t i = 0; i < group.total_tasks; i++) {
                    if (group.group_iovas[i] == 0) continue;
                    
                    iommu::PTData existing_data;
                    sc_time lat;
                    bool hit = cache_sub.pt_cache().lookup_pt(
                        main_task->GSCID, main_task->PSCID, group.group_iovas[i],
                        stage, sv48, gstage_x4, existing_data, lat);
                    
                    if (hit && existing_data.reserved.is_ph == 1) {
                        uint8_t h = existing_data.reserved.head_index;
                        // 检查是否已收集（去重）
                        bool dup = false;
                        for (uint8_t ch : chain_heads) {
                            if (ch == h) { dup = true; break; }
                        }
                        if (!dup && h != DEDUP_BUFFER_INVALID_IDX) {
                            chain_heads.push_back(h);
                        }
                    }
                }
                
                printf("[PTW_PREFETCH_MONITOR] Group %u: collected %zu buffer chain heads\n",
                       group_id, chain_heads.size());
                fflush(stdout);
                
                // ========== 批量更新PT Cache ==========
                cache_sub.pt_cache().batch_update_placeholders(
                    main_task->GSCID, main_task->PSCID, batch_updates,
                    stage, sv48, gstage_x4);
                
                printf("[PTW_PREFETCH_MONITOR] Group %u PT Cache batch update completed (%zu entries)\n",
                       group_id, batch_updates.size());
                fflush(stdout);
                
                // ========== Flush所有Buffer链（包括主链和预取链） ==========
                for (uint8_t h : chain_heads) {
                    flush_dedup_buffer_chain(h, group_id, main_task,
                                            group.group_iovas,
                                            group.total_tasks);
                }
                
                // 注意: 主任务已在flush_dedup_buffer_chain中转发,不再单独转发
                
                // 更新统计
                ptw_outstanding_task_count--;
                ptw_total_completed++;
                
                // [STAT] 累加PTW完成统计
                ptw_total_ddr_reads += main_task->walk_ctx.ddr_read_count;
                
                // [STAT] 计算任务总延时
                { auto _s = ptw_task_start_ns.find(main_task->task_id);
                  if (_s != ptw_task_start_ns.end()) {
                      double task_latency = sc_time_stamp().to_seconds()*1e9 - _s->second;
                      ptw_total_exec_ns += task_latency;
                      if (task_latency > ptw_max_task_latency_ns)
                          ptw_max_task_latency_ns = task_latency;
                      if (task_latency < ptw_min_task_latency_ns)
                          ptw_min_task_latency_ns = task_latency;
                      ptw_task_start_ns.erase(_s);
                  } }
                
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                
                printf("[PTW_PREFETCH_MONITOR] Group %u completed, flushed %zu buffer chains.\n",
                       group_id, chain_heads.size());
                fflush(stdout);
                
                // 删除组记录
                it = prefetch_groups.erase(it);
            } else {
                ++it;
            }
        }
        
        prefetch_group_mtx.unlock();
    }
}
