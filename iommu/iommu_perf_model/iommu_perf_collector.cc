// IOMMU Performance Model - Collector Threads (2 threads)
// SPEC Section 12.6-12.7
// Corresponds to iommu_translate_iova() steps 7-16

#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>

// ============================================================
// 12.6 collector_cache_lookup_result_thread
// Collector-1: Gather DC/PC cache results, route decision
// Corresponds to iommu_translate.cc L214-330
// ============================================================
void iommu_top::collector_cache_lookup_result_thread() {
    while (true) {
        // Check if data already available before waiting (avoid missing edge-triggered events)
        if (parser_to_collector_fifo.num_available() == 0 &&
            cache_sub.dc_response_fifo.num_available() == 0 &&
            cache_sub.pc_response_fifo.num_available() == 0) {
            wait(parser_to_collector_fifo.data_written_event() |
                 cache_sub.dc_response_fifo.data_written_event() |
                 cache_sub.pc_response_fifo.data_written_event());
        }

        // --- Process parser_to_collector_fifo ---
        while (parser_to_collector_fifo.num_available() > 0) {
            iommu_task_t* task = parser_to_collector_fifo.read();
            collector_mtx.lock();
            collector_entry_t& entry = pending_tasks[task->task_id];
            entry.task = task;
            entry.parser_arrived = true;
            collector_mtx.unlock();
        }

        // --- Process dc_cache_to_collector_fifo (from cache_sub.dc_response_fifo) ---
        while (cache_sub.dc_response_fifo.num_available() > 0) {
            iommu::CacheMessage resp = cache_sub.dc_response_fifo.read();
            
            std::cout << "[DC Lookup] task_id=" << resp.task_id
                      << " hit=" << resp.hit << std::endl;
            
            collector_mtx.lock();
            if (pending_tasks.find(resp.task_id) != pending_tasks.end()) {
                pending_tasks[resp.task_id].dc_done = true;
                
                // Convert CacheMessage response back to task fields
                iommu_task_t* task = pending_tasks[resp.task_id].task;
                dc_response_to_task(resp, task);
            }
            collector_mtx.unlock();
        }

        // --- Process pc_cache_to_collector_fifo (from cache_sub.pc_response_fifo) ---
        while (cache_sub.pc_response_fifo.num_available() > 0) {
            iommu::CacheMessage resp = cache_sub.pc_response_fifo.read();
            
            std::cout << "[PC Lookup] task_id=" << resp.task_id
                      << " hit=" << resp.hit << std::endl;
            
            collector_mtx.lock();
            if (pending_tasks.find(resp.task_id) != pending_tasks.end()) {
                pending_tasks[resp.task_id].pc_done = true;
                
                // Convert CacheMessage response back to task fields
                iommu_task_t* task = pending_tasks[resp.task_id].task;
                pc_response_to_task(resp, task);
            }
            collector_mtx.unlock();
        }

        // ========== Iterate pending_tasks: process completed entries ==========
        collector_mtx.lock();
        for (auto it = pending_tasks.begin(); it != pending_tasks.end(); ) {
            collector_entry_t& entry = it->second;
            if (!entry.parser_arrived || !entry.dc_done || !entry.pc_done) {
                ++it;
                continue;
            }

            iommu_task_t* task = entry.task;
            it = pending_tasks.erase(it);
            collector_mtx.unlock();

            // [PERF] Collector无需串行延时
            task->state = TASK_COLLECTING;

            // ===== DC Miss: need DDT walk =====
            if (!task->dc_hit) {
                printf("[COLLECTOR] task_id=%u, device_id=0x%x -> DC MISS, route to xDTW(DDT walk)\n",
                       task->task_id, task->device_id);
                fflush(stdout);
                task->walk_ctx.walk_type = WALK_DDT;
                task->state = TASK_DC_MISS;
                while (collector_dc_walk_outstanding >= COLLECTOR_MAX_DC_WALK_OUTSTANDING) {
                    wait(collector_dc_walk_completed_event);
                }
                collector_dc_walk_outstanding++;
                if (collector_dc_walk_outstanding > peak_collector_dc_walk_outstanding)
                    peak_collector_dc_walk_outstanding = collector_dc_walk_outstanding;
                collector_to_xdtw_dc_fifo.write(task);
                collector_mtx.lock();
                continue;
            }

            printf("[COLLECTOR] task_id=%u, device_id=0x%x -> DC HIT\n",
                   task->task_id, task->device_id);
            fflush(stdout);

            // ===== DC Hit: steps 7-13 checks =====
            // Step 7: EN_ATS check (iommu_translate.cc L214-218)
            if (task->DC.tc.EN_ATS == 0 &&
                (task->at == ADDR_TYPE_TRANSLATED ||
                 task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST)) {
                printf("[COLLECTOR] task_id=%u -> FAULT: EN_ATS=0 with Translated/ATS\n", task->task_id);
                fflush(stdout);
                task->cause = 260;
                task->state = TASK_FAULT;
                collector_to_fault_fifo.write(task);
                collector_mtx.lock();
                continue;
            }

            // Step 7: pid_valid + PDTV check (L220-223)
            if (task->pid_valid && task->DC.tc.PDTV == 0) {
                printf("[COLLECTOR] task_id=%u -> FAULT: pid_valid=1 but PDTV=0\n", task->task_id);
                fflush(stdout);
                task->cause = 260;
                task->state = TASK_FAULT;
                collector_to_fault_fifo.write(task);
                collector_mtx.lock();
                continue;
            }

            // Step 7: process_id width vs pdtp.MODE (L225-234)
            if (task->pid_valid && task->DC.tc.PDTV == 1) {
                if (task->DC.fsc.pdtp.MODE == PD17 &&
                    task->process_id > ((1UL << 17) - 1)) {
                    printf("[COLLECTOR] task_id=%u -> FAULT: process_id too wide for PD17\n", task->task_id);
                    fflush(stdout);
                    task->cause = 260;
                    task->state = TASK_FAULT;
                    collector_to_fault_fifo.write(task);
                    collector_mtx.lock();
                    continue;
                }
                if (task->DC.fsc.pdtp.MODE == PD8 &&
                    task->process_id > ((1UL << 8) - 1)) {
                    printf("[COLLECTOR] task_id=%u -> FAULT: process_id too wide for PD8\n", task->task_id);
                    fflush(stdout);
                    task->cause = 260;
                    task->state = TASK_FAULT;
                    collector_to_fault_fifo.write(task);
                    collector_mtx.lock();
                    continue;
                }
            }

            // Step 8: Translated + T2GPA=0 -> direct pass (L238-246)
            if (task->at == ADDR_TYPE_TRANSLATED && task->DC.tc.T2GPA == 0) {
                printf("[COLLECTOR] task_id=%u -> Translated+T2GPA=0, direct pass to forwarder\n", task->task_id);
                fflush(stdout);
                task->pa = task->iova;
                task->page_sz = PAGESIZE;
                task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = 1;
                task->vs_pte.PBMT = PMA;
                task->g_pte.X = task->g_pte.W = task->g_pte.R = 1;
                task->is_msi = 0;
                task->state = TASK_FORWARD;
                pt_cache_to_fwd_fifo.write(task);
                collector_mtx.lock();
                continue;
            }

            // Step 9: Translated + T2GPA=1 -> GPA, use Bare iosatp (L254-258)
            if (task->at == ADDR_TYPE_TRANSLATED && task->DC.tc.T2GPA == 1) {
                printf("[COLLECTOR] task_id=%u -> Translated+T2GPA=1, route to pt_cache_query\n", task->task_id);
                fflush(stdout);
                task->iosatp.MODE = IOSATP_Bare;
                task->SUM = 0;
                task->iohgatp = task->DC.iohgatp;
                configure_and_route(task);
                collector_mtx.lock();
                continue;
            }

            // Step 10: PDTV=0 → use DC.fsc as iosatp (L270-277)
            if (task->DC.tc.PDTV == 0) {
                printf("[COLLECTOR] task_id=%u, device_id=0x%x -> DC HIT, iosatp.MODE=%d, iohgatp.MODE=%d -> pt_cache_query\n",
                       task->task_id, task->device_id, task->DC.fsc.iosatp.MODE, task->DC.iohgatp.MODE);
                fflush(stdout);
                task->iosatp.MODE = task->DC.fsc.iosatp.MODE;
                task->iosatp.PPN = task->DC.fsc.iosatp.PPN;
                task->PSCID = task->DC.ta.PSCID;
                task->SUM = 0;
                task->iohgatp = task->DC.iohgatp;
                task->SXL = task->DC.tc.SXL;
                task->SADE = task->DC.tc.SADE;
                task->GADE = task->DC.tc.GADE;
                configure_and_route(task);
                collector_mtx.lock();
                continue;
            }

            // Step 11: DPE=1 + no process_id → default pid=0 (L281-285)
            if (task->DC.tc.DPE == 1 && task->pid_valid == 0) {
                task->pid_valid = 1;
                task->process_id = 0;
                task->priv_req = 0;
            }

            // Step 12-13: no pid_valid or PDTP_Bare → Bare iosatp (L297-302)
            if (task->pid_valid == 0 || task->DC.fsc.pdtp.MODE == PDTP_Bare) {
                task->iosatp.MODE = IOSATP_Bare;
                task->SUM = task->pid_valid;
                task->iohgatp = task->DC.iohgatp;
                task->SXL = task->DC.tc.SXL;
                task->SADE = task->DC.tc.SADE;
                task->GADE = task->DC.tc.GADE;
                configure_and_route(task);
                collector_mtx.lock();
                continue;
            }

            // Step 14: Need PC lookup - check PC cache result
            if (!task->pc_hit) {
                // PC miss: need PDT walk
                printf("[COLLECTOR] task_id=%u, device_id=0x%x, pid=%u -> PC MISS, route to xDTW(PDT walk)\n",
                       task->task_id, task->device_id, task->process_id);
                fflush(stdout);
                task->walk_ctx.walk_type = WALK_PDT;
                task->need_pc = 1;
                task->state = TASK_PC_MISS;
                while (collector_pc_walk_outstanding >= COLLECTOR_MAX_PC_WALK_OUTSTANDING) {
                    wait(collector_pc_walk_completed_event);
                }
                collector_pc_walk_outstanding++;
                if (collector_pc_walk_outstanding > peak_collector_pc_walk_outstanding)
                    peak_collector_pc_walk_outstanding = collector_pc_walk_outstanding;
                collector_to_xdtw_pc_fifo.write(task);
                collector_mtx.lock();
                continue;
            }

            printf("[COLLECTOR] task_id=%u, device_id=0x%x, pid=%u -> PC HIT\n",
                   task->task_id, task->device_id, task->process_id);
            fflush(stdout);

            // PC hit: Steps 15-16 (L309-330)
            // Step 15: ENS check
            if (task->PC.ta.ENS == 0 && task->pid_valid && task->priv_req) {
                printf("[COLLECTOR] task_id=%u -> FAULT: ENS=0 with priv_req\n", task->task_id);
                fflush(stdout);
                task->cause = 260;
                task->state = TASK_FAULT;
                collector_to_fault_fifo.write(task);
                collector_mtx.lock();
                continue;
            }

            // Step 16: Configure from PC
            task->iosatp.MODE = task->PC.fsc.iosatp.MODE;
            task->iosatp.PPN = task->PC.fsc.iosatp.PPN;
            task->PSCID = task->PC.ta.PSCID;
            task->SUM = task->PC.ta.SUM;
            task->iohgatp = task->DC.iohgatp;
            task->SXL = task->DC.tc.SXL;
            task->SADE = task->DC.tc.SADE;
            task->GADE = task->DC.tc.GADE;
            configure_and_route(task);

            collector_mtx.lock();
        }
        collector_mtx.unlock();
    }
}

// ============================================================
// 12.7 collector_xdtw_response_thread
// Collector-2: Process xDTW walk results
// ============================================================
void iommu_top::collector_xdtw_response_thread() {
    while (true) {
        iommu_task_t* task = xdtw_to_collector_fifo.read();
        // [PERF] collector_xdtw_response 无需串行延时

        // Fault from xDTW: forward to fault handler
        if (task->state == TASK_FAULT) {
            printf("[COLLECTOR_XDTW] task_id=%u -> FAULT from xDTW, cause=%d\n",
                   task->task_id, task->cause);
            fflush(stdout);
            if (task->walk_ctx.walk_type == WALK_DDT) {
                collector_dc_walk_outstanding--;
                collector_dc_walk_completed_event.notify(SC_ZERO_TIME);
            } else if (task->walk_ctx.walk_type == WALK_PDT) {
                collector_pc_walk_outstanding--;
                collector_pc_walk_completed_event.notify(SC_ZERO_TIME);
            }
            collector_to_fault_fifo.write(task);
            continue;
        }

        if (task->walk_ctx.walk_type == WALK_DDT) {
            // DDT walk completed: DC is now valid
            printf("[COLLECTOR_XDTW] task_id=%u, device_id=0x%x -> DDT walk DONE, DC valid\n",
                   task->task_id, task->device_id);
            fflush(stdout);
            collector_dc_walk_outstanding--;
            collector_dc_walk_completed_event.notify(SC_ZERO_TIME);
            task->dc_valid = 1;
            task->dc_hit = 1;
            task->DTF = task->DC.tc.DTF;

            // Update DC cache via CacheSubsystem
            iommu::CacheMessage dc_update_req = task_to_dc_update(task);
            cache_sub.dc_update_fifo.write(dc_update_req);

            // Steps 7-13 checks (same as above for DC hit path)
            if (task->DC.tc.EN_ATS == 0 &&
                (task->at == ADDR_TYPE_TRANSLATED ||
                 task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST)) {
                printf("[COLLECTOR_XDTW] task_id=%u -> FAULT: EN_ATS=0 with Translated/ATS\n", task->task_id);
                fflush(stdout);
                task->cause = 260;
                task->state = TASK_FAULT;
                collector_to_fault_fifo.write(task);
                continue;
            }
            if (task->pid_valid && task->DC.tc.PDTV == 0) {
                printf("[COLLECTOR_XDTW] task_id=%u -> FAULT: pid_valid=1 but PDTV=0\n", task->task_id);
                fflush(stdout);
                task->cause = 260;
                task->state = TASK_FAULT;
                collector_to_fault_fifo.write(task);
                continue;
            }
            if (task->pid_valid && task->DC.tc.PDTV == 1) {
                if (task->DC.fsc.pdtp.MODE == PD17 &&
                    task->process_id > ((1UL << 17) - 1)) {
                    printf("[COLLECTOR_XDTW] task_id=%u -> FAULT: process_id too wide for PD17\n", task->task_id);
                    fflush(stdout);
                    task->cause = 260;
                    task->state = TASK_FAULT;
                    collector_to_fault_fifo.write(task);
                    continue;
                }
                if (task->DC.fsc.pdtp.MODE == PD8 &&
                    task->process_id > ((1UL << 8) - 1)) {
                    printf("[COLLECTOR_XDTW] task_id=%u -> FAULT: process_id too wide for PD8\n", task->task_id);
                    fflush(stdout);
                    task->cause = 260;
                    task->state = TASK_FAULT;
                    collector_to_fault_fifo.write(task);
                    continue;
                }
            }

            // Step 8
            if (task->at == ADDR_TYPE_TRANSLATED && task->DC.tc.T2GPA == 0) {
                task->pa = task->iova;
                task->page_sz = PAGESIZE;
                task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = 1;
                task->vs_pte.PBMT = PMA;
                task->g_pte.X = task->g_pte.W = task->g_pte.R = 1;
                task->is_msi = 0;
                task->state = TASK_FORWARD;
                pt_cache_to_fwd_fifo.write(task);
                continue;
            }

            // Step 9
            if (task->at == ADDR_TYPE_TRANSLATED && task->DC.tc.T2GPA == 1) {
                task->iosatp.MODE = IOSATP_Bare;
                task->SUM = 0;
                task->iohgatp = task->DC.iohgatp;
                configure_and_route(task);
                continue;
            }

            // Step 10
            if (task->DC.tc.PDTV == 0) {
                task->iosatp.MODE = task->DC.fsc.iosatp.MODE;
                task->iosatp.PPN = task->DC.fsc.iosatp.PPN;
                task->PSCID = task->DC.ta.PSCID;
                task->SUM = 0;
                task->iohgatp = task->DC.iohgatp;
                task->SXL = task->DC.tc.SXL;
                task->SADE = task->DC.tc.SADE;
                task->GADE = task->DC.tc.GADE;
                configure_and_route(task);
                continue;
            }

            // Steps 11-13
            if (task->DC.tc.DPE == 1 && task->pid_valid == 0) {
                task->pid_valid = 1;
                task->process_id = 0;
                task->priv_req = 0;
            }
            if (task->pid_valid == 0 || task->DC.fsc.pdtp.MODE == PDTP_Bare) {
                task->iosatp.MODE = IOSATP_Bare;
                task->SUM = task->pid_valid;
                task->iohgatp = task->DC.iohgatp;
                task->SXL = task->DC.tc.SXL;
                task->SADE = task->DC.tc.SADE;
                task->GADE = task->DC.tc.GADE;
                configure_and_route(task);
                continue;
            }

            // Need PC: send PDT walk
            task->walk_ctx.walk_type = WALK_PDT;
            task->need_pc = 1;
            task->state = TASK_PC_MISS;
            while (collector_pc_walk_outstanding >= COLLECTOR_MAX_PC_WALK_OUTSTANDING) {
                wait(collector_pc_walk_completed_event);
            }
            collector_pc_walk_outstanding++;
            collector_to_xdtw_pc_fifo.write(task);
        }
        else if (task->walk_ctx.walk_type == WALK_PDT) {
            // PDT walk completed: PC is now valid
            printf("[COLLECTOR_XDTW] task_id=%u, device_id=0x%x, pid=%u -> PDT walk DONE, PC valid\n",
                   task->task_id, task->device_id, task->process_id);
            fflush(stdout);
            collector_pc_walk_outstanding--;
            collector_pc_walk_completed_event.notify(SC_ZERO_TIME);
            task->pc_valid = 1;
            task->pc_hit = 1;

            // Update PC cache via CacheSubsystem
            iommu::CacheMessage pc_update_req = task_to_pc_update(task);
            cache_sub.pc_update_fifo.write(pc_update_req);

            // Step 15: ENS check
            if (task->PC.ta.ENS == 0 && task->pid_valid && task->priv_req) {
                printf("[COLLECTOR_XDTW] task_id=%u -> FAULT: ENS=0 with priv_req\n", task->task_id);
                fflush(stdout);
                task->cause = 260;
                task->state = TASK_FAULT;
                collector_to_fault_fifo.write(task);
                continue;
            }

            // Step 16: Configure from PC
            task->iosatp.MODE = task->PC.fsc.iosatp.MODE;
            task->iosatp.PPN = task->PC.fsc.iosatp.PPN;
            task->PSCID = task->PC.ta.PSCID;
            task->SUM = task->PC.ta.SUM;
            task->iohgatp = task->DC.iohgatp;
            task->SXL = task->DC.tc.SXL;
            task->SADE = task->DC.tc.SADE;
            task->GADE = task->DC.tc.GADE;
            configure_and_route(task);
        }
    }
}
