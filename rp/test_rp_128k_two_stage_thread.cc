#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"
#include <cstdio>
#include <iostream>
using namespace std;

// ============================================================
// Test Scenario: 128KB Sequential Read + Two-Stage Address Translation
//   iosatp = Sv48, iohgatp = Sv48x4
//   IOVA range: 1MB (0x100000 ~ 0x1FFFFF), 256 pages
//   GPA  = IOVA (identity mapping at 4KB page granularity)
//   SPA  = GPA + 0x10000 (fixed offset)
//   2000 sequential READ requests, each 512B
//
//   Walker Cache and PT prefetch are DISABLED for this scenario.
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

        printf("\n========== IOMMU 128KB Sequential Read - Two-Stage Translation Test (device 0x0A) ==========\n");

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

        // Do NOT reset next_free_page here. add_device has already allocated
        // G-stage page table pages; resetting it would collide with those pages.

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
        // 1MB VS-stage page table + G-stage mapping
        // ============================================================
        const uint64_t IOVA_BASE   = 0x100000;         // 1MB aligned base
        const uint64_t RANGE_1MB   = 0x100000;         // 1MB
        const uint64_t PA_OFFSET   = 0x10000;          // SPA = GPA + 0x10000
        const int NUM_REQUESTS     = 2000;             // 2000 sequential requests
        const int TOTAL_PAGES_IN_RANGE = (int)(RANGE_1MB / 0x1000);  // 256

        printf("\n[TEST] 1MB Two-Stage Page Table Construction:\n");
        printf("[TEST]   IOVA range: 0x%lx ~ 0x%lx (1MB, %d pages)\n",
               IOVA_BASE, IOVA_BASE + RANGE_1MB, TOTAL_PAGES_IN_RANGE);
        printf("[TEST]   GPA = IOVA (identity), SPA = GPA + 0x%lx\n", PA_OFFSET);

        // Map all 256 pages in VS-stage page table, then map each GPA in G-stage
        for (int p = 0; p < TOTAL_PAGES_IN_RANGE; p++) {
            uint64_t iova_page = IOVA_BASE + (uint64_t)p * 0x1000;
            uint64_t gpa_page  = iova_page;                     // identity GPA
            uint64_t pa_page   = gpa_page + PA_OFFSET;          // final SPA

            // VS-stage leaf: IOVA -> GPA
            pte6.PPN = gpa_page / PAGESIZE;
            add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, iova_page, pte6, 0, DC6.iohgatp, 0);

            // G-stage leaf: GPA -> SPA
            gpte.PPN = pa_page / PAGESIZE;
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa_page, gpte, 0);
        }
        printf("[TEST]   Mapped %d VS-stage pages and %d G-stage pages\n",
               TOTAL_PAGES_IN_RANGE, TOTAL_PAGES_IN_RANGE);

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
        uint64_t single_expected_pa = single_iova + PA_OFFSET;

        tlm_generic_payload single_trans;
        sc_time single_delay = SC_ZERO_TIME;
        unsigned char single_data[1024] = {0};

        single_trans.set_address(single_iova);
        single_trans.set_data_ptr(single_data);
        single_trans.set_data_length(512);
        single_trans.set_command(TLM_READ_COMMAND);

        PayloadExtention single_ext;
        single_ext.requester_id = 0x0A;
        single_ext.pid_valid = 0;
        single_ext.process_id = 0;
        single_ext.exec_req = 0;
        single_ext.priv_req = 0;
        single_ext.no_write = 1;
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
            if (single_wait > 50) {
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
        printf("\n========== Phase 2: Full 128KB Sequential Read (%d requests) ==========\n", NUM_REQUESTS);

        // Reset counters for the full run
        response_count = 0;
        iommu_ptr->next_task_id = 2;  // single packet used task_id 1
        iommu_ptr->steady_start_count = NUM_REQUESTS * STEADY_STATE_START_PERCENT / 100;
        iommu_ptr->steady_end_count   = NUM_REQUESTS * STEADY_STATE_END_PERCENT / 100;

        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];

        printf("[TEST] Sending %d READ requests (sequential 512B stride)...\n", NUM_REQUESTS);
        printf("[TEST] Prefetch DISABLED (D=0), Walker Cache DISABLED\n");
        fflush(stdout);

        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t iova = IOVA_BASE + (uint64_t)i * 0x200;  // 512B stride
            uint64_t expected_pa = iova + PA_OFFSET;

            trans_array[i] = new tlm_generic_payload();
            sc_time delay = SC_ZERO_TIME;
            unsigned char* data = new unsigned char[1024]();

            trans_array[i]->set_address(iova);
            trans_array[i]->set_data_ptr(data);
            trans_array[i]->set_data_length(512);
            trans_array[i]->set_command(TLM_READ_COMMAND);

            ext_array[i] = new PayloadExtention();
            ext_array[i]->requester_id = 0x0A;
            ext_array[i]->pid_valid = 0;
            ext_array[i]->process_id = 0;
            ext_array[i]->exec_req = 0;
            ext_array[i]->priv_req = 0;
            ext_array[i]->no_write = 1;
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
                if (stall_count > 10) {
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
            uint64_t iova = IOVA_BASE + (uint64_t)i * 0x200;
            uint64_t expected_pa = iova + PA_OFFSET;
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

        printf("\n[TEST] %d-request sequential 128KB two-stage read test completed!\n", NUM_REQUESTS);

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

void RP_Module::send_translation_request_2_thread()
{
    while (true) {
        wait(concurrent_test_event);
        // Not used in this test scenario
        return;
    }
}

void RP_Module::send_translation_request_3_thread()
{
    while (true) {
        wait(concurrent_test_event);
        // Not used in this test scenario
        return;
    }
}
