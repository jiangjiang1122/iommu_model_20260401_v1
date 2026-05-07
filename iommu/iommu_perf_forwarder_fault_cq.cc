// IOMMU Performance Model - Forwarder + Fault/CQ Threads (3 threads)
// SPEC Section 12.19-12.20
// Corresponds to iommu_translate.cc step 20 + report_fault()

#include "iommu_top.hh"
#include <cstdio>

// ============================================================
// 12.19a pt_forwarder_thread - PT Cache Translation Forwarding
// Reads from pt_cache_to_fwd_fifo, outputs to axi_master_0_to_pcie_noc_to_cmn_rni_socket
// Corresponds to iommu_translate.cc L494-613 (step 20)
// ============================================================
void iommu_top::pt_forwarder_thread() {
    while (true) {
        // Wait for PT cache forward FIFO
        if (pt_cache_to_fwd_fifo.num_available() == 0) {
            wait(pt_cache_to_fwd_fifo.data_written_event());
        }

        iommu_task_t* task = pt_cache_to_fwd_fifo.read();

        printf("[PT_FORWARDER] task_id=%u, iova=0x%lx, pa=0x%lx -> axi_master_0_to_pcie_noc_to_cmn_rni\n",
               task->task_id, task->iova, task->pa);
        fflush(stdout);

        wait(FORWARDER_DELAY, SC_NS);
        task->state = TASK_FORWARD;

        // ========== Step 20: Response Generation ==========
        tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;
        if (!trans) {
            // No TLM payload (should not happen in normal flow)
            task->state = TASK_DONE;
            delete task;
            continue;
        }

        trans->set_response_status(tlm::TLM_OK_RESPONSE);

        // Set translated PA into payload
        trans->set_address(task->pa);

        // PT cache path: output to axi_master_0_to_pcie_noc_to_cmn_rni_socket (logical routing)
        // In the performance model, we directly send response to initiator
        // The actual data path would go through axi_master_0_to_pcie_noc_to_cmn_rni_socket to PCIE NoC

        // Send response to initiator (with translated PA)
        send_response_to_initiator(task);

        // Release task (ownership endpoint for normal completion path)
        task->state = TASK_DONE;
        if (!task->is_b_transport) {
            delete task;
        }
    }
}

// ============================================================
// 12.19b msipt_forwarder_thread - MSIPT Cache Translation Forwarding
// Reads from msipt_cache_to_fwd_fifo, routes based on MSI/MRIF:
//   - is_msi=1 && is_mrif=0 -> axi_stream_socket (IMSIC)
//   - otherwise -> axi_master_1_to_cmn_rnd_socket (DDR/normal DMA)
// Corresponds to iommu_translate.cc L494-613 (step 20)
// ============================================================
void iommu_top::msipt_forwarder_thread() {
    while (true) {
        // Wait for MSIPT cache forward FIFO
        if (msipt_cache_to_fwd_fifo.num_available() == 0) {
            wait(msipt_cache_to_fwd_fifo.data_written_event());
        }

        iommu_task_t* task = msipt_cache_to_fwd_fifo.read();

        printf("[MSIPT_FORWARDER] task_id=%u, iova=0x%lx, pa=0x%lx, is_msi=%d, is_mrif=%d -> routing\n",
               task->task_id, task->iova, task->pa, task->is_msi, task->is_mrif);
        fflush(stdout);

        wait(FORWARDER_DELAY, SC_NS);
        task->state = TASK_FORWARD;

        // ========== Step 20: Response Generation ==========
        tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;
        if (!trans) {
            // No TLM payload (should not happen in normal flow)
            task->state = TASK_DONE;
            delete task;
            continue;
        }

        trans->set_response_status(tlm::TLM_OK_RESPONSE);

        // Set translated PA into payload
        trans->set_address(task->pa);

        // MSIPT cache path: route based on MSI/MRIF flags
        if (task->is_msi == 1 && task->is_mrif == 0) {
            // MSI forwarding to IMSIC via axi_stream_socket
            printf("[MSIPT_FORWARDER] task_id=%u -> MSI to IMSIC (axi_stream)\n", task->task_id);
            fflush(stdout);
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            sc_time delay = SC_ZERO_TIME;
            axi_stream_socket->nb_transport_fw(*trans, phase, delay);
        } else {
            // Normal DMA or MRIF: output via axi_master_1_to_cmn_rnd_socket (logical routing)
            printf("[MSIPT_FORWARDER] task_id=%u -> normal DMA/MRIF (axi_master_1)\n", task->task_id);
            fflush(stdout);
            // In the performance model, we directly send response to initiator
            // The actual data path would go through axi_master_1_to_cmn_rnd_socket to DDR/CMN
        }

        // Send response to initiator
        send_response_to_initiator(task);

        // Release task (ownership endpoint for normal completion path)
        task->state = TASK_DONE;
        if (!task->is_b_transport) {
            delete task;
        }
    }
}

// ============================================================
// 12.20 fault_cq_proc_thread - Fault Recording + CQ Processing
// Corresponds to report_fault() in iommu_faults.cc
// + fault classification in iommu_translate.cc L654-730
// ============================================================
void iommu_top::fault_cq_proc_thread() {
    while (true) {
        iommu_task_t* task = collector_to_fault_fifo.read();
        wait(FORWARDER_DELAY, SC_NS);

        printf("[FAULT_CQ] task_id=%u, cause=%d, device_id=0x%x, iova=0x%lx -> processing fault\n",
               task->task_id, task->cause, task->device_id, task->iova);
        fflush(stdout);

        // 1. Call report_fault to record fault
        // Corresponds to iommu_faults.cc L8-159
        // Note: report_fault() internally calls write_memory() which now routes
        // through ctrl_path_req_ddr_fifo (see iommu_ref_api.cc v2 modification)
        report_fault(&iommu_inst, task->cause, task->iotval, task->iotval2,
                     task->TTYP, task->DTF, task->device_id,
                     task->pid_valid, task->process_id, task->priv);

        // 2. Determine response type
        // Corresponds to iommu_translate.cc L654-730
        tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;
        if (!trans) {
            task->state = TASK_DONE;
            delete task;
            continue;
        }

        if (task->at != ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) {
            // Non-ATS: UNSUPPORTED_REQUEST (L657-661)
            trans->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        } else {
            // ATS: classify by cause code
            if (task->cause == 12 || task->cause == 13 || task->cause == 15 ||
                task->cause == 20 || task->cause == 21 || task->cause == 23 ||
                task->cause == 266 || task->cause == 262) {
                // ATS Success with R=W=0 (L722-730)
                // No fault logged for these cause codes
                trans->set_response_status(tlm::TLM_OK_RESPONSE);
            } else if (task->cause == 1 || task->cause == 5 || task->cause == 7 ||
                       task->cause == 261 || task->cause == 263 ||
                       task->cause == 265 || task->cause == 267 ||
                       task->cause == 268 || task->cause == 269 ||
                       task->cause == 270 || task->cause == 271 ||
                       task->cause == 272 || task->cause == 274) {
                // Completer Abort (L679-685)
                trans->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            } else {
                // Unsupported Request (L696-700)
                trans->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            }
        }

        // 3. Send response via nb_transport_bw
        send_response_to_initiator(task);

        // 4. Release task (ownership endpoint for fault path)
        task->state = TASK_DONE;
        if (!task->is_b_transport) {
            delete task;
        }
    }
}
