#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"
#include <cstdio>
#include <iostream>
using namespace std;

void RP_Module::send_translation_request_1_thread()
{
    while (true)
    {
        wait(10, SC_NS);
        wait(10, SC_NS);

        device_context_t DC;
        spte_t pte;
        hb_to_iommu_req_t req;
        iommu_to_hb_rsp_t rsp;

        printf("\n========== IOMMU Single-Request Single-Level Test ==========\n");

        // 检查 IOMMU 模式
        ddtp_t ddtp_check;
        ddtp_check.raw = read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4);
        printf("[DEBUG] Current IOMMU mode before enable_iommu: %d\n", ddtp_check.iommu_mode);

        // 使能 IOMMU，分配并初始化 DDT 根表 (1级DDT)
        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));

        // 重置 next_free_page，避免与 DDT 冲突
        extern uint64_t next_free_page;
        next_free_page = 10;

        printf("[DEBUG] IOMMU mode after enable_iommu: %d (expect 2 for DDT_1LVL)\n",
               read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4) & 0x7);

        // ========== 配置单个设备: iohgatp=Bare, iosatp=Sv39 (只做第一级S-stage翻译) ==========
        printf("\n========== Config Device (device_id=0x05, Bare+Sv39) ==========\n");
        uint64_t DC_addr = add_device(iommu_ptr, 0x05, 1, 0, 0, 0, 0, 0,
                                      1, 1, 0, 0, 0,
                                      IOHGATP_Bare, IOSATP_Sv39, PDTP_Bare,
                                      MSIPTP_Off, 0, 0, 0);

        // 从 DDR 读回 DC 确认
        read_memory_test_rp(DC_addr, sizeof(device_context_t), (char*)&DC);
        printf("[DEV] DC addr: 0x%lx, iohgatp.MODE=%d (Bare), iosatp.MODE=%d (Sv39), iosatp.PPN=0x%lx\n",
               DC_addr, DC.iohgatp.MODE, DC.fsc.iosatp.MODE, DC.fsc.iosatp.PPN);

        // 重置页分配起点，避免冲突
        next_free_page = 20;

        // ========== 设置 S-stage 页表 (Sv39) ==========
        printf("\n[DEV] Setting up S-stage page table (Sv39)...\n");
        pte.raw = 0;
        pte.V = 1;
        pte.R = 1;
        pte.W = 1;
        pte.X = 0;
        pte.U = 1;
        pte.G = 0;
        pte.A = 0;
        pte.D = 0;
        pte.PBMT = PMA;

        // 映射：IOVA 0xA000 -> PA 0x12000
        uint64_t test_iova = 0xA000;
        uint64_t test_pa   = 0x12000;
        pte.PPN = test_pa / PAGESIZE;

        uint64_t pte_addr = add_s_stage_pte(DC.fsc.iosatp, test_iova, pte, 0, 0);
        if (pte_addr == (uint64_t)-1) {
            printf("[DEV] ERROR: add_s_stage_pte failed!\n");
            return;
        }
        printf("[DEV] Added S-stage PTE at addr 0x%lx, IOVA 0x%lx -> PA 0x%lx (PPN=0x%lx)\n",
               pte_addr, test_iova, test_pa, pte.PPN);

        // 刷新缓存使页表生效
        printf("\n[TEST] Invalidating IOMMU caches...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x05, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // ========== 下发单个翻译请求 ==========
        printf("\n========== Single Translation Request ==========\n");
        printf("[TEST] Starting translation: device_id=0x05, IOVA=0x%lx (expect PA=0x%lx via Sv39)\n",
               test_iova, test_pa);
        fflush(stdout);

        send_translation_request_rp(iommu_ptr, 0x05,
                                    0,    // pid_valid
                                    0,    // process_id
                                    0,    // no_write
                                    0,    // exec_req
                                    0,    // priv_req
                                    0,    // is_cxl_dev
                                    0,    // at = ADDR_TYPE_UNTRANSLATED
                                    test_iova,
                                    16,   // length
                                    READ, // read_writeAMO
                                    &req, &rsp);

        printf("[TEST] Request returned, status=0x%x\n", rsp.status);
        if (rsp.status == SUCCESS) {
            printf("[TEST] Translation SUCCESS! PA=0x%lx\n", rsp.trsp.PPN * PAGESIZE);
        } else {
            printf("[TEST] Translation FAILED with status=0x%x\n", rsp.status);
        }

        printf("\n[TEST] Single-request test completed!\n");

        // ============================================================
        // ========== Two-Stage Translation Test (Sv39 + Sv39x4) ==========
        // ============================================================
        printf("\n========== Two-Stage Translation Test ==========\n");

        // Reset page allocators to avoid conflict with first test
        next_free_page = 100;
        next_free_gpage[1] = 50;

        // Configure device with iohgatp=Sv39x4, iosatp=Sv39
        uint64_t dc2_addr = add_device(iommu_ptr, 0x06, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Sv39x4, IOSATP_Sv39, PDTP_Bare,
                                       MSIPTP_Off, 0, 0, 0);

        read_memory_test_rp(dc2_addr, sizeof(device_context_t), (char*)&DC);
        printf("[DEV2] DC addr: 0x%lx, iohgatp.MODE=%d (Sv39x4), iosatp.MODE=%d (Sv39), iosatp.PPN=0x%lx\n",
               dc2_addr, DC.iohgatp.MODE, DC.fsc.iosatp.MODE, DC.fsc.iosatp.PPN);

        next_free_page = 120;

        // Setup VS-stage page table: IOVA -> GPA
        pte.raw = 0;
        pte.V = 1;
        pte.R = 1;
        pte.W = 1;
        pte.X = 0;
        pte.U = 1;
        pte.G = 0;
        pte.A = 0;
        pte.D = 0;
        pte.PBMT = PMA;

        uint64_t test_iova2 = 0xB000;
        uint64_t test_gpa2  = 0x19000;  // GPA = 0x19000 (GPPN=0x19=25)
        uint64_t test_spa2  = 0x32000;  // SPA = 0x32000 (PPN=0x32=50)

        pte.PPN = test_gpa2 / PAGESIZE;  // VS-stage leaf PTE points to GPA

        uint64_t vs_pte_addr = add_vs_stage_pte(iommu_ptr, DC.fsc.iosatp, test_iova2, pte, 0,
                                                DC.iohgatp, DC.tc.SXL);
        if (vs_pte_addr == (uint64_t)-1) {
            printf("[DEV2] ERROR: add_vs_stage_pte failed!\n");
            return;
        }
        printf("[DEV2] Added VS-stage PTE at addr 0x%lx, IOVA 0x%lx -> GPA 0x%lx (PPN=0x%lx)\n",
               vs_pte_addr, test_iova2, test_gpa2, pte.PPN);

        // Setup G-stage page table: GPA -> SPA
        gpte_t gpte;
        gpte.raw = 0;
        gpte.V = 1;
        gpte.R = 1;
        gpte.W = 1;
        gpte.X = 0;
        gpte.U = 1;
        gpte.G = 0;
        gpte.A = 1;
        gpte.D = 1;
        gpte.PBMT = PMA;
        gpte.PPN = test_spa2 / PAGESIZE;

        uint64_t gs_pte_addr = add_g_stage_pte(iommu_ptr, DC.iohgatp, test_gpa2, gpte, 0);
        if (gs_pte_addr == (uint64_t)-1) {
            printf("[DEV2] ERROR: add_g_stage_pte failed!\n");
            return;
        }
        printf("[DEV2] Added G-stage PTE at addr 0x%lx, GPA 0x%lx -> SPA 0x%lx (PPN=0x%lx)\n",
               gs_pte_addr, test_gpa2, test_spa2, gpte.PPN);

        // Verify expected SPA using pure functional model
        uint64_t expected_spa = 0;
        int64_t gpte_lookup_addr = translate_gpa(iommu_ptr, DC.iohgatp, test_gpa2, &expected_spa);
        printf("[DEV2] Pure functional model: translate_gpa(GPA=0x%lx) -> SPA=0x%lx, PTE_addr=0x%lx\n",
               test_gpa2, expected_spa, gpte_lookup_addr);

        // Invalidate caches for two-stage test
        printf("\n[TEST] Invalidating IOMMU caches for two-stage test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x06, 0);
        iotinval(iommu_ptr, VMA, 1, 1, 0, 1, 0, 0);

        // Send two-stage translation request
        printf("\n========== Two-Stage Translation Request ==========\n");
        printf("[TEST] Starting two-stage translation: device_id=0x06, IOVA=0x%lx\n", test_iova2);
        printf("[TEST] Expected flow: IOVA 0x%lx -> VS-stage -> GPA 0x%lx -> G-stage -> SPA 0x%lx\n",
               test_iova2, test_gpa2, test_spa2);
        fflush(stdout);

        send_translation_request_rp(iommu_ptr, 0x06,
                                    0,    // pid_valid
                                    0,    // process_id
                                    0,    // no_write
                                    0,    // exec_req
                                    0,    // priv_req
                                    0,    // is_cxl_dev
                                    0,    // at = ADDR_TYPE_UNTRANSLATED
                                    test_iova2,
                                    16,   // length
                                    READ, // read_writeAMO
                                    &req, &rsp);

        printf("[TEST] Request returned, status=0x%x\n", rsp.status);
        if (rsp.status == SUCCESS) {
            uint64_t result_pa = rsp.trsp.PPN * PAGESIZE;
            printf("[TEST] Translation SUCCESS! PA=0x%lx\n", result_pa);
            if (result_pa == expected_spa) {
                printf("[TEST] PASS: Performance model PA matches pure functional model expected SPA!\n");
            } else {
                printf("[TEST] FAIL: Result PA=0x%lx != expected SPA=0x%lx\n",
                       result_pa, expected_spa);
            }
        } else {
            printf("[TEST] Translation FAILED with status=0x%x\n", rsp.status);
        }

        printf("\n[TEST] Two-stage translation test completed!\n");

        // ============================================================
        // ========== Concurrent Two-Stage Translation Test ==========
        // ============================================================
        printf("\n========== Concurrent Two-Stage Translation Test ==========\n");

        // Reset page allocators
        next_free_page = 200;
        next_free_gpage[1] = 100;

        // Configure device 0x07 with iohgatp=Sv39x4, iosatp=Sv39
        uint64_t dc3_addr = add_device(iommu_ptr, 0x07, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Sv39x4, IOSATP_Sv39, PDTP_Bare,
                                       MSIPTP_Off, 0, 0, 0);
        read_memory_test_rp(dc3_addr, sizeof(device_context_t), (char*)&DC);
        printf("[DEV3] DC addr: 0x%lx, iohgatp.MODE=%d (Sv39x4), iosatp.MODE=%d (Sv39)\n",
               dc3_addr, DC.iohgatp.MODE, DC.fsc.iosatp.MODE);

        // Configure device 0x08 with iohgatp=Sv39x4, iosatp=Sv39
        uint64_t dc4_addr = add_device(iommu_ptr, 0x08, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Sv39x4, IOSATP_Sv39, PDTP_Bare,
                                       MSIPTP_Off, 0, 0, 0);
        device_context_t DC2;
        read_memory_test_rp(dc4_addr, sizeof(device_context_t), (char*)&DC2);
        printf("[DEV4] DC addr: 0x%lx, iohgatp.MODE=%d (Sv39x4), iosatp.MODE=%d (Sv39)\n",
               dc4_addr, DC2.iohgatp.MODE, DC2.fsc.iosatp.MODE);

        next_free_page = 220;

        // Setup VS-stage for device 0x07: IOVA 0xC000 -> GPA 0x21000
        pte.raw = 0;
        pte.V = 1;
        pte.R = 1;
        pte.W = 1;
        pte.X = 0;
        pte.U = 1;
        pte.G = 0;
        pte.A = 0;
        pte.D = 0;
        pte.PBMT = PMA;

        uint64_t test_iova3 = 0xC000;
        uint64_t test_gpa3  = 0x21000;
        uint64_t test_spa3  = 0x42000;

        pte.PPN = test_gpa3 / PAGESIZE;
        uint64_t vs_pte_addr3 = add_vs_stage_pte(iommu_ptr, DC.fsc.iosatp, test_iova3, pte, 0,
                                                   DC.iohgatp, DC.tc.SXL);
        if (vs_pte_addr3 == (uint64_t)-1) {
            printf("[DEV3] ERROR: add_vs_stage_pte failed!\n");
            return;
        }
        printf("[DEV3] Added VS-stage PTE at addr 0x%lx, IOVA 0x%lx -> GPA 0x%lx (PPN=0x%lx)\n",
               vs_pte_addr3, test_iova3, test_gpa3, pte.PPN);

        // Setup G-stage for device 0x07: GPA 0x21000 -> SPA 0x42000
        gpte_t gpte3;
        gpte3.raw = 0;
        gpte3.V = 1;
        gpte3.R = 1;
        gpte3.W = 1;
        gpte3.X = 0;
        gpte3.U = 1;
        gpte3.G = 0;
        gpte3.A = 1;
        gpte3.D = 1;
        gpte3.PBMT = PMA;
        gpte3.PPN = test_spa3 / PAGESIZE;

        uint64_t gs_pte_addr3 = add_g_stage_pte(iommu_ptr, DC.iohgatp, test_gpa3, gpte3, 0);
        if (gs_pte_addr3 == (uint64_t)-1) {
            printf("[DEV3] ERROR: add_g_stage_pte failed!\n");
            return;
        }
        printf("[DEV3] Added G-stage PTE at addr 0x%lx, GPA 0x%lx -> SPA 0x%lx (PPN=0x%lx)\n",
               gs_pte_addr3, test_gpa3, test_spa3, gpte3.PPN);

        // Setup VS-stage for device 0x08: IOVA 0xD000 -> GPA 0x23000
        spte_t pte4;
        pte4.raw = 0;
        pte4.V = 1;
        pte4.R = 1;
        pte4.W = 1;
        pte4.X = 0;
        pte4.U = 1;
        pte4.G = 0;
        pte4.A = 0;
        pte4.D = 0;
        pte4.PBMT = PMA;

        uint64_t test_iova4 = 0xD000;
        uint64_t test_gpa4  = 0x23000;
        uint64_t test_spa4  = 0x44000;

        pte4.PPN = test_gpa4 / PAGESIZE;
        uint64_t vs_pte_addr4 = add_vs_stage_pte(iommu_ptr, DC2.fsc.iosatp, test_iova4, pte4, 0,
                                                   DC2.iohgatp, DC2.tc.SXL);
        if (vs_pte_addr4 == (uint64_t)-1) {
            printf("[DEV4] ERROR: add_vs_stage_pte failed!\n");
            return;
        }
        printf("[DEV4] Added VS-stage PTE at addr 0x%lx, IOVA 0x%lx -> GPA 0x%lx (PPN=0x%lx)\n",
               vs_pte_addr4, test_iova4, test_gpa4, pte4.PPN);

        // Setup G-stage for device 0x08: GPA 0x23000 -> SPA 0x44000
        gpte_t gpte4;
        gpte4.raw = 0;
        gpte4.V = 1;
        gpte4.R = 1;
        gpte4.W = 1;
        gpte4.X = 0;
        gpte4.U = 1;
        gpte4.G = 0;
        gpte4.A = 1;
        gpte4.D = 1;
        gpte4.PBMT = PMA;
        gpte4.PPN = test_spa4 / PAGESIZE;

        uint64_t gs_pte_addr4 = add_g_stage_pte(iommu_ptr, DC2.iohgatp, test_gpa4, gpte4, 0);
        if (gs_pte_addr4 == (uint64_t)-1) {
            printf("[DEV4] ERROR: add_g_stage_pte failed!\n");
            return;
        }
        printf("[DEV4] Added G-stage PTE at addr 0x%lx, GPA 0x%lx -> SPA 0x%lx (PPN=0x%lx)\n",
               gs_pte_addr4, test_gpa4, test_spa4, gpte4.PPN);

        // Verify expected SPA using pure functional model
        uint64_t expected_spa3 = 0;
        uint64_t expected_spa4 = 0;
        int64_t gpte_lookup_addr3 = translate_gpa(iommu_ptr, DC.iohgatp, test_gpa3, &expected_spa3);
        int64_t gpte_lookup_addr4 = translate_gpa(iommu_ptr, DC2.iohgatp, test_gpa4, &expected_spa4);
        printf("[DEV3] Pure functional model: translate_gpa(GPA=0x%lx) -> SPA=0x%lx, PTE_addr=0x%lx\n",
               test_gpa3, expected_spa3, gpte_lookup_addr3);
        printf("[DEV4] Pure functional model: translate_gpa(GPA=0x%lx) -> SPA=0x%lx, PTE_addr=0x%lx\n",
               test_gpa4, expected_spa4, gpte_lookup_addr4);

        // Invalidate caches for concurrent test
        printf("\n[TEST] Invalidating IOMMU caches for concurrent test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x07, 0);
        iodir(iommu_ptr, INVAL_DDT, 1, 0x08, 0);
        iotinval(iommu_ptr, VMA, 1, 1, 0, 1, 0, 0);

        printf("\n========== Triggering Concurrent Translation Requests ==========\n");
        printf("[TEST] Request 1: device_id=0x07, IOVA=0x%lx (expect SPA=0x%lx)\n", test_iova3, expected_spa3);
        printf("[TEST] Request 2: device_id=0x08, IOVA=0x%lx (expect SPA=0x%lx)\n", test_iova4, expected_spa4);
        fflush(stdout);

        // Notify concurrent threads to start
        concurrent_test_event.notify(SC_ZERO_TIME);

        // Wait for both threads to complete (give enough simulation time)
        wait(500, SC_NS);

        printf("\n[TEST] Concurrent two-stage translation test completed!\n");

        // ============================================================
        // ========== Megapage (2MB) Test - Page Offset Verification ==========
        // ============================================================
        printf("\n========== Megapage (2MB) Translation Test ==========\n");

        // Use next_free_page < 256 to stay within 1MB DDR (1024*1024 = 0x100000)
        next_free_page = 230;

        // Configure device 0x09 with iohgatp=Bare, iosatp=Sv39
        uint64_t dc5_addr = add_device(iommu_ptr, 0x09, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Bare, IOSATP_Sv39, PDTP_Bare,
                                       MSIPTP_Off, 0, 0, 0);
        read_memory_test_rp(dc5_addr, sizeof(device_context_t), (char*)&DC);
        printf("[DEV5] DC addr: 0x%lx, iohgatp.MODE=%d (Bare), iosatp.MODE=%d (Sv39)\n",
               dc5_addr, DC.iohgatp.MODE, DC.fsc.iosatp.MODE);

        next_free_page = 232;

        // Setup 2MB megapage at level=1: IOVA 0x201234 -> PA 0x401234
        // IOVA 0x201234 is in VPN[1]=1 region (0x200000-0x3FFFFF)
        // PPN=0x400 has PPN[0]=0 (aligned for 2MB superpage)
        pte.raw = 0;
        pte.V = 1;
        pte.R = 1;
        pte.W = 1;
        pte.X = 0;
        pte.U = 1;
        pte.G = 0;
        pte.A = 1;
        pte.D = 1;
        pte.PBMT = PMA;
        pte.PPN = 0x400;  // 2MB aligned: PA base = 0x400 * 4KB = 0x400000

        uint64_t test_iova5 = 0x201234;  // offset within 2MB page = 0x1234
        uint64_t expected_pa5 = 0x401234; // 0x400000 + 0x1234

        uint64_t mp_pte_addr = add_s_stage_pte(DC.fsc.iosatp, test_iova5, pte, 1, DC.tc.SXL);
        if (mp_pte_addr == (uint64_t)-1) {
            printf("[DEV5] ERROR: add_s_stage_pte (megapage) failed!\n");
            return;
        }
        printf("[DEV5] Added level-1 leaf PTE (megapage) at addr 0x%lx, IOVA 0x%lx -> PA 0x%lx (PPN=0x%lx)\n",
               mp_pte_addr, test_iova5, expected_pa5, pte.PPN);

        // Invalidate caches for megapage test
        printf("\n[TEST] Invalidating IOMMU caches for megapage test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x09, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // Send megapage translation request
        printf("\n========== Megapage Translation Request ==========\n");
        printf("[TEST] Starting megapage translation: device_id=0x09, IOVA=0x%lx (expect PA=0x%lx)\n",
               test_iova5, expected_pa5);
        fflush(stdout);

        send_translation_request_rp(iommu_ptr, 0x09,
                                    0, 0, 0, 0, 0, 0, 0,
                                    test_iova5, 16, READ,
                                    &req, &rsp);

        printf("[TEST] Request returned, status=0x%x\n", rsp.status);
        if (rsp.status == SUCCESS) {
            uint64_t result_pa = rsp.trsp.pa;
            printf("[TEST] Translation SUCCESS! PA=0x%lx\n", result_pa);
            if (result_pa == expected_pa5) {
                printf("[TEST] PASS: Megapage PA matches expected! Page offset 0x%lx correctly preserved.\n",
                       test_iova5 & 0x1FFFFF);
            } else {
                printf("[TEST] FAIL: PA=0x%lx != expected PA=0x%lx\n",
                       result_pa, expected_pa5);
            }
        } else {
            printf("[TEST] Translation FAILED with status=0x%x\n", rsp.status);
        }

        // ============================================================
        // ========== Misaligned Megapage Fault Test ==========
        // ============================================================
        printf("\n========== Misaligned Megapage Fault Test ==========\n");

        // PPN=0x401 has PPN[0]=1 (misaligned for 2MB superpage)
        pte.PPN = 0x401;
        uint64_t test_iova6 = 0x401234;

        uint64_t mis_pte_addr = add_s_stage_pte(DC.fsc.iosatp, test_iova6, pte, 1, DC.tc.SXL);
        if (mis_pte_addr == (uint64_t)-1) {
            printf("[DEV5] ERROR: add_s_stage_pte (misaligned) failed!\n");
            return;
        }
        printf("[DEV5] Added misaligned level-1 leaf PTE at addr 0x%lx, PPN=0x%lx (PPN[0]=1)\n",
               mis_pte_addr, pte.PPN);

        // Invalidate caches
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        printf("[TEST] Starting misaligned megapage translation: device_id=0x09, IOVA=0x%lx\n", test_iova6);
        fflush(stdout);

        send_translation_request_rp(iommu_ptr, 0x09,
                                    0, 0, 0, 0, 0, 0, 0,
                                    test_iova6, 16, READ,
                                    &req, &rsp);

        printf("[TEST] Request returned, status=0x%x, PA=0x%lx\n", rsp.status, rsp.trsp.pa);
        // Note: On misaligned fault, forwarder still sends response with status=SUCCESS but PA=0x0
        if (rsp.status == SUCCESS && rsp.trsp.pa == 0x0) {
            printf("[TEST] PASS: Misaligned megapage correctly faulted (PA=0x0)!\n");
        } else if (rsp.status == SUCCESS && rsp.trsp.pa != 0x0) {
            printf("[TEST] FAIL: Misaligned megapage should have faulted but got PA=0x%lx\n",
                   rsp.trsp.pa);
        } else {
            printf("[TEST] PASS: Misaligned megapage fault detected (status=0x%x)!\n", rsp.status);
        }

        printf("\n[TEST] All page offset tests completed!\n");

        // ============================================================
        // ========== 100-Request Concurrent Single-Stage Test ==========
        // ============================================================
        printf("\n========== 100-Request Concurrent Single-Stage Test ==========\n");

        // Reset page allocators (stay within 1MB DDR = 256 pages)
        next_free_page = 240;

        // Configure device 0x0A with iohgatp=Bare, iosatp=Sv39
        uint64_t dc6_addr = add_device(iommu_ptr, 0x0A, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Bare, IOSATP_Sv39, PDTP_Bare,
                                       MSIPTP_Off, 0, 0, 0);
        device_context_t DC6;
        read_memory_test_rp(dc6_addr, sizeof(device_context_t), (char*)&DC6);
        printf("[DEV6] DC addr: 0x%lx, iohgatp.MODE=%d (Bare), iosatp.MODE=%d (Sv39)\n",
               dc6_addr, DC6.iohgatp.MODE, DC6.fsc.iosatp.MODE);

        next_free_page = 245;

        // Setup S-stage page table for 100 requests
        spte_t pte6;
        pte6.raw = 0;
        pte6.V = 1;
        pte6.R = 1;
        pte6.W = 1;
        pte6.X = 0;
        pte6.U = 1;
        pte6.G = 0;
        pte6.A = 0;
        pte6.D = 0;
        pte6.PBMT = PMA;

        // Map 13 pages for 100 requests (8 requests per 4KB page, stride=512B)
        for (int p = 0; p < 13; p++) {
            uint64_t iova_page = 0x10000 + p * 0x1000;
            uint64_t pa_page = 0x20000 + p * 0x1000;
            pte6.PPN = pa_page / PAGESIZE;
            uint64_t pte_addr = add_s_stage_pte(DC6.fsc.iosatp, iova_page, pte6, 0, 0);
            printf("[DEV6] Added S-stage PTE at addr 0x%lx, IOVA 0x%lx -> PA 0x%lx\n", pte_addr, iova_page, pa_page);
        }

        // Invalidate caches
        printf("\n[TEST] Invalidating IOMMU caches for 100-request test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // Reset task ID counter so 100 requests get IDs 1-100
        iommu_ptr->next_task_id = 1;

        // Reset response counter
        response_count = 0;

        // Prepare and send 100 translation requests
        const int NUM_REQUESTS = 100;
        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];
        uint64_t base_iova = 0x10000;

        printf("\n[TEST] Sending %d translation requests with grouped timing (device_id=0x0A)...\n", NUM_REQUESTS);
        printf("[TEST] Strategy: 8 requests per 4KB page, 10ns delay between groups to allow PT cache hit\n");
        fflush(stdout);

        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t iova = base_iova + i * 512;
            uint64_t expected_pa = iova + 0x10000;

            trans_array[i] = new tlm_generic_payload();
            sc_time delay = SC_ZERO_TIME;
            unsigned char* data = new unsigned char[1024]();

            trans_array[i]->set_address(iova);
            trans_array[i]->set_data_ptr(data);
            trans_array[i]->set_data_length(16);
            trans_array[i]->set_command(TLM_READ_COMMAND);

            ext_array[i] = new PayloadExtention();
            ext_array[i]->requester_id = 0x0A;
            ext_array[i]->pid_valid = 0;
            ext_array[i]->process_id = 0;
            ext_array[i]->exec_req = 0;
            ext_array[i]->priv_req = 0;
            ext_array[i]->no_write = 0;
            ext_array[i]->at = 0;
            trans_array[i]->set_extension(ext_array[i]);

            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            tlm::tlm_sync_enum status =
                axi_master_to_pcie_noc_0_socket->nb_transport_fw(*trans_array[i], phase, delay);

            printf("[TEST] Sent request %2d: IOVA=0x%lx (expected PA=0x%lx), nb_status=%d\n",
                   i, iova, expected_pa, status);

            // 每8个请求（同一个4KB页）为一组，组间延迟50ns让PT cache有时间更新
            // PTW需要约20-30ns完成（4次DDR访问），50ns足够
            // 这样第2-8个请求应该能PT cache hit
            if ((i + 1) % 8 == 0 && i < NUM_REQUESTS - 1) {
                wait(50, SC_NS);
            }
        }

        printf("[TEST] All %d requests injected. Waiting for responses...\n", NUM_REQUESTS);
        fflush(stdout);

        // Wait for all responses
        while (response_count < NUM_REQUESTS) {
            wait(response_count_event);
        }

        printf("[TEST] All %d responses received! response_count=%d\n", NUM_REQUESTS, response_count);

        // Validate responses
        int pass_count = 0;
        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t iova = base_iova + i * 512;
            uint64_t expected_pa = iova + 0x10000;
            uint64_t result_pa = trans_array[i]->get_address();
            tlm::tlm_response_status status = trans_array[i]->get_response_status();

            if (status == tlm::TLM_OK_RESPONSE && result_pa == expected_pa) {
                pass_count++;
            } else {
                printf("[TEST] FAIL request %2d: IOVA=0x%lx, expected PA=0x%lx, got PA=0x%lx, status=%s\n",
                       i, iova, expected_pa, result_pa,
                       trans_array[i]->get_response_string().c_str());
            }
        }

        printf("\n[TEST] Validation: %d/%d passed\n", pass_count, NUM_REQUESTS);
        if (pass_count == NUM_REQUESTS) {
            printf("[TEST] PASS: All 100 requests translated correctly!\n");
        } else {
            printf("[TEST] FAIL: Some requests failed translation!\n");
        }

        // Cleanup
        for (int i = 0; i < NUM_REQUESTS; i++) {
            trans_array[i]->clear_extension(ext_array[i]);
            delete ext_array[i];
            if (trans_array[i]->get_data_ptr()) {
                delete[] trans_array[i]->get_data_ptr();
            }
            delete trans_array[i];
        }

        printf("\n[TEST] 100-request concurrent test completed!\n");

        // 打印Cache命中率统计信息
        iommu_ptr->print_cache_statistics();

        // 停止仿真
        sc_core::sc_stop();

        return;
    }
}

void RP_Module::send_translation_request_2_thread()
{
    while (true) {
        wait(concurrent_test_event);

        uint64_t test_iova = 0xC000;
        uint64_t expected_spa = 0x42000;

        printf("\n[TEST-THREAD2] Sending translation request: device_id=0x07, IOVA=0x%lx\n", test_iova);
        fflush(stdout);

        // Use b_transport to avoid response_event race condition
        tlm_generic_payload trans;
        sc_time delay = SC_ZERO_TIME;
        unsigned char data[1024] = {0};

        trans.set_address(test_iova);
        trans.set_data_ptr(data);
        trans.set_data_length(16);
        trans.set_command(TLM_READ_COMMAND);

        PayloadExtention* ext = new PayloadExtention();
        ext->requester_id = 0x07;
        ext->pid_valid = 0;
        ext->process_id = 0;
        ext->exec_req = 0;
        ext->priv_req = 0;
        ext->no_write = 0;
        ext->at = 0;
        trans.set_extension(ext);

        axi_master_to_pcie_noc_0_socket->b_transport(trans, delay);

        uint64_t result_pa = trans.get_address();
        tlm::tlm_response_status status = trans.get_response_status();

        printf("[TEST-THREAD2] b_transport returned, status=%s, PA=0x%lx\n",
               trans.get_response_string().c_str(), result_pa);

        if (status == tlm::TLM_OK_RESPONSE) {
            printf("[TEST-THREAD2] Translation SUCCESS! PA=0x%lx\n", result_pa);
            if (result_pa == expected_spa) {
                printf("[TEST-THREAD2] PASS: PA matches expected SPA!\n");
            } else {
                printf("[TEST-THREAD2] FAIL: PA=0x%lx != expected SPA=0x%lx\n",
                       result_pa, expected_spa);
            }
        } else {
            printf("[TEST-THREAD2] Translation FAILED with status=%s\n",
                   trans.get_response_string().c_str());
        }
        return;
    }
}

void RP_Module::send_translation_request_3_thread()
{
    while (true) {
        wait(concurrent_test_event);

        uint64_t test_iova = 0xD000;
        uint64_t expected_spa = 0x44000;

        printf("\n[TEST-THREAD3] Sending translation request: device_id=0x08, IOVA=0x%lx\n", test_iova);
        fflush(stdout);

        // Use b_transport to avoid response_event race condition
        tlm_generic_payload trans;
        sc_time delay = SC_ZERO_TIME;
        unsigned char data[1024] = {0};

        trans.set_address(test_iova);
        trans.set_data_ptr(data);
        trans.set_data_length(16);
        trans.set_command(TLM_READ_COMMAND);

        PayloadExtention* ext = new PayloadExtention();
        ext->requester_id = 0x08;
        ext->pid_valid = 0;
        ext->process_id = 0;
        ext->exec_req = 0;
        ext->priv_req = 0;
        ext->no_write = 0;
        ext->at = 0;
        trans.set_extension(ext);

        axi_master_to_pcie_noc_0_socket->b_transport(trans, delay);

        uint64_t result_pa = trans.get_address();
        tlm::tlm_response_status status = trans.get_response_status();

        printf("[TEST-THREAD3] b_transport returned, status=%s, PA=0x%lx\n",
               trans.get_response_string().c_str(), result_pa);

        if (status == tlm::TLM_OK_RESPONSE) {
            printf("[TEST-THREAD3] Translation SUCCESS! PA=0x%lx\n", result_pa);
            if (result_pa == expected_spa) {
                printf("[TEST-THREAD3] PASS: PA matches expected SPA!\n");
            } else {
                printf("[TEST-THREAD3] FAIL: PA=0x%lx != expected SPA=0x%lx\n",
                       result_pa, expected_spa);
            }
        } else {
            printf("[TEST-THREAD3] Translation FAILED with status=%s\n",
                   trans.get_response_string().c_str());
        }
        return;
    }
}
