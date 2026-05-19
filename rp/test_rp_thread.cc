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

        printf("\n========== IOMMU Simplified 500-Request Test (single device 0x0A) ==========\n");

        // 妫€鏌?IOMMU 妯″紡
        ddtp_t ddtp_check;
        ddtp_check.raw = read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4);
        printf("[DEBUG] Current IOMMU mode before enable_iommu: %d\n", ddtp_check.iommu_mode);

        // 浣胯兘 IOMMU锛屽垎閰嶅苟鍒濆鍖?DDT 鏍硅〃 (1绾DT)
        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));

        // 閲嶇疆 next_free_page锛岄伩鍏嶄笌 DDT 鍐茬獊
        extern uint64_t next_free_page;
        next_free_page = 240;

        printf("[DEBUG] IOMMU mode after enable_iommu: %d (expect 2 for DDT_1LVL)\n",
               read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4) & 0x7);

        // ============================================================
        // ========== 100-Request Concurrent Single-Stage Test ==========
        // ============================================================
        printf("\n========== 500-Request Concurrent Single-Stage Test ==========\n");

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

        // Setup S-stage page table for 500 requests
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

        // Map 125 pages for 500 requests (8 requests per 4KB page, stride=512B)
        // IOVA range: 0x10000 to 0x89B80 (500 requests * 512B stride)
        // PA range: 0x20000 to 0x99B80 (PA = IOVA + 0x10000)
        for (int p = 0; p < 125; p++) {
            uint64_t iova_page = 0x10000 + p * 0x1000;
            uint64_t pa_page = 0x20000 + p * 0x1000;
            pte6.PPN = pa_page / PAGESIZE;
            uint64_t pte_addr = add_s_stage_pte(DC6.fsc.iosatp, iova_page, pte6, 0, 0);
            printf("[DEV6] Added S-stage PTE at addr 0x%lx, IOVA 0x%lx -> PA 0x%lx\n", pte_addr, iova_page, pa_page);
        }

        // Invalidate caches
        printf("\n[TEST] Invalidating IOMMU caches for 500-request test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // Reset task ID counter so 500 requests get IDs 1-500
        iommu_ptr->next_task_id = 1;

        // Reset response counter
        response_count = 0;

        // Prepare and send 100 translation requests
        const int NUM_REQUESTS = 500;
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

            // 姣?涓姹傦紙鍚屼竴涓?KB椤碉級涓轰竴缁勶紝缁勯棿寤惰繜50ns璁㏄T cache鏈夋椂闂存洿鏂?
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

        printf("\n[TEST] 500-request concurrent test completed!\n");

        // 鎵撳嵃Cache鍛戒腑鐜囩粺璁′俊鎭?
        iommu_ptr->print_cache_statistics();

        // 鍋滄浠跨湡
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
