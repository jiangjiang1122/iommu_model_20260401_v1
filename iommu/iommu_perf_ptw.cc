//
// IOMMU 性能模型 - PTW (Page Table Walker) 模块实现
// 按照 SPEC v4 第 5.8 节定义实现
// 参考：iommu_two_stage_trans.cc, iommu_second_stage_trans.cc
// 包含 Walker Cache (PTWc_1/2/3) 逻辑
//

#include "iommu_top.hh"
#include "iommu_perf_model.hh"

// ============================================================================
// PTW 线程 1: 请求（含 Walker Cache 查询）
// ============================================================================

void iommu_top::ptw_req_thread() {
    printf("[PTW-1] Request thread started\n");
    
    while (true) {
        // 优先处理 Cache 失效命令
        cache_inv_cmd_t cmd;
        while (cq_to_cache_inv_fifo.num_available() > 0) {
            cq_to_cache_inv_fifo.read(cmd);
            
#ifdef DEBUG_PTW
            printf("[PTW-1] Processing cache invalidation cmd, type=%d\n", cmd.type);
#endif
            
            if (cmd.type == INV_VMA) {
                // 失效 Walker Cache
                if (cmd.gv && cmd.pscidv && cmd.av) {
                    walker_cache.invalidate_by_iova(cmd.gscid, cmd.pscid, cmd.addr);
#ifdef DEBUG_PTW
                    printf("[PTW-1] Walker Cache invalidate by IOVA (gscid=%d, pscid=%d, addr=0x%lx)\n",
                           cmd.gscid, cmd.pscid, cmd.addr);
#endif
                } else if (cmd.gv && cmd.pscidv) {
                    walker_cache.invalidate_by_gscid_pscid(cmd.gscid, cmd.pscid);
#ifdef DEBUG_PTW
                    printf("[PTW-1] Walker Cache invalidate by GSCID/PSCID\n");
#endif
                } else if (cmd.gv) {
                    walker_cache.invalidate_by_gscid(cmd.gscid);
#ifdef DEBUG_PTW
                    printf("[PTW-1] Walker Cache invalidate by GSCID\n");
#endif
                } else {
                    walker_cache.invalidate_all();
#ifdef DEBUG_PTW
                    printf("[PTW-1] Walker Cache invalidate all\n");
#endif
                }
            } else if (cmd.type == INV_DDT) {
                walker_cache.invalidate_by_gscid(cmd.gscid);
#ifdef DEBUG_PTW
                printf("[PTW-1] Walker Cache invalidate by GSCID (DDT inv)\n");
#endif
            }
        }
        
        // 处理页表 walk 请求
        iommu_task_t* task = nullptr;
        pt_cache_to_ptw_fifo.read(task);
        
#ifdef DEBUG_PTW
        printf("[PTW-1] Task %d: PTW request, iova=0x%lx, iosatp.MODE=%d, iohgatp.MODE=%d\n",
               task->task_id, task->iova, task->iosatp.MODE, task->iohgatp.MODE);
#endif
        
        // 检查 Bare 模式
        if (task->iosatp.MODE == IOSATP_Bare && task->iohgatp.MODE == IOHGATP_Bare) {
            // 两级都是 Bare，直接 IOVA=PA
#ifdef DEBUG_PTW
            printf("[PTW-1] Task %d: Bare mode, iova=pa\n", task->task_id);
#endif
            task->pa = task->iova;
            task->page_sz = PAGESIZE;
            task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = 1;
            task->vs_pte.PBMT = PMA;
            task->g_pte.X = task->g_pte.W = task->g_pte.R = 1;
            task->state = TASK_PT_WALK_DONE;
            ptw_to_pt_cache_fifo.write(task);
            continue;
        }
        
        // 查询 Walker Cache
        uint64_t ppn = 0;
        uint8_t addr_mode = (task->iohgatp.MODE != IOHGATP_Bare) ? task->iohgatp.MODE : task->iosatp.MODE;
        
        int wc_result = walker_cache.lookup(task->GSCID, task->PSCID, task->iova, addr_mode, &ppn);
        
        if (wc_result > 0) {
            // Walker Cache 命中
            task->wc_hit_level = wc_result;
            task->wc_hit_ppn = ppn;
            
#ifdef DEBUG_PTW
            printf("[PTW-1] Task %d: Walker Cache HIT at level %d, ppn=0x%lx\n", 
                   task->task_id, wc_result, ppn);
#endif
            
            // 从命中的 PPN 开始 walk
            // TODO: 实现从中间层级开始的页表 walk
            // 简化：直接使用命中的 PPN 作为最终 PPN
            task->pa = ppn << 12 | (task->iova & 0xFFF);
            task->page_sz = PAGESIZE;
            task->state = TASK_PT_WALK_DONE;
            ptw_to_pt_cache_fifo.write(task);
            
        } else {
            // Walker Cache 未命中，从根开始完整 walk
            task->wc_hit_level = -1;
            
#ifdef DEBUG_PTW
            printf("[PTW-1] Task %d: Walker Cache MISS, starting full walk\n", task->task_id);
#endif
            
            // 根据 IOSATP 和 IOHGATP 模式决定是 VS-stage 还是 G-stage 翻译
            if (task->iohgatp.MODE != IOHGATP_Bare && task->iosatp.MODE == IOSATP_Bare) {
                // G-stage translation only
                task->walk_ctx.walk_type = WALK_G_PT;
                
                // 计算 VPN 索引 (基于 IOHGATP 模式)
                uint16_t vpn[5];
                calculate_vpn_indices(task->gpa, task->iohgatp.MODE, vpn);
                
                uint8_t max_levels = get_page_table_levels(task->iohgatp.MODE);
                if (max_levels == 0) {
                    max_levels = 1; // 默认为 1 级
                }
                
                // 初始化 walk 上下文
                task->walk_ctx.level = max_levels - 1;
                task->walk_ctx.max_levels = max_levels;
                task->walk_ctx.g_a = task->gpa;
                
                // 计算根 PTE 地址
                uint64_t root_ppn = task->iohgatp.PPN;
                uint64_t pte_addr = root_ppn * 4096;
                if (max_levels > 1) {
                    pte_addr += vpn[max_levels - 1] * 8;
                }
                
                task->walk_ctx.read_addr = pte_addr;
                task->walk_ctx.read_size = 8;
                
                // 分配 AXI ID
                uint16_t axi_id = axi_id_alloc.alloc_id();
                task->current_axi_id = axi_id;
                
#ifdef DEBUG_PTW
                printf("[PTW-1] Task %d: G-stage translation, reading PTE at level %d, addr=0x%lx\n", 
                       task->task_id, task->walk_ctx.level, pte_addr);
#endif
                
                // 发起非阻塞 DDR 读
                send_ddr_nb_read(pte_addr, 8, axi_id, WALK_G_PT, task);
                
            } else if (task->iosatp.MODE != IOSATP_Bare) {
                // VS-stage translation
                task->walk_ctx.walk_type = WALK_VS_PT;
                
                // 计算 VPN 索引
                uint16_t vpn[5];
                calculate_vpn_indices(task->iova, task->iosatp.MODE, vpn);
                
                uint8_t max_levels = get_page_table_levels(task->iosatp.MODE);
                if (max_levels == 0) {
                    max_levels = get_page_table_levels(task->iohgatp.MODE);
                    if (max_levels == 0) {
                        max_levels = 1; // 默认为 1 级
                    }
                }
                
                // 初始化 walk 上下文
                task->walk_ctx.level = max_levels - 1;
                task->walk_ctx.max_levels = max_levels;
                task->walk_ctx.vs_a = task->iova;
                
                // 计算根 PTE 地址
                uint64_t root_ppn = task->iosatp.PPN;
                uint64_t pte_addr = root_ppn * 4096;
                if (max_levels > 1) {
                    pte_addr += vpn[max_levels - 1] * 8;
                }
                
                task->walk_ctx.read_addr = pte_addr;
                task->walk_ctx.read_size = 8;
                
                // 分配 AXI ID
                uint16_t axi_id = axi_id_alloc.alloc_id();
                task->current_axi_id = axi_id;
                
#ifdef DEBUG_PTW
                printf("[PTW-1] Task %d: VS-stage translation, reading PTE at level %d, addr=0x%lx\n", 
                       task->task_id, task->walk_ctx.level, pte_addr);
#endif
                
                // 发起非阻塞 DDR 读
                send_ddr_nb_read(pte_addr, 8, axi_id, WALK_VS_PT, task);
                
            } else {
                // Both stages are Bare, direct mapping
#ifdef DEBUG_PTW
                printf("[PTW-1] Task %d: Both stages Bare, iova=pa\n", task->task_id);
#endif
                task->pa = task->iova;
                task->page_sz = PAGESIZE;
                task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = 1;
                task->vs_pte.PBMT = PMA;
                task->g_pte.X = task->g_pte.W = task->g_pte.R = 1;
                task->state = TASK_PT_WALK_DONE;
                ptw_to_pt_cache_fifo.write(task);
            }
        }
        
        wait(PTW_DELAY_PER_ACCESS, SC_NS);
    }
}

// ============================================================================
// PTW 线程 2: 响应
// ============================================================================

void iommu_top::ptw_rsp_thread() {
    printf("[PTW-2] Response thread started\n");
    
    while (true) {
        // 等待 DDR 响应事件
        wait(ptw_rsp_evt);
        
        while (!ptw_rsp_queue.empty()) {
            ddr_response_t rsp = ptw_rsp_queue.front();
            ptw_rsp_queue.pop();
            
            // 从 outstanding 表恢复 task
            auto it = ddr_outstanding_table.find(rsp.axi_id);
            if (it == ddr_outstanding_table.end()) {
                printf("[PTW-2] ERROR: AXI ID %d not found\n", rsp.axi_id);
                continue;
            }
            
            iommu_task_t* task = it->second.task;
            
            // 从 outstanding 表移除并释放 AXI ID
            ddr_outstanding_table.erase(it);
            axi_id_alloc.free_id(rsp.axi_id);
            
            if (rsp.status != 0) {
                // DDR 错误
                printf("[PTW-2] Task %d: DDR error\n", task->task_id);
                task->state = TASK_FAULT;
                task->cause = 257;
                fault_fifo.write(task);
                continue;
            }
            
            // 解析 PTE
            uint64_t pte_raw = *((uint64_t*)rsp.data);
            uint64_t pte_ppn = get_pte_ppn(pte_raw);
            bool pte_valid = is_pte_valid(pte_raw);
            bool pte_leaf = is_pte_leaf(pte_raw);
            
#ifdef DEBUG_PTW
            printf("[PTW-2] Task %d: PTE=0x%lx, V=%d, Leaf=%d, level=%d\n", 
                   task->task_id, pte_raw, pte_valid, pte_leaf, task->walk_ctx.level);
#endif
            
            if (!pte_valid) {
                // PTE 无效
                task->state = TASK_FAULT;
                task->cause = 262; // Page fault
                fault_fifo.write(task);
                continue;
            }
            
            if (!pte_leaf) {
                // 非叶节点
                // 更新 Walker Cache
                uint8_t completed_levels = task->walk_ctx.max_levels - task->walk_ctx.level;
                
                uint16_t vpn[4];
                calculate_vpn_indices(task->iova, task->iosatp.MODE, vpn);
                
                if (completed_levels == 1) {
                    // 完成最高层，更新 PTWc_1
                    walker_cache.update_ptwc1(task->GSCID, task->PSCID, vpn[3], pte_ppn, task->iosatp.MODE);
#ifdef DEBUG_PTW
                    printf("[PTW-2] Updated PTWc_1 with vpn[3]=%d, ppn=0x%lx\n", vpn[3], pte_ppn);
#endif
                } else if (completed_levels == 2) {
                    // 完成第二层，更新 PTWc_2
                    walker_cache.update_ptwc2(task->GSCID, task->PSCID, vpn[3], vpn[2], pte_ppn, task->iosatp.MODE);
#ifdef DEBUG_PTW
                    printf("[PTW-2] Updated PTWc_2 with vpn[3]=%d, vpn[2]=%d, ppn=0x%lx\n", vpn[3], vpn[2], pte_ppn);
#endif
                } else if (completed_levels == 3 && task->iosatp.MODE == IOSATP_Sv48) {
                    // 完成第三层（仅 Sv48），更新 PTWc_3
                    walker_cache.update_ptwc3(task->GSCID, task->PSCID, vpn[3], vpn[2], vpn[1], pte_ppn, task->iosatp.MODE);
#ifdef DEBUG_PTW
                    printf("[PTW-2] Updated PTWc_3 with vpn[3]=%d, vpn[2]=%d, vpn[1]=%d, ppn=0x%lx\n", 
                           vpn[3], vpn[2], vpn[1], pte_ppn);
#endif
                }
                
                // 继续下一级 walk
                task->walk_ctx.level--;
                
                if (task->walk_ctx.level >= 0) {
                    // 计算下一级 PTE 地址
                    uint64_t next_pte_addr = pte_ppn * 4096 + vpn[task->walk_ctx.level] * 8;
                    
                    // 分配 AXI ID
                    uint16_t new_axi_id = axi_id_alloc.alloc_id();
                    task->current_axi_id = new_axi_id;
                    
#ifdef DEBUG_PTW
                    printf("[PTW-2] Task %d: Reading next PTE at level %d, addr=0x%lx\n", 
                           task->task_id, task->walk_ctx.level, next_pte_addr);
#endif
                    
                    // 发起 DDR 读
                    send_ddr_nb_read(next_pte_addr, 8, new_axi_id, WALK_VS_PT, task);
                } else {
                    // 不应该到这里
                    printf("[PTW-2] ERROR: level < 0 for non-leaf PTE\n");
                    task->state = TASK_FAULT;
                    task->cause = 262;
                    fault_fifo.write(task);
                }
                
            } else {
                // 叶节点
                // 权限检查（简化）
                uint8_t pte_r = (pte_raw >> 1) & 0x1;
                uint8_t pte_w = (pte_raw >> 2) & 0x1;
                uint8_t pte_x = (pte_raw >> 3) & 0x1;
                
                if ((task->is_read && !pte_r) || (task->is_write && !pte_w) || (task->is_exec && !pte_x)) {
                    task->state = TASK_FAULT;
                    task->cause = 264; // Page access permission fault
                    fault_fifo.write(task);
                    continue;
                }
                
                // 计算物理地址
                uint8_t level = task->walk_ctx.level;
                uint64_t page_size = PAGESIZE; // 默认页面大小
                uint64_t page_offset;
                
                // 根据页表模式和层级计算页面大小
                if (task->iosatp.MODE == IOSATP_Sv39) {
                    if (level == 0) page_size = 4096;           // 4KB page
                    else if (level == 1) page_size = 2048*1024; // 2MB page
                    else if (level == 2) page_size = 1024*1024*1024; // 1GB page
                } else if (task->iosatp.MODE == IOSATP_Sv48) {
                    if (level == 0) page_size = 4096;           // 4KB page
                    else if (level == 1) page_size = 2048*1024; // 2MB page
                    else if (level == 2) page_size = 1024*1024*1024; // 1GB page
                    else if (level == 3) page_size = 512*1024*1024*1024; // 512GB page
                } else if (task->iosatp.MODE == IOSATP_Sv57) {
                    for (int i = 0; i <= level; i++) {
                        page_size *= 512;
                    }
                } else if (task->iosatp.MODE == IOSATP_Sv32) {
                    if (level == 0) page_size = 4096;           // 4KB page
                    else if (level == 1) page_size = 4096*1024; // 4MB page
                }
                
                // 检查PTE是否为NAPOT（自然对齐页表项）类型
                uint8_t pte_n = (pte_raw >> 63) & 0x1; // N bit
                uint64_t pte_ppn = get_pte_ppn(pte_raw);
                
                if (pte_n) {
                    // NAPOT处理：页表项的PPN需要特殊处理
                    if (level == 0 && ((pte_ppn & 0xF) == 0x8)) { // 有效的NAPOT编码
                        // 对于NAPOT页面，需要调整PPN
                        uint64_t napot_bits = 4; // 64KB NAPOT区域
                        uint64_t napot_mask = (1ULL << napot_bits) - 1;
                        uint64_t iova_lower = (task->iova / PAGESIZE) & napot_mask;
                        pte_ppn = (pte_ppn & ~napot_mask) | iova_lower;
                    }
                }
                
                // 处理超级页面对齐检查
                if (level > 0) {
                    // 检查是否为对齐的超级页面
                    uint16_t vpn[5];
                    calculate_vpn_indices(task->iova, task->iosatp.MODE, vpn);
                    
                    // 验证PTE PPN的低位是否为0（超级页面对齐检查）
                    uint64_t ppn_mask = (1ULL << level) - 1;
                    if ((pte_ppn & ppn_mask) != 0) {
                        // 超级页面未对齐，报告页面故障
                        task->state = TASK_FAULT;
                        task->cause = 13; // Read page fault
                        fault_fifo.write(task);
                        continue;
                    }
                }
                
                page_offset = task->iova & (page_size - 1);
                task->pa = (pte_ppn << 12) | page_offset;
                task->page_sz = page_size;
                task->vs_pte.raw = pte_raw;
                
                // 处理PDTV、DPE等特定条件
                if (task->DC.tc.PDTV == 1) {
                    // 当PDTV=1时，确保进程ID有效
                    if (task->pid_valid == 0 && task->DC.tc.DPE == 0) {
                        // 如果DPE=0且无有效PID，则报告故障
                        task->state = TASK_FAULT;
                        task->cause = 260; // Transaction type disallowed
                        fault_fifo.write(task);
                        continue;
                    }
                    
                    // 检查进程ID是否超出PDTP.MODE指定范围
                    if (task->DC.fsc.pdtp.MODE == PD8 && task->process_id > 0xFF) {
                        task->state = TASK_FAULT;
                        task->cause = 260; // Transaction type disallowed
                        fault_fifo.write(task);
                        continue;
                    }
                    
                    if (task->DC.fsc.pdtp.MODE == PD17 && task->process_id > 0x1FFFF) {
                        task->state = TASK_FAULT;
                        task->cause = 260; // Transaction type disallowed
                        fault_fifo.write(task);
                        continue;
                    }
                }
                
#ifdef DEBUG_PTW
                printf("[PTW-2] Task %d: VS-stage leaf PTE, pa=0x%lx, page_sz=%lu\n", 
                       task->task_id, task->pa, page_size);
#endif
                
                // 检查是否需要 G-stage 翻译
                if (task->iohgatp.MODE != IOHGATP_Bare) {
                    // 需要 G-stage 翻译，保存当前 VS-stage 结果并启动 G-stage 翻译
                    task->gpa = task->pa; // 临时设置，实际需要进行 G-stage 翻译
                    
                    // 如果当前是 VS-stage，切换到 G-stage 翻译
                    if (task->walk_ctx.walk_type == WALK_VS_PT) {
                        // 保存 VS-stage 上下文
                        task->saved_vs_walk = task->walk_ctx;
                        
                        // 设置 G-stage 翻译参数
                        task->walk_ctx.walk_type = WALK_G_PT;
                        task->walk_ctx.level = get_page_table_levels(task->iohgatp.MODE) - 1;
                        task->walk_ctx.max_levels = get_page_table_levels(task->iohgatp.MODE);
                        task->walk_ctx.g_a = task->gpa;
                        
                        // 计算 G-stage VPN 索引
                        uint16_t gvpn[5];
                        calculate_vpn_indices(task->gpa, task->iohgatp.MODE, gvpn);
                        
                        // 计算 G-stage PTE 地址
                        uint64_t root_ppn = task->iohgatp.PPN;
                        uint64_t gpte_addr = root_ppn * 4096;
                        if (task->walk_ctx.max_levels > 1) {
                            gpte_addr += gvpn[task->walk_ctx.level] * 8;
                        }
                        
                        task->walk_ctx.read_addr = gpte_addr;
                        task->walk_ctx.read_size = 8;
                        
                        // 分配 AXI ID 并发起 G-stage DDR 读
                        uint16_t new_axi_id = axi_id_alloc.alloc_id();
                        task->current_axi_id = new_axi_id;
                        
#ifdef DEBUG_PTW
                        printf("[PTW-2] Task %d: Switching to G-stage translation, reading PTE at level %d, addr=0x%lx\n", 
                               task->task_id, task->walk_ctx.level, gpte_addr);
#endif
                        
                        // 发起 G-stage DDR 读
                        send_ddr_nb_read(gpte_addr, 8, new_axi_id, WALK_G_PT, task);
                        continue; // 继续处理当前任务的 G-stage 翻译
                    }
                    
                    // 如果已经是 G-stage，完成两阶段翻译
                    task->g_pte.raw = pte_raw;
                    task->pa = task->gpa; // 在嵌套翻译中，最终物理地址来自 G-stage
                } else {
                    // 不需要 G-stage 翻译，完成 VS-stage
                    task->g_pte.raw = 0; // G-stage PTE 为空
                    task->g_pte.V = task->g_pte.R = task->g_pte.W = task->g_pte.X = 1; // 设置默认权限
                }
                
                // 处理虚拟中断文件重叠
                handle_virtual_interrupt_file_overlap(&task->DC, task->gpa, &task->gst_page_sz);
                
                // 完成
                task->state = TASK_PT_WALK_DONE;
                ptw_to_pt_cache_fifo.write(task);
            }
        }
    }
}
