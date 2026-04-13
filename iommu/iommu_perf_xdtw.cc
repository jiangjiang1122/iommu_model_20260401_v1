//
// IOMMU 性能模型 - xDTW (DDT/PDT Walker) 模块实现
// 按照 SPEC v4 第 5.7 节定义实现
// 参考：iommu_device_context.cc, iommu_process_context.cc
//

#include "iommu_top.hh"
#include "iommu_perf_model.hh"

// ============================================================================
// xDTW 线程 1: 请求
// ============================================================================

void iommu_top::xdtw_req_thread() {
    printf("[xDTW-1] Request thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        collector_to_xdtw_fifo.read(task);
        
        // 判断 walk 类型（DDT 或 PDT）
        walk_type_t walk_type;
        uint64_t base_addr;
        uint8_t index;
        
        if (task->state == TASK_DC_MISS || !task->dc_valid) {
            // DDT walk
            walk_type = WALK_DDT;
            base_addr = iommu_inst.reg_file.ddtbase.PPN * 4096;
            
            // 计算 DDI 索引
            index = calculate_ddt_index(&iommu_inst, task->device_id);
            
#ifdef DEBUG_XDTW
            printf("[xDTW-1] Task %d: DDT walk for device_id %d, index=%d, base=0x%lx\n", 
                   task->task_id, task->device_id, index, base_addr);
#endif
        } else {
            // PDT walk
            walk_type = WALK_PDT;
            base_addr = task->DC.fsc.pdtp.PPN * 4096;
            
            // 计算 PDI 索引
            index = calculate_pdt_index(task->DC.fsc, task->process_id);
            
#ifdef DEBUG_XDTW
            printf("[xDTW-1] Task %d: PDT walk for process_id %d, index=%d, base=0x%lx\n", 
                   task->task_id, task->process_id, index, base_addr);
#endif
        }
        
        // 初始化 walk 上下文
        task->walk_ctx.walk_type = walk_type;
        task->walk_ctx.level = 0;
        task->walk_ctx.max_levels = 1; // DDT/PDT 只有 1 级
        task->walk_ctx.base_addr = base_addr;
        task->walk_ctx.index[0] = index;
        task->walk_ctx.pte_size = 8; // 8 字节 entry
        task->walk_ctx.read_addr = base_addr + index * 8;
        task->walk_ctx.read_size = 8;
        
        // 分配 AXI ID
        uint16_t axi_id = axi_id_alloc.alloc_id();
        task->current_axi_id = axi_id;
        
#ifdef DEBUG_XDTW
        printf("[xDTW-1] Task %d: Alloc AXI ID %d for %s entry at 0x%lx\n", 
               task->task_id, axi_id, (walk_type == WALK_DDT) ? "DDT" : "PDT", 
               task->walk_ctx.read_addr);
#endif

        // 发起非阻塞 DDR 读
        send_ddr_nb_read(task->walk_ctx.read_addr, 8, axi_id, walk_type, task);
        
        wait(XDTW_DELAY_PER_ACCESS, SC_NS);
    }
}

// ============================================================================
// xDTW 线程 2: 响应
// ============================================================================

void iommu_top::xdtw_rsp_thread() {
    printf("[xDTW-2] Response thread started\n");
    
    while (true) {
        // 等待 DDR 响应事件
        wait(xdtw_rsp_evt);
        
        while (!xdtw_rsp_queue.empty()) {
            ddr_response_t rsp = xdtw_rsp_queue.front();
            xdtw_rsp_queue.pop();
            
            // 从 outstanding 表恢复 task
            auto it = ddr_outstanding_table.find(rsp.axi_id);
            if (it == ddr_outstanding_table.end()) {
                printf("[xDTW-2] ERROR: AXI ID %d not found in outstanding table\n", rsp.axi_id);
                continue;
            }
            
            iommu_task_t* task = it->second.task;
            walk_type_t walk_type = it->second.walk_type;
            
            // 从 outstanding 表移除并释放 AXI ID
            ddr_outstanding_table.erase(it);
            axi_id_alloc.free_id(rsp.axi_id);
            
            if (rsp.status != 0) {
                // DDR 错误
                printf("[xDTW-2] Task %d: DDR error\n", task->task_id);
                task->state = TASK_FAULT;
                task->cause = 257; // DDR access error
                xdtw_to_collector_fifo.write(task);
                continue;
            }
            
            if (walk_type == WALK_DDT) {
                // DDT walk 处理
#ifdef DEBUG_XDTW
                printf("[xDTW-2] Task %d: Processing DDT entry\n", task->task_id);
#endif
                
                ddte_t ddte;
                ddte.raw = *((uint64_t*)rsp.data);
                
                if (!ddte.V) {
                    // DDT entry 无效
#ifdef DEBUG_XDTW
                    printf("[xDTW-2] Task %d: DDT entry invalid (V=0)\n", task->task_id);
#endif
                    task->state = TASK_FAULT;
                    task->cause = 258; // DDT not valid
                    xdtw_to_collector_fifo.write(task);
                    continue;
                }
                
                // DDT entry 有效，获取 DC 地址并发起第二次 DDR 读
                uint64_t dc_addr = ddte.PPN * 4096;
                
#ifdef DEBUG_XDTW
                printf("[xDTW-2] Task %d: Reading DC from 0x%lx\n", task->task_id, dc_addr);
#endif
                
                // 分配新的 AXI ID 读取 DC
                uint16_t new_axi_id = axi_id_alloc.alloc_id();
                task->current_axi_id = new_axi_id;
                
                // 将当前任务保存到 outstanding 表，等待 DC 读取完成
                ddr_outstanding_table[new_axi_id] = {task, WALK_DDT_DC_READ, dc_addr, (uint8_t)sizeof(device_context_t)};
                
                // 发起 DC 读取
                send_ddr_nb_read(dc_addr, sizeof(device_context_t), new_axi_id, WALK_DDT_DC_READ, task);
                
                continue; // 等待 DC 读取完成
                
            } else if (walk_type == WALK_PDT) {
                // PDT walk 处理
#ifdef DEBUG_XDTW
                printf("[xDTW-2] Task %d: Processing PDT entry\n", task->task_id);
#endif
                
                pdte_t pdte;
                pdte.raw = *((uint64_t*)rsp.data);
                
                if (!pdte.V) {
                    // PDT entry 无效
#ifdef DEBUG_XDTW
                    printf("[xDTW-2] Task %d: PDT entry invalid (V=0)\n", task->task_id);
#endif
                    task->state = TASK_FAULT;
                    task->cause = 259; // PDT not valid
                    xdtw_to_collector_fifo.write(task);
                    continue;
                }
                
                // PDT entry 有效，获取 PC 地址并发起第二次 DDR 读
                uint64_t pc_addr = pdte.PPN * 4096;
                
#ifdef DEBUG_XDTW
                printf("[xDTW-2] Task %d: Reading PC from 0x%lx\n", task->task_id, pc_addr);
#endif
                
                // 分配新的 AXI ID 读取 PC
                uint16_t new_axi_id_pc = axi_id_alloc.alloc_id();
                task->current_axi_id = new_axi_id_pc;
                
                // 将当前任务保存到 outstanding 表，等待 PC 读取完成
                ddr_outstanding_table[new_axi_id_pc] = {task, WALK_PDT_PC_READ, pc_addr, (uint8_t)sizeof(process_context_t)};
                
                // 发起 PC 读取
                send_ddr_nb_read(pc_addr, sizeof(process_context_t), new_axi_id_pc, WALK_PDT_PC_READ, task);
                
                continue; // 等待 PC 读取完成
            } else if (walk_type == WALK_DDT_DC_READ) {
                // 处理从 DDT 读取回来的 DC 数据
#ifdef DEBUG_XDTW
                printf("[xDTW-2] Task %d: Processing DC data from DDT\n", task->task_id);
#endif
                // 将读取的数据复制到 task 的 DC 结构中
                for(int i = 0; i < sizeof(device_context_t); i++) {
                    ((char*)&task->DC)[i] = rsp.data[i];
                }
                task->DC.tc.V = 1; // 标记 DC 有效
                task->state = TASK_DC_WALK_DONE;
                
#ifdef DEBUG_XDTW
                printf("[xDTW-2] Task %d: DC walk done, DTF=%d, PDTV=%d\n", 
                       task->task_id, task->DC.tc.DTF, task->DC.tc.PDTV);
#endif
            } else if (walk_type == WALK_PDT_PC_READ) {
                // 处理从 PDT 读取回来的 PC 数据
#ifdef DEBUG_XDTW
                printf("[xDTW-2] Task %d: Processing PC data from PDT\n", task->task_id);
#endif
                // 将读取的数据复制到 task 的 PC 结构中
                for(int i = 0; i < sizeof(process_context_t); i++) {
                    ((char*)&task->PC)[i] = rsp.data[i];
                }
                task->PC.ta.V = 1; // 标记 PC 有效
                task->state = TASK_PC_WALK_DONE;
                
#ifdef DEBUG_XDTW
                printf("[xDTW-2] Task %d: PC walk done, PSCID=%d, SUM=%d\n", 
                       task->task_id, task->PC.ta.PSCID, task->PC.ta.SUM);
#endif
            }
            
            // 发送结果给 Collector
            xdtw_to_collector_fifo.write(task);
        }
    }
}
