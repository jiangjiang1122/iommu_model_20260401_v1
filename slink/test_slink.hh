#ifndef TEST_SLINK_HH
#define TEST_SLINK_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/peq_with_get.h"
#include "iommu_perf_params.hh"
#include <cstdio>

using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;

// ============================================================
// SLINK Module: models the NoC interconnect path between
// IOMMU and DDR for page table / directory table accesses.
// Uses PEQ pipeline delay on both request and response paths.
// ============================================================
class SLINK_Module : public sc_module {
public:
    // ========== Sockets ==========
    // Target socket: receives requests from IOMMU axi_master_1_to_cmn_rnd_socket
    simple_target_socket<SLINK_Module, 64> targ_socket;

    // Initiator socket: forwards requests to DDR axi_slave_from_cmn_rnd_1_socket
    simple_initiator_socket<SLINK_Module, 64> init_socket;

    // ========== Internal FIFOs ==========
    sc_fifo<tlm_generic_payload*> req_fifo;   // Request path FIFO (depth=256, matches IOMMU master outstanding)
    sc_fifo<tlm_generic_payload*> rsp_fifo;   // Response path FIFO

    // ========== PEQs for pipeline delay ==========
    peq_with_get<tlm_generic_payload> fwd_peq;   // Forward path pipeline delay
    peq_with_get<tlm_generic_payload> bwd_peq;   // Backward path pipeline delay

    SC_HAS_PROCESS(SLINK_Module);

    // Constructor
    SLINK_Module(sc_module_name name);

    // ========== TLM Callbacks ==========
    tlm::tlm_sync_enum nb_transport_fw(
        tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay);

    tlm::tlm_sync_enum nb_transport_bw(
        tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay);

    // ========== Processing Threads ==========
    void slink_fwd_thread();           // req_fifo -> fwd_peq (schedule with NoC delay)
    void slink_fwd_output_thread();    // fwd_peq fires -> forward to DDR
    void slink_bwd_thread();           // rsp_fifo -> bwd_peq (schedule with NoC delay)
    void slink_bwd_output_thread();    // bwd_peq fires -> forward to IOMMU
};

#endif
