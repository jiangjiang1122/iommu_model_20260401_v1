//
// IOMMU 性能模型 - Collector 模块实现
// 按照 SPEC v4 第 5.2 节定义实现
//

#include "iommu_top.hh"
#include "iommu_perf_model.hh"

// ============================================================================
// Collector 线程 1: Cache 查询结果处理
// ============================================================================

void iommu_top::collector_cache_lookup_result_thread() {
    printf("[Collector-1] Cache lookup result thread started\n");
    
    // 内部维护 pending_tasks 表 (task_id -> task)
    std::map<uint32_t, iommu_task_t*> pending_tasks;

    while (true) {
        iommu_task_t* task = nullptr;
        bool got_data = false;
        
        // 非阻塞检查三个 FIFO
        if (parser_to_collector_fifo.num_available() > 0) {
            parser_to_collector_fifo.read(task);
            pending_tasks[task->task_id] = task;
            got_data = true;
#ifdef DEBUG_COLLECTOR
            printf("[Collector-1] Received task %d from parser\n", task->task_id);
#endif
        }
        
        if (dc_cache_to_collector_fifo.num_available() > 0) {
            dc_cache_to_collector_fifo.read(task);
            uint32_t tid = task->task_id;
            if (pending_tasks.find(tid) != pending_tasks.end()) {
                pending_tasks[tid]->dc_valid = true;
                pending_tasks[tid]->DC = task->DC;
                delete task; // 释放临时 task
                task = pending_tasks[tid];
                got_data = true;
#ifdef DEBUG_COLLECTOR
                printf("[Collector-1] Received DC result for task %d (state=%s)\n", 
                       tid, task->state == TASK_DC_HIT ? "HIT" : "MISS");
#endif
            } else {
                delete task;
            }
        }
        
        if (pc_cache_to_collector_fifo.num_available() > 0) {
            pc_cache_to_collector_fifo.read(task);
            uint32_t tid = task->task_id;
            if (pending_tasks.find(tid) != pending_tasks.end()) {
                pending_tasks[tid]->pc_valid = true;
                pending_tasks[tid]->PC = task->PC;
                delete task; // 释放临时 task
                task = pending_tasks[tid];
                got_data = true;
#ifdef DEBUG_COLLECTOR
                printf("[Collector-1] Received PC result for task %d (state=%s)\n", 
                       tid, task->state == TASK_PC_HIT ? "HIT" : "MISS");
#endif
            } else {
                delete task;
            }
        }

        if (!got_data) {
            wait(SC_ZERO_TIME);
            continue;
        }

        // 遍历 pending_tasks，处理 DC 和 PC 都到齐的任务
        for (auto it = pending_tasks.begin(); it != pending_tasks.end(); ) {
            task = it->second;
            
            // 只有 DC 和 PC 都有效才处理
            if (task->dc_valid && task->pc_valid) {
#ifdef DEBUG_COLLECTOR
                printf("[Collector-1] Processing task %d: DC=%s, PC=%s\n", 
                       task->task_id, 
                       task->state == TASK_DC_HIT ? "HIT" : "MISS",
                       task->state == TASK_PC_HIT ? "HIT" : "MISS");
#endif
                
                // 从 pending 表移除
                it = pending_tasks.erase(it);
                
                // 判断是否需要 PC（根据 DC.tc.PDTV）
                task->need_pc = task->DC.tc.PDTV;
                
                // 情况 1: DC_MISS -> 需要 xDTW 进行 DDT walk
                if (task->state == TASK_DC_MISS) {
#ifdef DEBUG_COLLECTOR
                    printf("[Collector-1] Task %d: DC MISS -> sending to xDTW for DDT walk\n", task->task_id);
#endif
                    collector_to_xdtw_fifo.write(task);
                    continue;
                }
                
                // DC_HIT 的情况
                if (!task->need_pc) {
                    // 不需要 PC，直接使用 DC
#ifdef DEBUG_COLLECTOR
                    printf("[Collector-1] Task %d: PC not needed (PDTV=0)\n", task->task_id);
#endif
                    task->PV = 0;
                    task->PSCID = 0;
                    task->iosatp = task->DC.fsc.iosatp;
                    task->iohgatp = task->DC.iohgatp;
                    task->SUM = 0;
                    task->SXL = task->DC.tc.SXL;
                    task->GV = (task->iohgatp.MODE != IOHGATP_Bare) ? 1 : 0;
                    task->GSCID = task->GV ? task->iohgatp.GSCID : 0;
                    
                    // 执行 DC 配置验证（步骤 7-9）
                    // 检查 EN_ATS
                    if (task->DC.tc.EN_ATS == 0 && 
                        (task->at == ADDR_TYPE_TRANSLATED || task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST)) {
                        task->state = TASK_FAULT;
                        task->cause = 260;
                        fault_fifo.write(task);
                        continue;
                    }
                    
                    // 判断 MSI 地址并路由
                    if (task->DC.msiptp.MODE != MSIPTP_Off) {
                        task->gpa = task->iova; // 第一级翻译完成
                        task->is_msi = is_msi_address(task->gpa, &task->DC) ? 1 : 0;
                        if (task->is_msi) {
#ifdef DEBUG_COLLECTOR
                            printf("[Collector-1] Task %d: MSI address detected -> MSIPT Cache\n", task->task_id);
#endif
                            collector_to_msipt_cache_query_fifo.write(task);
                            continue;
                        }
                    }
                    
#ifdef DEBUG_COLLECTOR
                    printf("[Collector-1] Task %d: -> PT Cache query\n", task->task_id);
#endif
                    collector_to_pt_cache_query_fifo.write(task);
                    
                } else if (task->pc_valid && task->state == TASK_PC_HIT) {
                    // 需要 PC 且已命中
#ifdef DEBUG_COLLECTOR
                    printf("[Collector-1] Task %d: PC HIT, configuring page table\n", task->task_id);
#endif
                    task->PV = task->PC.ta.V;
                    task->PSCID = task->PC.ta.PSCID;
                    task->SUM = task->PC.ta.SUM;
                    task->iosatp = task->PC.fsc.iosatp;
                    task->iohgatp = task->DC.iohgatp;
                    task->SXL = task->DC.tc.SXL;
                    task->GV = (task->iohgatp.MODE != IOHGATP_Bare) ? 1 : 0;
                    task->GSCID = task->GV ? task->iohgatp.GSCID : 0;
                    
                    // 执行 PC 配置验证（步骤 15）
                    if (task->PC.ta.ENS == 0 && task->pid_valid && task->priv_req) {
                        task->state = TASK_FAULT;
                        task->cause = 260;
                        fault_fifo.write(task);
                        continue;
                    }
                    
                    // 判断 MSI 地址并路由
                    if (task->DC.msiptp.MODE != MSIPTP_Off) {
                        task->gpa = task->iova;
                        task->is_msi = is_msi_address(task->gpa, &task->DC) ? 1 : 0;
                        if (task->is_msi) {
#ifdef DEBUG_COLLECTOR
                            printf("[Collector-1] Task %d: MSI address detected -> MSIPT Cache\n", task->task_id);
#endif
                            collector_to_msipt_cache_query_fifo.write(task);
                            continue;
                        }
                    }
                    
#ifdef DEBUG_COLLECTOR
                    printf("[Collector-1] Task %d: -> PT Cache query\n", task->task_id);
#endif
                    collector_to_pt_cache_query_fifo.write(task);
                    
                } else if (task->pc_valid && task->state == TASK_PC_MISS) {
                    // 需要 PC 但未命中 -> xDTW 进行 PDT walk
#ifdef DEBUG_COLLECTOR
                    printf("[Collector-1] Task %d: PC MISS -> sending to xDTW for PDT walk\n", task->task_id);
#endif
                    collector_to_xdtw_fifo.write(task);
                }
                
            } else {
                // DC 或 PC 还未到齐，继续等待
                ++it;
            }
        }

        wait(COLLECTOR_DELAY, SC_NS);
    }
}

// ============================================================================
// Collector 线程 2: xDTW 响应处理
// ============================================================================

void iommu_top::collector_xdtw_response_thread() {
    printf("[Collector-2] xDTW response thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        xdtw_to_collector_fifo.read(task);
        
#ifdef DEBUG_COLLECTOR
        printf("[Collector-2] Received xDTW response for task %d, state=%d\n", 
               task->task_id, task->state);
#endif

        if (task->state == TASK_DC_WALK_DONE) {
            // DDT walk 完成，得到 DC
#ifdef DEBUG_COLLECTOR
            printf("[Collector-2] Task %d: DC walk done, updating DC cache\n", task->task_id);
#endif
            // 更新 DC Cache
            collector_to_dc_cache_update_fifo.write(task);
            
            // 判断是否需要 PC
            task->need_pc = task->DC.tc.PDTV;
            
            if (task->need_pc && !task->pc_valid) {
                // 需要 PC，发起 PDT walk
#ifdef DEBUG_COLLECTOR
                printf("[Collector-2] Task %d: Need PC, sending to xDTW for PDT walk\n", task->task_id);
#endif
                collector_to_xdtw_fifo.write(task);
            } else {
                // 不需要 PC 或 PC 已命中
                if (!task->need_pc) {
                    task->PV = 0;
                    task->PSCID = 0;
                    task->iosatp = task->DC.fsc.iosatp;
                    task->iohgatp = task->DC.iohgatp;
                    task->SUM = 0;
                    task->SXL = task->DC.tc.SXL;
                } else {
                    task->PV = task->PC.ta.V;
                    task->PSCID = task->PC.ta.PSCID;
                    task->SUM = task->PC.ta.SUM;
                    task->iosatp = task->PC.fsc.iosatp;
                    task->iohgatp = task->DC.iohgatp;
                    task->SXL = task->DC.tc.SXL;
                }
                task->GV = (task->iohgatp.MODE != IOHGATP_Bare) ? 1 : 0;
                task->GSCID = task->GV ? task->iohgatp.GSCID : 0;
                
                // 执行配置验证
                if (task->DC.tc.EN_ATS == 0 && 
                    (task->at == ADDR_TYPE_TRANSLATED || task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST)) {
                    task->state = TASK_FAULT;
                    task->cause = 260;
                    fault_fifo.write(task);
                    continue;
                }
                
                // 判断 MSI 地址
                if (task->DC.msiptp.MODE != MSIPTP_Off) {
                    task->gpa = task->iova;
                    task->is_msi = is_msi_address(task->gpa, &task->DC) ? 1 : 0;
                    if (task->is_msi) {
                        collector_to_msipt_cache_query_fifo.write(task);
                        continue;
                    }
                }
                
                collector_to_pt_cache_query_fifo.write(task);
            }
            
        } else if (task->state == TASK_PC_WALK_DONE) {
            // PDT walk 完成，得到 PC
#ifdef DEBUG_COLLECTOR
            printf("[Collector-2] Task %d: PC walk done, updating PC cache\n", task->task_id);
#endif
            // 更新 PC Cache
            collector_to_pc_cache_update_fifo.write(task);
            
            // 设置 PC 相关字段
            task->PV = task->PC.ta.V;
            task->PSCID = task->PC.ta.PSCID;
            task->SUM = task->PC.ta.SUM;
            task->iosatp = task->PC.fsc.iosatp;
            task->iohgatp = task->DC.iohgatp;
            task->SXL = task->DC.tc.SXL;
            task->GV = (task->iohgatp.MODE != IOHGATP_Bare) ? 1 : 0;
            task->GSCID = task->GV ? task->iohgatp.GSCID : 0;
            
            // 判断 MSI 地址
            if (task->DC.msiptp.MODE != MSIPTP_Off) {
                task->gpa = task->iova;
                task->is_msi = is_msi_address(task->gpa, &task->DC) ? 1 : 0;
                if (task->is_msi) {
                    collector_to_msipt_cache_query_fifo.write(task);
                    continue;
                }
            }
            
            collector_to_pt_cache_query_fifo.write(task);
            
        } else if (task->state == TASK_FAULT) {
            // 错误上报
#ifdef DEBUG_COLLECTOR
            printf("[Collector-2] Task %d: Fault detected, cause=%d\n", task->task_id, task->cause);
#endif
            fault_fifo.write(task);
        }

        wait(COLLECTOR_DELAY, SC_NS);
    }
}
