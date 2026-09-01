#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"
#include "iommu_translate.hh"
#include <cstdio>
#include <iostream>
using namespace std;

// ============================================================
// MSI 通路性能模型功能验证 (方案修订版 v2, 点1~点12)
// 请求数可配置: make TEST=msi_perf MSI_N=1/10/100/1000
//
// 三个设备覆盖三种分流场景:
//   设备0x0B 场景A: S1=Bare(输入即GPA, 只开启一级翻译) + iohgatp=Sv48x4
//            -> 获取DC后直接MSI识别, 命中直达MSIPT大模块(点2)
//   设备0x0C 场景B: 两级翻译 (iosatp=Sv48 + iohgatp=Sv48x4)
//            -> 原始流程(PT Cache/Walker/dedup/PTW), S1完成得GPA后PTW内
//               识别, 跳过S2/预取, 带is_msi刷新缓存后转MSIPT(点3/4);
//               二轮请求经MSIPT Cache命中直出(点8/9)
//   设备0x0D 场景C: 单级翻译 (iosatp=Sv48, iohgatp=Bare)
//            -> PTW S1完成(无S2)后识别 -> 刷新 -> MSIPT
//
// MSI窗口: (GPA>>12) & ~0xFF == 0x1200 (GPA 0x1200000~0x12FFFFF)
// 注: pattern低位(掩码区域内)必须为0, 否则 I=extract(A>>12,mask) 会带pattern低位偏移
// MSI PTE: v0-7 Flat(M=3), v8 MRIF(M=1), v9 V=0(cause262), v10 M=0(cause263)
//          v11-63 Flat(M=3) 递增页, 支持大规模请求
// 普通请求与MSI请求IOVA段完全不重叠(点11)
// ============================================================

extern uint64_t g_msipt_cache_hit_count;
extern uint64_t g_msipt_cache_miss_count;

#ifndef TEST_CFG_MSI_REQS
#define TEST_CFG_MSI_REQS 10
#endif

// ---- MSI窗口/目标地址常量 (全部位于32MB DDR空间内) ----
static const uint64_t MSI_MASK         = 0xFF;
static const uint64_t MSI_PATTERN      = 0x1200;
static const uint64_t MSI_GPA_BASE     = MSI_PATTERN << 12;   // 0x1200000
static const uint64_t FLAT_PPN_A       = 0x800;               // dev A Flat目标基址
static const uint64_t FLAT_PPN_C       = 0x900;               // dev C
static const uint64_t FLAT_PPN_D       = 0xA00;               // dev D
static const uint64_t MRIF_DEST_ADDR   = 0x1C00000;           // MRIF pending区
static const uint64_t NOTICE_NPPN_A    = 0xB00;               // dev A notice页
static const uint64_t NOTICE_NPPN_C    = 0xB80;               // dev C notice页
static const uint32_t NID_A            = 0x21;
static const uint32_t NID_C            = 0x22;

static const uint64_t NORMAL_GPA_BASE  = 0x200000;            // 普通页GPA基址(A/B共用)
static const uint64_t SPA_OFFSET       = 0x100000;            // S2: SPA=GPA+offset
static const uint64_t DEVD_PA_BASE     = 0x640000;            // dev D 普通页PA基址

// 写一笔16字节MSI PTE到指定设备的MSI页表
static void write_msi_pte(RP_Module* rp, uint64_t msipt_base, uint32_t vec, msipte_t pte) {
    rp->write_memory_test_rp((char*)&pte.raw[0], msipt_base + vec * 16, 16);
}

void RP_Module::send_translation_request_1_thread()
{
    while (true)
    {
        wait(10, SC_NS);
        wait(10, SC_NS);

        const int MSI_REQS = TEST_CFG_MSI_REQS;
        printf("\n========== IOMMU MSI Path Test (MSI_N=%d, 场景A直达/场景B两级/场景C单级) ==========\n",
               MSI_REQS);

        fail_if((enable_iommu(iommu_ptr, DDT_1LVL) < 0));

        extern uint64_t next_free_page;
        extern uint64_t next_free_gpage[65536];
        next_free_page = 240;
        // G-stage页分配器偏移: 避开MSI窗口(0x1230xxx)与普通GPA(0x200xxx)
        next_free_gpage[1] = 0x1400;
        next_free_gpage[2] = 0x1400;
        // 地址空间布局: 普通GPA 0x200xxx | dev D PA 0x640xxx | MSI窗口 0x1200000~0x12FFFFF
        // | GPPN区 0x1400000+ | MRIF区 0x1C00000 —— 互不重叠(点11)

        // ============================================================
        // 设备0x0B: 场景A (S1=Bare, G-stage=Sv48x4, MSIPTP=Flat)
        // ============================================================
        uint64_t dcB_addr = add_device(iommu_ptr, 0x0B, 1, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Sv48x4, IOSATP_Bare, PDTP_Bare,
                                       MSIPTP_Flat, 1, MSI_MASK, MSI_PATTERN);
        device_context_t DCB;
        read_memory_test_rp(dcB_addr, sizeof(device_context_t), (char*)&DCB);
        printf("[DEV_B] iosatp.MODE=%d(Bare), iohgatp.MODE=%d(Sv48x4), msiptp.MODE=%d, msiptp.PPN=0x%lx\n",
               DCB.fsc.iosatp.MODE, DCB.iohgatp.MODE, DCB.msiptp.MODE, (unsigned long)DCB.msiptp.PPN);

        // dev B 普通页 G-stage映射: GPA 0x200000+p*4K -> SPA = GPA+0x100000
        {
            gpte_t gpte; gpte.raw = 0;
            gpte.V = 1; gpte.R = 1; gpte.W = 1; gpte.U = 1; gpte.A = 1; gpte.D = 1; gpte.PBMT = PMA;
            for (int p = 0; p < 4; p++) {
                uint64_t gpa = NORMAL_GPA_BASE + p * 0x1000;
                gpte.PPN = (gpa + SPA_OFFSET) / PAGESIZE;
                add_g_stage_pte(iommu_ptr, DCB.iohgatp, gpa, gpte, 0);
            }
        }

        // ============================================================
        // 设备0x0C: 场景B (两级: iosatp=Sv48 + iohgatp=Sv48x4, MSIPTP=Flat)
        // ============================================================
        uint64_t dcC_addr = add_device(iommu_ptr, 0x0C, 2, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Sv48x4, IOSATP_Sv48, PDTP_Bare,
                                       MSIPTP_Flat, 1, MSI_MASK, MSI_PATTERN);
        device_context_t DCC;
        read_memory_test_rp(dcC_addr, sizeof(device_context_t), (char*)&DCC);
        printf("[DEV_C] iosatp.MODE=%d(Sv48), iohgatp.MODE=%d(Sv48x4), msiptp.MODE=%d, msiptp.PPN=0x%lx\n",
               DCC.fsc.iosatp.MODE, DCC.iohgatp.MODE, DCC.msiptp.MODE, (unsigned long)DCC.msiptp.PPN);

        // ============================================================
        // 设备0x0D: 场景C (单级: iosatp=Sv48, iohgatp=Bare, MSIPTP=Flat)
        // ============================================================
        uint64_t dcD_addr = add_device(iommu_ptr, 0x0D, 3, 0, 0, 0, 0, 0,
                                       1, 1, 0, 0, 0,
                                       IOHGATP_Bare, IOSATP_Sv48, PDTP_Bare,
                                       MSIPTP_Flat, 1, MSI_MASK, MSI_PATTERN);
        device_context_t DCD;
        read_memory_test_rp(dcD_addr, sizeof(device_context_t), (char*)&DCD);
        printf("[DEV_D] iosatp.MODE=%d(Sv48), iohgatp.MODE=%d(Bare), msiptp.MODE=%d, msiptp.PPN=0x%lx\n",
               DCD.fsc.iosatp.MODE, DCD.iohgatp.MODE, DCD.msiptp.MODE, (unsigned long)DCD.msiptp.PPN);

        // [重要] 此处不得重置next_free_page: MSI页表页已由add_device分配,
        // 重置会使后续页表页分配与MSI页表页重叠导致页表损坏。

        // ============================================================
        // S1/G-stage 页表映射
        // ============================================================
        spte_t pte_leaf;
        pte_leaf.raw = 0;
        pte_leaf.V = 1; pte_leaf.R = 1; pte_leaf.W = 1; pte_leaf.X = 0;
        pte_leaf.U = 1; pte_leaf.G = 0; pte_leaf.A = 1; pte_leaf.D = 1; pte_leaf.PBMT = PMA;

        const int NORM_PAGES = (MSI_REQS >= 8) ? 8 : 4;

        // dev C: 普通页 IOVA 0x400000+p*4K -> GPA 0x200000+p*4K (S2再映射到SPA)
        // dev C: MSI页  IOVA 0x300000+v*4K -> GPA 窗口 0x1230000+v*4K (无S2映射!)
        for (int p = 0; p < NORM_PAGES; p++) {
            uint64_t iova = 0x400000 + p * 0x1000;
            pte_leaf.PPN = (NORMAL_GPA_BASE + p * 0x1000) / PAGESIZE;
            add_vs_stage_pte(iommu_ptr, DCC.fsc.iosatp, iova, pte_leaf, 0, DCC.iohgatp, 0);
        }
        for (int v = 0; v < 64; v++) {
            uint64_t iova = 0x300000 + v * 0x1000;
            pte_leaf.PPN = (MSI_GPA_BASE + v * 0x1000) / PAGESIZE;
            add_vs_stage_pte(iommu_ptr, DCC.fsc.iosatp, iova, pte_leaf, 0, DCC.iohgatp, 0);
        }
        // dev C: 普通GPA的S2映射 (窗口GPA故意不建映射, 反向验证MSI跳过S2)
        {
            gpte_t gpte; gpte.raw = 0;
            gpte.V = 1; gpte.R = 1; gpte.W = 1; gpte.U = 1; gpte.A = 1; gpte.D = 1; gpte.PBMT = PMA;
            for (int p = 0; p < NORM_PAGES; p++) {
                uint64_t gpa = NORMAL_GPA_BASE + p * 0x1000;
                gpte.PPN = (gpa + SPA_OFFSET) / PAGESIZE;
                add_g_stage_pte(iommu_ptr, DCC.iohgatp, gpa, gpte, 0);
            }
        }

        // dev D: 普通页 IOVA 0x600000+p*4K -> PA 0x640000+p*4K
        // dev D: MSI页  IOVA 0x500000+v*4K -> GPA 窗口 0x1230000+v*4K
        for (int p = 0; p < NORM_PAGES; p++) {
            uint64_t iova = 0x600000 + p * 0x1000;
            pte_leaf.PPN = (DEVD_PA_BASE + p * 0x1000) / PAGESIZE;
            add_s_stage_pte(DCD.fsc.iosatp, iova, pte_leaf, 0, 0);
        }
        for (int v = 0; v < 64; v++) {
            uint64_t iova = 0x500000 + v * 0x1000;
            pte_leaf.PPN = (MSI_GPA_BASE + v * 0x1000) / PAGESIZE;
            add_s_stage_pte(DCD.fsc.iosatp, iova, pte_leaf, 0, 0);
        }

        // ============================================================
        // 各设备 MSI 页表 (16B/PTE, 64 vectors)
        // ============================================================
        msipte_t mpte;
        // dev B: Flat v0-63 -> PPN 0x800+v; v8改为MRIF; v9 V=0; v10 M=0
        uint64_t msiptB = DCB.msiptp.PPN * PAGESIZE;
        for (int v = 0; v < 64; v++) {
            mpte.raw[0] = 0; mpte.raw[1] = 0;
            mpte.V = 1; mpte.M = 3; mpte.translate_rw.PPN = FLAT_PPN_A + v;
            write_msi_pte(this, msiptB, v, mpte);
        }
        mpte.raw[0] = 0; mpte.raw[1] = 0;
        mpte.V = 1; mpte.M = 1;
        mpte.mrif.MRIF_ADDR_55_9 = MRIF_DEST_ADDR >> 9;
        mpte.mrif.NPPN = NOTICE_NPPN_A; mpte.mrif.N90 = NID_A & 0x3FF; mpte.mrif.N10 = (NID_A >> 10) & 1;
        write_msi_pte(this, msiptB, 8, mpte);
        mpte.raw[0] = 0; mpte.raw[1] = 0;                          // v9: V=0
        write_msi_pte(this, msiptB, 9, mpte);
        mpte.raw[0] = 0; mpte.raw[1] = 0; mpte.V = 1; mpte.M = 0;  // v10: M=0
        write_msi_pte(this, msiptB, 10, mpte);

        // dev C: Flat v0-63 -> PPN 0x900+v; v8 MRIF
        uint64_t msiptC = DCC.msiptp.PPN * PAGESIZE;
        for (int v = 0; v < 64; v++) {
            mpte.raw[0] = 0; mpte.raw[1] = 0;
            mpte.V = 1; mpte.M = 3; mpte.translate_rw.PPN = FLAT_PPN_C + v;
            write_msi_pte(this, msiptC, v, mpte);
        }
        mpte.raw[0] = 0; mpte.raw[1] = 0;
        mpte.V = 1; mpte.M = 1;
        mpte.mrif.MRIF_ADDR_55_9 = MRIF_DEST_ADDR >> 9;
        mpte.mrif.NPPN = NOTICE_NPPN_C; mpte.mrif.N90 = NID_C & 0x3FF; mpte.mrif.N10 = (NID_C >> 10) & 1;
        write_msi_pte(this, msiptC, 8, mpte);

        // dev D: Flat v0-63 -> PPN 0xA00+v
        uint64_t msiptD = DCD.msiptp.PPN * PAGESIZE;
        for (int v = 0; v < 64; v++) {
            mpte.raw[0] = 0; mpte.raw[1] = 0;
            mpte.V = 1; mpte.M = 3; mpte.translate_rw.PPN = FLAT_PPN_D + v;
            write_msi_pte(this, msiptD, v, mpte);
        }
        printf("[TEST] MSI page tables ready: B@0x%lx, C@0x%lx, D@0x%lx\n",
               (unsigned long)msiptB, (unsigned long)msiptC, (unsigned long)msiptD);

        // Invalidate caches
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0B, 0);
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0C, 0);
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0D, 0);
        iotinval(iommu_ptr, VMA, 0, 0, 0, 0, 0, 0);

        iommu_ptr->next_task_id = 1;
        response_count = 0;
        g_msipt_cache_hit_count = 0;
        g_msipt_cache_miss_count = 0;

        // ============================================================
        // 请求注入 (分两阶段: 阶段1全部完成后再注入阶段2,
        // 保证二轮请求能命中MSIPT Cache/PT Cache is_msi条目)
        // check_mode: 0=OK+PA校验, 1=非OK响应(故障), 2=MRIF(响应地址+DDR回读)
        // ============================================================
        const int MAX_REQS = 7 * MSI_REQS + 256;
        tlm_generic_payload** trans_array = new tlm_generic_payload*[MAX_REQS];
        PayloadExtention** ext_array = new PayloadExtention*[MAX_REQS];
        int* check_mode = new int[MAX_REQS];
        uint64_t* expected_pa = new uint64_t[MAX_REQS];
        uint64_t* mrif_rd_addr = new uint64_t[MAX_REQS];
        uint32_t* mrif_rd_exp = new uint32_t[MAX_REQS];
        int idx = 0;

        auto inject = [&](uint16_t rid, uint64_t iova, uint32_t data_len, uint32_t msi_data) {
            if (idx >= MAX_REQS) return;
            trans_array[idx] = new tlm_generic_payload();
            unsigned char* data = new unsigned char[data_len]();
            if (data_len == 4) memcpy(data, &msi_data, 4);
            trans_array[idx]->set_address(iova);
            trans_array[idx]->set_data_ptr(data);
            trans_array[idx]->set_data_length(data_len);
            trans_array[idx]->set_command(TLM_WRITE_COMMAND);
            ext_array[idx] = new PayloadExtention();
            ext_array[idx]->requester_id = rid;
            ext_array[idx]->pid_valid = 0;
            ext_array[idx]->process_id = 0;
            ext_array[idx]->at = 0;
            trans_array[idx]->set_extension(ext_array[idx]);
            idx++;
        };

        // ---- 场景A (dev 0x0B): S1=Bare, DC/PC后直达MSIPT ----
        // 普通写 (输入即GPA, 窗口外 -> 正常S2翻译)
        for (int p = 0; p < 4; p++) {
            uint64_t iova = NORMAL_GPA_BASE + p * 0x1000 + 0x100;
            inject(0x0B, iova, 512, 0);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = iova + SPA_OFFSET;
        }
        // Flat MSI 首轮 (MSIPTW walk + 回填MSIPT Cache)
        for (int i = 0; i < MSI_REQS; i++) {
            int v = i % 64;
            if (v == 8 || v == 9 || v == 10) continue;  // MRIF/故障vector单独注入
            uint64_t offset = 0x40;
            inject(0x0B, MSI_GPA_BASE + v * 0x1000 + offset, 4, v);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = ((FLAT_PPN_A + v) << 12) | offset;
        }
        // MRIF MSI (iid=37 -> pending word 0x1C00004, bit 0x20)
        inject(0x0B, MSI_GPA_BASE + 8 * 0x1000, 4, 37);
        check_mode[idx-1] = 2;
        expected_pa[idx-1] = NOTICE_NPPN_A << 12;
        mrif_rd_addr[idx-1] = MRIF_DEST_ADDR + 4;
        mrif_rd_exp[idx-1] = 0x20;
        // 故障注入: v9(V=0 -> 262), v10(M=0 -> 263)
        inject(0x0B, MSI_GPA_BASE + 9 * 0x1000, 4, 0);
        check_mode[idx-1] = 1; expected_pa[idx-1] = 0;
        inject(0x0B, MSI_GPA_BASE + 10 * 0x1000, 4, 0);
        check_mode[idx-1] = 1; expected_pa[idx-1] = 0;

        // ---- 场景B (dev 0x0C): 两级, PTW S1完成后识别, 跳过S2 ----
        // 普通写 (IOVA -> S1 -> GPA -> S2 -> SPA)
        for (int p = 0; p < NORM_PAGES; p++) {
            uint64_t iova = 0x400000 + p * 0x1000 + 0x80;
            inject(0x0C, iova, 512, 0);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = NORMAL_GPA_BASE + p * 0x1000 + SPA_OFFSET + 0x80;
        }
        // Flat MSI 首轮 (PTW识别+is_msi回填PT/Walker Cache+dedup刷新+转MSIPT)
        for (int i = 0; i < MSI_REQS; i++) {
            int v = i % 64;
            if (v == 8) continue;  // MRIF vector单独注入
            uint64_t offset = 0x40;
            inject(0x0C, 0x300000 + v * 0x1000 + offset, 4, v);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = ((FLAT_PPN_C + v) << 12) | offset;
        }
        // MRIF MSI (iid=70 -> pending word 0x1C00008, bit 0x40)
        inject(0x0C, 0x300000 + 8 * 0x1000, 4, 70);
        check_mode[idx-1] = 2;
        expected_pa[idx-1] = NOTICE_NPPN_C << 12;
        mrif_rd_addr[idx-1] = MRIF_DEST_ADDR + 8;
        mrif_rd_exp[idx-1] = 0x40;

        // ---- 场景C (dev 0x0D): 单级, S1完成后识别(无S2) ----
        for (int p = 0; p < NORM_PAGES; p++) {
            uint64_t iova = 0x600000 + p * 0x1000 + 0x100;
            inject(0x0D, iova, 512, 0);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = DEVD_PA_BASE + p * 0x1000 + 0x100;
        }
        for (int i = 0; i < MSI_REQS; i++) {
            int v = i % 64;
            uint64_t offset = 0x40;
            inject(0x0D, 0x500000 + v * 0x1000 + offset, 4, v);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = ((FLAT_PPN_D + v) << 12) | offset;
        }

        const int PHASE1_REQUESTS = idx;

        // ---- 阶段2: 二轮Flat MSI (验证MSIPT Cache命中 / PT Cache is_msi HIT直达) ----
        for (int i = 0; i < MSI_REQS; i++) {
            int v = i % 64;
            if (v == 8 || v == 9 || v == 10) continue;
            uint64_t offset = 0x40;
            inject(0x0B, MSI_GPA_BASE + v * 0x1000 + offset, 4, v);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = ((FLAT_PPN_A + v) << 12) | offset;
        }
        for (int i = 0; i < MSI_REQS; i++) {
            int v = i % 64;
            if (v == 8) continue;
            uint64_t offset = 0x40;
            inject(0x0C, 0x300000 + v * 0x1000 + offset, 4, v);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = ((FLAT_PPN_C + v) << 12) | offset;
        }
        for (int i = 0; i < MSI_REQS; i++) {
            int v = i % 64;
            uint64_t offset = 0x40;
            inject(0x0D, 0x500000 + v * 0x1000 + offset, 4, v);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = ((FLAT_PPN_D + v) << 12) | offset;
        }

        const int NUM_REQUESTS = idx;
        printf("\n[TEST] Injecting phase1: %d requests (MSI_N=%d per device)...\n",
               PHASE1_REQUESTS, MSI_REQS);
        fflush(stdout);

        for (int i = 0; i < PHASE1_REQUESTS; i++) {
            sc_time delay = SC_ZERO_TIME;
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            axi_master_to_pcie_noc_0_socket->nb_transport_fw(*trans_array[i], phase, delay);
            wait(4, SC_NS);
        }
        printf("[TEST] Phase1 %d requests injected. Waiting for responses...\n", PHASE1_REQUESTS);
        fflush(stdout);
        while (response_count < PHASE1_REQUESTS) {
            wait(response_count_event);
        }
        printf("[TEST] Phase1 done (%d responses). Injecting phase2 (cache-hit round)...\n",
               PHASE1_REQUESTS);
        fflush(stdout);

        for (int i = PHASE1_REQUESTS; i < NUM_REQUESTS; i++) {
            sc_time delay = SC_ZERO_TIME;
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            axi_master_to_pcie_noc_0_socket->nb_transport_fw(*trans_array[i], phase, delay);
            wait(4, SC_NS);
        }

        printf("[TEST] All %d requests injected. Waiting for remaining responses...\n", NUM_REQUESTS);
        fflush(stdout);

        while (response_count < NUM_REQUESTS) {
            wait(response_count_event);
        }
        printf("[TEST] All %d responses received!\n", NUM_REQUESTS);

        // ============================================================
        // Phase3: MSIPT Cache 失效验证 (IODIR.INVAL_DDT 级联失效)
        //   使能CQ -> IODIR.INVAL_DDT DV=1(DID=0x0B) + IOFENCE ->
        //   重新注入 dev B 的 Flat MSI: MSIPT Cache 已失效, 应 MISS 重走,
        //   miss 计数增长且翻译结果不变(验证失效通路功能正确)
        // ============================================================
        fail_if((enable_cq(iommu_ptr, 1) < 0));
        const uint64_t miss_before_inval = g_msipt_cache_miss_count;
        iodir(iommu_ptr, INVAL_DDT, 1, 0x0B, 0);
        iofence(iommu_ptr, IOFENCE_C, 0, 0, 0, 0, 0, 0);
        printf("[TEST] IODIR.INVAL_DDT(DV=1, DID=0x0B) + IOFENCE issued\n");
        fflush(stdout);

        const int INVAL_RETEST_N = (MSI_REQS < 8) ? MSI_REQS : 8;
        for (int i = 0; i < INVAL_RETEST_N; i++) {
            int v = i % 64;
            if (v == 8 || v == 9 || v == 10) continue;
            uint64_t offset = 0x40;
            inject(0x0B, MSI_GPA_BASE + v * 0x1000 + offset, 4, v);
            check_mode[idx-1] = 0;
            expected_pa[idx-1] = ((FLAT_PPN_A + v) << 12) | offset;
        }
        const int NUM_REQUESTS_INV = idx;
        for (int i = NUM_REQUESTS; i < NUM_REQUESTS_INV; i++) {
            sc_time delay = SC_ZERO_TIME;
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            axi_master_to_pcie_noc_0_socket->nb_transport_fw(*trans_array[i], phase, delay);
            wait(4, SC_NS);
        }
        while (response_count < NUM_REQUESTS_INV) {
            wait(response_count_event);
        }
        const uint64_t miss_delta = g_msipt_cache_miss_count - miss_before_inval;
        printf("[TEST] After INVAL_DDT retest: MSIPT miss_delta=%lu (expect >=1)\n",
               (unsigned long)miss_delta);
        fflush(stdout);

        // ============================================================
        // 校验
        // ============================================================
        int pass_count = 0;
        for (int i = 0; i < NUM_REQUESTS_INV; i++) {
            tlm::tlm_response_status status = trans_array[i]->get_response_status();
            uint64_t result_pa = trans_array[i]->get_address();
            bool ok = false;

            if (check_mode[i] == 0) {
                ok = (status == tlm::TLM_OK_RESPONSE && result_pa == expected_pa[i]);
            } else if (check_mode[i] == 1) {
                ok = (status != tlm::TLM_OK_RESPONSE);
            } else {
                // MRIF: 响应地址=notice地址 + DDR中pending word已写入
                uint32_t mem_val = 0;
                read_memory_test_rp(mrif_rd_addr[i], 4, (char*)&mem_val);
                ok = (status == tlm::TLM_OK_RESPONSE &&
                      result_pa == expected_pa[i] &&
                      mem_val == mrif_rd_exp[i]);
                printf("[TEST] MRIF check req %d: rsp_addr=0x%lx (exp 0x%lx), DDR[0x%lx]=0x%x (exp 0x%x)\n",
                       i, result_pa, expected_pa[i], mrif_rd_addr[i], mem_val, mrif_rd_exp[i]);
            }

            if (ok) {
                pass_count++;
            } else {
                printf("[TEST] FAIL req %3d: mode=%d, iova=0x%lx, exp=0x%lx, got=0x%lx, status=%s\n",
                       i, check_mode[i], trans_array[i]->get_address(), expected_pa[i], result_pa,
                       trans_array[i]->get_response_string().c_str());
            }
        }

        printf("\n[TEST] Validation: %d/%d passed\n", pass_count, NUM_REQUESTS_INV);

        // MSIPT Cache命中校验: 阶段2的Flat MSI应命中 (dev A/C/D各MSI_REQS笔上下)
        printf("[TEST] MSIPT Cache: hit=%lu, miss=%lu\n",
               (unsigned long)g_msipt_cache_hit_count, (unsigned long)g_msipt_cache_miss_count);
        bool cache_ok = (g_msipt_cache_hit_count >= (uint64_t)MSI_REQS);
        // Phase3校验: 失效后重测必须产生 MSIPT Cache MISS(失效已生效)
        bool inval_ok = (miss_delta >= 1);

        if (pass_count == NUM_REQUESTS_INV && cache_ok && inval_ok) {
            printf("[TEST] PASS: MSI path + MSIPT invalidation (MSI_N=%d) all verified!\n", MSI_REQS);
        } else {
            printf("[TEST] FAIL: %d reqs failed, cache_ok=%d, inval_ok=%d (miss_delta=%lu)\n",
                   NUM_REQUESTS_INV - pass_count, cache_ok ? 1 : 0, inval_ok ? 1 : 0,
                   (unsigned long)miss_delta);
        }

        // Cleanup
        for (int i = 0; i < NUM_REQUESTS_INV; i++) {
            trans_array[i]->clear_extension(ext_array[i]);
            delete ext_array[i];
            if (trans_array[i]->get_data_ptr()) {
                delete[] trans_array[i]->get_data_ptr();
            }
            delete trans_array[i];
        }
        delete[] trans_array;
        delete[] ext_array;
        delete[] check_mode;
        delete[] expected_pa;
        delete[] mrif_rd_addr;
        delete[] mrif_rd_exp;

        printf("\n[TEST] MSI path test completed!\n");

        iommu_ptr->print_cache_statistics();

        sc_core::sc_stop();
        return;
    }
}
