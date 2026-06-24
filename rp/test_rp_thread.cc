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

#ifdef TEST_SEQ_128K
        printf("\n========== IOMMU 2000-Request Sequential 128KB Read Test (1MB Range) ==========\n");
#else
        printf("\n========== IOMMU 4000-Request Random 4KB Read Test (16MB Range, Shuffled) ==========\n");
#endif

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

#ifdef TEST_SEQ_128K
        // ============================================================
        // ===== 1MB Page Table + Sequential 128KB Read Scenario =====
        // ============================================================
        printf("\n========== 1MB Page Table + Sequential 128KB Read Test ==========\n");
#else
        // ============================================================
        // ===== 16MB Page Table + Random 4KB Read Scenario (Shuffled) =====
        // ============================================================
        printf("\n========== 16MB Page Table + Random 4KB Read Test (Shuffled) ==========\n");
#endif

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

        // Setup S-stage page table template
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

#ifdef TEST_SEQ_128K
        // ============================================================
        // 128KB Sequential Read Scenario:
        //   IOVA range: 1MB = 0x100000 bytes = 256 pages (4KB each)
        //   IOVA base:  0x100000 (1MB aligned)
        //   IOVA end:   0x100000 + 0x100000 = 0x200000
        //   PA = IOVA + 0x10000 (fixed offset)
        //
        //   2000 sequential requests, each 512B
        //   Total data: 2000 x 512B = 1,024,000 bytes ≈ 1MB
        //   All requests are READ, sequential access pattern.
        //
        //   Sequential access: prefetch D=3 will be VERY EFFECTIVE
        //   because consecutive requests hit same or adjacent pages.
        //   Expected: very high PT Cache hit rate after warmup.
        // ============================================================
        const uint64_t IOVA_BASE   = 0x100000;         // 1MB aligned base
        const uint64_t RANGE_1MB   = 0x100000;         // 1MB
        const uint64_t PA_OFFSET   = 0x10000;          // PA = IOVA + 0x10000
        const int NUM_REQUESTS     = 2000;             // 2000 sequential requests
        const int TOTAL_PAGES_IN_RANGE = (int)(RANGE_1MB / 0x1000);  // 256

        printf("\n[TEST] 1MB Page Table Construction:\n");
        printf("[TEST]   IOVA range: 0x%lx ~ 0x%lx (1MB, %d pages)\n",
               IOVA_BASE, IOVA_BASE + RANGE_1MB, TOTAL_PAGES_IN_RANGE);
        printf("[TEST]   PA offset: +0x%lx\n", PA_OFFSET);
        printf("[TEST]   Mapping %d sequential 4KB pages\n", TOTAL_PAGES_IN_RANGE);

        // Map all 256 pages sequentially in S-stage page table
        for (int p = 0; p < TOTAL_PAGES_IN_RANGE; p++) {
            uint64_t iova_page = IOVA_BASE + (uint64_t)p * 0x1000;
            uint64_t pa_page = iova_page + PA_OFFSET;
            pte6.PPN = pa_page / PAGESIZE;
            add_s_stage_pte(DC6.fsc.iosatp, iova_page, pte6, 0, 0);
        }
        printf("[TEST]   Mapped %d sequential pages in S-stage page table\n", TOTAL_PAGES_IN_RANGE);

#else
        // ============================================================
        // Address layout:
        //   IOVA range: 16MB = 0x1000000 bytes = 4096 pages (4KB each)
        //   IOVA base:  0x100000 (1MB aligned)
        //   IOVA end:   0x100000 + 0x1000000 = 0x1100000
        //   PA = IOVA + 0x10000 (fixed offset)
        //
        //   We select 500 unique random pages from 4096 possible pages,
        //   then shuffle the access order to simulate random access.
        //   Each page gets 8 requests (8 x 512B = 4KB).
        //   Total: 500 pages x 8 reqs = 4000 requests.
        //   All requests are READ.
        //
        //   Random access order: prefetch D=3 will be INEFFECTIVE because
        //   consecutive 4KB pages are NOT spatially local (shuffled order).
        // ============================================================
        const uint64_t IOVA_BASE   = 0x100000;         // 1MB aligned base
        const uint64_t RANGE_16MB  = 0x1000000;        // 16MB
        const uint64_t PA_OFFSET   = 0x10000;          // PA = IOVA + 0x10000
        const int PAGES_NEEDED     = 500;              // 4000 reqs / 8 per page
        const int REQ_PER_PAGE     = 8;                // 8 x 512B = 4KB
        const int NUM_REQUESTS     = PAGES_NEEDED * REQ_PER_PAGE;  // 4000
        const int TOTAL_PAGES_IN_RANGE = (int)(RANGE_16MB / 0x1000);  // 4096

        printf("\n[TEST] 16MB Page Table Construction:\n");
        printf("[TEST]   IOVA range: 0x%lx ~ 0x%lx (16MB, %d pages)\n",
               IOVA_BASE, IOVA_BASE + RANGE_16MB, TOTAL_PAGES_IN_RANGE);
        printf("[TEST]   PA offset: +0x%lx\n", PA_OFFSET);
        printf("[TEST]   Selecting %d unique random 4KB pages from %d candidates...\n",
               PAGES_NEEDED, TOTAL_PAGES_IN_RANGE);

        // Generate unique random page indices using deterministic seed
        srand(42);
        set<int> page_set;
        while ((int)page_set.size() < PAGES_NEEDED) {
            page_set.insert(rand() % TOTAL_PAGES_IN_RANGE);
        }

        // Convert to vector for indexed access
        vector<int> page_indices(page_set.begin(), page_set.end());
        
        // Shuffle the page access order to simulate random access pattern
        // This ensures prefetch D=3 is ineffective (non-sequential access)
        srand(123);
        for (int i = page_indices.size() - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            std::swap(page_indices[i], page_indices[j]);
        }
        
        printf("[TEST]   Generated %d unique random page indices (SHUFFLED access order)\n", (int)page_indices.size());
        printf("[TEST]   Sample access order: page[%d]=IOVA 0x%lx, page[%d]=IOVA 0x%lx, page[%d]=IOVA 0x%lx\n",
               0, IOVA_BASE + (uint64_t)page_indices[0] * 0x1000,
               1, IOVA_BASE + (uint64_t)page_indices[1] * 0x1000,
               499, IOVA_BASE + (uint64_t)page_indices[499] * 0x1000);

        // Map 500 random pages in S-stage page table
        for (int p = 0; p < PAGES_NEEDED; p++) {
            uint64_t iova_page = IOVA_BASE + (uint64_t)page_indices[p] * 0x1000;
            uint64_t pa_page = iova_page + PA_OFFSET;
            pte6.PPN = pa_page / PAGESIZE;
            add_s_stage_pte(DC6.fsc.iosatp, iova_page, pte6, 0, 0);
        }
        printf("[TEST]   Mapped %d random pages in S-stage page table\n", PAGES_NEEDED);
#endif

        // Reset response counter
        response_count = 0;

        // Invalidate caches
        printf("\n[TEST] Invalidating IOMMU caches for %d-request test...\n", NUM_REQUESTS);
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // Reset task ID counter
        iommu_ptr->next_task_id = 1;

        // Set steady-state IOPS sampling window (skip first 10% and last 10%)
        iommu_ptr->steady_start_count = NUM_REQUESTS * STEADY_STATE_START_PERCENT / 100;
        iommu_ptr->steady_end_count   = NUM_REQUESTS * STEADY_STATE_END_PERCENT / 100;

        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];

#ifdef TEST_SEQ_128K
        printf("\n[TEST] Sending %d READ requests (sequential 512B stride)...\n", NUM_REQUESTS);
        printf("[TEST] Prefetch D=3 ENABLED - expected to be VERY EFFECTIVE (sequential addresses)\n");
        printf("[TEST] Each page: req 0 MISS + 7 HIT (same page), next page MISS + prefetch\n");
#else
        printf("\n[TEST] Sending %d READ requests (random 4KB pages, 8 reqs/page)...\n", NUM_REQUESTS);
        printf("[TEST] Prefetch D=3 ENABLED - expected to be INEFFECTIVE (random addresses)\n");
        printf("[TEST] Dedup within 4KB page: req 0 MISS + 3 prefetch, req 1-7 HIT dedup\n");
#endif
        fflush(stdout);

#ifdef TEST_SEQ_128K
        for (int i = 0; i < NUM_REQUESTS; i++) {
            // Sequential access: IOVA increments by 512B each request
            uint64_t iova = IOVA_BASE + (uint64_t)i * 0x200;  // 512B stride
            uint64_t expected_pa = iova + PA_OFFSET;
#else
        for (int i = 0; i < NUM_REQUESTS; i++) {
            int page_idx = i / REQ_PER_PAGE;    // which random page (0~499)
            int offset_in_page = i % REQ_PER_PAGE;  // offset within page (0~7)

            uint64_t iova = IOVA_BASE + (uint64_t)page_indices[page_idx] * 0x1000
                          + offset_in_page * 0x200;  // 512B stride within page
            uint64_t expected_pa = iova + PA_OFFSET;
#endif

            trans_array[i] = new tlm_generic_payload();
            sc_time delay = SC_ZERO_TIME;
            unsigned char* data = new unsigned char[1024]();

            trans_array[i]->set_address(iova);
            trans_array[i]->set_data_ptr(data);
            trans_array[i]->set_data_length(512);
            trans_array[i]->set_command(TLM_READ_COMMAND);  // All READ

            ext_array[i] = new PayloadExtention();
            ext_array[i]->requester_id = 0x0A;
            ext_array[i]->pid_valid = 0;
            ext_array[i]->process_id = 0;
            ext_array[i]->exec_req = 0;
            ext_array[i]->priv_req = 0;
            ext_array[i]->no_write = 1;  // READ: no_write=1
            ext_array[i]->at = 0;
            trans_array[i]->set_extension(ext_array[i]);

            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            tlm::tlm_sync_enum status =
                axi_master_to_pcie_noc_0_socket->nb_transport_fw(*trans_array[i], phase, delay);

            // Print progress every 100 requests
            if ((i + 1) % 100 == 0) {
#ifdef TEST_SEQ_128K
                printf("[TEST] Progress: %d/%d requests injected (IOVA=0x%lx)\n",
                       i + 1, NUM_REQUESTS, iova);
#else
                printf("[TEST] Progress: %d/%d requests injected (page[%d] IOVA=0x%lx)\n",
                       i + 1, NUM_REQUESTS, page_idx, iova);
#endif
                fflush(stdout);
            }

            // Back-to-back injection, let FIFO backpressure throttle naturally
        }

        printf("[TEST] All %d requests injected. Waiting for responses...\n", NUM_REQUESTS);
        fflush(stdout);

        // Wait for all responses with timeout
        int stall_count = 0;
        int last_response_count = response_count;
        while (response_count < NUM_REQUESTS) {
            wait(1000, SC_NS);  // Wait 1us and check
            if (response_count == last_response_count) {
                stall_count++;
                if (stall_count > 10) {  // No progress for 10us, assume stuck
                    printf("[TEST] WARNING: No progress for 10us, breaking wait loop. Got %d/%d responses\n", 
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
#ifdef TEST_SEQ_128K
            // Sequential access: IOVA increments by 512B each request
            uint64_t iova = IOVA_BASE + (uint64_t)i * 0x200;
            uint64_t expected_pa = iova + PA_OFFSET;
#else
            int page_idx = i / REQ_PER_PAGE;
            int offset_in_page = i % REQ_PER_PAGE;
            uint64_t iova = IOVA_BASE + (uint64_t)page_indices[page_idx] * 0x1000
                          + offset_in_page * 0x200;
            uint64_t expected_pa = iova + PA_OFFSET;
#endif
            uint64_t result_pa = trans_array[i]->get_address();
            tlm::tlm_response_status status = trans_array[i]->get_response_status();

            if (status == tlm::TLM_OK_RESPONSE && result_pa == expected_pa) {
                pass_count++;
            } else {
#ifdef TEST_SEQ_128K
                printf("[TEST] FAIL req %4d: IOVA=0x%lx, expected PA=0x%lx, got PA=0x%lx, status=%s\n",
                       i, iova, expected_pa, result_pa,
                       trans_array[i]->get_response_string().c_str());
#else
                printf("[TEST] FAIL req %4d: page[%d]+%d IOVA=0x%lx, expected PA=0x%lx, got PA=0x%lx, status=%s\n",
                       i, page_idx, offset_in_page, iova, expected_pa, result_pa,
                       trans_array[i]->get_response_string().c_str());
#endif
            }
        }

        printf("\n[TEST] Validation: %d/%d passed\n", pass_count, NUM_REQUESTS);
        if (pass_count == NUM_REQUESTS) {
            printf("[TEST] PASS: All %d requests translated correctly!\n", NUM_REQUESTS);
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

#ifdef TEST_SEQ_128K
        printf("\n[TEST] %d-request sequential 128KB read test completed!\n", NUM_REQUESTS);
#else
        printf("\n[TEST] %d-request random 4KB read test completed!\n", NUM_REQUESTS);
#endif

        // [STAT] Print Buffer peak statistics
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
