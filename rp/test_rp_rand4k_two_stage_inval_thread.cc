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
// Test Scenario 10: 场景7 + 运行期随机穿插缓存失效命令
//
// 基础负载完全复用场景7(逐字节等价, 保证可与场景7直接对比):
//   4KB随机读 + 两阶段翻译(iosatp=Sv48, iohgatp=Sv48x4)
//   1250个随机4KB页(16MB范围内, 打乱访问顺序), 8 req/page -> 10000包
//   GPA = loop_index x 2MB, SPA = GPA + 0x10000
//   128GB/s入口/出口 + 全局并发512 + Dedup Buffer 512
//   DC/PC read_set=1(4ns) + PT/Dedup双多RAM(4组) + PTW=4 + D=3预取 + S2开启
//
// 新增: 在 10000 包注入过程中随机穿插 DC / PC / PT / Walker 失效命令
//   - 穿插时机随机: 平均每 TEST_CFG_INVAL_PERIOD_REQS 个请求一次,
//     实际间隔在 [P/2, 3P/2) 内均匀抖动
//   - 命令类型随机(加权, 权重参照 Qemu lazy 模式 trace: 扫表类占多数):
//       35%  IOTINVAL.VMA  GV=1 AV=0 PSCV=0   -> LAZY(仅GSCID)   PT+Walker
//       20%  IOTINVAL.VMA  GV=1 AV=0 PSCV=1   -> LAZY(GSCID+PSCID)
//       20%  IOTINVAL.GVMA GV=1 AV=0          -> LAZY(仅GSCID, 二阶段)
//       10%  IOTINVAL.VMA  GV=1 AV=1 PSCV=1   -> 小范围枚举失效(SCAN_RANGE)
//        5%  IOTINVAL.VMA  GV=1 AV=1 PSCV=1 NL=1 -> PT + Walker 全级别
//        5%  IODIR.INVAL_DDT DV=1             -> DC 精准 + PC 关联
//        5%  IODIR.INVAL_PDT DV=1             -> PC 精准
//   - 每条 IOMMU 失效命令前先做轻量 CPU 侧维护(SFENCE.VMA), 后跟 IOFENCE.C,
//     与真实 OS 行为及 Qemu trace 的 "失效指令 + iofence" 模式一致
//
// 验证目标:
//   1) 功能正确性: 10000 包在持续失效扰动下仍必须 100% 翻译正确
//   2) PT/Walker 延迟失效(LIB/VN)策略在真实负载下确实生效 ->
//      通过 lazy_inval_drops() 计数量化(>0 即证明旁路比对丢弃了失效数据)
//   3) 性能影响: 与场景7 基线对比稳态IOPS/PTW/命中率的下降幅度
//
// 注: 本文件为独立新增, 不修改场景5/6/7/8 的测试线程与构建目标。
// ============================================================

// [失效] 独立的线性同余随机数(与 rand() 状态隔离):
// 保证失效注入的随机决策**不影响**页索引生成与访问顺序,
// 从而使本场景的 IOVA 序列与场景7 逐包一致, 性能可直接对比。
namespace {
struct InvalRng {
    uint64_t s;
    explicit InvalRng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint32_t next() {           // xorshift64*
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return (uint32_t)((s * 0x2545F4914F6CDD1DULL) >> 32);
    }
    uint32_t below(uint32_t n) { return n ? (next() % n) : 0; }
};
} // namespace

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

        printf("\n========== Scenario 10: 4KB Random Read + Two-Stage + Runtime Cache Invalidation ==========\n");

        ddtp_t ddtp_check;
        ddtp_check.raw = read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4);
        printf("[DEBUG] Current IOMMU mode before enable_iommu: %d\n", ddtp_check.iommu_mode);

        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));

        // [失效] 使能命令队列: 失效指令必须经 CQ 才会被 IOMMU 执行
        // (未使能时 process_commands 因 cqon=0/cqen=0 直接返回, 失效为空操作)
        fail_if((enable_cq(iommu_ptr, 1) < 0));
        printf("[INVAL] Command queue enabled -> IOTINVAL/IODIR/IOFENCE effective\n");

        extern uint64_t next_free_page;
        next_free_page = 240;

        printf("[DEBUG] IOMMU mode after enable_iommu: %d (expect 2 for DDT_1LVL)\n",
               read_register(&iommu_ptr->iommu_inst, DDTP_OFFSET, 4) & 0x7);

        // ============================================================
        // 地址布局(与场景7 完全一致)
        // ============================================================
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

        // [失效] 失效注入参数
#ifndef TEST_CFG_INVAL_PERIOD_REQS
        const int INVAL_PERIOD = 500;      // 平均每500个请求一次失效 -> 10000包约20次
#else
        const int INVAL_PERIOD = TEST_CFG_INVAL_PERIOD_REQS;
#endif
        const uint32_t TEST_GSCID = 1;     // add_device 使用 GSCID=1
        const uint32_t TEST_PSCID = 0;     // PDTP_Bare -> PSCID=0

        printf("\n[TEST] Two-Stage Random 4KB Read Page Table Construction:\n");
        printf("[TEST]   IOVA range: 0x%lx ~ 0x%lx (16MB, %d pages)\n",
               IOVA_BASE, IOVA_BASE + RANGE_16MB, TOTAL_PAGES_IN_RANGE);
        printf("[TEST]   Selecting %d unique random 4KB pages (SHUFFLED access order)\n", PAGES_NEEDED);
        printf("[TEST]   GPA = loop_index x 0x%lx (2MB stride), SPA = GPA + 0x%lx\n",
               GPA_STRIDE, PA_OFFSET);
        printf("[INVAL]  Invalidation injection: avg period=%d reqs -> ~%d invalidations over %d reqs\n",
               INVAL_PERIOD, NUM_REQUESTS / INVAL_PERIOD, NUM_REQUESTS);

        extern uint64_t next_free_gpage[65536];
        uint64_t test_max_gppn = (uint64_t)(PAGES_NEEDED - 1) * (GPA_STRIDE / PAGESIZE);
        uint64_t gppn_offset = ((test_max_gppn + 0xFFFF) / 0x10000) * 0x10000;
        next_free_gpage[1] = gppn_offset;
        printf("[TEST]   GPPN offset: 0x%lx (test GPPN max=0x%lx)\n", gppn_offset, test_max_gppn);

        // ============================================================
        // Configure device 0x0A
        // ============================================================
        printf("\n========== Configuring Device 0x0A: iosatp=Sv48, iohgatp=Sv48x4 ==========\n");
        uint64_t dc6_addr = add_device(iommu_ptr, 0x0A, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Sv48x4, IOSATP_Sv48, PDTP_Bare,
                                       MSIPTP_Off, 0, 0, 0);
        device_context_t DC6;
        read_memory_test_rp(dc6_addr, sizeof(device_context_t), (char*)&DC6);
        printf("[DEV6] DC addr: 0x%lx, iohgatp.MODE=%d, iosatp.MODE=%d, GSCID=%d\n",
               dc6_addr, DC6.iohgatp.MODE, DC6.fsc.iosatp.MODE, (int)DC6.iohgatp.GSCID);

        // VS root page 的 G-stage 映射
        {
            uint64_t vs_root_gpa = (uint64_t)DC6.fsc.iosatp.PPN * PAGESIZE;
            gpte_t vs_root_gpte;
            vs_root_gpte.raw = 0;
            vs_root_gpte.V = 1; vs_root_gpte.R = 1; vs_root_gpte.W = 1;
            vs_root_gpte.X = 0; vs_root_gpte.U = 1; vs_root_gpte.G = 0;
            vs_root_gpte.A = 1; vs_root_gpte.D = 1;
            vs_root_gpte.PBMT = PMA;
            vs_root_gpte.PPN = get_free_ppn(1);
            unsigned char zero_page[4096] = {0};
            write_memory_test_rp((char*)zero_page, vs_root_gpte.PPN * PAGESIZE, 4096);
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, vs_root_gpa, vs_root_gpte, 0);
            printf("[TEST] Created G-stage mapping: GPA=0x%lx -> SPA=0x%lx (VS root page)\n",
                   vs_root_gpa, (uint64_t)vs_root_gpte.PPN * PAGESIZE);
        }

        // ============================================================
        // 生成随机页索引 —— 与场景7 使用相同种子, 保证 IOVA 序列一致
        // ============================================================
        srand(42);
        set<int> page_set;
        while ((int)page_set.size() < PAGES_NEEDED) {
            page_set.insert(rand() % TOTAL_PAGES_IN_RANGE);
        }
        vector<int> page_indices(page_set.begin(), page_set.end());
        srand(123);
        for (int i = page_indices.size() - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            std::swap(page_indices[i], page_indices[j]);
        }
        printf("[TEST]   Generated %d unique random page indices (SHUFFLED)\n",
               (int)page_indices.size());
        printf("[TEST]   Sample: page[0]=IOVA 0x%lx, page[1]=IOVA 0x%lx, page[%d]=IOVA 0x%lx\n",
               IOVA_BASE + (uint64_t)page_indices[0] * 0x1000,
               IOVA_BASE + (uint64_t)page_indices[1] * 0x1000,
               PAGES_NEEDED - 1,
               IOVA_BASE + (uint64_t)page_indices[PAGES_NEEDED - 1] * 0x1000);

        // ============================================================
        // 页表模板 + 建表(与场景7 一致)
        // ============================================================
        spte_t pte6;
        pte6.raw = 0;
        pte6.V = 1; pte6.R = 1; pte6.W = 1; pte6.X = 0;
        pte6.U = 1; pte6.G = 0; pte6.A = 1; pte6.D = 1;
        pte6.PBMT = PMA;

        gpte_t gpte;
        gpte.raw = 0;
        gpte.V = 1; gpte.R = 1; gpte.W = 1; gpte.X = 0;
        gpte.U = 1; gpte.G = 0; gpte.A = 1; gpte.D = 1;
        gpte.PBMT = PMA;

        for (int p = 0; p < PAGES_NEEDED; p++) {
            uint64_t iova_page = IOVA_BASE + (uint64_t)page_indices[p] * 0x1000;
            uint64_t gpa_page  = (uint64_t)p * GPA_STRIDE;
            uint64_t pa_page   = gpa_page + PA_OFFSET;
            pte6.PPN = gpa_page / PAGESIZE;
            add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, iova_page, pte6, 0, DC6.iohgatp, 0);
            gpte.PPN = pa_page / PAGESIZE;
            add_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa_page, gpte, 0);
        }
        printf("[TEST]   Mapped %d VS-stage pages and %d G-stage pages\n",
               PAGES_NEEDED, PAGES_NEEDED);

        // 初始清空缓存
        printf("\n[TEST] Invalidating IOMMU caches for two-stage random test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);
        iommu_ptr->next_task_id = 1;

        // ============================================================
        // 主测试: 10000 包随机读, 过程中随机穿插失效命令
        // ============================================================
        printf("\n========== %d-Request Two-Stage Random Read + Interleaved Invalidation ==========\n",
               NUM_REQUESTS);

        response_count = 0;
        iommu_ptr->steady_start_count = NUM_REQUESTS * STEADY_STATE_START_PERCENT / 100;
        iommu_ptr->steady_end_count   = NUM_REQUESTS * STEADY_STATE_END_PERCENT / 100;

        tlm_generic_payload* trans_array[NUM_REQUESTS];
        PayloadExtention* ext_array[NUM_REQUESTS];

        // [失效] 注入统计
        struct {
            int vma_lazy_g = 0, vma_lazy_gp = 0, gvma_lazy_g = 0;
            int vma_range = 0, vma_range_nl = 0;
            int iodir_ddt = 0, iodir_pdt = 0;
            int total = 0;
        } inv;
        double inval_total_ns = 0.0;     // 失效命令占用的仿真时间(阻塞注入的时长)
        double inval_max_ns = 0.0;

        InvalRng rng(0xC0FFEE);
        // 下一次失效的请求序号: [P/2, 3P/2) 抖动
        int next_inval_at = INVAL_PERIOD / 2 + (int)rng.below(INVAL_PERIOD);

        printf("[TEST] Sending %d READ requests (random 4KB pages, 8 reqs/page, shuffled)...\n",
               NUM_REQUESTS);
        printf("[TEST] Prefetch D=%d, Walker Cache ENABLED, S2 ENABLED\n",
               (int)TEST_CFG_PT_DEDUP_PREFETCH_DEPTH);
        fflush(stdout);

        int req_idx = 0;
        for (int global_req = 0; global_req < NUM_REQUESTS; global_req++) {
            // ---------- [失效] 到点则穿插一条失效命令 ----------
            if (global_req == next_inval_at) {
                const double t0 = sc_time_stamp().to_seconds() * 1e9;
                const uint32_t roll = rng.below(100);
                // 随机取一个已映射页作为 addr 型失效的目标
                const int tgt_page = (int)rng.below((uint32_t)PAGES_NEEDED);
                const uint64_t tgt_iova = IOVA_BASE + (uint64_t)page_indices[tgt_page] * 0x1000;

                // 轻量 CPU 侧维护: OS 改页表后先失效本 hart TLB
                // (规范语义: 该指令不影响 IOMMU IOATC, 故后面必须下发 IOTINVAL)
                cpu_sfence_vma(0, 0);

                if (roll < 35) {
                    // LAZY: 仅 GSCID -> PT + Walker 记录 LIB
                    iotinval(iommu_ptr, VMA, 1, 0, 0, TEST_GSCID, 0, 0);
                    inv.vma_lazy_g++;
                    printf("[INVAL_INJECT] req=%d -> IOTINVAL.VMA GV=1 AV=0 PSCV=0 (LAZY gscid)\n",
                           global_req);
                } else if (roll < 55) {
                    // LAZY: GSCID + PSCID
                    iotinval(iommu_ptr, VMA, 1, 0, 1, TEST_GSCID, TEST_PSCID, 0);
                    inv.vma_lazy_gp++;
                    printf("[INVAL_INJECT] req=%d -> IOTINVAL.VMA GV=1 AV=0 PSCV=1 (LAZY gscid+pscid)\n",
                           global_req);
                } else if (roll < 75) {
                    // LAZY: GVMA 仅 GSCID (第二阶段)
                    iotinval(iommu_ptr, GVMA, 1, 0, 0, TEST_GSCID, 0, 0);
                    inv.gvma_lazy_g++;
                    printf("[INVAL_INJECT] req=%d -> IOTINVAL.GVMA GV=1 AV=0 (LAZY gscid, stage2)\n",
                           global_req);
                } else if (roll < 85) {
                    // 小范围枚举失效(指定 addr, NL=0): 仅 PT Cache
                    iotinval(iommu_ptr, VMA, 1, 1, 1, TEST_GSCID, TEST_PSCID, tgt_iova, 0);
                    inv.vma_range++;
                    printf("[INVAL_INJECT] req=%d -> IOTINVAL.VMA AV=1 addr=0x%lx (SCAN_RANGE, PT only)\n",
                           global_req, (unsigned long)tgt_iova);
                } else if (roll < 90) {
                    // 小范围枚举 + NL=1: PT + Walker 全级别(非叶PTE)
                    iotinval(iommu_ptr, VMA, 1, 1, 1, TEST_GSCID, TEST_PSCID, tgt_iova, 1);
                    inv.vma_range_nl++;
                    printf("[INVAL_INJECT] req=%d -> IOTINVAL.VMA AV=1 NL=1 addr=0x%lx (PT+Walker all levels)\n",
                           global_req, (unsigned long)tgt_iova);
                } else if (roll < 95) {
                    // DC 精准失效 + PC 关联失效
                    iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
                    inv.iodir_ddt++;
                    printf("[INVAL_INJECT] req=%d -> IODIR.INVAL_DDT DV=1 (DC precise + PC cascade)\n",
                           global_req);
                } else {
                    // PC 精准失效
                    iodir(iommu_ptr, INVAL_PDT, 1, 0x0A, 0);
                    inv.iodir_pdt++;
                    printf("[INVAL_INJECT] req=%d -> IODIR.INVAL_PDT DV=1 (PC precise)\n",
                           global_req);
                }
                // 与 Qemu lazy 模式 trace 一致: 失效指令后紧跟 IOFENCE.C
                iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);

                inv.total++;
                const double dt = sc_time_stamp().to_seconds() * 1e9 - t0;
                inval_total_ns += dt;
                if (dt > inval_max_ns) inval_max_ns = dt;
                fflush(stdout);

                // 计算下一次失效点
                next_inval_at = global_req + INVAL_PERIOD / 2 + (int)rng.below(INVAL_PERIOD);
            }

            // ---------- 正常请求注入(与场景7 完全一致) ----------
            int page_idx = global_req / REQ_PER_PAGE;
            int offset_in_page = global_req % REQ_PER_PAGE;

            uint64_t iova = IOVA_BASE + (uint64_t)page_indices[page_idx] * 0x1000
                          + offset_in_page * 0x200;

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
                printf("[TEST] Progress: %d/%d requests injected (page[%d] IOVA=0x%lx, invals=%d)\n",
                       req_idx + 1, NUM_REQUESTS, page_idx, iova, inv.total);
                fflush(stdout);
            }
            req_idx++;
        }

        printf("[TEST] All %d requests injected (%d invalidations interleaved). Waiting for responses...\n",
               NUM_REQUESTS, inv.total);
        fflush(stdout);

        // 等待全部响应
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

        // ============================================================
        // 功能校验: 持续失效扰动下仍必须 100% 翻译正确
        // ============================================================
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
            printf("[TEST] PASS: All %d two-stage random requests translated correctly UNDER INVALIDATION!\n",
                   NUM_REQUESTS);
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

        // ============================================================
        // [失效] 注入与生效情况报告
        // ============================================================
        printf("\n========== Scenario 10: Invalidation Injection Report ==========\n");
        printf("  Injection period:            avg %d reqs (jitter [%d, %d))\n",
               INVAL_PERIOD, INVAL_PERIOD / 2, INVAL_PERIOD + INVAL_PERIOD / 2);
        printf("  Total invalidations issued:  %d  (over %d requests)\n", inv.total, NUM_REQUESTS);
        printf("  ---- by type ----\n");
        printf("    IOTINVAL.VMA  LAZY(gscid):        %d\n", inv.vma_lazy_g);
        printf("    IOTINVAL.VMA  LAZY(gscid+pscid):  %d\n", inv.vma_lazy_gp);
        printf("    IOTINVAL.GVMA LAZY(gscid):        %d\n", inv.gvma_lazy_g);
        printf("    IOTINVAL.VMA  SCAN_RANGE(addr):   %d\n", inv.vma_range);
        printf("    IOTINVAL.VMA  SCAN_RANGE + NL=1:  %d\n", inv.vma_range_nl);
        printf("    IODIR.INVAL_DDT (DC + PC):        %d\n", inv.iodir_ddt);
        printf("    IODIR.INVAL_PDT (PC):             %d\n", inv.iodir_pdt);
        printf("  ---- blocking cost on injector ----\n");
        printf("    Total time in invalidation: %.1f ns\n", inval_total_ns);
        printf("    Max single invalidation:    %.1f ns\n", inval_max_ns);
        if (inv.total > 0) {
            printf("    Avg per invalidation:       %.1f ns\n", inval_total_ns / inv.total);
        }

        // [失效] 延迟失效(LIB/VN)策略生效验证:
        // lazy_inval_drops = 查询命中但 CL.VN < LIB.VN 而被丢弃的次数,
        // >0 即证明延迟失效在真实负载下确实通过查询旁路生效。
        const uint64_t pt_drops = iommu_ptr->cache_sub.pt_cache().lazy_inval_drops();
        const uint64_t wk_drops = iommu_ptr->cache_sub.walker_cache().lazy_inval_drops();
        printf("  ---- lazy invalidation (LIB/VN) effectiveness ----\n");
        printf("    PT Cache     lazy-inval drops: %lu\n", (unsigned long)pt_drops);
        printf("    Walker Cache lazy-inval drops: %lu\n", (unsigned long)wk_drops);
        const bool lazy_ok = (inv.total == 0) || (pt_drops + wk_drops > 0);
        printf("    Lazy strategy verdict: %s\n",
               lazy_ok ? "EFFECTIVE (stale CLs dropped on lookup bypass)"
                       : "NOT OBSERVED (check LIB config / invalidation types)");
        printf("================================================================\n");

        // PTW DDR 统计
        printf("\n========== PTW DDR Access Statistics ==========\n");
        printf("  PTW total completed tasks: %lu\n", (unsigned long)iommu_ptr->ptw_total_completed);
        printf("  PTW total DDR reads:       %lu\n", (unsigned long)iommu_ptr->ptw_total_ddr_reads);
        if (iommu_ptr->ptw_total_completed > 0) {
            printf("  PTW avg DDR reads/task:    %.2f\n",
                   (double)iommu_ptr->ptw_total_ddr_reads / iommu_ptr->ptw_total_completed);
        }
        printf("================================================\n");

        // Dedup Buffer 峰值
        auto* dedup_buf = iommu_ptr->cache_sub.get_pt_dedup_buffer();
        if (dedup_buf) {
            printf("\n========== PT Dedup Buffer Statistics ==========\n");
            printf("  Buffer Size:        %u entries\n", PT_DEDUP_BUFFER_SIZE);
            printf("  Peak Valid Count:   %u entries\n", dedup_buf->get_peak_valid_count());
            printf("  Current Valid:      %u entries\n", dedup_buf->get_valid_count());
            printf("  Peak Usage:         %.1f%%\n",
                   100.0 * dedup_buf->get_peak_valid_count() / PT_DEDUP_BUFFER_SIZE);
            printf("==================================================\n");
        }

        print_cpu_instr_stats();
        iommu_ptr->print_cache_statistics();

        sc_core::sc_stop();
        return;
    }
}
