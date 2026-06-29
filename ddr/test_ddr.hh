#ifndef TEST_DDR_HH
#define TEST_DDR_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/peq_with_get.h"
#include <cstring>
#include "iommu_perf_params.hh"

using namespace std;
using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;

class DDR_Module : public sc_module {
public:
    // ========== Configuration ==========
    static const int DDR_READ_MAX_OUTSTANDING  = 64;
    static const int DDR_INTERNAL_LATENCY_NS   = 100;  // ns, 内部固定访问延时
    static const int DDR_WRITE_MAX_OUTSTANDING = 64;

    // Simulated DDR memory - 16MB storage space (expanded for 2MB GPA stride page tables)
    static const size_t DDR_MEMORY_SIZE = 16 * 1024 * 1024;
    unsigned char* memory;

public:
    // Target sockets (slave)
    simple_target_socket<DDR_Module, 64> axi_slave_from_cmn_rnd_socket;
    simple_target_socket<DDR_Module, 64> axi_slave_from_pcie_noc_0_socket;
    simple_target_socket<DDR_Module, 64> axi_slave_from_cmn_rnd_1_socket;

    // Internal request FIFO
    sc_fifo<tlm_generic_payload*> ddr_req_fifo;

    // Single PEQ for parallel delay (preserves FIFO order)
    peq_with_get<tlm_generic_payload> ddr_peq;

    // Outstanding counters (read/write separate flow control)
    int read_outstanding;
    int write_outstanding;
    sc_event read_slot_freed;
    sc_event write_slot_freed;

    SC_HAS_PROCESS(DDR_Module);

    // Constructor
    DDR_Module(sc_module_name name);

    // Destructor
    ~DDR_Module() { delete[] memory; }

    // TLM interface callbacks
    tlm::tlm_sync_enum nb_transport_fw(
        tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay);
    void b_transport(tlm_generic_payload& trans, sc_time& delay);

    // SC_THREAD processes
    void ddr_dispatch_thread();
    void ddr_response_thread();

    // Internal helper
    void process_memory_access(tlm_generic_payload& trans);
};

#endif
