#include "iommu_top.hh"
#include <cstring>
#include <cstdio>

using namespace std;

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

// ============================================================================
// Socket 回调实现
// ============================================================================

tlm::tlm_sync_enum iommu_top::axi_slave_nb_transport(
    tlm::tlm_generic_payload& trans,
    tlm::tlm_phase& phase,
    sc_time& delay
) {
    printf("[AXI_NB_TRANSPORT] AXI slave transport called, phase: %d\n", (int)phase);
    fflush(stdout);
    
    if (phase == tlm::BEGIN_REQ) {
        tlm::tlm_generic_payload* payload_ptr = &trans;
        
        printf("[AXI_NB_TRANSPORT] BEGIN_REQ phase, IOVA: 0x%lx\n", trans.get_address());
        fflush(stdout);
        
        if (inbound_fifo.num_free() > 0) {
            inbound_fifo.write(payload_ptr);
            phase = tlm::END_REQ;
            delay = sc_time(PARSER_DELAY, SC_NS);
            
            printf("[AXI_NB_TRANSPORT] Payload written to inbound_fifo, delay: %ld ns\n", PARSER_DELAY);
            fflush(stdout);
            
            return tlm::TLM_ACCEPTED;
        } else {
            delay = sc_time(10, SC_NS);
            printf("[AXI_NB_TRANSPORT] inbound_fifo full, returning TLM_UPDATED\n");
            fflush(stdout);
            return tlm::TLM_UPDATED;
        }
    }
    printf("[AXI_NB_TRANSPORT] Non-BEGIN_REQ phase, returning TLM_ACCEPTED\n");
    fflush(stdout);
    return tlm::TLM_ACCEPTED;
}

void iommu_top::ahb_slave_b_transport(tlm::tlm_generic_payload& trans, sc_time& delay) {
    tlm::tlm_command cmd = trans.get_command();
    sc_dt::uint64 addr = trans.get_address();
    unsigned char *data = trans.get_data_ptr();
    unsigned int len = trans.get_data_length();

    if (cmd == tlm::TLM_READ_COMMAND) {
        uint16_t offset = addr - IOMMU_BASE_ADDR;
        uint8_t num_bytes = 4;
        uint64_t reg_value = read_register(&iommu_inst, offset, num_bytes);
        memcpy(data, (unsigned char *)&reg_value, len);
    } else if (cmd == tlm::TLM_WRITE_COMMAND) {
        uint64_t val = 0;
        memcpy(&val, data, len);
        uint16_t offset = addr - IOMMU_BASE_ADDR;
        uint8_t num_bytes = 4;
        write_register(&iommu_inst, offset, num_bytes, val);
    }
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
    delay = sc_time(100, SC_NS);
}

tlm::tlm_sync_enum iommu_top::ddr_nb_transport_bw(
    tlm::tlm_generic_payload& trans,
    tlm::tlm_phase& phase,
    sc_time& delay
) {
    if (phase == tlm::BEGIN_RESP) {
        PayloadExtention* ext = nullptr;
        trans.get_extension(ext);
        if (!ext) {
            return tlm::TLM_COMPLETED;
        }

        uint16_t axi_id = ext->axi_id;
        walk_type_t walk_type = (walk_type_t)ext->walk_type;

        auto it = ddr_outstanding_table.find(axi_id);
        if (it == ddr_outstanding_table.end()) {
            return tlm::TLM_COMPLETED;
        }

        ddr_outstanding_entry_t entry = it->second;
        iommu_task_t* task = entry.task;

        ddr_response_t rsp;
        rsp.axi_id = axi_id;
        rsp.walk_type = walk_type;
        rsp.task_id = task->task_id;
        rsp.status = (trans.is_response_ok()) ? 0 : 1;
        rsp.data_size = entry.expected_size;
        memcpy(rsp.data, trans.get_data_ptr(), min((uint32_t)64, (uint32_t)entry.expected_size));

        switch (walk_type) {
            case WALK_DDT:
            case WALK_PDT:
                xdtw_rsp_queue.push(rsp);
                xdtw_rsp_evt.notify();
                break;
            case WALK_VS_PT:
            case WALK_G_PT:
            case WALK_G_PT_IMPLICIT:
                ptw_rsp_queue.push(rsp);
                ptw_rsp_evt.notify();
                break;
            case WALK_MSI_PT:
                msiptw_rsp_queue.push(rsp);
                msiptw_rsp_evt.notify();
                break;
            default:
                break;
        }

        axi_id_alloc.free_id(axi_id);
        ddr_outstanding_table.erase(it);

        phase = tlm::END_RESP;
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

// ============================================================================
// DDR 访问辅助函数
// ============================================================================

void iommu_top::send_ddr_nb_read(uint64_t addr, uint8_t size, uint16_t axi_id, walk_type_t type, iommu_task_t* task) {
    tlm::tlm_generic_payload* trans = new tlm::tlm_generic_payload;
    trans->set_command(TLM_READ_COMMAND);
    trans->set_address(addr);
    trans->set_data_length(size);
    trans->set_streaming_width(size);
    trans->set_data_ptr(new unsigned char[size]);
    trans->set_byte_enable_ptr(0);
    trans->set_dmi_allowed(false);
    trans->set_response_status(TLM_INCOMPLETE_RESPONSE);

    PayloadExtention* ext = new PayloadExtention();
    ext->axi_id = axi_id;
    ext->walk_type = type;
    trans->set_extension(ext);

    ddr_outstanding_entry_t entry;
    entry.task = task;
    entry.walk_type = type;
    entry.expected_addr = addr;
    entry.expected_size = size;
    ddr_outstanding_table[axi_id] = entry;

    tlm::tlm_phase phase = tlm::BEGIN_REQ;
    sc_time delay = SC_ZERO_TIME;
    axi_master_1_to_cmn_rnd_socket->nb_transport_fw(*trans, phase, delay);
}

void iommu_top::send_ddr_blocking_read(uint64_t addr, uint8_t size, char* data) {
    tlm::tlm_generic_payload* trans = new tlm::tlm_generic_payload;
    trans->set_command(TLM_READ_COMMAND);
    trans->set_address(addr);
    trans->set_data_length(size);
    trans->set_data_ptr((unsigned char*)data);
    
    sc_time delay = sc_time(DDR_READ_LATENCY, SC_NS);
    axi_master_1_to_cmn_rnd_socket->b_transport(*trans, delay);
    delete trans;
}

void iommu_top::send_ddr_blocking_write(uint64_t addr, uint8_t size, char* data) {
    tlm::tlm_generic_payload* trans = new tlm::tlm_generic_payload;
    trans->set_command(TLM_WRITE_COMMAND);
    trans->set_address(addr);
    trans->set_data_length(size);
    trans->set_data_ptr((unsigned char*)data);
    
    sc_time delay = sc_time(DDR_WRITE_LATENCY, SC_NS);
    axi_master_1_to_cmn_rnd_socket->b_transport(*trans, delay);
    delete trans;
}

void iommu_top::send_ddr_blocking_atomic_or(uint64_t addr, uint64_t or_value, uint8_t size) {
    char read_data[8] = {0};
    send_ddr_blocking_read(addr, size, read_data);
    uint64_t* ptr = (uint64_t*)read_data;
    *ptr |= or_value;
    send_ddr_blocking_write(addr, size, read_data);
}

// ============================================================================
// Cache 辅助函数
// ============================================================================

uint8_t iommu_top::lookup_dc_cache(uint32_t device_id, device_context_t* DC) {
    return lookup_ioatc_dc(&iommu_inst, device_id, DC);
}

void iommu_top::update_dc_cache(uint32_t device_id, device_context_t* DC) {
    cache_ioatc_dc(&iommu_inst, device_id, DC);
}

uint8_t iommu_top::lookup_pc_cache(uint32_t device_id, uint32_t process_id, process_context_t* PC) {
    return lookup_ioatc_pc(&iommu_inst, device_id, process_id, PC);
}

void iommu_top::update_pc_cache(uint32_t device_id, uint32_t process_id, process_context_t* PC) {
    cache_ioatc_pc(&iommu_inst, device_id, process_id, PC);
}

uint8_t iommu_top::lookup_iotlb(uint64_t iova, uint8_t PSCV, uint32_t PSCID, uint8_t GV, uint16_t GSCID,
                                uint8_t priv, uint8_t is_read, uint8_t is_write, uint8_t is_exec, uint8_t SUM,
                                uint32_t* cause, uint64_t* pa, uint64_t* page_sz, spte_t* vs_pte, gpte_t* g_pte) {
    uint8_t is_msi;
    return lookup_ioatc_iotlb(&iommu_inst, iova, 0, priv, is_read, is_write, is_exec, SUM, PSCV, PSCID, GV, GSCID,
                              cause, pa, page_sz, vs_pte, g_pte, &is_msi);
}

void iommu_top::update_iotlb(uint64_t iova, spte_t vs_pte, gpte_t g_pte, uint64_t pa, uint64_t page_sz,
                             uint8_t PSCV, uint32_t PSCID, uint8_t GV, uint16_t GSCID) {
    uint64_t vpn = iova >> 12;
    cache_ioatc_iotlb(&iommu_inst, vpn, GV, PSCV, GSCID, PSCID, &vs_pte, &g_pte, pa >> 12, (page_sz > 4096) ? 1 : 0, 0);
}

uint8_t iommu_top::lookup_msipt_cache(uint64_t msiptp_ppn, uint32_t interrupt_file_num, uint64_t* msipte_data) {
    return 0;
}

void iommu_top::update_msipt_cache(uint64_t msiptp_ppn, uint32_t interrupt_file_num, uint64_t msipte_data) {
}

// 空实现性能模型线程函数 - 保持链接兼容性
void iommu_top::parser_thread() {
    // 在功能模型中，这个线程不会被激活，所以只需一个空实现
    wait(SC_ZERO_TIME); // 避免无限循环
}

void iommu_top::collector_cache_lookup_result_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::collector_xdtw_response_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::dc_cache_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::pc_cache_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::pt_cache_query_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::pt_cache_result_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::pt_cache_ptw_rsp_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::msipt_cache_query_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::msipt_cache_result_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::msiptw_req_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::msiptw_rsp_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::xdtw_req_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::xdtw_rsp_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::ptw_req_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::ptw_rsp_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::ddr_rsp_router_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::forwarder_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::msi_forwarder_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::fault_proc_thread() {
    wait(SC_ZERO_TIME);
}

void iommu_top::cq_proc_thread() {
    wait(SC_ZERO_TIME);
}
