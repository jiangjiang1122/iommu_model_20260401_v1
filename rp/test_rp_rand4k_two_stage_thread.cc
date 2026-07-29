#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"
#include <cstdio>
#include <iostream>
#include <set>
#include <cstdlib>
#include <ctime>
using namespace std;

// ============================================================
// Test Scenario: 4KB Random Read + Two-Stage Address Translation
//   iosatp = Sv48, iohgatp = Sv48x4
//   IOVA pattern: 500 unique random 4KB pages from 16MB range,
//                 shuffled access order, 8 reqs/page (512B stride)
//   GPA  = loop_index × 0x200000 (2MB stride per IOVA page)
//   SPA  = GPA + 0x10000
//   Total: 500 pages × 8 reqs = 4000 requests
//
//   Key: IOVA-GPA has no fixed spatial relationship (random pages),
//        but GPA-SPA has fixed mapping (2MB stride + offset).
//        Prefetch D=3 expected to be INEFFECTIVE (random IOVA order).
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

        printf("\n========== IOMMU 4KB Random Read - Two-Stage Translation Test (device 0x0A) ==========\n");

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
        // Address layout:
        //   IOVA range: 16MB = 0x1000000 bytes = 4096 pages (4KB each)
        //   IOVA base:  0x100000 (1MB aligned)
        //   Select 500 unique random pages, shuffled access order
        //   Each page gets 8 requests (8 × 512B = 4KB)
        //   Total: 500 × 8 = 4000 requests
        //
        //   GPA = loop_index × 0x200000 (2MB stride)
        //   SPA = GPA + 0x10000
        // ============================================================
        const uint64_t IOVA_BASE   = 0x100000;         // 1MB aligned base
        const uint64_t RANGE_16MB  = 0x1000000;        // 16MB
        const uint64_t PA_OFFSET   = 0x10000;          // SPA = GPA + 0x10000
        const uint64_t GPA_STRIDE  = 0x200000;         // 2MB per page
        // [场景化] 默认625页(5000包); 场景7经Makefile传入TEST_CFG_NUM_PAGES=1250(10000包)
#ifndef TEST_CFG_NUM_PAGES
        const int PAGES_NEEDED     = 625;              // 5000 reqs / 8 per page
#else
        const int PAGES_NEEDED     = TEST_CFG_NUM_PAGES;
#endif
        const int REQ_PER_PAGE     = 8;                // 8 × 512B = 4KB
        const int NUM_REQUESTS     = PAGES_NEEDED * REQ_PER_PAGE;  // 5000 / 10000
        const int TOTAL_PAGES_IN_RANGE = (int)(RANGE_16MB / 0x1000);  // 4096

        printf("\n[TEST] Two-Stage Random 4KB Read Page Table Construction:\n");
        printf("[TEST]   IOVA range: 0x%lx ~ 0x%lx (16MB, %d pages)\n",
               IOVA_BASE, IOVA_BASE + RANGE_16MB, TOTAL_PAGES_IN_RANGE);
        printf("[TEST]   Selecting %d unique random 4KB pages (SHUFFLED access order)\n", PAGES_NEEDED);
        printf("[TEST]   GPA = loop_index × 0x%lx (2MB stride)\n", GPA_STRIDE);
        printf("[TEST]   SPA = GPA + 0x%lx\n", PA_OFFSET);

        // ============================================================
        // GPPN offset: avoid collision with test GPA range
        // Test GPA max = (PAGES_NEEDED-1) × GPA_STRIDE / PAGESIZE
        //              = 499 × 0x200 = 0x3EE00
        // ============================================================
        extern uint64_t next_free_gpage[65536];
        uint64_t test_max_gppn = (uint64_t)(PAGES_NEEDED - 1) * (GPA_STRIDE / PAGESIZE);
        uint64_t gppn_offset = ((test_max_gppn + 0xFFFF) / 0x10000) * 0x10000;
        next_free_gpage[1] = gppn_offset;
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

        // ============================================================
        // Create G-stage mapping for VS root page
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
            unsigned char zero_page[4096] = {0};
            write_memory_test_rp((char*)zero_page, vs_root_gpte.PPN * PAGESIZE, 4096);
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, vs_root_gpa, vs_root_gpte, 0);
            printf("[TEST] Created G-stage mapping: GPA=0x%lx -> SPA=0x%lx (VS root page)\n",
                   vs_root_gpa, (uint64_t)vs_root_gpte.PPN * PAGESIZE);
        }

        // ============================================================
        // Generate 500 unique random page indices (deterministic seed)
        // ============================================================
        srand(42);
        set<int> page_set;
        while ((int)page_set.size() < PAGES_NEEDED) {
            page_set.insert(rand() % TOTAL_PAGES_IN_RANGE);
        }

        vector<int> page_indices(page_set.begin(), page_set.end());

        // Shuffle to simulate random access (prefetch D=3 ineffective)
        srand(123);
        for (int i = page_indices.size() - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            std::swap(page_indices[i], page_indices[j]);
        }

        printf("[TEST]   Generated %d unique random page indices (SHUFFLED)\n", (int)page_indices.size());
        printf("[TEST]   Sample: page[0]=IOVA 0x%lx, page[1]=IOVA 0x%lx, page[624]=IOVA 0x%lx\n",
               IOVA_BASE + (uint64_t)page_indices[0] * 0x1000,
               IOVA_BASE + (uint64_t)page_indices[1] * 0x1000,
               IOVA_BASE + (uint64_t)page_indices[624] * 0x1000);

        // ============================================================
        // Page table templates
        // ============================================================
        // VS-stage leaf PTE (IOVA -> GPA)
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

        // G-stage leaf PTE (GPA -> SPA)
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
        // Map 500 random pages: VS-stage (IOVA->GPA) + G-stage (GPA->SPA)
        //   page_indices[p] -> random page offset in 16MB
        //   GPA = p * GPA_STRIDE (sequential 2MB stride)
        //   SPA = GPA + PA_OFFSET
        // ============================================================
        for (int p = 0; p < PAGES_NEEDED; p++) {
            uint64_t iova_page = IOVA_BASE + (uint64_t)page_indices[p] * 0x1000;
            uint64_t gpa_page  = (uint64_t)p * GPA_STRIDE;       // 2MB stride
            uint64_t pa_page   = gpa_page + PA_OFFSET;

            // VS-stage: IOVA -> GPA
            pte6.PPN = gpa_page / PAGESIZE;
            add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, iova_page, pte6, 0, DC6.iohgatp, 0);

            // G-stage: GPA -> SPA
            gpte.PPN = pa_page / PAGESIZE;
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa_page, gpte, 0);
        }
        printf("[TEST]   Mapped %d VS-stage pages and %d G-stage pages\n", PAGES_NEEDED, PAGES_NEEDED);

        // Invalidate caches
        printf("\n[TEST] Invalidating IOMMU caches for two-stage random test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // Reset task ID counter
        iommu_ptr->next_task_id = 1;

        // ============================================================
        // Full 5000-request two-stage random read test
        //   625 pages × 8 reqs/page = 5000 requests
        //   All pages shuffled, prefetch D=3 expected ineffective
        // ============================================================
        printf("\n========== 5000-Request Two-Stage Random Read Test ==========\n");

        response_count = 0;
        iommu_ptr->steady_start_count = NUM_REQUESTS * STEADY_STATE_START_PERCENT / 100;
        iommu_ptr->steady_end_count   = NUM_REQUESTS * STEADY_STATE_END_PERCENT / 100;

        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];

        printf("[TEST] Sending %d READ requests (random 4KB pages, 8 reqs/page, shuffled)...\n", NUM_REQUESTS);
        printf("[TEST] Prefetch D=%d, Walker Cache ENABLED (expected INEFFECTIVE for random IOVA)\n",
               (int)TEST_CFG_PT_DEDUP_PREFETCH_DEPTH);
        fflush(stdout);

        // Inject all 5000 requests
        int req_idx = 0;
        for (int global_req = 0; global_req < NUM_REQUESTS; global_req++) {
            int page_idx = global_req / REQ_PER_PAGE;
            int offset_in_page = global_req % REQ_PER_PAGE;

            uint64_t iova = IOVA_BASE + (uint64_t)page_indices[page_idx] * 0x1000
                          + offset_in_page * 0x200;
            // Expected PA: GPA = page_idx * GPA_STRIDE, SPA = GPA + PA_OFFSET + page_offset
            uint64_t expected_pa = (uint64_t)page_idx * GPA_STRIDE + PA_OFFSET + (iova & 0xFFF);

            trans_array[req_idx] = new tlm_generic_payload();
            sc_time delay = SC_ZERO_TIME;
            unsigned char* data = new unsigned char[1024]();

            trans_array[req_idx]->set_address(iova);
            trans_array[req_idx]->set_data_ptr(data);
            trans_array[req_idx]->set_data_length(512);
            trans_array[req_idx]->set_command(TLM_READ_COMMAND);

            ext_array[req_idx] = new PayloadExtention();
            ext_array[req_idx]->requester_id = 0x0A;
            ext_array[req_idx]->pid_valid = 0;
            ext_array[req_idx]->process_id = 0;
            ext_array[req_idx]->exec_req = 0;
            ext_array[req_idx]->priv_req = 0;
            ext_array[req_idx]->no_write = 1;
            ext_array[req_idx]->at = 0;
            trans_array[req_idx]->set_extension(ext_array[req_idx]);

            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            tlm::tlm_sync_enum status =
                axi_master_to_pcie_noc_0_socket->nb_transport_fw(*trans_array[req_idx], phase, delay);

            if ((req_idx + 1) % 1000 == 0) {
                printf("[TEST] Progress: %d/%d requests injected (page[%d] IOVA=0x%lx)\n",
                       req_idx + 1, NUM_REQUESTS, page_idx, iova);
                fflush(stdout);
            }
            req_idx++;
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
            if (response_count % 1000 == 0) {
                printf("[TEST] Progress: %d/%d responses received\n", response_count, NUM_REQUESTS);
                fflush(stdout);
            }
        }

        printf("[TEST] All %d responses received! response_count=%d\n", NUM_REQUESTS, response_count);

        // Validate responses
        int pass_count = 0;
        req_idx = 0;
        for (int global_req = 0; global_req < NUM_REQUESTS; global_req++) {
            int page_idx = global_req / REQ_PER_PAGE;
            int offset_in_page = global_req % REQ_PER_PAGE;
            uint64_t iova = IOVA_BASE + (uint64_t)page_indices[page_idx] * 0x1000
                          + offset_in_page * 0x200;
            uint64_t expected_pa = (uint64_t)page_idx * GPA_STRIDE + PA_OFFSET + (iova & 0xFFF);
            uint64_t result_pa = trans_array[req_idx]->get_address();
            tlm::tlm_response_status status = trans_array[req_idx]->get_response_status();

            if (status == tlm::TLM_OK_RESPONSE && result_pa == expected_pa) {
                pass_count++;
            } else {
                printf("[TEST] FAIL req %4d (global=%d): page[%d]+%d IOVA=0x%lx, expected PA=0x%lx, got PA=0x%lx, status=%s\n",
                       req_idx, global_req, page_idx, offset_in_page, iova, expected_pa, result_pa,
                       trans_array[req_idx]->get_response_string().c_str());
            }
            req_idx++;
        }

        printf("\n[TEST] Validation: %d/%d passed\n", pass_count, NUM_REQUESTS);
        if (pass_count == NUM_REQUESTS) {
            printf("[TEST] PASS: All %d two-stage random requests translated correctly!\n", NUM_REQUESTS);
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

        printf("\n[TEST] %d-request random 4KB two-stage read test completed!\n", NUM_REQUESTS);

        // Print PTW DDR statistics
        printf("\n========== PTW DDR Access Statistics ==========\n");
        printf("  PTW total completed tasks: %lu\n", (unsigned long)iommu_ptr->ptw_total_completed);
        printf("  PTW total DDR reads:       %lu\n", (unsigned long)iommu_ptr->ptw_total_ddr_reads);
        if (iommu_ptr->ptw_total_completed > 0) {
            printf("  PTW avg DDR reads/task:    %.2f\n",
                   (double)iommu_ptr->ptw_total_ddr_reads / iommu_ptr->ptw_total_completed);
        }
        printf("================================================\n");

        // Print Buffer peak statistics
        auto* dedup_buf = iommu_ptr->cache_sub.get_pt_dedup_buffer();
        if (dedup_buf) {
            printf("\n========== PT Dedup Buffer Statistics ==========\n");
            printf("  Buffer Size:        %u entries\n", PT_DEDUP_BUFFER_SIZE);
            printf("  Peak Valid Count:   %u entries\n", dedup_buf->get_peak_valid_count());
            printf("  Current Valid:      %u entries\n", dedup_buf->get_valid_count());
            printf("  Peak Usage:         %.1f%%\n", 100.0 * dedup_buf->get_peak_valid_count() / PT_DEDUP_BUFFER_SIZE);
            printf("==================================================\n");
        }

        // Print Cache hit rate statistics
        iommu_ptr->print_cache_statistics();

        // Stop simulation
        sc_core::sc_stop();

        return;
    }
}
