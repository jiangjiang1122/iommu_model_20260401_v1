#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>
#include <iostream>

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

        printf("[IOMMU_TOP] axi_slave_nb_transport_fw: task_id=%u, device_id=0x%x, iova=0x%lx, at=%d -> inbound_fifo\n",
               task->task_id, task->device_id, task->iova, task->at);
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

            // Increment outstanding counter after successful send
            axi_master_1_to_cmn_rnd_outstanding++;
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

    printf("[IOMMU_TOP] send_response_to_initiator: task_id=%u, pa=0x%lx, state=%d\n",
           task->task_id, task->pa, task->state);
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

    // Save original task to pending map for PT cache response correlation
    pt_cache_mtx.lock();
    pt_cache_pending_tasks[task->task_id] = task;
    pt_cache_mtx.unlock();

    // Convert task to CacheMessage and write to cache_sub.pt_request_fifo
    iommu::CacheMessage pt_req = task_to_pt_request(task);
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
// print_cache_statistics - 打印Cache命中率统计信息
/**********************************************/
void iommu_top::print_cache_statistics() {
    printf("\n========== Cache Hit/Miss Statistics (CacheSubsystem Internal) ==========\n");
    
    // 使用CacheSubsystem内部的StatsCollector
    cache_sub.stats().print_summary(std::cout);
    
    printf("========================================================================\n\n");
    fflush(stdout);
}
