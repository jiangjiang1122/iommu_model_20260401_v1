//
// IOMMU 性能模型 - Forwarder 和 Fault/CQ Proc 模块实现
// 按照 SPEC v4 第 5.9-5.12 节定义实现
//

#include "iommu_top.hh"
#include "iommu_perf_model.hh"

// ============================================================================
// Forwarder 线程
// ============================================================================

void iommu_top::forwarder_thread() {
    printf("[Forwarder] Thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        pt_cache_to_fwd_fifo.read(task);
        
#ifdef DEBUG_FORWARDER
        printf("[Forwarder] Task %d: Forwarding to PA 0x%lx\n", task->task_id, task->pa);
#endif
        
        if (task->original_trans) {
            // 设置物理地址
            task->original_trans->set_address(task->pa);
            
            // 通过 AXI Master 0 发起非阻塞转发
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            sc_time delay = sc_time(FORWARDER_DELAY, SC_NS);
            
            axi_master_0_to_pcie_noc_socket->nb_transport_fw(*task->original_trans, phase, delay);
            
#ifdef DEBUG_FORWARDER
            printf("[Forwarder] Task %d: Sent to AXI Master 0\n", task->task_id);
#endif
        }
        
        // 释放任务
        delete task;
        wait(FORWARDER_DELAY, SC_NS);
    }
}

// ============================================================================
// MSI Forwarder 线程
// ============================================================================

void iommu_top::msi_forwarder_thread() {
    printf("[MSI Forwarder] Thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        msipt_cache_to_fwd_fifo.read(task);
        
#ifdef DEBUG_FORWARDER
        printf("[MSI Forwarder] Task %d: MSI forwarding, is_mrif=%d\n", task->task_id, task->is_mrif ? 1 : 0);
#endif
        
        if (task->original_trans) {
            if (task->is_mrif) {
                // MRIF 模式：原子 OR 写入
                task->original_trans->set_address(task->dest_mrif_addr);
                
                // 使用阻塞模式进行原子 OR 写
                send_ddr_blocking_atomic_or(task->dest_mrif_addr, 0x1, 8);
                
#ifdef DEBUG_FORWARDER
                printf("[MSI Forwarder] Task %d: MRIF atomic write to 0x%lx\n", 
                       task->task_id, task->dest_mrif_addr);
#endif
            } else {
                // 基本翻译：通过 AXI Stream 发送
                task->original_trans->set_address(task->pa);
                
                axi_stream_to_cmn_rnd_socket->b_transport(*task->original_trans, 
                    sc_time(FORWARDER_DELAY, SC_NS));
                
#ifdef DEBUG_FORWARDER
                printf("[MSI Forwarder] Task %d: Sent to AXI Stream (IMSIC)\n", task->task_id);
#endif
            }
        }
        
        // 释放任务
        delete task;
        wait(FORWARDER_DELAY, SC_NS);
    }
}

// ============================================================================
// Fault Proc 线程
// ============================================================================

void iommu_top::fault_proc_thread() {
    printf("[Fault Proc] Thread started\n");
    
    while (true) {
        iommu_task_t* task = nullptr;
        fault_fifo.read(task);
        
#ifdef DEBUG_FAULTS
        printf("[Fault Proc] Task %d: Fault detected, cause=%d\n", task->task_id, task->cause);
#endif
        
        // 生成 fault record
        // TODO: 参考 iommu_faults.cc 实现完整的 fault record 生成
        
        // 检查 FQ 是否已满
        // 简化：直接写入 FQ
        if (iommu_inst.reg_file.fq.fqof) {
            // FQ overflow
#ifdef DEBUG_FAULTS
            printf("[Fault Proc] FQ overflow detected\n");
#endif
        } else {
            // 写入 FQ（32 字节）
            // TODO: 实现 FQ 写入逻辑
            // 使用 b_transport 写入 DDR
            uint64_t fq_addr = iommu_inst.reg_file.fqaddr * 4096 + iommu_inst.reg_file.fqt * 32;
            
            // 简化的 fault record
            struct {
                uint64_t cause;
                uint64_t ttyp;
                uint64_t iotval;
                uint64_t iotval2;
                uint64_t did_pid;
            } fault_record;
            
            fault_record.cause = task->cause;
            fault_record.ttyp = task->TTYP;
            fault_record.iotval = task->iotval;
            fault_record.iotval2 = task->iotval2;
            fault_record.did_pid = ((uint64_t)task->device_id << 32) | task->process_id;
            
            // 写入 DDR
            send_ddr_blocking_write(fq_addr, sizeof(fault_record), (char*)&fault_record);
            
            // 更新 fqt
            iommu_inst.reg_file.fqt = (iommu_inst.reg_file.fqt + 1) % iommu_inst.reg_file.fqsz;
            
            // 触发 fault 中断（如果使能）
            // TODO: 调用 interrupt 相关函数
        }
        
        // 设置原始事务的错误响应
        if (task->original_trans) {
            task->original_trans->set_response_status(TLM_GENERIC_ERROR_RESPONSE);
        }
        
        // 释放任务
        delete task;
        wait(1, SC_NS);
    }
}

// ============================================================================
// CQ Proc 线程
// ============================================================================

void iommu_top::cq_proc_thread() {
    printf("[CQ Proc] Thread started\n");
    
    while (true) {
        // 检查 CQ 是否有新命令（cqh != cqt）
        if (iommu_inst.reg_file.cqh == iommu_inst.reg_file.cqt) {
            // CQ 空，等待
            wait(10, SC_NS);
            continue;
        }
        
#ifdef DEBUG_COMMANDS
        printf("[CQ Proc] Processing CQ command, cqh=0x%lx, cqt=0x%lx\n", 
               iommu_inst.reg_file.cqh, iommu_inst.reg_file.cqt);
#endif
        
        // 通过 AXI Master 2 读取 CQ 条目（16 字节）
        uint64_t cq_addr = iommu_inst.reg_file.cqaddr * 4096 + iommu_inst.reg_file.cqh * 16;
        
        char cq_data[16];
        send_ddr_blocking_read(cq_addr, 16, cq_data);
        
        // 解析 CQ 命令
        uint64_t cmd_raw = *((uint64_t*)cq_data);
        uint8_t cmd_type = (cmd_raw >> 0) & 0xFF;
        uint8_t func3 = (cmd_raw >> 8) & 0x7;
        
#ifdef DEBUG_COMMANDS
        printf("[CQ Proc] CQ command type=0x%02x, func3=%d\n", cmd_type, func3);
#endif
        
        // 根据命令类型处理
        cache_inv_cmd_t inv_cmd;
        
        switch (cmd_type) {
            case 0x01: // IOTINVAL.VMA
#ifdef DEBUG_COMMANDS
                printf("[CQ Proc] Processing IOTINVAL.VMA\n");
#endif
                inv_cmd.type = INV_VMA;
                // 从 CQ 条目提取 gscid, pscid, addr 等字段
                inv_cmd.gscid = (cmd_raw >> 16) & 0xFFFF;
                inv_cmd.pscid = (cmd_raw >> 32) & 0xFFFFF;
                inv_cmd.addr = *((uint64_t*)(cq_data + 8));
                inv_cmd.gv = (cmd_raw >> 60) & 0x1;
                inv_cmd.pscidv = (cmd_raw >> 61) & 0x1;
                inv_cmd.av = (cmd_raw >> 62) & 0x1;
                
                // 发送到 Cache Invalidation FIFO
                cq_to_cache_inv_fifo.write(inv_cmd);
                break;
                
            case 0x02: // IOTINVAL.DDT
#ifdef DEBUG_COMMANDS
                printf("[CQ Proc] Processing IOTINVAL.DDT\n");
#endif
                inv_cmd.type = INV_DDT;
                inv_cmd.device_id = (cmd_raw >> 16) & 0xFFFFFF;
                inv_cmd.gscid = (cmd_raw >> 48) & 0xFFFF;
                
                cq_to_cache_inv_fifo.write(inv_cmd);
                break;
                
            case 0x03: // IOFENCE.C
#ifdef DEBUG_COMMANDS
                printf("[CQ Proc] Processing IOFENCE.C\n");
#endif
                // 等待所有 in-flight 翻译完成
                // TODO: 检查 outstanding 表
                while (!ddr_outstanding_table.empty()) {
                    wait(1, SC_NS);
                }
                break;
                
            case 0x04: // ATS.INVAL
#ifdef DEBUG_COMMANDS
                printf("[CQ Proc] Processing ATS.INVAL\n");
#endif
                // 通过 AXI Master 1 向 RP 发送 ATS Invalidation Request
                // TODO: 实现 ATS 消息发送
                break;
                
            case 0x05: // ATS.PRGR
#ifdef DEBUG_COMMANDS
                printf("[CQ Proc] Processing ATS.PRGR\n");
#endif
                // 通过 AXI Master 1 向 RP 发送 Page Request Group Response
                // TODO: 实现 ATS 消息发送
                break;
                
            default:
#ifdef DEBUG_COMMANDS
                printf("[CQ Proc] Unknown command type 0x%02x\n", cmd_type);
#endif
                break;
        }
        
        // 更新 cqh
        iommu_inst.reg_file.cqh = (iommu_inst.reg_file.cqh + 1) % iommu_inst.reg_file.cqsz;
        
        wait(1, SC_NS);
    }
}
