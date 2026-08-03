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
// Scenario 11: 虚拟化两级Stage — Lazy 模式 Cache Invalidate
//   对应规范 6.5.3 场景二(iohgatp.mode!=Bare 且 iosatp.mode!=Bare)
//
// 负载(与场景7/10 同源, 相同种子 -> IOVA 序列一致, 可直接对比):
//   4KB随机读 + 两阶段翻译, 1250随机页 x 8req = 10000包
//   128GB/s入口/出口 + 全局并发512 + PTW=4 + D=3预取 + S2开启
//
// 双失效发起方:
//   [Guest OS] 管理 Stage1(GVA->GPA)。unmap 后把 GVA 累积到 Guest Flush Queue,
//              **不立即失效**; Drain 触发时(深度/超时)经 vIOMMU 虚拟CQ 发起
//              **一条** IOTINVAL.VMA(GV=1, GSCID, PSCID, **AV=0**) 覆盖全部累积GVA,
//              由 VMM 拦截(trap-and-emulate)转换后写入物理IOMMU的CQ。
//              -> 物理侧走 LAZY: 记录 LIB + 全局VN++, PT/Walker 延迟失效
//   [VMM]      管理 Stage2(GPA->SPA)。unmap 后累积, 达批次时发 IOTINVAL.GVMA(GV=1,GSCID)
//
// unmap 目标选择: 只 unmap 已注入完全部 8 个请求的页(滞后 UNMAP_LAG_PAGES 页),
//   对应"DMA 完成后回收 buffer"的真实语义; 该页后续不再访问 -> 功能校验仍 100%
//
// 验证重点: Lazy 的批量合并率、延迟失效(LIB/VN)生效次数、unmap关键路径延迟低
// ============================================================
void RP_Module::send_translation_request_1_thread()
{
    while (true)
    {
        wait(10, SC_NS);

        printf("\n========== Scenario 11: Virtualized Two-Stage — LAZY Mode Invalidation ==========\n");

        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));
        fail_if((enable_cq(iommu_ptr, 1) < 0));
        printf("[VIRT] Command queue enabled (physical IOMMU CQ)\n");

        extern uint64_t next_free_page;
        next_free_page = 240;

        // ---------- 负载参数(与场景7/10 一致) ----------
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

        // ---------- 虚拟化 / Lazy 参数 ----------
        const uint32_t VM_GSCID  = 1;   // add_device 使用 GSCID=1 标识该VM
        const uint32_t VM_PSCID  = 0;   // PDTP_Bare -> PSCID=0
        // [关键] unmap 必须等该页 8 个请求全部**响应完成**才能回收 —— 这是
        //   "DMA 完成后回收 buffer" 的真实语义。若按注入序号滞后, 由于端到端
        //   延迟(约数万ns)远大于注入间隔(约4ns), 会 unmap 掉仍在翻译中的页,
        //   导致翻译失败(PA=0)。故门控条件为 response_count。
        // [v2] 加大安全余量至 512 请求，确保 unmap 时该页早已完成
        const int UNMAP_RESP_MARGIN = 512;
#ifndef TEST_CFG_VIRT_FQ_DEPTH
        guest_fq_depth = 32;
#else
        guest_fq_depth = TEST_CFG_VIRT_FQ_DEPTH;
#endif
#ifndef TEST_CFG_VMM_TRAP_NS
        vmm_trap_latency_ns = 2000.0;
#else
        vmm_trap_latency_ns = (double)TEST_CFG_VMM_TRAP_NS;
#endif
#ifndef TEST_CFG_VIRT_FQ_TIMEOUT_NS
        guest_fq_timeout_ns = 10000.0;
#else
        guest_fq_timeout_ns = (double)TEST_CFG_VIRT_FQ_TIMEOUT_NS;
#endif
        const int VMM_UNMAP_EVERY = 4;  // 每 N 个 Guest unmap 触发 1 次 VMM Stage2 unmap
        const int VMM_BATCH       = 8;  // VMM 累积 N 个 Stage2 unmap 后批量失效

        printf("[VIRT] Lazy config: Guest FQ depth=%u, FQ timeout=%.0f ns, "
               "VMM trap=%.0f ns, unmap gated by response_count (+%d margin)\n",
               guest_fq_depth, guest_fq_timeout_ns, vmm_trap_latency_ns,
               UNMAP_RESP_MARGIN);
        printf("[VIRT] VMM Stage2 unmap: every %d guest unmaps, batch=%d\n",
               VMM_UNMAP_EVERY, VMM_BATCH);

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

        // VS root page 的 G-stage 映射
        {
            uint64_t vs_root_gpa = (uint64_t)DC6.fsc.iosatp.PPN * PAGESIZE;
            gpte_t g; g.raw = 0;
            g.V = 1; g.R = 1; g.W = 1; g.U = 1; g.A = 1; g.D = 1; g.PBMT = PMA;
            g.PPN = get_free_ppn(1);
            unsigned char zp[4096] = {0};
            write_memory_test_rp((char*)zp, g.PPN * PAGESIZE, 4096);
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, vs_root_gpa, g, 0);
        }

        // IOFENCE.C AV=1 的完成标志页(物理IOMMU经AXI写此处, Guest轮询该处)
        iofence_flag_addr = get_free_ppn(1) * PAGESIZE;
        printf("[VIRT] IOFENCE.C completion flag addr = 0x%lx\n",
               (unsigned long)iofence_flag_addr);

        // ---------- 随机页索引(与场景7 同种子) ----------
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
        guest_fq_last_drain_ns = sc_core::sc_time_stamp().to_seconds() * 1e9;

        // ============================================================
        // 主测试
        // ============================================================
        printf("\n========== %d Requests + LAZY-mode unmap/invalidation ==========\n",
               NUM_REQUESTS);
        response_count = 0;
        iommu_ptr->steady_start_count = NUM_REQUESTS * STEADY_STATE_START_PERCENT / 100;
        iommu_ptr->steady_end_count   = NUM_REQUESTS * STEADY_STATE_END_PERCENT / 100;

        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];

        int next_unmap_page = 0;      // 下一个待 unmap 的页序号
        int vmm_pending = 0;          // VMM 侧累积待批量失效的 Stage2 unmap 数
        int vmm_unmap_cursor = 0;     // VMM 侧 unmap 的页游标

        for (int global_req = 0; global_req < NUM_REQUESTS; global_req++) {
            const int cur_page = global_req / REQ_PER_PAGE;

            // ---------- [Guest OS] Stage1 unmap: 只回收已全部响应完成的页 ----------
            // 条件: 页 P 的 8 个请求(索引 P*8 .. P*8+7)都已响应 + 安全余量
            while ((next_unmap_page + 1) * REQ_PER_PAGE + UNMAP_RESP_MARGIN
                       <= response_count
                   && next_unmap_page < cur_page) {
                const double t_start = sc_core::sc_time_stamp().to_seconds() * 1e9;
                const uint64_t gva = IOVA_BASE
                                   + (uint64_t)page_indices[next_unmap_page] * 0x1000;

                // 步骤1: 移除 Stage1 PTE (GVA->GPA)
                unmap_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, gva, DC6.iohgatp, 0);
                virt_stats.guest_unmaps++;
                // 步骤2(前置): fence 确保页表修改对IOMMU可见
                virt_stats.fence_ns += guest_fence_latency_ns;
                wait(guest_fence_latency_ns, SC_NS);

                // [LAZY 核心] 不立即失效, 把 GVA 记入 Guest Flush Queue
                guest_fq.push_back(gva);

                // ---------- [VMM] Stage2 unmap(按周期), 累积后批量失效 ----------
                if (virt_stats.guest_unmaps % VMM_UNMAP_EVERY == 0
                    && vmm_unmap_cursor < next_unmap_page) {
                    const uint64_t gpa = (uint64_t)vmm_unmap_cursor * GPA_STRIDE;
                    unmap_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa);
                    virt_stats.vmm_unmaps++;
                    vmm_unmap_cursor++;
                    if (++vmm_pending >= VMM_BATCH) {
                        vmm_issue_iotinval_gvma(iommu_ptr, VM_GSCID);   // 批量 Stage2 失效
                        vmm_pending = 0;
                    }
                }

                // Flush Queue Drain 判定(深度 或 超时)
                if (guest_fq_should_drain()) {
                    guest_fq_drain(iommu_ptr, VM_GSCID, VM_PSCID);
                }

                // unmap 关键路径延迟: unmap -> (Lazy下)入队即返回, GPA稍后批量回收
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
                printf("[TEST] Progress: %d/%d injected (guest_unmaps=%lu, fq=%zu, drains=%lu)\n",
                       global_req + 1, NUM_REQUESTS,
                       (unsigned long)virt_stats.guest_unmaps, guest_fq.size(),
                       (unsigned long)virt_stats.fq_drains);
                fflush(stdout);
            }
        }

        // 收尾: 强制 drain 剩余 Flush Queue + VMM 剩余批次
        if (!guest_fq.empty()) {
            virt_stats.fq_drain_forced++;
            guest_fq_drain(iommu_ptr, VM_GSCID, VM_PSCID);
        }
        if (vmm_pending > 0) {
            vmm_issue_iotinval_gvma(iommu_ptr, VM_GSCID);
            vmm_pending = 0;
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
        printf("[TEST] %s: LAZY-mode virtualized invalidation\n",
               (pass_count == NUM_REQUESTS) ? "PASS" : "FAIL");

        for (int i = 0; i < NUM_REQUESTS; i++) {
            trans_array[i]->clear_extension(ext_array[i]);
            delete ext_array[i];
            if (trans_array[i]->get_data_ptr()) delete[] trans_array[i]->get_data_ptr();
            delete trans_array[i];
        }

        // ---------- 报告 ----------
        print_virt_inval_stats("LAZY (6.5.3 场景二)");

        // 模式区分性自检
        const uint64_t pt_drops = iommu_ptr->cache_sub.pt_cache().lazy_inval_drops();
        const uint64_t wk_drops = iommu_ptr->cache_sub.walker_cache().lazy_inval_drops();
        printf("\n========== LAZY Mode Self-Check ==========\n");
        printf("  fq_drains > 0                        : %s (%lu)\n",
               virt_stats.fq_drains > 0 ? "PASS" : "FAIL",
               (unsigned long)virt_stats.fq_drains);
        printf("  guest_inval_cmds == fq_drains        : %s (%lu vs %lu)\n",
               virt_stats.guest_inval_cmds == virt_stats.fq_drains ? "PASS" : "FAIL",
               (unsigned long)virt_stats.guest_inval_cmds,
               (unsigned long)virt_stats.fq_drains);
        printf("  lazy_inval_drops(PT+Walker) > 0      : %s (PT=%lu, WK=%lu)\n",
               (pt_drops + wk_drops) > 0 ? "PASS" : "FAIL",
               (unsigned long)pt_drops, (unsigned long)wk_drops);
        printf("  batch merge ratio (GVA/drain)        : %.1f\n",
               virt_stats.fq_drains ? (double)virt_stats.fq_batched_gvas / virt_stats.fq_drains : 0.0);
        printf("==========================================\n");

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
