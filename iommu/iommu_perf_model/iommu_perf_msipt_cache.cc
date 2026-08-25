// IOMMU Performance Model - MSI Identification + MSIPT Cache + MSIPTW Threads
// SPEC Section 12.15-12.18
// Corresponds to msi_address_translation() in iommu_msi_trans.cc
//
// [MSI] 性能模型MSI处理路径 (对应架构 PIPE6 MSIPTW + MSIPT Cache):
//   PTW S1完成/Bare路径/PT Cache HIT路径调用 msi_id_check 识别虚拟 interrupt file页;
//   命中后经 route_to_msipt 改道到本模块:
//     collector_to_msipt_cache_query_fifo -> msipt_cache_query_thread (MSIPT Cache查询)
//       HIT  -> msipte_decode -> msipt_cache_to_fwd_fifo
//       MISS -> msipt_cache_to_msiptw_fifo -> msiptw_req_thread (DDR读16B MSI PTE)
//              -> msiptw_rsp_thread (msipte_decode + Flat回填MSIPT Cache)
//              -> msipt_cache_result_thread -> msipt_cache_to_fwd_fifo

#include "iommu_top.hh"
#include <cstdio>

// ============================================================
// [MSI] msi_id_check - MSI地址识别 (对应功能模型 iommu_msi_trans.cc step1-5)
// 判断GPA是否命中虚拟 interrupt file页; 命中时填充
// task->gpa / task->is_msi / task->walk_ctx.msi_index
// ============================================================
bool iommu_top::msi_id_check(iommu_task_t* task, uint64_t gpa) {
    // step1: DC.msiptp.MODE == Off -> 不做MSI识别
    if (task->DC.msiptp.MODE == MSIPTP_Off)
        return false;

    // step3: (A>>12) & ~mask & mgpaw_mask == pattern & ~mask & mgpaw_mask
    if (!check_is_msi_address(gpa, &task->DC, &iommu_inst))
        return false;

    // step5: I = extract(A >> 12, DC.msi_addr_mask & mgpaw_mask)
    uint64_t mgpaw_bits = calculate_mgpaw(&iommu_inst) - 12;
    uint64_t mgpaw_mask = (mgpaw_bits < 64) ? ((1ULL << mgpaw_bits) - 1) : ~0ULL;
    uint64_t I = msi_extract(gpa >> 12, task->DC.msi_addr_mask.mask & mgpaw_mask);

    task->gpa = gpa;
    task->is_msi = 1;
    task->walk_ctx.msi_index = I;

    printf("[MSI_ID] task_id=%u, gpa=0x%lx -> MSI HIT, msi_index=%llu (msiptp.PPN=0x%lx)\n",
           task->task_id, gpa, (unsigned long long)I, (unsigned long)task->DC.msiptp.PPN);
    fflush(stdout);
    return true;
}

// ============================================================
// [MSI] route_to_msipt - 将命中MSI的任务改道到MSIPT Cache查询路径
// ============================================================
void iommu_top::route_to_msipt(iommu_task_t* task) {
    task->state = TASK_MSI_QUERY;
    collector_to_msipt_cache_query_fifo.write(task);
}

// ============================================================
// [MSI] msipte_decode - 解析16B MSI PTE (对应功能模型 step9-15)
// 返回true表示fault (task->cause已设置); 否则填充翻译结果:
//   Flat(M=3): task->pa = PPN<<12 | A[11:0], is_mrif=0
//   MRIF(M=1): task->dest_mrif_addr/pa(NPPN<<12)/mrif_nid, is_mrif=1
// ============================================================
bool iommu_top::msipte_decode(iommu_task_t* task, const msipte_t& msipte) {
    // step9: V位检查
    if (msipte.V == 0) {
        task->cause = 262;  // MSI PTE not valid
        return true;
    }
    // step10: C位检查 (参考模型无自定义实现, 按misconfigured处理)
    if (msipte.C == 1) {
        task->cause = 263;  // MSI PTE misconfigured
        return true;
    }
    // step12: M == 0/2 -> misconfigured
    if (msipte.M == 0 || msipte.M == 2) {
        task->cause = 263;
        return true;
    }

    if (msipte.M == 3) {
        // step13: Flat (Translate/RW) 模式
        if (msipte.translate_rw.reserved != 0 || msipte.translate_rw.reserved0 != 0) {
            task->cause = 263;
            return true;
        }
        // translated_addr = (msipte.PPN << 12) | A[11:0]
        task->pa = (msipte.translate_rw.PPN * PAGESIZE) | (task->gpa & 0xFFF);
        task->is_mrif = 0;
        task->g_pte.raw = 0;
        task->g_pte.PPN = task->gpa / PAGESIZE;
        task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
        task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
        task->g_pte.X = 0;
        task->g_pte.PBMT = PMA;
        task->page_sz = PAGESIZE;
        task->gst_page_sz = PAGESIZE;
    } else if (msipte.M == 1) {
        // step14: MRIF 模式
        if (iommu_inst.reg_file.capabilities.msi_mrif == 0) {
            task->cause = 263;
            return true;
        }
        if (msipte.mrif.reserved1 != 0 || msipte.mrif.reserved2 != 0 ||
            msipte.mrif.reserved3 != 0 || msipte.mrif.reserved4 != 0) {
            task->cause = 263;
            return true;
        }
        // mrif_addr = MRIF_Address[55:9] * 512
        task->dest_mrif_addr = msipte.mrif.MRIF_ADDR_55_9 << 9;
        // notice_msi_addr = NPPN << 12
        task->pa = msipte.mrif.NPPN * PAGESIZE;
        // NID = (N10 << 10) | N[9:0]
        task->mrif_nid = (msipte.mrif.N10 << 10) | msipte.mrif.N90;
        task->is_mrif = 1;
        task->g_pte.raw = 0;
        task->g_pte.PPN = task->gpa / PAGESIZE;
        task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
        task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
        task->g_pte.X = 0;
        task->g_pte.PBMT = PMA;
        task->page_sz = PAGESIZE;
        task->gst_page_sz = PAGESIZE;
    } else {
        task->cause = 263;
        return true;
    }

    // step15: MSI翻译等价R=W=U=1/X=0的S2 PTE, 取指访问报instruction access fault
    if (task->is_exec == 1 && task->check_access_perms == 1) {
        task->cause = 1;
        return true;
    }
    return false;
}

// ============================================================
// 12.15 msipt_cache_query_thread - MSIPT Cache Query
// [MSI] 接入CacheSubsystem真实MSIPT Cache (替换原恒miss桩):
//   HIT  -> 解码缓存的raw MSI PTE, 直接转发forwarder
//   MISS -> 送MSIPTW发起DDR walk
// ============================================================
void iommu_top::msipt_cache_query_thread() {
    while (true) {
        iommu_task_t* task = collector_to_msipt_cache_query_fifo.read();
        task->state = TASK_MSI_QUERY;

        // MSIPT Cache lookup (msi_scheduler串行处理, 请求/响应按序配对)
        iommu::CacheMessage req;
        req.msg_type  = iommu::CacheMsgType::MSI_LOOKUP;
        req.task_id   = task->task_id;
        req.device_id = task->device_id;
        req.msi_index = (uint32_t)task->walk_ctx.msi_index;
        cache_sub.msi_request_fifo.write(req);
        iommu::CacheMessage resp = cache_sub.msi_response_fifo.read();

        wait(MSIPT_CACHE_HIT_DELAY, SC_NS);

        if (resp.hit && resp.msi_data.valid) {
            // ===== HIT: 解码缓存的MSI PTE =====
            g_msipt_cache_hit_count++;
            task->state = TASK_MSI_HIT;

            msipte_t msipte;
            msipte.raw[0] = resp.msi_data.pte;
            msipte.raw[1] = resp.msi_data.pte_hi;

            if (msipte_decode(task, msipte)) {
                printf("[MSIPT_CACHE] task_id=%u HIT -> decode FAULT cause=%u -> fault_handler\n",
                       task->task_id, task->cause);
                fflush(stdout);
                task->state = TASK_FAULT;
                collector_to_fault_fifo.write(task);
                continue;
            }

            printf("[MSIPT_CACHE] task_id=%u, gpa=0x%lx -> HIT, pa=0x%lx, is_mrif=%d -> forwarder\n",
                   task->task_id, task->gpa, task->pa, task->is_mrif);
            fflush(stdout);
            task->state = TASK_FORWARD;
            msipt_cache_to_fwd_fifo.write(task);
            continue;
        }

        // ===== MISS: 送MSIPTW发起DDR walk =====
        g_msipt_cache_miss_count++;
        task->state = TASK_MSI_MISS;
        printf("[MSIPT_CACHE] task_id=%u, gpa=0x%lx, msi_index=%llu -> MISS, route to MSIPTW\n",
               task->task_id, task->gpa, (unsigned long long)task->walk_ctx.msi_index);
        fflush(stdout);
        msipt_cache_to_msiptw_fifo.write(task);
    }
}

// ============================================================
// 12.16 msipt_cache_result_thread - MSIPT Cache Result (MSIPTW return)
// Receives MSIPTW results, forwards to Forwarder (fault -> fault_handler)
// ============================================================
void iommu_top::msipt_cache_result_thread() {
    while (true) {
        iommu_task_t* task = msiptw_to_msipt_cache_fifo.read();

        if (task->state == TASK_FAULT) {
            printf("[MSIPT_CACHE] task_id=%u -> FAULT cause=%u, route to fault_handler\n",
                   task->task_id, task->cause);
            fflush(stdout);
            collector_to_fault_fifo.write(task);
            continue;
        }

        wait(MSIPT_CACHE_HIT_DELAY, SC_NS);

        // PBMT aggregation (MSI translations)
        task->vs_pte.PBMT = (task->vs_pte.PBMT != PMA) ?
                             task->vs_pte.PBMT : task->g_pte.PBMT;

        printf("[MSIPT_CACHE] task_id=%u, pa=0x%lx, is_mrif=%d -> forwarder\n",
               task->task_id, task->pa, task->is_mrif);
        fflush(stdout);

        task->state = TASK_FORWARD;
        msipt_cache_to_fwd_fifo.write(task);
    }
}

// ============================================================
// 12.17 msiptw_req_thread - MSIPTW Request Thread (async DDR send)
// Calculates MSI PTE address, sends DDR read request
// Corresponds to iommu_msi_trans.cc step6-7
// ============================================================
void iommu_top::msiptw_req_thread() {
    while (true) {
        // Flow control
        while (msiptw_outstanding_task_count >= MSIPTW_MAX_OUTSTANDING_TASKS) {
            wait(msiptw_task_completed_event);
        }

        iommu_task_t* task = msipt_cache_to_msiptw_fifo.read();
        task->state = TASK_MSIPTW_REQ;
        msiptw_outstanding_task_count++;

        wait(MSIPTW_COMPUTE_DELAY, SC_NS);

        // ========== MSI PTE address calculation ==========
        // msi_index已在msi_id_check中按 I = extract(A>>12, mask&mgpaw_mask) 计算
        // msipte_addr = (DC.msiptp.PPN << 12) | (I << 4), 每个MSI PTE 16字节
        uint64_t m = task->DC.msiptp.PPN * PAGESIZE;
        uint64_t msipte_addr = m | (task->walk_ctx.msi_index * 16);

        // step7 PMA检查: 超出物理地址空间 -> "MSI PTE load access fault" (cause=261)
        uint64_t pas = iommu_inst.reg_file.capabilities.pas;
        uint64_t pa_mask = (pas < 64) ? ((1ULL << pas) - 1) : ~0ULL;
        if (msipte_addr & ~pa_mask) {
            printf("[MSIPTW_REQ] task_id=%u, msipte_addr=0x%lx out of PA range -> fault 261\n",
                   task->task_id, msipte_addr);
            fflush(stdout);
            task->cause = 261;
            task->state = TASK_FAULT;
            msiptw_outstanding_task_count--;
            msiptw_task_completed_event.notify(SC_ZERO_TIME);
            msiptw_to_msipt_cache_fifo.write(task);
            continue;
        }

        // Save walk context
        task->walk_ctx.walk_phase = MSIPTW_READ_MSIPTE;
        task->walk_ctx.read_addr = msipte_addr;
        task->walk_ctx.read_size = 16;  // MSI PTE = 128 bits = 16 bytes

        // Register in active_walks
        msiptw_walks_mtx.lock();
        msiptw_active_walks[task->task_id] = task;
        msiptw_walks_mtx.unlock();

        // Send DDR request
        ddr_req_entry_t req;
        req.task_id = task->task_id;
        req.addr = msipte_addr;
        req.size = 16;
        req.is_write = false;
        req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;  // [STAT]
        task->walk_ctx.ddr_read_count = 1;
        printf("[MSIPTW_REQ] task_id=%u, msipte_addr=0x%lx, read_count=1 -> ddr_req\n",
               task->task_id, msipte_addr);
        fflush(stdout);
        msiptw_req_ddr_fifo.write(req);
    }
}

// ============================================================
// 12.18 msiptw_rsp_thread - MSIPTW Response Thread (MSI PTE parse)
// Parses 16-byte MSI PTE, determines translation or fault
// Corresponds to iommu_msi_trans.cc step8-15
// ============================================================
void iommu_top::msiptw_rsp_thread() {
    while (true) {
        ddr_rsp_entry_t rsp = msiptw_rsp_ddr_fifo.read();
        wait(MSIPTW_PARSE_DELAY, SC_NS);

        // Find corresponding task
        msiptw_walks_mtx.lock();
        auto it = msiptw_active_walks.find(rsp.task_id);
        if (it == msiptw_active_walks.end()) {
            msiptw_walks_mtx.unlock();
            printf("[MSIPTW] ERROR: task_id %u not found in active_walks\n", rsp.task_id);
            continue;
        }
        iommu_task_t* task = it->second;
        msiptw_walks_mtx.unlock();

        memcpy(task->walk_ctx.read_buf, rsp.data, rsp.data_length);

        // ========== Parse 16-byte MSI PTE ==========
        msipte_t msipte;
        msipte.raw[0] = 0;
        msipte.raw[1] = 0;
        memcpy(&msipte.raw[0], task->walk_ctx.read_buf, 16);

        bool walk_fault = msipte_decode(task, msipte);

        // [MSI] Flat模式翻译结果回填MSIPT Cache; MRIF模式按规范不缓存
        // (功能模型 iommu_translate.cc L462-472: MRIF动作与普通翻译差异大且频率低)
        if (!walk_fault && task->is_mrif == 0) {
            iommu::CacheMessage upd;
            upd.msg_type  = iommu::CacheMsgType::MSI_UPDATE;
            upd.task_id   = task->task_id;
            upd.device_id = task->device_id;
            upd.msi_index = (uint32_t)task->walk_ctx.msi_index;
            upd.msi_data.valid     = true;
            upd.msi_data.pte       = msipte.raw[0];
            upd.msi_data.pte_hi    = msipte.raw[1];
            upd.msi_data.spa       = task->pa;
            upd.msi_data.mrif_mode = false;
            cache_sub.msi_update_fifo.write(upd);
            printf("[MSIPTW_RSP] task_id=%u -> MSIPT Cache FILL (msi_index=%llu, spa=0x%lx)\n",
                   task->task_id, (unsigned long long)task->walk_ctx.msi_index, task->pa);
            fflush(stdout);
        }

        if (walk_fault) {
            task->state = TASK_FAULT;
            printf("[MSIPTW_RSP] task_id=%u -> WALK FAULT, cause=%d\n", task->task_id, task->cause);
            fflush(stdout);
        } else {
            task->state = TASK_MSIPTW_DONE;
            printf("[MSIPTW_RSP] task_id=%u -> WALK COMPLETE, pa=0x%lx, is_mrif=%d -> msipt_cache\n",
                   task->task_id, task->pa, task->is_mrif);
            fflush(stdout);
        }

        // Cleanup active_walks and release outstanding count
        msiptw_walks_mtx.lock();
        msiptw_active_walks.erase(rsp.task_id);
        msiptw_walks_mtx.unlock();
        msiptw_outstanding_task_count--;
        msiptw_task_completed_event.notify(SC_ZERO_TIME);

        // Send to MSIPT Cache result thread
        msiptw_to_msipt_cache_fifo.write(task);
    }
}
