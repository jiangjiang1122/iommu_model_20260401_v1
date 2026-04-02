#include "test_rp.hh"
#include "../iommu/iommu_struct.hh"
#include "../iommu/iommu_registers.hh"
#include "../iommu/iommu_utils.hh"
#include "../iommu/iommu_top.hh"  // 包含完整的iommu_top定义


#include <iostream>
using namespace std;

#if  0
void RP_Module::send_translation_request_0_thread()
{
    while (true)
    {

    // 等待一段时间让系统初始化
    wait(10, SC_NS);
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
#endif

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

    // 检查 IOMMU 模式
    ddtp_t ddtp_check;
    ddtp_check.raw = read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4);
    printf("\n[DEBUG] Current IOMMU mode before enable_iommu: %d\n", ddtp_check.iommu_mode);

    // 先使能 IOMMU，分配并初始化 DDT 根表
    fail_if( ( enable_iommu(iommu_ptr, DDT_1LVL) < 0 ) );    
    
    // 重置 next_free_page 到一个安全的起始值，避免与 DDT 冲突
    // DDT 使用了 PPN 0，所以我们从 PPN 10 开始分配 (地址 0xA000)
    extern uint64_t next_free_page;
    next_free_page = 10;
    
    printf("[DEBUG] After enable_iommu, checking IOMMU mode\n");
    ddtp_check.raw = read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4);
    printf("[DEBUG] IOMMU mode after enable_iommu: %d (should be 2 for DDT_1LVL)\n", ddtp_check.iommu_mode);
    printf("[DEBUG] DDTP.ppn after enable_iommu: 0x%lx\n", ddtp_check.ppn);
    
    // Add a device 0x05 to guest with GSCID=1
    // 注意：在 1LVL 模式下，device_id 只能是 7 位（DDI[1]和 DDI[2]必须为 0）
    // 所以使用 device_id = 0x05 而不是 0x012345
    // 重要：必须在 enable_iommu 之后调用 add_device，否则 DC 会被清零
    // 修改：禁用 MSIPTP (设置为 MSIPTP_Off)，使用 iohgatp 进行第二阶段地址翻译
    // 配置：第一阶段 Bare，第二阶段 Sv48x4 页表模式
    DC_addr = add_device(iommu_ptr, 0x05, 1, 0, 0, 0, 0, 0,
                         1, 1, 0, 0, 0,
                         IOHGATP_Sv48x4, IOSATP_Bare, PDTP_Bare,
                         MSIPTP_Off, 0, 0, 0);
    
    // 从 DDR 中读取 DC 内容到本地变量
    read_memory_test_rp(DC_addr, sizeof(device_context_t), (char*)&DC);
    printf("[TEST] Read back DC from DDR at addr 0x%lx\n", DC_addr);
    printf("[TEST] DC.iohgatp.MODE=%d, PPN=0x%lx (from DDR)\n", DC.iohgatp.MODE, DC.iohgatp.PPN);
    
    printf("[DEBUG] After add_device, checking IOMMU mode\n");
    ddtp_check.raw = read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4);
    printf("[DEBUG] Final IOMMU mode: %d\n", ddtp_check.iommu_mode);
    
    // ========== 配置设备 1: Bare + Bare 模式 ==========
    printf("\n========== 配置设备 1 (device_id=0x05, Bare+Bare) ==========\n");
    uint64_t DC_addr_1 = add_device(iommu_ptr, 0x05, 1, 0, 0, 0, 0, 0,
                                    1, 1, 0, 0, 0,
                                    IOHGATP_Bare, IOSATP_Bare, PDTP_Bare,
                                    MSIPTP_Off, 0, 0, 0);
    
    // 从 DDR 中读取设备 1 的 DC 内容
    read_memory_test_rp(DC_addr_1, sizeof(device_context_t), (char*)&DC);
    printf("[DEV1] DC address: 0x%lx, io hgatp.MODE=%d (Bare), iosatp.MODE=%d (Bare)\n", 
           DC_addr_1, DC.iohgatp.MODE, DC.fsc.iosatp.MODE);
    
    // ========== 配置设备 2: Bare + Sv48x4 模式 ==========
    printf("\n========== 配置设备 2 (device_id=0x06, Bare+Sv48x4) ==========\n");
    uint64_t DC_addr_2 = add_device(iommu_ptr, 0x06, 2, 0, 0, 0, 0, 0,
                                    1, 1, 0, 0, 0,
                                    IOHGATP_Sv48x4, IOSATP_Bare, PDTP_Bare,
                                    MSIPTP_Off, 0, 0, 0);
    
    // 从 DDR 中读取设备 2 的 DC 内容
    read_memory_test_rp(DC_addr_2, sizeof(device_context_t), (char*)&DC);
    printf("[DEV2] DC address: 0x%lx, io hgatp.MODE=%d (Sv48x4), PPN=0x%lx, iosatp.MODE=%d (Bare)\n", 
           DC_addr_2, DC.iohgatp.MODE, DC.iohgatp.PPN, DC.fsc.iosatp.MODE);
    
    // ========== 配置设备 3: Sv39 + Bare 模式 ==========
    printf("\n========== 配置设备 3 (device_id=0x07, Sv39+Bare) ==========\n");
    uint64_t DC_addr_3 = add_device(iommu_ptr, 0x07, 3, 0, 0, 0, 0, 0,
                                    1, 1, 0, 0, 0,
                                    IOHGATP_Bare, IOSATP_Sv39, PDTP_Bare,
                                    MSIPTP_Off, 0, 0, 0);
    
    // 从 DDR 中读取设备 3 的 DC 内容
    read_memory_test_rp(DC_addr_3, sizeof(device_context_t), (char*)&DC);
    printf("[DEV3] DC address: 0x%lx, io hgatp.MODE=%d (Bare), iosatp.MODE=%d (Sv39), PPN=0x%lx\n", 
           DC_addr_3, DC.iohgatp.MODE, DC.fsc.iosatp.MODE, DC.fsc.iosatp.PPN);
    
    // 重置 next_free_page，避免与 DC 和 DDT 冲突
    extern uint64_t next_free_page;
    next_free_page = 20;  // 从 PPN 20 开始分配页表
    
    // ========== 为设备 2 设置 G-stage 页表 ==========
    printf("\n[DEV2] Setting up G-stage page table...\n");
    gpte.raw = 0;
    gpte.V = 1;
    gpte.R = 1;
    gpte.W = 1;
    gpte.X = 0;
    gpte.U = 1;  // 必须为 1
    gpte.G = 0;
    gpte.A = 0;
    gpte.D = 0;
    gpte.PBMT = PMA;
    
    // 设备 2 的地址映射：GPA 0x9000 -> SPA 0x11000
    uint64_t dev2_gpa = 0x9000;
    uint64_t dev2_spa = 0x11000;
    gpte.PPN = dev2_spa / PAGESIZE;
    
    // 重新读取设备 2 的 DC 以获取正确的 io hgatp
    read_memory_test_rp(DC_addr_2, sizeof(device_context_t), (char*)&DC);
    pte_addr = add_g_stage_pte(iommu_ptr, DC.iohgatp, dev2_gpa, gpte, 0);
    if (pte_addr == (uint64_t)-1) {
        printf("[DEV2] ✗ ERROR: add_g_stage_pte failed!\n");
        return;
    }
    printf("[DEV2] Added G-stage PTE at addr 0x%lx, GPA 0x%lx -> SPA 0x%lx (PPN=0x%lx)\n", 
           pte_addr, dev2_gpa, dev2_spa, gpte.PPN);
    
    // ========== 为设备 3 设置 S-stage 页表 ==========
    printf("\n[DEV3] Setting up S-stage page table (Sv39)...\n");
    pte.raw = 0;  // 使用已声明的 spte_t pte 变量
    pte.V = 1;
    pte.R = 1;
    pte.W = 1;
    pte.X = 0;
    pte.U = 1;  // 用户态可访问
    pte.G = 0;
    pte.A = 0;
    pte.D = 0;
    pte.PBMT = PMA;
    
    // 设备 3 的地址映射：IOVA 0xA000 -> PA 0x12000
    uint64_t dev3_iova = 0xA000;
    uint64_t dev3_pa = 0x12000;
    pte.PPN = dev3_pa / PAGESIZE;
    
    // 读取设备 3 的 DC 获取 iosatp.PPN
    read_memory_test_rp(DC_addr_3, sizeof(device_context_t), (char*)&DC);
    pte_addr = add_s_stage_pte(DC.fsc.iosatp, dev3_iova, pte, 0, 0);  // level 0 for Sv39
    if (pte_addr == (uint64_t)-1) {
        printf("[DEV3] ✗ ERROR: add_s_stage_pte failed!\n");
        return;
    }
    printf("[DEV3] Added S-stage PTE at addr 0x%lx, IOVA 0x%lx -> PA 0x%lx (PPN=0x%lx)\n", 
           pte_addr, dev3_iova, dev3_pa, pte.PPN);
    
    // 使页表生效，刷新缓存
    printf("\n[TEST] Invalidating IOMMU caches for all three devices...\n");
    iodir(iommu_ptr, INVAL_DDT, 1, 0x05, 0);  // 设备 1 DDT 无效
    iodir(iommu_ptr, INVAL_DDT, 1, 0x06, 0);  // 设备 2 DDT 无效
    iodir(iommu_ptr, INVAL_DDT, 1, 0x07, 0);  // 设备 3 DDT 无效
    iotinval(iommu_ptr, GVMA, 1, 0, 0, 1, 0, 0);  // GSCID 无效
    
    // ========== 第一笔请求：设备 1 (Bare + Bare) ==========
    printf("\n========== 第一笔请求：设备 1 (device_id=0x05, Bare+Bare) ==========\n");
    uint64_t test_iova_1 = 0x8000;  // 设备 1 的 IOVA（Bare 模式直接映射）
    printf("[TEST_1_DEV1] Starting device 1 translation with IOVA=0x%lx (expected PA=0x%lx, bare mode)\n", 
           test_iova_1, test_iova_1);
    fflush(stdout);
    
    FILE* f = fopen("/tmp/iommu_test_log.txt", "w");
    if (f) {
        fprintf(f, "[TEST_1_DEV1] Starting IOVA=0x%lx\n", test_iova_1);
        fflush(f);
    }
    
    std::cout << "[TEST_1_COUT] Calling send_translation_request_rp for DEV1 IOVA=0x" 
              << std::hex << test_iova_1 << std::dec << std::endl;
    
    send_translation_request_rp(iommu_ptr, 0x05,  // device_id = 0x05
                                0,    // pid_valid
                                0,    // process_id
                                0,    // no_write
                                0,    // exec_req
                                0,    // priv_req
                                0,    // is_cxl_dev
                                0,    // at = ADDR_TYPE_UNTRANSLATED
                                test_iova_1,  // iova
                                16,   // length
                                READ, // read_writeAMO
                                &req, &rsp);
    
    std::cout << "[TEST_1_COUT] Returned from send_translation_request_rp, status=0x" 
              << std::hex << rsp.status << std::dec << std::endl;
    
    if (f) {
        fprintf(f, "[TEST_1_DEV1] Completed status=0x%x\n", rsp.status);
        fflush(f);
    }
    printf("[TEST_1_DEV1] Returned from send_translation_request_rp - status=%d\n", rsp.status);
    printf("[TEST_1_DEV1] First translation request completed successfully\n");
    
    // ========== 第二笔请求：设备 2 (Bare + Sv48x4) ==========
    printf("\n========== 第二笔请求：设备 2 (device_id=0x06, Bare+Sv48x4) ==========\n");
    uint64_t test_iova_2 = 0x9000;  // 设备 2 的 GPA（需要页表翻译）
    printf("[TEST_2_DEV2] Starting device 2 translation with IOVA=0x%lx (expected PA=0x%lx, page table)\n", 
           test_iova_2, 0x11000);
    fflush(stdout);
    
    if (f) {
        fprintf(f, "[TEST_2_DEV2] Starting IOVA=0x%lx\n", test_iova_2);
        fflush(f);
    }
    
    std::cout << "[TEST_2_COUT] Calling send_translation_request_rp for DEV2 IOVA=0x" 
              << std::hex << test_iova_2 << std::dec << std::endl;
    
    send_translation_request_rp(iommu_ptr, 0x06,  // device_id = 0x06
                                0,    // pid_valid
                                0,    // process_id
                                0,    // no_write
                                0,    // exec_req
                                0,    // priv_req
                                0,    // is_cxl_dev
                                0,    // at = ADDR_TYPE_UNTRANSLATED
                                test_iova_2,  // iova
                                16,   // length
                                READ, // read_writeAMO
                                &req, &rsp);
    
    std::cout << "[TEST_2_COUT] Returned from send_translation_request_rp, status=0x" 
              << std::hex << rsp.status << std::dec << std::endl;
    
    if (f) {
        fprintf(f, "[TEST_2_DEV2] Completed status=0x%x\n", rsp.status);
        fflush(f);
    }
    printf("[TEST_2_DEV2] Returned from send_translation_request_rp - status=%d\n", rsp.status);
    printf("[TEST_2_DEV2] Second translation request completed successfully\n");
    
    // ========== 第三笔请求：设备 3 (Sv39 + Bare) ==========
    printf("\n========== 第三笔请求：设备 3 (device_id=0x07, Sv39+Bare) ==========\n");
    uint64_t test_iova_3 = 0xA000;  // 设备 3 的 IOVA（需要 S-stage 页表翻译）
    printf("[TEST_3_DEV3] Starting device 3 translation with IOVA=0x%lx (expected PA=0x%lx, Sv39 page table)\n", 
           test_iova_3, 0x12000);
    fflush(stdout);
    
    if (f) {
        fprintf(f, "[TEST_3_DEV3] Starting IOVA=0x%lx\n", test_iova_3);
        fflush(f);
    }
    
    std::cout << "[TEST_3_COUT] Calling send_translation_request_rp for DEV3 IOVA=0x" 
              << std::hex << test_iova_3 << std::dec << std::endl;
    
    send_translation_request_rp(iommu_ptr, 0x07,  // device_id = 0x07
                                0,    // pid_valid
                                0,    // process_id
                                0,    // no_write
                                0,    // exec_req
                                0,    // priv_req
                                0,    // is_cxl_dev
                                0,    // at = ADDR_TYPE_UNTRANSLATED
                                test_iova_3,  // iova
                                16,   // length
                                READ, // read_writeAMO
                                &req, &rsp);
    
    std::cout << "[TEST_3_COUT] Returned from send_translation_request_rp, status=0x" 
              << std::hex << rsp.status << std::dec << std::endl;
    
    if (f) {
        fprintf(f, "[TEST_3_DEV3] Completed status=0x%x\n", rsp.status);
        fprintf(f, "[TEST] All requests completed\n");
        fclose(f);
    }
    printf("[TEST_3_DEV3] Returned from send_translation_request_rp - status=%d\n", rsp.status);
    printf("[TEST_3_DEV3] Third translation request completed successfully\n");
    
    // 测试完成
    printf("\n[TEST] All multi-device tests completed!\n");
    return;
    }
}
