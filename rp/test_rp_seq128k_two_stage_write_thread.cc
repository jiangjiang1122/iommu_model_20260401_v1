#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"
#include <cstdio>
#include <iostream>
using namespace std;

// ============================================================
// Test Scenario: 128KB Sequential Write + Two-Stage Address Translation
//   iosatp = Sv48, iohgatp = Sv48x4
//   IOVA range: ~5MB (0x100000 ~ 0x5FFFFF), 1280 pages
//   GPA pattern: 20 unique GPAs, 2MB stride, cycling reuse
//   SPA  = GPA + 0x10000 (fixed offset)
//   10000 sequential WRITE requests, each 512B
//
//   Walker Cache and PT prefetch are ENABLED.
//   Write requests require strict ordering in reorder buffer.
// ============================================================
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

        printf("\n========== IOMMU 128KB Sequential Write - Two-Stage Translation Test (device 0x0A) ==========\n");

        // Check IOMMU mode
        ddtp_t ddtp_check;
        ddtp_check.raw = read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4);
        printf("[DEBUG] Current IOMMU mode before enable_iommu: %d\n", ddtp_check.iommu_mode);

        // Enable IOMMU
        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));

        extern uint64_t next_free_page;
        next_free_page = 240;

        printf("[DEBUG] IOMMU mode after enable_iommu: %d (expect 2 for DDT_1LVL)\n",
               read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4) & 0x7);

        // ============================================================
        // 1MB VS-stage page table + G-stage mapping
        // GPA pattern: 20 unique GPAs, 2MB stride, cycling reuse
        // ============================================================
        const uint64_t IOVA_BASE      = 0x100000;         // 1MB aligned base
        const uint64_t PA_OFFSET      = 0x10000;           // SPA = GPA + 0x10000
        const uint64_t GPA_STRIDE     = 0x200000;          // 2MB per GPA step
        const int      NUM_UNIQUE_GPAS = 20;               // 20 different GPAs
        const int      NUM_REQUESTS    = 10000;            // 10000 sequential requests
        // IOVA range: NUM_REQUESTS * 512B = 5MB -> 1280 pages
        const uint64_t IOVA_RANGE      = (uint64_t)NUM_REQUESTS * 0x200;  // 5MB
        const int      TOTAL_IOVA_PAGES = (int)(IOVA_RANGE / 0x1000);     // 1280 pages
        // GPA range: 20 unique GPAs * 2MB = 40MB
        const uint64_t GPA_RANGE       = (uint64_t)NUM_UNIQUE_GPAS * GPA_STRIDE; // 40MB
        const int      TOTAL_GPA_PAGES = (int)(GPA_RANGE / 0x1000);       // 10240 G-stage pages

        printf("\n[TEST] Two-Stage Page Table Construction:\n");
        printf("[TEST]   IOVA range: 0x%lx ~ 0x%lx (%luMB, %d pages)\n",
               IOVA_BASE, IOVA_BASE + IOVA_RANGE, (unsigned long)(IOVA_RANGE / 0x100000), TOTAL_IOVA_PAGES);
        printf("[TEST]   GPA = (page_idx %% %d) x 0x%lx (2MB stride, 20 unique GPAs cycling)\n",
               NUM_UNIQUE_GPAS, GPA_STRIDE);
        printf("[TEST]   SPA = GPA + 0x%lx, GPA range: 0 ~ 0x%lx (%d unique GPAs)\n",
               PA_OFFSET, (uint64_t)(NUM_UNIQUE_GPAS - 1) * GPA_STRIDE, NUM_UNIQUE_GPAS);

        // ============================================================
        // 关键修复：在add_device之前将GPPN分配器偏移到测试GPA范围之上
        // 测试GPA的GPPN范围: 0 ~ (NUM_UNIQUE_GPAS-1) * GPA_STRIDE / PAGESIZE
        // 例如: 19 * 0x200000 / 0x1000 = 19 * 512 = 9728
        // ============================================================
        extern uint64_t next_free_gpage[65536];
        uint64_t test_max_gppn = (uint64_t)(NUM_UNIQUE_GPAS - 1) * (GPA_STRIDE / PAGESIZE);
        uint64_t gppn_offset = ((test_max_gppn + 0xFFFF) / 0x10000) * 0x10000;  // 向上对齐到64K
        next_free_gpage[1] = gppn_offset;  // GSCID=1, 必须在add_device之前设置!
        printf("[TEST]   GPPN offset: 0x%lx (test GPPN max=0x%lx)\n", gppn_offset, test_max_gppn);

        // ============================================================
        // Configure device 0x0A: iohgatp=Sv48x4, iosatp=Sv48
        // ============================================================
        printf("\n========== Configuring Device 0x0A: iosatp=Sv48, iohgatp=Sv48x4 ==========\n");

        uint64_t dc6_addr = add_device(iommu_ptr, 0x0A, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Sv48x4, IOSATP_Sv48, PDTP_Bare,
                                       MSIPTP_Off, 0, 0, 0);
        device_context_t DC6;
        read_memory_test_rp(dc6_addr, sizeof(device_context_t), (char*)&DC6);
        printf("[DEV6] DC addr: 0x%lx, iohgatp.MODE=%d (Sv48x4), iosatp.MODE=%d (Sv48)\n",
               dc6_addr, DC6.iohgatp.MODE, DC6.fsc.iosatp.MODE);
        printf("[DEV6] iosatp.PPN=0x%lx -> VS root GPA=0x%lx\n",
               (uint64_t)DC6.fsc.iosatp.PPN, (uint64_t)DC6.fsc.iosatp.PPN * PAGESIZE);

        // Do NOT reset next_free_page here. add_device has already allocated
        // G-stage page table pages; resetting it would collide with those pages.

        // ============================================================
        // 关键修复：为VS root page创建G-stage映射
        // add_device分配了VS root GPPN（高位），但G-stage中还没有该GPA的映射
        // add_vs_stage_pte需要translate_gpa来定位VS页表页，如果G-stage映射不存在会失败
        // ============================================================
        {
            uint64_t vs_root_gpa = (uint64_t)DC6.fsc.iosatp.PPN * PAGESIZE;
            gpte_t vs_root_gpte;
            vs_root_gpte.raw = 0;
            vs_root_gpte.V = 1;
            vs_root_gpte.R = 1;
            vs_root_gpte.W = 1;
            vs_root_gpte.X = 0;
            vs_root_gpte.U = 1;
            vs_root_gpte.G = 0;
            vs_root_gpte.A = 1;
            vs_root_gpte.D = 1;
            vs_root_gpte.PBMT = PMA;
            vs_root_gpte.PPN = get_free_ppn(1);
            // 清零VS root page内存
            unsigned char zero_page[4096] = {0};
            write_memory_test_rp((char*)zero_page, vs_root_gpte.PPN * PAGESIZE, 4096);
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, vs_root_gpa, vs_root_gpte, 0);
            printf("[TEST] Created G-stage mapping: GPA=0x%lx -> SPA=0x%lx (VS root page)\n",
                   vs_root_gpa, (uint64_t)vs_root_gpte.PPN * PAGESIZE);
        }

        // Setup VS-stage page table template (leaf PTE)
        spte_t pte6;
        pte6.raw = 0;
        pte6.V = 1;
        pte6.R = 1;
        pte6.W = 1;
        pte6.X = 0;
        pte6.U = 1;
        pte6.G = 0;
        pte6.A = 1;
        pte6.D = 1;
        pte6.PBMT = PMA;

        // Setup G-stage leaf PTE template (maps GPA -> SPA)
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

        // ============================================================
        // Map VS-stage (IOVA→GPA) + G-stage (GPA→SPA)
        // VS-stage: 1280 IOVA pages, GPA cycles through 20 unique values
        // G-stage: only 20 unique GPA→SPA mappings needed
        // ============================================================

        // First: create G-stage mappings for all 20 unique GPAs
        for (int g = 0; g < NUM_UNIQUE_GPAS; g++) {
            uint64_t gpa_page = (uint64_t)g * GPA_STRIDE;    // unique GPA
            uint64_t pa_page  = gpa_page + PA_OFFSET;         // SPA

            gpte.PPN = pa_page / PAGESIZE;
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa_page, gpte, 0);
        }
        printf("[TEST]   Mapped %d unique G-stage entries (GPA 0~0x%lx -> SPA)\n",
               NUM_UNIQUE_GPAS, (uint64_t)(NUM_UNIQUE_GPAS - 1) * GPA_STRIDE);

        // Then: create VS-stage mappings for all IOVA pages (GPA cycling)
        for (int p = 0; p < TOTAL_IOVA_PAGES; p++) {
            uint64_t iova_page = IOVA_BASE + (uint64_t)p * 0x1000;
            int gpa_idx        = p % NUM_UNIQUE_GPAS;           // cycle through 20 GPAs
            uint64_t gpa_page  = (uint64_t)gpa_idx * GPA_STRIDE;

            pte6.PPN = gpa_page / PAGESIZE;
            add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, iova_page, pte6, 0, DC6.iohgatp, 0);
        }
        printf("[TEST]   Mapped %d VS-stage pages (IOVA -> cycling 20 GPAs)\n",
               TOTAL_IOVA_PAGES);

        // Invalidate caches
        printf("\n[TEST] Invalidating IOMMU caches for two-stage test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // Reset task ID counter
        iommu_ptr->next_task_id = 1;

        // ============================================================
        // Phase 1: Single packet injection sanity check
        // ============================================================
        printf("\n========== Phase 1: Single Packet Two-Stage Translation ==========\n");

        response_count = 0;
        uint64_t single_iova = IOVA_BASE;
        // Phase 1: IOVA=IOVA_BASE=0x100000, page 0, GPA=(0%20)*2MB=0, SPA=0+PA_OFFSET=0x10000
        uint64_t single_page_idx = (single_iova - IOVA_BASE) / 0x1000;
        int single_gpa_idx = single_page_idx % NUM_UNIQUE_GPAS;
        uint64_t single_expected_pa = ((uint64_t)single_gpa_idx * GPA_STRIDE) + PA_OFFSET + (single_iova & 0xFFF);

        tlm_generic_payload single_trans;
        sc_time single_delay = SC_ZERO_TIME;
        unsigned char single_data[1024] = {0};

        single_trans.set_address(single_iova);
        single_trans.set_data_ptr(single_data);
        single_trans.set_data_length(512);
        single_trans.set_command(TLM_WRITE_COMMAND);

        PayloadExtention single_ext;
        single_ext.requester_id = 0x0A;
        single_ext.pid_valid = 0;
        single_ext.process_id = 0;
        single_ext.exec_req = 0;
        single_ext.priv_req = 0;
        single_ext.no_write = 0;
        single_ext.at = 0;
        single_trans.set_extension(&single_ext);

        tlm::tlm_phase single_phase = tlm::BEGIN_REQ;
        tlm::tlm_sync_enum single_status =
            axi_master_to_pcie_noc_0_socket->nb_transport_fw(single_trans, single_phase, single_delay);

        printf("[TEST] Single packet injected: IOVA=0x%lx, expected PA=0x%lx\n",
               single_iova, single_expected_pa);

        // Wait for response
        int single_wait = 0;
        while (response_count < 1) {
            wait(1000, SC_NS);
            single_wait++;
            if (single_wait > 200) {
                printf("[TEST] ERROR: Single packet response timeout\n");
                break;
            }
        }

        uint64_t single_result_pa = single_trans.get_address();
        tlm::tlm_response_status single_rsp_status = single_trans.get_response_status();

        printf("[TEST] Single packet response: status=%s, PA=0x%lx\n",
               single_trans.get_response_string().c_str(), single_result_pa);
        printf("[TEST] Single packet PTW total DDR reads: %lu\n",
               (unsigned long)iommu_ptr->ptw_total_ddr_reads);

        if (single_rsp_status == tlm::TLM_OK_RESPONSE && single_result_pa == single_expected_pa) {
            printf("[TEST] PASS: Single packet two-stage translation correct!\n");
        } else {
            printf("[TEST] FAIL: Single packet expected PA=0x%lx, got PA=0x%lx\n",
                   single_expected_pa, single_result_pa);
        }

        single_trans.clear_extension(&single_ext);

        // ============================================================
        // Phase 2: Full 128KB sequential read test
        // ============================================================
        printf("\n========== Phase 2: Full 128KB Sequential Write (%d requests) ==========\n", NUM_REQUESTS);

        // Reset counters for the full run
        response_count = 0;
        iommu_ptr->next_task_id = 2;  // single packet used task_id 1
        iommu_ptr->steady_start_count = 256;
        iommu_ptr->steady_end_count   = NUM_REQUESTS - 256;

        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];

        printf("[TEST] Sending %d WRITE requests (sequential 512B stride)...\n", NUM_REQUESTS);
        printf("[TEST] Prefetch D=%d, Walker Cache ENABLED\n",
               (int)TEST_CFG_PT_DEDUP_PREFETCH_DEPTH);
        fflush(stdout);

        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t iova = IOVA_BASE + (uint64_t)(i + 1) * 0x200;  // 512B stride, +1跳过Phase 1已用的IOVA
            uint64_t p2_page_idx = (iova - IOVA_BASE) / 0x1000;
            int p2_gpa_idx = p2_page_idx % NUM_UNIQUE_GPAS;
            uint64_t expected_pa = ((uint64_t)p2_gpa_idx * GPA_STRIDE) + PA_OFFSET + (iova & 0xFFF);

            trans_array[i] = new tlm_generic_payload();
            sc_time delay = SC_ZERO_TIME;
            unsigned char* data = new unsigned char[1024]();

            trans_array[i]->set_address(iova);
            trans_array[i]->set_data_ptr(data);
            trans_array[i]->set_data_length(512);
            trans_array[i]->set_command(TLM_WRITE_COMMAND);

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

            if ((i + 1) % 100 == 0) {
                printf("[TEST] Progress: %d/%d requests injected (IOVA=0x%lx)\n",
                       i + 1, NUM_REQUESTS, iova);
                fflush(stdout);
            }
        }

        printf("[TEST] All %d requests injected. Waiting for responses...\n", NUM_REQUESTS);
        fflush(stdout);

        // Wait for all responses with timeout
        int stall_count = 0;
        int last_response_count = response_count;
        while (response_count < NUM_REQUESTS) {
            wait(1000, SC_NS);
            if (response_count == last_response_count) {
                stall_count++;
                if (stall_count > 50) {
                    printf("[TEST] WARNING: No progress for 50us, breaking wait loop. Got %d/%d responses\n",
                           response_count, NUM_REQUESTS);
                    break;
                }
            } else {
                stall_count = 0;
                last_response_count = response_count;
            }
            if (response_count % 100 == 0) {
                printf("[TEST] Progress: %d/%d responses received\n", response_count, NUM_REQUESTS);
                fflush(stdout);
            }
        }

        printf("[TEST] All %d responses received! response_count=%d\n", NUM_REQUESTS, response_count);

        // Validate responses
        int pass_count = 0;
        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t iova = IOVA_BASE + (uint64_t)(i + 1) * 0x200;  // 与注入循环保持一致
            // GPA = (page_idx % 20) × 2MB, SPA = GPA + PA_OFFSET
            uint64_t page_idx = (iova - IOVA_BASE) / 0x1000;
            int gpa_idx = page_idx % NUM_UNIQUE_GPAS;
            uint64_t expected_pa = ((uint64_t)gpa_idx * GPA_STRIDE) + PA_OFFSET + (iova & 0xFFF);
            uint64_t result_pa = trans_array[i]->get_address();
            tlm::tlm_response_status status = trans_array[i]->get_response_status();

            if (status == tlm::TLM_OK_RESPONSE && result_pa == expected_pa) {
                pass_count++;
            } else {
                printf("[TEST] FAIL req %4d: IOVA=0x%lx, expected PA=0x%lx, got PA=0x%lx, status=%s\n",
                       i, iova, expected_pa, result_pa,
                       trans_array[i]->get_response_string().c_str());
            }
        }

        printf("\n[TEST] Validation: %d/%d passed\n", pass_count, NUM_REQUESTS);
        if (pass_count == NUM_REQUESTS) {
            printf("[TEST] PASS: All %d two-stage requests translated correctly!\n", NUM_REQUESTS);
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

        printf("\n[TEST] %d-request sequential 128KB two-stage write test completed!\n", NUM_REQUESTS);

        // Print PTW DDR statistics
        printf("\n========== PTW DDR Access Statistics ==========\n");
        printf("  PTW total completed tasks: %lu\n", (unsigned long)iommu_ptr->ptw_total_completed);
        printf("  PTW total DDR reads:       %lu\n", (unsigned long)iommu_ptr->ptw_total_ddr_reads);
        if (iommu_ptr->ptw_total_completed > 0) {
            printf("  PTW avg DDR reads/task:    %.2f\n",
                   (double)iommu_ptr->ptw_total_ddr_reads / iommu_ptr->ptw_total_completed);
        }
        printf("================================================\n");

        // Print Cache hit rate statistics
        iommu_ptr->print_cache_statistics();

        // Stop simulation
        sc_core::sc_stop();

        return;
    }
}

