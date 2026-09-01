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
// 场景13: 4KB随机读写 + MSI 混合负载 (两阶段翻译 + S2开启)
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
// 负载模式: 每组 = 1个4KB随机读写任务(8x512B连续, 前4读后4写)
//           + 1个MSI任务(4B写, vector随机);
//   1111组 x 9包 + 末尾补1个MSI = 10000包 (IO 8888 + MSI 1112)
// ============================================================

// ---- 场景13 MSI常量 ----
static const uint64_t S13_MSI_MASK      = 0xFF;
static const uint64_t S13_MSI_PATTERN   = 0x3000;
static const uint64_t S13_MSI_GPA_BASE  = S13_MSI_PATTERN << 12;  // 0x3000000 (48MB)
static const int      S13_MSI_VECTORS   = 256;                    // mask=0xFF -> 256 vectors, PTE表恰好4KB/1页
static const uint64_t S13_FLAT_PPN_BASE = 0x1000;                 // Flat输出物理页基址(16MB, 32MB DDR内且避开页表分配区)
static const uint64_t S13_MSI_IOVA_BASE = 0x2000000;              // MSI IOVA区基址(32MB, 与普通16MB IOVA范围不重合)
static const int      S13_MSI_IOVA_CAND = 1024;                   // MSI IOVA候选页数(4MB区域), 从中随机挑256个不连续页
static const uint64_t S13_MSI_OFFSET    = 0x40;                   // MSI写页内偏移

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
        const uint64_t RANGE_16MB  = 0x1000000;        // 16MB
        const uint64_t HUGE_PAGE_SZ = 0x200000;        // 2MB (GPA大页粒度)
        const uint64_t SPA_OFFSET  = 0x80000000;       // SPA = GPA + 固定偏移(2MB对齐)
        const int NUM_GPA_HUGEPAGES = 20;              // 20个2MB大页GPA
        const int SLOTS_PER_HUGEPAGE = (int)(HUGE_PAGE_SZ / 0x1000);  // 512个4KB slot
        // [场景化] 默认1111页; Makefile传入TEST_CFG_NUM_PAGES=1111(8888 IO请求)
#ifndef TEST_CFG_NUM_PAGES
        const int PAGES_NEEDED     = 1111;             // 1111组 × 8 = 8888 IO请求
#else
        const int PAGES_NEEDED     = TEST_CFG_NUM_PAGES;
#endif
        const int REQ_PER_PAGE     = 8;                // 8 × 512B = 4KB
        const int NUM_GROUPS       = PAGES_NEEDED;     // 每组1个4KB页 + 1个MSI
        const int NUM_IO_REQS      = NUM_GROUPS * REQ_PER_PAGE;   // 8888
        const int NUM_MSI_REQS     = NUM_GROUPS + 1;              // 1112 (末尾补1个)
        const int NUM_REQUESTS     = NUM_IO_REQS + NUM_MSI_REQS;  // 10000
        const int TOTAL_PAGES_IN_RANGE = (int)(RANGE_16MB / 0x1000);  // 4096

        printf("\n[TEST] Scene 13 Mixed Load Construction:\n");
        printf("[TEST]   Normal IOVA range: 0x%lx ~ 0x%lx (16MB, %d pages, ALL mapped)\n",
               IOVA_BASE, IOVA_BASE + RANGE_16MB, TOTAL_PAGES_IN_RANGE);
        printf("[TEST]   MSI IOVA range:    0x%lx ~ 0x%lx (random %d of %d pages, NON-contiguous)\n",
               S13_MSI_IOVA_BASE, S13_MSI_IOVA_BASE + (uint64_t)S13_MSI_IOVA_CAND * 0x1000,
               S13_MSI_VECTORS, S13_MSI_IOVA_CAND);
        printf("[TEST]   MSI window GPA:    0x%lx ~ 0x%lx (mask=0x%lx, pattern=0x%lx, Flat)\n",
               S13_MSI_GPA_BASE, S13_MSI_GPA_BASE + 0xFFFFF, S13_MSI_MASK, S13_MSI_PATTERN);
        printf("[TEST]   Pattern: %d groups x (8x512B R/W + 1 MSI) + 1 MSI = %d packets (%d IO + %d MSI)\n",
               NUM_GROUPS, NUM_REQUESTS, NUM_IO_REQS, NUM_MSI_REQS);
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
        // 混合负载注入: 每组 = 8x512B读写(前4读后4写) + 1 MSI(4B写)
        // ============================================================
        printf("\n========== %d-Packet Mixed Test (%d IO + %d MSI) ==========\n",
               NUM_REQUESTS, NUM_IO_REQS, NUM_MSI_REQS);

        iommu_ptr->steady_start_count = NUM_REQUESTS * STEADY_STATE_START_PERCENT / 100;
        iommu_ptr->steady_end_count   = NUM_REQUESTS * STEADY_STATE_END_PERCENT / 100;

        tlm_generic_payload** trans_array = new tlm_generic_payload*[NUM_REQUESTS];
        PayloadExtention** ext_array = new PayloadExtention*[NUM_REQUESTS];
        uint64_t* expected_pa = new uint64_t[NUM_REQUESTS];
        int* req_is_msi = new int[NUM_REQUESTS];

        printf("[TEST] Injecting %d packets (group: 8x512B R/W + 1 MSI, MSI vector random)...\n", NUM_REQUESTS);
        printf("[TEST] Prefetch D=%d, Walker Cache ENABLED (MSI tasks skip prefetch/S2)\n",
               (int)TEST_CFG_PT_DEDUP_PREFETCH_DEPTH);
        fflush(stdout);

        int req_idx = 0;
        int msi_idx = 0;
        for (int g = 0; g < NUM_GROUPS; g++) {
            // ---- 1个4KB随机读写任务: 8个连续512B (offset 0~3读, 4~7写) ----
            int iova_pg = page_indices[g];
            for (int off = 0; off < REQ_PER_PAGE; off++) {
                uint64_t iova = IOVA_BASE + (uint64_t)iova_pg * 0x1000 + off * 0x200;
                uint64_t exp_pa = (uint64_t)iova_huge[iova_pg] * HUGE_PAGE_SZ
                                + (uint64_t)iova_slot[iova_pg] * 0x1000
                                + SPA_OFFSET + (iova & 0xFFF);
                bool is_read = (off < REQ_PER_PAGE / 2);

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
                trans_array[req_idx]->set_extension(ext_array[req_idx]);

                expected_pa[req_idx] = exp_pa;
                req_is_msi[req_idx] = 0;
                req_idx++;
            }

            // ---- 1个MSI任务: 4B写, vector随机, IOVA随机不连续 ----
            int v = msi_vectors[msi_idx];
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
            trans_array[req_idx]->set_extension(ext_array[req_idx]);

            expected_pa[req_idx] = ((S13_FLAT_PPN_BASE + v) << 12) | S13_MSI_OFFSET;
            req_is_msi[req_idx] = 1;
            req_idx++;
            msi_idx++;

            if ((g + 1) % 200 == 0) {
                printf("[TEST] Progress: group %d/%d built (%d packets)\n", g + 1, NUM_GROUPS, req_idx);
                fflush(stdout);
            }
        }

        // ---- 末尾补1个MSI, 凑满10000包 ----
        {
            int v = msi_vectors[msi_idx];
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
            trans_array[req_idx]->set_extension(ext_array[req_idx]);
            expected_pa[req_idx] = ((S13_FLAT_PPN_BASE + v) << 12) | S13_MSI_OFFSET;
            req_is_msi[req_idx] = 1;
            req_idx++;
        }

        if (req_idx != NUM_REQUESTS) {
            printf("[TEST] ERROR: built %d packets, expected %d\n", req_idx, NUM_REQUESTS);
        }

        // ---- 注入 (与场景7一致: 背靠背注入, 入口128GB/s带宽控制平均4ns/包) ----
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
        // 校验: IO请求(状态+PA) + MSI请求(Flat输出PA)
        // ============================================================
        int pass_count = 0;
        int io_pass = 0, msi_pass = 0;
        for (int i = 0; i < NUM_REQUESTS; i++) {
            uint64_t result_pa = trans_array[i]->get_address();
            tlm::tlm_response_status status = trans_array[i]->get_response_status();

            if (status == tlm::TLM_OK_RESPONSE && result_pa == expected_pa[i]) {
                pass_count++;
                if (req_is_msi[i]) msi_pass++; else io_pass++;
            } else {
                printf("[TEST] FAIL req %4d (%s): IOVA=0x%lx, expected PA=0x%lx, got PA=0x%lx, status=%s\n",
                       i, req_is_msi[i] ? "MSI" : "IO", trans_array[i]->get_address(),
                       expected_pa[i], result_pa, trans_array[i]->get_response_string().c_str());
            }
        }

        printf("\n[TEST] Validation: %d/%d passed (IO: %d/%d, MSI: %d/%d)\n",
               pass_count, NUM_REQUESTS, io_pass, NUM_IO_REQS, msi_pass, NUM_MSI_REQS);
        if (pass_count == NUM_REQUESTS) {
            printf("[TEST] PASS: All %d mixed packets (IO+MSI) translated correctly!\n", NUM_REQUESTS);
        } else {
            printf("[TEST] FAIL: Some packets failed translation!\n");
        }

        // ============================================================
        // 场景13专属统计
        // ============================================================
        printf("\n========== Scene 13 MSI Mixed Statistics ==========\n");
        printf("  MSI requests injected: %d\n", NUM_MSI_REQS);
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
        delete[] req_is_msi;

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
            printf("==================================================\n");
        }

        // Print Cache hit rate statistics
        iommu_ptr->print_cache_statistics();

        // Stop simulation
        sc_core::sc_stop();

        return;
    }
}
