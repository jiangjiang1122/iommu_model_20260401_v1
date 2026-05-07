#ifndef TEST_RP_HH
#define TEST_RP_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include "param_trans_def.hh"
#include "iommu_struct.hh"
#include "../ddr/test_ddr.hh"

// 声明print_cache_statistics函数（在iommu_top.cc中定义）
extern void print_cache_statistics();

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

    // Concurrent test synchronization
    sc_event concurrent_test_event;

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
        SC_THREAD(send_translation_request_2_thread);
        SC_THREAD(send_translation_request_3_thread);
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
    void send_translation_request_2_thread();
    void send_translation_request_3_thread();

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
    void iotinval(iommu_top *iommu,uint8_t f3, uint8_t GV, uint8_t AV, uint8_t PSCV, uint32_t GSCID, uint32_t PSCID, uint64_t address);
    void ats_command(iommu_top *iommu,uint8_t f3, uint8_t DSV, uint8_t PV, uint32_t PID, uint8_t DSEG, uint16_t RID, uint64_t payload) ;
    void generic_any(iommu_top *iommu,command_t cmd);
    void iodir(iommu_top *iommu,uint8_t f3, uint8_t DV, uint32_t DID, uint32_t PID);
    void iofence(iommu_top *iommu,uint8_t f3, uint8_t PR, uint8_t PW, uint8_t AV, uint8_t WSI_bit, uint64_t addr, uint32_t data);
};

#endif
