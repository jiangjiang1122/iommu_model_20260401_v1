// IOMMU Performance Model - PTW Threads (2 threads)
// SPEC Section 12.13-12.14
// Corresponds to two_stage_address_translation() + second_stage_address_translation()

#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>
#include <vector>

// [STAT] Helper function to print PTW task DDR access summary
static void print_ptw_task_summary(iommu_task_t* task, const char* completion_type) {
    const char* type_names[] = {
        "VS_non_leaf", "VS_leaf", "VS_leaf+burst", "GS_implicit",
        "GS_explicit", "AD_update", "Bare_explicit", "Burst_wait"
    };
    double ts_ns = sc_core::sc_time_stamp().to_seconds() * 1e9;
    double entry_ns = task->timestamp.to_seconds() * 1e9;
    double e2e_ns = ts_ns - entry_ns;
    printf("[PTW_STAT] task_id=%u, %s: Walker_HIT=%u(hit_level=%u), DDR_reads=%u, details=[",
           task->task_id, completion_type,
           (task->walk_ctx.walker_hit_level > 0) ? 1 : 0,
           task->walk_ctx.walker_hit_level,
           task->walk_ctx.ddr_read_count);
    for (int i = 0; i < task->walk_ctx.ddr_log_count && i < 8; i++) {
        if (i > 0) printf(", ");
        printf("#%u:%s@L%u(0x%lx,%uB)",
               i, type_names[task->walk_ctx.ddr_log_type[i]],
               task->walk_ctx.ddr_log_level[i],
               task->walk_ctx.ddr_log_addr[i],
               task->walk_ctx.ddr_log_size[i]);
    }
    printf("] [entry=%.1f ns, ptw_done=%.1f ns, e2e=%.1f ns]\n",
           entry_ns, ts_ns, e2e_ns);
    fflush(stdout);
}

// ============================================================
// 12.13 ptw_req_thread - PTW Request Dispatch (PEQ-based pipeline)
// Reads from FIFO, applies outstanding control, schedules via PEQ
// ============================================================
void iommu_top::ptw_req_thread() {
    while (true) {
        // Flow control: limit total PTW outstanding tasks
        while (ptw_outstanding_task_count >= PTW_MAX_OUTSTANDING_TASKS) {
            printf("[PTW_REQ] BLOCKED: outstanding=%d >= max=%d, waiting...\n",
                   ptw_outstanding_task_count, (int)PTW_MAX_OUTSTANDING_TASKS);
            fflush(stdout);
            // [DIAG] 打印当前所有group状态
            prefetch_group_mtx.lock();
            printf("[PTW_REQ][DIAG] prefetch_groups count=%zu:\n", prefetch_groups.size());
            for (auto& [gid, g] : prefetch_groups) {
                printf("  group %u: completed=%d, pending=%u, main_done=%d, total=%u\n",
                       gid, g.completed, g.pending_tasks, g.main_task_done, g.total_tasks);
            }
            prefetch_group_mtx.unlock();
            wait(ptw_task_completed_event);
            printf("[PTW_REQ] WAKE: outstanding=%d\n", ptw_outstanding_task_count);
            fflush(stdout);
        }

        iommu_task_t* task = pt_cache_to_ptw_fifo.read();
        // [STAT] 记录PTW任务入口时刻
        const double current_ns = sc_time_stamp().to_seconds() * 1e9;
        ptw_task_start_ns[task->task_id] = current_ns;
        
        // [STAT] 记录任务注入间隔
        if (ptw_last_inject_ns > 0.0) {
            double inject_interval = current_ns - ptw_last_inject_ns;
            ptw_inject_interval_total_ns += inject_interval;
            if (inject_interval > ptw_inject_interval_max_ns)
                ptw_inject_interval_max_ns = inject_interval;
            if (inject_interval < ptw_inject_interval_min_ns)
                ptw_inject_interval_min_ns = inject_interval;
            ptw_inject_interval_count++;
        }
        ptw_last_inject_ns = current_ns;
        
        task->state = TASK_PTW_REQ;
        ptw_outstanding_task_count++;
        if (ptw_outstanding_task_count > peak_ptw_outstanding) {
            peak_ptw_outstanding = ptw_outstanding_task_count;
            printf("[PTW_STAT] New peak outstanding=%d at task_id=%u, time=%s\n",
                   peak_ptw_outstanding, task->task_id, sc_time_stamp().to_string().c_str());
            fflush(stdout);
        }

        printf("[PTW_REQ] task_id=%u, iova=0x%lx, iosatp.MODE=%d, GV=%d -> dispatch to PEQ [t=%.1f ns]\n",
               task->task_id, task->iova, task->iosatp.MODE, task->GV,
               sc_time_stamp().to_seconds() * 1e9);
        fflush(stdout);

        // Pipeline delay via PEQ (non-blocking, parallel timing)
        ptw_req_peq.notify(*task, sc_time(PTW_REQ_PIPELINE_DELAY_NS, SC_NS));
    }
}

// ============================================================
// 12.13b ptw_req_process_thread - PTW Request Processing
// Fires after PEQ delay, performs walk init and sends DDR request
// ============================================================
void iommu_top::ptw_req_process_thread() {
    while (true) {
        iommu_task_t* task = ptw_req_peq.get_next_transaction();
        if (!task) { wait(ptw_req_peq.get_event()); continue; }

        // ========== 1. Bare mode fast path ==========
        // Corresponds to iommu_two_stage_trans.cc L39-82
        if (task->iosatp.MODE == IOSATP_Bare) {
            printf("[PTW_REQ] task_id=%u -> Bare mode fast path, no page table reads\n", task->task_id);
            fflush(stdout);
            task->vs_pte.raw = 0;
            task->vs_pte.D = task->vs_pte.A = task->vs_pte.G = task->vs_pte.U = 1;
            task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = task->vs_pte.V = 1;
            task->vs_pte.PBMT = PMA;
            task->gpa = task->iova;
            task->page_sz = get_bare_page_size(&iommu_inst);

            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                // Need G-stage explicit translation
                init_gstage_walk(task, task->gpa);
                task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;

                // [S2] S2 Walker Cache lookup BEFORE any DDR request (Bare mode path)
                bool s2_cache_hit = false;
                if (PTW_WALKER_S2_CACHE_ENABLED && PTW_WALKER_CACHE_ENABLED) {
                    iommu::CacheMessage s2_req = task_to_s2_walker_request(task, task->gpa);
                    cache_sub.walker_request_fifo.write(s2_req);
                    iommu::CacheMessage s2_resp = cache_sub.walker_response_fifo.read();

                    if (s2_resp.hit) {
                        s2_cache_hit = true;
                        task->walk_ctx.s2_walker_hit_level = s2_resp.walker_level;
                        uint8_t GS_LEVELS = 0;
                        uint16_t gs_vpn[5] = {0};
                        extract_gs_vpn(task->gpa, task->iohgatp.MODE, gs_vpn, &GS_LEVELS);
                        for (int k = 0; k < 5; k++) task->walk_ctx.gs_vpn[k] = gs_vpn[k];
                        uint8_t gs_start_level = GS_LEVELS - 1 - s2_resp.walker_level;
                        task->walk_ctx.gs_level = gs_start_level;
                        task->walk_ctx.gs_base_addr = s2_resp.walker_data.next_ppn * PAGESIZE;
                        uint64_t gs_pte_addr = task->walk_ctx.gs_base_addr +
                            task->walk_ctx.gs_vpn[gs_start_level] * 8;
                        task->walk_ctx.read_addr = gs_pte_addr;
                        task->walk_ctx.read_size = 8;
                        printf("[PTW_REQ] task_id=%u, Bare S2 Cache HIT level=%d -> GS_EXPLICIT from level=%d, addr=0x%lx\n",
                               task->task_id, s2_resp.walker_level, gs_start_level, gs_pte_addr);
                        fflush(stdout);
                    } else {
                        task->walk_ctx.s2_walker_hit_level = 0;
                        printf("[PTW_REQ] task_id=%u, Bare S2 Cache MISS -> normal GS_EXPLICIT\n", task->task_id);
                        fflush(stdout);
                    }
                }

                task->walk_ctx.ddr_read_count = 1;
                // [STAT] DDR access log
                if (task->walk_ctx.ddr_log_count < 8) {
                    task->walk_ctx.ddr_log_type[task->walk_ctx.ddr_log_count] = 6; // GS_EXPLICIT
                    task->walk_ctx.ddr_log_level[task->walk_ctx.ddr_log_count] = 0;
                    task->walk_ctx.ddr_log_addr[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_addr;
                    task->walk_ctx.ddr_log_size[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_size;
                    task->walk_ctx.ddr_log_count++;
                }

                ptw_walks_mtx.lock();
                ptw_active_walks[task->task_id] = task;
                ptw_walks_mtx.unlock();

                ddr_req_entry_t req;
                req.task_id = task->task_id;
                req.addr = task->walk_ctx.read_addr;
                req.size = task->walk_ctx.read_size;
                req.is_write = false;
                req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;  // [STAT] 记录DDR请求提交时间
                printf("[PTW_REQ] task_id=%u, Bare mode -> GS_EXPLICIT%s, addr=0x%lx, read_count=1 -> ddr_req\n",
                       task->task_id, s2_cache_hit ? " (S2 HIT)" : "", req.addr);
                fflush(stdout);
                ptw_req_ddr_fifo.write(req);
                continue;
            } else {
                // G-stage also Bare: complete immediately
                task->pa = task->gpa;
                task->gst_page_sz = get_gstage_bare_page_size(&iommu_inst);
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.X = task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.PBMT = PMA;
                task->state = TASK_PTW_DONE;
                ptw_outstanding_task_count--;
                assert(ptw_outstanding_task_count >= 0);  // [FIX-第10条] 防止下溢
                ptw_total_completed++;  // [STAT]
                // [STAT] 累加PTW完成统计（Bare直通路径）
                ptw_total_ddr_reads += task->walk_ctx.ddr_read_count;
                ptw_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                ptw_main_task_count++;
                ptw_main_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                ptw_main_total_ddr_reads += task->walk_ctx.ddr_read_count;
                { auto _s = ptw_task_start_ns.find(task->task_id);
                  if (_s != ptw_task_start_ns.end()) {
                      double lat = sc_time_stamp().to_seconds()*1e9 - _s->second;
                      ptw_total_exec_ns += lat;
                      ptw_task_latency_values.push_back(lat);
                      ptw_task_start_ns.erase(_s); } }
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                
                // Convert task to CacheMessage and write to cache_sub.pt_update_fifo
                iommu::CacheMessage pt_update_req = task_to_pt_update(task);
                pt_update_req.timestamp = sc_time_stamp();  // [STAT] 记录FIFO写入时刻
                cache_sub.pt_update_fifo.write(pt_update_req);
                
                // Also route to forwarder
                // TODO: 暂时禁用forwarded_task_ids，待修复崩溃问题
                pt_cache_to_fwd_fifo.write(task);
                // VA Dedup: 恢复挂起的同页任务
                va_dedup_recover(task, false);
                continue;
            }
        }

        // ========== 2. VS-stage Walk Initialization ==========
        {
            // =====================================================================
            // Walker Cache Lookup (如果启用)
            // =====================================================================
            bool walker_cache_enabled = PTW_WALKER_CACHE_ENABLED;  // 从PTW模块参数读取
            bool walker_hit = false;
            
            if (walker_cache_enabled) {
                // 使用转换函数构造Walker Cache Lookup请求
                iommu::CacheMessage req = task_to_walker_request(task);
                
                // 发送Lookup请求
                cache_sub.walker_request_fifo.write(req);
                
                // 阻塞等待响应
                iommu::CacheMessage resp = cache_sub.walker_response_fifo.read();
                
                // 使用转换函数将响应写回task
                walker_response_to_task(resp, task);
                
                walker_hit = resp.hit;
                
                // 保存lookup命中层级，用于update时避免冗余更新
                if (walker_hit) {
                    task->walk_ctx.walker_hit_level = resp.walker_level;
                } else {
                    task->walk_ctx.walker_hit_level = 0;  // 全部miss
                }
            }
            
            uint16_t vpn[5] = {0};
            uint8_t LEVELS = 0, PTESIZE = 0;
            extract_vpn(task->iova, task->iosatp.MODE, task->SXL,
                        vpn, &LEVELS, &PTESIZE);

            // Canonical address check
            if (!check_canonical(task->iova, task->iosatp.MODE, task->SXL)) {
                printf("[PTW_REQ] task_id=%u -> FAULT: canonical check failed\n", task->task_id);
                fflush(stdout);
                task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                task->state = TASK_FAULT;
                ptw_outstanding_task_count--;
                assert(ptw_outstanding_task_count >= 0);  // [FIX-第10条] 防止下溢
                ptw_total_completed++;  // [STAT]
                // [STAT] 累加PTW完成统计（canonical fault）
                ptw_total_ddr_reads += task->walk_ctx.ddr_read_count;
                ptw_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                ptw_main_task_count++;
                ptw_main_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                ptw_main_total_ddr_reads += task->walk_ctx.ddr_read_count;
                { auto _s = ptw_task_start_ns.find(task->task_id);
                  if (_s != ptw_task_start_ns.end()) {
                      double lat = sc_time_stamp().to_seconds()*1e9 - _s->second;
                      ptw_total_exec_ns += lat;
                      ptw_task_latency_values.push_back(lat);
                      ptw_task_start_ns.erase(_s); } }
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                
                // Convert task to CacheMessage and write to cache_sub.pt_update_fifo (for fault recording)
                iommu::CacheMessage pt_update_req = task_to_pt_update(task);
                pt_update_req.timestamp = sc_time_stamp();  // [STAT] 记录FIFO写入时刻
                cache_sub.pt_update_fifo.write(pt_update_req);
                
                // Route to fault FIFO
                collector_to_fault_fifo.write(task);
                // VA Dedup: 恢复挂起的同页任务(fault)
                va_dedup_recover(task, true);
                continue;
            }

            int8_t i = LEVELS - 1;
            uint64_t a = task->iosatp.PPN * PAGESIZE;

            // Save VPN to walk context
            for (int k = 0; k < 5; k++) task->walk_ctx.vpn[k] = vpn[k];
            task->walk_ctx.max_levels = LEVELS;
            task->walk_ctx.ptesize = PTESIZE;

            // =====================================================================
            // 根据 Walker Cache 命中情况初始化 walk
            // =====================================================================
            if (walker_cache_enabled && walker_hit) {
                // 命中：walker_response_to_task已经设置了walk starting point
                // task->walk_ctx.level 和 task->walk_ctx.base_addr 已经被设置
                
                printf("[PTW_REQ] task_id=%u, Walker hit: start from level=%d, base_addr=0x%lx\n",
                       task->task_id, task->walk_ctx.level, task->walk_ctx.base_addr);
                fflush(stdout);
            } else {
                // 未命中：从最高级开始完整 walk
                task->walk_ctx.level = i;
                task->walk_ctx.base_addr = a;
            }

            // Calculate first PTE address
            uint64_t pte_addr = task->walk_ctx.base_addr + 
                               task->walk_ctx.vpn[task->walk_ctx.level] * PTESIZE;

            // Check if G-stage implicit translation needed
            // 两阶段 walker cache hit 时，base_addr 已是 SPA（来自 walker cache 的 next_ppn），
            // pte_addr 也是 SPA，不需要 GS_IMPLICIT 翻译，直接读取
            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare && !(walker_cache_enabled && walker_hit)) {
                task->walk_ctx.pending_vs_pte_addr = pte_addr;
                task->walk_ctx.vs_level = task->walk_ctx.level;
                init_gstage_walk(task, pte_addr);
                task->walk_ctx.walk_phase = PTW_GS_IMPLICIT;
            } else {
                task->walk_ctx.read_addr = pte_addr;
                task->walk_ctx.read_size = PTESIZE;
                task->walk_ctx.walk_phase = PTW_VS_WALK;
                
                // [OPT] Walker Cache HIT at level=0 + 预取启用: 合并叶子PTE+预取PTE为1次DDR读
                if (walker_cache_enabled && walker_hit &&
                    task->walk_ctx.level == 0 &&
                    task->walk_ctx.prefetch_enabled && task->walk_ctx.prefetch_depth > 0) {
                    uint32_t ptesz = PTESIZE;
                    uint32_t D = task->walk_ctx.prefetch_depth;
                    
                    // 边界检查: 不跨越4KB页表页
                    uint64_t start = pte_addr;
                    uint64_t end = start + (1 + D) * ptesz - 1;
                    if ((start & ~PT_PAGE_MASK) != (end & ~PT_PAGE_MASK)) {
                        uint64_t page_end = (start | PT_PAGE_MASK) + 1;
                        D = (page_end - start) / ptesz - 1;
                        task->walk_ctx.prefetch_depth = D;
                    }
                    
                    if (D > 0) {
                        task->walk_ctx.read_size = (1 + D) * ptesz;
                        task->walk_ctx.combined_burst_ready = true;
                        printf("[PTW_REQ] task_id=%u -> Walker HIT combined leaf+burst read, size=%u (1+D=%u PTEs)\n",
                               task->task_id, task->walk_ctx.read_size, 1 + D);
                        fflush(stdout);

                        // [两阶段预取-early spawn] Walker cache hit@L3 + 两阶段翻译:
                        // 立即spawn预取任务，使其与主任务的5次DDR访问全部并发
                        // 此时所有信息已就绪: vs_l0_spa_ppn(来自walker cache hit), vpn[0](来自IOVA)
                        if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                            uint32_t group_id = task->task_id;
                            task->walk_ctx.is_two_stage_prefetch = true;
                            task->walk_ctx.prefetch_group_id = group_id;
                            task->walk_ctx.prefetch_total = 1 + D;
                            task->walk_ctx.prefetch_idx = 0;

                            // 预初始化预取组(供monitor线程读取)
                            // pending_tasks = D: 仅D个预取任务各递减一次
                            // 主任务walk_complete时跳过此块(由is_prefetch_task条件控制)
                            prefetch_group_mtx.lock();
                            auto& group = prefetch_groups[group_id];
                            group.main_task = task;
                            group.pending_tasks = D;
                            group.total_tasks = 1 + D;
                            group.completed = false;
                            group.group_iovas[0] = task->iova & ~PAGE_MASK_4KB;
                            // [STAT] 记录组起始时间(使用主任务进入PTW的时刻)
                            { auto _s = ptw_task_start_ns.find(task->task_id);
                              if (_s != ptw_task_start_ns.end()) {
                                  group.group_start_ns = _s->second;
                              } else {
                                  group.group_start_ns = sc_time_stamp().to_seconds() * 1e9;
                              } }
                            prefetch_group_mtx.unlock();

                            uint16_t main_vpn0 = task->walk_ctx.vpn[0];
                            uint64_t vs_l0_spa_ppn = task->walk_ctx.vs_l0_spa_ppn;

                            printf("[PTW_REQ_EARLY_PF] task_id=%u -> Early spawn %u prefetch tasks, vs_l0_spa_ppn=0x%lx, main_vpn0=%u\n",
                                   task->task_id, D, vs_l0_spa_ppn, main_vpn0);
                            fflush(stdout);

                            for (uint32_t d = 0; d < D; d++) {
                                iommu_task_t* pf_task = new iommu_task_t();
                                task_id_mtx.lock();
                                pf_task->task_id = next_task_id++;
                                task_id_mtx.unlock();

                                // 复制主任务上下文
                                pf_task->iosatp = task->iosatp;
                                pf_task->iohgatp = task->iohgatp;
                                pf_task->DC = task->DC;
                                pf_task->GV = task->GV;
                                pf_task->GADE = task->GADE;
                                pf_task->GSCID = task->GSCID;
                                pf_task->PSCID = task->PSCID;
                                pf_task->device_id = task->device_id;

                                // 预取专用标志
                                pf_task->walk_ctx.is_prefetch_task = true;
                                pf_task->walk_ctx.prefetch_group_id = group_id;
                                pf_task->walk_ctx.prefetch_idx = d + 1;
                                pf_task->walk_ctx.prefetch_enabled = false;

                                // 计算预取IOVA和VS L0 PTE SPA地址
                                uint64_t pf_iova = (task->iova & ~PAGE_MASK_4KB) + (d + 1) * PAGE_SIZE_4KB;
                                pf_task->iova = pf_iova;
                                pf_task->timestamp = task->timestamp;
                                pf_task->state = TASK_PTW_REQ;
                                pf_task->page_sz = PAGE_SIZE_4KB;

                                uint16_t pf_vpn0 = main_vpn0 + d + 1;
                                uint64_t vs_l0_pte_spa = (vs_l0_spa_ppn * PAGESIZE) + pf_vpn0 * ptesz;

                                // 保存预取IOVA到组(供monitor线程使用)
                                group.group_iovas[d + 1] = pf_iova;
                                task->walk_ctx.prefetch_group.group_iovas[d + 1] = pf_iova;

                                // 设置walk context: 直接读VS L0 PTE SPA，跳过GS_IMPLICIT
                                pf_task->walk_ctx.read_addr = vs_l0_pte_spa;
                                pf_task->walk_ctx.read_size = ptesz;
                                pf_task->walk_ctx.walk_phase = PTW_VS_PTE_READ;
                                pf_task->walk_ctx.ddr_read_count = 1;
                                pf_task->walk_ctx.ptesize = ptesz;
                                for (int k = 0; k < 5; k++)
                                    pf_task->walk_ctx.vpn[k] = task->walk_ctx.vpn[k];
                                pf_task->walk_ctx.vpn[0] = pf_vpn0;

                                // 注册到active_walks
                                ptw_walks_mtx.lock();
                                ptw_active_walks[pf_task->task_id] = pf_task;
                                ptw_walks_mtx.unlock();

                                // 立即发送DDR请求(与主任务DDR#1并发)
                                ddr_req_entry_t pf_req;
                                pf_req.task_id = pf_task->task_id;
                                pf_req.addr = vs_l0_pte_spa;
                                pf_req.size = ptesz;
                                pf_req.is_write = false;
                                pf_req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;
                                ptw_req_ddr_fifo.write(pf_req);

                                printf("[PTW_REQ_EARLY_PF] Spawned prefetch task_id=%u, idx=%u, vs_l0_pte_spa=0x%lx, pf_vpn0=%u, pf_iova=0x%lx\n",
                                       pf_task->task_id, d + 1, vs_l0_pte_spa, pf_vpn0, pf_iova);
                                fflush(stdout);
                                // [STAT] 记录预取任务起始时刻(用于延时计算)
                                ptw_task_start_ns[pf_task->task_id] = sc_time_stamp().to_seconds() * 1e9;
                            }
                        }
                    }
                }
            }

            // Register in active_walks and send DDR request
            ptw_walks_mtx.lock();
            ptw_active_walks[task->task_id] = task;
            ptw_walks_mtx.unlock();

            ddr_req_entry_t req;
            req.task_id = task->task_id;
            req.addr = task->walk_ctx.read_addr;
            req.size = task->walk_ctx.read_size;
            req.is_write = false;
            req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;  // [STAT]
            task->walk_ctx.ddr_read_count = 1;
            // [STAT] DDR access log - initial VS_WALK read
            if (task->walk_ctx.ddr_log_count < 8) {
                uint8_t log_type = (task->walk_ctx.walk_phase == PTW_VS_WALK) ? 
                    ((task->walk_ctx.level == 0 && task->walk_ctx.prefetch_enabled && task->walk_ctx.prefetch_depth > 0) ? 2 : 0) : 3;
                task->walk_ctx.ddr_log_type[task->walk_ctx.ddr_log_count] = log_type;
                task->walk_ctx.ddr_log_level[task->walk_ctx.ddr_log_count] = task->walk_ctx.level;
                task->walk_ctx.ddr_log_addr[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_addr;
                task->walk_ctx.ddr_log_size[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_size;
                task->walk_ctx.ddr_log_count++;
            }
            printf("[PTW_REQ] task_id=%u, walk_phase=%d, addr=0x%lx, size=%d, read_count=1 -> ddr_req\n",
                   task->task_id, task->walk_ctx.walk_phase, req.addr, req.size);
            fflush(stdout);
            ptw_req_ddr_fifo.write(req);
        }
    }
}

// ============================================================
// 12.14 ptw_rsp_thread - PTW Response Dispatch (PEQ-based pipeline)
// Reads DDR responses, schedules via PEQ with pipeline delay
// ============================================================
void iommu_top::ptw_rsp_thread() {
    while (true) {
        ddr_rsp_entry_t rsp = ptw_rsp_ddr_fifo.read();
        // Heap-allocate to ensure lifetime across PEQ delay
        ddr_rsp_entry_t* rsp_ptr = new ddr_rsp_entry_t(rsp);
        ptw_rsp_peq.notify(*rsp_ptr, sc_time(PTW_RSP_PIPELINE_DELAY_NS, SC_NS));
    }
}

// ============================================================
// 12.14b ptw_rsp_process_thread - PTW Response Processing
// Fires after PEQ delay, processes DDR responses for VS/GS walk
// ============================================================
void iommu_top::ptw_rsp_process_thread() {
    while (true) {
        ddr_rsp_entry_t* rsp_ptr = ptw_rsp_peq.get_next_transaction();
        if (!rsp_ptr) { wait(ptw_rsp_peq.get_event()); continue; }
        ddr_rsp_entry_t rsp = *rsp_ptr;
        delete rsp_ptr;

        printf("[PTW_RSP] task_id=%u, data_len=%d, error=%d -> processing\n",
               rsp.task_id, rsp.data_length, rsp.error);
        fflush(stdout);

        // [FIX] 防御性检查: task_id=0不应存在(next_task_id从1开始)
        if (rsp.task_id == 0) {
            printf("[PTW_RSP] ERROR: task_id=0 in DDR response! Skipping (memory corruption detected).\n");
            fflush(stdout);
            continue;
        }

        // Find corresponding task
        ptw_walks_mtx.lock();
        auto it = ptw_active_walks.find(rsp.task_id);
        if (it == ptw_active_walks.end()) {
            ptw_walks_mtx.unlock();
            printf("[PTW] ERROR: task_id %u not found in active_walks\n", rsp.task_id);
            continue;
        }
        iommu_task_t* task = it->second;
        // [FIX] 验证task指针有效性
        if (task == nullptr || task->task_id != rsp.task_id) {
            ptw_walks_mtx.unlock();
            printf("[PTW] ERROR: task_id %u stale/corrupt pointer (task=%p, task->task_id=%u)! Skipping.\n",
                   rsp.task_id, (void*)task, task ? task->task_id : 0);
            fflush(stdout);
            ptw_active_walks.erase(rsp.task_id);  // 清除无效条目
            continue;
        }
        ptw_walks_mtx.unlock();

        memcpy(task->walk_ctx.read_buf, rsp.data, rsp.data_length);

        // [STAT] 计算单次DDR访问延时
        double ddr_latency_ns = sc_time_stamp().to_seconds() * 1e9 - rsp.submit_time_ns;
        if (ddr_latency_ns > ptw_max_ddr_latency_ns)
            ptw_max_ddr_latency_ns = ddr_latency_ns;
        if (ddr_latency_ns < ptw_min_ddr_latency_ns)
            ptw_min_ddr_latency_ns = ddr_latency_ns;
        ptw_total_ddr_latency_ns += ddr_latency_ns;
        ptw_ddr_latency_count++;

        bool walk_complete = false;
        bool walk_fault = false;
        bool need_next_ddr = false;

        printf("[PTW_RSP] task_id=%u, walk_phase=%d (VS_WALK=0, GS_IMPLICIT=1, GS_EXPLICIT=2, AD_UPDATE=3), read_count=%u\n",
               task->task_id, task->walk_ctx.walk_phase, task->walk_ctx.ddr_read_count);
        fflush(stdout);

        switch (task->walk_ctx.walk_phase) {

        case PTW_PREFETCH_WAIT: {
            // =====================================================================
            // Burst预取DDR响应处理 (v2.0)
            // 接收Burst DDR响应,解析D个PTE,构造pt_updates
            // =====================================================================
            printf("[PTW_PREFETCH] task_id=%u -> Burst DDR response received\n",
                   task->task_id);
            fflush(stdout);
            
            // 从Burst响应数据中解析D个PTE
            uint32_t prefetch_depth = task->walk_ctx.prefetch_depth;
            uint32_t ptesize = task->walk_ctx.leaf_ptesize;
            uint64_t base_iova = task->iova & ~PAGE_MASK_4KB;  // 4KB页对齐
            
            // 解析Burst数据 (每个PTE=8字节)
            for (uint32_t d = 0; d < prefetch_depth; d++) {
                // 从read_buf中提取第d个PTE
                uint64_t pte_raw = 0;
                memcpy(&pte_raw, &task->walk_ctx.read_buf[d * ptesize], ptesize);
                
                spte_t vs_pte;
                vs_pte.raw = pte_raw;
                
                gpte_t g_pte;
                g_pte.raw = pte_raw;  // G-stage PTE与VS-stage相同
                
                // 计算预取IOVA对应的PA
                uint64_t prefetch_ppn = vs_pte.PPN;
                uint64_t prefetch_iova = base_iova + (d + 1) * PAGE_SIZE_4KB;  // 4KB步长
                uint64_t prefetch_pa = (prefetch_ppn * PAGESIZE) | (prefetch_iova & PAGE_MASK_4KB);
                
                // 保存到pt_updates[d+1]
                task->walk_ctx.pt_updates[d + 1].vs_pte = vs_pte;
                task->walk_ctx.pt_updates[d + 1].g_pte = g_pte;
                task->walk_ctx.pt_updates[d + 1].pa = prefetch_pa;
                task->walk_ctx.pt_updates[d + 1].iova = prefetch_iova;
                task->walk_ctx.pt_updates[d + 1].page_sz = PAGE_SIZE_4KB;
                
                printf("[PTW_PREFETCH] PTE[%u]: iova=0x%lx, PPN=0x%lx, pa=0x%lx\n",
                       d+1, prefetch_iova, prefetch_ppn, prefetch_pa);
            }
            
            fflush(stdout);
            
            // 标记Burst预取完成
            task->walk_ctx.prefetch_burst_pending = false;
            
            // [FIX] 减少pending_tasks并通知monitor
            prefetch_group_mtx.lock();
            auto& group = prefetch_groups[task->task_id];
            group.pending_tasks--;
            
            printf("[PTW_PREFETCH] task_id=%u -> Burst complete, pending_tasks=%u\n",
                   task->task_id, group.pending_tasks);
            fflush(stdout);
            
            if (group.pending_tasks == 0 && group.completed) {
                printf("[PTW_PREFETCH] task_id=%u -> Notifying monitor (all pending done)\n",
                       task->task_id);
                fflush(stdout);
                prefetch_group_completed_event.notify(SC_ZERO_TIME);
            }
            prefetch_group_mtx.unlock();
            
            break;
        }

        case PTW_VS_WALK: {
            // ========== VS-stage PTE parsing ==========
            // Corresponds to iommu_two_stage_trans.cc L163-471
            spte_t pte;
            pte.raw = 0;
            memcpy(&pte.raw, task->walk_ctx.read_buf, task->walk_ctx.ptesize);

            // Step 3: PTE validity check
            if ((pte.V == 0) || (pte.R == 0 && pte.W == 1) ||
                (pte.PBMT == 3) || (pte.reserved != 0)) {
                // Debug: dump read_buf for diagnosis
                uint64_t buf_val = 0;
                memcpy(&buf_val, task->walk_ctx.read_buf, 8);
                printf("[PTW_RSP] task_id=%u, VS_WALK INVALID PTE: raw=0x%lx, buf_val=0x%lx, V=%d, R=%d, W=%d, PBMT=%d, reserved=%d, level=%d, base_addr=0x%lx, pte_addr=0x%lx, iova=0x%lx, vpn0=%d\n",
                       task->task_id, (uint64_t)pte.raw, buf_val, pte.V, pte.R, pte.W, pte.PBMT, pte.reserved,
                       task->walk_ctx.level, task->walk_ctx.base_addr,
                       task->walk_ctx.base_addr + task->walk_ctx.vpn[task->walk_ctx.level] * task->walk_ctx.ptesize,
                       task->iova, task->walk_ctx.vpn[0]);
                fflush(stdout);
                task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                walk_fault = true;
                break;
            }

            // NAPOT check: non-leaf N bit must be 0
            if (task->walk_ctx.level != 0 && pte.N) {
                task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                walk_fault = true;
                break;
            }

            // Step 4: Leaf/non-leaf determination
            if (pte.R == 1 || pte.X == 1) {
                // ===== Leaf node (steps 5-7) =====
                // Permission checks
                if (task->check_access_perms) {
                    if ((task->is_exec && pte.X == 0) ||
                        (task->is_read && pte.R == 0) ||
                        (task->is_write && pte.W == 0)) {
                        task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                        walk_fault = true;
                        break;
                    }
                }
                if (task->priv == U_MODE && pte.U == 0) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }
                if (task->is_exec && task->priv == S_MODE && pte.U == 1) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }
                if (task->priv == S_MODE && !task->is_exec &&
                    task->SUM == 0 && pte.U == 1) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }

                // Superpage size calculation
                task->page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.level; j++) {
                    task->page_sz *= (task->walk_ctx.ptesize == 8) ? 512 : 1024;
                }

                // Step 6: Misaligned superpage check
                // If i > 0 and pte.ppn[i-1:0] != 0, this is a misaligned superpage
                if (task->walk_ctx.level > 0) {
                    uint8_t misaligned = 0;
                    if (task->iosatp.MODE == IOSATP_Sv32 && task->SXL == 1) {
                        // Sv32: PPN[0] is 10 bits
                        if ((pte.PPN & 0x3FF) != 0) misaligned = 1;
                    } else {
                        // Sv39/Sv48/Sv57: PPN subfields are 9 bits each
                        if ((pte.PPN & 0x1FF) != 0) misaligned = 1;
                        if (task->walk_ctx.level >= 2 && ((pte.PPN >> 9) & 0x1FF) != 0) misaligned = 1;
                        if (task->walk_ctx.level >= 3 && ((pte.PPN >> 18) & 0x1FF) != 0) misaligned = 1;
                        if (task->walk_ctx.level >= 4 && ((pte.PPN >> 27) & 0x1FF) != 0) misaligned = 1;
                    }
                    if (misaligned) {
                        task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                        walk_fault = true;
                        printf("[PTW_RSP] task_id=%u -> FAULT: misaligned superpage at level=%d, PPN=0x%lx\n",
                               task->task_id, task->walk_ctx.level, (uint64_t)pte.PPN);
                        fflush(stdout);
                        break;
                    }
                }

                printf("[PTW_RSP] task_id=%u, VS_WALK leaf: level=%d, page_sz=0x%lx, PPN=0x%lx, total_reads=%u\n",
                       task->task_id, task->walk_ctx.level, task->page_sz, pte.PPN, task->walk_ctx.ddr_read_count);
                fflush(stdout);

                // A/D bit processing
                if (pte.A == 0 || (task->is_write && pte.D == 0)) {
                    printf("[PTW_RSP] task_id=%u, A/D update needed: A=%d, D=%d, is_write=%d, SADE=%d, read_count=%u\n",
                           task->task_id, pte.A, pte.D, task->is_write, task->SADE, task->walk_ctx.ddr_read_count);
                    fflush(stdout);
                    if (task->SADE == 1) {
                        // Hardware A/D update: need AMO DDR write
                        pte.A = 1;
                        if (task->is_write) pte.D = 1;
                        task->walk_ctx.ad_pte = pte;
                        task->walk_ctx.ad_pte_addr = task->walk_ctx.base_addr +
                            task->walk_ctx.vpn[task->walk_ctx.level] * task->walk_ctx.ptesize;
                        task->vs_pte = pte;
                        task->gpa = ((pte.PPN * PAGESIZE) & ~(task->page_sz - 1)) |
                                    (task->iova & (task->page_sz - 1));
                        task->walk_ctx.walk_phase = PTW_AD_UPDATE;
                        task->walk_ctx.read_addr = task->walk_ctx.ad_pte_addr;
                        task->walk_ctx.read_size = task->walk_ctx.ptesize;
                        task->walk_ctx.ddr_read_count++;
                        need_next_ddr = true;
                        // [STAT] DDR access log - AD_UPDATE
                        if (task->walk_ctx.ddr_log_count < 8) {
                            task->walk_ctx.ddr_log_type[task->walk_ctx.ddr_log_count] = 5; // AD_UPDATE
                            task->walk_ctx.ddr_log_level[task->walk_ctx.ddr_log_count] = task->walk_ctx.level;
                            task->walk_ctx.ddr_log_addr[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_addr;
                            task->walk_ctx.ddr_log_size[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_size;
                            task->walk_ctx.ddr_log_count++;
                        }
                        printf("[PTW_RSP] task_id=%u, -> AD_UPDATE (read_count=%u), addr=0x%lx\n",
                               task->task_id, task->walk_ctx.ddr_read_count, task->walk_ctx.read_addr);
                        fflush(stdout);
                        break;
                    } else {
                        task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                        walk_fault = true;
                        break;
                    }
                }

                // NAPOT PPN adjustment
                if (pte.N) {
                    pte.PPN = (pte.PPN & ~0xFULL) |
                              ((task->iova / PAGESIZE) & 0xF);
                }

                // GPA calculation
                task->gpa = ((pte.PPN * PAGESIZE) & ~(task->page_sz - 1)) |
                            (task->iova & (task->page_sz - 1));
                task->vs_pte = pte;

                // Step 19: G-stage explicit translation
                if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                    // [两阶段预取] 保存VS L0表GPA基址（供预取任务计算顺序VS L0 PTE的GPA）
                    task->walk_ctx.vs_l0_gpa_base = task->walk_ctx.base_addr;
                    
                    // [S2] S2 Walker Cache lookup before GS_EXPLICIT
                    bool s2_cache_hit = false;
                    if (PTW_WALKER_S2_CACHE_ENABLED && PTW_WALKER_CACHE_ENABLED
                        && task->iosatp.MODE != IOSATP_Bare) {
                        iommu::CacheMessage s2_req = task_to_s2_walker_request(task, task->gpa);
                        cache_sub.walker_request_fifo.write(s2_req);
                        iommu::CacheMessage s2_resp = cache_sub.walker_response_fifo.read();
                        
                        if (s2_resp.hit) {
                            s2_cache_hit = true;
                            task->walk_ctx.s2_walker_hit_level = s2_resp.walker_level;
                            uint8_t GS_LEVELS = 0;
                            uint16_t gs_vpn[5] = {0};
                            extract_gs_vpn(task->gpa, task->iohgatp.MODE, gs_vpn, &GS_LEVELS);
                            for (int k = 0; k < 5; k++) task->walk_ctx.gs_vpn[k] = gs_vpn[k];
                            uint8_t gs_start_level = GS_LEVELS - 1 - s2_resp.walker_level;
                            task->walk_ctx.gs_level = gs_start_level;
                            task->walk_ctx.gs_base_addr = s2_resp.walker_data.next_ppn * PAGESIZE;
                            uint64_t gs_pte_addr = task->walk_ctx.gs_base_addr +
                                task->walk_ctx.gs_vpn[gs_start_level] * 8;
                            task->walk_ctx.read_addr = gs_pte_addr;
                            task->walk_ctx.read_size = 8;
                            task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                            task->walk_ctx.ddr_read_count++;
                            need_next_ddr = true;
                            printf("[PTW_RSP] task_id=%u, S2 Cache HIT level=%d -> GS_EXPLICIT from level=%d, addr=0x%lx\n",
                                   task->task_id, s2_resp.walker_level, gs_start_level, gs_pte_addr);
                            fflush(stdout);
                        } else {
                            task->walk_ctx.s2_walker_hit_level = 0;
                            printf("[PTW_RSP] task_id=%u, S2 Cache MISS -> normal GS_EXPLICIT\n", task->task_id);
                            fflush(stdout);
                        }
                    }
                    
                    if (!s2_cache_hit) {
                        init_gstage_walk(task, task->gpa);
                        task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                        task->walk_ctx.ddr_read_count++;
                        need_next_ddr = true;
                    }
                    // [STAT] DDR access log - GS_EXPLICIT
                    if (task->walk_ctx.ddr_log_count < 8) {
                        task->walk_ctx.ddr_log_type[task->walk_ctx.ddr_log_count] = 4; // GS_EXPLICIT
                        task->walk_ctx.ddr_log_level[task->walk_ctx.ddr_log_count] = 0;
                        task->walk_ctx.ddr_log_addr[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_addr;
                        task->walk_ctx.ddr_log_size[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_size;
                        task->walk_ctx.ddr_log_count++;
                    }
                    printf("[PTW_RSP] task_id=%u, -> GS_EXPLICIT (read_count=%u), gpa=0x%lx\n",
                           task->task_id, task->walk_ctx.ddr_read_count, task->gpa);
                    fflush(stdout);
                } else {
                    // G-stage Bare: complete
                    task->pa = task->gpa;
                    task->gst_page_sz = get_gstage_bare_page_size(&iommu_inst);
                    task->g_pte.raw = 0;
                    task->g_pte.PPN = task->gpa / PAGESIZE;
                    task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                    task->g_pte.X = task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                    task->g_pte.PBMT = PMA;
                    task->state = TASK_PTW_DONE;
                    walk_complete = true;
                    printf("[PTW_RSP] task_id=%u, VS_WALK leaf -> DONE, total_reads=%u, pa=0x%lx\n",
                           task->task_id, task->walk_ctx.ddr_read_count, task->pa);
                    fflush(stdout);
                }
            } else {
                // ===== Non-leaf node =====
                printf("[PTW_RSP] task_id=%u, VS_WALK non-leaf: level=%d -> next level %d, read_count=%u\n",
                       task->task_id, task->walk_ctx.level, task->walk_ctx.level - 1, task->walk_ctx.ddr_read_count);
                fflush(stdout);
                if (pte.PBMT != 0 || pte.D != 0 || pte.A != 0 || pte.U != 0) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }

                // =====================================================================
                // 保存中间结果到 Walker Cache entries（按 PTWc_X tag 含义对齐）
                // =====================================================================
                // 两阶段: VS PPN 是 GPA，SPA 将在 GS_IMPLICIT 完成后直接保存到 walker cache
                // 单阶段: VS PPN 即 SPA，直接保存
                // =====================================================================
                if (!(task->GV && task->iohgatp.MODE != IOHGATP_Bare)) {
                    // 单阶段: VS PPN 即 SPA，直接保存
                    if (task->iosatp.MODE == IOSATP_Sv48) {
                        if (task->walk_ctx.level == 3) {
                            task->walk_ctx.walker_cache_entries.ppn_level0 = pte.PPN;
                            task->walk_ctx.walker_cache_entries.valid_level0 = true;
                        } else if (task->walk_ctx.level == 2) {
                            task->walk_ctx.walker_cache_entries.ppn_level1 = pte.PPN;
                            task->walk_ctx.walker_cache_entries.valid_level1 = true;
                        } else if (task->walk_ctx.level == 1) {
                            task->walk_ctx.walker_cache_entries.ppn_level2 = pte.PPN;
                            task->walk_ctx.walker_cache_entries.valid_level2 = true;
                        }
                    } else {  // Sv39
                        if (task->walk_ctx.level == 2) {
                            task->walk_ctx.walker_cache_entries.ppn_level1 = pte.PPN;
                            task->walk_ctx.walker_cache_entries.valid_level1 = true;
                        } else if (task->walk_ctx.level == 1) {
                            task->walk_ctx.walker_cache_entries.ppn_level2 = pte.PPN;
                            task->walk_ctx.walker_cache_entries.valid_level2 = true;
                        }
                    }
                }
                // 两阶段: walker cache 由 GS_IMPLICIT leaf handler 直接保存，此处无需操作
                // level==0 为叶子节点，Walker Cache 不缓存叶子
                
                printf("[PTW_RSP] task_id=%u, VS_WALK non-leaf: level=%d -> next level %d, read_count=%u\n",
                       task->task_id, task->walk_ctx.level, task->walk_ctx.level - 1,
                       task->walk_ctx.ddr_read_count);
                fflush(stdout);

                task->walk_ctx.level--;
                if (task->walk_ctx.level < 0) {
                    task->cause = (task->is_exec ? 12 : task->is_read ? 13 : 15);
                    walk_fault = true;
                    break;
                }

                task->walk_ctx.base_addr = pte.PPN * PAGESIZE;
                uint64_t next_pte_addr = task->walk_ctx.base_addr +
                    task->walk_ctx.vpn[task->walk_ctx.level] * task->walk_ctx.ptesize;

                if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                    // 两阶段: VS PPN 是 GPA，需要 GS_IMPLICIT 翻译为 SPA
                    task->walk_ctx.vs_level = task->walk_ctx.level;
                    task->walk_ctx.pending_vs_pte_addr = next_pte_addr;
                    init_gstage_walk(task, next_pte_addr);
                    task->walk_ctx.walk_phase = PTW_GS_IMPLICIT;
                } else {
                    task->walk_ctx.read_addr = next_pte_addr;
                    task->walk_ctx.read_size = task->walk_ctx.ptesize;  // 默认: 单PTE
                    // walk_phase stays PTW_VS_WALK
                    
                    // [OPT] 合并叶子+Burst预取为1次DDR读 (仅在即将读L0叶子时)
                    if (task->walk_ctx.level == 0 &&
                        task->walk_ctx.prefetch_enabled && task->walk_ctx.prefetch_depth > 0) {
                        uint32_t ptesz = task->walk_ctx.ptesize;
                        uint32_t D = task->walk_ctx.prefetch_depth;  // 来自PT_DEDUP_PREFETCH_DEPTH
                        
                        // 边界检查: 不跨越4KB页表页 (使用PT_PAGE_MASK)
                        uint64_t start = next_pte_addr;
                        uint64_t end = start + (1 + D) * ptesz - 1;
                        if ((start & ~PT_PAGE_MASK) != (end & ~PT_PAGE_MASK)) {
                            uint64_t page_end = (start | PT_PAGE_MASK) + 1;
                            D = (page_end - start) / ptesz - 1;
                            task->walk_ctx.prefetch_depth = D;
                        }
                        
                        if (D > 0) {
                            task->walk_ctx.read_size = (1 + D) * ptesz;
                            task->walk_ctx.combined_burst_ready = true;
                            printf("[PTW_REQ] task_id=%u -> Combined leaf+burst read, size=%u (1+D=%u PTEs)\n",
                                   task->task_id, task->walk_ctx.read_size, 1 + D);
                            fflush(stdout);
                        }
                    }
                }
                task->walk_ctx.ddr_read_count++;
                need_next_ddr = true;
                // [STAT] DDR access log - VS_WALK non-leaf -> next level
                if (task->walk_ctx.ddr_log_count < 8) {
                    uint8_t log_type = (task->walk_ctx.level == 0 && task->walk_ctx.combined_burst_ready) ? 2 : 0;
                    task->walk_ctx.ddr_log_type[task->walk_ctx.ddr_log_count] = log_type;
                    task->walk_ctx.ddr_log_level[task->walk_ctx.ddr_log_count] = task->walk_ctx.level;
                    task->walk_ctx.ddr_log_addr[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_addr;
                    task->walk_ctx.ddr_log_size[task->walk_ctx.ddr_log_count] = task->walk_ctx.read_size;
                    task->walk_ctx.ddr_log_count++;
                }
                printf("[PTW_RSP] task_id=%u, VS_WALK non-leaf -> next addr=0x%lx, read_count=%u\n",
                       task->task_id, task->walk_ctx.read_addr, task->walk_ctx.ddr_read_count);
                fflush(stdout);
            }
            break;
        }

        case PTW_GS_IMPLICIT: {
            // ========== G-stage implicit translation ==========
            // Translates VS PTE address (GPA) to SPA
            // Corresponds to iommu_second_stage_trans.cc L139-255
            gpte_t gs_pte;
            gs_pte.raw = 0;
            memcpy(&gs_pte.raw, task->walk_ctx.read_buf, 8);

            if (gs_pte.V == 0 || (gs_pte.R == 0 && gs_pte.W == 1)) {
                task->cause = 21;  // Guest load page fault (implicit)
                task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                walk_fault = true;
                break;
            }

            if (gs_pte.R == 1 || gs_pte.X == 1) {
                // G-stage leaf: calculate translated SPA
                if (gs_pte.R == 0) {
                    task->cause = 21;
                    task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                    walk_fault = true;
                    break;
                }

                uint64_t gs_page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.gs_level; j++)
                    gs_page_sz *= 512;

                uint64_t spa = ((gs_pte.PPN * PAGESIZE) & ~(gs_page_sz - 1)) |
                               (task->walk_ctx.pending_vs_pte_addr & (gs_page_sz - 1));

                // G-stage A/D check (implicit: only need A=1)
                if (gs_pte.A == 0) {
                    if (task->GADE == 1) {
                        gs_pte.A = 1;
                    } else {
                        task->cause = 21;
                        task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                        walk_fault = true;
                        break;
                    }
                }

                // Return to VS-stage walk with translated address
                task->walk_ctx.read_addr = spa;
                task->walk_ctx.read_size = task->walk_ctx.ptesize;
                task->walk_ctx.walk_phase = PTW_VS_WALK;
                task->walk_ctx.ddr_read_count++;
                // 两阶段: 直接保存 SPA 到 walker cache（根据 vs_level 确定缓存层级）
                // vs_level = GS_IMPLICIT 翻译的目标 VS level（即即将读取的 VS PTE 所在表的层级）
                // vs_level=2 → SPA of L2 table → ppn_level0 (ptwc1, tag=VPN[3])
                // vs_level=1 → SPA of L1 table → ppn_level1 (ptwc2, tag=VPN[3]+VPN[2])
                // vs_level=0 → SPA of L0 table → ppn_level2 (ptwc3, tag=VPN[3]+VPN[2]+VPN[1])
                if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                    uint8_t vs_lvl = task->walk_ctx.vs_level;
                    uint64_t spa_ppn = spa / PAGESIZE;
                    // [两阶段预取] 保存VS L0表所在物理页号（供预取任务读取顺序VS L0 PTE）
                    if (vs_lvl == 0) {
                        task->walk_ctx.vs_l0_spa_ppn = spa_ppn;
                    }
                    if (task->iosatp.MODE == IOSATP_Sv48) {
                        if (vs_lvl == 2) {
                            task->walk_ctx.walker_cache_entries.ppn_level0 = spa_ppn;
                            task->walk_ctx.walker_cache_entries.valid_level0 = true;
                        } else if (vs_lvl == 1) {
                            task->walk_ctx.walker_cache_entries.ppn_level1 = spa_ppn;
                            task->walk_ctx.walker_cache_entries.valid_level1 = true;
                        } else if (vs_lvl == 0) {
                            task->walk_ctx.walker_cache_entries.ppn_level2 = spa_ppn;
                            task->walk_ctx.walker_cache_entries.valid_level2 = true;
                        }
                    } else {  // Sv39
                        if (vs_lvl == 1) {
                            task->walk_ctx.walker_cache_entries.ppn_level1 = spa_ppn;
                            task->walk_ctx.walker_cache_entries.valid_level1 = true;
                        } else if (vs_lvl == 0) {
                            task->walk_ctx.walker_cache_entries.ppn_level2 = spa_ppn;
                            task->walk_ctx.walker_cache_entries.valid_level2 = true;
                        }
                    }
                    printf("[PTW_RSP] task_id=%u, GS_IMPLICIT leaf -> VS_WALK (read_count=%u), spa=0x%lx, saved to walker cache vs_level=%d\n",
                           task->task_id, task->walk_ctx.ddr_read_count, spa, vs_lvl);
                } else {
                    printf("[PTW_RSP] task_id=%u, GS_IMPLICIT leaf -> VS_WALK (read_count=%u), spa=0x%lx\n",
                           task->task_id, task->walk_ctx.ddr_read_count, spa);
                }
                fflush(stdout);
            } else {
                // G-stage non-leaf: continue walk
                if (gs_pte.PBMT != 0 || gs_pte.D != 0 || gs_pte.A != 0 ||
                    gs_pte.U != 0) {
                    task->cause = 21;
                    task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                    walk_fault = true;
                    break;
                }
                task->walk_ctx.gs_level--;
                if (task->walk_ctx.gs_level < 0) {
                    task->cause = 21;
                    task->iotval2 = (task->walk_ctx.pending_vs_pte_addr & ~0x3ULL) | 0x1;
                    walk_fault = true;
                    break;
                }
                task->walk_ctx.gs_base_addr = gs_pte.PPN * PAGESIZE;
                uint64_t gs_next = task->walk_ctx.gs_base_addr +
                    task->walk_ctx.gs_vpn[task->walk_ctx.gs_level] * 8;
                task->walk_ctx.read_addr = gs_next;
                task->walk_ctx.read_size = 8;
                task->walk_ctx.ddr_read_count++;
                printf("[PTW_RSP] task_id=%u, GS_IMPLICIT non-leaf -> next (read_count=%u), addr=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, gs_next);
                fflush(stdout);
                // walk_phase stays PTW_GS_IMPLICIT
            }
            need_next_ddr = true;
            break;
        }

        case PTW_VS_PTE_READ: {
            // ========== [两阶段预取] 读取顺序VS L0 PTE ==========
            // 预取任务从DDR读取VS L0 PTE，提取数据页GPA PPN，然后启动G-stage walk
            spte_t vs_pte;
            vs_pte.raw = 0;
            memcpy(&vs_pte.raw, task->walk_ctx.read_buf, task->walk_ctx.ptesize);
            
            printf("[PTW_2STAGE_PF] task_id=%u, VS_PTE_READ -> raw=0x%lx, V=%d, R=%d, W=%d, PPN=0x%lx\n",
                   task->task_id, (uint64_t)vs_pte.raw, vs_pte.V, vs_pte.R, vs_pte.W, (uint64_t)vs_pte.PPN);
            fflush(stdout);
            
            // 验证VS L0 PTE
            if (vs_pte.V == 0 || (vs_pte.R == 0 && vs_pte.W == 1)) {
                printf("[PTW_2STAGE_PF] task_id=%u -> VS PTE invalid, skip prefetch\n", task->task_id);
                fflush(stdout);
                // 预取PTE无效，保存当前组结果并通知monitor
                if (task->walk_ctx.prefetch_group_id != 0) {
                    uint32_t group_id = task->walk_ctx.prefetch_group_id;
                    uint32_t task_idx = task->walk_ctx.prefetch_idx;
                    prefetch_group_mtx.lock();
                    auto g_it = prefetch_groups.find(group_id);
                    if (g_it != prefetch_groups.end()) {
                        auto& group = g_it->second;
                        group.vs_ptes[task_idx].raw = 0;
                        group.g_ptes[task_idx].raw = 0;
                        group.pas[task_idx] = 0;
                        group.page_szs[task_idx] = PAGE_SIZE_4KB;
                        // [FIX-V3] 防止下溢
                        if (group.pending_tasks > 0) {
                            group.pending_tasks--;
                        }
                        // [FIX] 必须等待主任务也完成(main_task_done=true)才能触发monitor
                        // 否则monitor会在主任务GS_EXPLICIT完成之前就处理组，导致PA=0
                        if (group.pending_tasks == 0 && group.main_task_done && !group.completed) {
                            group.completed = true;
                            prefetch_group_completed_event.notify(SC_ZERO_TIME);
                        }
                    }
                    prefetch_group_mtx.unlock();
                }
                // [FIX] 从active_walks中移除，不delete（Buffer entry可能指向该task）
                ptw_walks_mtx.lock();
                ptw_active_walks.erase(task->task_id);
                ptw_walks_mtx.unlock();
                goto skip_walk_cleanup;
            }
            
            // 提取数据页GPA PPN并计算GPA
            uint64_t data_page_gpa = vs_pte.PPN * PAGESIZE;  // 4KB页，offset=0
            task->vs_pte = vs_pte;
            task->vs_pte.PPN = vs_pte.PPN;
            task->gpa = data_page_gpa;
            
            // [S2] S2 Walker Cache lookup before GS_EXPLICIT (prefetch path)
            bool s2_pf_hit = false;
            if (PTW_WALKER_S2_CACHE_ENABLED && PTW_WALKER_CACHE_ENABLED
                && task->GV && task->iohgatp.MODE != IOHGATP_Bare
                && task->iosatp.MODE != IOSATP_Bare) {
                iommu::CacheMessage s2_req = task_to_s2_walker_request(task, data_page_gpa);
                cache_sub.walker_request_fifo.write(s2_req);
                iommu::CacheMessage s2_resp = cache_sub.walker_response_fifo.read();
                
                if (s2_resp.hit) {
                    s2_pf_hit = true;
                    task->walk_ctx.s2_walker_hit_level = s2_resp.walker_level;
                    uint8_t GS_LEVELS = 0;
                    uint16_t gs_vpn[5] = {0};
                    extract_gs_vpn(data_page_gpa, task->iohgatp.MODE, gs_vpn, &GS_LEVELS);
                    for (int k = 0; k < 5; k++) task->walk_ctx.gs_vpn[k] = gs_vpn[k];
                    uint8_t gs_start_level = GS_LEVELS - 1 - s2_resp.walker_level;
                    task->walk_ctx.gs_level = gs_start_level;
                    task->walk_ctx.gs_base_addr = s2_resp.walker_data.next_ppn * PAGESIZE;
                    uint64_t gs_pte_addr = task->walk_ctx.gs_base_addr +
                        task->walk_ctx.gs_vpn[gs_start_level] * 8;
                    task->walk_ctx.read_addr = gs_pte_addr;
                    task->walk_ctx.read_size = 8;
                    task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                    task->walk_ctx.ddr_read_count++;
                    need_next_ddr = true;
                    printf("[PTW_2STAGE_PF] task_id=%u, S2 Cache HIT level=%d -> GS_EXPLICIT from level=%d\n",
                           task->task_id, s2_resp.walker_level, gs_start_level);
                    fflush(stdout);
                } else {
                    task->walk_ctx.s2_walker_hit_level = 0;
                }
            }
            
            if (!s2_pf_hit) {
                // 初始化G-stage walk（将数据页GPA翻译为SPA）
                init_gstage_walk(task, data_page_gpa);
                task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;
                task->walk_ctx.ddr_read_count++;  // DDR #2: G-stage开始
                need_next_ddr = true;
            }
            
            printf("[PTW_2STAGE_PF] task_id=%u, VS_PTE_READ -> GS_EXPLICIT, data_gpa=0x%lx, ddr_count=%u\n",
                   task->task_id, data_page_gpa, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            
            break;
        }

        case PTW_GS_EXPLICIT: {
            // ========== G-stage explicit translation: GPA → final PA ==========
            // Corresponds to iommu_second_stage_trans.cc L139-423
            gpte_t gs_pte;
            gs_pte.raw = 0;
            memcpy(&gs_pte.raw, task->walk_ctx.read_buf, 8);

            if (gs_pte.V == 0 || (gs_pte.R == 0 && gs_pte.W == 1)) {
                set_guest_fault_cause(task, 0);
                task->iotval2 = (task->gpa & ~0x3ULL);
                walk_fault = true;
                break;
            }

            if (gs_pte.R == 1 || gs_pte.X == 1) {
                // G-stage leaf: permission check + PA calculation
                if (task->check_access_perms) {
                    if (task->is_read && gs_pte.R == 0) {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        break;
                    }
                    if (task->is_write && gs_pte.W == 0) {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        break;
                    }
                    if (task->is_exec && gs_pte.X == 0) {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        break;
                    }
                }

                // A/D bit check
                if (gs_pte.A == 0 || (task->is_write && gs_pte.D == 0)) {
                    if (task->GADE == 1) {
                        gs_pte.A = 1;
                        if (task->is_write) gs_pte.D = 1;
                    } else {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        break;
                    }
                }

                // G-stage superpage size calculation
                task->gst_page_sz = PAGESIZE;
                for (int j = 0; j < task->walk_ctx.gs_level; j++)
                    task->gst_page_sz *= 512;

                // Misaligned G-stage superpage check
                if (task->walk_ctx.gs_level > 0) {
                    uint8_t gs_misaligned = 0;
                    if ((gs_pte.PPN & 0x1FF) != 0) gs_misaligned = 1;
                    if (task->walk_ctx.gs_level >= 2 && ((gs_pte.PPN >> 9) & 0x1FF) != 0) gs_misaligned = 1;
                    if (task->walk_ctx.gs_level >= 3 && ((gs_pte.PPN >> 18) & 0x1FF) != 0) gs_misaligned = 1;
                    if (task->walk_ctx.gs_level >= 4 && ((gs_pte.PPN >> 27) & 0x1FF) != 0) gs_misaligned = 1;
                    if (gs_misaligned) {
                        set_guest_fault_cause(task, 0);
                        task->iotval2 = (task->gpa & ~0x3ULL);
                        walk_fault = true;
                        printf("[PTW_RSP] task_id=%u -> FAULT: misaligned G-stage superpage at level=%d, PPN=0x%lx\n",
                               task->task_id, task->walk_ctx.gs_level, (uint64_t)gs_pte.PPN);
                        fflush(stdout);
                        break;
                    }
                }

                task->pa = ((gs_pte.PPN * PAGESIZE) & ~(task->gst_page_sz - 1)) |
                           (task->gpa & (task->gst_page_sz - 1));
                task->g_pte = gs_pte;

                // Handle virtual interrupt file overlap
                handle_virtual_interrupt_file_overlap(&task->DC, task->gpa,
                                                     &task->gst_page_sz);

                task->state = TASK_PTW_DONE;
                walk_complete = true;
                printf("[PTW_RSP] task_id=%u, GS_EXPLICIT leaf -> DONE, total_reads=%u, pa=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, task->pa);
                fflush(stdout);
            } else {
                // G-stage non-leaf: continue walk
                if (gs_pte.PBMT != 0 || gs_pte.D != 0 || gs_pte.A != 0 ||
                    gs_pte.U != 0) {
                    set_guest_fault_cause(task, 0);
                    task->iotval2 = (task->gpa & ~0x3ULL);
                    walk_fault = true;
                    break;
                }
                // [S2] Save intermediate GS walk results for S2 Cache update
                // gs_level=3 result → ptwc1 (highest intermediate)
                // gs_level=2 result → ptwc2
                // gs_level=1 result → ptwc3 (lowest intermediate, closest to leaf)
                if (PTW_WALKER_S2_CACHE_ENABLED && PTW_WALKER_CACHE_ENABLED) {
                    if (task->walk_ctx.gs_level == 3) {
                        task->walk_ctx.s2_walker_cache_entries.s2_ppn_level1 = gs_pte.PPN;
                        task->walk_ctx.s2_walker_cache_entries.s2_valid_level1 = true;
                    } else if (task->walk_ctx.gs_level == 2) {
                        task->walk_ctx.s2_walker_cache_entries.s2_ppn_level2 = gs_pte.PPN;
                        task->walk_ctx.s2_walker_cache_entries.s2_valid_level2 = true;
                    } else if (task->walk_ctx.gs_level == 1) {
                        task->walk_ctx.s2_walker_cache_entries.s2_ppn_level3 = gs_pte.PPN;
                        task->walk_ctx.s2_walker_cache_entries.s2_valid_level3 = true;
                    }
                }
                task->walk_ctx.gs_level--;
                if (task->walk_ctx.gs_level < 0) {
                    set_guest_fault_cause(task, 0);
                    task->iotval2 = (task->gpa & ~0x3ULL);
                    walk_fault = true;
                    break;
                }
                task->walk_ctx.gs_base_addr = gs_pte.PPN * PAGESIZE;
                uint64_t gs_next = task->walk_ctx.gs_base_addr +
                    task->walk_ctx.gs_vpn[task->walk_ctx.gs_level] * 8;
                task->walk_ctx.read_addr = gs_next;
                task->walk_ctx.read_size = 8;
                task->walk_ctx.ddr_read_count++;
                printf("[PTW_RSP] task_id=%u, GS_EXPLICIT non-leaf -> next (read_count=%u), addr=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, gs_next);
                fflush(stdout);
                // walk_phase stays PTW_GS_EXPLICIT
                need_next_ddr = true;
            }
            break;
        }

        case PTW_AD_UPDATE: {
            // A/D bit AMO update completed
            // Continue to G-stage translation or complete
            printf("[PTW_RSP] task_id=%u, AD_UPDATE completed, read_count=%u\n",
                   task->task_id, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            if (task->GV && task->iohgatp.MODE != IOHGATP_Bare) {
                init_gstage_walk(task, task->gpa);
                task->walk_ctx.walk_phase = PTW_GS_EXPLICIT;

                // [S2] S2 Walker Cache lookup before GS_EXPLICIT (AD_UPDATE path)
                if (PTW_WALKER_S2_CACHE_ENABLED && PTW_WALKER_CACHE_ENABLED
                    && task->iosatp.MODE != IOSATP_Bare) {
                    iommu::CacheMessage s2_req = task_to_s2_walker_request(task, task->gpa);
                    cache_sub.walker_request_fifo.write(s2_req);
                    iommu::CacheMessage s2_resp = cache_sub.walker_response_fifo.read();

                    if (s2_resp.hit) {
                        task->walk_ctx.s2_walker_hit_level = s2_resp.walker_level;
                        uint8_t GS_LEVELS = 0;
                        uint16_t gs_vpn[5] = {0};
                        extract_gs_vpn(task->gpa, task->iohgatp.MODE, gs_vpn, &GS_LEVELS);
                        for (int k = 0; k < 5; k++) task->walk_ctx.gs_vpn[k] = gs_vpn[k];
                        uint8_t gs_start_level = GS_LEVELS - 1 - s2_resp.walker_level;
                        task->walk_ctx.gs_level = gs_start_level;
                        task->walk_ctx.gs_base_addr = s2_resp.walker_data.next_ppn * PAGESIZE;
                        uint64_t gs_pte_addr = task->walk_ctx.gs_base_addr +
                            task->walk_ctx.gs_vpn[gs_start_level] * 8;
                        task->walk_ctx.read_addr = gs_pte_addr;
                        task->walk_ctx.read_size = 8;
                        printf("[PTW_RSP] task_id=%u, AD_UPDATE S2 Cache HIT level=%d -> GS_EXPLICIT from level=%d\n",
                               task->task_id, s2_resp.walker_level, gs_start_level);
                        fflush(stdout);
                    } else {
                        task->walk_ctx.s2_walker_hit_level = 0;
                        printf("[PTW_RSP] task_id=%u, AD_UPDATE S2 Cache MISS -> normal GS_EXPLICIT\n", task->task_id);
                        fflush(stdout);
                    }
                }

                task->walk_ctx.ddr_read_count++;
                need_next_ddr = true;
                printf("[PTW_RSP] task_id=%u, AD_UPDATE -> GS_EXPLICIT (read_count=%u)\n",
                       task->task_id, task->walk_ctx.ddr_read_count);
                fflush(stdout);
            } else {
                task->pa = task->gpa;
                task->gst_page_sz = get_gstage_bare_page_size(&iommu_inst);
                task->g_pte.raw = 0;
                task->g_pte.PPN = task->gpa / PAGESIZE;
                task->g_pte.D = task->g_pte.A = task->g_pte.U = 1;
                task->g_pte.X = task->g_pte.W = task->g_pte.R = task->g_pte.V = 1;
                task->g_pte.PBMT = PMA;
                task->state = TASK_PTW_DONE;
                walk_complete = true;
                printf("[PTW_RSP] task_id=%u, AD_UPDATE -> DONE, total_reads=%u, pa=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, task->pa);
                fflush(stdout);
            }
            break;
        }

        } // end switch

        // ========== Unified walk result handling ==========
        if (walk_fault) {
            printf("[PTW_RSP] task_id=%u -> WALK FAULT, cause=%d, total_reads=%u, is_pf=%d\n",
                   task->task_id, task->cause, task->walk_ctx.ddr_read_count,
                   task->walk_ctx.is_prefetch_task);
            fflush(stdout);
            task->state = TASK_FAULT;
            ptw_walks_mtx.lock();
            ptw_active_walks.erase(rsp.task_id);
            ptw_walks_mtx.unlock();
            
            // [FIX-V3] outstanding计数递减逻辑（按组计数，避免双重递减）
            // - 预取任务: 不经过ptw_req_thread，未递增，此处不减
            // - 属于预取组的非预取任务(主任务): 由Monitor统一递减，此处不减
            //   (主任务fault时会标记group.completed，触发Monitor处理)
            // - 不属于预取组的非预取任务: 此处直接递减
            if (!task->walk_ctx.is_prefetch_task
                && task->walk_ctx.prefetch_group_id == 0
                && !task->walk_ctx.is_two_stage_prefetch) {
                ptw_outstanding_task_count--;
                assert(ptw_outstanding_task_count >= 0);  // [FIX-第10条] 防止下溢
                ptw_task_completed_event.notify(SC_ZERO_TIME);
            }
            ptw_total_completed++;  // [STAT]
            ptw_total_ddr_reads += task->walk_ctx.ddr_read_count;
            ptw_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
            if (task->walk_ctx.is_prefetch_task) {
                ptw_prefetch_task_count++;
                ptw_prefetch_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                ptw_prefetch_total_ddr_reads += task->walk_ctx.ddr_read_count;
            } else {
                ptw_main_task_count++;
                ptw_main_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                ptw_main_total_ddr_reads += task->walk_ctx.ddr_read_count;
            }
            { auto _s = ptw_task_start_ns.find(task->task_id);
              if (_s != ptw_task_start_ns.end()) {
                  double lat = sc_time_stamp().to_seconds()*1e9 - _s->second;
                  ptw_total_exec_ns += lat;
                  ptw_task_latency_values.push_back(lat);
                  ptw_task_start_ns.erase(_s); } }
            
            // [FIX] 预取任务fault: 递减group.pending_tasks并通知monitor
            if (task->walk_ctx.is_prefetch_task) {
                if (task->walk_ctx.prefetch_group_id != 0) {
                    uint32_t group_id = task->walk_ctx.prefetch_group_id;
                    uint32_t task_idx = task->walk_ctx.prefetch_idx;
                    prefetch_group_mtx.lock();
                    auto git = prefetch_groups.find(group_id);
                    if (git != prefetch_groups.end()) {
                        auto& group = git->second;
                        // 保存无效结果
                        group.vs_ptes[task_idx].raw = 0;
                        group.g_ptes[task_idx].raw = 0;
                        group.pas[task_idx] = 0;
                        // [FIX-V3] 防止pending_tasks下溢(主任务fault时可能已设为0)
                        if (group.pending_tasks > 0) {
                            group.pending_tasks--;
                        }
                        printf("[PTW_PREFETCH] Prefetch task %u fault in group %u, pending=%u\n",
                               task->task_id, group_id, group.pending_tasks);
                        if (group.pending_tasks == 0 && group.main_task_done && !group.completed) {
                            group.completed = true;
                            prefetch_group_completed_event.notify(SC_ZERO_TIME);
                        }
                    }
                    prefetch_group_mtx.unlock();
                }
                goto skip_walk_cleanup;
            }
            
            // [FIX] 非预取任务fault处理
            // 如果该任务是预取组的main_task，不能直接转发（monitor还需要访问task指针）
            // 标记组为fault完成，由monitor统一处理转发和释放
            if (task->walk_ctx.prefetch_group_id != 0 || task->walk_ctx.is_two_stage_prefetch) {
                uint32_t group_id = task->walk_ctx.prefetch_group_id;
                if (group_id != 0) {
                    prefetch_group_mtx.lock();
                    auto git = prefetch_groups.find(group_id);
                    if (git != prefetch_groups.end()) {
                        auto& group = git->second;
                        group.main_task_done = true;
                        group.pending_tasks = 0;
                        group.completed = true;
                        group.has_fault = true;
                        
                        // [FIX-V3] 填充main_task的pt_updates[0]，供flush_dedup_buffer_by_iova使用
                        // 主任务fault时，VS walk可能已成功但G-stage失败
                        // 使用vs_pte.PPN计算GPA作为PA的近似值（实际应report fault）
                        task->walk_ctx.pt_updates[0].vs_pte = task->vs_pte;
                        task->walk_ctx.pt_updates[0].g_pte = task->g_pte;
                        task->walk_ctx.pt_updates[0].pa = task->gpa;  // GPA作为fallback
                        task->walk_ctx.pt_updates[0].iova = task->iova & ~PAGE_MASK_4KB;
                        task->walk_ctx.pt_updates[0].page_sz = task->page_sz;
                        
                        // 同步填充group.vs_ptes[0]和group.g_ptes[0]
                        group.vs_ptes[0] = task->vs_pte;
                        group.g_ptes[0] = task->g_pte;
                        group.pas[0] = task->gpa;
                        
                        printf("[PTW_PREFETCH] Main task %u fault in group %u, marked group as faulted.\n",
                               task->task_id, group_id);
                        fflush(stdout);
                        prefetch_group_completed_event.notify(SC_ZERO_TIME);
                    }
                    prefetch_group_mtx.unlock();
                }
                // 不转发、不写pt_update_fifo - 由monitor处理
                goto skip_walk_cleanup;
            }
            
            // 普通非预取任务fault: 正常转发
            iommu::CacheMessage pt_update_req = task_to_pt_update(task);
            pt_update_req.timestamp = sc_time_stamp();
            cache_sub.pt_update_fifo.write(pt_update_req);
            
            pt_cache_to_fwd_fifo.write(task);
            va_dedup_recover(task, true);
        }
        else if (walk_complete) {
            walk_complete_handler:  // 用于goto跳转
            printf("[PTW_RSP] task_id=%u -> WALK COMPLETE, pa=0x%lx, total_reads=%u\n",
                   task->task_id, task->pa, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            
            // =====================================================================
            // 预取组结果收集(完整9笔独立walk方案)
            // =====================================================================
            if (task->walk_ctx.prefetch_group_id != 0) {
                // 这是预取组的一员(主任务或预取任务)
                uint32_t group_id = task->walk_ctx.prefetch_group_id;
                uint32_t task_idx = task->walk_ctx.prefetch_idx;
                
                prefetch_group_mtx.lock();
                
                // [FIX-V3] 检查group是否存在(可能已被Monitor处理并删除)
                // 如果group不存在, 说明主任务fault后Monitor已处理了该组,
                // 预取任务只需清理active_walks即可, 不要重新创建group
                auto git = prefetch_groups.find(group_id);
                if (git == prefetch_groups.end()) {
                    prefetch_group_mtx.unlock();
                    printf("[PTW_PREFETCH] Group %u already removed by monitor, task %u skipping\n",
                           group_id, task->task_id);
                    fflush(stdout);
                    // 预取任务: 清理active_walks后跳过
                    if (task->walk_ctx.is_prefetch_task) {
                        ptw_walks_mtx.lock();
                        ptw_active_walks.erase(rsp.task_id);
                        ptw_walks_mtx.unlock();
                        goto skip_walk_cleanup;
                    }
                    // 主任务: 也跳过(已由monitor处理)
                    ptw_walks_mtx.lock();
                    ptw_active_walks.erase(rsp.task_id);
                    ptw_walks_mtx.unlock();
                    goto skip_walk_cleanup;
                }
                auto& group = git->second;
                
                // 保存当前walk结果
                group.vs_ptes[task_idx] = task->vs_pte;
                group.g_ptes[task_idx] = task->g_pte;
                group.pas[task_idx] = task->pa;
                group.page_szs[task_idx] = task->page_sz;
                
                // [FIX] 同步填充main_task的pt_updates[]，供flush_dedup_buffer_by_iova使用
                // early spawn路径跳过了combined burst处理(其中原本会填充pt_updates[])
                // 关键: 所有任务(主+预取)都必须写到main_task的pt_updates[]中
                // 因为flush_dedup_buffer_by_iova只读取main_task->walk_ctx.pt_updates[]
                {
                    iommu_task_t* pt_upd_owner = group.main_task ? group.main_task : task;
                    pt_upd_owner->walk_ctx.pt_updates[task_idx].vs_pte = task->vs_pte;
                    pt_upd_owner->walk_ctx.pt_updates[task_idx].g_pte = task->g_pte;
                    pt_upd_owner->walk_ctx.pt_updates[task_idx].pa = task->pa;
                    pt_upd_owner->walk_ctx.pt_updates[task_idx].iova = task->iova & ~PAGE_MASK_4KB;
                    pt_upd_owner->walk_ctx.pt_updates[task_idx].page_sz = task->page_sz;
                }
                
                // 标记主任务已完成pt_updates填充
                if (!task->walk_ctx.is_prefetch_task) {
                    group.main_task_done = true;
                }
                
                // 仅预取任务递减pending计数(主任务由monitor线程单独处理)
                if (task->walk_ctx.is_prefetch_task) {
                    // [FIX-V3] 防止pending_tasks下溢:
                    // 当主任务fault时, pending_tasks被设为0, 此后预取任务完成时
                    // 不应再递减(否则0-1=UINT32_MAX, 导致group永远无法completed)
                    if (group.pending_tasks > 0) {
                        group.pending_tasks--;
                    }
                    
                    printf("[PTW_PREFETCH] Walk completed: group=%u, idx=%u, pending=%u\n",
                           group_id, task_idx, group.pending_tasks);
                    fflush(stdout);
                }
                
                // 检查是否可以触发monitor: 预取任务全部完成 + 主任务已完成
                if (group.pending_tasks == 0 && group.main_task_done && !group.completed) {
                    group.completed = true;
                    printf("[PTW_PREFETCH] Group %u ALL COMPLETED (pending=0, main_done)! Trigger batch update.\n", group_id);
                    fflush(stdout);
                    
                    // 触发批量返回
                    prefetch_group_completed_event.notify(SC_ZERO_TIME);
                }
                
                prefetch_group_mtx.unlock();
                
                // [FIX] 预取任务处理
                if (task->walk_ctx.is_prefetch_task) {
                    // [S2] 预取任务也需要更新S2 Walker Cache
                    if (PTW_WALKER_S2_CACHE_ENABLED && PTW_WALKER_CACHE_ENABLED
                        && task->GV && task->iohgatp.MODE != IOHGATP_Bare
                        && task->iosatp.MODE != IOSATP_Bare) {
                        iommu::CacheMessage s2_req = task_to_s2_walker_update(task, task->gpa);
                        if (s2_req.walker_update_kind != iommu::WalkerUpdateKind::NONE) {
                            cache_sub.walker_update_fifo.write(s2_req);
                            printf("[t=%llu ns][PTW_PREFETCH] task_id=%u -> S2 Walker Cache UPDATE (prefetch task, gpa=0x%lx, kind=%d)\n",
                                   (unsigned long long)sc_core::sc_time_stamp().value()/1000,
                                   task->task_id, task->gpa,
                                   static_cast<int>(s2_req.walker_update_kind));
                            fflush(stdout);
                        }
                    }
                    // 从active_walks中移除（预取任务已完成walk）
                    ptw_walks_mtx.lock();
                    ptw_active_walks.erase(rsp.task_id);
                    ptw_walks_mtx.unlock();
                    // 不delete! Buffer entry可能仍指向该task
                    // flush_dedup_buffer_by_iova会检查is_prefetch_task并释放
                    // [STAT] 预取任务完成时收集DDR读取统计
                    ptw_total_completed++;
                    ptw_total_ddr_reads += task->walk_ctx.ddr_read_count;
                    ptw_prefetch_task_count++;
                    ptw_prefetch_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                    ptw_prefetch_total_ddr_reads += task->walk_ctx.ddr_read_count;
                    ptw_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                    if (task->walk_ctx.ddr_read_count > ptw_max_ddr_reads)
                        ptw_max_ddr_reads = task->walk_ctx.ddr_read_count;
                    if (task->walk_ctx.ddr_read_count < ptw_min_ddr_reads)
                        ptw_min_ddr_reads = task->walk_ctx.ddr_read_count;
                    { auto _s = ptw_task_start_ns.find(task->task_id);
                      if (_s != ptw_task_start_ns.end()) {
                          double lat = sc_time_stamp().to_seconds()*1e9 - _s->second;
                          ptw_total_exec_ns += lat;
                          ptw_task_latency_values.push_back(lat);
                          if (lat > ptw_max_task_latency_ns)
                              ptw_max_task_latency_ns = lat;
                          if (lat < ptw_min_task_latency_ns)
                              ptw_min_task_latency_ns = lat;
                          ptw_task_start_ns.erase(_s);
                      } }
                    printf("[PTW_PREFETCH] Prefetch task %u completed, deferred to flush. DDR_reads=%u\n",
                           task->task_id, task->walk_ctx.ddr_read_count);
                    fflush(stdout);
                    goto skip_walk_cleanup;
                }
                // 主任务继续后续流程(由monitor thread释放)
            }
            
            // =====================================================================
            // [两阶段预取] GS_EXPLICIT leaf完成 + 预取启用 + 两阶段模式
            // 仅在early spawn未触发时执行(walker cache miss路径)
            // early spawn已在ptw_req_thread中设置is_two_stage_prefetch=true
            // =====================================================================
            if (task->walk_ctx.prefetch_enabled && task->walk_ctx.prefetch_depth > 0
                && task->GV && task->iohgatp.MODE != IOHGATP_Bare
                && !task->walk_ctx.is_prefetch_task
                && !task->walk_ctx.is_two_stage_prefetch) {
                
                uint32_t D = task->walk_ctx.prefetch_depth;
                uint32_t group_id = task->task_id;
                
                task->walk_ctx.is_two_stage_prefetch = true;
                
                printf("[PTW_2STAGE_PF] task_id=%u -> Two-stage prefetch, D=%u, base_vs_ppn=0x%lx\n",
                       task->task_id, D, (uint64_t)task->vs_pte.PPN);
                fflush(stdout);
                
                // 1. 初始化预取组，主任务结果存到index 0
                task->walk_ctx.prefetch_group_id = group_id;
                task->walk_ctx.prefetch_total = 1 + D;
                task->walk_ctx.prefetch_idx = 0;
                
                prefetch_group_mtx.lock();
                auto& group = prefetch_groups[group_id];
                group.main_task = task;
                group.pending_tasks = D;  // D个预取任务待完成
                group.total_tasks = 1 + D;
                group.completed = false;
                group.vs_ptes[0] = task->vs_pte;
                group.g_ptes[0] = task->g_pte;
                group.pas[0] = task->pa;
                group.page_szs[0] = task->page_sz;
                group.group_iovas[0] = task->iova & ~PAGE_MASK_4KB;
                // [STAT] 记录组起始时间(使用主任务进入PTW的时刻)
                { auto _s = ptw_task_start_ns.find(task->task_id);
                  if (_s != ptw_task_start_ns.end()) {
                      group.group_start_ns = _s->second;
                  } else {
                      group.group_start_ns = sc_time_stamp().to_seconds() * 1e9;
                  } }
                
                // 2. 主任务Walker Cache更新（两阶段预取路径）
                if (PTW_WALKER_CACHE_ENABLED) {
                    iommu::CacheMessage walker_req = task_to_walker_update(task);
                    cache_sub.walker_update_fifo.write(walker_req);
                    printf("[t=%llu ns][PTW_RSP] task_id=%u -> Walker Cache UPDATE (Two-stage PF path)\n",
                           (unsigned long long)sc_core::sc_time_stamp().value()/1000,
                           task->task_id);
                    fflush(stdout);
                }
                // [S2] S2 Walker Cache update (Two-stage PF path)
                if (PTW_WALKER_S2_CACHE_ENABLED && PTW_WALKER_CACHE_ENABLED
                    && task->GV && task->iohgatp.MODE != IOHGATP_Bare
                    && task->iosatp.MODE != IOSATP_Bare) {
                    iommu::CacheMessage s2_req = task_to_s2_walker_update(task, task->gpa);
                    if (s2_req.walker_update_kind != iommu::WalkerUpdateKind::NONE) {
                        cache_sub.walker_update_fifo.write(s2_req);
                        printf("[t=%llu ns][PTW_RSP] task_id=%u -> S2 Walker Cache UPDATE (Two-stage PF path)\n",
                               (unsigned long long)sc_core::sc_time_stamp().value()/1000, task->task_id);
                        fflush(stdout);
                    }
                }
                
                // 3. 创建D个预取任务：先SPA读VS L0 PTE，再G-stage walk数据页GPA（共5次DDR）
                uint64_t vs_l0_spa_ppn = task->walk_ctx.vs_l0_spa_ppn;  // VS L0表所在物理页号
                // [FIX] Walker Cache hit L3时，vs_l0_spa_ppn已在walker_response_to_task()中设置
                // 若仍为0（不应发生），使用base_addr作为最终回填（level=0时base_addr=VS L0表SPA）
                if (vs_l0_spa_ppn == 0 && task->walk_ctx.level == 0) {
                    vs_l0_spa_ppn = task->walk_ctx.base_addr / PAGESIZE;
                    printf("[PTW_2STAGE_PF] vs_l0_spa_ppn recovered from base_addr: 0x%lx\n", vs_l0_spa_ppn);
                    fflush(stdout);
                }
                uint64_t vs_l0_gpa_base = task->walk_ctx.vs_l0_gpa_base; // VS L0表GPA基址
                uint16_t main_vpn0 = task->walk_ctx.vpn[0];              // 主任务的VPN[0]
                uint8_t ptesize = task->walk_ctx.ptesize;                // PTE大小(8字节)
                
                printf("[PTW_2STAGE_PF] vs_l0_spa_ppn=0x%lx, vs_l0_gpa_base=0x%lx, vpn[0]=%u\n",
                       vs_l0_spa_ppn, vs_l0_gpa_base, main_vpn0);
                fflush(stdout);
                
                for (uint32_t d = 0; d < D; d++) {
                    iommu_task_t* pf_task = new iommu_task_t();
                    task_id_mtx.lock();
                    pf_task->task_id = next_task_id++;
                    task_id_mtx.unlock();
                    
                    // 复制主任务上下文
                    pf_task->iosatp = task->iosatp;
                    pf_task->iohgatp = task->iohgatp;
                    pf_task->DC = task->DC;
                    pf_task->GV = task->GV;
                    pf_task->GADE = task->GADE;
                    pf_task->GSCID = task->GSCID;
                    pf_task->PSCID = task->PSCID;
                    pf_task->device_id = task->device_id;
                    
                    // 预取专用标志
                    pf_task->walk_ctx.is_prefetch_task = true;
                    pf_task->walk_ctx.prefetch_group_id = group_id;
                    pf_task->walk_ctx.prefetch_idx = d + 1;  // 1-based
                    pf_task->walk_ctx.prefetch_enabled = false;  // 预取任务不再生成预取
                    
                    // 计算预取IOVA
                    uint64_t pf_iova = (task->iova & ~PAGE_MASK_4KB) + (d + 1) * PAGE_SIZE_4KB;
                    pf_task->iova = pf_iova;
                    pf_task->timestamp = task->timestamp;
                    pf_task->state = TASK_PTW_REQ;
                    pf_task->page_sz = PAGE_SIZE_4KB;  // 4KB页
                    
                    // 计算顺序VS L0 PTE的SPA地址
                    // VS L0 PTE在VS L0表中，该表在物理页 vs_l0_spa_ppn
                    // 顺序页面的VPN[0] = main_vpn0 + d + 1
                    uint16_t pf_vpn0 = main_vpn0 + d + 1;
                    uint64_t vs_l0_pte_spa = (vs_l0_spa_ppn * PAGESIZE) + pf_vpn0 * ptesize;
                    
                    // 保存预取IOVA到组
                    group.group_iovas[d + 1] = pf_iova;
                    task->walk_ctx.prefetch_group.group_iovas[d + 1] = pf_iova;
                    
                    // DDR #1: 读取VS L0 PTE（SPA地址，无需G-stage翻译）
                    pf_task->walk_ctx.read_addr = vs_l0_pte_spa;
                    pf_task->walk_ctx.read_size = ptesize;
                    pf_task->walk_ctx.walk_phase = PTW_VS_PTE_READ;
                    pf_task->walk_ctx.ddr_read_count = 1;
                    pf_task->walk_ctx.ptesize = ptesize;
                    pf_task->walk_ctx.vpn[0] = pf_vpn0;
                    // 复制完整vpn数组供后续G-stage使用
                    for (int k = 0; k < 5; k++) pf_task->walk_ctx.vpn[k] = task->walk_ctx.vpn[k];
                    pf_task->walk_ctx.vpn[0] = pf_vpn0;  // 修改VPN[0]为预取值
                    
                    // 注册到active_walks并发送DDR请求
                    ptw_walks_mtx.lock();
                    ptw_active_walks[pf_task->task_id] = pf_task;
                    ptw_walks_mtx.unlock();
                    
                    ddr_req_entry_t pf_req;
                    pf_req.task_id = pf_task->task_id;
                    pf_req.addr = vs_l0_pte_spa;
                    pf_req.size = ptesize;
                    pf_req.is_write = false;
                    pf_req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;
                    ptw_req_ddr_fifo.write(pf_req);
                    
                    printf("[PTW_2STAGE_PF] Spawned prefetch task_id=%u, idx=%u, vs_l0_pte_spa=0x%lx, pf_vpn0=%u, pf_iova=0x%lx\n",
                           pf_task->task_id, d + 1, vs_l0_pte_spa, pf_vpn0, pf_iova);
                    fflush(stdout);
                }
                
                task->walk_ctx.pt_update_count = 1 + D;
                task->walk_ctx.prefetch_group.group_iovas[0] = task->iova & ~PAGE_MASK_4KB;
                
                // [FIX] 非early-spawn路径: 填充pt_updates[0]和设置main_task_done
                // 因为主任务进入walk_complete时prefetch_group_id=0，跳过了group分支
                group.main_task->walk_ctx.pt_updates[0].vs_pte = task->vs_pte;
                group.main_task->walk_ctx.pt_updates[0].g_pte = task->g_pte;
                group.main_task->walk_ctx.pt_updates[0].pa = task->pa;
                group.main_task->walk_ctx.pt_updates[0].iova = task->iova & ~PAGE_MASK_4KB;
                group.main_task->walk_ctx.pt_updates[0].page_sz = task->page_sz;
                group.main_task_done = true;
                
                // 检查是否可以触发monitor（预取任务可能已全部完成）
                if (group.pending_tasks == 0 && !group.completed) {
                    group.completed = true;
                    printf("[PTW_PREFETCH] Group %u ALL COMPLETED (2stage_init, pending=0, main_done)! Trigger.\n", group_id);
                    fflush(stdout);
                    prefetch_group_completed_event.notify(SC_ZERO_TIME);
                }
                
                prefetch_group_mtx.unlock();
                
                // 主任务不进入normal cleanup路径，等待monitor线程释放
                goto skip_walk_cleanup;
            }
            
            // [early spawn] 主任务已通过early spawn初始化预取组，
            // 跳过normal Cleanup路径，由monitor线程统一处理outstanding递减
            // [FIX] 只有当walk_complete=true时才跳转，need_next_ddr时应继续发送DDR请求
            if (task->walk_ctx.is_two_stage_prefetch && walk_complete) {
                ptw_walks_mtx.lock();
                ptw_active_walks.erase(rsp.task_id);
                ptw_walks_mtx.unlock();
                goto skip_walk_cleanup;
            }
                        
            // [FIX-V3] non-early-spawn两阶段预取主任务: 也需跳过normal cleanup
            // 该主任务在walk_complete_handler中已通过prefetch_group_id分支(line 1364)
            // 收集结果并设置main_task_done，预取组由monitor线程统一处理。
            // 若不跳过，会导致:
            //   1) ptw_outstanding_task_count 双重递减(normal cleanup + monitor)
            //   2) task 双重转发到 pt_cache_to_fwd_fifo(normal cleanup + monitor flush)
            //   3) PT Cache 双重更新
            // 注意: early-spawn路径已在上方(line 1605)处理，此处仅捕获non-early-spawn
            if (walk_complete
                && task->walk_ctx.prefetch_group_id != 0
                && !task->walk_ctx.is_prefetch_task
                && !task->walk_ctx.is_two_stage_prefetch) {
                printf("[PTW_RSP] task_id=%u -> non-early-spawn main task, skip normal cleanup (monitor will handle)\n",
                       task->task_id);
                fflush(stdout);
                ptw_walks_mtx.lock();
                ptw_active_walks.erase(rsp.task_id);
                ptw_walks_mtx.unlock();
                goto skip_walk_cleanup;
            }
                        
            // =====================================================================
            // NEW: Burst预取方案 (v2.0)
            // 利用L0页表连续性,1次Burst读代替D次独立walk
            // =====================================================================
            if (task->walk_ctx.prefetch_enabled && task->walk_ctx.prefetch_depth > 0
                && !(task->GV && task->iohgatp.MODE != IOHGATP_Bare)) {
                printf("[PTW_PREFETCH] task_id=%u -> Burst prefetch mode, depth=%u\n",
                       task->task_id, task->walk_ctx.prefetch_depth);
                fflush(stdout);
                
                // [FIX] 主任务Walker Cache更新（Burst预取路径）
                // 主任务VS walk已完成，中间节点结果保存在walker_cache_entries中
                // 预取任务不查询/不更新Walker Cache
                if (PTW_WALKER_CACHE_ENABLED) {
                    iommu::CacheMessage walker_req = task_to_walker_update(task);
                    cache_sub.walker_update_fifo.write(walker_req);
                    
                    printf("[t=%llu ns][PTW_RSP] task_id=%u -> Walker Cache UPDATE (Burst path, kind=%d, L2=%d, L1=%d, L0=%d)\n",
                           (unsigned long long)sc_core::sc_time_stamp().value()/1000,
                           task->task_id,
                           static_cast<int>(walker_req.walker_update_kind),
                           task->walk_ctx.walker_cache_entries.valid_level2,
                           task->walk_ctx.walker_cache_entries.valid_level1,
                           task->walk_ctx.walker_cache_entries.valid_level0);
                    fflush(stdout);
                }
                
                // =================================================================
                // [OPT] 合并Burst路径: leaf+prefetch数据已在read_buf中
                // 无需第2次DDR读，直接解析预取PTEs
                // =================================================================
                if (task->walk_ctx.combined_burst_ready) {
                    // 保存主任务信息到pt_updates[0]
                    task->walk_ctx.pt_updates[0].vs_pte = task->vs_pte;
                    task->walk_ctx.pt_updates[0].g_pte = task->g_pte;
                    task->walk_ctx.pt_updates[0].pa = task->pa;
                    task->walk_ctx.pt_updates[0].iova = task->iova & ~PAGE_MASK_4KB;
                    task->walk_ctx.pt_updates[0].page_sz = task->page_sz;
                    
                    // 计算prefetch参数
                    uint64_t leaf_pt_base = task->walk_ctx.base_addr;  // L0页表基址
                    uint16_t current_vpn0 = task->walk_ctx.vpn[0];
                    uint32_t ptesize = task->walk_ctx.ptesize;
                    uint32_t prefetch_depth = task->walk_ctx.prefetch_depth;
                    uint64_t base_iova = task->iova & ~PAGE_MASK_4KB;
                    
                    // 初始化prefetch_iovas数组
                    for (uint32_t d = 0; d < prefetch_depth; d++) {
                        task->walk_ctx.prefetch_iovas[d] = base_iova + (d + 1) * PAGE_SIZE_4KB;
                    }
                    
                    // Burst参数 (合并读: 从叶子PTE开始，包含1+D个PTE)
                    uint64_t combined_start = leaf_pt_base + current_vpn0 * ptesize;
                    uint32_t combined_size = (1 + prefetch_depth) * ptesize;
                    task->walk_ctx.leaf_pt_base_addr = leaf_pt_base;
                    task->walk_ctx.leaf_ptesize = ptesize;
                    task->walk_ctx.burst_start_addr = combined_start;
                    task->walk_ctx.burst_size = combined_size;
                    task->walk_ctx.prefetch_burst_pending = false;  // 已完成
                    
                    // 解析预取PTEs (从read_buf[ptesize]开始, read_buf[0]=叶子PTE已解析)
                    for (uint32_t d = 0; d < prefetch_depth; d++) {
                        uint64_t pte_raw = 0;
                        memcpy(&pte_raw, &task->walk_ctx.read_buf[(1 + d) * ptesize], ptesize);
                        
                        spte_t vs_pte;
                        vs_pte.raw = pte_raw;
                        gpte_t g_pte;
                        g_pte.raw = pte_raw;
                        
                        uint64_t prefetch_ppn = vs_pte.PPN;
                        uint64_t prefetch_iova = base_iova + (d + 1) * PAGE_SIZE_4KB;
                        uint64_t prefetch_pa = (prefetch_ppn * PAGESIZE) | (prefetch_iova & PAGE_MASK_4KB);
                        
                        task->walk_ctx.pt_updates[d + 1].vs_pte = vs_pte;
                        task->walk_ctx.pt_updates[d + 1].g_pte = g_pte;
                        task->walk_ctx.pt_updates[d + 1].pa = prefetch_pa;
                        task->walk_ctx.pt_updates[d + 1].iova = prefetch_iova;
                        task->walk_ctx.pt_updates[d + 1].page_sz = PAGE_SIZE_4KB;
                        
                        printf("[PTW_COMBINED] PTE[%u]: iova=0x%lx, PPN=0x%lx, pa=0x%lx\n",
                               d+1, prefetch_iova, prefetch_ppn, prefetch_pa);
                    }
                    
                    task->walk_ctx.pt_update_count = 1 + prefetch_depth;
                    
                    // 初始化预取组
                    uint32_t group_id = task->task_id;
                    task->walk_ctx.prefetch_group_id = group_id;
                    task->walk_ctx.prefetch_total = 1 + prefetch_depth;
                    task->walk_ctx.prefetch_idx = 0;
                    
                    task->walk_ctx.prefetch_group.group_iovas[0] = task->iova & ~PAGE_MASK_4KB;
                    for (uint32_t d = 0; d < prefetch_depth; d++) {
                        task->walk_ctx.prefetch_group.group_iovas[1+d] = task->walk_ctx.prefetch_iovas[d];
                    }
                    
                    prefetch_group_mtx.lock();
                    auto& cmb_group = prefetch_groups[group_id];
                    cmb_group.main_task = task;
                    cmb_group.pending_tasks = 0;  // 合并读已完成，无需等待
                    cmb_group.total_tasks = task->walk_ctx.prefetch_total;
                    cmb_group.completed = true;
                    for (uint32_t i = 0; i < cmb_group.total_tasks; i++) {
                        cmb_group.group_iovas[i] = task->walk_ctx.prefetch_group.group_iovas[i];
                    }
                    prefetch_group_mtx.unlock();
                    
                    printf("[PTW_COMBINED] task_id=%u -> Combined burst complete, depth=%u, no extra DDR read\n",
                           task->task_id, prefetch_depth);
                    fflush(stdout);
                    
                    // 直接通知monitor (pending_tasks=0且completed=true)
                    prefetch_group_completed_event.notify(SC_ZERO_TIME);
                    
                    goto skip_walk_cleanup;
                }
                
                // =================================================================
                // 原有Burst路径: 需要第2次DDR读(仅combined_burst_ready=false时)
                // =================================================================
                task->walk_ctx.pt_updates[0].vs_pte = task->vs_pte;
                task->walk_ctx.pt_updates[0].g_pte = task->g_pte;
                task->walk_ctx.pt_updates[0].pa = task->pa;
                task->walk_ctx.pt_updates[0].iova = task->iova;
                task->walk_ctx.pt_updates[0].page_sz = task->page_sz;
                
                // 计算Burst参数
                uint64_t leaf_pt_base = task->walk_ctx.base_addr;  // L0页表基址
                uint16_t current_vpn0 = task->walk_ctx.vpn[0];     // 当前VPN[0]
                uint32_t ptesize = task->walk_ctx.ptesize;         // PTE大小 (Sv39=8字节)
                uint32_t prefetch_depth = task->walk_ctx.prefetch_depth;
                
                // [FIX] 初始化prefetch_iovas数组
                uint64_t base_iova = task->iova & ~PAGE_MASK_4KB;  // 4KB页对齐
                for (uint32_t d = 0; d < prefetch_depth; d++) {
                    task->walk_ctx.prefetch_iovas[d] = base_iova + (d + 1) * PAGE_SIZE_4KB;
                }
                
                // Burst起始地址: L0_base + (VPN[0]+1) * PTE_size
                uint64_t burst_start_addr = leaf_pt_base + (current_vpn0 + 1) * ptesize;
                uint32_t burst_size = prefetch_depth * ptesize;  // D * 8字节
                
                // [边界检查] 确保Burst不跨越4KB页表页
                uint64_t burst_end_addr = burst_start_addr + burst_size - 1;
                
                if ((burst_start_addr & ~PT_PAGE_MASK) != (burst_end_addr & ~PT_PAGE_MASK)) {
                    // Burst跨越页表页边界，截断
                    uint64_t page_boundary = (burst_start_addr | PT_PAGE_MASK) + 1;
                    burst_size = page_boundary - burst_start_addr;
                    prefetch_depth = burst_size / ptesize;
                    
                    printf("[PTW_PREFETCH] Burst truncated at page boundary, depth=%u, size=%u bytes\n",
                           prefetch_depth, burst_size);
                    fflush(stdout);
                    
                    task->walk_ctx.prefetch_depth = prefetch_depth;  // 更新实际预取深度
                }
                
                task->walk_ctx.pt_update_count = 1 + prefetch_depth;  // 1主 + D预取
                
                // 保存leaf页表信息
                task->walk_ctx.leaf_pt_base_addr = leaf_pt_base;
                task->walk_ctx.leaf_ptesize = ptesize;
                task->walk_ctx.burst_start_addr = burst_start_addr;
                task->walk_ctx.burst_size = burst_size;
                task->walk_ctx.prefetch_burst_pending = true;
                
                // 初始化预取组
                uint32_t group_id = task->task_id;
                task->walk_ctx.prefetch_group_id = group_id;
                task->walk_ctx.prefetch_total = 1 + prefetch_depth;
                task->walk_ctx.prefetch_idx = 0;  // 0=主任务
                
                // 保存所有IOVA到组上下文（[P6] 主任务IOVA也需要4KB对齐，与flush中比较逻辑一致）
                task->walk_ctx.prefetch_group.group_iovas[0] = task->iova & ~PAGE_MASK_4KB;
                for (uint32_t d = 0; d < prefetch_depth; d++) {
                    task->walk_ctx.prefetch_group.group_iovas[1+d] = task->walk_ctx.prefetch_iovas[d];
                }
                
                // 注册到预取组追踪表
                prefetch_group_mtx.lock();
                auto& group = prefetch_groups[group_id];
                group.main_task = task;
                group.pending_tasks = 1;  // [FIX] 仅等待1次Burst DDR响应 (主任务已完成)
                group.total_tasks = task->walk_ctx.prefetch_total;
                group.completed = true;  // [FIX] Burst方案: 主任务已完成,设置completed=true
                
                // 复制IOVA列表
                for (uint32_t i = 0; i < group.total_tasks; i++) {
                    group.group_iovas[i] = task->walk_ctx.prefetch_group.group_iovas[i];
                }
                
                prefetch_group_mtx.unlock();
                
                printf("[PTW_PREFETCH] task_id=%u -> Burst DDR: addr=0x%lx, size=%u bytes, depth=%u\n",
                       task->task_id, burst_start_addr, burst_size, prefetch_depth);
                fflush(stdout);
                
                // 发送Burst DDR读请求
                ddr_req_entry_t burst_req;
                burst_req.task_id = task->task_id;
                burst_req.addr = burst_start_addr;
                burst_req.size = burst_size;
                burst_req.is_write = false;
                burst_req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;
                
                task->walk_ctx.ddr_read_count++;
                task->walk_ctx.walk_phase = PTW_PREFETCH_WAIT;
                
                // [STAT] DDR access log - Burst prefetch read
                if (task->walk_ctx.ddr_log_count < 8) {
                    task->walk_ctx.ddr_log_type[task->walk_ctx.ddr_log_count] = 7; // Burst_wait
                    task->walk_ctx.ddr_log_level[task->walk_ctx.ddr_log_count] = 0;
                    task->walk_ctx.ddr_log_addr[task->walk_ctx.ddr_log_count] = burst_start_addr;
                    task->walk_ctx.ddr_log_size[task->walk_ctx.ddr_log_count] = burst_size;
                    task->walk_ctx.ddr_log_count++;
                }
                
                // 注册到active_walks
                ptw_walks_mtx.lock();
                ptw_active_walks[task->task_id] = task;
                ptw_walks_mtx.unlock();
                
                ptw_req_ddr_fifo.write(burst_req);
                
                // 跳过后续处理，等待Burst响应
                goto skip_walk_cleanup;
            }
            
            ptw_walks_mtx.lock();
            ptw_active_walks.erase(rsp.task_id);
            ptw_walks_mtx.unlock();
            ptw_outstanding_task_count--;
            assert(ptw_outstanding_task_count >= 0);  // [FIX-第10条] 防止下溢
            printf("[PTW_STAT] task_id=%u completed, outstanding now=%d\n",
                   task->task_id, ptw_outstanding_task_count);
            fflush(stdout);
            ptw_total_completed++;  // [STAT]
            // [STAT] 记录任务输出时间戳及输出间隔
            {
                const double output_ns = sc_time_stamp().to_seconds() * 1e9;
                printf("[PTW_STAT] task_id=%u output timestamp: %.1f ns\n",
                       task->task_id, output_ns);
                if (ptw_last_output_ns > 0.0) {
                    double output_interval = output_ns - ptw_last_output_ns;
                    ptw_output_interval_total_ns += output_interval;
                    if (output_interval > ptw_output_interval_max_ns)
                        ptw_output_interval_max_ns = output_interval;
                    if (output_interval < ptw_output_interval_min_ns)
                        ptw_output_interval_min_ns = output_interval;
                    ptw_output_interval_count++;
                    ptw_output_interval_values.push_back(output_interval);
                }
                ptw_last_output_ns = output_ns;
            }
            // [STAT] 累加PTW完成统计（walk complete）
            ptw_total_ddr_reads += task->walk_ctx.ddr_read_count;
            
            // [STAT] 更新DDR访问次数最大/最小值
            if (task->walk_ctx.ddr_read_count > ptw_max_ddr_reads)
                ptw_max_ddr_reads = task->walk_ctx.ddr_read_count;
            if (task->walk_ctx.ddr_read_count < ptw_min_ddr_reads)
                ptw_min_ddr_reads = task->walk_ctx.ddr_read_count;
            
            // [STAT] DDR访问次数分布
            ptw_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
            // [STAT] 主任务/预取任务分类计数
            if (task->walk_ctx.is_prefetch_task) {
                ptw_prefetch_task_count++;
                ptw_prefetch_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                ptw_prefetch_total_ddr_reads += task->walk_ctx.ddr_read_count;
            } else {
                ptw_main_task_count++;
                ptw_main_ddr_reads_distribution[task->walk_ctx.ddr_read_count]++;
                ptw_main_total_ddr_reads += task->walk_ctx.ddr_read_count;
            }
            
            // [STAT] 打印每个PTW任务的详细统计
            print_ptw_task_summary(task, "walk_complete");
            
            // [STAT] 计算任务总延时
            { auto _s = ptw_task_start_ns.find(task->task_id);
              if (_s != ptw_task_start_ns.end()) {
                  double task_latency = sc_time_stamp().to_seconds()*1e9 - _s->second;
                  ptw_total_exec_ns += task_latency;
                  ptw_task_latency_values.push_back(task_latency);
                  if (task_latency > ptw_max_task_latency_ns)
                      ptw_max_task_latency_ns = task_latency;
                  if (task_latency < ptw_min_task_latency_ns)
                      ptw_min_task_latency_ns = task_latency;
                  ptw_task_start_ns.erase(_s);
              } }
            ptw_task_completed_event.notify(SC_ZERO_TIME);
            
            // =====================================================================
            // [D=0] 无预取场景: 直接flush Buffer链表并释放entry
            // [FIX] 必须先flush释放Buffer entry, 再写pt_update_fifo
            //       避免pt_update_fifo满时阻塞导致Buffer无法释放的死锁
            // =====================================================================
            bool main_task_flushed_via_chain = false;  // [FIX] 防止主任务双重转发
            if (!task->walk_ctx.prefetch_enabled || task->walk_ctx.prefetch_depth == 0) {
                // D=0: 没有预取组,直接flush主任务的Buffer链表
                uint16_t head_idx = task->dedup_head_index;
                if (head_idx != DEDUP_BUFFER_INVALID_IDX) {
                    // 保存PTE数据到pt_updates[0]（flush_dedup_buffer_chain需要）
                    task->walk_ctx.pt_updates[0].vs_pte = task->vs_pte;
                    task->walk_ctx.pt_updates[0].g_pte = task->g_pte;
                    task->walk_ctx.pt_updates[0].pa = task->pa;
                    task->walk_ctx.pt_updates[0].iova = task->iova & ~PAGE_MASK_4KB;
                    task->walk_ctx.pt_updates[0].page_sz = task->page_sz;
                    
                    // 构造单任务IOVA列表
                    uint64_t main_iova = task->iova & ~PAGE_MASK_4KB;
                    
                    printf("[PTW_NO_PREFETCH] task_id=%u -> Direct flush buffer chain (head=%u, iova=0x%lx)\n",
                           task->task_id, head_idx, main_iova);
                    fflush(stdout);
                    
                    // 调用flush释放Buffer entry并唤醒挂起任务
                    // [FIX] flush_dedup_buffer_chain会将链表中所有任务(含主任务)写入pt_cache_to_fwd_fifo
                    //       因此后续不能再写主任务到fwd_fifo，否则导致double free
                    flush_dedup_buffer_chain(head_idx, task->task_id, task, &main_iova, 1);
                    main_task_flushed_via_chain = true;
                }
            }
            
            // [FIX] PT Cache更新放在flush之后, 确保Buffer entry已释放
            iommu::CacheMessage pt_update_req = task_to_pt_update(task);
            pt_update_req.timestamp = sc_time_stamp();  // [STAT] 记录FIFO写入时刻
            cache_sub.pt_update_fifo.write(pt_update_req);
            
            // =====================================================================
            // Update Walker Cache (如果启用)
            // =====================================================================
            bool walker_cache_enabled = PTW_WALKER_CACHE_ENABLED;  // 从PTW模块参数读取
            if (walker_cache_enabled) {
                // 使用转换函数构造Walker Cache Update请求
                iommu::CacheMessage walker_req = task_to_walker_update(task);
                
                // PTWC_1_2_3 / PTWC_2_3 / PTWC_3 三种更新类型都需要写入 walker_update_fifo
                cache_sub.walker_update_fifo.write(walker_req);
                
                printf("[t=%llu ns][PTW_RSP] task_id=%u -> Walker Cache UPDATE enqueue (iova=0x%lx, kind=%d, L2=%d, L1=%d, L0=%d)\n",
                       (unsigned long long)sc_core::sc_time_stamp().value()/1000,
                       task->task_id, task->iova,
                       static_cast<int>(walker_req.walker_update_kind),
                       task->walk_ctx.walker_cache_entries.valid_level2,
                       task->walk_ctx.walker_cache_entries.valid_level1,
                       task->walk_ctx.walker_cache_entries.valid_level0);
                fflush(stdout);
            }
            // [S2] S2 Walker Cache update (normal walk complete path)
            if (PTW_WALKER_S2_CACHE_ENABLED && PTW_WALKER_CACHE_ENABLED
                && task->GV && task->iohgatp.MODE != IOHGATP_Bare
                && task->iosatp.MODE != IOSATP_Bare) {
                iommu::CacheMessage s2_req = task_to_s2_walker_update(task, task->gpa);
                if (s2_req.walker_update_kind != iommu::WalkerUpdateKind::NONE) {
                    cache_sub.walker_update_fifo.write(s2_req);
                    printf("[t=%llu ns][PTW_RSP] task_id=%u -> S2 Walker Cache UPDATE (gpa=0x%lx, kind=%d)\n",
                           (unsigned long long)sc_core::sc_time_stamp().value()/1000,
                           task->task_id, task->gpa,
                           static_cast<int>(s2_req.walker_update_kind));
                    fflush(stdout);
                }
            }
            
            // Also route to forwarder after PT update
            // [FIX] 如果主任务已通过flush_dedup_buffer_chain转发，则跳过，避免double free
            if (!main_task_flushed_via_chain) {
                pt_cache_to_fwd_fifo.write(task);
            } else {
                printf("[PTW_FWD] task_id=%u -> skip duplicate forward (already flushed via buffer chain)\n",
                       task->task_id);
                fflush(stdout);
            }
            // VA Dedup: 恢复挂起的同页任务
            va_dedup_recover(task, false);
        }
        else if (need_next_ddr) {
            printf("[PTW_RSP] task_id=%u -> need_next_ddr, addr=0x%lx, size=%d, phase=%d, read_count=%u\n",
                   task->task_id, task->walk_ctx.read_addr, task->walk_ctx.read_size,
                   task->walk_ctx.walk_phase, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            ddr_req_entry_t req;
            req.task_id = task->task_id;
            req.addr = task->walk_ctx.read_addr;
            req.size = task->walk_ctx.read_size;
            req.is_write = (task->walk_ctx.walk_phase == PTW_AD_UPDATE);
            req.submit_time_ns = sc_time_stamp().to_seconds() * 1e9;  // [STAT]
            if (req.is_write) {
                memcpy(req.write_data, &task->walk_ctx.ad_pte,
                       task->walk_ctx.ptesize);
            }
            ptw_req_ddr_fifo.write(req);
        }
        
        skip_walk_cleanup:
        ;  // 空语句，用于goto跳转
    }
}

// ============================================================
// NEW: prefetch_group_monitor_thread - 预取组监控线程
// 监控预取组完成状态,执行批量PT Cache更新和Dedup Buffer刷新
// ============================================================
void iommu_top::prefetch_group_monitor_thread() {
    while (true) {
        // 等待预取组完成事件
        wait(prefetch_group_completed_event);
        
        // [FIX-V2] 收集待处理的组数据
        // 两阶段模式: 持锁仅收集数据+标记处理中, 释放锁后再执行flush和FIFO写入
        // 彻底避免FIFO满时持锁阻塞导致PTW响应线程死锁
        struct pending_group_update {
            uint32_t group_id;
            iommu_task_t* main_task;
            iommu::TransStage stage;
            bool sv48;
            bool gstage_x4;
            bool has_fault;
            std::vector<std::pair<uint64_t, iommu::PTData>> batch_updates;
            // [FIX-V2] 保存flush参数, 在无锁阶段执行
            std::vector<uint64_t> group_iovas;
            uint32_t total_tasks;
        };
        std::vector<pending_group_update> deferred_updates;
        
        prefetch_group_mtx.lock();
        
        // 查找已完成的组 (Burst预取方案)
        for (auto it = prefetch_groups.begin(); it != prefetch_groups.end(); ) {
            uint32_t group_id = it->first;
            auto& group = it->second;
            
            // Burst方案: pending_tasks=0 表示Burst DDR响应已处理
            if (group.completed && group.pending_tasks == 0) {
                printf("[PTW_PREFETCH_MONITOR] Processing completed group %u (total=%u, Burst mode)\n",
                       group_id, group.total_tasks);
                fflush(stdout);
                
                auto* main_task = group.main_task;
                
                // [FIX] 安全检查
                if (main_task == nullptr) {
                    printf("[PTW_PREFETCH_MONITOR] ERROR: main_task is nullptr for group %u!\n", group_id);
                    fflush(stdout);
                    it = prefetch_groups.erase(it);
                    continue;
                }
                
                // 动态计算stage
                bool sv48 = (main_task->iosatp.MODE == IOSATP_Sv48);
                bool gstage_x4 = (main_task->iohgatp.MODE == IOHGATP_Sv48x4);
                bool stage1_bare = (main_task->iosatp.MODE == RVI_IOMMU_IOSATP_Bare);
                bool stage2_bare = (main_task->iohgatp.MODE == RVI_IOMMU_IOHGATP_Bare);
                iommu::TransStage stage;
                if (stage1_bare && !stage2_bare) {
                    stage = iommu::TransStage::STAGE2_ONLY;
                } else if (!stage1_bare && !stage2_bare) {
                    stage = iommu::TransStage::STAGE1_AND_2;
                } else {
                    stage = iommu::TransStage::STAGE1_ONLY;
                }
                
                // 收集batch_updates数据
                pending_group_update pgu;
                pgu.group_id = group_id;
                pgu.main_task = main_task;
                pgu.stage = stage;
                pgu.sv48 = sv48;
                pgu.gstage_x4 = gstage_x4;
                pgu.has_fault = group.has_fault;
                
                for (uint32_t i = 0; i < group.total_tasks; i++) {
                    uint64_t iova = group.group_iovas[i];
                    iommu::PTData pt_data;
                    if (main_task->walk_ctx.is_two_stage_prefetch) {
                        pt_data.vs_pte.raw = group.vs_ptes[i].raw;
                        pt_data.g_pte.raw = group.g_ptes[i].raw;
                    } else {
                        pt_data.vs_pte.raw = main_task->walk_ctx.pt_updates[i].vs_pte.raw;
                        pt_data.g_pte.raw = main_task->walk_ctx.pt_updates[i].g_pte.raw;
                    }
                    pt_data.reserved.raw = 0;
                    pt_data.reserved.valid = 1;
                    pt_data.reserved.trans_type = static_cast<uint32_t>(stage);
                    pt_data.reserved.input_page_size = 0;
                    pt_data.reserved.result_page_size = 0;
                    pt_data.reserved.iova_is_va = 1;
                    pt_data.reserved.sv48 = sv48 ? 1U : 0U;
                    pt_data.reserved.gstage_x4 = gstage_x4 ? 1U : 0U;
                    pt_data.reserved.is_ph = 0;
                    pgu.batch_updates.push_back({iova, pt_data});
                    
                    printf("[PTW_PREFETCH_MONITOR] Batch update[%u]: iova=0x%lx, vs_ppn=0x%lx, g_ppn=0x%lx\n",
                           i, iova,
                           main_task->walk_ctx.is_two_stage_prefetch ? group.vs_ptes[i].PPN : main_task->walk_ctx.pt_updates[i].vs_pte.PPN,
                           main_task->walk_ctx.is_two_stage_prefetch ? (uint64_t)group.g_ptes[i].PPN : (uint64_t)main_task->walk_ctx.pt_updates[i].g_pte.PPN);
                }
                fflush(stdout);
                
                // [FIX-V2] 保存flush参数到deferred结构, 在无锁阶段执行
                pgu.total_tasks = group.total_tasks;
                for (uint32_t i = 0; i < group.total_tasks; i++) {
                    pgu.group_iovas.push_back(group.group_iovas[i]);
                }
                
                // [STAT] phase_tracker
                int saved_phase = cache_sub.pt_cache().current_phase();
                cache_sub.pt_cache().set_current_phase(2);
                for (uint32_t i = 0; i < group.total_tasks; i++) {
                    uint64_t iova = group.group_iovas[i];
                    if (iova == 0) continue;
                    printf("[PTW_PREFETCH_MONITOR] Group %u, iova=0x%lx: will scan buffer for matching entries\n",
                           group_id, iova);
                }
                fflush(stdout);
                cache_sub.pt_cache().set_current_phase(saved_phase);
                
                // PTW任务统计
                ptw_outstanding_task_count--;
                assert(ptw_outstanding_task_count >= 0);  // [FIX-第10条] 防止下溢
                ptw_total_completed++;
                // [STAT] 记录任务输出时间戳及输出间隔（burst_complete路径）
                {
                    const double output_ns = sc_time_stamp().to_seconds() * 1e9;
                    printf("[PTW_STAT] task_id=%u output timestamp: %.1f ns\n",
                           main_task->task_id, output_ns);
                    if (ptw_last_output_ns > 0.0) {
                        double output_interval = output_ns - ptw_last_output_ns;
                        ptw_output_interval_total_ns += output_interval;
                        if (output_interval > ptw_output_interval_max_ns)
                            ptw_output_interval_max_ns = output_interval;
                        if (output_interval < ptw_output_interval_min_ns)
                            ptw_output_interval_min_ns = output_interval;
                        ptw_output_interval_count++;
                        ptw_output_interval_values.push_back(output_interval);
                    }
                    ptw_last_output_ns = output_ns;
                }
                ptw_total_ddr_reads += main_task->walk_ctx.ddr_read_count;
                if (main_task->walk_ctx.ddr_read_count > ptw_max_ddr_reads)
                    ptw_max_ddr_reads = main_task->walk_ctx.ddr_read_count;
                if (main_task->walk_ctx.ddr_read_count < ptw_min_ddr_reads)
                    ptw_min_ddr_reads = main_task->walk_ctx.ddr_read_count;
                // [STAT] DDR访问次数分布
                ptw_ddr_reads_distribution[main_task->walk_ctx.ddr_read_count]++;
                // [STAT] 主任务/预取任务分类计数
                if (main_task->walk_ctx.is_two_stage_prefetch)
                    ptw_main_task_count++;  // two_stage_prefetch主任务算主任务
                else
                    ptw_main_task_count++;
                ptw_main_ddr_reads_distribution[main_task->walk_ctx.ddr_read_count]++;
                ptw_main_total_ddr_reads += main_task->walk_ctx.ddr_read_count;
                print_ptw_task_summary(main_task, main_task->walk_ctx.is_two_stage_prefetch ? "two_stage_pf_complete" : "burst_complete");
                { auto _s = ptw_task_start_ns.find(main_task->task_id);
                  if (_s != ptw_task_start_ns.end()) {
                      double task_latency = sc_time_stamp().to_seconds()*1e9 - _s->second;
                      ptw_total_exec_ns += task_latency;
                      ptw_task_latency_values.push_back(task_latency);
                      if (task_latency > ptw_max_task_latency_ns)
                          ptw_max_task_latency_ns = task_latency;
                      if (task_latency < ptw_min_task_latency_ns)
                          ptw_min_task_latency_ns = task_latency;
                      ptw_task_start_ns.erase(_s);
                  } }
                ptw_task_completed_event.notify(SC_ZERO_TIME);
                
                // [STAT] 任务组执行时间统计 (1主+D预取为一组)
                // 组执行时间 = 当前时刻 - 组起始时刻(主任务进入PTW)
                if (group.group_start_ns > 0.0) {
                    double group_exec_ns = sc_time_stamp().to_seconds() * 1e9 - group.group_start_ns;
                    ptw_group_exec_total_ns += group_exec_ns;
                    ptw_group_exec_count++;
                    ptw_group_exec_values.push_back(group_exec_ns);
                    if (group_exec_ns > ptw_group_exec_max_ns)
                        ptw_group_exec_max_ns = group_exec_ns;
                    if (group_exec_ns < ptw_group_exec_min_ns)
                        ptw_group_exec_min_ns = group_exec_ns;
                    printf("[PTW_GRP_STAT] group=%u, exec_time=%.1f ns (%.3f us), total_tasks=%u\n",
                           group_id, group_exec_ns, group_exec_ns / 1000.0, group.total_tasks);
                }
                
                // [FIX-V2] Buffer刷新移到无锁阶段执行（避免FIFO满时持锁阻塞）
                
                // 删除组记录（持锁期间完成）
                deferred_updates.push_back(std::move(pgu));
                it = prefetch_groups.erase(it);
            } else {
                ++it;
            }
        }
        
        // ========== 释放mutex ==========
        prefetch_group_mtx.unlock();
        
        // ========== [FIX-V2] 无锁阶段: 先flush Buffer, 再更新PT Cache ==========
        // flush必须在pt_update之前, 确保Buffer entry先释放
        // [MONITOR] 记录无锁阶段开始时间
        const double unlock_phase_start_ns = sc_time_stamp().to_seconds() * 1e9;
        
        for (auto& pgu : deferred_updates) {
            // [MONITOR] 记录单个group处理开始时间
            const double group_process_start_ns = sc_time_stamp().to_seconds() * 1e9;
            
            // Step 1: 扫描Buffer刷新entry（无锁状态, FIFO满时阻塞不会死锁）
            uint32_t flushed_iova_count = 0;
            for (uint32_t i = 0; i < pgu.total_tasks; i++) {
                uint64_t iova = pgu.group_iovas[i];
                if (iova == 0) continue;
                flush_dedup_buffer_by_iova(iova, pgu.group_id, pgu.main_task,
                                          pgu.group_iovas.data(), pgu.total_tasks);
                flushed_iova_count++;
            }
            printf("[PTW_PREFETCH_MONITOR] Group %u completed, scanned %u IOVAs for buffer flush.\n",
                   pgu.group_id, flushed_iova_count);
            fflush(stdout);
            
            // Step 2: PT Cache处理（无锁状态下）
            // [FIX-第9条] faulted组需要显式失效PT Cache占位条目，释放buffer任务
            // 正常组: 更新PT Cache（替换placeholder CL）
            // faulted组: 失效PT Cache占位条目（删除placeholder CL），防止后续命中错误的缓存
            if (pgu.has_fault) {
                // [FAULT路径] 显式失效PT Cache占位条目
                for (const auto& [iova, pt_data] : pgu.batch_updates) {
                    if (iova == 0) continue;  // 跳过空IOVA
                    iommu::CacheMessage inv_msg;
                    inv_msg.msg_type = iommu::CacheMsgType::PT_INVALIDATE;
                    inv_msg.task_id = pgu.main_task->task_id;
                    inv_msg.gscid = pgu.main_task->GSCID;
                    inv_msg.pscid = pgu.main_task->PSCID;
                    inv_msg.iova = iova;
                    inv_msg.invalidate_mode = iommu::CacheInvalidateMode::PRECISE;
                    inv_msg.timestamp = sc_time_stamp();
                    cache_sub.pt_invalidate_fifo.write(inv_msg);
                    printf("[PTW_PREFETCH_MONITOR][FAULT] Group %u: invalidate PT Cache placeholder iova=0x%lx\n",
                           pgu.group_id, iova);
                    fflush(stdout);
                }
            } else {
                // [正常路径] 更新PT Cache（替换placeholder CL）
                for (const auto& [iova, pt_data] : pgu.batch_updates) {
                    iommu::CacheMessage update_msg;
                    update_msg.msg_type = iommu::CacheMsgType::PT_UPDATE;
                    update_msg.task_id = pgu.main_task->task_id;
                    update_msg.gscid = pgu.main_task->GSCID;
                    update_msg.pscid = pgu.main_task->PSCID;
                    update_msg.iova = iova;
                    update_msg.stage = pgu.stage;
                    update_msg.pt_sv48 = pgu.sv48;
                    update_msg.pt_gstage_x4 = pgu.gstage_x4;
                    update_msg.pt_data = pt_data;
                    update_msg.from_prefetch = false;
                    update_msg.timestamp = sc_time_stamp();
                    
                    // [MONITOR] 测量PT UPDATE FIFO写入阻塞时间
                    const double pt_upd_start_ns = sc_time_stamp().to_seconds() * 1e9;
                    const int pt_fifo_avail = cache_sub.pt_update_fifo.num_available();
                    cache_sub.pt_update_fifo.write(update_msg);
                    const double pt_upd_end_ns = sc_time_stamp().to_seconds() * 1e9;
                    const double pt_upd_delay_ns = pt_upd_end_ns - pt_upd_start_ns;
                    
                    monitor_pt_update_total_count++;
                    monitor_pt_update_total_ns += pt_upd_delay_ns;
                    if (pt_upd_delay_ns > monitor_pt_update_max_ns)
                        monitor_pt_update_max_ns = pt_upd_delay_ns;
                    
                    if (pt_upd_delay_ns > 0.0) {
                        monitor_pt_update_fifo_block_count++;
                        monitor_pt_update_fifo_block_ns += pt_upd_delay_ns;
                        printf("[MONITOR_PT_UPD] FIFO BLOCK: group=%u, iova=0x%lx, delay=%.1f ns, fifo_avail=%d\n",
                               pgu.group_id, iova, pt_upd_delay_ns, pt_fifo_avail);
                        fflush(stdout);
                    }
                }
            }
            printf("[PTW_PREFETCH_MONITOR] Group %u: %s PT Cache (%zu entries, has_fault=%d)\n",
                   pgu.group_id, pgu.has_fault ? "INVALIDATE" : "UPDATE",
                   pgu.batch_updates.size(), pgu.has_fault);
            fflush(stdout);
            
            // [MONITOR] 记录单个group处理结束时间
            const double group_process_end_ns = sc_time_stamp().to_seconds() * 1e9;
            const double group_process_ns = group_process_end_ns - group_process_start_ns;
            monitor_group_total_count++;
            monitor_group_total_process_ns += group_process_ns;
            if (group_process_ns > monitor_group_max_process_ns)
                monitor_group_max_process_ns = group_process_ns;
            printf("[MONITOR_GRP] group_id=%u, process_time=%.1f ns (flush+PT_UPDATE)\n",
                   pgu.group_id, group_process_ns);
            fflush(stdout);
        }
        
        // [MONITOR] 记录无锁阶段总耗时
        const double unlock_phase_end_ns = sc_time_stamp().to_seconds() * 1e9;
        printf("[MONITOR_PHASE] unlock_phase_total=%.1f ns, groups_processed=%zu\n",
               unlock_phase_end_ns - unlock_phase_start_ns, deferred_updates.size());
        fflush(stdout);
        
        // ========== [FIX-V3] 防止event丢失导致Monitor永久阻塞 ==========
        // sc_event是edge-triggered: 在Monitor处理期间(持锁收集+无锁flush/PT Cache更新),
        // 其他group完成时的notify会丢失(Monitor不在wait状态).
        // 处理完当前batch后, 检查是否还有completed group遗留, 如有则重新notify.
        {
            bool has_completed = false;
            prefetch_group_mtx.lock();
            for (auto& [gid, g] : prefetch_groups) {
                if (g.completed && g.pending_tasks == 0) {
                    has_completed = true;
                    break;
                }
            }
            prefetch_group_mtx.unlock();
            if (has_completed) {
                printf("[PTW_PREFETCH_MONITOR] Re-notify: completed groups still pending\n");
                fflush(stdout);
                prefetch_group_completed_event.notify(SC_ZERO_TIME);
            }
        }
    }
}
