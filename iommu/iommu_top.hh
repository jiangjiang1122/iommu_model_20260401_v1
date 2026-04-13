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

using namespace std;
using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;

#define IOMMU_BASE_ADDR 0x80000000ULL

// 前向声明
class iommu_top : public sc_module
{
public:
    // ==================== Socket 声明 ====================
    // AXI Stream: MSI data to IMSIC
    tlm_utils::simple_initiator_socket<iommu_top, BUS_WIDTH> axi_stream_to_cmn_rnd_socket;
    
    // AXI Master 0: DMA data forward / access RP
    tlm_utils::simple_initiator_socket<iommu_top, BUS_WIDTH> axi_master_0_to_pcie_noc_socket;
    
    // AXI Master 1: Access DDT/PDT/CQ/FQ/PQ/MRIF (64b@1GHz)
    tlm_utils::simple_initiator_socket<iommu_top, BUS_WIDTH> axi_master_1_to_cmn_rnd_socket;
    
    // AXI Master 2: Send ATS msg back to RP (128b@1GHz)
    tlm_utils::simple_initiator_socket<iommu_top, BUS_WIDTH> axi_master_2_to_pcie_noc_socket;

    // AXI Slave 0: Receive translation requests (512b@1GHz)
    tlm_utils::simple_target_socket<iommu_top, BUS_WIDTH> axi_slave_from_pcie_noc_0_socket;
    
    // AHB Slave: Firmware register access (32b@100MHz)
    tlm_utils::simple_target_socket<iommu_top, BUS_WIDTH> ahb_slave_from_pcie_noc_1_socket;

    // ==================== 全局数据结构 ====================
    iommu_t iommu_inst;
    
    // AXI ID 分配器
    axi_id_allocator_t axi_id_alloc;
    
    // Outstanding DDR 请求表
    std::map<uint16_t, ddr_outstanding_entry_t> ddr_outstanding_table;
    
    // Walker Cache 实例（内置于 PTW 模块）
    walker_cache_t walker_cache;
    
    // 全局任务计数器
    uint32_t global_task_counter;
    
    // sc_mutex 保护并发访问
    sc_mutex global_mutex;

    // ==================== FIFO 声明 ====================
    
    // === 入站缓冲 ===
    sc_fifo<tlm::tlm_generic_payload*> inbound_fifo;              // 深度 16

    // === Parser 输出 ===
    sc_fifo<iommu_task_t*> parser_to_collector_fifo;              // 深度 16
    sc_fifo<iommu_task_t*> parser_to_pq_fifo;                     // 深度 4
    sc_fifo<iommu_task_t*> parser_to_dc_cache_query_fifo;         // 深度 8
    sc_fifo<iommu_task_t*> parser_to_pc_cache_query_fifo;         // 深度 8

    // === Cache 查询结果返回 Collector ===
    sc_fifo<iommu_task_t*> dc_cache_to_collector_fifo;            // 深度 8
    sc_fifo<iommu_task_t*> pc_cache_to_collector_fifo;            // 深度 8

    // === Collector 到 Cache (更新操作) ===
    sc_fifo<iommu_task_t*> collector_to_dc_cache_update_fifo;     // 深度 8
    sc_fifo<iommu_task_t*> collector_to_pc_cache_update_fifo;     // 深度 8

    // === Collector 到 Walker ===
    sc_fifo<iommu_task_t*> collector_to_xdtw_fifo;                // 深度 4

    // === Collector 到 PT/MSIPT Cache (查询操作) ===
    sc_fifo<iommu_task_t*> collector_to_pt_cache_query_fifo;      // 深度 8
    sc_fifo<iommu_task_t*> collector_to_msipt_cache_query_fifo;   // 深度 8

    // === xDTW 返回 ===
    sc_fifo<iommu_task_t*> xdtw_to_collector_fifo;                // 深度 4

    // === PT Cache 内部 + 外部 FIFO ===
    sc_fifo<iommu_task_t*> pt_cache_lookup_result_fifo;           // 深度 4 (内部)
    sc_fifo<iommu_task_t*> pt_cache_to_ptw_fifo;                  // 深度 4 (PT Cache miss -> PTW)
    sc_fifo<iommu_task_t*> ptw_to_pt_cache_fifo;                  // 深度 4 (PTW 结果返回 PT Cache)
    sc_fifo<iommu_task_t*> pt_cache_to_fwd_fifo;                  // 深度 8 (地址翻译完成 -> AXI Master 0)

    // === MSIPT Cache 内部 + 外部 FIFO ===
    sc_fifo<iommu_task_t*> msipt_cache_lookup_result_fifo;        // 深度 4 (内部)
    sc_fifo<iommu_task_t*> msipt_cache_to_msiptw_fifo;            // 深度 4 (MSIPT Cache miss -> 内置 MSIPTW)
    sc_fifo<iommu_task_t*> msiptw_to_msipt_cache_fifo;            // 深度 4 (MSIPTW 结果返回 MSIPT Cache)
    sc_fifo<iommu_task_t*> msipt_cache_to_fwd_fifo;               // 深度 8 (MSI 翻译完成 -> AXI Stream / AXI Master 2)

    // === 全局错误处理 ===
    sc_fifo<iommu_task_t*> fault_fifo;                             // 深度 8

    // === Cache Invalidation (V4 新增) ===
    sc_fifo<cache_inv_cmd_t> cq_to_cache_inv_fifo;                // 深度 8

    // === DDR 响应 FIFO（统一） ===
    sc_fifo<ddr_response_t> ddr_rsp_fifo;                          // 深度 16

    // ==================== DDR 响应路由用内部队列 ====================
    // xDTW 响应队列
    std::queue<ddr_response_t> xdtw_rsp_queue;
    sc_event xdtw_rsp_evt;
    
    // PTW 响应队列
    std::queue<ddr_response_t> ptw_rsp_queue;
    sc_event ptw_rsp_evt;
    
    // MSIPTW 响应队列
    std::queue<ddr_response_t> msiptw_rsp_queue;
    sc_event msiptw_rsp_evt;

    // ==================== 构造函数 ====================
    SC_HAS_PROCESS(iommu_top);
    iommu_top(sc_core::sc_module_name name) : sc_module(name),
        inbound_fifo(FIFO_DEPTH_INBOUND),
        parser_to_collector_fifo(FIFO_DEPTH_PARSER_TO_COLLECTOR),
        parser_to_pq_fifo(FIFO_DEPTH_PARSER_TO_PQ),
        parser_to_dc_cache_query_fifo(FIFO_DEPTH_COLLECTOR_TO_DC_CACHE_UPDATE),
        parser_to_pc_cache_query_fifo(FIFO_DEPTH_COLLECTOR_TO_PC_CACHE_UPDATE),
        dc_cache_to_collector_fifo(FIFO_DEPTH_DC_CACHE_TO_COLLECTOR),
        pc_cache_to_collector_fifo(FIFO_DEPTH_PC_CACHE_TO_COLLECTOR),
        collector_to_dc_cache_update_fifo(FIFO_DEPTH_COLLECTOR_TO_DC_CACHE_UPDATE),
        collector_to_pc_cache_update_fifo(FIFO_DEPTH_COLLECTOR_TO_PC_CACHE_UPDATE),
        collector_to_xdtw_fifo(FIFO_DEPTH_COLLECTOR_TO_XDTW),
        collector_to_pt_cache_query_fifo(FIFO_DEPTH_COLLECTOR_TO_PT_CACHE_QUERY),
        collector_to_msipt_cache_query_fifo(FIFO_DEPTH_COLLECTOR_TO_MSIPT_CACHE_QUERY),
        xdtw_to_collector_fifo(FIFO_DEPTH_XDTW_TO_COLLECTOR),
        pt_cache_lookup_result_fifo(4),
        pt_cache_to_ptw_fifo(FIFO_DEPTH_PT_CACHE_TO_PTW),
        ptw_to_pt_cache_fifo(FIFO_DEPTH_PTW_TO_PT_CACHE),
        pt_cache_to_fwd_fifo(FIFO_DEPTH_PT_CACHE_TO_FWD),
        msipt_cache_lookup_result_fifo(4),
        msipt_cache_to_msiptw_fifo(FIFO_DEPTH_MSIPT_CACHE_TO_MSIPTW),
        msiptw_to_msipt_cache_fifo(FIFO_DEPTH_MSIPTW_TO_MSIPT_CACHE),
        msipt_cache_to_fwd_fifo(FIFO_DEPTH_MSIPT_CACHE_TO_FWD),
        fault_fifo(FIFO_DEPTH_COLLECTOR_TO_FAULT),
        cq_to_cache_inv_fifo(8),
        ddr_rsp_fifo(FIFO_DEPTH_DDR_RSP)
    {
        iommu_inst.top = this;
        global_task_counter = 0;
        axi_id_alloc.init(MAX_AXI_IDS);

        // 注册 socket 回调
        axi_slave_from_pcie_noc_0_socket.register_nb_transport_fw(this, &iommu_top::axi_slave_nb_transport);
        ahb_slave_from_pcie_noc_1_socket.register_b_transport(this, &iommu_top::ahb_slave_b_transport);

        // 注册 DDR 响应回调
        axi_master_1_to_cmn_rnd_socket.register_nb_transport_bw(this, &iommu_top::ddr_nb_transport_bw);

        // ==================== 注册所有 SC_THREAD ====================
        
        // Parser: 1 个线程
        SC_THREAD(parser_thread);

        // Collector: 2 个线程
        SC_THREAD(collector_cache_lookup_result_thread);
        SC_THREAD(collector_xdtw_response_thread);

        // DC/PC Cache: 各 1 个线程
        SC_THREAD(dc_cache_thread);
        SC_THREAD(pc_cache_thread);

        // PT Cache: 3 个线程
        SC_THREAD(pt_cache_query_thread);
        SC_THREAD(pt_cache_result_thread);
        SC_THREAD(pt_cache_ptw_rsp_thread);

        // MSIPT Cache: 4 个线程（含内置 MSIPTW）
        SC_THREAD(msipt_cache_query_thread);
        SC_THREAD(msipt_cache_result_thread);
        SC_THREAD(msiptw_req_thread);
        SC_THREAD(msiptw_rsp_thread);

        // xDTW: 2 个线程
        SC_THREAD(xdtw_req_thread);
        SC_THREAD(xdtw_rsp_thread);

        // PTW: 2 个线程（含 Walker Cache）
        SC_THREAD(ptw_req_thread);
        SC_THREAD(ptw_rsp_thread);

        // DDR 响应路由：1 个线程
        SC_THREAD(ddr_rsp_router_thread);

        // Forwarder: 2 个线程
        SC_THREAD(forwarder_thread);
        SC_THREAD(msi_forwarder_thread);

        // Fault/CQ Proc: 2 个线程
        SC_THREAD(fault_proc_thread);
        SC_THREAD(cq_proc_thread);
    }

    ~iommu_top() {
        // 清理未释放的任务
    }

    // ==================== Socket 回调声明 ====================
    
    // AXI Slave 0 nb_transport 回调（接收翻译请求）
    tlm::tlm_sync_enum axi_slave_nb_transport(
        tlm::tlm_generic_payload& trans,
        tlm::tlm_phase& phase,
        sc_time& delay
    );

    // AHB Slave b_transport 回调（寄存器访问）
    void ahb_slave_b_transport(tlm::tlm_generic_payload& trans, sc_time& delay);

    // DDR nb_transport_bw 回调（接收 DDR 响应）
    tlm::tlm_sync_enum ddr_nb_transport_bw(
        tlm::tlm_generic_payload& trans,
        tlm::tlm_phase& phase,
        sc_time& delay
    );

    void before_end_of_elaboration();

    // ==================== SC_THREAD 声明 ====================
    
    // Parser
    void parser_thread();

    // Collector
    void collector_cache_lookup_result_thread();
    void collector_xdtw_response_thread();

    // DC/PC Cache
    void dc_cache_thread();
    void pc_cache_thread();

    // PT Cache
    void pt_cache_query_thread();
    void pt_cache_result_thread();
    void pt_cache_ptw_rsp_thread();

    // MSIPT Cache
    void msipt_cache_query_thread();
    void msipt_cache_result_thread();
    void msiptw_req_thread();
    void msiptw_rsp_thread();

    // xDTW
    void xdtw_req_thread();
    void xdtw_rsp_thread();

    // PTW (含 Walker Cache)
    void ptw_req_thread();
    void ptw_rsp_thread();

    // DDR 响应路由
    void ddr_rsp_router_thread();

    // Forwarder
    void forwarder_thread();
    void msi_forwarder_thread();

    // Fault/CQ Proc
    void fault_proc_thread();
    void cq_proc_thread();

    // ==================== DDR 访问辅助函数 ====================
    
    // 非阻塞 DDR 读
    void send_ddr_nb_read(uint64_t addr, uint8_t size, uint16_t axi_id, walk_type_t type, iommu_task_t* task);
    
    // 阻塞 DDR 读（用于原子操作）
    void send_ddr_blocking_read(uint64_t addr, uint8_t size, char* data);
    
    // 阻塞 DDR 写（用于原子操作）
    void send_ddr_blocking_write(uint64_t addr, uint8_t size, char* data);
    
    // 阻塞 DDR 原子 OR 写（用于 MRIF）
    void send_ddr_blocking_atomic_or(uint64_t addr, uint64_t or_value, uint8_t size);

    // ==================== Cache 查找/更新辅助函数 ====================
    
    // DC Cache 查找
    uint8_t lookup_dc_cache(uint32_t device_id, device_context_t* DC);
    
    // DC Cache 更新
    void update_dc_cache(uint32_t device_id, device_context_t* DC);
    
    // PC Cache 查找
    uint8_t lookup_pc_cache(uint32_t device_id, uint32_t process_id, process_context_t* PC);
    
    // PC Cache 更新
    void update_pc_cache(uint32_t device_id, uint32_t process_id, process_context_t* PC);
    
    // PT Cache (IOTLB) 查找
    uint8_t lookup_iotlb(uint64_t iova, uint8_t PSCV, uint32_t PSCID, uint8_t GV, uint16_t GSCID,
                         uint8_t priv, uint8_t is_read, uint8_t is_write, uint8_t is_exec, uint8_t SUM,
                         uint32_t* cause, uint64_t* pa, uint64_t* page_sz, spte_t* vs_pte, gpte_t* g_pte);
    
    // PT Cache (IOTLB) 更新
    void update_iotlb(uint64_t iova, spte_t vs_pte, gpte_t g_pte, uint64_t pa, uint64_t page_sz,
                      uint8_t PSCV, uint32_t PSCID, uint8_t GV, uint16_t GSCID);
    
    // MSIPT Cache 查找
    uint8_t lookup_msipt_cache(uint64_t msiptp_ppn, uint32_t interrupt_file_num, uint64_t* msipte_data);
    
    // MSIPT Cache 更新
    void update_msipt_cache(uint64_t msiptp_ppn, uint32_t interrupt_file_num, uint64_t msipte_data);
};

#endif
