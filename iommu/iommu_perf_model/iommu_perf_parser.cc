// IOMMU Performance Model - Parser Thread
// Corresponds to iommu_translate_iova() steps 1-5
// SPEC Section 12.1

#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>

void iommu_top::parser_thread() {
    while (true) {
        // 0. 全局 outstanding 反压：达到上限则等待 reorder 释放（保留兼容）
        while (iommu_global_outstanding >= (int)IOMMU_GLOBAL_MAX_OUTSTANDING) {
            wait(iommu_global_outstanding_freed_event);
        }

        // 1. Read task from inbound FIFO
        iommu_task_t* task = inbound_fifo.read();
        task->timestamp = sc_time_stamp();  // [STAT] 记录IO入口时刻
        printf("[PARSER] task_id=%u popped from inbound_fifo\n", task->task_id);
        fflush(stdout);

        // [场景13] 读写分离 outstanding 反压：按任务方向分别限流
        bool is_write = (task->read_writeAMO == WRITE);
        if (is_write) {
            while (iommu_write_outstanding >= (int)IOMMU_WRITE_MAX_OUTSTANDING) {
                wait(iommu_write_outstanding_freed_event);
            }
        } else {
            while (iommu_read_outstanding >= (int)IOMMU_READ_MAX_OUTSTANDING) {
                wait(iommu_read_outstanding_freed_event);
            }
        }

        // 1.1 入口注册到 reorder buffer，并申请全局 outstanding 槽
        reorder_register_task(task);

        task->state = TASK_PARSING;
        // [PERF] Parser无需串行延时：瓶颈由并发outstanding数和下游模块决定

        // 2. TTYP classification and access attribute extraction
        // Corresponds to iommu_translate.cc L46-89
        classify_ttype_and_attribs(task);

        // 3. Step 1: Check ddtp.iommu_mode == Off
        // Corresponds to iommu_translate.cc L97-113
        if (iommu_inst.reg_file.ddtp.iommu_mode == Off) {
            printf("[PARSER] task_id=%u -> FAULT: iommu_mode=Off\n", task->task_id);
            fflush(stdout);
            task->cause = 256; // All inbound transactions disallowed
            task->iotval = task->iova;
            task->state = TASK_FAULT;
            if (task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) {
                // ATS in Off mode: report fault with UNSUPPORTED_REQUEST
            }
            collector_to_fault_fifo.write(task);
            continue;
        }

        // 4. Step 2: Check ddtp.iommu_mode == Bare
        // Corresponds to iommu_translate.cc L120-141
        if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_Bare) {
            if (task->at == ADDR_TYPE_TRANSLATED ||
                task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) {
                printf("[PARSER] task_id=%u -> FAULT: Bare mode with Translated/ATS\n", task->task_id);
                fflush(stdout);
                task->cause = 260; // Transaction type disallowed
                task->iotval = task->iova;
                task->state = TASK_FAULT;
                collector_to_fault_fifo.write(task);
                continue;
            }
            // Bare mode: PA = IOVA
            printf("[PARSER] task_id=%u -> Bare mode, direct pass to forwarder\n", task->task_id);
            fflush(stdout);
            task->pa = task->iova;
            task->page_sz = PAGESIZE;
            task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = 1;
            task->vs_pte.PBMT = PMA;
            task->g_pte.X = task->g_pte.W = task->g_pte.R = 1;
            task->is_msi = 0;
            task->is_bare_translation = 1;
            task->state = TASK_FORWARD;
            pt_cache_to_fwd_fifo.write(task);
            continue;
        }

        // 5. Steps 3-4: DDI extraction
        // Corresponds to iommu_translate.cc L143-158
        extract_DDI(task, &iommu_inst);

        // 6. Step 5: Device ID width validation
        // Corresponds to iommu_translate.cc L162-182
        if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_2LVL && task->DDI[2] != 0) {
            printf("[PARSER] task_id=%u -> FAULT: DDT_2LVL with DDI[2]!=0\n", task->task_id);
            fflush(stdout);
            task->cause = 260; // Transaction type disallowed
            task->iotval = task->iova;
            task->state = TASK_FAULT;
            collector_to_fault_fifo.write(task);
            continue;
        }
        if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_1LVL &&
            (task->DDI[2] != 0 || task->DDI[1] != 0)) {
            printf("[PARSER] task_id=%u -> FAULT: DDT_1LVL with DDI[2/1]!=0\n", task->task_id);
            fflush(stdout);
            task->cause = 260; // Transaction type disallowed
            task->iotval = task->iova;
            task->state = TASK_FAULT;
            collector_to_fault_fifo.write(task);
            continue;
        }

        // 7. Dispatch to DC/PC Cache query and Collector
        // Parser writes task to collector, and CacheMessage to cache_sub FIFOs
        task->state = TASK_PARSE_DONE;
        task->iotval = task->iova;
        task->DTF = 0;

        printf("[PARSER] task_id=%u, device_id=0x%x, iova=0x%lx, TTYP=%d -> collector+dc_cache+pc_cache\n",
               task->task_id, task->device_id, task->iova, task->TTYP);
        fflush(stdout);

        // Send to collector
        parser_to_collector_fifo.write(task);
        
        // Convert task to CacheMessage and send to CacheSubsystem DC request FIFO
        iommu::CacheMessage dc_req = task_to_dc_request(task);
        cache_sub.dc_request_fifo.write(dc_req);
        
        // Convert task to CacheMessage and send to CacheSubsystem PC request FIFO
        iommu::CacheMessage pc_req = task_to_pc_request(task);
        cache_sub.pc_request_fifo.write(pc_req);
    }
}
