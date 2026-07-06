#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>
#include <iostream>

// ===================== Cache命中率统计全局变量定义 =====================
uint64_t g_dc_cache_hit_count = 0;
uint64_t g_dc_cache_miss_count = 0;
uint64_t g_pc_cache_hit_count = 0;
uint64_t g_pc_cache_miss_count = 0;
uint64_t g_pt_cache_hit_count = 0;
uint64_t g_pt_cache_miss_count = 0;
uint64_t g_msipt_cache_hit_count = 0;
uint64_t g_msipt_cache_miss_count = 0;

/**********************************************/
// before_end_of_elaboration - IOMMU reset and initialization
/**********************************************/
void iommu_top::before_end_of_elaboration()
{
    capabilities_t cap = {0};
    fctl_t fctl = {0};
    uint64_t sv57_bare_sz, sv48_bare_sz, sv39_bare_sz, sv32_bare_sz;
    uint64_t sv57x4_bare_sz, sv48x4_bare_sz, sv39x4_bare_sz, sv32x4_bare_sz;

    // Reset the IOMMU
    cap.version = 0x10;
    cap.Sv39 = cap.Sv48 = cap.Sv57 = cap.Sv39x4 = cap.Sv48x4 = cap.Sv57x4 = 1;
    cap.amo_hwad = cap.ats = cap.t2gpa = cap.hpm = cap.msi_flat = cap.msi_mrif = cap.amo_mrif = 1;
    cap.dbg = 1;
    cap.pas = 50;
    cap.pd20 = cap.pd17 = cap.pd8 = 1;
    cap.Svrsw60t59b = 1;
    sv57_bare_sz = sv48_bare_sz = sv39_bare_sz = 0x40000000;
    sv32_bare_sz = 0x200000;
    sv57x4_bare_sz = sv48x4_bare_sz = sv39x4_bare_sz = 0x40000000;
    sv32x4_bare_sz = 0x200000;
    int reset_result = reset_iommu(&iommu_inst, 8, 40, 0xff, 3, Off, DDT_3LVL, 0xFFFFFF, 0, 0,
                           (FILL_IOATC_ATS_T2GPA | FILL_IOATC_ATS_ALWAYS),
                           cap, fctl, sv57_bare_sz, sv48_bare_sz, sv39_bare_sz,
                           sv32_bare_sz, sv57x4_bare_sz, sv48x4_bare_sz, sv39x4_bare_sz,
                           sv32x4_bare_sz);
    if (reset_result < 0) {
        printf("\x1B[31mFAIL. Line %d\x1B[0m\n", __LINE__);
    }

    iommu_inst.reg_file.ddtp.iommu_mode = DDT_1LVL;
}

/**********************************************/
// AHB slave b_transport - register access (non-performance path)
/**********************************************/
void iommu_top::ahb_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay)
{
    tlm::tlm_command cmd = trans.get_command();
    sc_dt::uint64 addr = trans.get_address();
    unsigned char *data = trans.get_data_ptr();
    unsigned int len = trans.get_data_length();

    if (cmd == tlm::TLM_READ_COMMAND)
    {
        uint16_t offset = addr - IOMMU_BASE_ADDR;
        uint8_t num_bytes = 4;
        uint64_t reg_value = read_register(&iommu_inst, offset, num_bytes);
        memcpy(data, (unsigned char *)&reg_value, len);
        printf("%s:%d Read addr 0x%04llx, val 0x%08x\n", __func__, __LINE__,
               (unsigned long long)addr, *(unsigned int *)data);
    }
    else if (cmd == tlm::TLM_WRITE_COMMAND)
    {
        uint64_t val = 0;
        memcpy(&val, data, len);
        uint16_t offset = addr - IOMMU_BASE_ADDR;
        uint8_t num_bytes = 4;
        write_register(&iommu_inst, offset, num_bytes, val);
    }
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

/**********************************************/
// Legacy axi_slave_b_transport - kept for backward compatibility
// In performance model, nb_transport_fw is the primary path
/**********************************************/
void iommu_top::axi_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay)
{
    // In performance model, this path is deprecated.
    // Redirect to nb_transport path by creating a task and pushing to inbound_fifo
    PayloadExtention *ext = nullptr;
    trans.get_extension(ext);
    if (!ext) {
        trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }

    // For backward compatibility with RP test thread that still uses b_transport
    // Create task and process synchronously
    iommu_task_t* task = new iommu_task_t();

    task_id_mtx.lock();
    task->task_id = next_task_id++;
    task_id_mtx.unlock();

    task->tlm_trans_ptr = &trans;
    task->device_id = ext->requester_id;
    task->pid_valid = ext->pid_valid;
    task->process_id = ext->process_id;
    task->exec_req = ext->exec_req;
    task->priv_req = ext->priv_req;
    task->no_write = ext->no_write;
    task->is_cxl_dev = 0;
    task->iova = trans.get_address();
    task->length = trans.get_data_length();
    task->read_writeAMO = (trans.get_command() == tlm::TLM_READ_COMMAND) ? READ : WRITE;

    if (ext->at == 0) task->at = ADDR_TYPE_UNTRANSLATED;
    else if (ext->at == 1) task->at = ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST;
    else if (ext->at == 2) task->at = ADDR_TYPE_TRANSLATED;

    task->timestamp = sc_time_stamp();
    task->state = TASK_INIT;
    task->is_b_transport = 1;

    // Push to inbound FIFO for pipeline processing
    inbound_fifo.write(task);

    // For b_transport compatibility, we need to wait for completion
    // The forwarder/fault thread will signal completion through the response event
    // We use a simple polling approach with wait
    while (task->state != TASK_DONE) {
        wait(1, SC_NS);
    }

    trans.set_response_status(tlm::TLM_OK_RESPONSE);
    delete task;
}

/**********************************************/
// Inbound AT nb_transport_fw callback (RP -> IOMMU)
/**********************************************/
tlm::tlm_sync_enum iommu_top::axi_slave_nb_transport_fw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_REQ) {
        // ===== Bandwidth control: slave port accept delay =====
        // delay_ns = 1000 * length * 8 / bandwidth_mbps
        unsigned int data_len = trans.get_data_length();
        slave_0_total_bytes += data_len;  // [STAT] 入口字节计数
        double slave_bw_delay_ns = 1000.0 * data_len * 8 / AXI_SLAVE_0_BANDWIDTH_MBPS;
        wait(slave_bw_delay_ns, SC_NS);

        // 1. Extract PayloadExtention
        PayloadExtention* ext = nullptr;
        trans.get_extension(ext);

        // 2. Create task and fill fields
        iommu_task_t* task = new iommu_task_t();

        task_id_mtx.lock();
        task->task_id = next_task_id++;
        task_id_mtx.unlock();

        task->tlm_trans_ptr = &trans;
        task->iova = trans.get_address();
        task->length = trans.get_data_length();
        task->read_writeAMO = (trans.get_command() == tlm::TLM_READ_COMMAND) ? READ : WRITE;

        if (ext) {
            task->device_id = ext->requester_id;
            task->pid_valid = ext->pid_valid;
            task->process_id = ext->process_id;
            task->exec_req = ext->exec_req;
            task->priv_req = ext->priv_req;
            task->no_write = ext->no_write;
            task->is_cxl_dev = 0;

            if (ext->at == 0) task->at = ADDR_TYPE_UNTRANSLATED;
            else if (ext->at == 1) task->at = ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST;
            else if (ext->at == 2) task->at = ADDR_TYPE_TRANSLATED;
        }

        task->timestamp = sc_time_stamp();
        task->state = TASK_INIT;

        double ts_ns = sc_time_stamp().to_seconds() * 1e9;
        printf("[IOMMU_TOP] axi_slave_nb_transport_fw: task_id=%u, device_id=0x%x, iova=0x%lx, at=%d -> inbound_fifo [t=%.1f ns]\n",
               task->task_id, task->device_id, task->iova, task->at, ts_ns);
        fflush(stdout);

        // 3. Push to inbound_fifo
        inbound_fifo.write(task);

        // 4. Return END_REQ (request accepted)
        phase = tlm::END_REQ;
        return tlm::TLM_UPDATED;
    }
    else if (phase == tlm::END_RESP) {
        // Initiator confirmed receiving response
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

/**********************************************/
// DDR response AT nb_transport_bw callback (DDR -> IOMMU)
/**********************************************/
tlm::tlm_sync_enum iommu_top::ddr_nb_transport_bw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_RESP) {
        // Pop from ddr_pending_queue head (FIFO ordering guarantee)
        ddr_queue_mtx.lock();
        if (ddr_pending_queue.empty()) {
            ddr_queue_mtx.unlock();
            printf("[IOMMU] ERROR: DDR response received but pending queue is empty!\n");
            fflush(stdout);
            phase = tlm::END_RESP;
            return tlm::TLM_COMPLETED;
        }
        ddr_pending_entry_t pending = ddr_pending_queue.front();
        ddr_pending_queue.pop();
        ddr_queue_mtx.unlock();

        // Construct response entry
        ddr_rsp_entry_t rsp;
        rsp.task_id = pending.task_id;
        rsp.data_length = trans.get_data_length();
        if (rsp.data_length > sizeof(rsp.data)) rsp.data_length = sizeof(rsp.data);
        memcpy(rsp.data, trans.get_data_ptr(), rsp.data_length);
        rsp.error = (trans.get_response_status() != tlm::TLM_OK_RESPONSE);
        rsp.submit_time_ns = pending.submit_time_ns;  // [STAT] 传递时间戳

        // Route to corresponding rsp_ddr_fifo based on source_module
        // Use blocking write() to prevent silent drop when fifo is full
        switch (pending.source_module) {
            case DDR_SRC_XDTW:
                xdtw_rsp_ddr_fifo.write(rsp);
                break;
            case DDR_SRC_PTW:
                ptw_rsp_ddr_fifo.write(rsp);
                break;
            case DDR_SRC_MSIPTW:
                msiptw_rsp_ddr_fifo.write(rsp);
                break;
            case DDR_SRC_CTRL_PATH:
                // Control path: copy data to ctrl_path_rsp_buf and notify
                memcpy(ctrl_path_rsp_buf, rsp.data, rsp.data_length);
                ctrl_path_rsp_event.notify(SC_ZERO_TIME);
                break;
        }

        // Free TLM payload allocated by arbiter
        if (pending.trans_ptr) {
            if (pending.trans_ptr->get_data_ptr()) {
                delete[] pending.trans_ptr->get_data_ptr();
            }
            delete pending.trans_ptr;
        }

        // Decrement axi_master_1 outstanding counter
        axi_master_1_to_cmn_rnd_outstanding--;
        printf("[DDR_RSP] axi_master_1 outstanding-- -> %d (task_id=%u, source=%d)\n",
               axi_master_1_to_cmn_rnd_outstanding, pending.task_id, pending.source_module);
        fflush(stdout);
        axi_master_1_slot_freed_event.notify(SC_ZERO_TIME);

        // Notify arbiter that a slot is freed
        ddr_pending_freed_event.notify(SC_ZERO_TIME);

        // Send END_RESP confirmation
        phase = tlm::END_RESP;
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

/**********************************************/
// DDR Arbiter Thread
/**********************************************/
void iommu_top::ddr_arbiter_thread() {
    uint8_t rr_index = 0;  // round-robin rotation index
    while (true) {
        // Wait for any req_ddr_fifo to have data
        // Check if data already available before waiting (avoid missing edge-triggered events)
        if (ctrl_path_req_ddr_fifo.num_available() == 0 &&
            xdtw_req_ddr_fifo.num_available() == 0 &&
            ptw_req_ddr_fifo.num_available() == 0 &&
            msiptw_req_ddr_fifo.num_available() == 0) {
            wait(ctrl_path_req_ddr_fifo.data_written_event() |
                 xdtw_req_ddr_fifo.data_written_event() |
                 ptw_req_ddr_fifo.data_written_event() |
                 msiptw_req_ddr_fifo.data_written_event());
        }

        // Process all available requests
        bool processed_any = true;
        while (processed_any) {
            processed_any = false;

            // Flow control: check global outstanding count
            if (ddr_pending_queue.size() >= DDR_MAX_OUTSTANDING) {
                wait(ddr_pending_freed_event);
            }

            // Flow control: check axi_master_1_to_cmn_rnd outstanding count
            if (axi_master_1_to_cmn_rnd_outstanding >= AXI_MASTER_1_TO_CMN_RND_MAX_OUTSTANDING) {
                wait(axi_master_1_slot_freed_event);
            }

            ddr_req_entry_t req;
            uint8_t source_module = 0;
            bool found = false;

            // Priority: ctrl_path > round-robin(xDTW, PTW, MSIPTW)
            if (ctrl_path_req_ddr_fifo.num_available() > 0) {
                ctrl_path_ddr_req_t ctrl_req = ctrl_path_req_ddr_fifo.read();
                req.task_id = 0;  // control path has no task_id
                req.addr = ctrl_req.addr;
                req.size = ctrl_req.size;
                req.is_write = ctrl_req.is_write;
                if (ctrl_req.is_write) {
                    memcpy(req.write_data, ctrl_req.write_data, ctrl_req.size);
                }
                source_module = DDR_SRC_CTRL_PATH;
                found = true;
            } else {
                // Round-robin arbitration: xDTW(0), PTW(1), MSIPTW(2)
                sc_fifo<ddr_req_entry_t>* fifos[3] = {
                    &xdtw_req_ddr_fifo, &ptw_req_ddr_fifo, &msiptw_req_ddr_fifo
                };
                for (int k = 0; k < 3; k++) {
                    uint8_t idx = (rr_index + k) % 3;
                    if (fifos[idx]->num_available() > 0) {
                        req = fifos[idx]->read();
                        source_module = idx;  // 0=XDTW, 1=PTW, 2=MSIPTW
                        rr_index = (idx + 1) % 3;
                        found = true;
                        break;
                    }
                }
            }

            if (!found) break;
            processed_any = true;

            const char* src_name = (source_module == DDR_SRC_XDTW) ? "XDTW" :
                                   (source_module == DDR_SRC_PTW) ? "PTW" :
                                   (source_module == DDR_SRC_MSIPTW) ? "MSIPTW" : "CTRL";
            printf("[DDR_ARBITER] Route %s request: task_id=%u, addr=0x%lx, size=%d, is_write=%d -> DDR\n",
                   src_name, req.task_id, req.addr, req.size, req.is_write);
            fflush(stdout);

            // Construct ddr_pending_entry and enqueue
            ddr_pending_entry_t pending;
            pending.task_id = req.task_id;
            pending.source_module = source_module;
            pending.addr = req.addr;
            pending.size = req.size;
            pending.submit_time_ns = req.submit_time_ns;  // [STAT] 传递时间戳

            // Allocate TLM payload
            tlm::tlm_generic_payload* trans = new tlm::tlm_generic_payload();
            trans->set_address(req.addr);
            trans->set_data_length(req.size);
            trans->set_streaming_width(req.size);
            trans->set_byte_enable_ptr(nullptr);
            trans->set_dmi_allowed(false);
            trans->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

            uint8_t* data_buf = new uint8_t[req.size];
            if (req.is_write) {
                trans->set_command(tlm::TLM_WRITE_COMMAND);
                memcpy(data_buf, req.write_data, req.size);
            } else {
                trans->set_command(tlm::TLM_READ_COMMAND);
                memset(data_buf, 0, req.size);
            }
            trans->set_data_ptr(data_buf);
            pending.trans_ptr = trans;

            ddr_queue_mtx.lock();
            ddr_pending_queue.push(pending);
            ddr_queue_mtx.unlock();

            // Send nb_transport_fw to DDR
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            sc_time delay = SC_ZERO_TIME;
            axi_master_1_to_cmn_rnd_socket->nb_transport_fw(*trans, phase, delay);

            // ===== Bandwidth control: master_1 port (DDR access) =====
            // delay_ns = 1000 * length * 8 / bandwidth_mbps
            double master1_bw_delay_ns = 1000.0 * req.size * 8 / AXI_MASTER_1_BANDWIDTH_MBPS;
            master_1_total_bytes += req.size;  // [STAT] DDR字节计数
            wait(master1_bw_delay_ns, SC_NS);

            // Increment outstanding counter after successful send
            axi_master_1_to_cmn_rnd_outstanding++;
            if (axi_master_1_to_cmn_rnd_outstanding > peak_axi_master_1_outstanding)
                peak_axi_master_1_outstanding = axi_master_1_to_cmn_rnd_outstanding;
            printf("[DDR_ARBITER] axi_master_1 outstanding++ -> %d (task_id=%u)\n",
                   axi_master_1_to_cmn_rnd_outstanding, req.task_id);
            fflush(stdout);
        }
    }
}

/**********************************************/
// send_response_to_initiator - send AT response back to RP
/**********************************************/
void iommu_top::send_response_to_initiator(iommu_task_t* task) {
    tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;
    if (!trans) return;

    // Set translation result into payload
    trans->set_address(task->pa);

    // For b_transport tasks, skip nb_transport_bw (response returned via b_transport return path)
    if (task->is_b_transport) {
        return;
    }

    double ts_ns = sc_time_stamp().to_seconds() * 1e9;
    double entry_ns = task->timestamp.to_seconds() * 1e9;
    double e2e_ns = ts_ns - entry_ns;
    printf("[IOMMU_TOP] send_response_to_initiator: task_id=%u, pa=0x%lx, state=%d [entry=%.1f ns, exit=%.1f ns, e2e=%.1f ns]\n",
           task->task_id, task->pa, task->state, entry_ns, ts_ns, e2e_ns);
    fflush(stdout);

    // Send nb_transport_bw to notify initiator
    tlm::tlm_phase phase = tlm::BEGIN_RESP;
    sc_time delay = SC_ZERO_TIME;
    axi_slave_from_pcie_noc_0_socket->nb_transport_bw(*trans, phase, delay);
}

/**********************************************/
// configure_and_route - common routing after DC/PC configuration
/**********************************************/
void iommu_top::configure_and_route(iommu_task_t* task) {
    // Configure translation parameters
    task->PSCV = (task->iosatp.MODE == IOSATP_Bare) ? 0 : 1;
    task->GV = (task->iohgatp.MODE == IOHGATP_Bare) ? 0 : 1;
    task->GSCID = (task->GV == 0) ? 0 : task->iohgatp.GSCID;
    task->PSCID = (task->PSCV == 0) ? 0 : task->PSCID;
    task->check_access_perms =
        (task->TTYP != PCIE_ATS_TRANSLATION_REQUEST) ? 1 : 0;

    // Check for MSI address
    if (task->DC.msiptp.MODE != MSIPTP_Off) {
        // MSI check happens after PTW, route to PT cache first
    }

    task->state = TASK_ROUTE_DECISION;
    
    // [NEW] Phase 1: 启用预取功能 (使用配置参数PT_DEDUP_PREFETCH_DEPTH)
    // D=0表示关闭预取，D>0表示启用预取
    if (!task->walk_ctx.prefetch_enabled && PT_DEDUP_PREFETCH_DEPTH > 0) {
        task->walk_ctx.prefetch_enabled = true;
        task->walk_ctx.prefetch_depth = PT_DEDUP_PREFETCH_DEPTH;
        printf("[CONFIGURE] task_id=%u -> Prefetch enabled (D=%u)\n",
               task->task_id, task->walk_ctx.prefetch_depth);
    } else if (PT_DEDUP_PREFETCH_DEPTH == 0) {
        task->walk_ctx.prefetch_enabled = false;
        task->walk_ctx.prefetch_depth = 0;
        printf("[CONFIGURE] task_id=%u -> Prefetch DISABLED (D=0)\n", task->task_id);
    }

    // Save original task to pending map for PT cache response correlation
    pt_cache_mtx.lock();
    pt_cache_pending_tasks[task->task_id] = task;
    pt_cache_mtx.unlock();

    // Convert task to CacheMessage and write to cache_sub.pt_request_fifo
    iommu::CacheMessage pt_req = task_to_pt_request(task);
    pt_req.timestamp = sc_time_stamp();  // [STAT] 记录FIFO写入时刻
    cache_sub.pt_request_fifo.write(pt_req);
}

/**********************************************/
// init_gstage_walk - initialize G-stage walk context
/**********************************************/
void iommu_top::init_gstage_walk(iommu_task_t* task, uint64_t gpa) {
    // Corresponds to iommu_second_stage_trans.cc L80-139
    uint16_t gs_vpn[5];
    uint8_t GS_LEVELS;
    extract_gs_vpn(gpa, task->iohgatp.MODE, gs_vpn, &GS_LEVELS);
    for (int k = 0; k < 5; k++) task->walk_ctx.gs_vpn[k] = gs_vpn[k];
    task->walk_ctx.gs_level = GS_LEVELS - 1;
    task->walk_ctx.gs_base_addr = task->iohgatp.PPN * PAGESIZE;
    uint64_t gs_pte_addr = task->walk_ctx.gs_base_addr +
                           gs_vpn[GS_LEVELS - 1] * 8; // G-stage PTESIZE=8
    task->walk_ctx.read_addr = gs_pte_addr;
    task->walk_ctx.read_size = 8;
}

/**********************************************/
// va_dedup_recover - VA去重恢复：PTW完成后将挂起的同页任务批量转发
/**********************************************/
void iommu_top::va_dedup_recover(iommu_task_t* completed_task, bool is_fault) {
    if (!PT_CACHE_VA_DEDUP_ENABLED) return;

    // [FIX] 提前缓存 completed_task 数据到局部变量，避免后续 FIFO write yield 时
    //       forwarder 线程读取并修改 completed_task，导致 page_sz 等字段被破坏
    uint64_t  saved_pa          = completed_task->pa;
    uint64_t  saved_page_sz     = completed_task->page_sz;
    uint64_t  saved_gst_page_sz = completed_task->gst_page_sz;
    spte_t    saved_vs_pte      = completed_task->vs_pte;
    gpte_t    saved_g_pte       = completed_task->g_pte;
    uint32_t  saved_cause       = completed_task->cause;
    uint32_t  saved_task_id     = completed_task->task_id;

    va_dedup_key_t key;
    key.gscid = completed_task->GSCID;
    key.pscid = completed_task->PSCID;
    key.page_iova = completed_task->iova & ~0xFFFULL;

    va_dedup_mtx.lock();
    auto it = va_dedup_table.find(key);
    if (it == va_dedup_table.end() || !it->second.valid) {
        va_dedup_mtx.unlock();
        return;
    }
    std::vector<iommu_task_t*> pending = std::move(it->second.pending_tasks);
    va_dedup_table.erase(it);
    va_dedup_mtx.unlock();

    for (auto* ptask : pending) {
        if (is_fault) {
            ptask->cause = saved_cause;
            ptask->state = TASK_FAULT;
        } else {
            // 复制翻译结果，PA按各自的页内偏移计算
            uint64_t page_mask = saved_page_sz - 1;
            ptask->pa = (saved_pa & ~page_mask) | (ptask->iova & page_mask);
            ptask->vs_pte = saved_vs_pte;
            ptask->g_pte = saved_g_pte;
            ptask->page_sz = saved_page_sz;
            ptask->gst_page_sz = saved_gst_page_sz;
            ptask->state = TASK_PTW_DONE;
        }
        printf("[VA_DEDUP_RECOVER] task_id=%u, pa=0x%lx (from task_id=%u)\n",
               ptask->task_id, ptask->pa, saved_task_id);
        fflush(stdout);
        pt_cache_to_fwd_fifo.write(ptask);
    }
    if (!pending.empty()) {
        printf("[VA_DEDUP_RECOVER] Recovered %zu tasks for page 0x%lx\n",
               pending.size(), key.page_iova);
        fflush(stdout);
    }
}

/**********************************************/
// print_cache_statistics - 打印Cache命中率统计信息
/**********************************************/
void iommu_top::print_cache_statistics() {
    // [FIX] 等待 pt_update_worker_thread 处理完 FIFO 中所有剩余请求，修正仿真终止时的统计误差
    while (cache_sub.pt_update_fifo.num_available() > 0) {
        sc_core::wait(sc_core::SC_ZERO_TIME);
    }

    printf("\n========== Cache Hit/Miss Statistics (CacheSubsystem Internal) ==========\n");
    
    // 使用CacheSubsystem内部的StatsCollector
    cache_sub.stats().print_summary(std::cout);
    
    printf("========================================================================\n\n");

    // [STAT] PT Scheduler任务间隔分析
    cache_sub.print_pt_scheduler_gap_report();

    // VA Dedup Statistics
    printf("========== VA Dedup Statistics ==========\n");
    printf("  VA Dedup Enabled:  %s\n", PT_CACHE_VA_DEDUP_ENABLED ? "YES" : "NO");
    printf("  Dedup Table Miss:  %lu (sent to PTW)\n", (unsigned long)va_dedup_miss_count);
    printf("  Dedup Table Hit:   %lu (saved PTW tasks)\n", (unsigned long)va_dedup_hit_count);
    if (va_dedup_miss_count + va_dedup_hit_count > 0) {
        printf("  PTW Tasks Saved:   %.1f%% reduction\n",
               100.0 * va_dedup_hit_count / (va_dedup_miss_count + va_dedup_hit_count));
    }
    printf("==========================================\n\n");

    // Walker Cache Update Statistics
    printf("========== Walker Cache Update Statistics ==========\n");
    printf("  PTWC_1_2_3 updates:  %lu  (update c1+c2+c3)\n", 
           (unsigned long)cache_sub.walker_cache().get_update_ptwc_123_count());
    printf("  PTWC_2_3 updates:    %lu  (update c2+c3)\n",
           (unsigned long)cache_sub.walker_cache().get_update_ptwc_23_count());
    printf("  PTWC_3 updates:      %lu  (update c3 only)\n",
           (unsigned long)cache_sub.walker_cache().get_update_ptwc_3_count());
    printf("  NONE (skipped):      %lu  (redundant updates avoided)\n",
           (unsigned long)cache_sub.walker_cache().get_update_none_count());
    uint64_t total_updates = cache_sub.walker_cache().get_update_ptwc_123_count() +
                             cache_sub.walker_cache().get_update_ptwc_23_count() +
                             cache_sub.walker_cache().get_update_ptwc_3_count();
    uint64_t total_requests = total_updates + cache_sub.walker_cache().get_update_none_count();
    printf("  Total updates:       %lu\n", (unsigned long)total_updates);
    printf("  Total requests:      %lu\n", (unsigned long)total_requests);
    if (total_requests > 0) {
        printf("  Update distribution:\n");
        printf("    PTWC_1_2_3: %.1f%%\n", 
               100.0 * cache_sub.walker_cache().get_update_ptwc_123_count() / total_requests);
        printf("    PTWC_2_3:   %.1f%%\n",
               100.0 * cache_sub.walker_cache().get_update_ptwc_23_count() / total_requests);
        printf("    PTWC_3:     %.1f%%\n",
               100.0 * cache_sub.walker_cache().get_update_ptwc_3_count() / total_requests);
        printf("    NONE:       %.1f%%  (redundancy elimination rate)\n",
               100.0 * cache_sub.walker_cache().get_update_none_count() / total_requests);
    }
    printf("======================================================\n\n");

    // [S2] S2 Walker Cache Statistics
    printf("========== S2 Walker Cache Statistics ==========\n");
    printf("  S2 Cache Enabled:    %s\n", PTW_WALKER_S2_CACHE_ENABLED ? "YES" : "NO");
    {
        uint64_t s2_lookup = cache_sub.walker_cache().get_s2_lookup_count();
        uint64_t s2_hit_c3 = cache_sub.walker_cache().get_s2_hit_c3_count();
        uint64_t s2_hit_c2 = cache_sub.walker_cache().get_s2_hit_c2_count();
        uint64_t s2_hit_c1 = cache_sub.walker_cache().get_s2_hit_c1_count();
        uint64_t s2_miss   = cache_sub.walker_cache().get_s2_miss_count();
        uint64_t s2_total_hit = s2_hit_c3 + s2_hit_c2 + s2_hit_c1;
        printf("  S2 Lookups:          %lu\n", (unsigned long)s2_lookup);
        printf("  S2 Hits (total):     %lu\n", (unsigned long)s2_total_hit);
        printf("    C3 hit (1 DDR):    %lu\n", (unsigned long)s2_hit_c3);
        printf("    C2 hit (2 DDR):    %lu\n", (unsigned long)s2_hit_c2);
        printf("    C1 hit (3 DDR):    %lu\n", (unsigned long)s2_hit_c1);
        printf("  S2 Miss (4 DDR):     %lu\n", (unsigned long)s2_miss);
        if (s2_lookup > 0) {
            printf("  S2 Hit Rate:         %.1f%%\n", 100.0 * s2_total_hit / s2_lookup);
            printf("  S2 Hit Distribution:\n");
            printf("    C3: %.1f%%  C2: %.1f%%  C1: %.1f%%  Miss: %.1f%%\n",
                   100.0 * s2_hit_c3 / s2_lookup,
                   100.0 * s2_hit_c2 / s2_lookup,
                   100.0 * s2_hit_c1 / s2_lookup,
                   100.0 * s2_miss / s2_lookup);
            // DDR walk节省次数: 每次C3 hit省3次, C2 hit省2次, C1 hit省1次
            uint64_t ddr_saved = s2_hit_c3 * 3 + s2_hit_c2 * 2 + s2_hit_c1 * 1;
            printf("  DDR Walks Saved:     %lu  (vs baseline 4 reads/lookup)\n", (unsigned long)ddr_saved);
        }
    }
    printf("================================================\n\n");

    // IOMMU / PTW IOPS
    double sim_time_sec = sc_time_stamp().to_seconds();
    printf("========== IOMMU / PTW Throughput (IOPS) ==========\n");
    printf("  Sim Time:             %.3f us\n", sim_time_sec * 1e6);
    if (sim_time_sec > 0) {
        double iommu_iops_mps = (double)iommu_total_completed / sim_time_sec / 1e6;
        double ptw_iops_mps   = (double)ptw_total_completed   / sim_time_sec / 1e6;
        printf("  IOMMU Completed:      %lu trans\n",  (unsigned long)iommu_total_completed);
        printf("  IOMMU IOPS:           %.2f M trans/s\n", iommu_iops_mps);
        printf("  PTW  Completed:       %lu tasks\n",  (unsigned long)ptw_total_completed);
        printf("  PTW  IOPS:            %.2f M tasks/s\n", ptw_iops_mps);
        if (ptw_total_completed > 0) {
            printf("  PTW  avg DDR reads:   %.2f reads/task\n",
                   (double)ptw_total_ddr_reads / ptw_total_completed);
            printf("  PTW  avg exec lat:    %.1f ns  (%.3f us)\n",
                   ptw_total_exec_ns / ptw_total_completed,
                   ptw_total_exec_ns / ptw_total_completed / 1000.0);
            
            // [STAT] PTW详细DDR访问统计
            printf("  ---\n");
            printf("  PTW DDR access detail:\n");
            printf("    Max DDR reads/task:  %u\n", ptw_max_ddr_reads);
            printf("    Min DDR reads/task:  %u\n", ptw_min_ddr_reads == 999999 ? 0 : ptw_min_ddr_reads);
            printf("    Avg DDR latency:     %.1f ns\n",
                   ptw_ddr_latency_count > 0 ? ptw_total_ddr_latency_ns / ptw_ddr_latency_count : 0);
            printf("    Max DDR latency:     %.1f ns\n", ptw_max_ddr_latency_ns);
            printf("    Min DDR latency:     %.1f ns\n", ptw_min_ddr_latency_ns == 999999999.0 ? 0 : ptw_min_ddr_latency_ns);
            printf("  ---\n");
            printf("  PTW task latency:\n");
            printf("    Max task latency:    %.1f ns  (%.3f us)\n",
                   ptw_max_task_latency_ns, ptw_max_task_latency_ns / 1000.0);
            printf("    Min task latency:    %.1f ns  (%.3f us)\n",
                   ptw_min_task_latency_ns == 999999999.0 ? 0 : ptw_min_task_latency_ns,
                   ptw_min_task_latency_ns == 999999999.0 ? 0 : ptw_min_task_latency_ns / 1000.0);
        }
        if (iommu_total_completed > 0) {
            double avg_e2e_ns = iommu_total_e2e_latency_ns / iommu_total_completed;
            double avg_req_ns = sim_time_sec * 1e9 / iommu_total_completed;
            printf("  IO   avg e2e lat:     %.1f ns  (%.3f us)\n", avg_e2e_ns, avg_e2e_ns/1000.0);
            printf("  IO   avg req time:    %.1f ns  (%.3f us)\n", avg_req_ns, avg_req_ns/1000.0);
        }
        // 稳态IOPS（跳过warmup/drain，只取中间稳定段）
        if (steady_end_ns > steady_start_ns && steady_end_count > steady_start_count) {
            double steady_duration_ns = steady_end_ns - steady_start_ns;
            uint64_t steady_trans = steady_end_count - steady_start_count;
            double steady_iops = (double)steady_trans / steady_duration_ns * 1e3; // M trans/s
            double theory_iops = (double)AXI_SLAVE_0_BANDWIDTH_MBPS / 8.0 / 512.0; // Mbps / 8 / 512B = M/s
            printf("  ---\n");
            printf("  Steady IOPS:          %.2f M trans/s  (window: #%lu ~ #%lu, %.1f ns)\n",
                   steady_iops,
                   (unsigned long)steady_start_count, (unsigned long)steady_end_count,
                   steady_duration_ns);
            printf("  Theory peak:          %.2f M trans/s  (%.0f GB/s / 512B)\n",
                   theory_iops, AXI_SLAVE_0_BANDWIDTH_MBPS / 8000.0);
            printf("  Efficiency:           %.1f%%\n", steady_iops / theory_iops * 100.0);
        }
    }
    printf("====================================================\n\n");

    // Port Average Bandwidth
    printf("========== Port Average Bandwidth ==========\n");
    if (sim_time_sec > 0) {
        // peak BW (GB/s): BANDWIDTH_MBPS is in Mbps = 1e6 bit/s; GB/s = MBPS*1e6/8/1e9 = MBPS/8000
        const double peak_slave0_GBs  = AXI_SLAVE_0_BANDWIDTH_MBPS  / 8000.0;
        const double peak_master0_GBs = AXI_MASTER_0_BANDWIDTH_MBPS / 8000.0;
        const double peak_master1_GBs = AXI_MASTER_1_BANDWIDTH_MBPS / 8000.0;
        double slave0_bw_GBs  = (double)slave_0_total_bytes  / sim_time_sec / 1e9;
        double master0_bw_GBs = (double)master_0_total_bytes / sim_time_sec / 1e9;
        double master1_bw_GBs = (double)master_1_total_bytes / sim_time_sec / 1e9;
        printf("  slave_0  (入口): %10lu B  avg %.3f GB/s  (peak %.0f GB/s, util %.2f%%)\n",
               (unsigned long)slave_0_total_bytes,  slave0_bw_GBs,  peak_slave0_GBs,
               slave0_bw_GBs  / peak_slave0_GBs  * 100.0);
        printf("  master_0 (出口): %10lu B  avg %.3f GB/s  (peak %.0f GB/s, util %.2f%%)\n",
               (unsigned long)master_0_total_bytes, master0_bw_GBs, peak_master0_GBs,
               master0_bw_GBs / peak_master0_GBs * 100.0);
        printf("  master_1 (DDR):  %10lu B  avg %.3f GB/s  (peak %.0f GB/s, util %.2f%%)\n",
               (unsigned long)master_1_total_bytes, master1_bw_GBs, peak_master1_GBs,
               master1_bw_GBs / peak_master1_GBs * 100.0);
    }
    printf("============================================\n\n");

    // Outstanding Peak Statistics
    printf("========== Outstanding Peak Statistics ==========\n");
    printf("  %-30s peak=%4d / max=%4d\n", "IOMMU Global:",
           peak_iommu_global_outstanding,  (int)IOMMU_GLOBAL_MAX_OUTSTANDING);
    printf("  %-30s peak=%4d / max=%4d\n", "PTW:",
           peak_ptw_outstanding,            (int)PTW_MAX_OUTSTANDING_TASKS);
    printf("  %-30s peak=%4d / max=%4d\n", "xDTW DC walk:",
           peak_xdtw_dc_outstanding,        (int)XDTW_MAX_DC_OUTSTANDING_TASKS);
    printf("  %-30s peak=%4d / max=%4d\n", "xDTW PC walk:",
           peak_xdtw_pc_outstanding,        (int)XDTW_MAX_PC_OUTSTANDING_TASKS);
    printf("  %-30s peak=%4d / max=%4d\n", "Collector DC walk:",
           peak_collector_dc_walk_outstanding, (int)COLLECTOR_MAX_DC_WALK_OUTSTANDING);
    printf("  %-30s peak=%4d / max=%4d\n", "Collector PC walk:",
           peak_collector_pc_walk_outstanding, (int)COLLECTOR_MAX_PC_WALK_OUTSTANDING);
    printf("  %-30s peak=%4d / max=%4d\n", "DDR (master_1):",
           peak_axi_master_1_outstanding,   (int)AXI_MASTER_1_TO_CMN_RND_MAX_OUTSTANDING);
    printf("  %-30s peak=%4d / max=%4d\n", "Output (master_0):",
           peak_axi_master_0_outstanding,   (int)AXI_MASTER_0_TO_PCIE_NOC_MAX_OUTSTANDING);
    printf("=================================================\n\n");

    // Serial Cache Pipeline Busy-Time Analysis [DISABLED - causes segfault after sc_stop]
    /*
    printf("========== Serial Cache Pipeline Analysis ==========\n");
    double sim_ns = sim_time_sec * 1e9;
    if (sim_ns > 0) {
        // DC Cache (CacheSubsystem internal)
        const auto& dc_stats = cache_sub.stats().get_stats("dc_cache");
        double dc_busy_ns = dc_stats.total_execution_latency_ns;
        printf("  DC Cache (sub):     %lu reqs  exec_busy=%.0f ns (%.3f us)  util=%.1f%%\n",
               (unsigned long)dc_stats.request_latency_samples,
               dc_busy_ns, dc_busy_ns/1000.0,
               dc_busy_ns / sim_ns * 100.0);
        if (dc_stats.request_latency_samples > 0) {
            printf("    avg exec=%.1f ns  avg total=%.1f ns\n",
                   dc_stats.avg_execution_latency_ns(),
                   dc_stats.avg_request_latency_ns());
        }

        // PC Cache (CacheSubsystem internal)
        const auto& pc_stats = cache_sub.stats().get_stats("pc_cache");
        double pc_busy_ns = pc_stats.total_execution_latency_ns;
        printf("  PC Cache (sub):     %lu reqs  exec_busy=%.0f ns (%.3f us)  util=%.1f%%\n",
               (unsigned long)pc_stats.request_latency_samples,
               pc_busy_ns, pc_busy_ns/1000.0,
               pc_busy_ns / sim_ns * 100.0);
        if (pc_stats.request_latency_samples > 0) {
            printf("    avg exec=%.1f ns  avg total=%.1f ns\n",
                   pc_stats.avg_execution_latency_ns(),
                   pc_stats.avg_request_latency_ns());
        }

        // PT Cache (CacheSubsystem internal)
        const auto& pt_stats = cache_sub.stats().get_stats("pt_cache");
        double pt_busy_ns = pt_stats.total_execution_latency_ns;
        printf("  PT Cache (sub):     %lu reqs  exec_busy=%.0f ns (%.3f us)  util=%.1f%%\n",
               (unsigned long)pt_stats.request_latency_samples,
               pt_busy_ns, pt_busy_ns/1000.0,
               pt_busy_ns / sim_ns * 100.0);
        if (pt_stats.request_latency_samples > 0) {
            printf("    avg exec=%.1f ns  avg total=%.1f ns\n",
                   pt_stats.avg_execution_latency_ns(),
                   pt_stats.avg_request_latency_ns());
        }

        // Walker Cache (CacheSubsystem internal)
        const auto& wk_stats = cache_sub.stats().get_stats("walker_cache");
        double wk_busy_ns = wk_stats.total_execution_latency_ns;
        printf("  Walker Cache (sub): %lu reqs  exec_busy=%.0f ns (%.3f us)  util=%.1f%%\n",
               (unsigned long)wk_stats.request_latency_samples,
               wk_busy_ns, wk_busy_ns/1000.0,
               wk_busy_ns / sim_ns * 100.0);
        if (wk_stats.request_latency_samples > 0) {
            printf("    avg exec=%.1f ns  avg total=%.1f ns\n",
                   wk_stats.avg_execution_latency_ns(),
                   wk_stats.avg_request_latency_ns());
        }

        // Pipeline drain analysis
        double input_time_ns = (double)iommu_total_completed * 8.0; // 8ns per 512B @ 64GB/s
        double drain_ns = sim_ns - input_time_ns;
        printf("  ---\n");
        printf("  Input active:       %.0f ns (%.3f us)\n", input_time_ns, input_time_ns/1000.0);
        printf("  Pipeline drain:     %.0f ns (%.3f us)\n", drain_ns, drain_ns/1000.0);
        printf("  Drain / SimTime:    %.1f%%\n", drain_ns / sim_ns * 100.0);
    }
    printf("===================================================\n\n");
    */
    fflush(stdout);
}
