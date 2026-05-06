#ifndef IOMMU_TOP_HH
#define IOMMU_TOP_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include <queue>
#include <map>
#include "iommu_struct.hh"
#include "param_trans_def.hh"
#include "iommu_task.hh"
#include "iommu_perf_params.hh"
#include "iommu_perf_model.hh"

using namespace std;
using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;

#define IOMMU_BASE_ADDR 0x80000000ULL

class iommu_top : public sc_module
{
public:
    // ===================== Sockets (unchanged names for main.cpp compatibility) =====================
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_stream_to_cmn_rnd_socket;      // MSI data to IMSIC
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_master_0_to_pcie_noc_socket;    // DMA data / access RP
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_master_1_to_cmn_rnd_socket;     // DDR access for DDT/PDT/CQ/FQ/PQ/MRIF
    tlm_utils::simple_initiator_socket<iommu_top,BUS_WIDTH> axi_master_2_to_pcie_noc_socket;    // ATS msg back to RP

    tlm_utils::simple_target_socket<iommu_top,BUS_WIDTH>  axi_slave_from_pcie_noc_0_socket;
    tlm_utils::simple_target_socket<iommu_top,BUS_WIDTH>  ahb_slave_from_pcie_noc_1_socket;

    // ===================== IOMMU Instance =====================
    iommu_t iommu_inst;

    // ===================== Task ID Counter =====================
    uint32_t next_task_id;
    sc_mutex task_id_mtx;

    // ===================== Pipeline Data FIFOs (sc_fifo<iommu_task_t*>) =====================
    sc_fifo<iommu_task_t*> inbound_fifo;
    sc_fifo<iommu_task_t*> parser_to_collector_fifo;
    sc_fifo<iommu_task_t*> parser_to_dc_cache_query_fifo;
    sc_fifo<iommu_task_t*> parser_to_pc_cache_query_fifo;
    sc_fifo<iommu_task_t*> dc_cache_to_collector_fifo;
    sc_fifo<iommu_task_t*> pc_cache_to_collector_fifo;
    sc_fifo<iommu_task_t*> collector_to_xdtw_dc_fifo;
    sc_fifo<iommu_task_t*> collector_to_xdtw_pc_fifo;
    sc_fifo<iommu_task_t*> collector_to_dc_cache_update_fifo;
    sc_fifo<iommu_task_t*> collector_to_pc_cache_update_fifo;
    sc_fifo<iommu_task_t*> collector_to_pt_cache_query_fifo;
    sc_fifo<iommu_task_t*> collector_to_msipt_cache_query_fifo;
    sc_fifo<iommu_task_t*> collector_to_fault_fifo;
    sc_fifo<iommu_task_t*> xdtw_to_collector_fifo;
    sc_fifo<iommu_task_t*> pt_cache_to_ptw_fifo;
    sc_fifo<iommu_task_t*> ptw_to_pt_cache_fifo;
    sc_fifo<iommu_task_t*> pt_cache_to_fwd_fifo;
    sc_fifo<iommu_task_t*> msipt_cache_to_msiptw_fifo;
    sc_fifo<iommu_task_t*> msiptw_to_msipt_cache_fifo;
    sc_fifo<iommu_task_t*> msipt_cache_to_fwd_fifo;

    // ===================== DDR Request/Response FIFOs =====================
    sc_fifo<ddr_req_entry_t> xdtw_req_ddr_fifo;
    sc_fifo<ddr_rsp_entry_t> xdtw_rsp_ddr_fifo;
    sc_fifo<ddr_req_entry_t> ptw_req_ddr_fifo;
    sc_fifo<ddr_rsp_entry_t> ptw_rsp_ddr_fifo;
    sc_fifo<ddr_req_entry_t> msiptw_req_ddr_fifo;
    sc_fifo<ddr_rsp_entry_t> msiptw_rsp_ddr_fifo;
    sc_fifo<ctrl_path_ddr_req_t> ctrl_path_req_ddr_fifo;

    // ===================== DDR Pending Queue =====================
    std::queue<ddr_pending_entry_t> ddr_pending_queue;
    sc_mutex ddr_queue_mtx;
    sc_event ddr_pending_freed_event;

    // ===================== Control Path Response =====================
    uint8_t ctrl_path_rsp_buf[64];
    sc_event ctrl_path_rsp_event;

    // ===================== Collector Pending Tasks =====================
    std::map<uint32_t, collector_entry_t> pending_tasks;
    sc_mutex collector_mtx;

    // ===================== Walker Active Walks =====================
    std::map<uint32_t, iommu_task_t*> xdtw_active_walks;
    sc_mutex xdtw_walks_mtx;
    int xdtw_dc_outstanding_task_count;
    int xdtw_pc_outstanding_task_count;
    sc_event xdtw_dc_task_completed_event;
    sc_event xdtw_pc_task_completed_event;

    // Collector walk outstanding counters
    int collector_dc_walk_outstanding;
    int collector_pc_walk_outstanding;
    sc_event collector_dc_walk_completed_event;
    sc_event collector_pc_walk_completed_event;

    std::map<uint32_t, iommu_task_t*> ptw_active_walks;
    sc_mutex ptw_walks_mtx;
    int ptw_outstanding_task_count;
    sc_event ptw_task_completed_event;

    std::map<uint32_t, iommu_task_t*> msiptw_active_walks;
    sc_mutex msiptw_walks_mtx;
    int msiptw_outstanding_task_count;
    sc_event msiptw_task_completed_event;

    // ===================== Cache Mutex =====================
    sc_mutex iommu_cache_mtx;
    sc_mutex walker_cache_mtx;

    // ===================== Legacy CQ Event =====================
    sc_event cq_process_evt;
    std::queue<uint8_t> Ini_Process_queue;

    // ===================== 21 SC_THREAD Declarations =====================
    // Parser (1 thread)
    void parser_thread();

    // DC Cache (2 threads)
    void dc_cache_query_thread();
    void dc_cache_update_thread();

    // PC Cache (2 threads)
    void pc_cache_query_thread();
    void pc_cache_update_thread();

    // Collector (2 threads)
    void collector_cache_lookup_result_thread();
    void collector_xdtw_response_thread();

    // xDTW (2 threads)
    void xdtw_req_thread();
    void xdtw_rsp_thread();

    // PT Cache (3 threads)
    void pt_cache_query_thread();
    void pt_cache_result_thread();
    void pt_cache_ptw_rsp_thread();

    // PTW (2 threads)
    void ptw_req_thread();
    void ptw_rsp_thread();

    // MSIPT Cache (2 threads)
    void msipt_cache_query_thread();
    void msipt_cache_result_thread();

    // MSIPTW (2 threads)
    void msiptw_req_thread();
    void msiptw_rsp_thread();

    // Forwarder (1 thread)
    void forwarder_thread();

    // Fault/CQ (1 thread)
    void fault_cq_proc_thread();

    // DDR Arbiter (1 thread)
    void ddr_arbiter_thread();

    // ===================== AT Transport Callbacks =====================
    // Inbound AT callback (RP -> IOMMU)
    tlm::tlm_sync_enum axi_slave_nb_transport_fw(
        tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay);

    // DDR response AT callback (DDR -> IOMMU)
    tlm::tlm_sync_enum ddr_nb_transport_bw(
        tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay);

    // ===================== Helper Methods =====================
    void send_response_to_initiator(iommu_task_t* task);
    void configure_and_route(iommu_task_t* task);
    void init_gstage_walk(iommu_task_t* task, uint64_t gpa);

    // Legacy b_transport (kept for AHB register access)
    void axi_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay);
    void ahb_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay);

    void before_end_of_elaboration();

    SC_HAS_PROCESS(iommu_top);
    iommu_top(sc_core::sc_module_name name) : sc_module(name),
        // Initialize FIFOs with SPEC-defined depths
        inbound_fifo("inbound_fifo", FIFO_DEPTH_INBOUND),
        parser_to_collector_fifo("parser_to_collector_fifo", FIFO_DEPTH_PARSER_TO_COLLECTOR),
        parser_to_dc_cache_query_fifo("parser_to_dc_cache_query_fifo", FIFO_DEPTH_PARSER_TO_DC_CACHE_QUERY),
        parser_to_pc_cache_query_fifo("parser_to_pc_cache_query_fifo", FIFO_DEPTH_PARSER_TO_PC_CACHE_QUERY),
        dc_cache_to_collector_fifo("dc_cache_to_collector_fifo", FIFO_DEPTH_DC_CACHE_TO_COLLECTOR),
        pc_cache_to_collector_fifo("pc_cache_to_collector_fifo", FIFO_DEPTH_PC_CACHE_TO_COLLECTOR),
        collector_to_xdtw_dc_fifo("collector_to_xdtw_dc_fifo", FIFO_DEPTH_COLLECTOR_TO_XDTW),
        collector_to_xdtw_pc_fifo("collector_to_xdtw_pc_fifo", FIFO_DEPTH_COLLECTOR_TO_XDTW),
        collector_to_dc_cache_update_fifo("collector_to_dc_cache_update_fifo", FIFO_DEPTH_COLLECTOR_TO_DC_CACHE_UPDATE),
        collector_to_pc_cache_update_fifo("collector_to_pc_cache_update_fifo", FIFO_DEPTH_COLLECTOR_TO_PC_CACHE_UPDATE),
        collector_to_pt_cache_query_fifo("collector_to_pt_cache_query_fifo", FIFO_DEPTH_COLLECTOR_TO_PT_CACHE_QUERY),
        collector_to_msipt_cache_query_fifo("collector_to_msipt_cache_query_fifo", FIFO_DEPTH_COLLECTOR_TO_MSIPT_CACHE_QUERY),
        collector_to_fault_fifo("collector_to_fault_fifo", FIFO_DEPTH_COLLECTOR_TO_FAULT),
        xdtw_to_collector_fifo("xdtw_to_collector_fifo", FIFO_DEPTH_XDTW_TO_COLLECTOR),
        pt_cache_to_ptw_fifo("pt_cache_to_ptw_fifo", FIFO_DEPTH_PT_CACHE_TO_PTW),
        ptw_to_pt_cache_fifo("ptw_to_pt_cache_fifo", FIFO_DEPTH_PTW_TO_PT_CACHE),
        pt_cache_to_fwd_fifo("pt_cache_to_fwd_fifo", FIFO_DEPTH_PT_CACHE_TO_FWD),
        msipt_cache_to_msiptw_fifo("msipt_cache_to_msiptw_fifo", FIFO_DEPTH_MSIPT_CACHE_TO_MSIPTW),
        msiptw_to_msipt_cache_fifo("msiptw_to_msipt_cache_fifo", FIFO_DEPTH_MSIPTW_TO_MSIPT_CACHE),
        msipt_cache_to_fwd_fifo("msipt_cache_to_fwd_fifo", FIFO_DEPTH_MSIPT_CACHE_TO_FWD),
        // DDR FIFOs
        xdtw_req_ddr_fifo("xdtw_req_ddr_fifo", FIFO_DEPTH_XDTW_REQ_DDR),
        xdtw_rsp_ddr_fifo("xdtw_rsp_ddr_fifo", FIFO_DEPTH_XDTW_RSP_DDR),
        ptw_req_ddr_fifo("ptw_req_ddr_fifo", FIFO_DEPTH_PTW_REQ_DDR),
        ptw_rsp_ddr_fifo("ptw_rsp_ddr_fifo", FIFO_DEPTH_PTW_RSP_DDR),
        msiptw_req_ddr_fifo("msiptw_req_ddr_fifo", FIFO_DEPTH_MSIPTW_REQ_DDR),
        msiptw_rsp_ddr_fifo("msiptw_rsp_ddr_fifo", FIFO_DEPTH_MSIPTW_RSP_DDR),
        ctrl_path_req_ddr_fifo("ctrl_path_req_ddr_fifo", FIFO_DEPTH_CTRL_PATH_REQ_DDR),
        // State initialization
        next_task_id(1),
        xdtw_dc_outstanding_task_count(0),
        xdtw_pc_outstanding_task_count(0),
        collector_dc_walk_outstanding(0),
        collector_pc_walk_outstanding(0),
        ptw_outstanding_task_count(0),
        msiptw_outstanding_task_count(0)
    {
        iommu_inst.top = this;

        // Register AT nb_transport callbacks for inbound and DDR
        axi_slave_from_pcie_noc_0_socket.register_nb_transport_fw(
            this, &iommu_top::axi_slave_nb_transport_fw);
        axi_master_1_to_cmn_rnd_socket.register_nb_transport_bw(
            this, &iommu_top::ddr_nb_transport_bw);

        // Keep AHB register access as b_transport (non-performance path)
        ahb_slave_from_pcie_noc_1_socket.register_b_transport(
            this, &iommu_top::ahb_slave_b_transport);

        // Register all 21 SC_THREADs
        SC_THREAD(parser_thread);
        SC_THREAD(dc_cache_query_thread);
        SC_THREAD(dc_cache_update_thread);
        SC_THREAD(pc_cache_query_thread);
        SC_THREAD(pc_cache_update_thread);
        SC_THREAD(collector_cache_lookup_result_thread);
        SC_THREAD(collector_xdtw_response_thread);
        SC_THREAD(xdtw_req_thread);
        SC_THREAD(xdtw_rsp_thread);
        SC_THREAD(pt_cache_query_thread);
        SC_THREAD(pt_cache_result_thread);
        SC_THREAD(pt_cache_ptw_rsp_thread);
        SC_THREAD(ptw_req_thread);
        SC_THREAD(ptw_rsp_thread);
        SC_THREAD(msipt_cache_query_thread);
        SC_THREAD(msipt_cache_result_thread);
        SC_THREAD(msiptw_req_thread);
        SC_THREAD(msiptw_rsp_thread);
        SC_THREAD(forwarder_thread);
        SC_THREAD(fault_cq_proc_thread);
        SC_THREAD(ddr_arbiter_thread);

        memset(ctrl_path_rsp_buf, 0, sizeof(ctrl_path_rsp_buf));
    }

    ~iommu_top(){
    }
};

#endif
