#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"
#include <cstdio>
#include <iostream>
using namespace std;

// ============================================================
// [失效] Test Scenario: Cache Invalidation (DC/PC/PT/Walker + CQ)
//   iosatp = Sv48, iohgatp = Sv48x4 (两阶段, 复用场景5页表构造)
//   小规模访问(每轮 BATCH 个 512B 顺序读), 每轮之间下发一条失效指令,
//   通过 "PTW 完成任务数增量" 判定缓存是否真的被失效:
//     - 未失效时重复访问同一批 IOVA -> 全命中, PTW 增量应为 0
//     - 失效生效后重复访问 -> 需重新 walk, PTW 增量 > 0
//
//   覆盖用例:
//     C1  IOTINVAL.VMA GV=0 AV=0 PSCV=0        清表(GLOBAL)
//     C2  IOTINVAL.VMA GV=1 AV=0 PSCV=0        LAZY(仅GSCID) 记录LIB
//     C3  IOTINVAL.VMA GV=1 AV=0 PSCV=1        LAZY(GSCID+PSCID)
//     C4  IOTINVAL.VMA GV=0 AV=0 PSCV=1        LAZY(仅PSCID)
//     C5  IOTINVAL.VMA GV=1 AV=1 PSCV=1 +ADDR  小范围枚举失效(SCAN_RANGE)
//     C6  IOTINVAL.GVMA GV=1 AV=0              LAZY(仅GSCID, 二阶段)
//     C7  IOTINVAL.GVMA GV=0                   清表
//     C8  IODIR.INVAL_PDT DV=1                 PC 精准失效
//     C9  IODIR.INVAL_DDT DV=1                 DC 精准失效 + PC 关联
//     C10 IODIR.INVAL_DDT DV=0                 DC 全清 + PC 全清
//     C11 LAZY 连续下发直至 VN 回绕             触发批量扫表
//
//   每条失效指令后紧跟 IOFENCE.C(与 Qemu lazy 模式 trace 一致), 确保
//   IOMMU 已执行完失效指令再继续后续访问。
// ============================================================
void RP_Module::send_translation_request_1_thread()
{
    while (true)
    {
        wait(10, SC_NS);

        printf("\n========== IOMMU Cache Invalidation Test (device 0x0A, two-stage) ==========\n");

        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));
        // [失效] 使能命令队列: 失效指令必须经 CQ 才能被 IOMMU 执行
        // (未使能时 process_commands 因 cqon=0/cqen=0 直接返回, 失效为空操作)
        fail_if((enable_cq(iommu_ptr, 1) < 0));
        printf("[INVAL_TEST] Command queue enabled (1 page)\n");
        extern uint64_t next_free_page;
        next_free_page = 240;

        const uint64_t IOVA_BASE       = 0x100000;
        const uint64_t PA_OFFSET       = 0x10000;
        const uint64_t GPA_STRIDE      = 0x200000;
        const int      NUM_UNIQUE_GPAS = 20;
        const int      BATCH           = 32;      // 每轮访问请求数(512B步进)
        const int      TOTAL_IOVA_PAGES = 8;      // 32*512B = 16KB -> 4页, 留余量
        const uint32_t TEST_GSCID      = 1;       // add_device 使用 GSCID=1
        const uint32_t TEST_PSCID      = 0;       // PDTP_Bare -> PSCID=0

        extern uint64_t next_free_gpage[65536];
        uint64_t test_max_gppn = (uint64_t)(NUM_UNIQUE_GPAS - 1) * (GPA_STRIDE / PAGESIZE);
        next_free_gpage[1] = ((test_max_gppn + 0xFFFF) / 0x10000) * 0x10000;

        uint64_t dc_addr = add_device(iommu_ptr, 0x0A, 1, 0, 0, 0, 0, 0,
                                      1, 1, 0, 0, 0,
                                      IOHGATP_Sv48x4, IOSATP_Sv48, PDTP_Bare,
                                      MSIPTP_Off, 0, 0, 0);
        device_context_t DC6;
        read_memory_test_rp(dc_addr, sizeof(device_context_t), (char*)&DC6);
        printf("[INVAL_TEST] DC addr=0x%lx, iohgatp.MODE=%d, iosatp.MODE=%d, GSCID=%d\n",
               dc_addr, DC6.iohgatp.MODE, DC6.fsc.iosatp.MODE, (int)DC6.iohgatp.GSCID);

        // VS root page 的 G-stage 映射
        {
            uint64_t vs_root_gpa = (uint64_t)DC6.fsc.iosatp.PPN * PAGESIZE;
            gpte_t vs_root_gpte;
            vs_root_gpte.raw = 0;
            vs_root_gpte.V = 1; vs_root_gpte.R = 1; vs_root_gpte.W = 1;
            vs_root_gpte.U = 1; vs_root_gpte.A = 1; vs_root_gpte.D = 1;
            vs_root_gpte.PBMT = PMA;
            vs_root_gpte.PPN = get_free_ppn(1);
            unsigned char zero_page[4096] = {0};
            write_memory_test_rp((char*)zero_page, vs_root_gpte.PPN * PAGESIZE, 4096);
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, vs_root_gpa, vs_root_gpte, 0);
        }

        spte_t pte6;
        pte6.raw = 0;
        pte6.V = 1; pte6.R = 1; pte6.W = 1; pte6.U = 1; pte6.A = 1; pte6.D = 1;
        pte6.PBMT = PMA;

        gpte_t gpte;
        gpte.raw = 0;
        gpte.V = 1; gpte.R = 1; gpte.W = 1; gpte.U = 1; gpte.A = 1; gpte.D = 1;
        gpte.PBMT = PMA;

        for (int g = 0; g < NUM_UNIQUE_GPAS; g++) {
            uint64_t gpa_page = (uint64_t)g * GPA_STRIDE;
            gpte.PPN = (gpa_page + PA_OFFSET) / PAGESIZE;
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa_page, gpte, 0);
        }
        for (int p = 0; p < TOTAL_IOVA_PAGES; p++) {
            uint64_t iova_page = IOVA_BASE + (uint64_t)p * 0x1000;
            uint64_t gpa_page = (uint64_t)(p % NUM_UNIQUE_GPAS) * GPA_STRIDE;
            pte6.PPN = gpa_page / PAGESIZE;
            add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, iova_page, pte6, 0, DC6.iohgatp, 0);
        }
        printf("[INVAL_TEST] Page tables built: %d IOVA pages, %d unique GPAs\n",
               TOTAL_IOVA_PAGES, NUM_UNIQUE_GPAS);

        // 初始清空所有缓存
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);
        iommu_ptr->next_task_id = 1;

        // ============================================================
        // 访问一批请求并返回 (PTW完成任务增量, 校验通过数)
        // ============================================================
        int total_pass = 0;
        int total_fail = 0;
        auto run_batch = [&](const char* tag, uint64_t* out_ptw_delta) -> int {
            const uint64_t ptw_before = iommu_ptr->ptw_total_completed;
            const int base_resp = response_count;

            tlm_generic_payload* tr[BATCH];
            PayloadExtention*    ex[BATCH];
            for (int i = 0; i < BATCH; i++) {
                uint64_t iova = IOVA_BASE + (uint64_t)(i + 1) * 0x200;
                tr[i] = new tlm_generic_payload();
                unsigned char* data = new unsigned char[1024]();
                tr[i]->set_address(iova);
                tr[i]->set_data_ptr(data);
                tr[i]->set_data_length(512);
                tr[i]->set_command(TLM_READ_COMMAND);
                ex[i] = new PayloadExtention();
                ex[i]->requester_id = 0x0A;
                ex[i]->pid_valid = 0;
                ex[i]->process_id = 0;
                ex[i]->exec_req = 0;
                ex[i]->priv_req = 0;
                ex[i]->no_write = 1;
                ex[i]->at = 0;
                tr[i]->set_extension(ex[i]);
                tlm::tlm_phase ph = tlm::BEGIN_REQ;
                sc_time dly = SC_ZERO_TIME;
                axi_master_to_pcie_noc_0_socket->nb_transport_fw(*tr[i], ph, dly);
            }

            int stall = 0, last = response_count;
            while (response_count - base_resp < BATCH) {
                wait(1000, SC_NS);
                if (response_count == last) {
                    if (++stall > 50) {
                        printf("[INVAL_TEST] %s: TIMEOUT, got %d/%d\n",
                               tag, response_count - base_resp, BATCH);
                        break;
                    }
                } else { stall = 0; last = response_count; }
            }

            int pass = 0;
            for (int i = 0; i < BATCH; i++) {
                uint64_t iova = IOVA_BASE + (uint64_t)(i + 1) * 0x200;
                uint64_t page_idx = (iova - IOVA_BASE) / 0x1000;
                uint64_t exp_pa = ((uint64_t)(page_idx % NUM_UNIQUE_GPAS) * GPA_STRIDE)
                                  + PA_OFFSET + (iova & 0xFFF);
                if (tr[i]->get_response_status() == tlm::TLM_OK_RESPONSE &&
                    tr[i]->get_address() == exp_pa) {
                    pass++;
                } else {
                    printf("[INVAL_TEST] %s FAIL req %d: IOVA=0x%lx exp=0x%lx got=0x%lx\n",
                           tag, i, iova, exp_pa, tr[i]->get_address());
                }
                tr[i]->clear_extension(ex[i]);
                delete ex[i];
                if (tr[i]->get_data_ptr()) delete[] tr[i]->get_data_ptr();
                delete tr[i];
            }

            const uint64_t delta = iommu_ptr->ptw_total_completed - ptw_before;
            if (out_ptw_delta) *out_ptw_delta = delta;
            printf("[INVAL_TEST] %-42s pass=%2d/%2d, PTW tasks delta=%lu\n",
                   tag, pass, BATCH, (unsigned long)delta);
            fflush(stdout);
            if (pass == BATCH) total_pass++; else total_fail++;
            return pass;
        };

        // 检查失效是否生效: delta>0 表示确实重新 walk 了
        auto check = [&](const char* name, uint64_t delta, bool expect_refill) {
            const bool ok = expect_refill ? (delta > 0) : (delta == 0);
            printf("[INVAL_TEST] %-10s %s (delta=%lu, expect %s)\n",
                   name, ok ? "PASS" : "FAIL", (unsigned long)delta,
                   expect_refill ? "refill(>0)" : "all-hit(==0)");
            if (ok) total_pass++; else total_fail++;
            fflush(stdout);
        };

        uint64_t d = 0;
        // ---------- 预热: 首次访问必然 miss 并 walk ----------
        run_batch("WARMUP (cold cache)", &d);
        check("WARMUP", d, true);

        // ---------- 基线: 未失效时重复访问应全命中 ----------
        run_batch("REPEAT (no invalidation)", &d);
        check("NO-INVAL", d, false);

        // ---------- C1: IOTINVAL.VMA 000 清表 ----------
        printf("\n[INVAL_TEST] --- C1: IOTINVAL.VMA GV=0 AV=0 PSCV=0 (clear all) ---\n");
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C1 VMA clear-table", &d);
        check("C1", d, true);

        // ---------- C2: IOTINVAL.VMA 100 LAZY(仅GSCID) ----------
        printf("\n[INVAL_TEST] --- C2: IOTINVAL.VMA GV=1 AV=0 PSCV=0 (LAZY gscid) ---\n");
        run_batch("re-warm before C2", &d);
        iotinval(iommu_ptr, VMA, 1, 0, 0, TEST_GSCID, 0, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C2 VMA lazy(gscid)", &d);
        check("C2", d, true);

        // ---------- C3: IOTINVAL.VMA 101 LAZY(GSCID+PSCID) ----------
        printf("\n[INVAL_TEST] --- C3: IOTINVAL.VMA GV=1 AV=0 PSCV=1 (LAZY gscid+pscid) ---\n");
        run_batch("re-warm before C3", &d);
        iotinval(iommu_ptr, VMA, 1, 0, 1, TEST_GSCID, TEST_PSCID, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C3 VMA lazy(gscid+pscid)", &d);
        check("C3", d, true);

        // ---------- C4: IOTINVAL.VMA 001 LAZY(仅PSCID) ----------
        printf("\n[INVAL_TEST] --- C4: IOTINVAL.VMA GV=0 AV=0 PSCV=1 (LAZY pscid) ---\n");
        run_batch("re-warm before C4", &d);
        iotinval(iommu_ptr, VMA, 0, 0, 1, 0, TEST_PSCID, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C4 VMA lazy(pscid)", &d);
        check("C4", d, true);

        // ---------- C5: IOTINVAL.VMA 111 小范围枚举失效(指定ADDR) ----------
        // 仅失效 IOVA_BASE 所在页; 该页覆盖 batch 中前 7 个请求(0x200~0xE00)
        printf("\n[INVAL_TEST] --- C5: IOTINVAL.VMA GV=1 AV=1 PSCV=1 ADDR=0x%lx (SCAN_RANGE) ---\n",
               IOVA_BASE);
        run_batch("re-warm before C5", &d);
        iotinval(iommu_ptr, VMA, 1, 1, 1, TEST_GSCID, TEST_PSCID, IOVA_BASE);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C5 VMA addr-range", &d);
        check("C5", d, true);

        // ---------- C6: IOTINVAL.GVMA GV=1 AV=0 ----------
        printf("\n[INVAL_TEST] --- C6: IOTINVAL.GVMA GV=1 AV=0 (LAZY gscid, stage2) ---\n");
        run_batch("re-warm before C6", &d);
        iotinval(iommu_ptr, GVMA, 1, 0, 0, TEST_GSCID, 0, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C6 GVMA lazy(gscid)", &d);
        check("C6", d, true);

        // ---------- C7: IOTINVAL.GVMA GV=0 清表 ----------
        printf("\n[INVAL_TEST] --- C7: IOTINVAL.GVMA GV=0 (clear all) ---\n");
        run_batch("re-warm before C7", &d);
        iotinval(iommu_ptr, GVMA, 0, 0, 0, 0, 0, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C7 GVMA clear-table", &d);
        check("C7", d, true);

        // ---------- C8: IODIR.INVAL_PDT DV=1 (PC 精准失效) ----------
        // PDTP_Bare 下 PC Cache 无条目, 该指令不应影响 PT/Walker -> 应仍全命中
        printf("\n[INVAL_TEST] --- C8: IODIR.INVAL_PDT DV=1 (PC precise, no PT/Walker impact) ---\n");
        run_batch("re-warm before C8", &d);
        iodir(iommu_ptr, INVAL_PDT, 1, 0x0A, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C8 IODIR.INVAL_PDT", &d);
        check("C8", d, false);   // 按 spec: 仅失效 PDT 目录缓存, 不动 PT/Walker

        // ---------- C9: IODIR.INVAL_DDT DV=1 (DC 精准 + PC 关联) ----------
        // DC 失效导致下次翻译需重新读 DDT, 但 PT/Walker 未失效 -> PT 仍命中
        printf("\n[INVAL_TEST] --- C9: IODIR.INVAL_DDT DV=1 (DC precise + PC cascade) ---\n");
        run_batch("re-warm before C9", &d);
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C9 IODIR.INVAL_DDT(DV=1)", &d);
        check("C9", d, false);   // PT/Walker 未失效 -> PTW 不应被再次触发

        // ---------- C10: IODIR.INVAL_DDT DV=0 (DC/PC 全清) ----------
        printf("\n[INVAL_TEST] --- C10: IODIR.INVAL_DDT DV=0 (DC+PC global) ---\n");
        run_batch("re-warm before C10", &d);
        iodir(iommu_ptr, INVAL_DDT, 0, 0, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C10 IODIR.INVAL_DDT(DV=0)", &d);
        check("C10", d, false);  // 同上, 不触及 PT/Walker

        // ---------- C11: 连续 LAZY 直至 VN 回绕(触发批量扫表) ----------
        // VN_MAX=15: 对同一 tag 连续下发 16 次即回绕一次
        printf("\n[INVAL_TEST] --- C11: 16x LAZY VMA(gscid) -> VN wrap-around batch sweep ---\n");
        run_batch("re-warm before C11", &d);
        for (int k = 0; k < 16; k++) {
            iotinval(iommu_ptr, VMA, 1, 0, 0, TEST_GSCID, 0, 0);
        }
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        run_batch("after C11 VN wrap sweep", &d);
        check("C11", d, true);

        printf("\n========== Cache Invalidation Test Summary ==========\n");
        printf("  Checks passed: %d\n", total_pass);
        printf("  Checks failed: %d\n", total_fail);
        printf("  Result: %s\n", (total_fail == 0) ? "ALL PASS" : "SOME FAILED");
        printf("====================================================\n");

        iommu_ptr->print_cache_statistics();
        sc_core::sc_stop();
        return;
    }
}
