#include "test_slink.hh"

// ============================================================
// Constructor
// ============================================================
SLINK_Module::SLINK_Module(sc_module_name name) : sc_module(name),
    req_fifo("slink_req_fifo", 256),
    rsp_fifo("slink_rsp_fifo", 256),
    fwd_peq("slink_fwd_peq"),
    bwd_peq("slink_bwd_peq")
{
    // Register nb_transport_fw on target socket (receives from IOMMU)
    targ_socket.register_nb_transport_fw(this, &SLINK_Module::nb_transport_fw);

    // Register nb_transport_bw on initiator socket (receives from DDR)
    init_socket.register_nb_transport_bw(this, &SLINK_Module::nb_transport_bw);

    // Processing threads
    SC_THREAD(slink_fwd_thread);
    SC_THREAD(slink_fwd_output_thread);
    SC_THREAD(slink_bwd_thread);
    SC_THREAD(slink_bwd_output_thread);

    printf("[SLINK] Module created: NoC pipeline latency=%d ns (PEQ), req_fifo_depth=256\n",
           SLINK_NOC_LATENCY_NS);
}

// ============================================================
// nb_transport_fw callback: receive request from IOMMU
// Pushes to req_fifo, returns END_REQ immediately
// ============================================================
tlm::tlm_sync_enum SLINK_Module::nb_transport_fw(
    tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_REQ) {
        req_fifo.nb_write(&trans);
        phase = tlm::END_REQ;
        return tlm::TLM_UPDATED;
    }
    else if (phase == tlm::END_RESP) {
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

// ============================================================
// nb_transport_bw callback: receive response from DDR
// Pushes to rsp_fifo, returns END_RESP immediately
// (runs in DDR's ddr_response_thread context - SC_THREAD, blocking OK)
// ============================================================
tlm::tlm_sync_enum SLINK_Module::nb_transport_bw(
    tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_RESP) {
        rsp_fifo.write(&trans);
        phase = tlm::END_RESP;
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

// ============================================================
// Forward path dispatch thread:
//   req_fifo -> schedule into fwd_peq with NoC pipeline delay
// ============================================================
void SLINK_Module::slink_fwd_thread() {
    while (true) {
        tlm_generic_payload* trans = req_fifo.read();
        // Schedule into PEQ: fires after SLINK_NOC_LATENCY_NS
        fwd_peq.notify(*trans, sc_time(SLINK_NOC_LATENCY_NS, SC_NS));
    }
}

// ============================================================
// Forward path output thread:
//   fwd_peq fires after delay -> forward to DDR via init_socket
// ============================================================
void SLINK_Module::slink_fwd_output_thread() {
    while (true) {
        tlm_generic_payload* trans = fwd_peq.get_next_transaction();
        if (!trans) { wait(fwd_peq.get_event()); continue; }

        // Forward to DDR
        tlm::tlm_phase phase = tlm::BEGIN_REQ;
        sc_time delay = SC_ZERO_TIME;
        init_socket->nb_transport_fw(*trans, phase, delay);
    }
}

// ============================================================
// Backward path dispatch thread:
//   rsp_fifo -> schedule into bwd_peq with NoC pipeline delay
// ============================================================
void SLINK_Module::slink_bwd_thread() {
    while (true) {
        tlm_generic_payload* trans = rsp_fifo.read();
        // Schedule into PEQ: fires after SLINK_NOC_LATENCY_NS
        bwd_peq.notify(*trans, sc_time(SLINK_NOC_LATENCY_NS, SC_NS));
    }
}

// ============================================================
// Backward path output thread:
//   bwd_peq fires after delay -> forward to IOMMU via targ_socket
// ============================================================
void SLINK_Module::slink_bwd_output_thread() {
    while (true) {
        tlm_generic_payload* trans = bwd_peq.get_next_transaction();
        if (!trans) { wait(bwd_peq.get_event()); continue; }

        // Forward response to IOMMU
        tlm::tlm_phase phase = tlm::BEGIN_RESP;
        sc_time delay = SC_ZERO_TIME;
        targ_socket->nb_transport_bw(*trans, phase, delay);
    }
}
