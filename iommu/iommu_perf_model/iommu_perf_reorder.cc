// IOMMU Performance Model - Output Reorder Buffer
// ------------------------------------------------------------------
// 出口重排序：所有完成翻译（含 fault）的 task 在 forwarder/fault thread
// 中不再直接调用 send_response_to_initiator，而是先标记到 reorder_buf。
// reorder_output_thread 按以下规则消费 reorder_buf：
//   - 写请求(read_writeAMO == WRITE)：按 task_id 单调保序输出（FIFO）
//   - 读请求(read_writeAMO == READ) ：到达即输出（可乱序）
//   - 读 / 写 互不影响：读到达不会等待写头部
// 所有响应通过 send_response_to_initiator 真正下发到 RP 之后，
// IOMMU 全局 outstanding 才会释放（iommu_global_outstanding--）。
// ------------------------------------------------------------------

#include "iommu_top.hh"
#include "iommu_req_rsp.hh"
#include <cstdio>
#include <vector>

// ------------------------------------------------------------------
// reorder_register_task
//   入口（parser_thread）从 inbound_fifo 读出 task 后调用：
//   1) 在 reorder_buf 注册条目（ready=false）
//   2) 写请求按到达顺序入 reorder_write_order
//   3) iommu_global_outstanding++
// 注意：不做反压等待，反压由 parser_thread 在调用本函数前完成。
// ------------------------------------------------------------------
void iommu_top::reorder_register_task(iommu_task_t* task) {
    if (!task) return;

    bool is_write = (task->read_writeAMO == WRITE);

    reorder_mtx.lock();
    reorder_entry_t entry;
    entry.task = task;
    entry.ready = false;
    entry.is_write = is_write;
    reorder_buf[task->task_id] = entry;
    if (is_write) {
        reorder_write_order.push(task->task_id);
    }
    iommu_global_outstanding++;
    if (iommu_global_outstanding > peak_iommu_global_outstanding)
        peak_iommu_global_outstanding = iommu_global_outstanding;
    reorder_mtx.unlock();

    printf("[REORDER] register task_id=%u, %s, global_outstanding=%d\n",
           task->task_id, is_write ? "WRITE" : "READ",
           iommu_global_outstanding);
    fflush(stdout);
}

// ------------------------------------------------------------------
// reorder_mark_ready
//   出口（pt_forwarder / msipt_forwarder / fault_cq）调用：
//   将翻译完成的 task 在 reorder_buf 标记为 ready，notify 输出线程。
//   注意：调用方完成时不再 delete task，task 由 reorder_output_thread 释放。
// ------------------------------------------------------------------
void iommu_top::reorder_mark_ready(iommu_task_t* task) {
    if (!task) return;

    reorder_mtx.lock();
    auto it = reorder_buf.find(task->task_id);
    if (it == reorder_buf.end()) {
        // 未注册：可能来自 b_transport 旁路或异常路径，直接旁路输出。
        reorder_mtx.unlock();
        printf("[REORDER] WARN: task_id=%u not registered, bypass-send\n",
               task->task_id);
        fflush(stdout);
        send_response_to_initiator(task);
        if (!task->is_b_transport) delete task;
        else task->state = TASK_DONE;
        return;
    }
    it->second.ready = true;
    reorder_mtx.unlock();

    printf("[REORDER] mark_ready task_id=%u, %s\n",
           task->task_id,
           it->second.is_write ? "WRITE" : "READ");
    fflush(stdout);

    reorder_ready_event.notify(SC_ZERO_TIME);
}

// ------------------------------------------------------------------
// reorder_output_thread
//   消费 reorder_buf：
//     1) 输出所有 ready 的读请求（顺序无关）
//     2) 推进写请求队头（reorder_write_order.front 的 task ready 时输出）
//   每次输出后释放 iommu_global_outstanding。
// ------------------------------------------------------------------
void iommu_top::reorder_output_thread() {
    while (true) {
        // 收集本轮可输出的 task（在锁内）
        std::vector<iommu_task_t*> to_send;

        reorder_mtx.lock();

        // 1) 读请求：所有 ready 的读请求即可输出
        for (auto it = reorder_buf.begin(); it != reorder_buf.end(); ) {
            if (it->second.ready && !it->second.is_write) {
                to_send.push_back(it->second.task);
                it = reorder_buf.erase(it);
            } else {
                ++it;
            }
        }

        // 2) 写请求：按 reorder_write_order 队头严格保序
        while (!reorder_write_order.empty()) {
            uint32_t head_id = reorder_write_order.front();
            auto it = reorder_buf.find(head_id);
            if (it == reorder_buf.end()) {
                // 异常防护：队列与 buf 不一致，丢弃队头继续
                printf("[REORDER] WARN: write head task_id=%u not in buf, drop\n", head_id);
                fflush(stdout);
                reorder_write_order.pop();
                continue;
            }
            if (it->second.ready) {
                to_send.push_back(it->second.task);
                reorder_buf.erase(it);
                reorder_write_order.pop();
            } else {
                // 队头未 ready，必须等待，不可越过（写保序）
                break;
            }
        }

        reorder_mtx.unlock();

        // 出锁后真实下发响应
        for (iommu_task_t* task : to_send) {
            if (!task) continue;

            // axi_master_0 端口并发流控：等待slot释放
            while (axi_master_0_to_pcie_noc_outstanding >= (int)AXI_MASTER_0_TO_PCIE_NOC_MAX_OUTSTANDING) {
                wait(axi_master_0_slot_freed_event);
            }
            axi_master_0_to_pcie_noc_outstanding++;
            if (axi_master_0_to_pcie_noc_outstanding > peak_axi_master_0_outstanding)
                peak_axi_master_0_outstanding = axi_master_0_to_pcie_noc_outstanding;

            printf("[REORDER] output task_id=%u, %s, pa=0x%lx, axi_master_0_out=%d\n",
                   task->task_id,
                   (task->read_writeAMO == WRITE) ? "WRITE" : "READ",
                   task->pa,
                   axi_master_0_to_pcie_noc_outstanding);
            fflush(stdout);

            // [PERF] reorder_output 无需串行延时，出口速率由 master_0 带宽模型控制
            send_response_to_initiator(task);
            iommu_total_completed++;  // [STAT] IOMMU 完成翻译计数
            // [STAT] 累加端到端延时 (parser入口 -> reorder出口)
            double e2e_ns = (sc_time_stamp() - task->timestamp).to_seconds() * 1e9;
            iommu_total_e2e_latency_ns += e2e_ns;

            // [STAT] 稳态IOPS采样：记录稳态开始/结束时刻
            if (steady_start_ns == 0.0 && steady_start_count > 0 &&
                iommu_total_completed >= steady_start_count) {
                steady_start_ns = sc_time_stamp().to_seconds() * 1e9;
                ptw_steady_start_completed = ptw_total_completed;
            }
            if (steady_end_ns == 0.0 && steady_end_count > 0 &&
                iommu_total_completed >= steady_end_count) {
                steady_end_ns = sc_time_stamp().to_seconds() * 1e9;
                ptw_steady_end_completed = ptw_total_completed;
            }

            // ===== Bandwidth control: master_0 port (翻译输出) =====
            // delay_ns = 1000 * length * 8 / bandwidth_mbps
            unsigned int resp_data_len = task->tlm_trans_ptr ? task->tlm_trans_ptr->get_data_length() : 16;
            master_0_total_bytes += resp_data_len;  // [STAT] 出口字节计数
            double master0_bw_delay_ns = 1000.0 * resp_data_len * 8 / AXI_MASTER_0_BANDWIDTH_MBPS;
            wait(master0_bw_delay_ns, SC_NS);

            // 收到下游响应（当前模型为同步），释放slot
            axi_master_0_to_pcie_noc_outstanding--;
            axi_master_0_slot_freed_event.notify(SC_ZERO_TIME);

            // 标记完成
            task->state = TASK_DONE;

            // 释放 IOMMU 全局 outstanding
            reorder_mtx.lock();
            if (iommu_global_outstanding > 0) iommu_global_outstanding--;
            reorder_mtx.unlock();
            iommu_global_outstanding_freed_event.notify(SC_ZERO_TIME);

            // task 释放（b_transport 路径由发起方释放）
            if (!task->is_b_transport) {
                delete task;
            }
        }

        // 若本轮无任何输出，等待下一次 ready 通知
        if (to_send.empty()) {
            wait(reorder_ready_event);
        }
    }
}
