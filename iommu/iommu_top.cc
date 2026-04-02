#include "iommu_top.hh"

/**********************************************/
//
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
                           
    // 将IOMMU模式设置为DDB_Bare，允许简单的地址直通
    iommu_inst.reg_file.ddtp.iommu_mode = DDT_1LVL;
}

/**********************************************/
//
/**********************************************/
void iommu_top::ahb_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay)
{
    tlm::tlm_command cmd = trans.get_command();
    sc_dt::uint64 addr = trans.get_address();
    unsigned char *data = trans.get_data_ptr();
    unsigned int len = trans.get_data_length();
    //unsigned char *byt = trans.get_byte_enable_ptr();
    //unsigned int wid = trans.get_streaming_width();

    if (cmd == tlm::TLM_READ_COMMAND)
    {
        uint16_t offset = addr - IOMMU_BASE_ADDR; // TODO
        uint8_t num_bytes = 4;

        uint64_t reg_value = read_register(&iommu_inst, offset, num_bytes);
        memcpy(data, (unsigned char *)&reg_value, len);

        printf("%s:%d Read addr 0x%04llx, val 0x%08x\n", __func__, __LINE__,
               addr, *(unsigned int *)data);
    }
    else if (cmd == tlm::TLM_WRITE_COMMAND)
    {
        uint64_t val = 0;
        memcpy(&val, data, len);
        //uint32_t byte_en = 0xF >> (4 - len);
        uint16_t offset = addr - IOMMU_BASE_ADDR;
        uint8_t num_bytes = 4;

        write_register(&iommu_inst, offset, num_bytes, val);
    }
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

/**********************************************/
//
/**********************************************/
void iommu_top::axi_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay)
{
    tlm::tlm_command cmd = trans.get_command();
    sc_dt::uint64 addr = trans.get_address();
    unsigned char *data = trans.get_data_ptr();
    unsigned int len = trans.get_data_length();
    unsigned char *byt = trans.get_byte_enable_ptr();
    unsigned int wid = trans.get_streaming_width();

    PayloadExtention *ext = nullptr;
    trans.get_extension(ext);
    assert(ext != nullptr);

    printf("[IOMMU] AXI slave received transaction - Address: 0x%lx, Command: %s\n", 
           addr, (cmd == tlm::TLM_READ_COMMAND) ? "READ" : "WRITE");

    printf("[IOMMU] Extension info - requester_id: 0x%x, at: %d, pid_valid: %d\n",
           ext->requester_id, ext->at, ext->pid_valid);

    // 1. Judge req is PRI or not
    if(ext->msg_type == 0b0010) // message req
    {
        printf("[IOMMU] Processing message request - msg_code: 0x%x\n", ext->msg_code);
        
        if(ext->msg_code == 0b0001101) //PAGE_REQUEST or STOP Maker
        {
            printf("[IOMMU] Processing page request\n");
            ats_msg_t pr;
            pr.MSGCODE = ext->msg_code;
            pr.TAG = ext->tag;
            pr.RID = ext->requester_id;
            pr.PV = ext->pid_valid;
            pr.PID = ext->process_id;
            pr.PRIV = ext->priv_req;
            pr.EXEC_REQ = ext->exec_req;
            pr.DSV = 0;
            pr.DSEG = 0;
            memcpy(data, (unsigned char *)&pr.PAYLOAD, len);
            handle_page_request(&iommu_inst,&pr);
        }
    }
    else
    {
        printf("[IOMMU] Processing IOVA translation request\n");
        
        // translate iova
        hb_to_iommu_req_t req;
        iommu_to_hb_rsp_t rsp;

        req.device_id = ext->requester_id;
        req.pid_valid = ext->pid_valid;
        req.process_id = ext->process_id;
        req.exec_req = ext->exec_req;
        req.priv_req = ext->priv_req;
        req.is_cxl_dev = 0;
        if(ext->at == 0){req.tr.at = ADDR_TYPE_UNTRANSLATED;}  // Fixed assignment operator
        if(ext->at == 1){req.tr.at = ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST;}  // Fixed assignment operator
        if(ext->at == 2){req.tr.at = ADDR_TYPE_TRANSLATED;}  // Fixed assignment operator
        req.tr.iova = addr;
        req.tr.length = len;//byte
        req.tr.read_writeAMO = (cmd == tlm::TLM_READ_COMMAND)?1:0;
        iommu_inst.trans_for_debug = 1;

        printf("[IOMMU] Calling iommu_translate_iova with IOVA: 0x%lx, device_id: 0x%x\n", 
               req.tr.iova, req.device_id);
        
        iommu_translate_iova(&iommu_inst, &req, &rsp);

        printf("[IOMMU] iommu_translate_iova completed with status: %d\n", rsp.status);

        if(rsp.status == SUCCESS)
        {
            printf("[IOMMU] Translation successful - PPN: 0x%lx, S: %d\n", 
                   rsp.trsp.PPN, rsp.trsp.S);
            
            if((rsp.trsp.is_msi==1) && (rsp.trsp.is_mrif==0))
            {
                printf("[IOMMU] MSI translation detected, forwarding to stream socket\n");
                trans.set_address(rsp.trsp.dest_mrif_addr);
                axi_stream_to_cmn_rnd_socket->b_transport(trans,delay);
            }
            else
            {
                printf("[IOMMU] Regular translation, calculating physical address\n");
                printf("[IOMMU] Debug info - PPN: 0x%lx, S bit: %d, page_offset: 0x%lx\n", 
                       rsp.trsp.PPN, rsp.trsp.S, trans.get_address() & 0xFFF);
                
                // Calculate physical address from PPN and page size
                // In Bare mode, PPN is already the direct page number, not NAPOT format
                uint64_t page_size;
                if (rsp.trsp.S == 1) {
                    // Large page (2MB)
                    page_size = 0x200000;
                } else {
                    // Small page (4KB)
                    page_size = 0x1000;
                }
                
                // For Bare mode translation, use the PPN directly
                // For page-based translations with NAPOT format, convert NAPOT PPN back to regular PPN
                uint64_t actual_ppn;
                if (page_size >= 0x40000000) {  // Bare mode
                    actual_ppn = rsp.trsp.PPN;
                } else {
                    // NAPOT format: extract base PPN by clearing the NAPOT bits
                    // NAPOT PPN has form: base_PPN | ((page_sz/2/PAGESIZE) - 1)
                    uint64_t napot_mask = (page_size/2/PAGESIZE) - 1;
                    actual_ppn = rsp.trsp.PPN & ~napot_mask;
                }
                
                uint64_t calculated_pa = (actual_ppn << 12) | (trans.get_address() & 0xFFF);
                
                printf("[IOMMU] Using page_size: 0x%lx, Calculated physical address: 0x%lx\n", 
                       page_size, calculated_pa);
                printf("[IOMMU] Debug info - PPN: 0x%lx, S bit: %d, page_offset: 0x%lx\n",
                       rsp.trsp.PPN, rsp.trsp.S, trans.get_address() & 0xFFF);
                
                trans.set_address(calculated_pa);
                axi_master_0_to_pcie_noc_socket->b_transport(trans,delay);
            }
        }
        else
        {
            printf("[IOMMU] Translation failed with status: %d\n", rsp.status);
        }
    }
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

/**********************************************/
//
/**********************************************/
void iommu_top::CQ_Monitor_Process_Thread(){
    while(true)
    {
        wait(cq_process_evt);

        //process_commands(&iommu_inst);
    }
}