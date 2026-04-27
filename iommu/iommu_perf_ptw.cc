// IOMMU Performance Model - PTW Threads (2 threads)
// SPEC Section 12.13-12.14
// Corresponds to two_stage_address_translation() + second_stage_address_translation()

#include "iommu_top.hh"
#include <cstdio>

// ============================================================
// 12.13 ptw_req_thread - PTW Request Thread (async DDR send)
// Initializes VS/GS walk context, sends first DDR request
// ============================================================
void iommu_top::ptw_req_thread() {
    while (true) {
        // Flow control
        while (ptw_outstanding_task_count >= PTW_MAX_OUTSTANDING_TASKS) {
            wait(ptw_task_completed_event);
        }

        iommu_task_t* task = pt_cache_to_ptw_fifo.read();
        task->state = TASK_PTW_REQ;
        ptw_outstanding_task_count++;

        printf("[PTW_REQ] task_id=%u, iova=0x%lx, iosatp.MODE=%d, GV=%d -> start walk\n",
               task->task_id, task->iova, task->iosatp.MODE, task->GV);
        fflush(stdout);

        wait(PTW_COMPUTE_DELAY, SC_NS);

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
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                ptw_to_pt_cache_fifo.write(task);
                continue;
            }
        }

        // ========== 2. VS-stage Walk Initialization ==========
        {
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
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                ptw_to_pt_cache_fifo.write(task);
                continue;
            }

            int8_t i = LEVELS - 1;
            uint64_t a = task->iosatp.PPN * PAGESIZE;

            // Save VPN to walk context
            for (int k = 0; k < 5; k++) task->walk_ctx.vpn[k] = vpn[k];
            task->walk_ctx.max_levels = LEVELS;
            task->walk_ctx.ptesize = PTESIZE;

            task->walk_ctx.level = i;
            task->walk_ctx.base_addr = a;

            // Calculate first PTE address
            uint64_t pte_addr = a + vpn[i] * PTESIZE;

            // Check if G-stage implicit translation needed
            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                task->walk_ctx.pending_vs_pte_addr = pte_addr;
                task->walk_ctx.vs_level = i;
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
            task->walk_ctx.ddr_read_count = 1;
            printf("[PTW_REQ] task_id=%u, walk_phase=%d, addr=0x%lx, size=%d, read_count=1 -> ddr_req\n",
                   task->task_id, task->walk_ctx.walk_phase, req.addr, req.size);
            fflush(stdout);
            ptw_req_ddr_fifo.write(req);
        }
    }
}

// ============================================================
// 12.14 ptw_rsp_thread - PTW Response Thread (multi-phase SM)
// Processes DDR responses for VS/GS walk phases
// ============================================================
void iommu_top::ptw_rsp_thread() {
    while (true) {
        ddr_rsp_entry_t rsp = ptw_rsp_ddr_fifo.read();
        wait(PTW_PARSE_DELAY, SC_NS);

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

        bool walk_complete = false;
        bool walk_fault = false;
        bool need_next_ddr = false;

        printf("[PTW_RSP] task_id=%u, walk_phase=%d (VS_WALK=0, GS_IMPLICIT=1, GS_EXPLICIT=2, AD_UPDATE=3), read_count=%u\n",
               task->task_id, task->walk_ctx.walk_phase, task->walk_ctx.ddr_read_count);
        fflush(stdout);

        switch (task->walk_ctx.walk_phase) {

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

                // PA calculation
                task->gst_page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.gs_level; j++)
                    task->gst_page_sz *= 512;
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
            ptw_task_completed_event.notify(SC_ZERO_TIME);
            ptw_to_pt_cache_fifo.write(task);
        }
        else if (walk_complete) {
            printf("[PTW_RSP] task_id=%u -> WALK COMPLETE, pa=0x%lx, total_reads=%u -> pt_cache\n",
                   task->task_id, task->pa, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            ptw_walks_mtx.lock();
            ptw_active_walks.erase(rsp.task_id);
            ptw_walks_mtx.unlock();
            ptw_outstanding_task_count--;
            ptw_task_completed_event.notify(SC_ZERO_TIME);
            ptw_to_pt_cache_fifo.write(task);
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
            if (req.is_write) {
                memcpy(req.write_data, &task->walk_ctx.ad_pte,
                       task->walk_ctx.ptesize);
            }
            ptw_req_ddr_fifo.write(req);
        }
    }
}
