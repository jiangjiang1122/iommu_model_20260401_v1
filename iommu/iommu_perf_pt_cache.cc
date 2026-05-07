// IOMMU Performance Model - PT Cache Threads (3 threads)
// SPEC Section 12.10-12.12
// Corresponds to lookup_ioatc_iotlb() + cache_ioatc_iotlb() + MSI check

#include "iommu_top.hh"
#include <cstdio>

// ============================================================
// 12.10 pt_cache_query_thread - IOTLB Query
// Corresponds to lookup_ioatc_iotlb() in iommu_atc.cc
// ============================================================
void iommu_top::pt_cache_query_thread() {
    while (true) {
        iommu_task_t* task = collector_to_pt_cache_query_fifo.read();
        printf("[PT_CACHE_ENTRY] task_id=%u, time=%s, iova=0x%lx\n", 
               task->task_id, sc_time_stamp().to_string().c_str(), task->iova);
        fflush(stdout);
        
        task->state = TASK_TLB_QUERY;
        wait(PT_CACHE_HIT_DELAY, SC_NS);

        iommu_cache_mtx.lock();
        uint8_t ioatc_status = lookup_ioatc_iotlb(
            &iommu_inst, task->iova, task->check_access_perms,
            task->priv, task->is_read, task->is_write, task->is_exec,
            task->SUM, task->PSCV, task->PSCID, task->GV, task->GSCID,
            &task->cause, &task->pa, &task->page_sz,
            &task->vs_pte, &task->g_pte, &task->is_msi);
        iommu_cache_mtx.unlock();

        if (ioatc_status == IOATC_HIT) {
            g_pt_cache_hit_count++;
            printf("[PT_CACHE_EXIT] task_id=%u, time=%s, iova=0x%lx -> IOTLB HIT, pa=0x%lx -> forwarder (total_hit=%lu)\n",
                   task->task_id, sc_time_stamp().to_string().c_str(), task->iova, task->pa, g_pt_cache_hit_count);
            fflush(stdout);
            task->is_mrif = 0;
            task->state = TASK_FORWARD;
            pt_cache_to_fwd_fifo.write(task);
        }
        else if (ioatc_status == IOATC_FAULT) {
            printf("[PT_CACHE_EXIT] task_id=%u, time=%s, iova=0x%lx -> IOTLB FAULT\n", 
                   task->task_id, sc_time_stamp().to_string().c_str(), task->iova);
            fflush(stdout);
            task->state = TASK_FAULT;
            collector_to_fault_fifo.write(task);
        }
        else {
            // IOATC_MISS: send to PTW
            g_pt_cache_miss_count++;
            printf("[PT_CACHE_EXIT] task_id=%u, time=%s, iova=0x%lx -> IOTLB MISS -> PTW (total_miss=%lu)\n", 
                   task->task_id, sc_time_stamp().to_string().c_str(), task->iova, g_pt_cache_miss_count);
            fflush(stdout);
            task->state = TASK_TLB_MISS;
            pt_cache_to_ptw_fifo.write(task);
        }
    }
}

// ============================================================
// 12.11 pt_cache_result_thread
// Note: In SPEC, this thread's functionality is merged into
// pt_cache_query_thread (query + immediate routing).
// This thread serves as a placeholder for future separation.
// ============================================================
void iommu_top::pt_cache_result_thread() {
    while (true) {
        // This thread is a placeholder - routing is done in pt_cache_query_thread
        // Wait indefinitely (never executes meaningful work)
        wait(SC_ZERO_TIME);
        wait(1, SC_SEC);
    }
}

// ============================================================
// 12.12 pt_cache_ptw_rsp_thread - PTW Response Processing
// Processes PTW results: MSI check, PBMT aggregation, IOTLB fill
// Corresponds to iommu_translate.cc L388-492 (steps 18-19 post-processing)
// ============================================================
void iommu_top::pt_cache_ptw_rsp_thread() {
    while (true) {
        iommu_task_t* task = ptw_to_pt_cache_fifo.read();

        // Fault from PTW: forward to fault handler
        if (task->state == TASK_FAULT) {
            collector_to_fault_fifo.write(task);
            continue;
        }

        wait(PT_CACHE_HIT_DELAY, SC_NS);

        // Step 18: MSI address translation check
        // Corresponds to iommu_translate.cc L388-418
        if (task->DC.msiptp.MODE != MSIPTP_Off) {
            uint8_t is_msi_addr = check_is_msi_address(task->gpa, &task->DC, &iommu_inst);
            if (is_msi_addr) {
                printf("[PT_CACHE_PTW] task_id=%u, gpa=0x%lx -> MSI address, route to MSIPT cache\n",
                       task->task_id, task->gpa);
                fflush(stdout);
                task->is_msi = 1;
                task->state = TASK_MSI_QUERY;
                collector_to_msipt_cache_query_fifo.write(task);
                continue;
            }
        }

        // Step 19 post-processing: PBMT aggregation + page size merge
        // Corresponds to iommu_translate.cc L442-456
        task->vs_pte.PBMT = (task->vs_pte.PBMT != PMA) ?
                             task->vs_pte.PBMT : task->g_pte.PBMT;
        task->page_sz = (task->gst_page_sz < task->page_sz) ?
                         task->gst_page_sz : task->page_sz;
        task->pa = (task->pa & ~(task->page_sz - 1)) |
                   (task->iova & (task->page_sz - 1));

        // IOTLB cache fill
        // Corresponds to iommu_translate.cc L458-492
        uint64_t napot_ppn = (((task->pa & ~(task->page_sz - 1)) |
                              ((task->page_sz / 2) - 1)) / PAGESIZE);
        uint64_t napot_iova = (((task->iova & ~(task->page_sz - 1)) |
                               ((task->page_sz / 2) - 1)) / PAGESIZE);
        uint64_t napot_gpa = (((task->gpa & ~(task->page_sz - 1)) |
                              ((task->page_sz / 2) - 1)) / PAGESIZE);

        if (task->at == ADDR_TYPE_UNTRANSLATED &&
            (task->is_msi == 0 || (task->is_msi == 1 && task->is_mrif == 0))) {
            iommu_cache_mtx.lock();
            cache_ioatc_iotlb(&iommu_inst, napot_iova, task->GV, task->PSCV,
                              task->iohgatp.GSCID, task->PSCID,
                              &task->vs_pte, &task->g_pte, napot_ppn,
                              ((task->page_sz > PAGESIZE) ? 1 : 0), task->is_msi);
            iommu_cache_mtx.unlock();
        }

        // ATS translation: cache fill with T2GPA consideration
        if (task->TTYP == PCIE_ATS_TRANSLATION_REQUEST &&
            (task->is_msi == 0 || (task->is_msi == 1 && task->is_mrif == 0)) &&
            ((task->DC.tc.T2GPA == 1 &&
              ((iommu_inst.fill_ats_trans_in_ioatc & FILL_IOATC_ATS_T2GPA) != 0)) ||
             ((iommu_inst.fill_ats_trans_in_ioatc & FILL_IOATC_ATS_ALWAYS) != 0))) {
            iommu_cache_mtx.lock();
            cache_ioatc_iotlb(&iommu_inst,
                              (task->DC.tc.T2GPA == 1) ? napot_gpa : napot_iova,
                              task->GV,
                              (task->DC.tc.T2GPA == 1) ? 0 : task->PSCV,
                              task->iohgatp.GSCID,
                              (task->DC.tc.T2GPA == 1) ? 0 : task->PSCID,
                              &task->vs_pte, &task->g_pte, napot_ppn,
                              ((task->page_sz > PAGESIZE) ? 1 : 0), task->is_msi);
            iommu_cache_mtx.unlock();

            // Return GPA as translation response if T2GPA is 1
            if (task->DC.tc.T2GPA == 1) {
                task->pa = task->gpa;
            }
        }

        printf("[PT_CACHE_PTW] task_id=%u, iova=0x%lx -> PTW DONE, pa=0x%lx -> forwarder\n",
               task->task_id, task->iova, task->pa);
        fflush(stdout);
        task->state = TASK_FORWARD;
        pt_cache_to_fwd_fifo.write(task);
    }
}
