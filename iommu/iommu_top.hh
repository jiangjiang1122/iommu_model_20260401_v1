#ifndef IOMMU_TOP_HH
#define IOMMU_TOP_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include <queue>
#include "iommu_struct.hh"
#include "param_trans_def.hh"

using namespace std;
using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;

#define IOMMU_BASE_ADDR 0x80000000ULL

class iommu_top : public sc_module
{
public:
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_stream_to_cmn_rnd_socket; // msi data to IMSIC;
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_master_0_to_pcie_noc_socket; // access ddr for dma data / access rp
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_master_1_to_cmn_rnd_socket; // access ddr to set/get ddt/pdt/cq/fq/pq/mrif
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_master_2_to_pcie_noc_socket; // send ats msg back to rp

    tlm_utils::simple_target_socket<iommu_top,BUS_WIDTH>  axi_slave_from_pcie_noc_0_socket;
    tlm_utils::simple_target_socket<iommu_top,BUS_WIDTH>  ahb_slave_from_pcie_noc_1_socket;

    iommu_t iommu_inst;

    sc_event cq_process_evt;
    std::queue<uint8_t> Ini_Process_queue;

    void axi_slave_b_transport(tlm::tlm_generic_payload &trans,sc_time &delay);
    void ahb_slave_b_transport(tlm::tlm_generic_payload &trans,sc_time &delay);

    void CQ_Monitor_Process_Thread();

    void before_end_of_elaboration();

    SC_HAS_PROCESS(iommu_top);
    iommu_top(sc_core::sc_module_name name) : sc_module(name) {
        iommu_inst.top = this;

        axi_slave_from_pcie_noc_0_socket.register_b_transport(this, &iommu_top::axi_slave_b_transport);
        ahb_slave_from_pcie_noc_1_socket.register_b_transport(this, &iommu_top::ahb_slave_b_transport);

        SC_THREAD(CQ_Monitor_Process_Thread);
        sensitive << cq_process_evt;
    }

    ~iommu_top(){
    }
};

#endif