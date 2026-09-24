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
// 场景13-v4: 4KB随机读写 + SQ/CQ/MSI 混合负载 (两阶段翻译 + S2开启)
//
// 基础负载沿用场景7地址布局与页表构造:
//   IOVA范围 16MB (0x100000~0x10FFFFF), 全部4096个4KB页建立
//   IOVA->GPA(20x2MB大页)->SPA(=GPA+固定偏移) 映射;
//   随机挑选唯一页(shuffled), 每页8个连续512B读写请求。
//
// MSI负载叠加:
//   - 设备0x0A使能 MSIPTP_Flat: mask=0xFF, pattern=0x3000
//     -> MSI窗口 GPA 0x3000000~0x30FFFFF (48MB),
//        与普通GPA 0~0x27FFFFF (20x2MB大页, 40MB) 完全不重合;
//   - MSI IOVA区 0x2000000~0x23FFFFF (4MB, 1024个候选页),
//     与普通16MB IOVA范围完全不重合; 随机挑选256个不连续页,
//     每页经VS-stage映射到MSI窗口内一个vector页(无S2映射,
//     两级模式下PTW在S1完成后识别MSI并跳过S2);
//   - MSI PTE: 256个vector全部 Flat(M=3), 输出物理页基址16MB。
//
// [v4] 负载模式 (系统级任务奇读偶写交替):
//   系统任务1: 8x512B Data读 + 1x32B SQ读 + 1x16B CQ写 + 1x4B MSI写 = 11包
//   系统任务2: 8x512B Data写 + 1x32B SQ读 + 1x16B CQ写 + 1x4B MSI写 = 11包
//   系统任务3: 8x512B Data读 + ... (交替)
//   909组 x 11包 + 末尾补1个MSI = 10000包 (Data 7272 + SQ 909 + CQ 909 + MSI 910)
//   SQ/CQ/MSI固定IOVA地址, 经预热后稳态100%命中PT Cache/MSIPT Cache,
//   不进入去重cache及后续PTW模块。
// ============================================================

// ---- 场景13 MSI/SQ/CQ 地址常量 ----
static const uint64_t S13_MSI_MASK      = 0xFF;
#ifdef TEST_CFG_S13_IOVA_512MB
// [512MB新测试项] 数据IOVA扩至512MB(1~513MB), MSI/SQ/CQ的IOVA与GPA均移到数据区之上避免重叠
static const uint64_t S13_MSI_PATTERN   = 0x7000;      // MSI窗口GPA=0x7000000(112MB, 避开数据GPA 0~100MB)
static const uint64_t S13_MSI_IOVA_BASE = 0x21000000;  // MSI IOVA区基址(528MB, 避开数据IOVA 1~513MB)
static const uint64_t S13_SQ_IOVA       = 0x22000000;  // SQ固定IOVA(544MB)
static const uint64_t S13_CQ_IOVA       = 0x22001000;  // CQ固定IOVA
static const uint64_t S13_SQ_CQ_GPA     = 0x8000000;   // SQ/CQ共用GPA区(128MB, 避开数据100MB/MSI112MB)
#else
// [16MB原测试项] 原地址布局(保持不变)
static const uint64_t S13_MSI_PATTERN   = 0x3000;      // MSI窗口GPA=0x3000000(48MB)
static const uint64_t S13_MSI_IOVA_BASE = 0x2000000;   // MSI IOVA区基址(32MB)
static const uint64_t S13_SQ_IOVA       = 0x5000000;   // SQ固定IOVA(80MB)
static const uint64_t S13_CQ_IOVA       = 0x5001000;   // CQ固定IOVA
static const uint64_t S13_SQ_CQ_GPA     = 0x4000000;   // SQ/CQ共用GPA区(64MB, 避开Data/MSI)
#endif
static const uint64_t S13_MSI_GPA_BASE  = S13_MSI_PATTERN << 12;  // MSI窗口GPA基址
static const int      S13_MSI_VECTORS   = 256;                    // mask=0xFF -> 256 vectors
static const uint64_t S13_FLAT_PPN_BASE = 0x1000;                 // Flat输出物理页基址(16MB)
static const int      S13_MSI_IOVA_CAND = 1024;                   // MSI IOVA候选页数(4MB区域)
static const uint64_t S13_MSI_OFFSET    = 0x40;                   // MSI写页内偏移
static const int      S13_MSI_FIXED_VEC = 0;                      // MSI固定vector=0(稳态命中)

// 写一笔16字节MSI PTE到设备MSI页表
static void s13_write_msi_pte(RP_Module* rp, uint64_t msipt_base, uint32_t vec, msipte_t pte) {
    rp->write_memory_test_rp((char*)&pte.raw[0], msipt_base + vec * 16, 16);
}

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

        printf("\n========== IOMMU Scene 13: 4KB Random R/W + MSI Mixed - Two-Stage (device 0x0A) ==========\n");

        // Enable IOMMU
        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));

        extern uint64_t next_free_page;
        next_free_page = 240;

        // ============================================================
        // 地址布局 (与场景7一致, 叠加MSI区):
        //   普通IOVA: 0x100000 ~ 0x10FFFFF (16MB, 4096页, 全部映射)
        //   MSI IOVA: 0x2000000 ~ 0x23FFFFF (4MB内随机挑256个不连续页)
        //   普通GPA:  20 x 2MB大页, 0 ~ 0x27FFFFF (40MB)
        //   MSI窗口:  0x3000000 ~ 0x30FFFFF (48MB, pattern=0x3000/mask=0xFF)
        //   G-stage页表页: GPPN 0x10000+ (256MB GPA), 避开MSI窗口
        // ============================================================
        const uint64_t IOVA_BASE   = 0x100000;         // 1MB aligned base
#ifdef TEST_CFG_S13_IOVA_512MB
        const uint64_t IOVA_RANGE  = 0x20000000;       // [512MB新测试项] 512MB IOVA数据随机范围
        const int NUM_GPA_HUGEPAGES = 50;              // [512MB] 50个2MB大页GPA(100MB)
#else
        const uint64_t IOVA_RANGE  = 0x1000000;        // [16MB原测试项] 16MB
        const int NUM_GPA_HUGEPAGES = 20;              // [16MB] 20个2MB大页GPA(40MB)
#endif
        const uint64_t HUGE_PAGE_SZ = 0x200000;        // 2MB (GPA大页粒度)
        const uint64_t SPA_OFFSET  = 0x80000000;       // SPA = GPA + 固定偏移(2MB对齐)
        const int SLOTS_PER_HUGEPAGE = (int)(HUGE_PAGE_SZ / 0x1000);  // 512个4KB slot
        // [场景化] 默认909组; Makefile传入TEST_CFG_NUM_PAGES=909
        // 每组 = 8 Data(512B) + 1 SQ(32B) + 1 CQ(16B) + 1 MSI(4B) = 11包
        // 909 × 11 = 9999, 末尾补1个MSI = 10000包
#ifndef TEST_CFG_NUM_PAGES
        const int PAGES_NEEDED     = 909;              // 909组
#else
        const int PAGES_NEEDED     = TEST_CFG_NUM_PAGES;
#endif
#ifndef TEST_CFG_TAIL_MSI
#define TEST_CFG_TAIL_MSI 1                            // 默认末尾补1个MSI(保持旧负载形状)
#endif
        const int REQ_PER_PAGE     = 8;                // 8 × 512B = 4KB Data
        const int NUM_GROUPS       = PAGES_NEEDED;     // 每组 = 8 Data + 1 SQ + 1 CQ + 1 MSI
        const int NUM_IO_REQS      = NUM_GROUPS * REQ_PER_PAGE;   // Data请求数(计入IOPS)
        const int NUM_SQ_REQS      = NUM_GROUPS;                  // SQ请求数(不计入IOPS)
        const int NUM_CQ_REQS      = NUM_GROUPS;                  // CQ请求数(不计入IOPS)
        const int NUM_MSI_REQS     = NUM_GROUPS + TEST_CFG_TAIL_MSI;  // MSI请求数(末尾补齐, 不计入IOPS)
        const int NUM_REQUESTS     = NUM_IO_REQS + NUM_SQ_REQS + NUM_CQ_REQS + NUM_MSI_REQS;  // 总包数
        const int TOTAL_PAGES_IN_RANGE = (int)(IOVA_RANGE / 0x1000);  // 4096(16MB) / 131072(512MB)

        printf("\n[TEST] Scene 13-v2 Mixed Load Construction (Data+SQ+CQ+MSI):\n");
        printf("[TEST]   Normal IOVA range: 0x%lx ~ 0x%lx (%dMB, %d pages)\n",
               IOVA_BASE, IOVA_BASE + IOVA_RANGE, (int)(IOVA_RANGE >> 20), TOTAL_PAGES_IN_RANGE);
        printf("[TEST]   SQ fixed IOVA:     0x%lx (32B write, is_ctrl=1)\n", S13_SQ_IOVA);
        printf("[TEST]   CQ fixed IOVA:     0x%lx (16B write, is_ctrl=1)\n", S13_CQ_IOVA);
        printf("[TEST]   MSI fixed IOVA:    0x%lx (vector=%d, 4B write, is_ctrl=1)\n",
               S13_MSI_IOVA_BASE + S13_MSI_OFFSET, S13_MSI_FIXED_VEC);
        printf("[TEST]   Pattern: %d groups x (8x512B Data + 1 SQ + 1 CQ + 1 MSI) + 1 MSI = %d packets\n",
               NUM_GROUPS, NUM_REQUESTS);
        printf("[TEST]   IOPS counts only Data: %d packets (SQ/CQ/MSI excluded)\n", NUM_IO_REQS);
        printf("[TEST]   GPA = %d x 2MB hugepages, SPA = GPA + 0x%lx\n", NUM_GPA_HUGEPAGES, SPA_OFFSET);

        // GPPN offset: 避开测试GPA(40MB)与MSI窗口(48MB) -> 256MB
        extern uint64_t next_free_gpage[65536];
        uint64_t test_max_gppn = (uint64_t)NUM_GPA_HUGEPAGES * (HUGE_PAGE_SZ / PAGESIZE);
        uint64_t gppn_offset = ((test_max_gppn + 0xFFFF) / 0x10000) * 0x10000;
        next_free_gpage[1] = gppn_offset;
        printf("[TEST]   GPPN offset: 0x%lx (test GPPN max=0x%lx, MSI window=0x%lx)\n",
               gppn_offset, test_max_gppn, S13_MSI_GPA_BASE);

        // ============================================================
        // 配置设备0x0A: iohgatp=Sv48x4, iosatp=Sv48, MSIPTP=Flat
        // ============================================================
        printf("\n========== Configuring Device 0x0A: iosatp=Sv48, iohgatp=Sv48x4, MSIPTP=Flat ==========\n");

        uint64_t dc_addr = add_device(iommu_ptr, 0x0A, 1, 0, 0, 0, 0, 0,
                                      1, 1, 0, 0, 0,
                                      IOHGATP_Sv48x4, IOSATP_Sv48, PDTP_Bare,
                                      MSIPTP_Flat, 1, S13_MSI_MASK, S13_MSI_PATTERN);
        device_context_t DC6;
        read_memory_test_rp(dc_addr, sizeof(device_context_t), (char*)&DC6);
        printf("[DEV] DC addr: 0x%lx, iohgatp.MODE=%d (Sv48x4), iosatp.MODE=%d (Sv48)\n",
               dc_addr, DC6.iohgatp.MODE, DC6.fsc.iosatp.MODE);
        printf("[DEV] msiptp.MODE=%d (Flat), msiptp.PPN=0x%lx, mask=0x%lx, pattern=0x%lx\n",
               DC6.msiptp.MODE, (unsigned long)DC6.msiptp.PPN,
               (unsigned long)DC6.msi_addr_mask.mask, (unsigned long)DC6.msi_addr_pattern.pattern);

        // ============================================================
        // MSI页表: 256个vector全部Flat(M=3), 输出PPN=S13_FLAT_PPN_BASE+v
        // ============================================================
        {
            uint64_t msipt_base = DC6.msiptp.PPN * PAGESIZE;
            msipte_t mpte;
            for (int v = 0; v < S13_MSI_VECTORS; v++) {
                mpte.raw[0] = 0; mpte.raw[1] = 0;
                mpte.V = 1; mpte.M = 3; mpte.translate_rw.PPN = S13_FLAT_PPN_BASE + v;
                s13_write_msi_pte(this, msipt_base, v, mpte);
            }
            printf("[TEST] MSI page table ready: base=0x%lx, %d vectors Flat -> PPN 0x%lx~0x%lx\n",
                   (unsigned long)msipt_base, S13_MSI_VECTORS,
                   (unsigned long)S13_FLAT_PPN_BASE, (unsigned long)(S13_FLAT_PPN_BASE + S13_MSI_VECTORS - 1));
        }

        // ============================================================
        // Create G-stage mapping for VS root page (与场景7一致)
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
        // 生成 PAGES_NEEDED 个唯一随机普通页索引 (deterministic seed) + shuffle
        // ============================================================
        srand(2024);
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

        printf("[TEST]   Generated %d unique random normal page indices (SHUFFLED)\n", (int)page_indices.size());

        // ============================================================
        // MSI IOVA页: 从4MB候选区随机挑256个不连续页
        // ============================================================
        srand(777);
        set<int> msi_page_set;
        while ((int)msi_page_set.size() < S13_MSI_VECTORS) {
            msi_page_set.insert(rand() % S13_MSI_IOVA_CAND);
        }
        vector<int> msi_pages(msi_page_set.begin(), msi_page_set.end());  // msi_pages[v] = IOVA候选页号

        // 每笔MSI请求随机选vector (0~255)
        srand(555);
        vector<int> msi_vectors(NUM_MSI_REQS);
        for (int i = 0; i < NUM_MSI_REQS; i++) {
            msi_vectors[i] = rand() % S13_MSI_VECTORS;
        }
        printf("[TEST]   MSI: %d unique IOVA pages (random non-contiguous), %d MSI reqs over %d vectors\n",
               (int)msi_pages.size(), NUM_MSI_REQS, S13_MSI_VECTORS);

        // ============================================================
        // Page table templates
        // ============================================================
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
        // G-stage: 20 x 2MB hugepage mappings (GPA -> SPA)
        // ============================================================
        for (int h = 0; h < NUM_GPA_HUGEPAGES; h++) {
            uint64_t gpa_2mb = (uint64_t)h * HUGE_PAGE_SZ;
            uint64_t spa_2mb = gpa_2mb + SPA_OFFSET;
            gpte.PPN = spa_2mb / PAGESIZE;
            fail_if((add_g_stage_pte(iommu_ptr, DC6.iohgatp, gpa_2mb, gpte, 1) == (uint64_t)-1));
        }
        printf("[TEST]   Created %d G-stage 2MB hugepage mappings (GPA 0~0x%lx, 不含MSI窗口0x%lx)\n",
               NUM_GPA_HUGEPAGES, (uint64_t)(NUM_GPA_HUGEPAGES - 1) * HUGE_PAGE_SZ, S13_MSI_GPA_BASE);

        // ============================================================
        // VS-stage: 普通页全量映射 (IOVA -> GPA, 与场景7一致)
        // ============================================================
        srand(42);
        vector<int> iova_huge(TOTAL_PAGES_IN_RANGE, 0);
        vector<int> iova_slot(TOTAL_PAGES_IN_RANGE, 0);
#ifdef TEST_CFG_S13_IOVA_512MB
        // [512MB] 部分映射: 仅映射被访问页(909数据页 + 各自D个预取相邻页), 多对一随机GPA。
        //   原因: IOVA页(131072) >> GPA slot(50×512=25600)无法一对一; 且全量映射会产生海量
        //   add_vs_stage_pte调试printf。仅映射数据页+预取页即可保证主/预取任务iova→gpa→spa全有效。
        {
            set<uint64_t> accessed_pages;
            for (int k = 0; k < PAGES_NEEDED; k++) {
                uint64_t base = IOVA_BASE + (uint64_t)page_indices[k] * 0x1000;
                for (int d = 0; d <= (int)TEST_CFG_PT_DEDUP_PREFETCH_DEPTH; d++) {
                    uint64_t pg = base + (uint64_t)d * 0x1000;
                    if (pg < IOVA_BASE + IOVA_RANGE) accessed_pages.insert(pg);
                }
            }
            for (uint64_t pg : accessed_pages) {
                int idx = (int)((pg - IOVA_BASE) / 0x1000);
                int h = rand() % NUM_GPA_HUGEPAGES;          // 随机GPA大页(多对一)
                int slot = rand() % SLOTS_PER_HUGEPAGE;      // 随机slot
                iova_huge[idx] = h;
                iova_slot[idx] = slot;
                uint64_t gpa_page = (uint64_t)h * HUGE_PAGE_SZ + (uint64_t)slot * 0x1000;
                pte6.PPN = gpa_page / PAGESIZE;
                fail_if((add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, pg, pte6, 0, DC6.iohgatp, 0) == (uint64_t)-1));
            }
            printf("[TEST]   512MB partial-mapped %zu accessed pages (data+prefetch) onto %d GPA hugepages (many-to-one)\n",
                   accessed_pages.size(), NUM_GPA_HUGEPAGES);
        }
#else
        // [16MB] 全量一对一映射(原逻辑不变)
        int slot_counter[20] = {0};
        for (int i = 0; i < TOTAL_PAGES_IN_RANGE; i++) {
            int h = rand() % NUM_GPA_HUGEPAGES;
            int probe = 0;
            while (slot_counter[h] >= SLOTS_PER_HUGEPAGE && probe < NUM_GPA_HUGEPAGES) {
                h = (h + 1) % NUM_GPA_HUGEPAGES;
                probe++;
            }
            iova_huge[i] = h;
            iova_slot[i] = slot_counter[h]++;

            uint64_t iova_page = IOVA_BASE + (uint64_t)i * 0x1000;
            uint64_t gpa_page  = (uint64_t)h * HUGE_PAGE_SZ + (uint64_t)iova_slot[i] * 0x1000;

            pte6.PPN = gpa_page / PAGESIZE;
            fail_if((add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, iova_page, pte6, 0, DC6.iohgatp, 0) == (uint64_t)-1));
        }
        printf("[TEST]   Mapped all %d normal VS-stage pages onto %d G-stage 2MB hugepages\n",
               TOTAL_PAGES_IN_RANGE, NUM_GPA_HUGEPAGES);
#endif

        // ============================================================
        // VS-stage: MSI页映射 (MSI IOVA -> MSI窗口GPA, 无S2映射)
        //   两级模式下PTW在S1完成得到GPA后识别MSI窗口, 跳过S2直达MSIPT
        // ============================================================
        for (int v = 0; v < S13_MSI_VECTORS; v++) {
            uint64_t iova_page = S13_MSI_IOVA_BASE + (uint64_t)msi_pages[v] * 0x1000;
            uint64_t gpa_page  = S13_MSI_GPA_BASE + (uint64_t)v * 0x1000;
            pte6.PPN = gpa_page / PAGESIZE;
            fail_if((add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, iova_page, pte6, 0, DC6.iohgatp, 0) == (uint64_t)-1));
        }
        printf("[TEST]   Mapped %d MSI VS-stage pages: IOVA 0x%lx+rand -> GPA window 0x%lx (no S2 mapping)\n",
               S13_MSI_VECTORS, S13_MSI_IOVA_BASE, S13_MSI_GPA_BASE);

        // ============================================================
        // [场景13-v2] SQ/CQ 固定地址映射 (稳态100%命中PT Cache)
        //   SQ: IOVA 0x5000000 -> GPA 0x4000000 -> SPA = GPA + SPA_OFFSET
        //   CQ: IOVA 0x5001000 -> GPA 0x4001000 -> SPA = GPA + SPA_OFFSET
        // ============================================================
        {
            // SQ 映射
            uint64_t sq_gpa = S13_SQ_CQ_GPA;  // 0x4000000
            pte6.PPN = sq_gpa / PAGESIZE;
            fail_if((add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, S13_SQ_IOVA, pte6, 0, DC6.iohgatp, 0) == (uint64_t)-1));
            // SQ G-stage 映射 (4KB页)
            gpte.PPN = (sq_gpa + SPA_OFFSET) / PAGESIZE;
            fail_if((add_g_stage_pte(iommu_ptr, DC6.iohgatp, sq_gpa, gpte, 0) == (uint64_t)-1));

            // CQ 映射
            uint64_t cq_gpa = S13_SQ_CQ_GPA + 0x1000;  // 0x4001000
            pte6.PPN = cq_gpa / PAGESIZE;
            fail_if((add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, S13_CQ_IOVA, pte6, 0, DC6.iohgatp, 0) == (uint64_t)-1));
            // CQ G-stage 映射 (4KB页)
            gpte.PPN = (cq_gpa + SPA_OFFSET) / PAGESIZE;
            fail_if((add_g_stage_pte(iommu_ptr, DC6.iohgatp, cq_gpa, gpte, 0) == (uint64_t)-1));

            printf("[TEST]   SQ/CQ fixed mapping: SQ IOVA=0x%lx->GPA=0x%lx, CQ IOVA=0x%lx->GPA=0x%lx\n",
                   S13_SQ_IOVA, sq_gpa, S13_CQ_IOVA, cq_gpa);

            // [FIX] SQ/CQ预取相邻页映射: 控制包偶发PT Cache miss(冷启动)后进PTW会late-spawn预取,
            //   预取页(SQ/CQ_IOVA + 4KB×d)若未映射会导致VS PTE invalid skip。
            //   此处将这些相邻页映射到数据GPA区(G-stage大页已全覆盖), 保证预取PTE有效、无skip。
            {
                set<uint64_t> ctrl_pf_pages;
                for (int d = 1; d <= (int)TEST_CFG_PT_DEDUP_PREFETCH_DEPTH; d++) {
                    ctrl_pf_pages.insert(S13_SQ_IOVA + (uint64_t)d * 0x1000);
                    ctrl_pf_pages.insert(S13_CQ_IOVA + (uint64_t)d * 0x1000);
                }
                for (uint64_t pg : ctrl_pf_pages) {
                    if (pg == S13_SQ_IOVA || pg == S13_CQ_IOVA) continue;  // 跳过SQ/CQ本身(已映射)
                    int h = rand() % NUM_GPA_HUGEPAGES;
                    int slot = rand() % SLOTS_PER_HUGEPAGE;
                    uint64_t gpa_page = (uint64_t)h * HUGE_PAGE_SZ + (uint64_t)slot * 0x1000;
                    pte6.PPN = gpa_page / PAGESIZE;
                    fail_if((add_vs_stage_pte(iommu_ptr, DC6.fsc.iosatp, pg, pte6, 0, DC6.iohgatp, 0) == (uint64_t)-1));
                }
                printf("[TEST]   Mapped %zu SQ/CQ prefetch-adjacent pages (avoid VS PTE invalid on ctrl miss)\n",
                       ctrl_pf_pages.size());
            }
        }

        // Invalidate caches
        printf("\n[TEST] Invalidating IOMMU caches for scene 13 mixed test...\n");
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0A, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        // Reset counters
        iommu_ptr->next_task_id = 1;
        response_count = 0;
        extern uint64_t g_msipt_cache_hit_count;
        extern uint64_t g_msipt_cache_miss_count;
        g_msipt_cache_hit_count = 0;
        g_msipt_cache_miss_count = 0;

        // ============================================================
        // [场景13-v3] MSIPT Cache 预热: 发送几个MSI请求填充Cache
        //   固定vector=0, 预热后稳态MSIPT命中率=100%
        // ============================================================
        {
            const int WARMUP_MSI = 8;
            printf("[WARMUP] Sending %d MSI requests to warm up MSIPT Cache (vector=%d)...\n",
                   WARMUP_MSI, S13_MSI_FIXED_VEC);
            int warmup_resp = 0;
            for (int w = 0; w < WARMUP_MSI; w++) {
                int v = S13_MSI_FIXED_VEC;
                uint64_t msi_iova = S13_MSI_IOVA_BASE + (uint64_t)msi_pages[v] * 0x1000 + S13_MSI_OFFSET;
                tlm_generic_payload* warmup_tr = new tlm_generic_payload();
                unsigned char* warmup_data = new unsigned char[4]();
                memcpy(warmup_data, &v, 4);
                warmup_tr->set_address(msi_iova);
                warmup_tr->set_data_ptr(warmup_data);
                warmup_tr->set_data_length(4);
                warmup_tr->set_command(TLM_WRITE_COMMAND);
                PayloadExtention* warmup_ext = new PayloadExtention();
                warmup_ext->requester_id = 0x0A;
                warmup_ext->pid_valid = 0;   // [FIX] 显式初始化(构造函数未初始化这些字段, 避免垃圾值导致cause=260预热fault)
                warmup_ext->process_id = 0;
                warmup_ext->exec_req = 0;
                warmup_ext->priv_req = 0;
                warmup_ext->no_write = 0;
                warmup_ext->at = 0;
                warmup_ext->is_ctrl = 1;
                warmup_tr->set_extension(warmup_ext);
                sc_time delay = SC_ZERO_TIME;
                tlm::tlm_phase phase = tlm::BEGIN_REQ;
                axi_master_to_pcie_noc_0_socket->nb_transport_fw(*warmup_tr, phase, delay);
            }
            // 等待预热请求全部完成
            int warmup_stall = 0;
            while (response_count < WARMUP_MSI) {
                wait(100, SC_NS);
                if (++warmup_stall > 100) break;
            }
            printf("[WARMUP] %d MSI warmup requests completed (response_count=%d)\n",
                   WARMUP_MSI, response_count);
            // 重置计数器, 预热不计入统计
            response_count = 0;
            g_msipt_cache_hit_count = 0;
            g_msipt_cache_miss_count = 0;
        }

        // ============================================================
        // [场景13-v4] SQ/CQ PT Cache 预热: 发送几笔SQ(读)+CQ(写)请求
        //   固定IOVA地址, 预热后稳态PT Cache命中率=100%, 不进dedup/PTW
        // ============================================================
        {
            const int WARMUP_SQ_CQ = 4;
            printf("[WARMUP] Sending %d SQ+CQ requests to warm up PT Cache...\n", WARMUP_SQ_CQ * 2);
            for (int w = 0; w < WARMUP_SQ_CQ; w++) {
                // SQ: 32B读
                tlm_generic_payload* sq_tr = new tlm_generic_payload();
                unsigned char* sq_data = new unsigned char[32]();
                sq_tr->set_address(S13_SQ_IOVA);
                sq_tr->set_data_ptr(sq_data);
                sq_tr->set_data_length(32);
                sq_tr->set_command(TLM_READ_COMMAND);
                PayloadExtention* sq_ext = new PayloadExtention();
                sq_ext->requester_id = 0x0A;
                sq_ext->pid_valid = 0;   // [FIX] 显式初始化(避免预热fault)
                sq_ext->process_id = 0;
                sq_ext->exec_req = 0;
                sq_ext->priv_req = 0;
                sq_ext->no_write = 1;    // SQ: 32B读
                sq_ext->at = 0;
                sq_ext->is_ctrl = 1;
                sq_tr->set_extension(sq_ext);
                sc_time sq_delay = SC_ZERO_TIME;
                tlm::tlm_phase sq_phase = tlm::BEGIN_REQ;
                axi_master_to_pcie_noc_0_socket->nb_transport_fw(*sq_tr, sq_phase, sq_delay);

                // CQ: 16B写
                tlm_generic_payload* cq_tr = new tlm_generic_payload();
                unsigned char* cq_data = new unsigned char[16]();
                cq_tr->set_address(S13_CQ_IOVA);
                cq_tr->set_data_ptr(cq_data);
                cq_tr->set_data_length(16);
                cq_tr->set_command(TLM_WRITE_COMMAND);
                PayloadExtention* cq_ext = new PayloadExtention();
                cq_ext->requester_id = 0x0A;
                cq_ext->pid_valid = 0;   // [FIX] 显式初始化(避免预热fault)
                cq_ext->process_id = 0;
                cq_ext->exec_req = 0;
                cq_ext->priv_req = 0;
                cq_ext->no_write = 0;    // CQ: 16B写
                cq_ext->at = 0;
                cq_ext->is_ctrl = 1;
                cq_tr->set_extension(cq_ext);
                sc_time cq_delay = SC_ZERO_TIME;
                tlm::tlm_phase cq_phase = tlm::BEGIN_REQ;
                axi_master_to_pcie_noc_0_socket->nb_transport_fw(*cq_tr, cq_phase, cq_delay);
            }
            // 等待预热请求全部完成 (WARMUP_SQ_CQ * 2 笔)
            int warmup_stall2 = 0;
            int expected_warmup = WARMUP_SQ_CQ * 2;
            while (response_count < expected_warmup) {
                wait(100, SC_NS);
                if (++warmup_stall2 > 100) break;
            }
            printf("[WARMUP] %d SQ+CQ warmup requests completed (response_count=%d)\n",
                   expected_warmup, response_count);
            // 重置计数器, 预热不计入统计
            response_count = 0;
        }

        // ============================================================
        // [场景13-v4] 混合负载注入: 每组 = 8x512B Data + 1 SQ(32B) + 1 CQ(16B) + 1 MSI(4B)
        //   所有任务(数据+控制)均计入IOPS
        // ============================================================
        printf("\n========== %d-Packet Mixed Test (%d Data + %d SQ + %d CQ + %d MSI) ==========\n",
               NUM_REQUESTS, NUM_IO_REQS, NUM_SQ_REQS, NUM_CQ_REQS, NUM_MSI_REQS);
        printf("[TEST] Task distribution:\n");
        printf("  - 512B Data tasks:  %d (%.2f%%)\n", NUM_IO_REQS, 100.0 * NUM_IO_REQS / NUM_REQUESTS);
        printf("  - 32B SQ tasks:     %d (%.2f%%)\n", NUM_SQ_REQS, 100.0 * NUM_SQ_REQS / NUM_REQUESTS);
        printf("  - 16B CQ tasks:     %d (%.2f%%)\n", NUM_CQ_REQS, 100.0 * NUM_CQ_REQS / NUM_REQUESTS);
        printf("  - 4B MSI tasks:     %d (%.2f%%)\n", NUM_MSI_REQS, 100.0 * NUM_MSI_REQS / NUM_REQUESTS);
        printf("  - Total:            %d (100%%)\n", NUM_REQUESTS);

        // [场景13-v4] IOPS统计所有任务(数据+控制信息)
        iommu_ptr->steady_start_count = NUM_REQUESTS * STEADY_STATE_START_PERCENT / 100;
        iommu_ptr->steady_end_count   = NUM_REQUESTS * STEADY_STATE_END_PERCENT / 100;

        tlm_generic_payload** trans_array = new tlm_generic_payload*[NUM_REQUESTS];
        PayloadExtention** ext_array = new PayloadExtention*[NUM_REQUESTS];
        uint64_t* expected_pa = new uint64_t[NUM_REQUESTS];
        int* req_type = new int[NUM_REQUESTS];  // 0=Data, 1=SQ, 2=CQ, 3=MSI

        printf("[TEST] Injecting %d packets (group: 8x512B Data + 1 SQ + 1 CQ + 1 MSI)...\n", NUM_REQUESTS);
        printf("[TEST] Prefetch D=%d, Walker Cache ENABLED, MSI vector FIXED=%d\n",
               (int)TEST_CFG_PT_DEDUP_PREFETCH_DEPTH, S13_MSI_FIXED_VEC);
        fflush(stdout);

        int req_idx = 0;
        for (int g = 0; g < NUM_GROUPS; g++) {
            // ---- 8个512B Data任务: 奇读偶写交替 (系统任务1=全读, 系统任务2=全写, ...) ----
            int iova_pg = page_indices[g];
            bool group_is_read = (g % 2 == 0);  // 偶数组=读, 奇数组=写
            for (int off = 0; off < REQ_PER_PAGE; off++) {
                uint64_t iova = IOVA_BASE + (uint64_t)iova_pg * 0x1000 + off * 0x200;
                uint64_t exp_pa = (uint64_t)iova_huge[iova_pg] * HUGE_PAGE_SZ
                                + (uint64_t)iova_slot[iova_pg] * 0x1000
                                + SPA_OFFSET + (iova & 0xFFF);
                bool is_read = group_is_read;

                trans_array[req_idx] = new tlm_generic_payload();
                unsigned char* data = new unsigned char[1024]();
                trans_array[req_idx]->set_address(iova);
                trans_array[req_idx]->set_data_ptr(data);
                trans_array[req_idx]->set_data_length(512);
                trans_array[req_idx]->set_command(is_read ? TLM_READ_COMMAND : TLM_WRITE_COMMAND);

                ext_array[req_idx] = new PayloadExtention();
                ext_array[req_idx]->requester_id = 0x0A;
                ext_array[req_idx]->pid_valid = 0;
                ext_array[req_idx]->process_id = 0;
                ext_array[req_idx]->exec_req = 0;
                ext_array[req_idx]->priv_req = 0;
                ext_array[req_idx]->no_write = is_read ? 1 : 0;
                ext_array[req_idx]->at = 0;
                ext_array[req_idx]->is_ctrl = 0;  // Data: 计入IOPS
                trans_array[req_idx]->set_extension(ext_array[req_idx]);

                expected_pa[req_idx] = exp_pa;
                req_type[req_idx] = 0;  // Data
                req_idx++;
            }

            // ---- 1个SQ任务: 32B读, 固定IOVA, is_ctrl=1 ----
            {
                trans_array[req_idx] = new tlm_generic_payload();
                unsigned char* sq_data = new unsigned char[32]();
                trans_array[req_idx]->set_address(S13_SQ_IOVA);
                trans_array[req_idx]->set_data_ptr(sq_data);
                trans_array[req_idx]->set_data_length(32);
                trans_array[req_idx]->set_command(TLM_READ_COMMAND);

                ext_array[req_idx] = new PayloadExtention();
                ext_array[req_idx]->requester_id = 0x0A;
                ext_array[req_idx]->pid_valid = 0;
                ext_array[req_idx]->process_id = 0;
                ext_array[req_idx]->exec_req = 0;
                ext_array[req_idx]->priv_req = 0;
                ext_array[req_idx]->no_write = 1;
                ext_array[req_idx]->at = 0;
                ext_array[req_idx]->is_ctrl = 1;  // SQ: 不计入IOPS
                trans_array[req_idx]->set_extension(ext_array[req_idx]);

                expected_pa[req_idx] = S13_SQ_CQ_GPA + SPA_OFFSET;
                req_type[req_idx] = 1;  // SQ
                req_idx++;
            }


            // ---- 1个CQ任务: 16B写, 固定IOVA, is_ctrl=1 ----
            {
                trans_array[req_idx] = new tlm_generic_payload();
                unsigned char* cq_data = new unsigned char[16]();
                trans_array[req_idx]->set_address(S13_CQ_IOVA);
                trans_array[req_idx]->set_data_ptr(cq_data);
                trans_array[req_idx]->set_data_length(16);
                trans_array[req_idx]->set_command(TLM_WRITE_COMMAND);

                ext_array[req_idx] = new PayloadExtention();
                ext_array[req_idx]->requester_id = 0x0A;
                ext_array[req_idx]->pid_valid = 0;
                ext_array[req_idx]->process_id = 0;
                ext_array[req_idx]->exec_req = 0;
                ext_array[req_idx]->priv_req = 0;
                ext_array[req_idx]->no_write = 0;
                ext_array[req_idx]->at = 0;
                ext_array[req_idx]->is_ctrl = 1;  // CQ: 不计入IOPS
                trans_array[req_idx]->set_extension(ext_array[req_idx]);

                expected_pa[req_idx] = S13_SQ_CQ_GPA + 0x1000 + SPA_OFFSET;
                req_type[req_idx] = 2;  // CQ
                req_idx++;
            }

            // ---- 1个MSI任务: 4B写, 固定vector=0, is_ctrl=1 ----
            {
                int v = S13_MSI_FIXED_VEC;
                uint64_t msi_iova = S13_MSI_IOVA_BASE + (uint64_t)msi_pages[v] * 0x1000 + S13_MSI_OFFSET;

                trans_array[req_idx] = new tlm_generic_payload();
                unsigned char* msi_data = new unsigned char[4]();
                memcpy(msi_data, &v, 4);
                trans_array[req_idx]->set_address(msi_iova);
                trans_array[req_idx]->set_data_ptr(msi_data);
                trans_array[req_idx]->set_data_length(4);
                trans_array[req_idx]->set_command(TLM_WRITE_COMMAND);

                ext_array[req_idx] = new PayloadExtention();
                ext_array[req_idx]->requester_id = 0x0A;
                ext_array[req_idx]->pid_valid = 0;
                ext_array[req_idx]->process_id = 0;
                ext_array[req_idx]->exec_req = 0;
                ext_array[req_idx]->priv_req = 0;
                ext_array[req_idx]->no_write = 0;
                ext_array[req_idx]->at = 0;
                ext_array[req_idx]->is_ctrl = 1;  // MSI: 不计入IOPS
                trans_array[req_idx]->set_extension(ext_array[req_idx]);

                expected_pa[req_idx] = ((S13_FLAT_PPN_BASE + v) << 12) | S13_MSI_OFFSET;
                req_type[req_idx] = 3;  // MSI
                req_idx++;
            }

            if ((g + 1) % 200 == 0) {
                printf("[TEST] Progress: group %d/%d built (%d packets)\n", g + 1, NUM_GROUPS, req_idx);
                fflush(stdout);
            }
        }

        // ---- 末尾补TEST_CFG_TAIL_MSI个MSI, 凑满总包数(默认1个=10000包) ----
        for (int t = 0; t < TEST_CFG_TAIL_MSI; t++) {
            int v = S13_MSI_FIXED_VEC;
            uint64_t msi_iova = S13_MSI_IOVA_BASE + (uint64_t)msi_pages[v] * 0x1000 + S13_MSI_OFFSET;
            trans_array[req_idx] = new tlm_generic_payload();
            unsigned char* msi_data = new unsigned char[4]();
            memcpy(msi_data, &v, 4);
            trans_array[req_idx]->set_address(msi_iova);
            trans_array[req_idx]->set_data_ptr(msi_data);
            trans_array[req_idx]->set_data_length(4);
            trans_array[req_idx]->set_command(TLM_WRITE_COMMAND);
            ext_array[req_idx] = new PayloadExtention();
            ext_array[req_idx]->requester_id = 0x0A;
            ext_array[req_idx]->pid_valid = 0;
            ext_array[req_idx]->process_id = 0;
            ext_array[req_idx]->exec_req = 0;
            ext_array[req_idx]->priv_req = 0;
            ext_array[req_idx]->no_write = 0;
            ext_array[req_idx]->at = 0;
            ext_array[req_idx]->is_ctrl = 1;  // MSI: 不计入IOPS
            trans_array[req_idx]->set_extension(ext_array[req_idx]);
            expected_pa[req_idx] = ((S13_FLAT_PPN_BASE + v) << 12) | S13_MSI_OFFSET;
            req_type[req_idx] = 3;  // MSI
            req_idx++;
        }

        if (req_idx != NUM_REQUESTS) {
            printf("[TEST] ERROR: built %d packets, expected %d\n", req_idx, NUM_REQUESTS);
        }

        // ============================================================
        // [场景13-v3] 注入时序控制:
        //   Data: 背靠背注入(端口带宽自然形成4ns间隔)
        //   SQ/CQ/MSI: 理想0耗时, 不消耗注入时间(无wait)
        // ============================================================
        for (int i = 0; i < NUM_REQUESTS; i++) {
            sc_time delay = SC_ZERO_TIME;
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            axi_master_to_pcie_noc_0_socket->nb_transport_fw(*trans_array[i], phase, delay);

            if ((i + 1) % 1000 == 0) {
                printf("[TEST] Progress: %d/%d packets injected\n", i + 1, NUM_REQUESTS);
                fflush(stdout);
            }
        }

        printf("[TEST] All %d packets injected. Waiting for responses...\n", NUM_REQUESTS);
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

        printf("[TEST] Done. response_count=%d / %d\n", response_count, NUM_REQUESTS);

        // ============================================================
        // [场景13-v2] 校验: Data/SQ/CQ/MSI 分类统计
        // ============================================================
        int pass_count = 0;
        int data_pass = 0, sq_pass = 0, cq_pass = 0, msi_pass = 0;
        const char* type_names[] = {"Data", "SQ", "CQ", "MSI"};
        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t result_pa = trans_array[i]->get_address();
            tlm::tlm_response_status status = trans_array[i]->get_response_status();

            if (status == tlm::TLM_OK_RESPONSE && result_pa == expected_pa[i]) {
                pass_count++;
                switch (req_type[i]) {
                    case 0: data_pass++; break;
                    case 1: sq_pass++; break;
                    case 2: cq_pass++; break;
                    case 3: msi_pass++; break;
                }
            } else {
                printf("[TEST] FAIL req %4d (%s): IOVA=0x%lx, expected PA=0x%lx, got PA=0x%lx, status=%s\n",
                       i, type_names[req_type[i]], trans_array[i]->get_address(),
                       expected_pa[i], result_pa, trans_array[i]->get_response_string().c_str());
            }
        }

        printf("\n[TEST] Validation: %d/%d passed\n", pass_count, NUM_REQUESTS);
        printf("[TEST]   Data: %d/%d, SQ: %d/%d, CQ: %d/%d, MSI: %d/%d\n",
               data_pass, NUM_IO_REQS, sq_pass, NUM_SQ_REQS,
               cq_pass, NUM_CQ_REQS, msi_pass, NUM_MSI_REQS);
        if (pass_count == NUM_REQUESTS) {
            printf("[TEST] PASS: All %d mixed packets translated correctly!\n", NUM_REQUESTS);
        } else {
            printf("[TEST] FAIL: Some packets failed translation!\n");
        }

        // ============================================================
        // [场景13-v4] 输入模式验证: 确认是否严格按照11个任务一组输入
        // ============================================================
        printf("\n[TEST] Input Pattern Verification:\n");
        int pattern_violations = 0;
        int expected_group_size = 11;  // 8 Data + 1 SQ + 1 CQ + 1 MSI
        int total_groups = NUM_REQUESTS / expected_group_size;
        
        // 验证每组是否严格按照 8 Data + 1 SQ + 1 CQ + 1 MSI 的顺序
        for (int g = 0; g < NUM_GROUPS; g++) {
            int base_idx = g * expected_group_size;
            
            // 检查前8个是否为Data (req_type=0)
            for (int i = 0; i < 8; i++) {
                if (base_idx + i < NUM_REQUESTS && req_type[base_idx + i] != 0) {
                    printf("[TEST] VIOLATION: Group %d, position %d: expected Data(0), got %d\n",
                           g, i, req_type[base_idx + i]);
                    pattern_violations++;
                }
            }
            
            // 检查第9个是否为SQ (req_type=1)
            if (base_idx + 8 < NUM_REQUESTS && req_type[base_idx + 8] != 1) {
                printf("[TEST] VIOLATION: Group %d, position 8: expected SQ(1), got %d\n",
                       g, req_type[base_idx + 8]);
                pattern_violations++;
            }
            
            // 检查第10个是否为CQ (req_type=2)
            if (base_idx + 9 < NUM_REQUESTS && req_type[base_idx + 9] != 2) {
                printf("[TEST] VIOLATION: Group %d, position 9: expected CQ(2), got %d\n",
                       g, req_type[base_idx + 9]);
                pattern_violations++;
            }
            
            // 检查第11个是否为MSI (req_type=3)
            if (base_idx + 10 < NUM_REQUESTS && req_type[base_idx + 10] != 3) {
                printf("[TEST] VIOLATION: Group %d, position 10: expected MSI(3), got %d\n",
                       g, req_type[base_idx + 10]);
                pattern_violations++;
            }
        }
        
        if (pattern_violations == 0) {
            printf("[TEST] PASS: Input pattern strictly follows 11-task groups (8 Data + 1 SQ + 1 CQ + 1 MSI)\n");
            printf("[TEST]   Total groups: %d, Tasks per group: %d\n", NUM_GROUPS, expected_group_size);
        } else {
            printf("[TEST] FAIL: Found %d pattern violations\n", pattern_violations);
        }

        // ============================================================
        // [场景13-v4] 专属统计 (所有任务均计入IOPS)
        // ============================================================
        printf("\n========== Scene 13-v4 Mixed Statistics ==========\n");
        printf("  Total requests (IOPS counted): %d\n", NUM_REQUESTS);
        printf("    - 512B Data tasks:  %d (%.2f%%)\n", NUM_IO_REQS, 100.0 * NUM_IO_REQS / NUM_REQUESTS);
        printf("    - 32B SQ tasks:     %d (%.2f%%)\n", NUM_SQ_REQS, 100.0 * NUM_SQ_REQS / NUM_REQUESTS);
        printf("    - 16B CQ tasks:     %d (%.2f%%)\n", NUM_CQ_REQS, 100.0 * NUM_CQ_REQS / NUM_REQUESTS);
        printf("    - 4B MSI tasks:     %d (%.2f%%)\n", NUM_MSI_REQS, 100.0 * NUM_MSI_REQS / NUM_REQUESTS);
        printf("  MSIPT Cache: hit=%lu, miss=%lu (hit rate %.1f%%)\n",
               (unsigned long)g_msipt_cache_hit_count, (unsigned long)g_msipt_cache_miss_count,
               (g_msipt_cache_hit_count + g_msipt_cache_miss_count) > 0 ?
               100.0 * g_msipt_cache_hit_count / (g_msipt_cache_hit_count + g_msipt_cache_miss_count) : 0.0);
        printf("=====================================================\n");

        // Cleanup
        for (int i = 0; i < NUM_REQUESTS; i++) {
            trans_array[i]->clear_extension(ext_array[i]);
            delete ext_array[i];
            if (trans_array[i]->get_data_ptr()) {
                delete[] trans_array[i]->get_data_ptr();
            }
            delete trans_array[i];
        }
        delete[] trans_array;
        delete[] ext_array;
        delete[] expected_pa;
        delete[] req_type;

        printf("\n[TEST] Scene 13 mixed test completed!\n");

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
            printf("  Buffer Full Bypass: %lu tasks\n", (unsigned long)iommu_ptr->cache_sub.get_dedup_buffer_full_bypass_count());
            // [STAT需求] buffer实际占用率(时间加权平均): 反映buffer真实使用强度
            printf("  --- Buffer 实际占用率 (时间加权平均) ---\n");
            printf("    Avg Occupancy:    %.2f entries / %u (%.2f%%)\n",
                   dedup_buf->get_avg_occupancy(), PT_DEDUP_BUFFER_SIZE, dedup_buf->get_avg_occupancy_pct());
            printf("    Occupancy window: %.1f ns\n", dedup_buf->get_occupancy_window_ns());
            // [STAT] buffer任务持有延时: 从申请(allocate_entry)到PTW完成释放(free_entry)
            printf("  --- Buffer Task Hold Time (alloc -> PTW-done free) ---\n");
            printf("    Avg hold:  %.1f ns\n", dedup_buf->get_avg_hold_ns());
            printf("    Max hold:  %.1f ns\n", dedup_buf->get_max_hold_ns());
            printf("    Min hold:  %.1f ns\n", dedup_buf->get_min_hold_ns());
            printf("    Samples:   %lu\n", (unsigned long)dedup_buf->get_hold_count());
            printf("==================================================\n");
        }

        // Print Cache hit rate statistics
        iommu_ptr->print_cache_statistics();

        // Stop simulation
        sc_core::sc_stop();

        return;
    }
}
