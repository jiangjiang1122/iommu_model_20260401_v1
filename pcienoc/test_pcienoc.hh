#ifndef TEST_PCIENOC_HH
#define TEST_PCIENOC_HH

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_initiator_socket.h>
#include "../iommu/param_trans_def.hh"
#include "../iommu/iommu_struct.hh"

class PCIENOC_Module : public sc_core::sc_module {
public:
    // 与IOMMU模块的target socket对应的initiator socket (master)
    tlm_utils::simple_initiator_socket<PCIENOC_Module, 64> ahb_master_to_pcie_noc_1_socket;

    SC_HAS_PROCESS(PCIENOC_Module);

    PCIENOC_Module(sc_core::sc_module_name name) : sc_module(name) {
    }
};

#endif