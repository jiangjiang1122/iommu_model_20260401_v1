#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"
#include <cstdio>
#include <iostream>
#include <set>
#include <cstdlib>
using namespace std;

// ============================================================
// Scenario 12: 虚拟化两级Stage — Strict 模式 Cache Invalidate
//   对应规范 6.5.4 场景二(iohgatp.mode!=Bare 且 iosatp.mode!=Bare)
//
// 负载与 unmap 序列**与场景11(Lazy) 完全一致**(同种子/同页序/同滞后策略),
// 仅失效策略不同 -> 可与场景11 直接 A/B 对比。
//
// 双失效发起方:
//   [Guest OS] 管理 Stage1(GVA->GPA)。每次 unmap **立即**经 vIOMMU 虚拟CQ 发起
//              IOTINVAL.VMA(GV=1, GSCID, PSCID, **AV=1, ADDR=GVA**),
//              由 VMM 拦截转换后写物理CQ, 并**等待 IOFENCE.C 完成后才释放 GPA**。
//              -> 物理侧走 SCAN_RANGE: PT 枚举8候选set精准失效(不进LIB)
//   [VMM]      管理 Stage2(GPA->SPA)。每次 unmap 后**立即**发 IOTINVAL.GVMA(GV=1,GSCID)
//
// 关键特征: 每次 unmap 都完整走规范步骤1~11, 确保设备不会通过陈旧的
//   Stage1 映射访问已回收内存; 代价是 unmap 关键路径延迟高(含trap+IOFENCE等待)
// ============================================================
void RP_Module::send_translation_request_1_thread()
{
    while (true)
    {
        wait(10, SC_NS);

        printf("\n========== Scenario 12: Virtualized Two-Stage — STRICT Mode Invalidation ==========\n");

        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));
        fail_if((enable_cq(iommu_ptr, 1) < 0));
        printf("[VIRT] Command queue enabled (physical IOMMU CQ)\n");

        extern uint64_t next_free_page;
        next_free_page = 240;

        // ---------- 负载参数(与场景7/10/11 一致) ----------
        const uint64_t IOVA_BASE   = 0x100000;
        const uint64_t RANGE_16MB  = 0x1000000;
        const uint64_t PA_OFFSET   = 0x10000;
        const uint64_t GPA_STRIDE  = 0x200000;
#ifndef TEST_CFG_NUM_PAGES
        const int PAGES_NEEDED     = 625;
#else
        const int PAGES_NEEDED     = TEST_CFG_NUM_PAGES;
#endif
        const int REQ_PER_PAGE     = 8;
        const int NUM_REQUESTS     = PAGES_NEEDED * REQ_PER_PAGE;
        const int TOTAL_PAGES_IN_RANGE = (int)(RANGE_16MB / 0x1000);

        // ---------- 虚拟化 / Strict 参数 ----------
        const uint32_t VM_GSCID  = 1;
        const uint32_t VM_PSCID  = 0;
        // [关键] 与场景11 一致: unmap 门控于 response_count(该页8个请求全部响应完成),
        //   对应 "DMA 完成后回收 buffer" 的真实语义。按注入序号滞后会因端到端延迟
        //   远大于注入间隔而 unmap 掉仍在翻译中的页, 导致翻译失败。
        // [v2] 加大安全余量至 512 请求
        const int UNMAP_RESP_MARGIN = 512;
#ifndef TEST_CFG_VMM_TRAP_NS
        vmm_trap_latency_ns = 2000.0;
#else
        vmm_trap_latency_ns = (double)TEST_CFG_VMM_TRAP_NS;
#endif
        const int VMM_UNMAP_EVERY = 4;   // 与场景11 一致

        printf("[VIRT] Strict config: VMM trap=%.0f ns, unmap gated by response_count "
               "(+%d margin) (NO flush queue: every unmap -> immediate full invalidation)\n",
               vmm_trap_latency_ns, UNMAP_RESP_MARGIN);
        printf("[VIRT] VMM Stage2 unmap: every %d guest unmaps, immediate GVMA\n",
               VMM_UNMAP_EVERY);

        extern uint64_t next_free_gpage[65536];
        uint64_t test_max_gppn = (uint64_t)(PAGES_NEEDED - 1) * (GPA_STRIDE / PAGESIZE);
        next_free_gpage[1] = ((test_max_gppn + 0xFFFF) / 0x10000) * 0x10000;

        // ---------- 配置设备: 两级Stage 均使能 ----------
        uint64_t dc6_addr = add_device(iommu_ptr, 0x0A, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Sv48x4, IOSATP_Sv48, PDTP_Bare,
                                       MSIPTP_Off, 0, 0, 0);
        device_context_t DC6;
        read_memory_test_rp(dc6_addr, sizeof(device_context_t), (char*)&DC6);
        printf("[VIRT] DC: iohgatp.MODE=%d (Stage2 by VMM), iosatp.MODE=%d (Stage1 by Guest), GSCID=%d\n",
               DC6.iohgatp.MODE, DC6.fsc.iosatp.MODE, (int)DC6.iohgatp.GSCID);

        {
            uint64_t vs_root_gpa = (uint64_t)DC6.fsc.iosatp.PPN * PAGESIZE;
            gpte_t g; g.raw = 0;
            g.V = 1; g.R = 1; g.W = 1; g.U = 1; g.A = 1; g.D = 1; g.PBMT = PMA;
            g.PPN = get_free_ppn(1);
            unsigned char zp[4096] = {0};
            write_memory_test_rp((char*)zp, g.PPN * PAGESIZE, 4096);
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, vs_root_gpa, g, 0);
        }

        iofence_flag_addr = get_free_ppn(1) * PAGESIZE;
        printf("[VIRT] IOFENCE.C completion flag addr = 0x%lx\n",
               (unsigned long)iofence_flag_addr);

        // ---------- 随机页索引(与场景7/11 同种子) ----------
        srand(42);
        set<int> page_set;
        while ((int)page_set.size() < PAGES_NEEDED)
            page_set.insert(rand() % TOTAL_PAGES_IN_RANGE);
        vector<int> page_indices(page_set.begin(), page_set.end());
        srand(123);
        for (int i = page_indices.size() - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            std::swap(page_indices[i], page_indices[j]);
        }

        // ---------- 建表 ----------
        spte_t pte6; pte6.raw = 0;
        pte6.V = 1; pte6.R = 1; pte6.W = 1; pte6.U = 1; pte6.A = 1; pte6.D = 1;
        pte6.PBMT = PMA;
        gpte_t gpte; gpte.raw = 0;
        gpte.V = 1; gpte.R = 1; gpte.W = 1; gpte.U = 1; gpte.A = 1; gpte.D = 1;
        gpte.PBMT = PMA;

        for (int p = 0; p < PAGES_NEEDED; p++) {
            uint64_t iova_page = IOVA_BASE + (uint64_t)page_indices[p] * 0x1000;
            uint64_t gpa_page  = (uint64_t)p * GPA_STRIDE;
            pte6.PPN = gpa_page / PAGESIZE;
            add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, iova_page, pte6, 0, DC6.iohgatp, 0);
            gpte.PPN = (gpa_page + PA_OFFSET) / PAGESIZE;
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa_page, gpte, 0);
        }
        printf("[VIRT] Mapped %d Stage1(VS) + %d Stage2(G) pages\n",
               PAGES_NEEDED, PAGES_NEEDED);

        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);
        iommu_ptr->next_task_id = 1;

        // ============================================================
        // 主测试
        // ============================================================
        printf("\n========== %d Requests + STRICT-mode unmap/invalidation ==========\n",
               NUM_REQUESTS);
        response_count = 0;
        iommu_ptr->steady_start_count = NUM_REQUESTS * STEADY_STATE_START_PERCENT / 100;
        iommu_ptr->steady_end_count   = NUM_REQUESTS * STEADY_STATE_END_PERCENT / 100;

        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];

        int next_unmap_page = 0;
        int vmm_unmap_cursor = 0;

        for (int global_req = 0; global_req < NUM_REQUESTS; global_req++) {
            const int cur_page = global_req / REQ_PER_PAGE;

            // ---------- [Guest OS] Stage1 unmap + 立即完整失效 ----------
            // 条件: 页 P 的 8 个请求都已响应完成 + 安全余量(与场景11 完全一致)
            while ((next_unmap_page + 1) * REQ_PER_PAGE + UNMAP_RESP_MARGIN
                       <= response_count
                   && next_unmap_page < cur_page) {
                const double t_start = sc_core::sc_time_stamp().to_seconds() * 1e9;
                const uint64_t gva = IOVA_BASE
                                   + (uint64_t)page_indices[next_unmap_page] * 0x1000;

                // 步骤1: 移除 Stage1 PTE (GVA->GPA)
                unmap_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, gva, DC6.iohgatp, 0);
                virt_stats.guest_unmaps++;
                // 步骤2: fence 内存屏障
                virt_stats.fence_ns += guest_fence_latency_ns;
                wait(guest_fence_latency_ns, SC_NS);

                // [STRICT 核心] 步骤3~11: 立即发起带 ADDR 的精准失效并等待完成
                printf("[t=%llu ns][GUEST] STRICT unmap GVA=0x%lx -> immediate "
                       "IOTINVAL.VMA(AV=1, ADDR=GVA)\n",
                       (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
                       (unsigned long)gva);
                fflush(stdout);
                guest_issue_iotinval_vma(iommu_ptr, VM_GSCID, VM_PSCID,
                                         1 /*AV=1*/, gva);
                // 步骤11 已在封装内完成 -> 此刻才可安全回收该 GPA

                // ---------- [VMM] Stage2 unmap + 立即失效 ----------
                if (virt_stats.guest_unmaps % VMM_UNMAP_EVERY == 0
                    && vmm_unmap_cursor < next_unmap_page) {
                    const uint64_t gpa = (uint64_t)vmm_unmap_cursor * GPA_STRIDE;
                    unmap_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa);
                    virt_stats.vmm_unmaps++;
                    vmm_unmap_cursor++;
                    vmm_issue_iotinval_gvma(iommu_ptr, VM_GSCID);   // 立即, 不批量
                }

                const double dt = sc_core::sc_time_stamp().to_seconds() * 1e9 - t_start;
                virt_stats.unmap_path_total_ns += dt;
                virt_stats.unmap_path_samples++;
                if (dt > virt_stats.unmap_path_max_ns) virt_stats.unmap_path_max_ns = dt;

                next_unmap_page++;
            }

            // ---------- 正常请求注入 ----------
            const int offset_in_page = global_req % REQ_PER_PAGE;
            const uint64_t iova = IOVA_BASE + (uint64_t)page_indices[cur_page] * 0x1000
                                + offset_in_page * 0x200;

            trans_array[global_req] = new tlm_generic_payload();
            sc_time delay = SC_ZERO_TIME;
            unsigned char* data = new unsigned char[1024]();
            trans_array[global_req]->set_address(iova);
            trans_array[global_req]->set_data_ptr(data);
            trans_array[global_req]->set_data_length(512);
            trans_array[global_req]->set_command(TLM_READ_COMMAND);
            ext_array[global_req] = new PayloadExtention();
            ext_array[global_req]->requester_id = 0x0A;
            ext_array[global_req]->pid_valid = 0;
            ext_array[global_req]->process_id = 0;
            ext_array[global_req]->exec_req = 0;
            ext_array[global_req]->priv_req = 0;
            ext_array[global_req]->no_write = 1;
            ext_array[global_req]->at = 0;
            trans_array[global_req]->set_extension(ext_array[global_req]);
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            axi_master_to_pcie_noc_0_socket->nb_transport_fw(*trans_array[global_req],
                                                            phase, delay);

            if ((global_req + 1) % 1000 == 0) {
                printf("[TEST] Progress: %d/%d injected (guest_unmaps=%lu, "
                       "inval_cmds=%lu)\n",
                       global_req + 1, NUM_REQUESTS,
                       (unsigned long)virt_stats.guest_unmaps,
                       (unsigned long)virt_stats.guest_inval_cmds);
                fflush(stdout);
            }
        }

        printf("[TEST] All %d requests injected. Waiting for responses...\n", NUM_REQUESTS);
        fflush(stdout);

        int stall = 0, last = response_count;
        while (response_count < NUM_REQUESTS) {
            wait(1000, SC_NS);
            if (response_count == last) {
                if (++stall > 50) {
                    printf("[TEST] WARNING: no progress 50us, got %d/%d\n",
                           response_count, NUM_REQUESTS);
                    break;
                }
            } else { stall = 0; last = response_count; }
            if (response_count % 1000 == 0) {
                printf("[TEST] Progress: %d/%d responses\n", response_count, NUM_REQUESTS);
                fflush(stdout);
            }
        }

        // ---------- 功能校验 ----------
        int pass_count = 0;
        for (int global_req = 0; global_req < NUM_REQUESTS; global_req++) {
            const int page_idx = global_req / REQ_PER_PAGE;
            const int offset_in_page = global_req % REQ_PER_PAGE;
            const uint64_t iova = IOVA_BASE + (uint64_t)page_indices[page_idx] * 0x1000
                                + offset_in_page * 0x200;
            const uint64_t exp_pa = (uint64_t)page_idx * GPA_STRIDE + PA_OFFSET
                                  + (iova & 0xFFF);
            if (trans_array[global_req]->get_response_status() == tlm::TLM_OK_RESPONSE &&
                trans_array[global_req]->get_address() == exp_pa) {
                pass_count++;
            } else {
                printf("[TEST] FAIL req %d: IOVA=0x%lx exp=0x%lx got=0x%lx status=%s\n",
                       global_req, iova, exp_pa, trans_array[global_req]->get_address(),
                       trans_array[global_req]->get_response_string().c_str());
            }
        }
        printf("\n[TEST] Validation: %d/%d passed\n", pass_count, NUM_REQUESTS);
        printf("[TEST] %s: STRICT-mode virtualized invalidation\n",
               (pass_count == NUM_REQUESTS) ? "PASS" : "FAIL");

        for (int i = 0; i < NUM_REQUESTS; i++) {
            trans_array[i]->clear_extension(ext_array[i]);
            delete ext_array[i];
            if (trans_array[i]->get_data_ptr()) delete[] trans_array[i]->get_data_ptr();
            delete trans_array[i];
        }

        // ---------- 报告 ----------
        print_virt_inval_stats("STRICT (6.5.4 场景二)");

        // 模式区分性自检
        const uint64_t pt_drops = iommu_ptr->cache_sub.pt_cache().lazy_inval_drops();
        const uint64_t wk_drops = iommu_ptr->cache_sub.walker_cache().lazy_inval_drops();
        printf("\n========== STRICT Mode Self-Check ==========\n");
        printf("  guest_inval_cmds == guest_unmaps     : %s (%lu vs %lu)\n",
               virt_stats.guest_inval_cmds == virt_stats.guest_unmaps ? "PASS" : "FAIL",
               (unsigned long)virt_stats.guest_inval_cmds,
               (unsigned long)virt_stats.guest_unmaps);
        printf("  no flush queue (fq_drains == 0)      : %s (%lu)\n",
               virt_stats.fq_drains == 0 ? "PASS" : "FAIL",
               (unsigned long)virt_stats.fq_drains);
        printf("  PT lazy_inval_drops == 0 (precise!)  : %s (PT=%lu)\n",
               pt_drops == 0 ? "PASS" : "FAIL", (unsigned long)pt_drops);
        printf("  (Walker lazy drops: %lu — VMM GVMA(AV=0) 仍走LAZY, 非Guest侧)\n",
               (unsigned long)wk_drops);
        printf("===========================================\n");

        printf("\n========== PTW DDR Access Statistics ==========\n");
        printf("  PTW total completed tasks: %lu\n", (unsigned long)iommu_ptr->ptw_total_completed);
        printf("  PTW total DDR reads:       %lu\n", (unsigned long)iommu_ptr->ptw_total_ddr_reads);
        if (iommu_ptr->ptw_total_completed > 0)
            printf("  PTW avg DDR reads/task:    %.2f\n",
                   (double)iommu_ptr->ptw_total_ddr_reads / iommu_ptr->ptw_total_completed);
        printf("================================================\n");

        iommu_ptr->print_cache_statistics();
        sc_core::sc_stop();
        return;
    }
}
