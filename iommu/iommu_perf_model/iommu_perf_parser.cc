// IOMMU Performance Model - Parser Thread
// Corresponds to iommu_translate_iova() steps 1-5
// SPEC Section 12.1

#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>

void iommu_top::parser_thread() {
    // [读写端口分离] 并发门控已前移到端口准入(axi_slave_r_nb_transport_fw /
    //   axi_slave_w_nb_transport_fw): 端口先门控
    //   (自身方向 outstanding < 上限 且 global < 上限)再 ++ 计数, 故 parser 不再做
    //   global/方向 outstanding 反压; parser_blk_by_global/parser_blk_dir_* 字段保留但不再累加。
    //   弹出策略: 读写轮询(RR), 从上次弹出方向的另一侧开始尝试非阻塞 nb_read,
    //   该侧空则试另一侧, 两侧都空则 wait 任一 inbound_fifo 的 data_written_event。
    bool prefer_read = true;   // RR 交替标志: 本次优先尝试的方向
    while (true) {
        // [读写端口分离] 下游有空位才弹出: 避免 parser 弹出后阻塞在下游写满,
        //   卡死另一方向(残余下游串行属流水线物理约束, 由端口阻塞统计归因)。
        if (parser_to_collector_fifo.num_free() == 0 ||
            cache_sub.dc_request_fifo.num_free() == 0 ||
            cache_sub.pc_request_fifo.num_free() == 0) {
            wait(parser_to_collector_fifo.data_read_event() |
                 cache_sub.dc_request_fifo.data_read_event() |
                 cache_sub.pc_request_fifo.data_read_event());
            continue;
        }

        // 1. RR 非阻塞弹出: 先试优先方向, 空则试另一方向
        iommu_task_t* task = nullptr;
        bool from_write = false;
        if (prefer_read) {
            if      (inbound_fifo_r.nb_read(task)) from_write = false;
            else if (inbound_fifo_w.nb_read(task)) from_write = true;
        } else {
            if      (inbound_fifo_w.nb_read(task)) from_write = true;
            else if (inbound_fifo_r.nb_read(task)) from_write = false;
        }
        if (!task) {
            // 两侧都空: 等待任一方向写入 inbound_fifo_r/w
            wait(inbound_fifo_r.data_written_event() | inbound_fifo_w.data_written_event());
            continue;
        }
        prefer_read = from_write;   // 下次从本次弹出方向的另一侧开始尝试
#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::BusyGuard md_busy(md_trace.active);
        md_trace.stamp(task->task_id, iommu_md::Point::POP);
#endif
        task->timestamp = sc_time_stamp();  // [STAT] 记录IO入口时刻
        printf("[PARSER] task_id=%u popped from inbound_fifo_%c (RR)\n",
               task->task_id, from_write ? 'w' : 'r');
        fflush(stdout);

        // [读写端口分离] is_write 仅用于门控快照分类(反压已在端口完成)
        bool is_write = (task->read_writeAMO == WRITE);

        // ===== [STAT] 门控快照: 收到该请求时的读/写/全局并发数目 =====
        //   采样于 RR 弹出之后(门控已前移到端口), 用于对比改造前后 parser 视角的并发分布
        {
            const int r_out = iommu_read_outstanding;
            const int w_out = iommu_write_outstanding;
            const int g_out = iommu_global_outstanding;
            const bool r_full = (r_out >= (int)IOMMU_READ_MAX_OUTSTANDING);
            const bool w_full = (w_out >= (int)IOMMU_WRITE_MAX_OUTSTANDING);
            const bool g_full = (g_out >= (int)IOMMU_GLOBAL_MAX_OUTSTANDING);
            const bool self_full  = is_write ? w_full : r_full;
            const bool other_full = is_write ? r_full : w_full;

            parser_recv_total++;
            if (is_write) parser_recv_write++; else parser_recv_read++;
            if (r_full) parser_snap_read_full++;
            if (w_full) parser_snap_write_full++;
            if (r_full || w_full) parser_snap_either_full++;
            if (r_full && w_full) parser_snap_both_full++;
            if (g_full) parser_snap_global_full++;
            if (self_full) parser_snap_selfdir_full++;
            // 纯 HOL 受害: 自身方向有空位, 但因队头是另一方向且其已满而进不来
            //   (在本采样点上表现为: 自身未满但另一方向满, 说明读写额度不对称)
            if (other_full && !self_full) parser_snap_otherdir_full++;
            parser_snap_read_sum   += r_out;
            parser_snap_write_sum  += w_out;
            parser_snap_global_sum += g_out;

            printf("[PARSER_CONC] t=%.1f task_id=%u dir=%s read_out=%d write_out=%d "
                   "global_out=%d R_FULL=%d W_FULL=%d G_FULL=%d EITHER_FULL=%d "
                   "SELF_FULL=%d OTHER_FULL=%d\n",
                   sc_time_stamp().to_seconds() * 1e9, task->task_id,
                   is_write ? "W" : "R", r_out, w_out, g_out,
                   r_full ? 1 : 0, w_full ? 1 : 0, g_full ? 1 : 0,
                   (r_full || w_full) ? 1 : 0, self_full ? 1 : 0, other_full ? 1 : 0);
            fflush(stdout);
        }

        // [读写端口分离] 方向门控已前移到端口准入(见 axi_slave_r/w_nb_transport_fw),
        //   parser 不再 wait 方向 outstanding; parser_blk_by_read/write 保持 0(改造生效证据)。

        // 1.1 入口注册到 reorder buffer(outstanding 计数已在端口准入完成, 此处不再++)
        reorder_register_task(task);
#if TEST_CFG_MULTI_DEVICE_SCENE
        md_trace.stamp(task->task_id, iommu_md::Point::ADMIT);
#endif

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
