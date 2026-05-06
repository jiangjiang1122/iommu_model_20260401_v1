#ifndef TEST_DDR_HH
#define TEST_DDR_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/simple_initiator_socket.h"
#include <cstring>  // for memcpy and memset
#include "../iommu/iommu_perf_params.hh"

using namespace std;
using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;

class DDR_Module : public sc_module {
public:
    // Simulated DDR memory - 1MB storage space
    unsigned char memory[1024 * 1024]; // 1MB of simulated memory

public:
    // Target sockets (slave) - corresponding to IOMMU initiator sockets
    simple_target_socket<DDR_Module, 64> axi_slave_from_cmn_rnd_socket;
    simple_target_socket<DDR_Module, 64> axi_slave_from_pcie_noc_0_socket;
    simple_target_socket<DDR_Module, 64> axi_slave_from_cmn_rnd_1_socket;

    // Internal request FIFO for non-blocking processing
    sc_fifo<tlm_generic_payload*> ddr_req_fifo;

    SC_HAS_PROCESS(DDR_Module);

    DDR_Module(sc_module_name name) : sc_module(name),
        ddr_req_fifo("ddr_req_fifo", 512)
    {
        // Initialize simulated memory
        memset(memory, 0, sizeof(memory));

        // Register nb_transport_fw for the main DDR socket (from IOMMU arbiter)
        axi_slave_from_cmn_rnd_1_socket.register_nb_transport_fw(
            this, &DDR_Module::nb_transport_fw);

        // Keep b_transport for the other sockets (used by RP direct memory access)
        axi_slave_from_cmn_rnd_socket.register_b_transport(this, &DDR_Module::b_transport);
        axi_slave_from_pcie_noc_0_socket.register_b_transport(this, &DDR_Module::b_transport);

        SC_THREAD(ddr_process_thread);
    }

    // Non-blocking transport forward (AT mode - for IOMMU DDR arbiter)
    tlm::tlm_sync_enum nb_transport_fw(
        tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
    {
        if (phase == tlm::BEGIN_REQ) {
            // Enqueue request payload pointer for processing by ddr_process_thread
            ddr_req_fifo.nb_write(&trans);
            phase = tlm::END_REQ;
            return tlm::TLM_UPDATED;
        }
        else if (phase == tlm::END_RESP) {
            return tlm::TLM_COMPLETED;
        }
        return tlm::TLM_ACCEPTED;
    }

    // DDR processing thread - simulates DDR latency
    void ddr_process_thread() {
        while (true) {
            tlm_generic_payload* trans = ddr_req_fifo.read();

            // Simulate DDR read/write latency
            if (trans->get_command() == tlm::TLM_READ_COMMAND) {
                wait(DDR_READ_LATENCY, SC_NS);
            } else {
                wait(DDR_WRITE_LATENCY, SC_NS);
            }

            // Execute memory read/write
            process_memory_access(*trans);

            // Return response via nb_transport_bw
            tlm::tlm_phase phase = tlm::BEGIN_RESP;
            sc_time delay = SC_ZERO_TIME;
            axi_slave_from_cmn_rnd_1_socket->nb_transport_bw(*trans, phase, delay);
        }
    }

    // Shared memory access logic
    void process_memory_access(tlm_generic_payload& trans) {
        tlm_command cmd = trans.get_command();
        sc_dt::uint64 addr = trans.get_address();
        unsigned char* data = trans.get_data_ptr();
        unsigned int len = trans.get_data_length();

        // Check address range
        if (addr + len > sizeof(memory)) {
            printf("[DDR] ERROR: Address 0x%lx + len %d exceeds memory size %zu\n",
                   (unsigned long)addr, len, sizeof(memory));
            trans.set_response_status(TLM_ADDRESS_ERROR_RESPONSE);
            return;
        }

        if (cmd == TLM_READ_COMMAND) {
            memcpy(data, &memory[addr], len);
#ifdef DEBUG_TRANSLATION
            printf("[DDR] READ addr=0x%lx, len=%d\n", (unsigned long)addr, len);
            fflush(stdout);
#endif
        } else if (cmd == TLM_WRITE_COMMAND) {
            memcpy(&memory[addr], data, len);
#ifdef DEBUG_TRANSLATION
            printf("[DDR] WRITE addr=0x%lx, len=%d\n", (unsigned long)addr, len);
            fflush(stdout);
#endif
        }

        trans.set_response_status(TLM_OK_RESPONSE);
    }

    // Blocking transport (for RP direct memory access and other non-performance paths)
    void b_transport(tlm_generic_payload& trans, sc_time& delay) {
        process_memory_access(trans);
    }
};
#endif
