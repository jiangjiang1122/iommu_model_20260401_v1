#include "test_ddr.hh"
#include <cstdio>
#include <cstring>

// ============================================================
// Constructor
// ============================================================
DDR_Module::DDR_Module(sc_module_name name) : sc_module(name),
    ddr_req_fifo("ddr_req_fifo", 512),
    ddr_peq("ddr_peq"),
    read_outstanding(0),
    write_outstanding(0)
{
    // Dynamically allocate DDR memory (avoids large object size issues with SystemC)
    memory = new unsigned char[DDR_MEMORY_SIZE];
    memset(memory, 0, DDR_MEMORY_SIZE);
    printf("[DDR] Allocated %zu MB DDR memory\n", DDR_MEMORY_SIZE / (1024*1024));

    // Register nb_transport_fw for the main DDR socket (from IOMMU arbiter)
    axi_slave_from_cmn_rnd_1_socket.register_nb_transport_fw(
        this, &DDR_Module::nb_transport_fw);

    // Keep b_transport for other sockets (used by RP direct memory access)
    axi_slave_from_cmn_rnd_socket.register_b_transport(this, &DDR_Module::b_transport);
    axi_slave_from_pcie_noc_0_socket.register_b_transport(this, &DDR_Module::b_transport);

    SC_THREAD(ddr_dispatch_thread);
    SC_THREAD(ddr_response_thread);
}

// ============================================================
// nb_transport_fw - AT mode non-blocking forward path
// ============================================================
tlm::tlm_sync_enum DDR_Module::nb_transport_fw(
    tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_REQ) {
        ddr_req_fifo.nb_write(&trans);
        phase = tlm::END_REQ;
        return tlm::TLM_UPDATED;
    }
    else if (phase == tlm::END_RESP) {
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

// ============================================================
// b_transport - blocking transport for RP direct memory access
// ============================================================
void DDR_Module::b_transport(tlm_generic_payload& trans, sc_time& delay) {
    process_memory_access(trans);
}

// ============================================================
// ddr_dispatch_thread - outstanding control + PEQ scheduling
// ============================================================
void DDR_Module::ddr_dispatch_thread() {
    while (true) {
        tlm_generic_payload* trans = ddr_req_fifo.read();

        sc_time arrive_time = sc_time_stamp();

        if (trans->get_command() == tlm::TLM_READ_COMMAND) {
            while (read_outstanding >= DDR_READ_MAX_OUTSTANDING) {
                wait(read_slot_freed);
            }
            read_outstanding++;
        } else {
            while (write_outstanding >= DDR_WRITE_MAX_OUTSTANDING) {
                wait(write_slot_freed);
            }
            write_outstanding++;
        }

        sc_time dispatch_time = sc_time_stamp();
        sc_time queue_delay = dispatch_time - arrive_time;

        const char* cmd_str = (trans->get_command() == tlm::TLM_READ_COMMAND) ? "READ" : "WRITE";
        printf("[DDR_DISPATCH] %s addr=0x%lx, arrive=%s, dispatch=%s, queue_wait=%s, "
               "rd_out=%d, wr_out=%d\n",
               cmd_str, (unsigned long)trans->get_address(),
               arrive_time.to_string().c_str(),
               dispatch_time.to_string().c_str(),
               queue_delay.to_string().c_str(),
               read_outstanding, write_outstanding);
        fflush(stdout);

        // Schedule into PEQ: response fires at dispatch_time + 100ns
        ddr_peq.notify(*trans, sc_time(DDR_INTERNAL_LATENCY_NS,SC_NS));
    }
}

// ============================================================
// ddr_response_thread - PEQ fires after latency, preserves order
// ============================================================
void DDR_Module::ddr_response_thread() {
    while (true) {
        tlm_generic_payload* trans = ddr_peq.get_next_transaction();
        if (!trans) { wait(ddr_peq.get_event()); continue; }

        sc_time resp_time = sc_time_stamp();

        // Execute memory access
        process_memory_access(*trans);

        const char* cmd_str = (trans->get_command() == tlm::TLM_READ_COMMAND) ? "READ" : "WRITE";
        printf("[DDR_RESP] %s addr=0x%lx, resp_time=%s\n",
               cmd_str, (unsigned long)trans->get_address(),
               resp_time.to_string().c_str());
        fflush(stdout);

        // Save command before nb_transport_bw (callback may free trans)
        tlm::tlm_command saved_cmd = trans->get_command();

        // Return response via nb_transport_bw
        tlm::tlm_phase phase = tlm::BEGIN_RESP;
        sc_time delay = SC_ZERO_TIME;
        axi_slave_from_cmn_rnd_1_socket->nb_transport_bw(*trans, phase, delay);

        // Release outstanding slot (use saved_cmd since trans may be freed)
        if (saved_cmd == tlm::TLM_READ_COMMAND) {
            read_outstanding--;
            read_slot_freed.notify(SC_ZERO_TIME);
        } else {
            write_outstanding--;
            write_slot_freed.notify(SC_ZERO_TIME);
        }
    }
}

// ============================================================
// process_memory_access - shared memory read/write logic
// ============================================================
void DDR_Module::process_memory_access(tlm_generic_payload& trans) {
    tlm_command cmd = trans.get_command();
    sc_dt::uint64 addr = trans.get_address();
    unsigned char* data = trans.get_data_ptr();
    unsigned int len = trans.get_data_length();

    if (addr + len > DDR_MEMORY_SIZE) {
        printf("[DDR] ERROR: Address 0x%lx + len %d exceeds memory size %zu\n",
               (unsigned long)addr, len, DDR_MEMORY_SIZE);
        trans.set_response_status(TLM_ADDRESS_ERROR_RESPONSE);
        return;
    }

    if (cmd == TLM_READ_COMMAND) {
        memcpy(data, &memory[addr], len);
    } else if (cmd == TLM_WRITE_COMMAND) {
        memcpy(&memory[addr], data, len);
    }

    trans.set_response_status(TLM_OK_RESPONSE);
}
