#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"
#include <cstdio>
#include <iostream>
using namespace std;

// ============================================================
// Test Scenario 8: 512B Sequential Read + Two-Stage + 2MB Huge Pages
//   iosatp = Sv48 (VS-stage 2MB leaf at L1)
//   iohgatp = Sv48x4 (G-stage 2MB leaf at level 1)
//   IOVA: base 0x200000, 512B stride, sequential increasing
//   10000 requests -> covers 3 x 2MB IOVA pages (vpn[1]=1..3)
//   Mapping: IOVA 2MB page idx -> GPA = GPA_BASE + idx*2MB
//            SPA = SPA_DATA_BASE + idx*2MB (data PA never accessed in DDR)
//
//   Expected behavior (hugepage semantics):
//   - PT Cache: query-only, never written -> hit rate 0
//   - Walker Cache: end-to-end 2MB leaf cached (VS + S2), steady-state
//     PTW completes with 0 DDR reads (front leaf short-circuit)
//   - PTW: no prefetch spawn for hugepage main tasks, still returns
//     D+1 results (D marked invalid) to flush dedup placeholders
//   - First packet walk: 3x(3 GS-implicit + 1 VS read) + 3 GS-explicit = 15 DDR reads
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

        printf("\n========== IOMMU 512B Sequential Read - Two-Stage 2MB Huge Page Test (device 0x0A) ==========\n");

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
        // Scenario constants
        // ============================================================
        const uint64_t IOVA_BASE     = 0x200000;       // 2MB aligned base (vpn[1]=1)
        const uint64_t HUGE_PAGE_SZ  = 0x200000;       // 2MB
        const uint64_t GPA_BASE      = 0x40000000;     // data GPA base (1GB, 2MB aligned)
        const uint64_t SPA_DATA_BASE = 0x80000000;     // data SPA base (2GB, never accessed)
        const uint64_t VS_PT_SPA_BASE = 0x1000000;     // VS page table pages SPA (16MB, inside 32MB DDR)
        const int      NUM_REQUESTS  = 10000;          // 512B stride sequential reads
        // IOVA range: Phase2 uses IOVA_BASE+0x200 .. IOVA_BASE+NUM_REQUESTS*0x200
        const uint64_t IOVA_END      = IOVA_BASE + (uint64_t)NUM_REQUESTS * 0x200;
        const int      FIRST_VPN1    = (int)(IOVA_BASE >> 21);          // =1
        const int      LAST_VPN1     = (int)(IOVA_END >> 21);           // =3
        const int      NUM_2MB_PAGES = LAST_VPN1 - FIRST_VPN1 + 1;      // =3

        printf("\n[TEST] Two-Stage 2MB Huge Page Table Construction:\n");
        printf("[TEST]   IOVA range: 0x%lx ~ 0x%lx (512B stride, %d reqs, %d x 2MB pages)\n",
               IOVA_BASE, IOVA_END, NUM_REQUESTS, NUM_2MB_PAGES);
        printf("[TEST]   VS 2MB leaf: IOVA page idx -> GPA = 0x%lx + idx*2MB\n", GPA_BASE);
        printf("[TEST]   G  2MB leaf: GPA -> SPA = 0x%lx + idx*2MB (data PA not accessed)\n", SPA_DATA_BASE);

        // ============================================================
        // GPPN allocator offset: root VS page GPA at 4GB (2MB aligned region),
        // away from data GPA region (1GB)
        // ============================================================
        extern uint64_t next_free_gpage[65536];
        next_free_gpage[1] = 0x100000;  // GSCID=1: root GPPN -> GPA=4GB, before add_device!
        printf("[TEST]   GPPN offset: 0x100000 (root VS GPA = 0x100000000)\n");

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
        // G-stage page tables (all 2MB leaves, add_level=1):
        //   (a) VS page-table page region: root GPA 2MB region -> VS_PT_SPA_BASE
        //   (b) data regions: GPA_BASE+idx*2MB -> SPA_DATA_BASE+idx*2MB
        // G-stage walk for any GPA = 3 DDR reads (L3/L2 non-leaf + L1 2MB leaf)
        // ============================================================
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

        // (a) VS page table page region (root + L2 + L1 tables live in same 2MB GPA region)
        uint64_t vs_root_gpa = (uint64_t)DC6.fsc.iosatp.PPN * PAGESIZE;
        uint64_t vs_pt_gpa_2mb = vs_root_gpa & ~(HUGE_PAGE_SZ - 1);
        gpte.PPN = VS_PT_SPA_BASE / PAGESIZE;  // 2MB aligned SPA (16MB)
        fail_if((add_g_stage_pte(iommu_ptr, DC6.iohgatp, vs_pt_gpa_2mb, gpte, 1) == (uint64_t)-1));
        printf("[TEST] G-stage 2MB mapping (VS PT region): GPA=0x%lx -> SPA=0x%lx\n",
               vs_pt_gpa_2mb, VS_PT_SPA_BASE);

        // (b) data regions
        for (int idx = 0; idx < NUM_2MB_PAGES; idx++) {
            uint64_t gpa_2mb = GPA_BASE + (uint64_t)idx * HUGE_PAGE_SZ;
            uint64_t spa_2mb = SPA_DATA_BASE + (uint64_t)idx * HUGE_PAGE_SZ;
            gpte.PPN = spa_2mb / PAGESIZE;
            fail_if((add_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa_2mb, gpte, 1) == (uint64_t)-1));
            printf("[TEST] G-stage 2MB mapping (data): GPA=0x%lx -> SPA=0x%lx\n", gpa_2mb, spa_2mb);
        }

        // ============================================================
        // VS-stage page tables (manual construction, 2MB leaves at L1):
        //   root(L3)[0] -> L2 table, L2[0] -> L1 table, L1[vpn1] = 2MB leaf
        //   Table pages GPA: root_gpa, root_gpa+0x1000, root_gpa+0x2000
        //   (all inside the same 2MB G-mapped region -> GS-implicit = 3 reads)
        //   Table contents written at SPA = VS_PT_SPA_BASE + (gpa & 0x1FFFFF)
        // ============================================================
        uint64_t vs_root_off = vs_root_gpa & (HUGE_PAGE_SZ - 1);
        uint64_t l2_gpa = vs_root_gpa + 0x1000;
        uint64_t l1_gpa = vs_root_gpa + 0x2000;
        uint64_t root_spa = VS_PT_SPA_BASE + vs_root_off;
        uint64_t l2_spa   = VS_PT_SPA_BASE + (l2_gpa & (HUGE_PAGE_SZ - 1));
        uint64_t l1_spa   = VS_PT_SPA_BASE + (l1_gpa & (HUGE_PAGE_SZ - 1));

        // Clear the three VS page table pages
        {
            unsigned char zero_page[4096] = {0};
            write_memory_test_rp((char*)zero_page, root_spa, 4096);
            write_memory_test_rp((char*)zero_page, l2_spa, 4096);
            write_memory_test_rp((char*)zero_page, l1_spa, 4096);
        }

        // root(L3)[vpn3=0] -> L2 table (non-leaf, PPN = L2 GPPN)
        spte_t nl_pte;
        nl_pte.raw = 0;
        nl_pte.V = 1;
        nl_pte.PPN = l2_gpa / PAGESIZE;
        write_memory_test_rp((char*)&nl_pte.raw, root_spa + 0 * 8, 8);

        // L2[vpn2=0] -> L1 table (non-leaf, PPN = L1 GPPN)
        nl_pte.raw = 0;
        nl_pte.V = 1;
        nl_pte.PPN = l1_gpa / PAGESIZE;
        write_memory_test_rp((char*)&nl_pte.raw, l2_spa + 0 * 8, 8);

        // L1[vpn1] = 2MB leaf: IOVA 2MB page -> GPA 2MB page
        spte_t leaf_pte;
        leaf_pte.raw = 0;
        leaf_pte.V = 1;
        leaf_pte.R = 1;
        leaf_pte.W = 1;
        leaf_pte.X = 0;
        leaf_pte.U = 1;
        leaf_pte.G = 0;
        leaf_pte.A = 1;
        leaf_pte.D = 1;
        leaf_pte.PBMT = PMA;
        for (int vpn1 = FIRST_VPN1; vpn1 <= LAST_VPN1; vpn1++) {
            int idx = vpn1 - FIRST_VPN1;
            uint64_t gpa_2mb = GPA_BASE + (uint64_t)idx * HUGE_PAGE_SZ;
            leaf_pte.PPN = gpa_2mb / PAGESIZE;  // 2MB aligned -> low 9 bits of PPN are 0
            write_memory_test_rp((char*)&leaf_pte.raw, l1_spa + (uint64_t)vpn1 * 8, 8);
            printf("[TEST] VS 2MB leaf: L1[%d] IOVA=0x%lx -> GPA=0x%lx (PPN=0x%lx)\n",
                   vpn1, (uint64_t)vpn1 << 21, gpa_2mb, (uint64_t)leaf_pte.PPN);
        }
        printf("[TEST]   VS page tables: root@GPA=0x%lx(SPA=0x%lx), L2@0x%lx, L1@0x%lx\n",
               vs_root_gpa, root_spa, l2_gpa, l1_gpa);

        // Invalidate caches
        printf("\n[TEST] Invalidating IOMMU caches for two-stage 2MB test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // Reset task ID counter
        iommu_ptr->next_task_id = 1;

        // ============================================================
        // Phase 1: Single packet sanity check (expect 15 DDR reads)
        // [场景化] TEST_CFG_SKIP_PHASE1=1 跳过单包, 仅注入10000个两阶段请求,
        //   避免Phase1的PTW/DDR统计污染主测试性能指标
        // ============================================================
#ifdef TEST_CFG_SKIP_PHASE1
        const bool RUN_PHASE1 = false;
#else
        const bool RUN_PHASE1 = true;
#endif
        if (RUN_PHASE1) {
        printf("\n========== Phase 1: Single Packet Two-Stage 2MB Translation ==========\n");

        response_count = 0;
        uint64_t single_iova = IOVA_BASE;
        // idx = 0 (vpn1=1), PA = SPA_DATA_BASE + 0*2MB + offset(0)
        uint64_t single_expected_pa = SPA_DATA_BASE + (single_iova & (HUGE_PAGE_SZ - 1));

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
            if (single_wait > 200) {
                printf("[TEST] ERROR: Single packet response timeout\n");
                break;
            }
        }

        uint64_t single_result_pa = single_trans.get_address();
        tlm::tlm_response_status single_rsp_status = single_trans.get_response_status();

        printf("[TEST] Single packet response: status=%s, PA=0x%lx\n",
               single_trans.get_response_string().c_str(), single_result_pa);
        printf("[TEST] Single packet PTW total DDR reads: %lu (expect 15 for 2MB two-stage)\n",
               (unsigned long)iommu_ptr->ptw_total_ddr_reads);

        if (single_rsp_status == tlm::TLM_OK_RESPONSE && single_result_pa == single_expected_pa) {
            printf("[TEST] PASS: Single packet two-stage 2MB translation correct!\n");
        } else {
            printf("[TEST] FAIL: Single packet expected PA=0x%lx, got PA=0x%lx\n",
                   single_expected_pa, single_result_pa);
        }

        single_trans.clear_extension(&single_ext);
        }  // if (RUN_PHASE1)

        // ============================================================
        // Phase 2: Full 512B-stride sequential read test
        // ============================================================
        printf("\n========== Phase 2: Full 512B Sequential Read (%d requests, 2MB pages) ==========\n", NUM_REQUESTS);

        response_count = 0;
        iommu_ptr->next_task_id = RUN_PHASE1 ? 2 : 1;  // Phase1占用task_id=1
        iommu_ptr->steady_start_count = 256;
        iommu_ptr->steady_end_count   = NUM_REQUESTS - 256;

        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];

        printf("[TEST] Sending %d READ requests (sequential 512B stride)...\n", NUM_REQUESTS);
        printf("[TEST] Prefetch D=%d (suppressed for hugepage), Walker Cache ENABLED (2MB end-to-end leaf)\n",
               (int)TEST_CFG_PT_DEDUP_PREFETCH_DEPTH);
        fflush(stdout);

        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t iova = IOVA_BASE + (uint64_t)(i + 1) * 0x200;  // 512B stride, +1 skips Phase1 IOVA
            int idx = (int)(iova >> 21) - FIRST_VPN1;
            uint64_t expected_pa = SPA_DATA_BASE + (uint64_t)idx * HUGE_PAGE_SZ
                                 + (iova & (HUGE_PAGE_SZ - 1));
            (void)expected_pa;

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

            if ((i + 1) % 1000 == 0) {
                printf("[TEST] Progress: %d/%d requests injected (IOVA=0x%lx)\n",
                       i + 1, NUM_REQUESTS, iova);
                fflush(stdout);
            }
        }

        printf("[TEST] All %d requests injected. Waiting for responses...\n", NUM_REQUESTS);
        fflush(stdout);

        // Wait for all responses with stall detection
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
        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t iova = IOVA_BASE + (uint64_t)(i + 1) * 0x200;
            int idx = (int)(iova >> 21) - FIRST_VPN1;
            uint64_t expected_pa = SPA_DATA_BASE + (uint64_t)idx * HUGE_PAGE_SZ
                                 + (iova & (HUGE_PAGE_SZ - 1));
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
            printf("[TEST] PASS: All %d two-stage 2MB requests translated correctly!\n", NUM_REQUESTS);
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

        printf("\n[TEST] %d-request sequential 512B two-stage 2MB read test completed!\n", NUM_REQUESTS);

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
