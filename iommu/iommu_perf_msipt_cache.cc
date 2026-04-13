//
// IOMMU 性能模型 - MSIPT Cache 模块实现
// 按照 SPEC v4 第 5.6 节定义实现
// 参考：iommu_msi_trans.cc
//

#include "iommu_top.hh"
#include "iommu_perf_model.hh"

// ============================================================================
// MSIPT Cache 线程 1: 查询
// ============================================================================

void iommu_top::msipt_cache_query_thread() {
    printf("[MSIPT Cache-1] Query thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        collector_to_msipt_cache_query_fifo.read(task);
        
#ifdef DEBUG_MSIPT_CACHE
        printf("[MSIPT Cache-1] Query for task %d: msiptp.PPN=0x%lx, iova=0x%lx\n", 
               task->task_id, task->DC.msiptp.PPN, task->iova);
#endif
        
        // 计算 interrupt_file_num
        // interrupt_file_num = (iova >> 12) & 0xFF (简化)
        uint32_t interrupt_file_num = (task->iova >> 12) & 0xFF;
        
        // 查找 MSIPT Cache
        uint64_t msipte_data = 0;
        uint8_t result = lookup_msipt_cache(task->DC.msiptp.PPN, interrupt_file_num, &msipte_data);
        
        if (result) {
            // Hit
            task->state = TASK_MSIPT_HIT;
            // TODO: 解析 msipte_data 填充 task 字段
#ifdef DEBUG_MSIPT_CACHE
            printf("[MSIPT Cache-1] Task %d: MSIPT HIT\n", task->task_id);
#endif
        } else {
            // Miss
            task->state = TASK_MSIPT_MISS;
#ifdef DEBUG_MSIPT_CACHE
            printf("[MSIPT Cache-1] Task %d: MSIPT MISS\n", task->task_id);
#endif
        }
        
        msipt_cache_lookup_result_fifo.write(task);
        wait(MSIPT_CACHE_HIT_DELAY, SC_NS);
    }
}

// ============================================================================
// MSIPT Cache 线程 2: 结果处理
// ============================================================================

void iommu_top::msipt_cache_result_thread() {
    printf("[MSIPT Cache-2] Result thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        msipt_cache_lookup_result_fifo.read(task);
        
#ifdef DEBUG_MSIPT_CACHE
        printf("[MSIPT Cache-2] Processing task %d, state=%d\n", task->task_id, task->state);
#endif
        
        if (task->state == TASK_MSIPT_HIT) {
            // 命中，执行 MSI 输出路由
            // TODO: 解析 MSI PTE，判断 M 字段
            task->state = TASK_FORWARD;
#ifdef DEBUG_MSIPT_CACHE
            printf("[MSIPT Cache-2] Task %d: MSIPT HIT -> forwarding\n", task->task_id);
#endif
            msipt_cache_to_fwd_fifo.write(task);
            
        } else if (task->state == TASK_MSIPT_MISS) {
            // 未命中，提交 MSIPTW
#ifdef DEBUG_MSIPT_CACHE
            printf("[MSIPT Cache-2] Task %d: MSIPT MISS -> sending to MSIPTW\n", task->task_id);
#endif
            msipt_cache_to_msiptw_fifo.write(task);
        }
        
        wait(MSIPT_CACHE_HIT_DELAY, SC_NS);
    }
}

// ============================================================================
// MSIPTW 线程 3: 请求
// ============================================================================

void iommu_top::msiptw_req_thread() {
    printf("[MSIPTW-3] Request thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        msipt_cache_to_msiptw_fifo.read(task);
        
#ifdef DEBUG_MSIPTW
        printf("[MSIPTW-3] Task %d: MSI PTW request\n", task->task_id);
#endif
        
        // 计算 interrupt_file_num
        uint32_t interrupt_file_num = (task->iova >> 12) & 0xFF;
        
        // 计算 MSI PTE 地址
        // addr = msiptp.PPN * PAGESIZE + (interrupt_file_num * 16)
        uint64_t addr = task->DC.msiptp.PPN * 4096 + (interrupt_file_num * 16);
        
#ifdef DEBUG_MSIPTW
        printf("[MSIPTW-3] Task %d: Reading MSI PTE at 0x%lx (file_num=%d)\n", 
               task->task_id, addr, interrupt_file_num);
#endif
        
        // 设置 walk 上下文
        task->walk_ctx.walk_type = WALK_MSI_PT;
        task->walk_ctx.read_addr = addr;
        task->walk_ctx.read_size = 16; // MSI PTE 是 16 字节
        
        // 分配 AXI ID
        uint16_t axi_id = axi_id_alloc.alloc_id();
        task->current_axi_id = axi_id;
        
        // 发起非阻塞 DDR 读（16 字节）
        send_ddr_nb_read(addr, 16, axi_id, WALK_MSI_PT, task);
        
        wait(MSIPTW_DELAY_PER_ACCESS, SC_NS);
    }
}

// ============================================================================
// MSIPTW 线程 4: 响应
// ============================================================================

void iommu_top::msiptw_rsp_thread() {
    printf("[MSIPTW-4] Response thread started\n");
    
    while (true) {
        // 等待 DDR 响应事件
        wait(msiptw_rsp_evt);
        
        while (!msiptw_rsp_queue.empty()) {
            ddr_response_t rsp = msiptw_rsp_queue.front();
            msiptw_rsp_queue.pop();
            
            // 从 outstanding 表恢复 task
            auto it = ddr_outstanding_table.find(rsp.axi_id);
            if (it == ddr_outstanding_table.end()) {
                printf("[MSIPTW-4] ERROR: AXI ID %d not found\n", rsp.axi_id);
                continue;
            }
            
            iommu_task_t* task = it->second.task;
            
            // 从 outstanding 表移除并释放 AXI ID
            ddr_outstanding_table.erase(it);
            axi_id_alloc.free_id(rsp.axi_id);
            
            if (rsp.status != 0) {
                // DDR 错误
                printf("[MSIPTW-4] Task %d: DDR error\n", task->task_id);
                task->state = TASK_FAULT;
                task->cause = 257;
                fault_fifo.write(task);
                continue;
            }
            
#ifdef DEBUG_MSIPTW
            printf("[MSIPTW-4] Task %d: Received MSI PTE data\n", task->task_id);
#endif
            
            // 解析 MSI PTE（16 字节）
            // msipte 格式：
            //   bits 63:0:   PPN
            //   bits 67:64:  M (mode)
            //   bit 0:       V (valid)
            uint64_t* ptr = (uint64_t*)rsp.data;
            uint64_t msipte_low = ptr[0];
            uint64_t msipte_high = ptr[1];
            
            uint8_t V = msipte_low & 0x1;
            uint8_t M = (msipte_high >> 0) & 0xF; // 简化
            uint64_t PPN = (msipte_low >> 10) & 0xFFFFFFFFFULL;
            
#ifdef DEBUG_MSIPTW
            printf("[MSIPTW-4] Task %d: MSI PTE V=%d, M=%d, PPN=0x%lx\n", 
                   task->task_id, V, M, PPN);
#endif
            
            // 检查 V 位
            if (V == 0) {
                task->state = TASK_FAULT;
                task->cause = 261; // MSI PTE not valid
                fault_fifo.write(task);
                continue;
            }
            
            // 检查 M 字段
            if (M == 0 || M == 2) {
                task->state = TASK_FAULT;
                task->cause = 263; // Invalid MSI PTE mode
                fault_fifo.write(task);
                continue;
            }
            
            // M == 3: Basic Translate
            if (M == 3) {
                task->pa = PPN << 12 | (task->gpa & 0xFFF);
                task->is_mrif = 0;
                task->is_msi = 1;
                task->state = TASK_MSIPT_WALK_DONE;
#ifdef DEBUG_MSIPTW
                printf("[MSIPTW-4] Task %d: M=3 Basic Translate, pa=0x%lx\n", 
                       task->task_id, task->pa);
#endif
                msipt_cache_to_fwd_fifo.write(task);
            }
            // M == 1: MRIF 模式
            else if (M == 1) {
                // 提取 MRIF 信息
                // TODO: 从 msipte 中提取 MRIF 地址和 NID
                task->is_mrif = 1;
                task->is_msi = 1;
                task->state = TASK_MSIPT_WALK_DONE;
#ifdef DEBUG_MSIPTW
                printf("[MSIPTW-4] Task %d: M=1 MRIF mode\n", task->task_id);
#endif
                msipt_cache_to_fwd_fifo.write(task);
            }
        }
    }
}
