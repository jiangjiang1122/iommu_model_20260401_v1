#ifndef TEST_RP_HH
#define TEST_RP_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include "param_trans_def.hh"
#include "iommu_struct.hh"
#include "../ddr/test_ddr.hh"

using namespace std;
using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;

int test_num = 0;
// Global variable declarations
ats_msg_t exp_msg;
ats_msg_t rcvd_msg;
uint8_t exp_msg_received;
uint8_t message_received;
int8_t *memory;
uint64_t access_viol_addr;
uint64_t data_corruption_addr;
uint8_t pr_go_requested;
uint8_t pw_go_requested;
uint64_t next_free_page;
uint64_t next_free_gpage[65536];
int test_endian;

#define START_TEST(__STR__)\
    test_num++;\
    printf("Test %02d : %-40s : ", test_num, __STR__);
#define fail_if(__COND__) if(__COND__) {printf("\x1B[31mFAIL. Line %d\x1B[0m\n", __LINE__); return;}
#define END_TEST() {printf("\x1B[32mPASS\x1B[0m\n");}
#define FOR_ALL_TRANSACTION_TYPES(at, pid_valid, exec_req, priv_req, no_write, code)\
    for ( at = 0; at < 3; at++ ) {\
        for ( pid_valid = 0; pid_valid < 2; pid_valid++ ) {\
            for ( exec_req = 0; exec_req < 2; exec_req++ ) {\
                for ( priv_req = 0; priv_req < 2; priv_req++ ) {\
                    for ( no_write = 0; no_write < 2; no_write++ )  {\
                        code\
                    }\
                }\
            }\
        }\
    }

extern int8_t *memory;
extern uint64_t next_free_page;
extern uint64_t next_free_gpage[65536];

class RP_Module : public sc_module {
public:
    // Initiator socket (master) - to IOMMU target socket
    simple_initiator_socket<RP_Module, 64> axi_master_to_pcie_noc_0_socket;

    // Target socket for ATS responses from IOMMU
    simple_target_socket<RP_Module, 64> axi_slave_to_pcie_noc_0_socket;

    // Pointers to IOMMU and DDR modules for direct operations
    iommu_top* iommu_ptr;
    DDR_Module* ddr_ptr;

    // AT response synchronization
    sc_event response_event;
    tlm::tlm_generic_payload* pending_response_trans;

    // Multi-request response tracking
    int response_count;
    sc_event response_count_event;

    SC_HAS_PROCESS(RP_Module);

    RP_Module(sc_module_name name, iommu_top* iommu_module, DDR_Module* ddr_module) :
        sc_module(name), iommu_ptr(iommu_module), ddr_ptr(ddr_module),
        pending_response_trans(nullptr), response_count(0)
    {
        // Register nb_transport_bw for receiving AT responses from IOMMU
        axi_master_to_pcie_noc_0_socket.register_nb_transport_bw(
            this, &RP_Module::nb_transport_bw);

        // Register b_transport for ATS target socket (receives ATS messages)
        axi_slave_to_pcie_noc_0_socket.register_b_transport(
            this, &RP_Module::ats_slave_b_transport);

        SC_THREAD(send_translation_request_1_thread);
    }

    // nb_transport_bw callback: receives AT responses from IOMMU
    tlm::tlm_sync_enum nb_transport_bw(
        tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
    {
        if (phase == tlm::BEGIN_RESP) {
            printf("[RP_RSP] nb_transport_bw: BEGIN_RESP, addr=0x%lx, status=%s\n",
                   (uint64_t)trans.get_address(), trans.get_response_string().c_str());
            fflush(stdout);
            // Save response and notify waiting thread
            pending_response_trans = &trans;
            response_event.notify(SC_ZERO_TIME);
            // Increment multi-request response counter
            response_count++;
            response_count_event.notify(SC_ZERO_TIME);
            // Send END_RESP
            phase = tlm::END_RESP;
            return tlm::TLM_COMPLETED;
        }
        return tlm::TLM_ACCEPTED;
    }

    // ATS slave b_transport (for receiving ATS messages from IOMMU master_2)
    void ats_slave_b_transport(tlm_generic_payload& trans, sc_time& delay) {
        // Handle ATS invalidation completion and page group responses
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    // Send translation request using AT (non-blocking) method
    void send_translation_request_at(tlm_generic_payload& trans) {
        tlm::tlm_phase phase = tlm::BEGIN_REQ;
        sc_time delay = SC_ZERO_TIME;

        // Send non-blocking request
        tlm::tlm_sync_enum status =
            axi_master_to_pcie_noc_0_socket->nb_transport_fw(trans, phase, delay);

        if (status == TLM_UPDATED && phase == END_REQ) {
            // Request accepted, wait for response
            wait(response_event);
        } else if (status == TLM_COMPLETED) {
            // Transaction completed immediately (shouldn't happen in AT)
        } else {
            // TLM_ACCEPTED: wait for response
            wait(response_event);
        }
    }

    void send_translation_request_1_thread();

    // Function declarations
    void send_translation_request_rp(iommu_top *iommu, uint32_t did, uint8_t pid_valid, uint32_t pid, uint8_t no_write,uint8_t exec_req,
                                    uint8_t priv_req, uint8_t is_cxl_dev, uint8_t at, uint64_t iova,uint32_t length,
                                    uint8_t read_writeAMO,hb_to_iommu_req_t *req, iommu_to_hb_rsp_t *rsp);
    void iommu_translate_iova_rp(iommu_top *iommu,hb_to_iommu_req_t *req, iommu_to_hb_rsp_t *rsp_msg);
    uint8_t read_memory_test_rp(uint64_t addr, uint8_t size, char *data);
    uint8_t write_memory_test_rp(char *data, uint64_t addr, uint32_t size);
    int8_t check_faults_rp(iommu_top *iommu,uint16_t cause, uint8_t  exp_PV, uint32_t exp_PID, uint8_t  exp_PRIV,uint32_t exp_DID,
                           uint64_t exp_iotval, uint8_t ttyp, uint64_t exp_iotval2);
    int8_t check_rsp_and_faults_rp(iommu_top *iommu,hb_to_iommu_req_t *req,iommu_to_hb_rsp_t *rsp,status_t status,uint16_t cause,uint64_t exp_iotval2);
    uint64_t add_device(iommu_top *iommu, uint32_t device_id, uint32_t gscid, uint8_t en_ats, uint8_t en_pri, uint8_t t2gpa,uint8_t dtf, uint8_t prpr,
                        uint8_t gade, uint8_t sade, uint8_t dpe, uint8_t sbe, uint8_t sxl,uint8_t iohgatp_mode, uint8_t iosatp_mode, uint8_t pdt_mode,
                        uint8_t msiptp_mode, uint8_t msiptp_pages, uint64_t msi_addr_mask,uint64_t msi_addr_pattern);
    uint64_t get_free_ppn(uint64_t num_ppn);
    uint64_t get_free_gppn(uint64_t num_gppn, iohgatp_t iohgatp);
    uint64_t add_g_stage_pte (iommu_top *iommu,iohgatp_t iohgatp, uint64_t gpa, gpte_t gpte, uint8_t add_level) ;
    uint64_t add_dev_context(iommu_top *iommu,device_context_t *DC, uint32_t device_id) ;
    uint64_t translate_gpa (iommu_top *iommu, iohgatp_t iohgatp, uint64_t gpa, uint64_t *spa) ;
    uint64_t add_vs_stage_pte (iommu_top *iommu,iosatp_t satp, uint64_t va, spte_t pte, uint8_t add_level,iohgatp_t iohgatp, uint8_t SXL) ;
    uint64_t add_s_stage_pte (iosatp_t satp, uint64_t va, spte_t pte, uint8_t add_level, uint8_t SXL);
    uint64_t add_process_context(iommu_top *iommu,device_context_t *DC, process_context_t *PC, uint32_t process_id) ;

    int8_t reset_system(uint8_t mem_gb, uint16_t num_vms);
    uint32_t log2szm1(uint32_t n);
    int8_t enable_cq(iommu_top *iommu,uint32_t nppn);
    int8_t enable_fq(iommu_top *iommu,uint32_t nppn);
    int8_t enable_disable_pq(iommu_top *iommu,uint32_t nppn, uint8_t enable_disable) ;
    int8_t enable_iommu(iommu_top *iommu,uint8_t iommu_mode);
    int8_t check_exp_pq_rec(iommu_top *iommu, uint32_t DID, uint32_t PID, uint8_t PV, uint8_t PRIV, uint8_t EXEC,uint16_t reserved0, uint8_t reserved1, uint64_t PLOAD);
    // [失效] NL 参数(默认0): 非叶PTE失效扩展位, 需 capabilities.NL=1 才合法。
    // NL=1 且 AV=1 时 IOMMU 将联动失效 Walker Cache(非叶PTE缓存)全部级别。
    void iotinval(iommu_top *iommu,uint8_t f3, uint8_t GV, uint8_t AV, uint8_t PSCV, uint32_t GSCID, uint32_t PSCID, uint64_t address, uint8_t NL = 0);
    void ats_command(iommu_top *iommu,uint8_t f3, uint8_t DSV, uint8_t PV, uint32_t PID, uint8_t DSEG, uint16_t RID, uint64_t payload) ;
    void generic_any(iommu_top *iommu,command_t cmd);
    void iodir(iommu_top *iommu,uint8_t f3, uint8_t DV, uint32_t DID, uint32_t PID);
    void iofence(iommu_top *iommu,uint8_t f3, uint8_t PR, uint8_t PW, uint8_t AV, uint8_t WSI_bit, uint64_t addr, uint32_t data);

    // ============================================================
    // [CPU侧] RISC-V hart 侧 Cache/TLB 失效指令行为建模
    //
    // 本模型不含 CPU core, 这些方法用于在测试激励中**模拟 OS 更新页表时的
    // 完整 CPU 侧序列**, 从而复现真实软件行为并验证一条关键规范语义:
    //   CPU 侧的 SFENCE.VMA / SINVAL.VMA 只失效 hart 自己的 TLB,
    //   **不会**失效 IOMMU 的 IOATC(DC/PC/PT/Walker) —— 后者必须由
    //   IOTINVAL.VMA/GVMA 经命令队列显式失效。
    //
    // 建模口径(无 CPU core, 故为"行为记账 + 时序占位 + 日志"):
    //   - 数据可见性: 测试用 write_memory_test_rp 直写 DDR, 页表对 IOMMU
    //     天然可见, 故 CBO.CLEAN/FLUSH 的回写语义为空操作, 但仍记账与计时,
    //     以便在时间轴上体现 OS 维护开销;
    //   - 每条指令按 cpu_instr_latency_ns 推进仿真时间(默认 2ns/条);
    //   - CBO.* 按 Cache 块粒度操作, 块大小由 CPU_CACHE_BLOCK_SIZE 声明
    //     (对应规范"软件发现机制": 软件必须知道块大小才能正确使用 CBO)
    // ============================================================

    // Cache 块大小(字节): CBO.* 系列指令的操作粒度(软件发现机制)
    static constexpr uint64_t CPU_CACHE_BLOCK_SIZE = 64;
    // 每条 CPU 侧指令的建模延时(ns)
    double cpu_instr_latency_ns = 2.0;

    // FENCE.I: 刷新本地 I-cache 与取指流水线(保证数据写对取指可见)
    void cpu_fence_i();
    // SFENCE.VMA: 失效 hart 本地 TLB。rs1=addr(0=全部), rs2=asid(0=全部地址空间)
    //   注意: 不影响 IOMMU IOATC
    void cpu_sfence_vma(uint64_t addr = 0, uint32_t asid = 0);
    // SINVAL.VMA (Svinval): 与 SFENCE.VMA 相同失效效果, 但不提供内存排序保证,
    //   必须包裹在 SFENCE.W.INVAL ... SFENCE.INVAL.IR 之间
    void cpu_sfence_w_inval();
    void cpu_sinval_vma(uint64_t addr = 0, uint32_t asid = 0);
    void cpu_sfence_inval_ir();
    // CBO.CLEAN: 将脏块写回主存, 不失效该块
    void cpu_cbo_clean(uint64_t addr);
    // CBO.FLUSH: 原子地 Clean 后 Inval
    void cpu_cbo_flush(uint64_t addr);
    // CBO.INVAL: 失效(释放一致性域内该块的所有副本), 不回写
    void cpu_cbo_inval(uint64_t addr);
    // 对 [addr, addr+len) 覆盖的所有 Cache 块执行 CBO.FLUSH(页表区维护常用)
    void cpu_cbo_flush_range(uint64_t addr, uint64_t len);

    // [CPU侧] OS 更新页表后的标准维护序列:
    //   CBO.FLUSH(页表区) -> SFENCE.VMA(失效本 hart TLB) -> FENCE.I(可选)
    // 该序列**不含** IOTINVAL, 调用方随后需显式下发 IOMMU 失效命令。
    void cpu_pagetable_update_sequence(uint64_t pt_addr, uint64_t pt_len,
                                       uint32_t asid = 0, bool with_fence_i = false);

    // [CPU侧] 指令执行计数(用于测试断言与统计报告)
    struct CpuInstrStats {
        uint64_t fence_i = 0;
        uint64_t sfence_vma = 0;
        uint64_t sinval_vma = 0;
        uint64_t sfence_w_inval = 0;
        uint64_t sfence_inval_ir = 0;
        uint64_t cbo_clean = 0;
        uint64_t cbo_flush = 0;
        uint64_t cbo_inval = 0;
        uint64_t blocks_processed = 0;   // CBO.* 实际处理的 Cache 块数
    };
    CpuInstrStats cpu_stats;
    void print_cpu_instr_stats();
};

#endif
