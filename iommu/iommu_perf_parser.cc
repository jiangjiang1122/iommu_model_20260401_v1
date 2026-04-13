//
// IOMMU 性能模型 - Parser 模块实现
// 按照 SPEC v4 第 5.1 节定义实现
//

#include "iommu_top.hh"
#include "iommu_perf_model.hh"

// ============================================================================
// Parser 线程实现
// ============================================================================

void iommu_top::parser_thread() {
    printf("[Parser] Thread started\n");
    
    while (true) {
        // 从 inbound_fifo 阻塞读取请求
        tlm::tlm_generic_payload* payload = nullptr;
        inbound_fifo.read(payload);
        
        // 提取 PayloadExtention 信息
        PayloadExtention* ext = nullptr;
        payload->get_extension(ext);
        if (!ext) {
            printf("[Parser] ERROR: No extension in payload\n");
            continue;
        }

        // 创建新的任务上下文
        iommu_task_t* task = new iommu_task_t();
        task->task_id = ++global_task_counter;
        task->timestamp = sc_time_stamp();
        task->original_trans = payload;
        
        // 填充原始请求字段
        task->device_id = ext->requester_id;
        task->process_id = ext->process_id;
        task->pid_valid = ext->pid_valid;
        task->exec_req = ext->exec_req;
        task->priv_req = ext->priv_req;
        task->no_write = ext->no_write;
        task->at = (addr_type_t)ext->at;
        task->iova = payload->get_address();
        task->length = payload->get_data_length();
        task->read_writeAMO = (payload->get_command() == TLM_READ_COMMAND) ? READ : WRITE;
        
        // 初始化 Walker Cache 命中信息
        task->wc_hit_level = -1;
        task->wc_hit_ppn = 0;

#ifdef DEBUG_PARSER
        printf("[Parser] Task %d: device_id=%d, iova=0x%lx, at=%d, pid_valid=%d\n", 
               task->task_id, task->device_id, task->iova, task->at, task->pid_valid);
#endif

        // Step 1: 判断请求类型
        // 如果是 PRI 消息 (PAGE_REQ)
        if (ext->msg_type == 0b0010 && ext->msg_code == 0x00) { // PAGE_REQ
#ifdef DEBUG_PARSER
            printf("[Parser] Task %d: PRI Page Request detected\n", task->task_id);
#endif
            parser_to_pq_fifo.write(task);
            wait(PARSER_DELAY, SC_NS);
            continue;
        }

        // Step 2: 提取访问属性
        get_attribs_from_req(task, &task->is_read, &task->is_write, &task->is_exec, &task->priv);
        
        // 推导 TTYP (Transaction Type)
        task->TTYP = 0;
        if (task->at == ADDR_TYPE_UNTRANSLATED && task->read_writeAMO == READ) {
            task->TTYP = task->exec_req ? UNTRANSLATED_READ_FOR_EXECUTE_TRANSACTION : UNTRANSLATED_READ_TRANSACTION;
        } else if (task->at == ADDR_TYPE_UNTRANSLATED && task->read_writeAMO == WRITE) {
            task->TTYP = UNTRANSLATED_WRITE_AMO_TRANSACTION;
        } else if (task->at == ADDR_TYPE_TRANSLATED && task->read_writeAMO == READ) {
            task->TTYP = task->pid_valid && task->exec_req ? TRANSLATED_READ_FOR_EXECUTE_TRANSACTION : TRANSLATED_READ_TRANSACTION;
        } else if (task->at == ADDR_TYPE_TRANSLATED && task->read_writeAMO == WRITE) {
            task->TTYP = TRANSLATED_WRITE_AMO_TRANSACTION;
        } else if (task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) {
            task->TTYP = PCIE_ATS_TRANSLATION_REQUEST;
        }

        // Step 3: 检查 ddtp.iommu_mode
        if (iommu_inst.reg_file.ddtp.iommu_mode == Off) {
#ifdef DEBUG_PARSER
            printf("[Parser] Task %d: ddtp.iommu_mode is Off, reporting fault (cause=256)\n", task->task_id);
#endif
            task->state = TASK_FAULT;
            task->cause = 256; // "All inbound transactions disallowed"
            
            // 特殊处理 ATS 翻译请求
            if (task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) {
                // TODO: 发送 ATS 不支持响应
                delete task;
            } else {
                fault_fifo.write(task);
            }
            wait(PARSER_DELAY, SC_NS);
            continue;
        }

        // Step 4: 检查 Bare 模式
        if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_Bare) {
#ifdef DEBUG_PARSER
            printf("[Parser] Task %d: Bare mode detected\n", task->task_id);
#endif
            // Bare 模式下不允许 Translated 或 ATS 请求
            if (task->at == ADDR_TYPE_TRANSLATED || task->at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) {
                task->state = TASK_FAULT;
                task->cause = 260; // "Transaction type disallowed"
                fault_fifo.write(task);
                wait(PARSER_DELAY, SC_NS);
                continue;
            }
            
            // Bare 模式：IOVA = PA 直通
            task->is_bare_mode = 1;
            task->pa = task->iova;
            task->page_sz = PAGESIZE;
            task->vs_pte.X = task->vs_pte.W = task->vs_pte.R = 1;
            task->vs_pte.PBMT = PMA;
            task->g_pte.X = task->g_pte.W = task->g_pte.R = 1;
            task->is_msi = 0;
            task->state = TASK_FORWARD;
            
#ifdef DEBUG_PARSER
            printf("[Parser] Task %d: Bare mode translation complete, pa=0x%lx\n", task->task_id, task->pa);
#endif
            
            pt_cache_to_fwd_fifo.write(task);
            wait(PARSER_DELAY, SC_NS);
            continue;
        }

        // Step 5: 提取 DDI 索引并检查设备 ID 宽度
        uint8_t DDI[3];
        extract_ddi(&iommu_inst, task->device_id, DDI);
        
#ifdef DEBUG_PARSER
        printf("[Parser] Task %d: DDI=[%d,%d,%d], iommu_mode=%d\n", 
               task->task_id, DDI[0], DDI[1], DDI[2], iommu_inst.reg_file.ddtp.iommu_mode);
#endif

        if (!check_device_id_width(&iommu_inst, DDI)) {
#ifdef DEBUG_PARSER
            printf("[Parser] Task %d: device_id width exceeds supported range, cause=260\n", task->task_id);
#endif
            task->state = TASK_FAULT;
            task->cause = 260; // "Transaction type disallowed"
            fault_fifo.write(task);
            wait(PARSER_DELAY, SC_NS);
            continue;
        }

        // Step 6: 正常地址翻译流程
        // 初始化任务状态
        task->state = TASK_PARSE_DONE;
        task->dc_valid = false;
        task->pc_valid = false;
        task->need_pc = false;
        
#ifdef DEBUG_PARSER
        printf("[Parser] Task %d: Sending to collector and cache query\n", task->task_id);
#endif

        // 同时发送到三个 FIFO
        // (1) 发送到 Collector（建立 pending_tasks 跟踪）
        parser_to_collector_fifo.write(task);
        
        // (2) 发送到 DC Cache 进行查询
        parser_to_dc_cache_query_fifo.write(task);
        
        // (3) 发送到 PC Cache 进行查询
        parser_to_pc_cache_query_fifo.write(task);

        wait(PARSER_DELAY, SC_NS);
    }
}
