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

        // [PERF] pt_forwarder 无需串行延时
        task->state = TASK_FORWARD;

        // ========== Step 20: Response Generation ==========
        tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;
        if (!trans) {
            // No TLM payload (should not happen in normal flow)
            // 仍走 reorder_mark_ready 以释放 reorder_buf / outstanding
            reorder_mark_ready(task);
            continue;
        }

        trans->set_response_status(tlm::TLM_OK_RESPONSE);

        // Set translated PA into payload
        trans->set_address(task->pa);

        // PT cache path: output to axi_master_0_to_pcie_noc_to_cmn_rni_socket (logical routing)
        // In the performance model, we directly send response to initiator
        // The actual data path would go through axi_master_0_to_pcie_noc_to_cmn_rni_socket to PCIE NoC

        // 入口已注册 reorder_buf：此处仅标记 ready，由 reorder_output_thread 真正下发
        // 写请求按 task_id 保序输出，读请求可乱序输出。
        task->state = TASK_FORWARD;
        reorder_mark_ready(task);
        // 注意：task 释放与 outstanding 释放交由 reorder_output_thread
    }
}

// ============================================================
// 12.19b msipt_forwarder_thread - MSIPT Mode Dispatch + 出口执行 (PIPE7, 点10)
// Reads from msipt_cache_to_fwd_fifo, 根据MSI PTE解码结果选择输出:
//   Flat/IMSIC Access: translated_addr = (PTE.PPN<<12)|A[11:0] 已在MSIPTW算好,
//                      从 AXI Master0 出口输出, 与普通地址翻译出口一致(写保序);
//   MRIF Access      : pending word原子OR写(AMO_MRIF)从 AXI Master2(DDR端口)
//                      输出, 写DDR即完成; 独立仲裁源DDR_SRC_MSI_MRIF,
//                      响应静默丢弃, 与ctrl/PTW/MSIPTW配对机制隔离。
// ============================================================
void iommu_top::msipt_forwarder_thread() {
    while (true) {
        // Wait for MSIPT cache forward FIFO
        if (msipt_cache_to_fwd_fifo.num_available() == 0) {
            wait(msipt_cache_to_fwd_fifo.data_written_event());
        }

        iommu_task_t* task = msipt_cache_to_fwd_fifo.read();

        printf("[MSIPT_FORWARDER] task_id=%u, iova=0x%lx, gpa=0x%lx, pa=0x%lx, is_msi=%d, is_mrif=%d -> dispatch\n",
               task->task_id, task->iova, task->gpa, task->pa, task->is_msi, task->is_mrif);
        fflush(stdout);

        task->state = TASK_FORWARD;

        tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;
        if (!trans) {
            // No TLM payload (should not happen in normal flow)
            // 仍走 reorder_mark_ready 以释放 reorder_buf / outstanding
            reorder_mark_ready(task);
            continue;
        }

        trans->set_response_status(tlm::TLM_OK_RESPONSE);

        if (task->is_mrif == 0) {
            // ===== Flat/IMSIC Access: AXI Master0 出口, 与普通翻译出口一致 =====
            // 写地址=translated_addr, 写数据=原始MSI写数据, 发往目标IMSIC
            trans->set_address(task->pa);
            msipt_flat_count++;
            printf("[MSIPT_FORWARDER] task_id=%u -> FLAT/IMSIC MSI write, pa=0x%lx (master_0 exit)\n",
                   task->task_id, task->pa);
            fflush(stdout);
        } else {
            // ===== MRIF Access: AXI Master2(DDR端口)写, 写DDR即完成 =====
            // 1. 从原始MSI写数据提取 interrupt identity
            uint32_t iid = 0;
            if (trans->get_data_ptr() != nullptr && trans->get_data_length() >= 4) {
                memcpy(&iid, trans->get_data_ptr(), 4);
            }

            // 2. MRIF pending word 地址与bit mask:
            //    pending_addr = dest_mrif_addr + (iid/32)*4, bit = 1<<(iid%32)
            uint64_t pending_addr = task->dest_mrif_addr + (uint64_t)(iid / 32) * 4;
            uint32_t pending_bit  = 1u << (iid % 32);

            // 3. 经DDR仲裁独立源发起4B写(建模AMO_MRIF原子OR), 响应静默丢弃;
            //    仲裁器内的master_1带宽延时建模自动生效, 此处只记账不重复建模
            ddr_req_entry_t mrif_req;
            mrif_req.task_id = task->task_id;
            mrif_req.addr = pending_addr;
            mrif_req.size = 4;
            mrif_req.is_write = true;
            memcpy(mrif_req.write_data, &pending_bit, 4);
            mrif_req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;
            msi_mrif_req_ddr_fifo.write(mrif_req);
            master_1_total_bytes += 4;  // [STAT]
            msi_atomic_or_count++;
            msipt_mrif_count++;
            printf("[MSIPT_FORWARDER] task_id=%u -> MRIF Access: pending_addr=0x%lx, bit=0x%x (iid=%u), master_2 exit\n",
                   task->task_id, pending_addr, pending_bit, iid);
            fflush(stdout);

            // 原始请求的响应地址置为notice地址(NPPN<<12, 即task->pa)
            trans->set_address(task->pa);
        }

        // 回响应给发起方: 经统一出口(写保序), 与正常翻译一致;
        // task 释放与 outstanding 释放交由 reorder_output_thread
        reorder_mark_ready(task);
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
        // [PERF] fault_cq 无需串行延时

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
        // 入口已注册 reorder_buf：此处仅标记 ready，由 reorder_output_thread 真正下发
        // Fault 路径与正常路径都进入同一个重排序队列，统一保证写保序读乱序的语义
        reorder_mark_ready(task);
        // 注意：task 释放与 outstanding 释放交由 reorder_output_thread
    }
}
