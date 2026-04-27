// IOMMU Performance Model - xDTW Threads (2 threads)
// SPEC Section 12.8-12.9
// Corresponds to locate_device_context() and locate_process_context()

#include "iommu_top.hh"
#include <cstdio>

// ============================================================
// 12.8 xdtw_req_thread - xDTW Request Thread (async DDR send)
// Initializes walk context, sends first DDR request
// ============================================================
void iommu_top::xdtw_req_thread() {
    while (true) {
        // Flow control: wait for outstanding count below limit
        while (xdtw_outstanding_task_count >= XDTW_MAX_OUTSTANDING_TASKS) {
            wait(xdtw_task_completed_event);
        }

        iommu_task_t* task = collector_to_xdtw_fifo.read();
        task->state = TASK_XDTW_REQ;
        xdtw_outstanding_task_count++;

        wait(XDTW_COMPUTE_DELAY, SC_NS);

        if (task->walk_ctx.walk_type == WALK_DDT) {
            // ========== DDT Walk Initialization ==========
            // Corresponds to iommu_device_context.cc L96-109
            uint64_t a = iommu_inst.reg_file.ddtp.ppn * PAGESIZE;
            uint8_t LEVELS;
            if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_3LVL) LEVELS = 3;
            else if (iommu_inst.reg_file.ddtp.iommu_mode == DDT_2LVL) LEVELS = 2;
            else LEVELS = 1;

            task->walk_ctx.max_levels = LEVELS;
            task->walk_ctx.level = LEVELS - 1;
            task->walk_ctx.base_addr = a;
            task->walk_ctx.indexes[0] = task->DDI[0];
            task->walk_ctx.indexes[1] = task->DDI[1];
            task->walk_ctx.indexes[2] = task->DDI[2];

            if (LEVELS > 1) {
                task->walk_ctx.walk_phase = XDTW_DDT_NON_LEAF;
                uint64_t read_addr = a + (task->DDI[LEVELS - 1] * 8);
                task->walk_ctx.read_addr = read_addr;
                task->walk_ctx.read_size = 8;
            } else {
                // 1-level DDT: directly read DC
                task->walk_ctx.walk_phase = XDTW_DDT_READ_DC;
                uint8_t DC_SIZE = (iommu_inst.reg_file.capabilities.msi_flat == 1) ?
                                  EXT_FORMAT_DC_SIZE : BASE_FORMAT_DC_SIZE;
                uint64_t dc_addr = a + (task->DDI[0] * DC_SIZE);
                task->walk_ctx.read_addr = dc_addr;
                task->walk_ctx.read_size = DC_SIZE;
            }
        }
        else if (task->walk_ctx.walk_type == WALK_PDT) {
            // ========== PDT Walk Initialization ==========
            // Corresponds to iommu_process_context.cc L35-78
            uint16_t PDI[3];
            PDI[0] = get_bits(7,  0, task->process_id);
            PDI[1] = get_bits(16, 8, task->process_id);
            PDI[2] = get_bits(19, 17, task->process_id);

            uint64_t a = task->DC.fsc.pdtp.PPN * PAGESIZE;
            uint8_t LEVELS;
            if (task->DC.fsc.pdtp.MODE == PD20) LEVELS = 3;
            else if (task->DC.fsc.pdtp.MODE == PD17) LEVELS = 2;
            else LEVELS = 1;

            task->walk_ctx.max_levels = LEVELS;
            task->walk_ctx.level = LEVELS - 1;
            task->walk_ctx.base_addr = a;
            task->walk_ctx.indexes[0] = PDI[0];
            task->walk_ctx.indexes[1] = PDI[1];
            task->walk_ctx.indexes[2] = PDI[2];

            if (LEVELS > 1) {
                task->walk_ctx.walk_phase = XDTW_PDT_NON_LEAF;
                uint64_t read_addr = a + PDI[LEVELS - 1] * 8;
                task->walk_ctx.read_addr = read_addr;
                task->walk_ctx.read_size = 8;
            } else {
                task->walk_ctx.walk_phase = XDTW_PDT_READ_PC;
                uint64_t pc_addr = a + PDI[0] * 16;
                task->walk_ctx.read_addr = pc_addr;
                task->walk_ctx.read_size = 16;
            }
        }

        // Register task in active_walks
        xdtw_walks_mtx.lock();
        xdtw_active_walks[task->task_id] = task;
        xdtw_walks_mtx.unlock();

        // Send first DDR request
        ddr_req_entry_t req;
        req.task_id = task->task_id;
        req.addr = task->walk_ctx.read_addr;
        req.size = task->walk_ctx.read_size;
        req.is_write = false;
        task->walk_ctx.ddr_read_count = 1;

        printf("[XDTW_REQ] task_id=%u, walk_type=%s, level=%d, addr=0x%lx, size=%d, read_count=1 -> ddr_req\n",
               task->task_id, (task->walk_ctx.walk_type == WALK_DDT) ? "DDT" : "PDT",
               task->walk_ctx.level, req.addr, req.size);
        fflush(stdout);

        xdtw_req_ddr_fifo.write(req);
    }
}

// ============================================================
// 12.9 xdtw_rsp_thread - xDTW Response Thread (state machine)
// Processes DDR responses, advances walk or completes
// ============================================================
void iommu_top::xdtw_rsp_thread() {
    while (true) {
        ddr_rsp_entry_t rsp = xdtw_rsp_ddr_fifo.read();
        wait(XDTW_PARSE_DELAY, SC_NS);

        // Find corresponding task
        xdtw_walks_mtx.lock();
        auto it = xdtw_active_walks.find(rsp.task_id);
        if (it == xdtw_active_walks.end()) {
            xdtw_walks_mtx.unlock();
            printf("[XDTW] ERROR: task_id %u not found in active_walks\n", rsp.task_id);
            continue;
        }
        iommu_task_t* task = it->second;
        xdtw_walks_mtx.unlock();

        // Copy DDR response data
        memcpy(task->walk_ctx.read_buf, rsp.data, rsp.data_length);

        bool walk_complete = false;
        bool walk_fault = false;
        bool need_next_ddr = false;

        switch (task->walk_ctx.walk_phase) {

        case XDTW_DDT_NON_LEAF: {
            // DDT non-leaf level processing
            // Corresponds to iommu_device_context.cc L112-169
            printf("[XDTW_RSP] task_id=%u, DDT_NON_LEAF: read_count=%u, level=%d, addr=0x%lx\n",
                   task->task_id, task->walk_ctx.ddr_read_count, task->walk_ctx.level, task->walk_ctx.read_addr);
            fflush(stdout);

            ddte_t ddte;
            ddte.raw = 0;
            memcpy(&ddte.raw, task->walk_ctx.read_buf, 8);

            if (ddte.V == 0) {
                task->cause = 258;  // DDT entry not valid
                walk_fault = true;
                break;
            }
            if (ddte.reserved0 != 0 || ddte.reserved1 != 0) {
                task->cause = 259;  // DDT entry misconfigured
                walk_fault = true;
                break;
            }

            task->walk_ctx.base_addr = ddte.PPN * PAGESIZE;
            task->walk_ctx.level--;

            if (task->walk_ctx.level > 0) {
                // More non-leaf levels: continue walk
                uint64_t next_addr = task->walk_ctx.base_addr +
                    (task->walk_ctx.indexes[task->walk_ctx.level] * 8);
                task->walk_ctx.read_addr = next_addr;
                task->walk_ctx.read_size = 8;
                task->walk_ctx.ddr_read_count++;
                need_next_ddr = true;
                printf("[XDTW_RSP] task_id=%u, DDT_NON_LEAF -> next level=%d, read_count=%u, addr=0x%lx\n",
                       task->task_id, task->walk_ctx.level, task->walk_ctx.ddr_read_count, next_addr);
                fflush(stdout);
            } else {
                // Reached leaf level: read DC next
                task->walk_ctx.walk_phase = XDTW_DDT_READ_DC;
                uint8_t DC_SIZE = (iommu_inst.reg_file.capabilities.msi_flat == 1) ?
                                  EXT_FORMAT_DC_SIZE : BASE_FORMAT_DC_SIZE;
                uint64_t dc_addr = task->walk_ctx.base_addr +
                    (task->walk_ctx.indexes[0] * DC_SIZE);
                task->walk_ctx.read_addr = dc_addr;
                task->walk_ctx.read_size = DC_SIZE;
                task->walk_ctx.ddr_read_count++;
                need_next_ddr = true;
                printf("[XDTW_RSP] task_id=%u, DDT_NON_LEAF -> READ_DC (read_count=%u), dc_addr=0x%lx, DC_SIZE=%d\n",
                       task->task_id, task->walk_ctx.ddr_read_count, dc_addr, DC_SIZE);
                fflush(stdout);
            }
            break;
        }

        case XDTW_DDT_READ_DC: {
            // Read and validate DC
            // Corresponds to iommu_device_context.cc L182-228
            uint8_t DC_SIZE = task->walk_ctx.read_size;
            printf("[XDTW_RSP] task_id=%u, DDT_READ_DC: read_count=%u, addr=0x%lx, DC_SIZE=%d\n",
                   task->task_id, task->walk_ctx.ddr_read_count, task->walk_ctx.read_addr, DC_SIZE);
            fflush(stdout);
            memcpy(&task->DC, task->walk_ctx.read_buf, DC_SIZE);

            if (task->DC.tc.V == 0) {
                task->cause = 258;  // DDT entry not valid
                walk_fault = true;
            } else if (do_device_context_configuration_checks(&iommu_inst, &task->DC)) {
                task->cause = 259;  // DDT entry misconfigured
                walk_fault = true;
            } else {
                task->state = TASK_XDTW_DONE;
                walk_complete = true;
            }
            break;
        }

        case XDTW_PDT_NON_LEAF: {
            // PDT non-leaf level processing
            // Corresponds to iommu_process_context.cc L81-157
            printf("[XDTW_RSP] task_id=%u, PDT_NON_LEAF: read_count=%u, level=%d, addr=0x%lx\n",
                   task->task_id, task->walk_ctx.ddr_read_count, task->walk_ctx.level, task->walk_ctx.read_addr);
            fflush(stdout);

            pdte_t pdte;
            pdte.raw = 0;
            memcpy(&pdte.raw, task->walk_ctx.read_buf, 8);

            if (pdte.V == 0) {
                task->cause = 266;  // PDT entry not valid
                walk_fault = true;
                break;
            }
            if (pdte.reserved0 || pdte.reserved1) {
                task->cause = 267;  // PDT entry misconfigured
                walk_fault = true;
                break;
            }

            task->walk_ctx.base_addr = pdte.PPN * PAGESIZE;
            task->walk_ctx.level--;

            if (task->walk_ctx.level > 0) {
                uint64_t next_addr = task->walk_ctx.base_addr +
                    task->walk_ctx.indexes[task->walk_ctx.level] * 8;
                task->walk_ctx.read_addr = next_addr;
                task->walk_ctx.read_size = 8;
                task->walk_ctx.ddr_read_count++;
                need_next_ddr = true;
                printf("[XDTW_RSP] task_id=%u, PDT_NON_LEAF -> next level=%d, read_count=%u, addr=0x%lx\n",
                       task->task_id, task->walk_ctx.level, task->walk_ctx.ddr_read_count, next_addr);
                fflush(stdout);
            } else {
                // Read final PC
                task->walk_ctx.walk_phase = XDTW_PDT_READ_PC;
                uint64_t pc_addr = task->walk_ctx.base_addr +
                    task->walk_ctx.indexes[0] * 16;
                task->walk_ctx.read_addr = pc_addr;
                task->walk_ctx.read_size = 16;
                task->walk_ctx.ddr_read_count++;
                need_next_ddr = true;
                printf("[XDTW_RSP] task_id=%u, PDT_NON_LEAF -> READ_PC (read_count=%u), pc_addr=0x%lx\n",
                       task->task_id, task->walk_ctx.ddr_read_count, pc_addr);
                fflush(stdout);
            }
            break;
        }

        case XDTW_PDT_GS_IMPLICIT: {
            // PDT G-stage implicit translation
            // Simplified: restore PDT walk context after G-stage translation
            printf("[XDTW_RSP] task_id=%u, PDT_GS_IMPLICIT: read_count=%u\n",
                   task->task_id, task->walk_ctx.ddr_read_count);
            fflush(stdout);
            task->walk_ctx.walk_phase = XDTW_PDT_NON_LEAF;
            task->walk_ctx.ddr_read_count++;
            need_next_ddr = true;
            break;
        }

        case XDTW_PDT_READ_PC: {
            // Read and validate PC
            // Corresponds to iommu_process_context.cc L159-196
            printf("[XDTW_RSP] task_id=%u, PDT_READ_PC: read_count=%u, addr=0x%lx\n",
                   task->task_id, task->walk_ctx.ddr_read_count, task->walk_ctx.read_addr);
            fflush(stdout);
            memcpy(&task->PC, task->walk_ctx.read_buf, 16);

            if (task->PC.ta.V == 0) {
                task->cause = 266;  // PDT entry not valid
                walk_fault = true;
            } else if (do_process_context_configuration_checks(
                           &iommu_inst, &task->DC, &task->PC)) {
                task->cause = 267;  // PDT entry misconfigured
                walk_fault = true;
            } else {
                task->state = TASK_XDTW_DONE;
                walk_complete = true;
            }
            break;
        }

        } // end switch

        if (walk_fault) {
            task->state = TASK_FAULT;
            xdtw_walks_mtx.lock();
            xdtw_active_walks.erase(rsp.task_id);
            xdtw_walks_mtx.unlock();
            xdtw_outstanding_task_count--;
            xdtw_task_completed_event.notify(SC_ZERO_TIME);
            xdtw_to_collector_fifo.write(task);
        }
        else if (walk_complete) {
            printf("[XDTW_RSP] task_id=%u, walk_type=%s -> DONE, total_ddr_reads=%u, return to collector\n",
                   task->task_id, (task->walk_ctx.walk_type == WALK_DDT) ? "DDT" : "PDT",
                   task->walk_ctx.ddr_read_count);
            fflush(stdout);
            xdtw_walks_mtx.lock();
            xdtw_active_walks.erase(rsp.task_id);
            xdtw_walks_mtx.unlock();
            xdtw_outstanding_task_count--;
            xdtw_task_completed_event.notify(SC_ZERO_TIME);
            xdtw_to_collector_fifo.write(task);
        }
        else if (need_next_ddr) {
            ddr_req_entry_t req;
            req.task_id = task->task_id;
            req.addr = task->walk_ctx.read_addr;
            req.size = task->walk_ctx.read_size;
            req.is_write = false;
            xdtw_req_ddr_fifo.write(req);
        }
    }
}
