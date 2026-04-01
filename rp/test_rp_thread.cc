#include "test_rp.hh"
#include "../iommu/iommu_struct.hh"
#include "../iommu/iommu_registers.hh"
#include "../iommu/iommu_utils.hh"
#include "../iommu/iommu_top.hh"  // 包含完整的iommu_top定义


#include <iostream>
using namespace std;

void RP_Module::send_translation_request_0_thread()
{
    while (true)
    {

    // 等待一段时间让系统初始化
    wait(10, SC_NS);

    uint8_t at, pid_valid, exec_req, priv_req, no_write, PR, PW, AV;
    hb_to_iommu_req_t req;
    iommu_to_hb_rsp_t rsp;     

    cout << "[RP Module] Starting IOMMU tests" << endl;

    // 使用简化的测试逻辑，避免在void函数中使用返回值的宏
    cout << "Test: All inbound transactions disallowed" << endl;
    at = 0;
    pid_valid = 1;
    exec_req = 0;
    priv_req = 0;
    no_write = 1;

    // 执行单个测试而不是循环
    if ( at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST ) {
        send_translation_request_rp(iommu_ptr, 0x012345, pid_valid, 0x99, no_write, exec_req,
                                 priv_req, 0, at, 0xdeadbeef, 16, READ, &req, &rsp);
        int8_t result = check_rsp_and_faults_rp(iommu_ptr, &req, &rsp, UNSUPPORTED_REQUEST, 256, 0);
        if(result < 0) {
            cout << "\033[31mFAIL\033[0m" << endl;
        } else {
            cout << "\033[32mPASS\033[0m" << endl;
        }
    } else {
        send_translation_request_rp(iommu_ptr, 0x012345, pid_valid, 0x99, no_write, exec_req,
                                 priv_req, 0, at, 0xdeadbeef, 16, (no_write ^ 1), &req, &rsp);
        int8_t result = check_rsp_and_faults_rp(iommu_ptr, &req, &rsp, UNSUPPORTED_REQUEST, 256, 0);
        if(result < 0) {
            cout << "\033[31mFAIL\033[0m" << endl;
        } else {
            cout << "\033[32mPASS\033[0m" << endl;
        }
    }

    cout << "[RP Module] Tests completed" << endl;
   }
}

void RP_Module::send_translation_request_1_thread() 
{    
    while (true)
    {
        wait(10, SC_NS);

        // 等待一段时间让系统初始化
        wait(10, SC_NS);        

    capabilities_t cap = {0};
    uint32_t offset;
    fctl_t fctl = {0};
    uint8_t at, pid_valid, exec_req, priv_req, no_write, PR, PW, AV;
    uint32_t i, j, test_num = 0;
    uint64_t DC_addr, exp_iotval2, iofence_PPN, iofence_data, spa, gpa;
    uint64_t gva, gpte_addr, pte_addr, PC_addr, temp;
    uint64_t sv57_bare_sz, sv48_bare_sz, sv39_bare_sz, sv32_bare_sz;
    uint64_t sv57x4_bare_sz, sv48x4_bare_sz, sv39x4_bare_sz, sv32x4_bare_sz;
    uint64_t g_pg_sz, vs_pg_sz, exp_trn_sz, exp_pa;
    volatile uint64_t temp1;
    device_context_t DC;
    process_context_t PC;
    ddte_t ddte;
    ddtp_t ddtp;
    gpte_t gpte;
    spte_t  pte;
    fqcsr_t fqcsr;
    cqcsr_t cqcsr;
    pqcsr_t pqcsr;
    cqb_t cqb;
    cqt_t cqt;
    cqh_t cqh;
    command_t cmd;
    hb_to_iommu_req_t req;
    iommu_to_hb_rsp_t rsp;
    tr_req_iova_t tr_req_iova;
    tr_req_ctrl_t tr_req_ctrl;
    tr_response_t tr_response;
    iohpmevt_t event;
    pdte_t pdte;
    ats_msg_t pr;
    ats_msg_t inv_cc;
    pqb_t pqb;
    ipsr_t ipsr;
    fqb_t fqb;
    fqh_t fqh;
    fault_rec_t fault_rec;
   
    printf("Device context invalid");

    // Add a device 0x012345 to guest with GSCID=1
    DC_addr = add_device(iommu_ptr, 0x012345, 1, 0, 0, 0, 0, 0,
                         1, 1, 0, 0, 0,
                         IOHGATP_Sv48x4, IOSATP_Bare, PDTP_Bare,
                         MSIPTP_Flat, 1, 0xFFFFFFFFFF, 0x1000000000);
    fail_if( ( enable_iommu(iommu_ptr, DDT_1LVL) < 0 ) );    

    printf("G-stage translation sizes");
    req.device_id = 0x012345;
    req.pid_valid = 0;
    req.is_cxl_dev = 0;
    req.tr.at = ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST;
    req.tr.length = 64;
    req.tr.read_writeAMO = WRITE;
    gpte.raw = 0;
    gpte.V = 1;
    gpte.R = 1;
    gpte.W = 1;
    gpte.X = 1;
    gpte.U = 1;
    gpte.G = 0;
    gpte.A = 0;
    gpte.D = 0;
    gpte.PBMT = PMA;
    DC.tc.SADE = 1;
    DC.tc.GADE = 1;
    for ( j = 0; j < 1; j++ ) {
        if ( j == 2 ) {
            DC.iohgatp.MODE = IOHGATP_Sv57x4;
            gpa = 512UL * 512UL * 512UL * 512UL * PAGESIZE;
            gpa = gpa * 8;
        } else if ( j == 1 ) {
            DC.iohgatp.MODE = IOHGATP_Sv48x4;
            gpa = 512UL * 512UL * 512UL * PAGESIZE;
            gpa = gpa * 4;
        } else {
            DC.iohgatp.MODE = IOHGATP_Sv39x4;
            gpa = 512UL * 512UL * PAGESIZE;
        }
        write_memory_test_rp((char *)&DC, DC_addr, 64);
        iodir(iommu_ptr, INVAL_DDT, 1, 0x012345, 0);
        iotinval(iommu_ptr, GVMA, 1, 0, 0, DC.iohgatp.GSCID, 0, 0);
        for ( i = 0; i < 5; i++ ) {
            if ( (i == 4) && DC.iohgatp.MODE != IOHGATP_Sv57x4 ) continue;
            if ( (i == 3) && DC.iohgatp.MODE != IOHGATP_Sv48x4 &&
                             DC.iohgatp.MODE != IOHGATP_Sv57x4 ) continue;
            gpa = gpa | ((1 << (i * 9)) * PAGESIZE) | 2048;
            req.tr.iova = gpa;
            gpte.PPN = 512UL * 512UL * 512UL * 512UL;
            gpte.PPN |= (1UL << (i * 9UL));
            pte_addr = add_g_stage_pte(iommu_ptr, DC.iohgatp, gpa, gpte, i);
            iommu_translate_iova_rp(iommu_ptr, &req, &rsp);


            // Test for walking past max levels
            if ( i == 0 ) {
                gpte.X = 0;
                gpte.W = 0;
                gpte.R = 0;
                write_memory_test_rp((char *)&gpte, pte_addr, 8);
                iotinval(iommu_ptr, GVMA, 0, 0, 0, 0, 0, 0);
                iommu_translate_iova_rp(iommu_ptr, &req, &rsp);
                fail_if( ( rsp.status != SUCCESS ) );
                fail_if( ( rsp.trsp.R != 0 ) );
                fail_if( ( rsp.trsp.W != 0 ) );
                gpte.X = 1;
                gpte.W = 1;
                gpte.R = 1;
                write_memory_test_rp((char *)&gpte, pte_addr, 8);
                iotinval(iommu_ptr, GVMA, 0, 0, 0, 0, 0, 0);
            }
        }
    }

    while (true)
    {
        wait(10, SC_NS);
    }
    }   


    /*
    iommu_ptr->iommu_inst.reg_file.capabilities.Sv57x4 = 0;
    iommu_ptr->iommu_inst.reg_file.capabilities.Sv48x4 = 0;
    iommu_ptr->iommu_inst.reg_file.capabilities.Sv39x4 = 0;
    iommu_ptr->iommu_inst.reg_file.capabilities.Sv32x4 = 0;
    for ( i = 0; i < 4; i++ ) {
        if ( i == 0 ) iommu_ptr->iommu_inst.reg_file.capabilities.Sv32x4 = 1;
        if ( i == 0 ) iommu_ptr->iommu_inst.reg_file.fctl.gxl = 1;
        if ( i == 0 ) DC.tc.SXL = 1;
        if ( i == 1 ) iommu_ptr->iommu_inst.reg_file.capabilities.Sv39x4 = 1;
        if ( i == 2 ) iommu_ptr->iommu_inst.reg_file.capabilities.Sv48x4 = 1;
        if ( i == 3 ) iommu_ptr->iommu_inst.reg_file.capabilities.Sv57x4 = 1;
        DC.iohgatp.MODE = IOHGATP_Bare;
        write_memory_test_rp((char *)&DC, DC_addr, 64);
        iodir(iommu_ptr, INVAL_DDT, 1, 0x012345, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);
        gpa = 512UL * 512UL * PAGESIZE;
        req.tr.iova = gpa;
        iommu_translate_iova_rp(iommu_ptr, &req, &rsp);

    }
    END_TEST();
    */
    
}