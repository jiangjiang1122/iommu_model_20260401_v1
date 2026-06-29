#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"
#include <cstdio>
#include <iostream>
using namespace std;

// ============================================================
// Test Scenario: iosatp=Sv48, iohgatp=Bare, 1000 continuous WRITE requests
// Page table: 4-level (L3->L2->L1->L0)
// IOVA range: 0x100000 ~ 0x17FE00 (stride=512B, 8 req/page)
// PA = IOVA + 0x100000 (offset mapping)
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

        printf("\n========== IOMMU Sv48+Bare 1000-Request Test (device 0x0A) ==========\n");

        // Check IOMMU mode
        ddtp_t ddtp_check;
        ddtp_check.raw = read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4);
        printf("[DEBUG] Current IOMMU mode before enable_iommu: %d\n", ddtp_check.iommu_mode);

        // Enable IOMMU, allocate and initialize DDT root table (1-level DDT)
        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));

        // Reset next_free_page to avoid collision with DDT
        extern uint64_t next_free_page;
        next_free_page = 240;

        printf("[DEBUG] IOMMU mode after enable_iommu: %d (expect 2 for DDT_1LVL)\n",
               read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4) & 0x7);

        // ============================================================
        // Configure device 0x0A: iohgatp=Bare, iosatp=Sv48
        // ============================================================
        printf("\n========== Configuring Device 0x0A: iosatp=Sv48, iohgatp=Bare ==========\n");

        uint64_t dc_addr = add_device(iommu_ptr, 0x0A, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Bare, IOSATP_Sv48, PDTP_Bare,
                                       MSIPTP_Off, 0, 0, 0);
        device_context_t DC_sv48;
        read_memory_test_rp(dc_addr, sizeof(device_context_t), (char*)&DC_sv48);
        printf("[DEV] DC addr: 0x%lx, iohgatp.MODE=%d (Bare), iosatp.MODE=%d (Sv48)\n",
               dc_addr, DC_sv48.iohgatp.MODE, DC_sv48.fsc.iosatp.MODE);

        next_free_page = 245;

        // ============================================================
        // Setup Sv48 S-stage page table for 1000 requests
        // Sv48: 4-level (L3->L2->L1->L0), each level 512 entries (9-bit VPN)
        // IOVA base=0x100000, stride=512B, 8 requests per 4KB page
        // Need 125 pages mapped (1000/8=125)
        // VPN decomposition for 0x100000~0x17FE00:
        //   VPN[3]=0, VPN[2]=0, VPN[1]=0, VPN[0]=256..383
        // PA = IOVA + 0x100000
        // ============================================================
        printf("\n[TEST] Setting up Sv48 page table (4-level)...\n");

        spte_t pte_leaf;
        pte_leaf.raw = 0;
        pte_leaf.V = 1;
        pte_leaf.R = 1;
        pte_leaf.W = 1;
        pte_leaf.X = 0;
        pte_leaf.U = 1;
        pte_leaf.G = 0;
        pte_leaf.A = 1;
        pte_leaf.D = 1;
        pte_leaf.PBMT = PMA;

        // Map 125 pages for 1000 requests (8 req per page, stride=512B)
        const int NUM_PAGES = 125;
        for (int p = 0; p < NUM_PAGES; p++) {
            uint64_t iova_page = 0x100000 + p * 0x1000;
            uint64_t pa_page = 0x200000 + p * 0x1000;  // PA = IOVA + 0x100000
            pte_leaf.PPN = pa_page / PAGESIZE;
            uint64_t pte_addr = add_s_stage_pte(DC_sv48.fsc.iosatp, iova_page, pte_leaf, 0, 0);
            if (p < 5 || p >= NUM_PAGES - 2) {
                printf("[DEV] Sv48 PTE page %d: IOVA 0x%lx -> PA 0x%lx (pte_addr=0x%lx)\n",
                       p, iova_page, pa_page, pte_addr);
            }
        }
        printf("[TEST] Mapped %d pages for Sv48 translation\n", NUM_PAGES);

        // Invalidate caches
        printf("\n[TEST] Invalidating IOMMU caches...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // Reset task ID counter
        iommu_ptr->next_task_id = 1;

        // Reset response counter
        response_count = 0;

        // ============================================================
        // Send 1000 translation requests (WRITE, back-to-back)
        // ============================================================
        const int NUM_REQUESTS = 1000;
        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];
        uint64_t base_iova = 0x100000;

        printf("\n[TEST] Sending %d Sv48 translation requests (device_id=0x0A, WRITE)...\n", NUM_REQUESTS);
        printf("[TEST] IOVA range: 0x%lx ~ 0x%lx (stride=512B)\n",
               base_iova, base_iova + (NUM_REQUESTS - 1) * 512);
        printf("[TEST] Expected PA range: 0x%lx ~ 0x%lx\n",
               base_iova + 0x100000, base_iova + 0x100000 + (NUM_REQUESTS - 1) * 512);
        fflush(stdout);

        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t iova = base_iova + i * 512;
            uint64_t expected_pa = iova + 0x100000;

            trans_array[i] = new tlm_generic_payload();
            sc_time delay = SC_ZERO_TIME;
            unsigned char* data = new unsigned char[1024]();

            trans_array[i]->set_address(iova);
            trans_array[i]->set_data_ptr(data);
            trans_array[i]->set_data_length(512);  // [MODEL] 512B DMA 载荷占用入口带宽
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

            if (i < 5 || i >= NUM_REQUESTS - 2) {
                printf("[TEST] Sent req %4d: IOVA=0x%lx (PA_exp=0x%lx)\n",
                       i, iova, expected_pa);
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
            uint64_t expected_pa = iova + 0x100000;
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
            printf("[TEST] PASS: All %d Sv48+Bare requests translated correctly!\n", NUM_REQUESTS);
        } else {
            printf("[TEST] FAIL: %d requests failed translation!\n", NUM_REQUESTS - pass_count);
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

        printf("\n[TEST] Sv48+Bare 1000-request test completed!\n");

        // Print Cache hit rate statistics
        iommu_ptr->print_cache_statistics();

        // Stop simulation
        sc_core::sc_stop();

        return;
    }
}

