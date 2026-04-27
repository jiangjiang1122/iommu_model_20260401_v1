// IOMMU Performance Model - MSIPT Cache + MSIPTW Threads (4 threads)
// SPEC Section 12.15-12.18
// Corresponds to msi_address_translation() in iommu_msi_trans.cc

#include "iommu_top.hh"
#include <cstdio>

// ============================================================
// 12.15 msipt_cache_query_thread - MSIPT Cache Query
// ============================================================
void iommu_top::msipt_cache_query_thread() {
    while (true) {
        iommu_task_t* task = collector_to_msipt_cache_query_fifo.read();
        task->state = TASK_MSI_QUERY;
        wait(MSIPT_CACHE_HIT_DELAY, SC_NS);

        // Note: The functional model doesn't have an explicit MSIPT cache.
        // In the performance model, we always treat MSI PTE lookups as misses
        // and route to MSIPTW for DDR walk.
        // Future enhancement: implement actual MSIPT cache with hit/miss logic.

        printf("[MSIPT_CACHE] task_id=%u, gpa=0x%lx -> MISS, route to MSIPTW\n",
               task->task_id, task->gpa);
        fflush(stdout);

        // Always miss: route to MSIPTW
        task->state = TASK_MSI_MISS;
        msipt_cache_to_msiptw_fifo.write(task);
    }
}

// ============================================================
// 12.16 msipt_cache_result_thread - MSIPT Cache Result (PTW return)
// Receives MSIPTW results, updates cache, forwards to Forwarder
// ============================================================
void iommu_top::msipt_cache_result_thread() {
    while (true) {
        iommu_task_t* task = msiptw_to_msipt_cache_fifo.read();

        if (task->state == TASK_FAULT) {
            printf("[MSIPT_CACHE] task_id=%u -> FAULT, route to fault_handler\n", task->task_id);
            fflush(stdout);
            collector_to_fault_fifo.write(task);
            continue;
        }

        wait(MSIPT_CACHE_HIT_DELAY, SC_NS);

        // PBMT aggregation (MSI translations)
        task->vs_pte.PBMT = (task->vs_pte.PBMT != PMA) ?
                             task->vs_pte.PBMT : task->g_pte.PBMT;

        printf("[MSIPT_CACHE] task_id=%u, pa=0x%lx, is_mrif=%d -> forwarder\n",
               task->task_id, task->pa, task->is_mrif);
        fflush(stdout);

        task->state = TASK_FORWARD;
        msipt_cache_to_fwd_fifo.write(task);
    }
}

// ============================================================
// 12.17 msiptw_req_thread - MSIPTW Request Thread (async DDR send)
// Calculates MSI PTE address, sends DDR read request
// Corresponds to iommu_msi_trans.cc L57-115
// ============================================================
void iommu_top::msiptw_req_thread() {
    while (true) {
        // Flow control
        while (msiptw_outstanding_task_count >= MSIPTW_MAX_OUTSTANDING_TASKS) {
            wait(msiptw_task_completed_event);
        }

        iommu_task_t* task = msipt_cache_to_msiptw_fifo.read();
        task->state = TASK_MSIPTW_REQ;
        msiptw_outstanding_task_count++;

        wait(MSIPTW_COMPUTE_DELAY, SC_NS);

        // ========== MSI PTE address calculation ==========
        // Corresponds to iommu_msi_trans.cc L57-115

        // 1. MGPAW calculation
        uint64_t mgpaw = iommu_inst.reg_file.capabilities.pas;
        uint64_t mgpaw_mask = (mgpaw < 64) ? ((1ULL << mgpaw) - 1) : ~0ULL;

        // 2. Extract interrupt file number I
        uint64_t I = msi_extract((task->gpa >> 12),
                                  (task->DC.msi_addr_mask.mask & mgpaw_mask));

        // 3. Calculate MSI PTE address
        uint64_t m = task->DC.msiptp.PPN * PAGESIZE;
        uint64_t msipte_addr = m | (I * 16);

        // Save walk context
        task->walk_ctx.walk_phase = MSIPTW_READ_MSIPTE;
        task->walk_ctx.read_addr = msipte_addr;
        task->walk_ctx.read_size = 16;  // MSI PTE = 128 bits = 16 bytes

        // Register in active_walks
        msiptw_walks_mtx.lock();
        msiptw_active_walks[task->task_id] = task;
        msiptw_walks_mtx.unlock();

        // Send DDR request
        ddr_req_entry_t req;
        req.task_id = task->task_id;
        req.addr = msipte_addr;
        req.size = 16;
        req.is_write = false;
        task->walk_ctx.ddr_read_count = 1;
        printf("[MSIPTW_REQ] task_id=%u, msipte_addr=0x%lx, read_count=1 -> ddr_req\n",
               task->task_id, msipte_addr);
        fflush(stdout);
        msiptw_req_ddr_fifo.write(req);
    }
}

// ============================================================
// 12.18 msiptw_rsp_thread - MSIPTW Response Thread (MSI PTE parse)
// Parses 16-byte MSI PTE, determines translation or fault
// Corresponds to iommu_msi_trans.cc L127-293
// ============================================================
void iommu_top::msiptw_rsp_thread() {
    while (true) {
        ddr_rsp_entry_t rsp = msiptw_rsp_ddr_fifo.read();
        wait(MSIPTW_PARSE_DELAY, SC_NS);

        // Find corresponding task
        msiptw_walks_mtx.lock();
        auto it = msiptw_active_walks.find(rsp.task_id);
        if (it == msiptw_active_walks.end()) {
            msiptw_walks_mtx.unlock();
            printf("[MSIPTW] ERROR: task_id %u not found in active_walks\n", rsp.task_id);
            continue;
        }
        iommu_task_t* task = it->second;
        msiptw_walks_mtx.unlock();

        memcpy(task->walk_ctx.read_buf, rsp.data, rsp.data_length);

        bool walk_complete = false;
        bool walk_fault = false;

        // ========== Parse 16-byte MSI PTE ==========
        // Corresponds to iommu_msi_trans.cc L127-293
        msipte_t msipte;
        msipte.raw[0] = 0;
        msipte.raw[1] = 0;
        memcpy(&msipte.raw[0], task->walk_ctx.read_buf, 16);

        // Validate MSI PTE
        if (msipte.V == 0) {
            task->cause = 262;  // MSI PTE not valid
            walk_fault = true;
        }
        else if (msipte.C == 1 || msipte.M == 0 || msipte.M == 2) {
            task->cause = 263;  // MSI PTE misconfigured
            walk_fault = true;
        }
        else if (msipte.M == 3) {
            // M=3: Basic Translate/RW mode
            if (msipte.translate_rw.reserved != 0) {
                task->cause = 263;
                walk_fault = true;
            } else {
                task->pa = (msipte.translate_rw.PPN * PAGESIZE) |
                           (task->gpa & 0xFFF);
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.X = 0;
                task->g_pte.PBMT = PMA;
                task->page_sz = PAGESIZE;
                task->is_mrif = 0;
                task->state = TASK_MSIPTW_DONE;
                walk_complete = true;
            }
        }
        else if (msipte.M == 1) {
            // M=1: MRIF mode
            if (iommu_inst.reg_file.capabilities.msi_mrif == 0) {
                task->cause = 263;
                walk_fault = true;
            } else {
                task->dest_mrif_addr = msipte.mrif.MRIF_ADDR_55_9 << 9;
                task->pa = msipte.mrif.NPPN * PAGESIZE;
                task->mrif_nid = (msipte.mrif.N10 << 10) | msipte.mrif.N90;
                task->is_mrif = 1;
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.X = 0;
                task->g_pte.PBMT = PMA;
                task->page_sz = PAGESIZE;
                task->state = TASK_MSIPTW_DONE;
                walk_complete = true;
            }
        }

        // Execute permission check for MSI
        if (walk_complete && task->is_exec == 1 &&
            task->check_access_perms == 1) {
            task->cause = 1;  // Instruction access fault
            walk_complete = false;
            walk_fault = true;
        }

        if (walk_fault) {
            task->state = TASK_FAULT;
            printf("[MSIPTW_RSP] task_id=%u -> WALK FAULT, cause=%d\n", task->task_id, task->cause);
            fflush(stdout);
        } else if (walk_complete) {
            printf("[MSIPTW_RSP] task_id=%u -> WALK COMPLETE, pa=0x%lx, is_mrif=%d -> msipt_cache\n",
                   task->task_id, task->pa, task->is_mrif);
            fflush(stdout);
        }

        // Cleanup active_walks and release outstanding count
        msiptw_walks_mtx.lock();
        msiptw_active_walks.erase(rsp.task_id);
        msiptw_walks_mtx.unlock();
        msiptw_outstanding_task_count--;
        msiptw_task_completed_event.notify(SC_ZERO_TIME);

        // Send to MSIPT Cache result thread
        msiptw_to_msipt_cache_fifo.write(task);
    }
}
