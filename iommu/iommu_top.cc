#include "iommu_top.hh"
#include "iommu_task_cache_convert.hh"
#include <cstdio>
#include <iostream>

// ===================== Cache命中率统计全局变量定义 =====================
uint64_t g_dc_cache_hit_count = 0;
uint64_t g_dc_cache_miss_count = 0;
uint64_t g_pc_cache_hit_count = 0;
uint64_t g_pc_cache_miss_count = 0;
uint64_t g_pt_cache_hit_count = 0;
uint64_t g_pt_cache_miss_count = 0;
uint64_t g_msipt_cache_hit_count = 0;
uint64_t g_msipt_cache_miss_count = 0;

// ===================== [失效] CQ->性能模型失效桥接 =====================
// 由功能模型 iommu_command_queue.cc 的 do_inval_*/do_iotinval_* 调用。
// 将失效命令构造为 CacheMessage 送入 CacheSubsystem 失效 pipeline,
// 并同步阻塞等待完成(保证后续命令/IOFENCE 前失效已生效)。
// 基线场景不下发失效命令, 此路径不执行。
void perf_enqueue_cache_invalidation(iommu_t *iommu, int cmd_type,
    uint8_t dv, uint32_t did, uint32_t pid, uint8_t gv, uint8_t av,
    uint8_t pscv, uint8_t nl, uint32_t gscid, uint32_t pscid, uint64_t addr_pn) {
    if (iommu == nullptr || iommu->top == nullptr) return;
    iommu_top* top = iommu->top;
    if (!top->cache_sub.invalidation_enabled()) return;
    // 必须在 SystemC 进程上下文(阻塞读写 FIFO)
    if (!sc_get_current_process_handle().valid()) return;

    iommu::CacheMessage cmd;
    cmd.msg_type = iommu::CacheMsgType::CACHE_INVALIDATE_RESPONSE;  // 仅占位
    cmd.task_id = 0;
    switch (cmd_type) {
        case 0:  // IODIR.INVAL_DDT
            cmd.cmd_type = iommu::InvalidCmdType::IODIR_INVAL_DDT;
            cmd.has_device_id = (dv != 0);
            cmd.device_id = did;
            break;
        case 1:  // IODIR.INVAL_PDT
            cmd.cmd_type = iommu::InvalidCmdType::IODIR_INVAL_PDT;
            cmd.has_device_id = true;
            cmd.device_id = did;
            cmd.has_process_id = true;
            cmd.process_id = pid;
            break;
        case 2:  // IOTINVAL.VMA
            cmd.cmd_type = iommu::InvalidCmdType::IOTINVAL_VMA;
            cmd.has_gscid = (gv != 0);
            cmd.has_pscid = (pscv != 0);
            cmd.has_iova = (av != 0);
            cmd.gscid = static_cast<iommu::gscid_t>(gscid);
            cmd.pscid = static_cast<iommu::pscid_t>(pscid);
            cmd.iova = addr_pn << 12;
            cmd.inval_nl = (nl != 0);
            break;
        case 3:  // IOTINVAL.GVMA
            cmd.cmd_type = iommu::InvalidCmdType::IOTINVAL_GVMA;
            cmd.has_gscid = (gv != 0);
            cmd.has_iova = (av != 0);
            cmd.gscid = static_cast<iommu::gscid_t>(gscid);
            cmd.iova = addr_pn << 12;
            cmd.inval_nl = (nl != 0);
            break;
        default:
            return;
    }
    // 送入失效 pipeline 并同步等待完成
    top->cache_sub.invalidation_request_fifo.write(cmd);
    top->cache_sub.invalidation_response_fifo.read();
}

/**********************************************/
// before_end_of_elaboration - IOMMU reset and initialization
/**********************************************/
void iommu_top::before_end_of_elaboration()
{
    capabilities_t cap = {0};
    fctl_t fctl = {0};
    uint64_t sv57_bare_sz, sv48_bare_sz, sv39_bare_sz, sv32_bare_sz;
    uint64_t sv57x4_bare_sz, sv48x4_bare_sz, sv39x4_bare_sz, sv32x4_bare_sz;

    // Reset the IOMMU
    cap.version = 0x10;
    cap.Sv39 = cap.Sv48 = cap.Sv57 = cap.Sv39x4 = cap.Sv48x4 = cap.Sv57x4 = 1;
    cap.amo_hwad = cap.ats = cap.t2gpa = cap.hpm = cap.msi_flat = cap.msi_mrif = cap.amo_mrif = 1;
    cap.dbg = 1;
    // [失效] 声明支持非叶PTE失效扩展(capabilities.NL=1):
    // 使 IOTINVAL.VMA/GVMA 的 NL 位(bit34)成为有效域, 从而让
    // execute_invalidation_pipeline 中 "NL=1 时联动 Walker Cache 全级别失效"
    // 的路径可达。未声明时 CQ 会将 NL=1 的命令判为 command_illegal。
    // 注: capabilities.S(地址范围/NAPOT 失效扩展)仍为 0, 性能模型未实现
    // ADDR 范围展开, 保持 S=1 命令被判非法, 与能力声明自洽。
    cap.nl = 1;
    cap.pas = 50;
    cap.pd20 = cap.pd17 = cap.pd8 = 1;
    cap.Svrsw60t59b = 1;
    sv57_bare_sz = sv48_bare_sz = sv39_bare_sz = 0x40000000;
    sv32_bare_sz = 0x200000;
    sv57x4_bare_sz = sv48x4_bare_sz = sv39x4_bare_sz = 0x40000000;
    sv32x4_bare_sz = 0x200000;
    int reset_result = reset_iommu(&iommu_inst, 8, 40, 0xff, 3, Off, DDT_3LVL, 0xFFFFFF, 0, 0,
                           (FILL_IOATC_ATS_T2GPA | FILL_IOATC_ATS_ALWAYS),
                           cap, fctl, sv57_bare_sz, sv48_bare_sz, sv39_bare_sz,
                           sv32_bare_sz, sv57x4_bare_sz, sv48x4_bare_sz, sv39x4_bare_sz,
                           sv32x4_bare_sz);
    if (reset_result < 0) {
        printf("\x1B[31mFAIL. Line %d\x1B[0m\n", __LINE__);
    }

    iommu_inst.reg_file.ddtp.iommu_mode = DDT_1LVL;
}

/**********************************************/
// AHB slave b_transport - register access (non-performance path)
/**********************************************/
void iommu_top::ahb_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay)
{
    tlm::tlm_command cmd = trans.get_command();
    sc_dt::uint64 addr = trans.get_address();
    unsigned char *data = trans.get_data_ptr();
    unsigned int len = trans.get_data_length();

    if (cmd == tlm::TLM_READ_COMMAND)
    {
        uint16_t offset = addr - IOMMU_BASE_ADDR;
        uint8_t num_bytes = 4;
        uint64_t reg_value = read_register(&iommu_inst, offset, num_bytes);
        memcpy(data, (unsigned char *)&reg_value, len);
        printf("%s:%d Read addr 0x%04llx, val 0x%08x\n", __func__, __LINE__,
               (unsigned long long)addr, *(unsigned int *)data);
    }
    else if (cmd == tlm::TLM_WRITE_COMMAND)
    {
        uint64_t val = 0;
        memcpy(&val, data, len);
        uint16_t offset = addr - IOMMU_BASE_ADDR;
        uint8_t num_bytes = 4;
        write_register(&iommu_inst, offset, num_bytes, val);
    }
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

/**********************************************/
// Legacy axi_slave_b_transport - kept for backward compatibility
// In performance model, nb_transport_fw is the primary path
/**********************************************/
void iommu_top::axi_slave_b_transport(tlm::tlm_generic_payload &trans, sc_time &delay)
{
    // In performance model, this path is deprecated.
    // Redirect to nb_transport path by creating a task and pushing to inbound_fifo
    PayloadExtention *ext = nullptr;
    trans.get_extension(ext);
    if (!ext) {
        trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }

    // For backward compatibility with RP test thread that still uses b_transport
    // Create task and process synchronously
    iommu_task_t* task = new iommu_task_t();

    task_id_mtx.lock();
    task->task_id = next_task_id++;
    task_id_mtx.unlock();

    task->tlm_trans_ptr = &trans;
    task->device_id = ext->requester_id;
    task->pid_valid = ext->pid_valid;
    task->process_id = ext->process_id;
    task->exec_req = ext->exec_req;
    task->priv_req = ext->priv_req;
    task->no_write = ext->no_write;
    task->is_cxl_dev = 0;
    task->is_ctrl = ext->is_ctrl;  // [场景13] 控制包标记(SQ/CQ/MSI不计入IOPS)
    task->iova = trans.get_address();
    task->length = trans.get_data_length();
    task->read_writeAMO = (trans.get_command() == tlm::TLM_READ_COMMAND) ? READ : WRITE;

    if (ext->at == 0) task->at = ADDR_TYPE_UNTRANSLATED;
    else if (ext->at == 1) task->at = ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST;
    else if (ext->at == 2) task->at = ADDR_TYPE_TRANSLATED;

    task->timestamp = sc_time_stamp();
    task->state = TASK_INIT;
    task->is_b_transport = 1;

    // Push to inbound FIFO for pipeline processing
    // [读写端口分离] inbound_fifo 已拆为读/写两条, 遗留 b_transport 路径按命令方向分流
    if (task->read_writeAMO == WRITE) inbound_fifo_w.write(task);
    else                              inbound_fifo_r.write(task);

    // For b_transport compatibility, we need to wait for completion
    // The forwarder/fault thread will signal completion through the response event
    // We use a simple polling approach with wait
    while (task->state != TASK_DONE) {
        wait(1, SC_NS);
    }

    trans.set_response_status(tlm::TLM_OK_RESPONSE);
    delete task;
}

/**********************************************/
// [STAT] IOMMU 内部并发 4KB 页统计 (需求1)
//   只在 512Byte 数据翻译任务(is_ctrl=0 && length=512)的入口/出口打点;
//   SQ/CQ/MSI 等控制包(is_ctrl=1)完全不记录。
//   按 device_id 分开维护 4KB 页跟踪表(需求中的 4kb_info 数组), 并发页数 =
//   当前 valid 表项数; 平均值用时间加权 Σ(并发数 × Δt) / 窗口长度 求得,
//   每次并发数变化前先按旧值累加面积。
/**********************************************/
namespace {
// 时间加权面积累加: 用旧并发值 × 距上次变化的时长; last_ns<=0 表示窗口尚未开始
inline void kb4_accum_area(double& area, uint32_t old_conc, double& last_ns, double now_ns) {
    if (last_ns > 0.0 && now_ns > last_ns) area += (double)old_conc * (now_ns - last_ns);
    last_ns = now_ns;
}
// 全设备当前总并发 4KB 页数
inline uint32_t kb4_total_conc(const std::map<uint32_t, iommu_top::kb4_dev_stat_t>& m) {
    uint32_t total = 0;
    for (const auto& kv : m) total += kv.second.cur_conc;
    return total;
}
}  // namespace

// ------------------------------------------------------------------
// track_4kb_page_in - 入口打点(parser 准入点 reorder_register_task, 与 outstanding++ 同点)
//   根据 512B 数据任务的 device_id 与 iova 算出 4KB 页值, 到该设备的跟踪表查询:
//     无匹配 -> 插入表项, page_is_valid=1, page_value=页值, task_count=1, 并发数+1
//     有匹配 -> page_is_valid 应为1, 仅 task_count++(并发数不变)
// ------------------------------------------------------------------
void iommu_top::track_4kb_page_in(iommu_task_t* task) {
    if (!task || task->is_ctrl || task->length != KB4_DATA_LEN) return;  // 只统计512B数据任务

    const double now_ns = sc_time_stamp().to_seconds() * 1e9;
    if (kb4_win_first_ns < 0.0) kb4_win_first_ns = now_ns;  // 首次采样开启窗口

    const uint64_t page = task->iova & ~0xFFFULL;   // iova -> 4KB 页值
    kb4_dev_stat_t& ds = kb4_dev_stats[task->device_id];
    ds.in_tasks++;

    auto it = ds.pages.find(page);
    if (it != ds.pages.end() && it->second.page_is_valid && it->second.page_value == page) {
        it->second.task_count++;   // 已在处理该4KB页: 仅计数, 并发页数不变
        return;
    }

    // 无匹配(或表项已被清零) -> 插入新表项
    if (it == ds.pages.end()) {
        ds.distinct_pages++;
        it = ds.pages.emplace(page, kb4_page_info_t()).first;
    }
    it->second.page_is_valid = true;
    it->second.page_value    = page;
    it->second.task_count    = 1;

    // 并发页数 +1: 先按旧值累加面积(本设备 + 全设备), 再自增并更新峰值
    kb4_accum_area(ds.area_ns, ds.cur_conc, ds.last_change_ns, now_ns);
    const uint32_t old_total = kb4_total_conc(kb4_dev_stats);
    kb4_accum_area(kb4_total_area_ns, old_total, kb4_win_last_change_ns, now_ns);
    ds.cur_conc++;
    if (ds.cur_conc > ds.peak_conc) ds.peak_conc = ds.cur_conc;
    if (old_total + 1 > kb4_total_peak) kb4_total_peak = old_total + 1;
}

// ------------------------------------------------------------------
// track_4kb_page_out - 出口打点(reorder 释放点, 与 outstanding-- 同点)
//   用该512B任务的 iova 算出4KB页值, 到当前设备跟踪表查询:
//     无匹配 -> 异常情况, 计数并打印告警(不中断仿真)
//     有匹配 -> task_count--; 非0则结束; 为0则清零表项(valid=0, value=0), 并发数-1
// ------------------------------------------------------------------
void iommu_top::track_4kb_page_out(iommu_task_t* task) {
    if (!task || task->is_ctrl || task->length != KB4_DATA_LEN) return;  // 控制包不额外做操作

    const double now_ns = sc_time_stamp().to_seconds() * 1e9;
    const uint64_t page = task->iova & ~0xFFFULL;
    kb4_dev_stat_t& ds = kb4_dev_stats[task->device_id];
    ds.out_tasks++;

    auto it = ds.pages.find(page);
    if (it == ds.pages.end() || !it->second.page_is_valid || it->second.page_value != page) {
        // 异常情况: 出口任务对应的4KB页在表中不存在/已无效
        ds.out_no_match++;
        printf("[KB4_ANOMALY] out-no-match: did=%u task_id=%u iova=0x%lx page=0x%lx\n",
               task->device_id, task->task_id, (unsigned long)task->iova, (unsigned long)page);
        fflush(stdout);
        return;
    }

    if (it->second.task_count > 0) it->second.task_count--;
    if (it->second.task_count != 0) return;  // 该4KB页仍有在途512B任务, 不做操作

    // 该4KB页的所有512B任务均已完成 -> 清零表项
    it->second.page_is_valid = false;
    it->second.page_value    = 0;

    // 并发页数 -1: 先按旧值累加面积, 再自减
    kb4_accum_area(ds.area_ns, ds.cur_conc, ds.last_change_ns, now_ns);
    const uint32_t old_total = kb4_total_conc(kb4_dev_stats);
    kb4_accum_area(kb4_total_area_ns, old_total, kb4_win_last_change_ns, now_ns);
    if (ds.cur_conc > 0) ds.cur_conc--;
}

// ------------------------------------------------------------------
// reset_4kb_concurrency_window - 测量 epoch 开始前重置统计窗口
//   调用时已保证 IOMMU 内部无在途任务(quiescent), 故可安全清空跟踪表。
// ------------------------------------------------------------------
void iommu_top::reset_4kb_concurrency_window() {
    const double now_ns = sc_time_stamp().to_seconds() * 1e9;
    kb4_total_area_ns      = 0.0;
    kb4_total_peak         = 0;
    kb4_win_first_ns       = now_ns;
    kb4_win_last_change_ns = now_ns;
    // [STAT] 同时打开"注入窗口"并发统计(RP 全部请求送完后由 conc_window_close 关闭)
    conc_window_open();
    for (auto& kv : kb4_dev_stats) {
        kv.second.pages.clear();
        kv.second.cur_conc        = 0;
        kv.second.peak_conc       = 0;
        kv.second.area_ns         = 0.0;
        kv.second.last_change_ns  = now_ns;
        kv.second.in_tasks        = 0;
        kv.second.out_tasks       = 0;
        kv.second.distinct_pages  = 0;
        kv.second.out_no_match    = 0;
        kv.second.bad_state_cnt   = 0;
    }
}

// ------------------------------------------------------------------
// print_4kb_concurrency_report - 打印每设备与全设备的平均/峰值并发 4KB 页数
// ------------------------------------------------------------------
void iommu_top::print_4kb_concurrency_report() const {
    const double now_ns = sc_time_stamp().to_seconds() * 1e9;
    const double window_ns = (kb4_win_first_ns >= 0.0) ? (now_ns - kb4_win_first_ns) : 0.0;

    printf("\n========== IOMMU 4KB Page Concurrency Statistics (512B data tasks; parser-admit->reorder-release span, bounded by outstanding cap) ==========\n");
    printf("  Sampling window:      %.1f ns (%.3f us)\n", window_ns, window_ns / 1000.0);
    printf("  Tracked devices:      %lu\n", (unsigned long)kb4_dev_stats.size());
    printf("    %-5s %-14s %-10s %-10s %-10s %-10s %-9s %-9s\n",
           "DID", "AvgConc(pages)", "PeakConc", "InTasks", "OutTasks", "DistinctPg", "Residual", "NoMatch");

    double     sum_avg       = 0.0;
    uint64_t   sum_in        = 0, sum_out = 0, sum_distinct = 0, sum_nomatch = 0, sum_residual = 0;
    uint32_t   max_dev_peak  = 0;
    for (const auto& kv : kb4_dev_stats) {
        const kb4_dev_stat_t& ds = kv.second;
        // 收尾: 加上从上次变化到当前仿真时刻的面积
        double area = ds.area_ns;
        if (ds.last_change_ns > 0.0 && now_ns > ds.last_change_ns)
            area += (double)ds.cur_conc * (now_ns - ds.last_change_ns);
        const double avg = (window_ns > 0.0) ? (area / window_ns) : 0.0;
        // 残留未清零表项数(仿真结束时应为0)
        uint64_t residual = 0;
        for (const auto& p : ds.pages) if (p.second.page_is_valid) residual++;

        printf("    %-5u %-14.3f %-10u %-10lu %-10lu %-10lu %-9lu %-9lu\n",
               kv.first, avg, ds.peak_conc,
               (unsigned long)ds.in_tasks, (unsigned long)ds.out_tasks,
               (unsigned long)ds.distinct_pages, (unsigned long)residual,
               (unsigned long)ds.out_no_match);

        sum_avg       += avg;
        sum_in        += ds.in_tasks;
        sum_out       += ds.out_tasks;
        sum_distinct  += ds.distinct_pages;
        sum_nomatch   += ds.out_no_match;
        sum_residual  += residual;
        if (ds.peak_conc > max_dev_peak) max_dev_peak = ds.peak_conc;
    }

    // 全设备总并发(时间加权): 面积收尾后除以同一窗口
    double total_area = kb4_total_area_ns;
    if (kb4_win_last_change_ns > 0.0 && now_ns > kb4_win_last_change_ns)
        total_area += (double)kb4_total_conc(kb4_dev_stats) * (now_ns - kb4_win_last_change_ns);
    const double total_avg = (window_ns > 0.0) ? (total_area / window_ns) : 0.0;

    const uint32_t dedup_lines = cache_sub.get_dedup_total_lines();
    printf("    ---------------------------------------------------------------------------\n");
    printf("    %-5s %-14.3f %-10u %-10lu %-10lu %-10lu %-9lu %-9lu\n",
           "TOTAL", total_avg, kb4_total_peak,
           (unsigned long)sum_in, (unsigned long)sum_out,
           (unsigned long)sum_distinct, (unsigned long)sum_residual,
           (unsigned long)sum_nomatch);
    printf("  Cross-check (Σ per-device avg): %.3f pages\n", sum_avg);
    printf("  Max single-device peak:         %u pages\n", max_dev_peak);
    if (dedup_lines > 0) {
        printf("  Dedup Cache capacity:           %u cachelines (sets=%u x ways=%u)\n",
               dedup_lines, cache_sub.get_dedup_num_sets(), cache_sub.get_dedup_num_ways());
        printf("  AvgConc / Dedup capacity:       %.2f%%  (并发4KB页数 vs 去重Cache总行数)\n",
               100.0 * total_avg / (double)dedup_lines);
        printf("  PeakConc / Dedup capacity:      %.2f%%\n",
               100.0 * (double)kb4_total_peak / (double)dedup_lines);
    }
    // [STAT需求1-诊断] 入口未决队列积压: 门控已前移到端口, 此积压 = 已准入但尚未被 parser 弹出
    {
        const double inq_window = (input_q_first_ns >= 0.0) ? (now_ns - input_q_first_ns) : 0.0;
        double inq_area = input_q_area_ns;
        if (input_q_last_ns > 0.0 && now_ns > input_q_last_ns)
            inq_area += (double)input_q_count * (now_ns - input_q_last_ns);
        const double inq_avg = (inq_window > 0.0) ? (inq_area / inq_window) : 0.0;
        // [读写端口分离] 按方向拆分的时间加权平均
        const double inq_dir_window = (input_q_dir_first_ns >= 0.0) ? (now_ns - input_q_dir_first_ns) : 0.0;
        double inq_r_area = input_q_r_area_ns, inq_w_area = input_q_w_area_ns;
        if (input_q_dir_last_ns > 0.0 && now_ns > input_q_dir_last_ns) {
            inq_r_area += (double)input_q_r_count * (now_ns - input_q_dir_last_ns);
            inq_w_area += (double)input_q_w_count * (now_ns - input_q_dir_last_ns);
        }
        const double inq_r_avg = (inq_dir_window > 0.0) ? (inq_r_area / inq_dir_window) : 0.0;
        const double inq_w_avg = (inq_dir_window > 0.0) ? (inq_w_area / inq_dir_window) : 0.0;
        printf("  ----------------------------------------------------------------------\n");
        printf("  Input-queue backlog (input_delay_queue_r/w, 位于端口准入之后、parser弹出之前):\n");
        printf("    peak tasks queued:              %u  (read %u / write %u)\n",
               input_q_peak, input_q_r_peak, input_q_w_peak);
        printf("    time-weighted avg tasks queued: %.1f  (read %.1f / write %.1f)\n",
               inq_avg, inq_r_avg, inq_w_avg);
        printf("    端口准入并发上限: Global=%d (peak used %d), Read=%d, Write=%d\n",
               (int)IOMMU_GLOBAL_MAX_OUTSTANDING, peak_iommu_global_outstanding,
               (int)IOMMU_READ_MAX_OUTSTANDING, (int)IOMMU_WRITE_MAX_OUTSTANDING);
        printf("    注: [读写端口分离] 并发门控已在端口准入处完成(计数同点++),\n");
        printf("        此队列仅反映准入后等待 parser 轮询弹出的短暂停留。\n");
    }
    printf("====================================================================================\n\n");
}

/**********************************************/
// [非 SPLIT 场景专用] 单入站端口同时接收读+写的入口回调 axi_slave_nb_transport_fw
//   仅在 TEST_CFG_SPLIT_RW_PORTS=0 的旧场景被注册(基线 A/B 对比用);
//   读写端口分离场景不使用本函数, 而是使用下面两个完全独立的注册函数。
//   方向由 trans.get_command() 判定; 节拍保持旧场景公式(单端口全带宽, 控制包2ns)。
/**********************************************/
tlm::tlm_sync_enum iommu_top::axi_slave_nb_transport_fw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_REQ) {
        // 1. Extract PayloadExtention FIRST (needed for delay calculation)
        PayloadExtention* ext = nullptr;
        trans.get_extension(ext);

        const bool is_write_req = (trans.get_command() == tlm::TLM_WRITE_COMMAND);
        const char* port_name   = is_write_req ? "W-PORT" : "R-PORT";

        // ===== Bandwidth control: slave port accept delay =====
        // 单端口同时收读写: 字节全部计入 slave_0_total_bytes(与基线口径一致)。
        unsigned int data_len = trans.get_data_length();
        slave_0_total_bytes += data_len;  // [STAT] 入口字节计数
        double slave_bw_delay_ns;
        bool is_ctrl_packet = (ext && ext->is_ctrl);
        if (is_ctrl_packet) {
            slave_bw_delay_ns = AXI_SLAVE_CTRL_DELAY_NS;  // 旧场景: 2ns
        } else {
            slave_bw_delay_ns = 1000.0 * data_len * 8 / AXI_SLAVE_PORT_BW_MBPS;
        }
        // [STAT] 端口节拍占用累计(判定该口是否一直按自身节拍收包)
        if (is_write_req) port_beat_write_ns += slave_bw_delay_ns;
        else              port_beat_read_ns  += slave_bw_delay_ns;
        wait(slave_bw_delay_ns, SC_NS);

        // ===== [读写端口分离] 端口准入门控 + outstanding 计数(从 parser 前移到此) =====
        //   读口: while(read >= READ_MAX || global >= GLOBAL_MAX) wait(对应freed事件)
        //   写口: while(write >= WRITE_MAX || global >= GLOBAL_MAX) wait(对应freed事件)
        //   门控与计数均持 reorder_mtx, 与出口释放(reorder_output_thread)互斥;
        //   计数在准入点++, 故 parser 弹出前任务已计入并发, 不会超限。
        {
            const double gate_t0 = sc_time_stamp().to_seconds() * 1e9;
            bool gated = false;
            reorder_mtx.lock();
            while (true) {
                const bool g_full = (iommu_global_outstanding >= (int)IOMMU_GLOBAL_MAX_OUTSTANDING);
                const bool d_full = is_write_req
                    ? (iommu_write_outstanding >= (int)IOMMU_WRITE_MAX_OUTSTANDING)
                    : (iommu_read_outstanding  >= (int)IOMMU_READ_MAX_OUTSTANDING);
                if (!g_full && !d_full) break;
                gated = true;
                reorder_mtx.unlock();
                if (d_full) {
                    wait(is_write_req ? iommu_write_outstanding_freed_event
                                      : iommu_read_outstanding_freed_event);
                } else {
                    wait(iommu_global_outstanding_freed_event);
                }
                reorder_mtx.lock();
            }
            // [STAT] 并发面积采样: 在 outstanding++ 之前(与出口 -- 之前采样配对)
            iommu_sample_concurrency_before_change();
            conc_window_sample();   // [STAT] 注入窗口内并发面积/顶满时长
            if (is_write_req) {
                iommu_write_outstanding++;
                if (iommu_write_outstanding > peak_iommu_write_outstanding)
                    peak_iommu_write_outstanding = iommu_write_outstanding;
                port_admit_write_total++;
            } else {
                iommu_read_outstanding++;
                if (iommu_read_outstanding > peak_iommu_read_outstanding)
                    peak_iommu_read_outstanding = iommu_read_outstanding;
                port_admit_read_total++;
            }
            iommu_global_outstanding++;
            if (iommu_global_outstanding > peak_iommu_global_outstanding)
                peak_iommu_global_outstanding = iommu_global_outstanding;
            reorder_mtx.unlock();
            // [STAT] 记录注入窗口内本端口首次准入时刻(用于拆出端口自身注入跨度)
            if (conc_win_open) {
                const double admit_ns = sc_time_stamp().to_seconds() * 1e9;
                if (is_write_req) { if (port_first_admit_w_ns < 0.0) port_first_admit_w_ns = admit_ns; }
                else              { if (port_first_admit_r_ns < 0.0) port_first_admit_r_ns = admit_ns; }
            }
            // [STAT] 端口门控阻塞时长(读/写各自记录, 预期无交叉阻塞)
            if (gated) {
                const double dur = sc_time_stamp().to_seconds() * 1e9 - gate_t0;
                if (is_write_req) {
                    port_blk_write_cnt++;
                    port_blk_write_ns += dur;
                    if (dur > port_blk_write_max_ns) port_blk_write_max_ns = dur;
                } else {
                    port_blk_read_cnt++;
                    port_blk_read_ns += dur;
                    if (dur > port_blk_read_max_ns) port_blk_read_max_ns = dur;
                }
            }
        }

        // 2. Create task and fill fields
        iommu_task_t* task = new iommu_task_t();

        task_id_mtx.lock();
        task->task_id = next_task_id++;
        task_id_mtx.unlock();

        task->tlm_trans_ptr = &trans;
        task->iova = trans.get_address();
        task->length = trans.get_data_length();
        // [读写端口分离] 读写方向由【端口】决定(R口=READ, W口=WRITE)
        task->read_writeAMO = is_write_req ? WRITE : READ;

        if (ext) {
            task->device_id = ext->requester_id;
            task->pid_valid = ext->pid_valid;
            task->process_id = ext->process_id;
            task->exec_req = ext->exec_req;
            task->priv_req = ext->priv_req;
            task->no_write = ext->no_write;
            task->is_cxl_dev = 0;
            task->is_ctrl = ext->is_ctrl;  // [场景13] 控制包标记(SQ/CQ/MSI不计入IOPS)

            if (ext->at == 0) task->at = ADDR_TYPE_UNTRANSLATED;
            else if (ext->at == 1) task->at = ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST;
            else if (ext->at == 2) task->at = ADDR_TYPE_TRANSLATED;
        }

        task->timestamp = sc_time_stamp();
        task->state = TASK_INIT;
#if TEST_CFG_MULTI_DEVICE_SCENE
        md_trace.accept(&trans, task->task_id, task->device_id, task->iova);
#endif

        // [STAT] 控制包(SQ/CQ/MSI)入口计数
        if (task->is_ctrl) {
            ctrl_total_count++;
            // 基于固定IOVA区分SQ/CQ/MSI (场景13约定; 512MB测试项SQ/CQ IOVA上移至0x22000000/0x22001000)
#ifdef TEST_CFG_S13_IOVA_512MB
            if (task->iova == 0x22000000) {
                ctrl_sq_count++;
            } else if (task->iova == 0x22001000) {
                ctrl_cq_count++;
            } else {
                ctrl_msi_count++;  // MSI请求
            }
#else
            if (task->iova == 0x5000000) {
                ctrl_sq_count++;
            } else if (task->iova == 0x5001000) {
                ctrl_cq_count++;
            } else {
                ctrl_msi_count++;  // MSI请求
            }
#endif
        }

        // [STAT需求1] 4KB页并发跟踪: 随 outstanding 计数前移到端口准入点打点
        //   (与端口 iommu_*_outstanding++ 同点), 统计口径 = 端口准入->reorder释放,
        //   仍严格受 640 outstanding 约束。入口队列积压另由 input_q_* 统计。
        track_4kb_page_in(task);

        double ts_ns = sc_time_stamp().to_seconds() * 1e9;
        printf("[IOMMU_TOP] axi_slave_nb_transport_fw(%s): task_id=%u, device_id=0x%x, iova=0x%lx, at=%d -> input_pipeline_peq [t=%.1f ns]\n",
               port_name, task->task_id, task->device_id, task->iova, task->at, ts_ns);
        fflush(stdout);

        // [STAT] IOMMU输入端口任务间隔采样(合计 + 读口/写口分开)
        if (iommu_in_last_ns > 0.0) {
            double in_interval = ts_ns - iommu_in_last_ns;
            iommu_in_interval_total_ns += in_interval;
            if (in_interval > iommu_in_interval_max_ns) iommu_in_interval_max_ns = in_interval;
            if (in_interval < iommu_in_interval_min_ns) iommu_in_interval_min_ns = in_interval;
            iommu_in_interval_count++;
            iommu_in_interval_values.push_back(in_interval);
            iommu_in_interval_end_ns.push_back(ts_ns);  // [STAT需求1] 记录该间隔结束时刻(稳态窗口筛选)
        }
        iommu_in_last_ns = ts_ns;
        if (is_write_req) {
            if (iommu_in_w_last_ns > 0.0) {
                double d = ts_ns - iommu_in_w_last_ns;
                iommu_in_w_interval_total_ns += d;
                if (d > iommu_in_w_interval_max_ns) iommu_in_w_interval_max_ns = d;
                if (d < iommu_in_w_interval_min_ns) iommu_in_w_interval_min_ns = d;
                iommu_in_w_interval_count++;
                iommu_in_w_interval_values.push_back(d);
                iommu_in_w_interval_end_ns.push_back(ts_ns);
            }
            iommu_in_w_last_ns = ts_ns;
        } else {
            if (iommu_in_r_last_ns > 0.0) {
                double d = ts_ns - iommu_in_r_last_ns;
                iommu_in_r_interval_total_ns += d;
                if (d > iommu_in_r_interval_max_ns) iommu_in_r_interval_max_ns = d;
                if (d < iommu_in_r_interval_min_ns) iommu_in_r_interval_min_ns = d;
                iommu_in_r_interval_count++;
                iommu_in_r_interval_values.push_back(d);
                iommu_in_r_interval_end_ns.push_back(ts_ns);
            }
            iommu_in_r_last_ns = ts_ns;
        }

        // [流水线延时] 静态延时线: 仅入队(task + 到期时刻), 由常驻 input_delay_thread_r/w
        //   在到期后写入 inbound_fifo_r/w。[读写端口分离] 延时线按方向拆为两条,
        //   读写互不阻塞。入队不阻塞, 且延时固定 -> 到期顺序即到达顺序。
#if TEST_CFG_MULTI_DEVICE_SCENE
        ++md_trace.input_pending;
#endif
        // [STAT需求1-诊断] 入口未决队列积压 +1 (端口已准入, 尚未被 parser 弹出)
        input_q_sample_before_change();
        input_q_count++;
        if (input_q_count > input_q_peak) input_q_peak = input_q_count;
        if (is_write_req) {
            input_q_w_count++;
            if (input_q_w_count > input_q_w_peak) input_q_w_peak = input_q_w_count;
        } else {
            input_q_r_count++;
            if (input_q_r_count > input_q_r_peak) input_q_r_peak = input_q_r_count;
        }
        {
            pipeline_delay_item_t item;
            item.task = task;
            item.due_ns = ts_ns + (double)IOMMU_INPUT_PIPELINE_DELAY_NS;
            if (is_write_req) {
                input_delay_mtx_w.lock();
                input_delay_queue_w.push(item);
                input_delay_mtx_w.unlock();
                input_delay_event_w.notify(SC_ZERO_TIME);
            } else {
                input_delay_mtx_r.lock();
                input_delay_queue_r.push(item);
                input_delay_mtx_r.unlock();
                input_delay_event_r.notify(SC_ZERO_TIME);
            }
        }

        // 4. Return END_REQ (request accepted)
        phase = tlm::END_REQ;
        return tlm::TLM_UPDATED;
    }
    else if (phase == tlm::END_RESP) {
        // Initiator confirmed receiving response
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

/**********************************************/
// [读写端口分离] axi slave R port 专用注册函数 (绑定 axi_slave_from_pcie_noc_0_socket)
//   完全独立实现: 不引用任何共享入口函数, 端口节拍 wait 就在本函数内部。
//   只处理读请求:
//     512B 数据包 -> 间隔 8ns, 128B 数据包 -> 间隔 2ns (读口 512bit@1000MHz),
//     SQ/CQ/MSI 控制包 -> 间隔 1ns;
//   只按【读额度】门控: iommu_read_outstanding < 320 且 global < 640,
//     通过后在端口准入点立即 outstanding++, 保证"端口->响应释放"内并发不超限;
//   准入后的读任务投入读方向入口延时线 -> inbound_fifo_r, 由 parser 轮询取出。
//   收到写命令 = RP 读/写 socket 绑定错误, 直接 FATAL(不静默改方向)。
/**********************************************/
tlm::tlm_sync_enum iommu_top::axi_slave_r_nb_transport_fw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_REQ) {
        PayloadExtention* ext = nullptr;
        trans.get_extension(ext);

        if (trans.get_command() == tlm::TLM_WRITE_COMMAND) {
            port_dir_mismatch_cnt++;
            fprintf(stderr,
                "[FATAL][R-PORT] axi_slave_r_nb_transport_fw 只应收到读请求, 实际收到写请求: "
                "iova=0x%lx len=%u (RP 读/写 socket 绑定错误)\n",
                (unsigned long)trans.get_address(), trans.get_data_length());
            fflush(stderr);
            SC_REPORT_FATAL("IOMMU_PORT_DIR", "axi slave R port 收到写请求");
        }

        // ===== 读端口独立节拍(本函数内部计时, 与写端口无关) =====
        unsigned int data_len = trans.get_data_length();
        slave_0_total_bytes += data_len;   // [STAT] 读口入口字节计数
        double slave_bw_delay_ns;
        bool is_ctrl_packet = (ext && ext->is_ctrl);
        if (is_ctrl_packet) {
            slave_bw_delay_ns = AXI_SLAVE_R_CTRL_DELAY_NS;            // 1ns
        } else {
            slave_bw_delay_ns = 1000.0 * data_len * 8 / AXI_SLAVE_R_PORT_BW_MBPS;  // 512B->8ns, 128B->2ns
        }
        port_beat_read_ns += slave_bw_delay_ns;   // [STAT] 读口节拍占用累计
        wait(slave_bw_delay_ns, SC_NS);

        // ===== 读端口独立准入: 只等 Read(320)/Global(640) =====
        {
            const double gate_t0 = sc_time_stamp().to_seconds() * 1e9;
            bool gated = false;
            reorder_mtx.lock();
            while (true) {
                const bool g_full = (iommu_global_outstanding >= (int)IOMMU_GLOBAL_MAX_OUTSTANDING);
                const bool d_full = (iommu_read_outstanding  >= (int)IOMMU_READ_MAX_OUTSTANDING);
                if (!g_full && !d_full) break;
                gated = true;
                reorder_mtx.unlock();
                wait(d_full ? iommu_read_outstanding_freed_event
                            : iommu_global_outstanding_freed_event);
                reorder_mtx.lock();
            }
            // [STAT] 并发面积采样: 在 outstanding++ 之前(与出口 -- 之前采样配对)
            iommu_sample_concurrency_before_change();
            conc_window_sample();       // [STAT] 注入窗口内并发面积/顶满时长
            iommu_read_outstanding++;
            if (iommu_read_outstanding > peak_iommu_read_outstanding)
                peak_iommu_read_outstanding = iommu_read_outstanding;
            port_admit_read_total++;
            iommu_global_outstanding++;
            if (iommu_global_outstanding > peak_iommu_global_outstanding)
                peak_iommu_global_outstanding = iommu_global_outstanding;
            reorder_mtx.unlock();
            // [STAT] 记录读口在本注入窗口内的首次准入时刻
            if (conc_win_open && port_first_admit_r_ns < 0.0)
                port_first_admit_r_ns = sc_time_stamp().to_seconds() * 1e9;
            // [STAT] 读口因(读满/全局满)阻塞时长
            if (gated) {
                const double dur = sc_time_stamp().to_seconds() * 1e9 - gate_t0;
                port_blk_read_cnt++;
                port_blk_read_ns += dur;
                if (dur > port_blk_read_max_ns) port_blk_read_max_ns = dur;
            }
        }

        // ===== 创建读任务 =====
        iommu_task_t* task = new iommu_task_t();

        task_id_mtx.lock();
        task->task_id = next_task_id++;
        task_id_mtx.unlock();

        task->tlm_trans_ptr = &trans;
        task->iova = trans.get_address();
        task->length = trans.get_data_length();
        task->read_writeAMO = READ;      // [读写端口分离] R 端口固定为读方向

        if (ext) {
            task->device_id = ext->requester_id;
            task->pid_valid = ext->pid_valid;
            task->process_id = ext->process_id;
            task->exec_req = ext->exec_req;
            task->priv_req = ext->priv_req;
            task->no_write = ext->no_write;
            task->is_cxl_dev = 0;
            task->is_ctrl = ext->is_ctrl;  // [场景13] 控制包标记(SQ/CQ/MSI不计入IOPS)

            if (ext->at == 0) task->at = ADDR_TYPE_UNTRANSLATED;
            else if (ext->at == 1) task->at = ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST;
            else if (ext->at == 2) task->at = ADDR_TYPE_TRANSLATED;
        }

        task->timestamp = sc_time_stamp();
        task->state = TASK_INIT;
#if TEST_CFG_MULTI_DEVICE_SCENE
        md_trace.accept(&trans, task->task_id, task->device_id, task->iova);
#endif

        // [STAT] 控制包(SQ/CQ/MSI)入口计数
        if (task->is_ctrl) {
            ctrl_total_count++;
#ifdef TEST_CFG_S13_IOVA_512MB
            if (task->iova == 0x22000000)      ctrl_sq_count++;
            else if (task->iova == 0x22001000) ctrl_cq_count++;
            else                               ctrl_msi_count++;
#else
            if (task->iova == 0x5000000)       ctrl_sq_count++;
            else if (task->iova == 0x5001000)  ctrl_cq_count++;
            else                               ctrl_msi_count++;
#endif
        }

        // [STAT需求1] 4KB 页并发跟踪(与端口 outstanding++ 同点)
        track_4kb_page_in(task);

        double ts_ns = sc_time_stamp().to_seconds() * 1e9;
        printf("[IOMMU_TOP] axi_slave_r_nb_transport_fw(R-PORT): task_id=%u, device_id=0x%x, iova=0x%lx, at=%d -> inbound_fifo_r [t=%.1f ns]\n",
               task->task_id, task->device_id, task->iova, task->at, ts_ns);
        fflush(stdout);

        // [STAT] 入口任务间隔采样(合计 + 读口)
        if (iommu_in_last_ns > 0.0) {
            double in_interval = ts_ns - iommu_in_last_ns;
            iommu_in_interval_total_ns += in_interval;
            if (in_interval > iommu_in_interval_max_ns) iommu_in_interval_max_ns = in_interval;
            if (in_interval < iommu_in_interval_min_ns) iommu_in_interval_min_ns = in_interval;
            iommu_in_interval_count++;
            iommu_in_interval_values.push_back(in_interval);
            iommu_in_interval_end_ns.push_back(ts_ns);
        }
        iommu_in_last_ns = ts_ns;
        if (iommu_in_r_last_ns > 0.0) {
            double d = ts_ns - iommu_in_r_last_ns;
            iommu_in_r_interval_total_ns += d;
            if (d > iommu_in_r_interval_max_ns) iommu_in_r_interval_max_ns = d;
            if (d < iommu_in_r_interval_min_ns) iommu_in_r_interval_min_ns = d;
            iommu_in_r_interval_count++;
            iommu_in_r_interval_values.push_back(d);
            iommu_in_r_interval_end_ns.push_back(ts_ns);
        }
        iommu_in_r_last_ns = ts_ns;

        // [流水线延时] 读方向静态延时线: 入队不阻塞, 到期后由
        //   input_delay_thread_r 写入 inbound_fifo_r, 再由 parser 轮询取出。
#if TEST_CFG_MULTI_DEVICE_SCENE
        ++md_trace.input_pending;
#endif
        input_q_sample_before_change();
        input_q_count++;
        if (input_q_count > input_q_peak) input_q_peak = input_q_count;
        input_q_r_count++;
        if (input_q_r_count > input_q_r_peak) input_q_r_peak = input_q_r_count;
        {
            pipeline_delay_item_t item;
            item.task = task;
            item.due_ns = ts_ns + (double)IOMMU_INPUT_PIPELINE_DELAY_NS;
            input_delay_mtx_r.lock();
            input_delay_queue_r.push(item);
            input_delay_mtx_r.unlock();
            input_delay_event_r.notify(SC_ZERO_TIME);
        }

        phase = tlm::END_REQ;          // 读请求已接收
        return tlm::TLM_UPDATED;
    }
    else if (phase == tlm::END_RESP) {
        // Initiator confirmed receiving response
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

/**********************************************/
// [读写端口分离] axi slave W port 专用注册函数 (绑定 axi_slave_from_pcie_noc_0w_socket)
//   完全独立实现: 不引用任何共享入口函数, 也不调读端口任何代码,
//   端口节拍 wait 就在本函数内部。
//   只处理写请求:
//     512B 数据包 -> 间隔 8ns, 128B 数据包 -> 间隔 2ns (写口 512bit@1000MHz),
//     SQ/CQ/MSI 控制包 -> 间隔 1ns;
//   只按【写额度】门控: iommu_write_outstanding < 320 且 global < 640,
//     通过后在端口准入点立即 outstanding++;
//   准入后的写任务投入写方向入口延时线 -> inbound_fifo_w, 由 parser 轮询取出。
//   收到读命令 = RP 读/写 socket 绑定错误, 直接 FATAL。
/**********************************************/
tlm::tlm_sync_enum iommu_top::axi_slave_w_nb_transport_fw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_REQ) {
        PayloadExtention* ext = nullptr;
        trans.get_extension(ext);

        if (trans.get_command() == tlm::TLM_READ_COMMAND) {
            port_dir_mismatch_cnt++;
            fprintf(stderr,
                "[FATAL][W-PORT] axi_slave_w_nb_transport_fw 只应收到写请求, 实际收到读请求: "
                "iova=0x%lx len=%u (RP 读/写 socket 绑定错误)\n",
                (unsigned long)trans.get_address(), trans.get_data_length());
            fflush(stderr);
            SC_REPORT_FATAL("IOMMU_PORT_DIR", "axi slave W port 收到读请求");
        }

        // ===== 写端口独立节拍(本函数内部计时, 与读端口无关) =====
        unsigned int data_len = trans.get_data_length();
        slave_0w_total_bytes += data_len;   // [STAT] 写口入口字节计数
        double slave_bw_delay_ns;
        bool is_ctrl_packet = (ext && ext->is_ctrl);
        if (is_ctrl_packet) {
            slave_bw_delay_ns = AXI_SLAVE_W_CTRL_DELAY_NS;            // 1ns
        } else {
            slave_bw_delay_ns = 1000.0 * data_len * 8 / AXI_SLAVE_W_PORT_BW_MBPS;  // 512B->8ns, 128B->2ns
        }
        port_beat_write_ns += slave_bw_delay_ns;   // [STAT] 写口节拍占用累计
        wait(slave_bw_delay_ns, SC_NS);

        // ===== 写端口独立准入: 只等 Write(320)/Global(640) =====
        {
            const double gate_t0 = sc_time_stamp().to_seconds() * 1e9;
            bool gated = false;
            reorder_mtx.lock();
            while (true) {
                const bool g_full = (iommu_global_outstanding >= (int)IOMMU_GLOBAL_MAX_OUTSTANDING);
                const bool d_full = (iommu_write_outstanding >= (int)IOMMU_WRITE_MAX_OUTSTANDING);
                if (!g_full && !d_full) break;
                gated = true;
                reorder_mtx.unlock();
                wait(d_full ? iommu_write_outstanding_freed_event
                            : iommu_global_outstanding_freed_event);
                reorder_mtx.lock();
            }
            // [STAT] 并发面积采样: 在 outstanding++ 之前(与出口 -- 之前采样配对)
            iommu_sample_concurrency_before_change();
            conc_window_sample();       // [STAT] 注入窗口内并发面积/顶满时长
            iommu_write_outstanding++;
            if (iommu_write_outstanding > peak_iommu_write_outstanding)
                peak_iommu_write_outstanding = iommu_write_outstanding;
            port_admit_write_total++;
            iommu_global_outstanding++;
            if (iommu_global_outstanding > peak_iommu_global_outstanding)
                peak_iommu_global_outstanding = iommu_global_outstanding;
            reorder_mtx.unlock();
            // [STAT] 记录写口在本注入窗口内的首次准入时刻
            if (conc_win_open && port_first_admit_w_ns < 0.0)
                port_first_admit_w_ns = sc_time_stamp().to_seconds() * 1e9;
            // [STAT] 写口因(写满/全局满)阻塞时长
            if (gated) {
                const double dur = sc_time_stamp().to_seconds() * 1e9 - gate_t0;
                port_blk_write_cnt++;
                port_blk_write_ns += dur;
                if (dur > port_blk_write_max_ns) port_blk_write_max_ns = dur;
            }
        }

        // ===== 创建写任务 =====
        iommu_task_t* task = new iommu_task_t();

        task_id_mtx.lock();
        task->task_id = next_task_id++;
        task_id_mtx.unlock();

        task->tlm_trans_ptr = &trans;
        task->iova = trans.get_address();
        task->length = trans.get_data_length();
        task->read_writeAMO = WRITE;     // [读写端口分离] W 端口固定为写方向

        if (ext) {
            task->device_id = ext->requester_id;
            task->pid_valid = ext->pid_valid;
            task->process_id = ext->process_id;
            task->exec_req = ext->exec_req;
            task->priv_req = ext->priv_req;
            task->no_write = ext->no_write;
            task->is_cxl_dev = 0;
            task->is_ctrl = ext->is_ctrl;  // [场景13] 控制包标记(SQ/CQ/MSI不计入IOPS)

            if (ext->at == 0) task->at = ADDR_TYPE_UNTRANSLATED;
            else if (ext->at == 1) task->at = ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST;
            else if (ext->at == 2) task->at = ADDR_TYPE_TRANSLATED;
        }

        task->timestamp = sc_time_stamp();
        task->state = TASK_INIT;
#if TEST_CFG_MULTI_DEVICE_SCENE
        md_trace.accept(&trans, task->task_id, task->device_id, task->iova);
#endif

        // [STAT] 控制包(SQ/CQ/MSI)入口计数
        if (task->is_ctrl) {
            ctrl_total_count++;
#ifdef TEST_CFG_S13_IOVA_512MB
            if (task->iova == 0x22000000)      ctrl_sq_count++;
            else if (task->iova == 0x22001000) ctrl_cq_count++;
            else                               ctrl_msi_count++;
#else
            if (task->iova == 0x5000000)       ctrl_sq_count++;
            else if (task->iova == 0x5001000)  ctrl_cq_count++;
            else                               ctrl_msi_count++;
#endif
        }

        // [STAT需求1] 4KB 页并发跟踪(与端口 outstanding++ 同点)
        track_4kb_page_in(task);

        double ts_ns = sc_time_stamp().to_seconds() * 1e9;
        printf("[IOMMU_TOP] axi_slave_w_nb_transport_fw(W-PORT): task_id=%u, device_id=0x%x, iova=0x%lx, at=%d -> inbound_fifo_w [t=%.1f ns]\n",
               task->task_id, task->device_id, task->iova, task->at, ts_ns);
        fflush(stdout);

        // [STAT] 入口任务间隔采样(合计 + 写口)
        if (iommu_in_last_ns > 0.0) {
            double in_interval = ts_ns - iommu_in_last_ns;
            iommu_in_interval_total_ns += in_interval;
            if (in_interval > iommu_in_interval_max_ns) iommu_in_interval_max_ns = in_interval;
            if (in_interval < iommu_in_interval_min_ns) iommu_in_interval_min_ns = in_interval;
            iommu_in_interval_count++;
            iommu_in_interval_values.push_back(in_interval);
            iommu_in_interval_end_ns.push_back(ts_ns);
        }
        iommu_in_last_ns = ts_ns;
        if (iommu_in_w_last_ns > 0.0) {
            double d = ts_ns - iommu_in_w_last_ns;
            iommu_in_w_interval_total_ns += d;
            if (d > iommu_in_w_interval_max_ns) iommu_in_w_interval_max_ns = d;
            if (d < iommu_in_w_interval_min_ns) iommu_in_w_interval_min_ns = d;
            iommu_in_w_interval_count++;
            iommu_in_w_interval_values.push_back(d);
            iommu_in_w_interval_end_ns.push_back(ts_ns);
        }
        iommu_in_w_last_ns = ts_ns;

        // [流水线延时] 写方向静态延时线: 入队不阻塞, 到期后由
        //   input_delay_thread_w 写入 inbound_fifo_w, 再由 parser 轮询取出。
#if TEST_CFG_MULTI_DEVICE_SCENE
        ++md_trace.input_pending;
#endif
        input_q_sample_before_change();
        input_q_count++;
        if (input_q_count > input_q_peak) input_q_peak = input_q_count;
        input_q_w_count++;
        if (input_q_w_count > input_q_w_peak) input_q_w_peak = input_q_w_count;
        {
            pipeline_delay_item_t item;
            item.task = task;
            item.due_ns = ts_ns + (double)IOMMU_INPUT_PIPELINE_DELAY_NS;
            input_delay_mtx_w.lock();
            input_delay_queue_w.push(item);
            input_delay_mtx_w.unlock();
            input_delay_event_w.notify(SC_ZERO_TIME);
        }

        phase = tlm::END_REQ;          // 写请求已接收
        return tlm::TLM_UPDATED;
    }
    else if (phase == tlm::END_RESP) {
        // Initiator confirmed receiving response
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

/**********************************************/
// [STAT] 注入窗口开关
//   open  : 测量 epoch 开始前(RP 调用 reset_4kb_concurrency_window 同点),
//           同时重置面积/顶满时长累加器;
//   close : RP 把全部请求送达端口后立即调用,
//           并把“末次采样->关闭时刻”按当时 outstanding 补齐。
//   窗口内 RP 侧始终有请求可注入, 故窗口平均并发与顶满时长占比才能真实反映
//   "读写是否各自顶满 320"; 全运行窗口会被注入结束后的 drain 段拉低。
/**********************************************/
void iommu_top::conc_window_open() {
    const double now_ns = sc_time_stamp().to_seconds() * 1e9;
    conc_win_open       = true;
    conc_win_start_ns   = now_ns;
    conc_win_last_ns    = now_ns;
    conc_win_end_ns     = 0.0;
    conc_win_area_read_ns = conc_win_area_write_ns = conc_win_area_global_ns = 0.0;
    conc_win_full_read_ns = conc_win_full_write_ns = conc_win_full_global_ns = 0.0;
    conc_win_sample_cnt   = 0;
    // [STAT] 字节快照: 用于算"注入期"无稀释带宽(上方 Port Average Bandwidth 用全仿真时长作分母)
    bw_snap_win_r_bytes   = slave_0_total_bytes;
    bw_snap_win_w_bytes   = slave_0w_total_bytes;
    bw_snap_win_m0_bytes  = master_0_total_bytes;
    conc_win_hist.clear();
    conc_win_hist.reserve(200000);   // 避免大事务量下反复重分配(mps128 约 6.4 万样本)
    port_first_admit_r_ns = -1.0;   // 端口首次准入时刻重新记录(仅统计本窗口)
    port_first_admit_w_ns = -1.0;
    printf("[CONC_WIN] 注入并发窗口打开 t=%.1f ns (Read<=%d, Write<=%d, Global<=%d)\n",
           now_ns, (int)IOMMU_READ_MAX_OUTSTANDING,
           (int)IOMMU_WRITE_MAX_OUTSTANDING, (int)IOMMU_GLOBAL_MAX_OUTSTANDING);
    fflush(stdout);
}

void iommu_top::conc_window_close() {
    if (!conc_win_open) return;
    const double now_ns = sc_time_stamp().to_seconds() * 1e9;
    const double delta  = now_ns - conc_win_last_ns;
    if (delta > 0.0) {   // 补齐最后一段(采样点 -> 注入结束)
        const double vr = (double)iommu_read_outstanding;
        const double vw = (double)iommu_write_outstanding;
        const double vg = (double)iommu_global_outstanding;
        conc_win_area_read_ns   += vr * delta;
        conc_win_area_write_ns  += vw * delta;
        conc_win_area_global_ns += vg * delta;
        if (iommu_read_outstanding   >= (int)IOMMU_READ_MAX_OUTSTANDING)   conc_win_full_read_ns   += delta;
        if (iommu_write_outstanding  >= (int)IOMMU_WRITE_MAX_OUTSTANDING)  conc_win_full_write_ns  += delta;
        if (iommu_global_outstanding >= (int)IOMMU_GLOBAL_MAX_OUTSTANDING) conc_win_full_global_ns += delta;
        conc_win_hist.push_back(conc_seg_t{conc_win_last_ns, now_ns, vr, vw, vg});
        conc_win_last_ns = now_ns;
    }
    conc_win_end_ns = now_ns;
    conc_win_open   = false;
    printf("[CONC_WIN] 注入并发窗口关闭 t=%.1f ns (窗口 %.1f ns, 采样 %lu 次)\n",
           now_ns, conc_win_end_ns - conc_win_start_ns,
           (unsigned long)conc_win_sample_cnt);
    fflush(stdout);
}

/**********************************************/
// conc_window_slice - 在任意子窗口 [t0,t1] 内按分段常数重算时间加权并发与顶满时长
//   数据源 = conc_win_hist(每次 outstanding 变化前记录的旧计数与其生效起点)。
/**********************************************/
void iommu_top::conc_window_slice(double t0, double t1,
                                  double& avg_r, double& avg_w, double& avg_g,
                                  double& full_r, double& full_w, double& full_g) const {
    avg_r = avg_w = avg_g = full_r = full_w = full_g = 0.0;
    if (t1 <= t0 || conc_win_hist.empty()) return;
    double ar = 0.0, aw = 0.0, ag = 0.0, fr = 0.0, fw = 0.0, fg = 0.0;
    // 段已按时间递增存入: [s,e) 内计数恒为 (r,w,g)
    for (const auto& seg : conc_win_hist) {
        if (seg.e <= t0) continue;
        if (seg.s >= t1) break;
        const double lo = (seg.s > t0) ? seg.s : t0;
        const double hi = (seg.e < t1) ? seg.e : t1;
        if (hi <= lo) continue;
        const double dt = hi - lo;
        ar += seg.r * dt;
        aw += seg.w * dt;
        ag += seg.g * dt;
        if (seg.r >= (double)IOMMU_READ_MAX_OUTSTANDING)   fr += dt;
        if (seg.w >= (double)IOMMU_WRITE_MAX_OUTSTANDING)  fw += dt;
        if (seg.g >= (double)IOMMU_GLOBAL_MAX_OUTSTANDING) fg += dt;
    }
    const double span = t1 - t0;
    avg_r = ar / span; avg_w = aw / span; avg_g = ag / span;
    full_r = fr / span * 100.0; full_w = fw / span * 100.0; full_g = fg / span * 100.0;
}

/**********************************************/
// input_delay_line - 入口流水线延时线通用实现(常驻静态线程)
//   [读写端口分离] 读/写各一条: 从对应延时线队列按 FIFO 取出 task,
//   等待到其到期时刻(入队时刻 + IOMMU_INPUT_PIPELINE_DELAY_NS)后写入
//   对应方向的 inbound_fifo。因延时固定, 队首即最早到期, 单线程串行下发
//   的时序与原并行协程等价, 且进程数恒定(不随事务量增长)。
/**********************************************/
void iommu_top::input_delay_line(std::queue<pipeline_delay_item_t>& q, sc_mutex& mtx,
                                 sc_event& evt, sc_fifo<iommu_task_t*>& out_fifo,
                                 uint32_t& dir_q_count, uint32_t& dir_q_peak) {
    (void)dir_q_peak;
    while (true) {
        iommu_task_t* task = nullptr;
        double due_ns = 0.0;

        mtx.lock();
        while (q.empty()) {
            mtx.unlock();
            wait(evt);
            mtx.lock();
        }
        task = q.front().task;
        due_ns = q.front().due_ns;
        q.pop();
        mtx.unlock();
        // [STAT需求1-诊断] 入口未决队列积压 -1 (离开延时线, 即将写入 inbound_fifo_r/w)
        input_q_sample_before_change();
        if (input_q_count > 0) input_q_count--;
        if (dir_q_count > 0) dir_q_count--;

        double now_ns = sc_time_stamp().to_seconds() * 1e9;
        if (due_ns > now_ns) wait(due_ns - now_ns, SC_NS);

        out_fifo.write(task);
#if TEST_CFG_MULTI_DEVICE_SCENE
        --md_trace.input_pending;
        md_trace.progress();
#endif
    }
}

void iommu_top::input_delay_thread_r() {
    input_delay_line(input_delay_queue_r, input_delay_mtx_r, input_delay_event_r,
                     inbound_fifo_r, input_q_r_count, input_q_r_peak);
}

void iommu_top::input_delay_thread_w() {
    input_delay_line(input_delay_queue_w, input_delay_mtx_w, input_delay_event_w,
                     inbound_fifo_w, input_q_w_count, input_q_w_peak);
}

/**********************************************/
// DDR response AT nb_transport_bw callback (DDR -> IOMMU)
/**********************************************/
tlm::tlm_sync_enum iommu_top::ddr_nb_transport_bw(
    tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase, sc_time& delay)
{
    if (phase == tlm::BEGIN_RESP) {
#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::BusyGuard md_busy(md_trace.active);
#endif
        // Pop from ddr_pending_queue head (FIFO ordering guarantee)
        ddr_queue_mtx.lock();
        if (ddr_pending_queue.empty()) {
            ddr_queue_mtx.unlock();
            printf("[IOMMU] ERROR: DDR response received but pending queue is empty!\n");
            fflush(stdout);
            phase = tlm::END_RESP;
            return tlm::TLM_COMPLETED;
        }
        ddr_pending_entry_t pending = ddr_pending_queue.front();
        ddr_pending_queue.pop();
        ddr_queue_mtx.unlock();

        // Construct response entry
        ddr_rsp_entry_t rsp;
        rsp.task_id = pending.task_id;
        rsp.data_length = trans.get_data_length();
        if (rsp.data_length > sizeof(rsp.data)) rsp.data_length = sizeof(rsp.data);
        memcpy(rsp.data, trans.get_data_ptr(), rsp.data_length);
        rsp.error = (trans.get_response_status() != tlm::TLM_OK_RESPONSE);
#if TEST_CFG_MULTI_DEVICE_SCENE
        md_trace.progress();
        if (pending.task_id) {
            auto& owner = md_trace.task(pending.task_id);
            md_trace.operation(pending.task_id, "ddr", pending.is_write ? "write" : "read",
                               owner.device, pending.submit_time_ns, !rsp.error, pending.source_module,
                               0, 0, pending.size);
        }
#endif
        rsp.submit_time_ns = pending.submit_time_ns;  // [STAT] 传递时间戳

        // Route to corresponding rsp_ddr_fifo based on source_module
        // Use blocking write() to prevent silent drop when fifo is full
        switch (pending.source_module) {
            case DDR_SRC_XDTW:
                xdtw_rsp_ddr_fifo.write(rsp);
                break;
            case DDR_SRC_PTW:
                ptw_rsp_ddr_fifo.write(rsp);
                break;
            case DDR_SRC_MSIPTW:
                msiptw_rsp_ddr_fifo.write(rsp);
                break;
            case DDR_SRC_CTRL_PATH:
                // Control path: copy data to ctrl_path_rsp_buf and notify
                memcpy(ctrl_path_rsp_buf, rsp.data, rsp.data_length);
                ctrl_path_rsp_event.notify(SC_ZERO_TIME);
                break;
            case DDR_SRC_MSI_MRIF:
                // [MSI] MRIF写响应: 写DDR即完成, 响应静默丢弃(不进入任何等待方)
                printf("[DDR_RSP] MSI_MRIF write response silently consumed (task_id=%u)\n",
                       rsp.task_id);
                fflush(stdout);
                break;
        }

        // Free TLM payload allocated by arbiter
        if (pending.trans_ptr) {
            if (pending.trans_ptr->get_data_ptr()) {
                delete[] pending.trans_ptr->get_data_ptr();
            }
            delete pending.trans_ptr;
        }

        // Decrement axi_master_1 outstanding counter
        axi_master_1_to_cmn_rnd_outstanding--;
        // [STAT] DDR端口读写分离并发: 收response时按is_write分别--
        if (pending.is_write) axi_master_1_write_outstanding--;
        else                  axi_master_1_read_outstanding--;
        printf("[DDR_RSP] axi_master_1 outstanding-- -> %d (task_id=%u, source=%d)\n",
               axi_master_1_to_cmn_rnd_outstanding, pending.task_id, pending.source_module);
        fflush(stdout);
        axi_master_1_slot_freed_event.notify(SC_ZERO_TIME);

        // Notify arbiter that a slot is freed
        ddr_pending_freed_event.notify(SC_ZERO_TIME);

        // Send END_RESP confirmation
        phase = tlm::END_RESP;
        return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
}

/**********************************************/
// DDR Arbiter Thread
/**********************************************/
void iommu_top::ddr_arbiter_thread() {
    uint8_t rr_index = 0;  // round-robin rotation index
    while (true) {
        // Wait for any req_ddr_fifo to have data
        // Check if data already available before waiting (avoid missing edge-triggered events)
        if (ctrl_path_req_ddr_fifo.num_available() == 0 &&
            xdtw_req_ddr_fifo.num_available() == 0 &&
            ptw_req_ddr_fifo.num_available() == 0 &&
            msiptw_req_ddr_fifo.num_available() == 0 &&
            msi_mrif_req_ddr_fifo.num_available() == 0) {
            wait(ctrl_path_req_ddr_fifo.data_written_event() |
                 xdtw_req_ddr_fifo.data_written_event() |
                 ptw_req_ddr_fifo.data_written_event() |
                 msiptw_req_ddr_fifo.data_written_event() |
                 msi_mrif_req_ddr_fifo.data_written_event());
        }

        // Process all available requests
        bool processed_any = true;
        while (processed_any) {
            processed_any = false;

            // Flow control: check global outstanding count
            if (ddr_pending_queue.size() >= DDR_MAX_OUTSTANDING) {
                wait(ddr_pending_freed_event);
            }

            // Flow control: check axi_master_1_to_cmn_rnd outstanding count
            if (axi_master_1_to_cmn_rnd_outstanding >= AXI_MASTER_1_TO_CMN_RND_MAX_OUTSTANDING) {
                wait(axi_master_1_slot_freed_event);
            }

            ddr_req_entry_t req;
            uint8_t source_module = 0;
            bool found = false;

            // Priority: ctrl_path > round-robin(xDTW, PTW, MSIPTW)
            if (ctrl_path_req_ddr_fifo.num_available() > 0) {
                ctrl_path_ddr_req_t ctrl_req = ctrl_path_req_ddr_fifo.read();
                req.task_id = 0;  // control path has no task_id
                req.addr = ctrl_req.addr;
                req.size = ctrl_req.size;
                req.is_write = ctrl_req.is_write;
                if (ctrl_req.is_write) {
                    memcpy(req.write_data, ctrl_req.write_data, ctrl_req.size);
                }
                source_module = DDR_SRC_CTRL_PATH;
                found = true;
            } else {
                // Round-robin arbitration: xDTW(0), PTW(1), MSIPTW(2), MSI_MRIF(3)
                sc_fifo<ddr_req_entry_t>* fifos[4] = {
                    &xdtw_req_ddr_fifo, &ptw_req_ddr_fifo, &msiptw_req_ddr_fifo,
                    &msi_mrif_req_ddr_fifo
                };
                for (int k = 0; k < 4; k++) {
                    uint8_t idx = (rr_index + k) % 4;
                    if (fifos[idx]->num_available() > 0) {
                        req = fifos[idx]->read();
                        // 0=XDTW, 1=PTW, 2=MSIPTW, 3数组位->DDR_SRC_MSI_MRIF(4)
                        source_module = (idx == 3) ? DDR_SRC_MSI_MRIF : idx;
                        rr_index = (idx + 1) % 4;
                        found = true;
                        break;
                    }
                }
            }

            if (!found) break;
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_trace.active);
#endif
            processed_any = true;

            const char* src_name = (source_module == DDR_SRC_XDTW) ? "XDTW" :
                                   (source_module == DDR_SRC_PTW) ? "PTW" :
                                   (source_module == DDR_SRC_MSIPTW) ? "MSIPTW" :
                                   (source_module == DDR_SRC_MSI_MRIF) ? "MSI_MRIF" : "CTRL";
            printf("[DDR_ARBITER] Route %s request: task_id=%u, addr=0x%lx, size=%d, is_write=%d -> DDR\n",
                   src_name, req.task_id, req.addr, req.size, req.is_write);
            fflush(stdout);

            // Construct ddr_pending_entry and enqueue
            ddr_pending_entry_t pending;
            pending.task_id = req.task_id;
            pending.source_module = source_module;
            pending.addr = req.addr;
            pending.size = req.size;
            pending.submit_time_ns = req.submit_time_ns;  // [STAT] 传递时间戳
            pending.is_write = req.is_write;  // [STAT] 存读写标记, 供响应侧分离读写outstanding--

            // Allocate TLM payload
            tlm::tlm_generic_payload* trans = new tlm::tlm_generic_payload();
            trans->set_address(req.addr);
            trans->set_data_length(req.size);
            trans->set_streaming_width(req.size);
            trans->set_byte_enable_ptr(nullptr);
            trans->set_dmi_allowed(false);
            trans->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

            uint8_t* data_buf = new uint8_t[req.size];
            if (req.is_write) {
                trans->set_command(tlm::TLM_WRITE_COMMAND);
                memcpy(data_buf, req.write_data, req.size);
            } else {
                trans->set_command(tlm::TLM_READ_COMMAND);
                memset(data_buf, 0, req.size);
            }
            trans->set_data_ptr(data_buf);
            pending.trans_ptr = trans;

            ddr_queue_mtx.lock();
            ddr_pending_queue.push(pending);
            ddr_queue_mtx.unlock();

            // Send nb_transport_fw to DDR
            tlm::tlm_phase phase = tlm::BEGIN_REQ;
            sc_time delay = SC_ZERO_TIME;
            axi_master_1_to_cmn_rnd_socket->nb_transport_fw(*trans, phase, delay);

            // ===== Bandwidth control: master_1 port (DDR access) =====
            // delay_ns = 1000 * length * 8 / bandwidth_mbps
            double master1_bw_delay_ns = 1000.0 * req.size * 8 / AXI_MASTER_1_BANDWIDTH_MBPS;
            master_1_total_bytes += req.size;  // [STAT] DDR字节计数
            wait(master1_bw_delay_ns, SC_NS);

            // Increment outstanding counter after successful send
            axi_master_1_to_cmn_rnd_outstanding++;
            if (axi_master_1_to_cmn_rnd_outstanding > peak_axi_master_1_outstanding)
                peak_axi_master_1_outstanding = axi_master_1_to_cmn_rnd_outstanding;
            // [STAT] DDR端口读写分离并发: 发请求时按is_write分别++并更新峰值
            if (req.is_write) {
                axi_master_1_write_outstanding++;
                axi_master_1_write_req_total++;
                if (axi_master_1_write_outstanding > peak_axi_master_1_write_outstanding)
                    peak_axi_master_1_write_outstanding = axi_master_1_write_outstanding;
            } else {
                axi_master_1_read_outstanding++;
                axi_master_1_read_req_total++;
                if (axi_master_1_read_outstanding > peak_axi_master_1_read_outstanding)
                    peak_axi_master_1_read_outstanding = axi_master_1_read_outstanding;
            }
            printf("[DDR_ARBITER] axi_master_1 outstanding++ -> %d (task_id=%u)\n",
                   axi_master_1_to_cmn_rnd_outstanding, req.task_id);
            fflush(stdout);
        }
    }
}

/**********************************************/
// send_response_to_initiator - send AT response back to RP
/**********************************************/
void iommu_top::send_response_to_initiator(iommu_task_t* task) {
    tlm::tlm_generic_payload* trans = task->tlm_trans_ptr;
    if (!trans) return;

    // Set translation result into payload
    trans->set_address(task->pa);

    // For b_transport tasks, skip nb_transport_bw (response returned via b_transport return path)
    if (task->is_b_transport) {
        return;
    }

    double ts_ns = sc_time_stamp().to_seconds() * 1e9;
    double entry_ns = task->timestamp.to_seconds() * 1e9;
    double e2e_ns = ts_ns - entry_ns;
    printf("[IOMMU_TOP] send_response_to_initiator: task_id=%u, pa=0x%lx, state=%d [entry=%.1f ns, exit=%.1f ns, e2e=%.1f ns]\n",
           task->task_id, task->pa, task->state, entry_ns, ts_ns, e2e_ns);
    fflush(stdout);

#if TEST_CFG_MULTI_DEVICE_SCENE
    md_trace.stamp(task->task_id, iommu_md::Point::RESPONSE);
#endif
    // Send nb_transport_bw to notify initiator
    // [读写端口分离] 响应按任务方向从对应端口返回: 写->写口, 读->读口。
    tlm::tlm_phase phase = tlm::BEGIN_RESP;
    sc_time delay = SC_ZERO_TIME;
#if TEST_CFG_SPLIT_RW_PORTS
    if (task->read_writeAMO == WRITE) {
        axi_slave_from_pcie_noc_0w_socket->nb_transport_bw(*trans, phase, delay);
    } else {
        axi_slave_from_pcie_noc_0_socket->nb_transport_bw(*trans, phase, delay);
    }
#else
    axi_slave_from_pcie_noc_0_socket->nb_transport_bw(*trans, phase, delay);
#endif
}

/**********************************************/
// configure_and_route - common routing after DC/PC configuration
/**********************************************/
void iommu_top::configure_and_route(iommu_task_t* task) {
    // Configure translation parameters
    task->PSCV = (task->iosatp.MODE == IOSATP_Bare) ? 0 : 1;
    task->GV = (task->iohgatp.MODE == IOHGATP_Bare) ? 0 : 1;
    task->GSCID = (task->GV == 0) ? 0 : task->iohgatp.GSCID;
    task->PSCID = (task->PSCV == 0) ? 0 : task->PSCID;
#if TEST_CFG_MULTI_DEVICE_SCENE
    iommu_md::require(task->device_id < md_trace.devices && task->GSCID == task->device_id + 1 &&
                      task->PSCID == 0, "DC配置后任务GSCID/PSCID与设备身份不匹配");
    md_trace.progress();
#endif
    task->check_access_perms =
        (task->TTYP != PCIE_ATS_TRANSLATION_REQUEST) ? 1 : 0;

    // ===================== [MSI] 分流决策 (获取DC/PC后, 点2) =====================
    // MSI请求与普通数据翻译请求在此分流, MSI通路与PT Cache/Walker Cache/PTW完全独立:
    // - S1=Bare(输入地址即GPA, 只开启一级翻译): 直接用当前地址做MSI识别,
    //   命中 -> 分流到独立的MSIPT大模块(内部先查MSIPT Cache,
    //          MISS由MSI PTW自行DDR walk, 与原始PTW无关);
    // - S1非Bare(单级/两级): 无法在此判断(需S1得到GPA后才能判断),
    //   task->DC已携带 msiptp.MODE/msi_addr_mask/msi_addr_pattern,
    //   按原始流程走PT Cache/Walker/dedup/PTW, 由PTW在S1完成后识别(点4)。
    if (task->DC.msiptp.MODE != MSIPTP_Off) {
        if (task->iosatp.MODE == IOSATP_Bare) {
            if (msi_id_check(task, task->iova)) {
                printf("[CONFIGURE] task_id=%u -> MSI hit at DC/PC (S1=Bare, addr=GPA), divert to MSIPT module\n",
                       task->task_id);
                fflush(stdout);
                route_to_msipt(task);
                return;
            }
        }
        printf("[CONFIGURE] task_id=%u -> MSI-capable device (msiptp.MODE=%d, iosatp.MODE=%d)%s\n",
               task->task_id, task->DC.msiptp.MODE, task->iosatp.MODE,
               (task->iosatp.MODE == IOSATP_Bare) ? ", normal flow" :
               ", identify after S1 in PTW");
        fflush(stdout);
    }

    task->state = TASK_ROUTE_DECISION;
    
    // [NEW] Phase 1: 启用预取功能 (使用配置参数PT_DEDUP_PREFETCH_DEPTH)
    // D=0表示关闭预取，D>0表示启用预取
    // [v6] MSI设备也开启预取: 普通Data任务正常预取(late-spawn);
    //      MSI任务在PTW识别后禁用预取并清理dedup占位(见iommu_perf_ptw.cc)
    if (!task->walk_ctx.prefetch_enabled && PT_DEDUP_PREFETCH_DEPTH > 0) {
        task->walk_ctx.prefetch_enabled = true;
        task->walk_ctx.prefetch_depth = PT_DEDUP_PREFETCH_DEPTH;
        printf("[CONFIGURE] task_id=%u -> Prefetch enabled (D=%u)\n",
               task->task_id, task->walk_ctx.prefetch_depth);
    } else if (PT_DEDUP_PREFETCH_DEPTH == 0) {
        task->walk_ctx.prefetch_enabled = false;
        task->walk_ctx.prefetch_depth = 0;
        printf("[CONFIGURE] task_id=%u -> Prefetch DISABLED (D=0)\n", task->task_id);
    }

    // Save original task to pending map for PT cache response correlation
    pt_cache_mtx.lock();
    pt_cache_pending_tasks[task->task_id] = task;
    pt_cache_mtx.unlock();

    // [前置] Walker Cache前置查询: 与PT Cache查询同时发起, 结果由
    // walker_front_response_thread写回task->walk_ctx.walker_front_*字段,
    // 随任务透传经去重模块直到PTW直接使用
    // [MSI] MSI使能设备不发起前置查询: 端到端leaf命中无GPA中间值,
    // 无法做MSI识别; 此类设备走同步walker查询+完整walk路径
    if (PTW_WALKER_CACHE_ENABLED && WALKER_FRONT_ENABLED &&
        task->DC.msiptp.MODE == MSIPTP_Off) {
        task->walk_ctx.walker_front_requested = true;
        walker_front_mtx.lock();
        walker_front_pending[task->task_id] = task;
        walker_front_mtx.unlock();
        iommu::CacheMessage walker_req = task_to_walker_request(task);
        walker_req.timestamp = sc_time_stamp();
        cache_sub.walker_front_request_fifo.write(walker_req);
    }

    // Convert task to CacheMessage and write to cache_sub.pt_request_fifo
    iommu::CacheMessage pt_req = task_to_pt_request(task);
    pt_req.timestamp = sc_time_stamp();  // [STAT] 记录FIFO写入时刻
    cache_sub.pt_request_fifo.write(pt_req);
}

/**********************************************/
// walker_front_response_thread - [前置] Walker前置查询响应处理
// 读walker_front_response_fifo, 按task_id查pending表:
//   命中: 结果写入walk_ctx.walker_front_*字段, 置valid并通知PTW兜底等待
//   已移除(PT HIT已直接输出): 丢弃响应, 不触碰task(防use-after-free)
/**********************************************/
void iommu_top::walker_front_response_thread() {
    while (true) {
        iommu::CacheMessage resp = cache_sub.walker_front_response_fifo.read();

        walker_front_mtx.lock();
        auto it = walker_front_pending.find(resp.task_id);
        if (it == walker_front_pending.end()) {
            walker_front_mtx.unlock();
            // PT HIT常规CL已直接输出并移除表项: 丢弃前置结果
            printf("[WALKER_FRONT] task_id=%u response discarded (task already output via PT HIT)\n",
                   (unsigned)resp.task_id);
            fflush(stdout);
            continue;
        }
        iommu_task_t* task = it->second;
        walker_front_pending.erase(it);
        walker_front_mtx.unlock();

        // 记录前置查询结果到任务信息(一直保存直到输入PTW)
        task->walk_ctx.walker_front_hit = resp.hit;
        task->walk_ctx.walker_front_level = resp.hit ? resp.walker_level : 0;
        task->walk_ctx.walker_front_next_ppn = resp.hit ? resp.walker_data.next_ppn : 0;
        // [大页] 端到端leaf命中: 页大小由命中子表级推导(C3=2MB/C2=1GB/C1=512GB)
        if (resp.hit && iommu::walker_is_leaf(resp.walker_data)) {
            task->walk_ctx.walker_front_is_leaf = true;
            task->walk_ctx.walker_front_leaf_page_sz =
                (resp.walker_level == 3) ? 0x200000ULL :
                (resp.walker_level == 2) ? 0x40000000ULL : 0x8000000000ULL;
        } else {
            task->walk_ctx.walker_front_is_leaf = false;
            task->walk_ctx.walker_front_leaf_page_sz = 0;
        }
        task->walk_ctx.walker_front_valid = true;

        printf("[WALKER_FRONT] task_id=%u result recorded: hit=%d, level=%u, next_ppn=0x%lx%s\n",
               task->task_id, resp.hit ? 1 : 0,
               task->walk_ctx.walker_front_level,
               (unsigned long)task->walk_ctx.walker_front_next_ppn,
               task->walk_ctx.walker_front_is_leaf ? " [LEAF]" : "");
        fflush(stdout);

        // 通知PTW输入侧的兜底等待
        walker_front_ready_event.notify(SC_ZERO_TIME);
    }
}

/**********************************************/
// init_gstage_walk - initialize G-stage walk context
/**********************************************/
void iommu_top::init_gstage_walk(iommu_task_t* task, uint64_t gpa) {
    // Corresponds to iommu_second_stage_trans.cc L80-139
    uint16_t gs_vpn[5];
    uint8_t GS_LEVELS;
    extract_gs_vpn(gpa, task->iohgatp.MODE, gs_vpn, &GS_LEVELS);
    for (int k = 0; k < 5; k++) task->walk_ctx.gs_vpn[k] = gs_vpn[k];
    task->walk_ctx.gs_level = GS_LEVELS - 1;
    task->walk_ctx.gs_base_addr = task->iohgatp.PPN * PAGESIZE;
    uint64_t gs_pte_addr = task->walk_ctx.gs_base_addr +
                           gs_vpn[GS_LEVELS - 1] * 8; // G-stage PTESIZE=8
    task->walk_ctx.read_addr = gs_pte_addr;
    task->walk_ctx.read_size = 8;

    // [S2] 重置S2 Walker Cache中间结果，防止前一个任务的残留数据污染
    task->walk_ctx.s2_walker_hit_level = 0;
    task->walk_ctx.s2_walker_cache_entries.s2_ppn_level1 = 0;
    task->walk_ctx.s2_walker_cache_entries.s2_ppn_level2 = 0;
    task->walk_ctx.s2_walker_cache_entries.s2_ppn_level3 = 0;
    task->walk_ctx.s2_walker_cache_entries.s2_valid_level1 = false;
    task->walk_ctx.s2_walker_cache_entries.s2_valid_level2 = false;
    task->walk_ctx.s2_walker_cache_entries.s2_valid_level3 = false;
}

/**********************************************/
// va_dedup_recover - VA去重恢复：PTW完成后将挂起的同页任务批量转发
/**********************************************/
void iommu_top::va_dedup_recover(iommu_task_t* completed_task, bool is_fault) {
    if (!PT_CACHE_VA_DEDUP_ENABLED) return;

    // [FIX] 提前缓存 completed_task 数据到局部变量，避免后续 FIFO write yield 时
    //       forwarder 线程读取并修改 completed_task，导致 page_sz 等字段被破坏
    uint64_t  saved_pa          = completed_task->pa;
    uint64_t  saved_page_sz     = completed_task->page_sz;
    uint64_t  saved_gst_page_sz = completed_task->gst_page_sz;
    spte_t    saved_vs_pte      = completed_task->vs_pte;
    gpte_t    saved_g_pte       = completed_task->g_pte;
    uint32_t  saved_cause       = completed_task->cause;
    uint32_t  saved_task_id     = completed_task->task_id;

    va_dedup_key_t key;
    key.gscid = completed_task->GSCID;
    key.pscid = completed_task->PSCID;
    key.page_iova = completed_task->iova & ~0xFFFULL;

    va_dedup_mtx.lock();
    auto it = va_dedup_table.find(key);
    if (it == va_dedup_table.end() || !it->second.valid) {
        va_dedup_mtx.unlock();
        return;
    }
    std::vector<iommu_task_t*> pending = std::move(it->second.pending_tasks);
    va_dedup_table.erase(it);
    va_dedup_mtx.unlock();

    for (auto* ptask : pending) {
        if (is_fault) {
            ptask->cause = saved_cause;
            ptask->state = TASK_FAULT;
        } else {
            // 复制翻译结果，PA按各自的页内偏移计算
            uint64_t page_mask = saved_page_sz - 1;
            ptask->pa = (saved_pa & ~page_mask) | (ptask->iova & page_mask);
            ptask->vs_pte = saved_vs_pte;
            ptask->g_pte = saved_g_pte;
            ptask->page_sz = saved_page_sz;
            ptask->gst_page_sz = saved_gst_page_sz;
            ptask->state = TASK_PTW_DONE;
        }
        printf("[VA_DEDUP_RECOVER] task_id=%u, pa=0x%lx (from task_id=%u)\n",
               ptask->task_id, ptask->pa, saved_task_id);
        fflush(stdout);
        pt_cache_to_fwd_fifo.write(ptask);
    }
    if (!pending.empty()) {
        printf("[VA_DEDUP_RECOVER] Recovered %zu tasks for page 0x%lx\n",
               pending.size(), key.page_iova);
        fflush(stdout);
    }
}

/**********************************************/
// print_cache_statistics - 打印Cache命中率统计信息
/**********************************************/
#if TEST_CFG_MULTI_DEVICE_SCENE
bool iommu_top::md_quiescent() const {
    if (!md_trace.all_responded() || md_trace.input_pending || md_trace.output_pending ||
        md_trace.aggregate_pending || md_trace.req_peq_pending || md_trace.rsp_peq_pending || md_trace.active)
        return false;
    if (!pending_tasks.empty() || !pt_cache_pending_tasks.empty() || !walker_front_pending.empty() ||
        !xdtw_active_walks.empty() || !ptw_active_walks.empty() || !msiptw_active_walks.empty() ||
        !prefetch_groups.empty() || !reorder_buf.empty() || !reorder_write_order.empty() ||
        !ddr_pending_queue.empty() || iommu_global_outstanding || iommu_read_outstanding ||
        iommu_write_outstanding || ptw_outstanding_task_count || msiptw_outstanding_task_count ||
        axi_master_1_to_cmn_rnd_outstanding || axi_master_0_to_pcie_noc_outstanding)
        return false;
#if TEST_CFG_PERDEV_WRITE_ORDER
    for (unsigned d = 0; d < iommu_md::MAX_DEVICE_COUNT; d++)
        if (!reorder_write_order_dev[d].empty()) return false;
#endif
    const sc_fifo<iommu_task_t*>* task_fifos[] = {
        &inbound_fifo_r, &inbound_fifo_w, &parser_to_collector_fifo, &collector_to_xdtw_dc_fifo, &collector_to_xdtw_pc_fifo,
        &collector_to_msipt_cache_query_fifo, &collector_to_fault_fifo, &xdtw_to_collector_fifo,
        &pt_cache_to_ptw_fifo, &ptw_to_pt_cache_fifo, &pt_cache_to_fwd_fifo,
        &msipt_cache_to_msiptw_fifo, &msiptw_to_msipt_cache_fifo, &msipt_cache_to_fwd_fifo, &ptw_response_fifo};
    for (auto* f : task_fifos) if (f->num_available()) return false;
    if (xdtw_req_ddr_fifo.num_available() || xdtw_rsp_ddr_fifo.num_available() ||
        ptw_req_ddr_fifo.num_available() || ptw_rsp_ddr_fifo.num_available() ||
        msiptw_req_ddr_fifo.num_available() || msiptw_rsp_ddr_fifo.num_available() ||
        msi_mrif_req_ddr_fifo.num_available() || ctrl_path_req_ddr_fifo.num_available()) return false;
    return cache_sub.md_quiescent();
}

void iommu_top::md_update_write_hol() {
    std::map<uint32_t, uint32_t> blocked;
#if TEST_CFG_PERDEV_WRITE_ORDER
    // [每设备写保序] 跨设备写HOL按定义不存在: 仅关闭未结算区间
    md_trace.update_hol(blocked);
    (void)reorder_buf;
#else
    uint32_t head = 0;
    for (const auto& pair : reorder_buf) {
        const auto& entry = pair.second;
        if (!entry.is_write) continue;
        if (!head && !entry.ready) head = pair.first;
        if (head && entry.ready && md_trace.task(head).device != entry.task->device_id)
            blocked.emplace(pair.first, head);
    }
    md_trace.update_hol(blocked);
#endif
}
#endif

void iommu_top::print_cache_statistics() {
    // [FIX] 等待 pt_update_worker_thread 处理完 FIFO 中所有剩余请求，修正仿真终止时的统计误差
    // [多RAM] 同时等待内部 RAM FIFO 排空(多RAM改造后任务可能滞留在分发FIFO中)
    // [dedup多RAM] dedup 内部在途任务(hash_in/RAM FIFO/inside预取)同样需排空
    while (cache_sub.pt_update_fifo.num_available() > 0 ||
           cache_sub.pt_ram_fifo_pending() > 0 ||
           cache_sub.dedup_ram_fifo_pending() > 0) {
        sc_core::wait(sc_core::SC_ZERO_TIME);
    }

    printf("\n========== Cache Hit/Miss Statistics (CacheSubsystem Internal) ==========\n");
    
    // 使用CacheSubsystem内部的StatsCollector
    cache_sub.stats().print_summary(std::cout);
    
    printf("========================================================================\n\n");

    // [STAT] PT Scheduler任务间隔分析
    cache_sub.print_pt_scheduler_gap_report();

    // [STAT] DC Cache 查询完成时间间隔统计
    {
        printf("========== DC Cache Query Interval Statistics ==========\n");
        printf("  Total DC queries:      %lu\n", (unsigned long)cache_sub.get_dc_query_total_count());
        printf("  Interval samples:      %lu\n", (unsigned long)cache_sub.get_dc_query_interval_count());
        if (cache_sub.get_dc_query_interval_count() > 0) {
            printf("  Avg query interval:    %.2f ns\n", cache_sub.get_dc_query_interval_avg_ns());
            printf("  Max query interval:    %.2f ns\n", cache_sub.get_dc_query_interval_max_ns());
            printf("  Min query interval:    %.2f ns\n", cache_sub.get_dc_query_interval_min_ns());
        }
        printf("==========================================================\n\n");
    }

    // [STAT] PC Cache 查询完成时间间隔统计(与DC同口径)
    {
        printf("========== PC Cache Query Interval Statistics ==========\n");
        printf("  Total PC queries:      %lu\n", (unsigned long)cache_sub.get_pc_query_total_count());
        printf("  Interval samples:      %lu\n", (unsigned long)cache_sub.get_pc_query_interval_count());
        if (cache_sub.get_pc_query_interval_count() > 0) {
            printf("  Avg query interval:    %.2f ns\n", cache_sub.get_pc_query_interval_avg_ns());
            printf("  Max query interval:    %.2f ns\n", cache_sub.get_pc_query_interval_max_ns());
            printf("  Min query interval:    %.2f ns\n", cache_sub.get_pc_query_interval_min_ns());
        }
        printf("==========================================================\n\n");
    }

    // [STAT] 32任务组REQUEST排队/执行延时统计
    cache_sub.print_pt_group_report();

    // [STAT] Dedup Scheduler 执行延时统计
    cache_sub.print_dedup_scheduler_report();

    // [多RAM] PT Cache 多 RAM 统计报告
    cache_sub.print_pt_multi_ram_report();

    // [STAT] 串行环节预算表(每包固定延时归属/多RAM分布与同RAM背靠背)
    cache_sub.print_serial_stage_budget_report();

    // [STAT需求1] IOMMU 内部并发 4KB 页统计(每设备 + 全设备)
    print_4kb_concurrency_report();

    // [dedup多RAM] 去重Cache 多 RAM 统计报告
    cache_sub.print_dedup_multi_ram_report();
    
        // [walker多RAM] Walker Cache 多 RAM 统计报告
        cache_sub.print_walker_multi_ram_report();

    // VA Dedup Statistics
    printf("========== VA Dedup Statistics ==========\n");
    printf("  VA Dedup Enabled:  %s\n", PT_CACHE_VA_DEDUP_ENABLED ? "YES" : "NO");
    printf("  Dedup Table Miss:  %lu (sent to PTW)\n", (unsigned long)va_dedup_miss_count);
    printf("  Dedup Table Hit:   %lu (saved PTW tasks)\n", (unsigned long)va_dedup_hit_count);
    if (va_dedup_miss_count + va_dedup_hit_count > 0) {
        printf("  PTW Tasks Saved:   %.1f%% reduction\n",
               100.0 * va_dedup_hit_count / (va_dedup_miss_count + va_dedup_hit_count));
    }
    printf("==========================================\n\n");

    // [MSI] MSI Path Statistics
    printf("========== MSI Path Statistics ==========\n");
    printf("  MSIPT Cache Hit:   %lu\n", (unsigned long)g_msipt_cache_hit_count);
    printf("  MSIPT Cache Miss:  %lu (MSIPTW DDR walk)\n", (unsigned long)g_msipt_cache_miss_count);
    if (g_msipt_cache_hit_count + g_msipt_cache_miss_count > 0) {
        printf("  MSIPT Hit Rate:    %.1f%%\n",
               100.0 * g_msipt_cache_hit_count / (g_msipt_cache_hit_count + g_msipt_cache_miss_count));
    }
    printf("  Flat MSI done:     %lu\n", (unsigned long)msipt_flat_count);
    printf("  MRIF MSI done:     %lu\n", (unsigned long)msipt_mrif_count);
    printf("  MRIF atomic OR:    %lu\n", (unsigned long)msi_atomic_or_count);
    printf("  notice MSI sent:   %lu\n", (unsigned long)msi_notice_count);
    printf("==========================================\n\n");

    // Walker Cache Update Statistics
    printf("========== Walker Cache Update Statistics ==========\n");
    printf("  PTWC_1_2_3 updates:  %lu  (update c1+c2+c3)\n", 
           (unsigned long)cache_sub.walker_cache().get_update_ptwc_123_count());
    printf("  PTWC_2_3 updates:    %lu  (update c2+c3)\n",
           (unsigned long)cache_sub.walker_cache().get_update_ptwc_23_count());
    printf("  PTWC_3 updates:      %lu  (update c3 only)\n",
           (unsigned long)cache_sub.walker_cache().get_update_ptwc_3_count());
    printf("  NONE (skipped):      %lu  (redundant updates avoided)\n",
           (unsigned long)cache_sub.walker_cache().get_update_none_count());
    uint64_t total_updates = cache_sub.walker_cache().get_update_ptwc_123_count() +
                             cache_sub.walker_cache().get_update_ptwc_23_count() +
                             cache_sub.walker_cache().get_update_ptwc_3_count();
    uint64_t total_requests = total_updates + cache_sub.walker_cache().get_update_none_count();
    printf("  Total updates:       %lu\n", (unsigned long)total_updates);
    printf("  Total requests:      %lu\n", (unsigned long)total_requests);
    if (total_requests > 0) {
        printf("  Update distribution:\n");
        printf("    PTWC_1_2_3: %.1f%%\n", 
               100.0 * cache_sub.walker_cache().get_update_ptwc_123_count() / total_requests);
        printf("    PTWC_2_3:   %.1f%%\n",
               100.0 * cache_sub.walker_cache().get_update_ptwc_23_count() / total_requests);
        printf("    PTWC_3:     %.1f%%\n",
               100.0 * cache_sub.walker_cache().get_update_ptwc_3_count() / total_requests);
        printf("    NONE:       %.1f%%  (redundancy elimination rate)\n",
               100.0 * cache_sub.walker_cache().get_update_none_count() / total_requests);
    }
    printf("======================================================\n\n");

    // [VS] VS-stage Walker Cache Statistics
    printf("========== VS-stage Walker Cache Statistics ==========\n");
    {
        uint64_t vs_lookup = cache_sub.walker_cache().get_vs_lookup_count();
        uint64_t vs_hit_c3 = cache_sub.walker_cache().get_vs_hit_c3_count();
        uint64_t vs_hit_c2 = cache_sub.walker_cache().get_vs_hit_c2_count();
        uint64_t vs_hit_c1 = cache_sub.walker_cache().get_vs_hit_c1_count();
        uint64_t vs_miss   = cache_sub.walker_cache().get_vs_miss_count();
        uint64_t vs_total_hit = vs_hit_c3 + vs_hit_c2 + vs_hit_c1;
        printf("  VS Lookups:          %lu\n", (unsigned long)vs_lookup);
        printf("  VS Hits (total):     %lu\n", (unsigned long)vs_total_hit);
        printf("    C3 hit (1 DDR):    %lu\n", (unsigned long)vs_hit_c3);
        printf("    C2 hit (2 DDR):    %lu\n", (unsigned long)vs_hit_c2);
        printf("    C1 hit (3 DDR):    %lu\n", (unsigned long)vs_hit_c1);
        printf("  VS Miss (4 DDR):     %lu\n", (unsigned long)vs_miss);
        // [大页] 端到端leaf命中(包含在对应级hit中, 命中后PTW 0次DDR)
        printf("  VS LEAF hits:        %lu  (end-to-end hugepage leaf, 0 DDR)\n",
               (unsigned long)cache_sub.walker_cache().get_vs_leaf_hit_count());
        if (vs_lookup > 0) {
            printf("  VS Hit Rate:         %.1f%%\n", 100.0 * vs_total_hit / vs_lookup);
            printf("  VS Hit Distribution (of lookups):\n");
            printf("    C3: %.1f%%  C2: %.1f%%  C1: %.1f%%  Miss: %.1f%%\n",
                   100.0 * vs_hit_c3 / vs_lookup,
                   100.0 * vs_hit_c2 / vs_lookup,
                   100.0 * vs_hit_c1 / vs_lookup,
                   100.0 * vs_miss / vs_lookup);
            if (vs_total_hit > 0) {
                printf("  VS Hit Distribution (of hits):\n");
                printf("    C3: %.1f%%  C2: %.1f%%  C1: %.1f%%\n",
                       100.0 * vs_hit_c3 / vs_total_hit,
                       100.0 * vs_hit_c2 / vs_total_hit,
                       100.0 * vs_hit_c1 / vs_total_hit);
            }
            uint64_t ddr_saved = vs_hit_c3 * 3 + vs_hit_c2 * 2 + vs_hit_c1 * 1;
            uint64_t ddr_baseline = vs_lookup * 4;
            uint64_t ddr_actual = vs_lookup * 4 - ddr_saved;
            printf("  DDR Walks Saved:     %lu  (vs baseline %lu reads)\n", (unsigned long)ddr_saved, (unsigned long)ddr_baseline);
            printf("  DDR Actual Reads:    %lu  (saved %.1f%%)\n", (unsigned long)ddr_actual, 100.0 * ddr_saved / ddr_baseline);
            // [ANALYSIS] C1/C2/C3命中率分析
            printf("  ---\n");
            printf("  [Analysis] VS-stage Cache Level Hit Rate:\n");
            printf("    C3 (leaf, 1 DDR):  %lu/%lu = %.1f%%\n", (unsigned long)vs_hit_c3, (unsigned long)vs_lookup, 100.0 * vs_hit_c3 / vs_lookup);
            printf("    C2 (L1, 2 DDR):    %lu/%lu = %.1f%%\n", (unsigned long)vs_hit_c2, (unsigned long)vs_lookup, 100.0 * vs_hit_c2 / vs_lookup);
            printf("    C1 (L2, 3 DDR):    %lu/%lu = %.1f%%\n", (unsigned long)vs_hit_c1, (unsigned long)vs_lookup, 100.0 * vs_hit_c1 / vs_lookup);
            printf("    Miss (4 DDR):      %lu/%lu = %.1f%%\n", (unsigned long)vs_miss, (unsigned long)vs_lookup, 100.0 * vs_miss / vs_lookup);
            printf("    Avg DDR/lookup:    %.2f  (baseline=4.00, saved=%.2f)\n",
                   (double)ddr_actual / vs_lookup, (double)ddr_saved / vs_lookup);
        }
    }
    printf("==========================================================\n\n");

    // [S2] S2 Walker Cache Statistics
    printf("========== S2 Walker Cache Statistics ==========\n");
    printf("  S2 Cache Enabled:    %s\n", PTW_WALKER_S2_CACHE_ENABLED ? "YES" : "NO");
    {
        uint64_t s2_lookup = cache_sub.walker_cache().get_s2_lookup_count();
        uint64_t s2_hit_c3 = cache_sub.walker_cache().get_s2_hit_c3_count();
        uint64_t s2_hit_c2 = cache_sub.walker_cache().get_s2_hit_c2_count();
        uint64_t s2_hit_c1 = cache_sub.walker_cache().get_s2_hit_c1_count();
        uint64_t s2_miss   = cache_sub.walker_cache().get_s2_miss_count();
        uint64_t s2_total_hit = s2_hit_c3 + s2_hit_c2 + s2_hit_c1;
        printf("  S2 Lookups:          %lu\n", (unsigned long)s2_lookup);
        printf("  S2 Hits (total):     %lu\n", (unsigned long)s2_total_hit);
        printf("    C3 hit (1 DDR):    %lu\n", (unsigned long)s2_hit_c3);
        printf("    C2 hit (2 DDR):    %lu\n", (unsigned long)s2_hit_c2);
        printf("    C1 hit (3 DDR):    %lu\n", (unsigned long)s2_hit_c1);
        printf("  S2 Miss (4 DDR):     %lu\n", (unsigned long)s2_miss);
        // [大页] S2端到端leaf命中(GS_EXPLICIT 0次DDR)
        printf("  S2 LEAF hits:        %lu  (end-to-end hugepage leaf, 0 DDR)\n",
               (unsigned long)cache_sub.walker_cache().get_s2_leaf_hit_count());
        if (s2_lookup > 0) {
            printf("  S2 Hit Rate:         %.1f%%\n", 100.0 * s2_total_hit / s2_lookup);
            printf("  S2 Hit Distribution (of lookups):\n");
            printf("    C3: %.1f%%  C2: %.1f%%  C1: %.1f%%  Miss: %.1f%%\n",
                   100.0 * s2_hit_c3 / s2_lookup,
                   100.0 * s2_hit_c2 / s2_lookup,
                   100.0 * s2_hit_c1 / s2_lookup,
                   100.0 * s2_miss / s2_lookup);
            if (s2_total_hit > 0) {
                printf("  S2 Hit Distribution (of hits):\n");
                printf("    C3: %.1f%%  C2: %.1f%%  C1: %.1f%%\n",
                       100.0 * s2_hit_c3 / s2_total_hit,
                       100.0 * s2_hit_c2 / s2_total_hit,
                       100.0 * s2_hit_c1 / s2_total_hit);
            }
            // DDR walk节省次数: 每次C3 hit省3次, C2 hit省2次, C1 hit省1次
            uint64_t ddr_saved = s2_hit_c3 * 3 + s2_hit_c2 * 2 + s2_hit_c1 * 1;
            uint64_t ddr_baseline = s2_lookup * 4;
            uint64_t ddr_actual = s2_lookup * 4 - ddr_saved;
            printf("  DDR Walks Saved:     %lu  (vs baseline %lu reads)\n", (unsigned long)ddr_saved, (unsigned long)ddr_baseline);
            printf("  DDR Actual Reads:    %lu  (saved %.1f%%)\n", (unsigned long)ddr_actual, 100.0 * ddr_saved / ddr_baseline);
            // [ANALYSIS] C1/C2/C3命中率分析
            printf("  ---\n");
            printf("  [Analysis] S2 Cache Level Hit Rate:\n");
            printf("    C3 (leaf, 1 DDR):  %lu/%lu = %.1f%%\n", (unsigned long)s2_hit_c3, (unsigned long)s2_lookup, 100.0 * s2_hit_c3 / s2_lookup);
            printf("    C2 (L1, 2 DDR):    %lu/%lu = %.1f%%\n", (unsigned long)s2_hit_c2, (unsigned long)s2_lookup, 100.0 * s2_hit_c2 / s2_lookup);
            printf("    C1 (L2, 3 DDR):    %lu/%lu = %.1f%%\n", (unsigned long)s2_hit_c1, (unsigned long)s2_lookup, 100.0 * s2_hit_c1 / s2_lookup);
            printf("    Miss (4 DDR):      %lu/%lu = %.1f%%\n", (unsigned long)s2_miss, (unsigned long)s2_lookup, 100.0 * s2_miss / s2_lookup);
            printf("    Avg DDR/lookup:    %.2f  (baseline=4.00, saved=%.2f)\n",
                   (double)ddr_actual / s2_lookup, (double)ddr_saved / s2_lookup);
        }
    }
    printf("================================================\n\n");

    // IOMMU / PTW IOPS
    double sim_time_sec = sc_time_stamp().to_seconds();
    printf("========== IOMMU / PTW Throughput (IOPS) ==========\n");
    printf("  Sim Time:             %.3f us\n", sim_time_sec * 1e6);
    if (sim_time_sec > 0) {
        double iommu_iops_mps = (double)iommu_total_completed / sim_time_sec / 1e6;
        double ptw_iops_mps   = (double)ptw_total_completed   / sim_time_sec / 1e6;
        printf("  IOMMU Completed:      %lu trans\n",  (unsigned long)iommu_total_completed);
        printf("  IOMMU IOPS:           %.2f M trans/s\n", iommu_iops_mps);
        printf("  PTW  Completed:       %lu tasks\n",  (unsigned long)ptw_total_completed);
        printf("    Main tasks:         %lu\n",  (unsigned long)ptw_main_task_count);
        printf("    Prefetch tasks:     %lu\n",  (unsigned long)ptw_prefetch_task_count);
        // [大页] 大页任务统计
        printf("    Hugepage main:      %lu  (page_sz>4KB, prefetch spawn skipped)\n",
               (unsigned long)ptw_hugepage_main_count);
        printf("    Hugepage PF skip:   %lu  (prefetch spawns suppressed)\n",
               (unsigned long)ptw_hugepage_pf_skipped);
        printf("    Invalid result slots: %lu  (D per hugepage group, placeholder-clear only)\n",
               (unsigned long)ptw_hugepage_invalid_slots);
        printf("    Front LEAF hits:    %lu  (short-circuit complete, 0 DDR)\n",
               (unsigned long)ptw_front_leaf_hits);
        printf("  PTW  IOPS:            %.2f M tasks/s\n", ptw_iops_mps);
        if (ptw_total_completed > 0) {
            printf("  PTW  avg DDR reads:   %.2f reads/task\n",
                   (double)ptw_total_ddr_reads / ptw_total_completed);
            printf("  PTW  avg exec lat:    %.1f ns  (%.3f us)\n",
                   ptw_total_exec_ns / ptw_total_completed,
                   ptw_total_exec_ns / ptw_total_completed / 1000.0);
            
            // [STAT] PTW详细DDR访问统计
            printf("  ---\n");
            printf("  PTW DDR access detail:\n");
            printf("    Max DDR reads/task:  %u\n", ptw_max_ddr_reads);
            printf("    Min DDR reads/task:  %u\n", ptw_min_ddr_reads == 999999 ? 0 : ptw_min_ddr_reads);
            printf("    Avg DDR latency:     %.1f ns\n",
                   ptw_ddr_latency_count > 0 ? ptw_total_ddr_latency_ns / ptw_ddr_latency_count : 0);
            printf("    Max DDR latency:     %.1f ns\n", ptw_max_ddr_latency_ns);
            printf("    Min DDR latency:     %.1f ns\n", ptw_min_ddr_latency_ns == 999999999.0 ? 0 : ptw_min_ddr_latency_ns);
            printf("  ---\n");
            printf("  PTW task latency:\n");
            printf("    Max task latency:    %.1f ns  (%.3f us)\n",
                   ptw_max_task_latency_ns, ptw_max_task_latency_ns / 1000.0);
            printf("    Min task latency:    %.1f ns  (%.3f us)\n",
                   ptw_min_task_latency_ns == 999999999.0 ? 0 : ptw_min_task_latency_ns,
                   ptw_min_task_latency_ns == 999999999.0 ? 0 : ptw_min_task_latency_ns / 1000.0);
            // [NEW] PTW任务注入/输出间隔统计
            printf("  ---\n");
            printf("  PTW inject interval (pt_cache_to_ptw_fifo):\n");
            printf("    Avg inject interval: %.2f ns\n",
                   ptw_inject_interval_count > 0 ? ptw_inject_interval_total_ns / ptw_inject_interval_count : 0);
            printf("    Max inject interval: %.2f ns\n", ptw_inject_interval_max_ns);
            printf("    Min inject interval: %.2f ns\n",
                   ptw_inject_interval_min_ns == 999999999.0 ? 0 : ptw_inject_interval_min_ns);
            printf("    Inject count:        %lu\n", (unsigned long)ptw_inject_interval_count);
            printf("  PTW output interval (walk_complete):\n");
            printf("    Avg output interval: %.2f ns\n",
                   ptw_output_interval_count > 0 ? ptw_output_interval_total_ns / ptw_output_interval_count : 0);
            printf("    Max output interval: %.2f ns\n", ptw_output_interval_max_ns);
            printf("    Min output interval: %.2f ns\n",
                   ptw_output_interval_min_ns == 999999999.0 ? 0 : ptw_output_interval_min_ns);
            printf("    Output count:        %lu\n", (unsigned long)ptw_output_interval_count);
            printf("  PTW avg exec latency:  %.2f ns\n",
                   ptw_total_exec_ns / ptw_total_completed);
            
            // [STAT] DDR访问次数分布直方图
            printf("  ---\n");
            printf("  PTW DDR reads distribution (ALL tasks):\n");
            printf("    %-12s %-8s %-8s\n", "DDR_reads", "count", "percent");
            for (auto& [ddr_cnt, cnt] : ptw_ddr_reads_distribution) {
                printf("    %-12u %-8u %.2f%%\n",
                       ddr_cnt, cnt, (double)cnt / ptw_total_completed * 100.0);
            }
            
            // [STAT] 主任务DDR reads分布
            printf("  ---\n");
            printf("  PTW DDR reads distribution (MAIN tasks, count=%lu):\n",
                   (unsigned long)ptw_main_task_count);
            if (ptw_main_task_count > 0) {
                printf("    Avg DDR reads:  %.2f\n", (double)ptw_main_total_ddr_reads / ptw_main_task_count);
                printf("    Max DDR reads:  %u\n", ptw_main_max_ddr_reads);
                printf("    Min DDR reads:  %u\n", ptw_main_min_ddr_reads == 999999 ? 0 : ptw_main_min_ddr_reads);
                // [STAT需求3] 主任务两阶段DDR访问分解 + 预取启动前DDR次数
                if (ptw_main_stage_count > 0) {
                    printf("    [两阶段分解] 第一阶段(iova->gpa,VS_WALK) avg DDR: %.3f\n",
                           (double)ptw_main_vs_ddr_sum / ptw_main_stage_count);
                    printf("    [两阶段分解] 第二阶段(gpa->spa,GS_*)     avg DDR: %.3f\n",
                           (double)ptw_main_gs_ddr_sum / ptw_main_stage_count);
                    printf("    [两阶段分解] AD_UPDATE                   avg DDR: %.3f\n",
                           (double)ptw_main_ad_ddr_sum / ptw_main_stage_count);
                    printf("    [两阶段分解] 合计(VS+GS+AD) avg DDR/主任务: %.3f (样本主任务=%lu)\n",
                           (double)(ptw_main_vs_ddr_sum + ptw_main_gs_ddr_sum + ptw_main_ad_ddr_sum) / ptw_main_stage_count,
                           (unsigned long)ptw_main_stage_count);
                }
                if (ptw_spawn_start_ddr_count > 0) {
                    printf("    [预取启动] 主任务访问DDR几次后启动预取 (spawn事件=%lu): 平均 %.3f\n",
                           (unsigned long)ptw_spawn_start_ddr_count,
                           (double)ptw_spawn_start_ddr_sum / ptw_spawn_start_ddr_count);
                    if (ptw_early_spawn_cnt > 0)
                        printf("      early-spawn(walker hit@L0): %.3f DDR后启动 (count=%lu)\n",
                               (double)ptw_early_spawn_ddr_sum / ptw_early_spawn_cnt, (unsigned long)ptw_early_spawn_cnt);
                    if (ptw_late_spawn_cnt > 0)
                        printf("      late-spawn(GS leaf完成后):  %.3f DDR后启动 (count=%lu)\n",
                               (double)ptw_late_spawn_ddr_sum / ptw_late_spawn_cnt, (unsigned long)ptw_late_spawn_cnt);
                }
                printf("    %-12s %-8s %-8s\n", "DDR_reads", "count", "percent");
                for (auto& [ddr_cnt, cnt] : ptw_main_ddr_reads_distribution) {
                    printf("    %-12u %-8u %.2f%%\n",
                           ddr_cnt, cnt, (double)cnt / ptw_main_task_count * 100.0);
                }
            }
            
            // [STAT] 预取任务DDR reads分布
            printf("  ---\n");
            printf("  PTW DDR reads distribution (PREFETCH tasks, count=%lu):\n",
                   (unsigned long)ptw_prefetch_task_count);
            if (ptw_prefetch_task_count > 0) {
                printf("    Avg DDR reads:  %.2f\n", (double)ptw_prefetch_total_ddr_reads / ptw_prefetch_task_count);
                printf("    Max DDR reads:  %u\n", ptw_prefetch_max_ddr_reads);
                printf("    Min DDR reads:  %u\n", ptw_prefetch_min_ddr_reads == 999999 ? 0 : ptw_prefetch_min_ddr_reads);
                printf("    %-12s %-8s %-8s\n", "DDR_reads", "count", "percent");
                for (auto& [ddr_cnt, cnt] : ptw_prefetch_ddr_reads_distribution) {
                    printf("    %-12u %-8u %.2f%%\n",
                           ddr_cnt, cnt, (double)cnt / ptw_prefetch_task_count * 100.0);
                }
            }
            
            // [STAT] 任务执行延时分布（分桶）
            printf("  ---\n");
            printf("  PTW task exec latency distribution:\n");
            // 自动分桶: 0~100ns, 100~500ns, 500~1000ns, 1000~5000ns, >5000ns
            uint32_t lat_buckets[5] = {0};
            const char* lat_labels[5] = {
                "0~100ns", "100~500ns", "500~1000ns", "1000~5000ns", ">5000ns"
            };
            for (auto lat : ptw_task_latency_values) {
                if (lat < 100) lat_buckets[0]++;
                else if (lat < 500) lat_buckets[1]++;
                else if (lat < 1000) lat_buckets[2]++;
                else if (lat < 5000) lat_buckets[3]++;
                else lat_buckets[4]++;
            }
            printf("    %-16s %-8s %-8s\n", "range", "count", "percent");
            for (int i = 0; i < 5; i++) {
                if (lat_buckets[i] > 0) {
                    printf("    %-16s %-8u %.2f%%\n",
                           lat_labels[i], lat_buckets[i],
                           (double)lat_buckets[i] / ptw_task_latency_values.size() * 100.0);
                }
            }
            
            // [STAT] PTW任务组执行时间统计 (1主+D预取为一组)
            if (ptw_group_exec_count > 0) {
                uint32_t avg_pf_per_group = (uint32_t)((ptw_prefetch_task_count + ptw_group_exec_count / 2) / ptw_group_exec_count); // 四舍五入
                printf("  ---\n");
                printf("  PTW Task Group Execution Statistics (1 main + %u prefetch = %u tasks/group):\n",
                       avg_pf_per_group, avg_pf_per_group + 1);
                printf("    Completed groups:    %lu\n", (unsigned long)ptw_group_exec_count);
                printf("    Avg exec time:       %.1f ns  (%.3f us)\n",
                       ptw_group_exec_total_ns / ptw_group_exec_count,
                       ptw_group_exec_total_ns / ptw_group_exec_count / 1000.0);
                printf("    Max exec time:       %.1f ns  (%.3f us)\n",
                       ptw_group_exec_max_ns, ptw_group_exec_max_ns / 1000.0);
                printf("    Min exec time:       %.1f ns  (%.3f us)\n",
                       ptw_group_exec_min_ns == 999999999.0 ? 0 : ptw_group_exec_min_ns,
                       ptw_group_exec_min_ns == 999999999.0 ? 0 : ptw_group_exec_min_ns / 1000.0);
                // 组执行时间分布直方图
                uint32_t grp_buckets[5] = {0};
                const char* grp_labels[5] = {
                    "0~500ns", "500~1000ns", "1000~2000ns", "2000~5000ns", ">5000ns"
                };
                for (auto v : ptw_group_exec_values) {
                    if (v < 500) grp_buckets[0]++;
                    else if (v < 1000) grp_buckets[1]++;
                    else if (v < 2000) grp_buckets[2]++;
                    else if (v < 5000) grp_buckets[3]++;
                    else grp_buckets[4]++;
                }
                printf("    Group exec time distribution:\n");
                printf("      %-16s %-8s %-8s\n", "range", "count", "percent");
                for (int i = 0; i < 5; i++) {
                    if (grp_buckets[i] > 0) {
                        printf("      %-16s %-8u %.2f%%\n",
                               grp_labels[i], grp_buckets[i],
                               (double)grp_buckets[i] / ptw_group_exec_values.size() * 100.0);
                    }
                }
            }
            
            // [STAT] PTW任务组DDR读次数统计 (1主+D预取为一组)
            if (ptw_group_exec_count > 0) {
                printf("  ---\n");
                printf("  PTW Task Group DDR Reads Statistics (per group, 1 main + D prefetch):\n");
                printf("    Avg DDR reads/group: %.2f\n",
                       (double)ptw_group_total_ddr_reads / ptw_group_exec_count);
                printf("    Max DDR reads/group: %u\n", ptw_group_max_ddr_reads);
                printf("    Min DDR reads/group: %u\n",
                       ptw_group_min_ddr_reads == 999999 ? 0 : ptw_group_min_ddr_reads);
                // 组DDR读次数分布直方图
                std::map<uint32_t, uint32_t> ddr_dist;
                for (auto v : ptw_group_ddr_values) {
                    ddr_dist[v]++;
                }
                printf("    Group DDR reads distribution:\n");
                printf("      %-12s %-8s %-8s\n", "ddr_reads", "count", "percent");
                for (auto& [ddr_cnt, cnt] : ddr_dist) {
                    printf("      %-12u %-8u %.2f%%\n",
                           ddr_cnt, cnt,
                           (double)cnt / ptw_group_ddr_values.size() * 100.0);
                }
            }
            
            // [STAT] 导出输出间隔数据到CSV文件, 用于Python绘制波动曲线
            {
                FILE* fp = fopen("ptw_output_intervals.csv", "w");
                if (fp) {
                    fprintf(fp, "index,interval_ns\n");
                    for (size_t i = 0; i < ptw_output_interval_values.size(); i++) {
                        fprintf(fp, "%zu,%.2f\n", i, ptw_output_interval_values[i]);
                    }
                    fclose(fp);
                    printf("  ---\n");
                    printf("  Output interval data exported to: ptw_output_intervals.csv (%zu samples)\n",
                           ptw_output_interval_values.size());
                }
            }
            // [STAT] 导出DDR reads分布到CSV文件, 用于Python绘制柱状图
            {
                FILE* fp = fopen("ptw_ddr_reads_distribution.csv", "w");
                if (fp) {
                    fprintf(fp, "task_type,ddr_reads,count,percent\n");
                    for (auto& [ddr_cnt, cnt] : ptw_ddr_reads_distribution) {
                        fprintf(fp, "ALL,%u,%u,%.4f\n", ddr_cnt, cnt,
                                (double)cnt / ptw_total_completed * 100.0);
                    }
                    for (auto& [ddr_cnt, cnt] : ptw_main_ddr_reads_distribution) {
                        fprintf(fp, "MAIN,%u,%u,%.4f\n", ddr_cnt, cnt,
                                ptw_main_task_count > 0 ? (double)cnt / ptw_main_task_count * 100.0 : 0);
                    }
                    for (auto& [ddr_cnt, cnt] : ptw_prefetch_ddr_reads_distribution) {
                        fprintf(fp, "PREFETCH,%u,%u,%.4f\n", ddr_cnt, cnt,
                                ptw_prefetch_task_count > 0 ? (double)cnt / ptw_prefetch_task_count * 100.0 : 0);
                    }
                    fclose(fp);
                    printf("  DDR reads distribution exported to: ptw_ddr_reads_distribution.csv (%zu bins)\n",
                           ptw_ddr_reads_distribution.size());
                }
            }
            // [STAT] 导出任务执行延时到CSV文件, 用于Python绘制分布图
            {
                FILE* fp = fopen("ptw_task_latency.csv", "w");
                if (fp) {
                    fprintf(fp, "index,latency_ns\n");
                    for (size_t i = 0; i < ptw_task_latency_values.size(); i++) {
                        fprintf(fp, "%zu,%.2f\n", i, ptw_task_latency_values[i]);
                    }
                    fclose(fp);
                    printf("  Task latency data exported to: ptw_task_latency.csv (%zu samples)\n",
                           ptw_task_latency_values.size());
                }
            }
            // [STAT] 预取分析报告
            printf("  ---\n");
            printf("  Prefetch Analysis Report:\n");
            printf("    Main tasks entering PTW:           %lu\n", (unsigned long)ptw_main_task_count);
            printf("    Late-spawn triggered:              %lu\n", (unsigned long)ptw_late_spawn_triggered);
            printf("    Late-spawn skipped (MSI):          %lu\n", (unsigned long)ptw_late_spawn_skipped_msi);
            printf("    Late-spawn skipped (bypass):       %lu\n", (unsigned long)ptw_late_spawn_skipped_bypass);
            printf("    Prefetch tasks spawned (total):    %lu\n", (unsigned long)ptw_prefetch_spawned_total);
            printf("    Prefetch tasks completed:          %lu\n", (unsigned long)ptw_prefetch_task_count);
            if (ptw_late_spawn_triggered > 0) {
                printf("    Avg prefetch per triggered main:   %.2f (target D=3)\n",
                       (double)ptw_prefetch_spawned_total / ptw_late_spawn_triggered);
            }
            if (ptw_main_task_count > 0) {
                printf("    Late-spawn trigger rate:           %.1f%%\n",
                       100.0 * ptw_late_spawn_triggered / ptw_main_task_count);
            }
            // [STAT] 导出任务组执行时间到CSV文件
            if (ptw_group_exec_count > 0) {
                FILE* fp = fopen("ptw_group_exec_latency.csv", "w");
                if (fp) {
                    fprintf(fp, "group_index,exec_time_ns\n");
                    for (size_t i = 0; i < ptw_group_exec_values.size(); i++) {
                        fprintf(fp, "%zu,%.2f\n", i, ptw_group_exec_values[i]);
                    }
                    fclose(fp);
                    printf("  Group exec latency data exported to: ptw_group_exec_latency.csv (%zu samples)\n",
                           ptw_group_exec_values.size());
                }
            }
        }
        if (iommu_total_completed > 0) {
            double avg_e2e_ns = iommu_total_e2e_latency_ns / iommu_total_completed;
            double avg_req_ns = sim_time_sec * 1e9 / iommu_total_completed;
            printf("  IO   avg e2e lat:     %.1f ns  (%.3f us)\n", avg_e2e_ns, avg_e2e_ns/1000.0);
            printf("  IO   max e2e lat:     %.1f ns  (%.3f us)\n", iommu_e2e_max_ns, iommu_e2e_max_ns/1000.0);
            printf("  IO   min e2e lat:     %.1f ns  (%.3f us)\n",
                   iommu_e2e_min_ns == 999999999.0 ? 0 : iommu_e2e_min_ns,
                   iommu_e2e_min_ns == 999999999.0 ? 0 : iommu_e2e_min_ns/1000.0);
            printf("  IO   avg req time:    %.1f ns  (%.3f us)\n", avg_req_ns, avg_req_ns/1000.0);
            // [STAT] IOMMU e2e延时分布
            if (!iommu_e2e_values.empty()) {
                uint32_t e2e_buckets[6] = {0};
                const char* e2e_labels[6] = {"0~500ns", "500~1000ns", "1000~2000ns", "2000~5000ns", "5000~10000ns", ">10000ns"};
                for (auto v : iommu_e2e_values) {
                    if (v < 500) e2e_buckets[0]++;
                    else if (v < 1000) e2e_buckets[1]++;
                    else if (v < 2000) e2e_buckets[2]++;
                    else if (v < 5000) e2e_buckets[3]++;
                    else if (v < 10000) e2e_buckets[4]++;
                    else e2e_buckets[5]++;
                }
                printf("  IO   e2e latency distribution:\n");
                printf("    %-16s %-8s %-8s\n", "range", "count", "percent");
                for (int i = 0; i < 6; i++) {
                    if (e2e_buckets[i] > 0) {
                        printf("    %-16s %-8u %.2f%%\n", e2e_labels[i], e2e_buckets[i],
                               (double)e2e_buckets[i] / iommu_e2e_values.size() * 100.0);
                    }
                }
            }
        }
        // 稳态IOPS（跳过warmup/drain，只取中间稳定段）
        if (steady_end_ns > steady_start_ns && steady_end_count > steady_start_count) {
            double steady_duration_ns = steady_end_ns - steady_start_ns;
            uint64_t steady_trans = steady_end_count - steady_start_count;
            double steady_iops = (double)steady_trans / steady_duration_ns * 1e3; // M trans/s
            // [MPS/MRRS] 理论峰值按 IO_PACKET_BYTES 换算(默认512B; 小包场景传128B -> 1000 M/s)
            double theory_iops = (double)AXI_SLAVE_0_BANDWIDTH_MBPS / 8.0 / IO_PACKET_BYTES; // Mbps / 8 / pkt_bytes = M/s
            printf("  ---\n");
            printf("  Steady IOPS:          %.2f M trans/s  (window: #%lu ~ #%lu, %.1f ns)\n",
                   steady_iops,
                   (unsigned long)steady_start_count, (unsigned long)steady_end_count,
                   steady_duration_ns);
            printf("  Theory peak:          %.2f M trans/s  (%.0f GB/s / %uB)\n",
                   theory_iops, AXI_SLAVE_0_BANDWIDTH_MBPS / 8000.0, IO_PACKET_BYTES);
            printf("  Efficiency:           %.1f%%\n", steady_iops / theory_iops * 100.0);

            // [STAT需求1] 稳态窗口[#start~#end]内的输入/输出端口平均间隔
            {
                double in_sum = 0.0; uint64_t in_cnt = 0;
                for (size_t i = 0; i < iommu_in_interval_values.size() && i < iommu_in_interval_end_ns.size(); i++) {
                    if (iommu_in_interval_end_ns[i] >= steady_start_ns && iommu_in_interval_end_ns[i] <= steady_end_ns) {
                        in_sum += iommu_in_interval_values[i]; in_cnt++;
                    }
                }
                double out_sum = 0.0; uint64_t out_cnt = 0;
                for (size_t i = 0; i < iommu_out_interval_values.size() && i < iommu_out_interval_end_ns.size(); i++) {
                    if (iommu_out_interval_end_ns[i] >= steady_start_ns && iommu_out_interval_end_ns[i] <= steady_end_ns) {
                        out_sum += iommu_out_interval_values[i]; out_cnt++;
                    }
                }
                printf("  ---\n");
                printf("  [需求1] 稳态窗口内端口注入平均间隔 (window [%.1f, %.1f] ns):\n",
                       steady_start_ns, steady_end_ns);
                printf("    输入端口平均间隔(稳态): %.3f ns  (samples=%lu)\n",
                       in_cnt > 0 ? in_sum / in_cnt : 0.0, (unsigned long)in_cnt);
                printf("    输出端口平均间隔(稳态): %.3f ns  (samples=%lu)\n",
                       out_cnt > 0 ? out_sum / out_cnt : 0.0, (unsigned long)out_cnt);
            }

            // PTW Steady IOPS
            if (ptw_steady_end_completed > ptw_steady_start_completed && steady_duration_ns > 0) {
                uint64_t ptw_steady_tasks = ptw_steady_end_completed - ptw_steady_start_completed;
                double ptw_steady_iops = (double)ptw_steady_tasks / steady_duration_ns * 1e3;
                printf("  ---\n");
                printf("  PTW Steady IOPS:      %.2f M tasks/s  (window: #%lu ~ #%lu, %lu tasks)\n",
                       ptw_steady_iops,
                       (unsigned long)steady_start_count, (unsigned long)steady_end_count,
                       (unsigned long)ptw_steady_tasks);
                printf("  PTW Theory peak:      %.2f M tasks/s  (4 concurrent / 742ns avg)\n",
                       4.0 / 742.0 * 1e3);
            }
        }
    }
    printf("====================================================\n\n");

    // Port Average Bandwidth
    printf("========== Port Average Bandwidth ==========\n");
    if (sim_time_sec > 0) {
        // peak BW (GB/s): BANDWIDTH_MBPS is in Mbps = 1e6 bit/s; GB/s = MBPS*1e6/8/1e9 = MBPS/8000
        const double peak_slave0_GBs  = AXI_SLAVE_0_BANDWIDTH_MBPS  / 8000.0;
        const double peak_master0_GBs = AXI_MASTER_0_BANDWIDTH_MBPS / 8000.0;
        const double peak_master1_GBs = AXI_MASTER_1_BANDWIDTH_MBPS / 8000.0;
        double slave0_bw_GBs  = (double)slave_0_total_bytes  / sim_time_sec / 1e9;
        double master0_bw_GBs = (double)master_0_total_bytes / sim_time_sec / 1e9;
        double master1_bw_GBs = (double)master_1_total_bytes / sim_time_sec / 1e9;
#if TEST_CFG_SPLIT_RW_PORTS
        // [读写端口分离] 读口/写口入口带宽分开统计(每端口峰值 64GB/s)
        const double peak_port_GBs = AXI_SLAVE_PORT_BW_MBPS / 8000.0;
        double slaver_bw_GBs = (double)slave_0_total_bytes  / sim_time_sec / 1e9;
        double slavew_bw_GBs = (double)slave_0w_total_bytes / sim_time_sec / 1e9;
        printf("  slave_0r (读口入口): %10lu B  avg %.3f GB/s  (peak %.0f GB/s, util %.2f%%)\n",
               (unsigned long)slave_0_total_bytes, slaver_bw_GBs, peak_port_GBs,
               slaver_bw_GBs / peak_port_GBs * 100.0);
        printf("  slave_0w (写口入口): %10lu B  avg %.3f GB/s  (peak %.0f GB/s, util %.2f%%)\n",
               (unsigned long)slave_0w_total_bytes, slavew_bw_GBs, peak_port_GBs,
               slavew_bw_GBs / peak_port_GBs * 100.0);
        printf("  slave_0  (入口合计): %10lu B  avg %.3f GB/s  (聚合peak %.0f GB/s, util %.2f%%)\n",
               (unsigned long)(slave_0_total_bytes + slave_0w_total_bytes),
               slaver_bw_GBs + slavew_bw_GBs, peak_slave0_GBs,
               (slaver_bw_GBs + slavew_bw_GBs) / peak_slave0_GBs * 100.0);
#else
        printf("  slave_0  (入口): %10lu B  avg %.3f GB/s  (peak %.0f GB/s, util %.2f%%)\n",
               (unsigned long)slave_0_total_bytes,  slave0_bw_GBs,  peak_slave0_GBs,
               slave0_bw_GBs  / peak_slave0_GBs  * 100.0);
#endif
        printf("  master_0 (出口): %10lu B  avg %.3f GB/s  (peak %.0f GB/s, util %.2f%%)\n",
               (unsigned long)master_0_total_bytes, master0_bw_GBs, peak_master0_GBs,
               master0_bw_GBs / peak_master0_GBs * 100.0);
        printf("  master_1 (DDR):  %10lu B  avg %.3f GB/s  (peak %.0f GB/s, util %.2f%%)\n",
               (unsigned long)master_1_total_bytes, master1_bw_GBs, peak_master1_GBs,
               master1_bw_GBs / peak_master1_GBs * 100.0);
    }
    printf("============================================\n\n");

    // ============================================================
    // [STAT] 端口带宽的无稀释口径(稳态窗口 / 注入期)
    //   上方 "Port Average Bandwidth" 分母 = 全仿真时长(含页表构建/warmup 与 drain),
    //   故其值明显低于稳态真实带宽。本段用字节快照/窗口长度重算:
    //     稳态窗口 = 完成数 10%~90% 区间; 注入期 = 本端口首次准入~末次准入。
    //   校验恒等式: 稳态带宽 = 稳态 IOPS × 窗口内平均包长(含控制包)。
    // ============================================================
    printf("========== Port Bandwidth (稳态窗口 / 注入期口径) ==========\n");
#if TEST_CFG_SPLIT_RW_PORTS
    {
        const double peak_port_GBs = AXI_SLAVE_PORT_BW_MBPS / 8000.0;
        // (1) 稳态窗口(完成数 10%~90%)
        const double sw_ns = steady_end_ns - steady_start_ns;
        if (sw_ns > 0.0 && steady_end_slave_r_bytes >= steady_start_slave_r_bytes) {
            const double rb = (double)(steady_end_slave_r_bytes - steady_start_slave_r_bytes) / sw_ns;
            const double wb = (double)(steady_end_slave_w_bytes - steady_start_slave_w_bytes) / sw_ns;
            const double m0 = (double)(steady_end_master0_bytes - steady_start_master0_bytes) / sw_ns;
            printf("  [稳态窗口 %.1f ns: %.1f~%.1f]\n", sw_ns, steady_start_ns, steady_end_ns);
            printf("    读口 slave_0r: %10lu B  %.3f GB/s (util %.2f%% of %.0f)\n",
                   (unsigned long)(steady_end_slave_r_bytes - steady_start_slave_r_bytes),
                   rb, rb / peak_port_GBs * 100.0, peak_port_GBs);
            printf("    写口 slave_0w: %10lu B  %.3f GB/s (util %.2f%% of %.0f)\n",
                   (unsigned long)(steady_end_slave_w_bytes - steady_start_slave_w_bytes),
                   wb, wb / peak_port_GBs * 100.0, peak_port_GBs);
            printf("    入口合计:      %.3f GB/s (聚合peak %.0f GB/s, util %.2f%%)\n",
                   rb + wb, peak_port_GBs * 2.0, (rb + wb) / (peak_port_GBs * 2.0) * 100.0);
            printf("    出口 master_0: %.3f GB/s\n", m0);
        } else {
            printf("  [稳态窗口] 未开启或窗口无效(需 RP 设置 steady_start_count/steady_end_count)\n");
        }
        // (2) 注入期(每端口自身的首次~末次准入)
        const double r_span = iommu_in_r_last_ns - port_first_admit_r_ns;
        const double w_span = iommu_in_w_last_ns - port_first_admit_w_ns;
        if (port_first_admit_r_ns > 0.0 && r_span > 0.0) {
            const double rb = (double)(slave_0_total_bytes - bw_snap_win_r_bytes) / r_span;
            printf("  [读口注入期 %.1f ns] %lu B -> %.3f GB/s (util %.2f%% of %.0f)\n",
                   r_span, (unsigned long)(slave_0_total_bytes - bw_snap_win_r_bytes),
                   rb, rb / peak_port_GBs * 100.0, peak_port_GBs);
        }
        if (port_first_admit_w_ns > 0.0 && w_span > 0.0) {
            const double wb = (double)(slave_0w_total_bytes - bw_snap_win_w_bytes) / w_span;
            printf("  [写口注入期 %.1f ns] %lu B -> %.3f GB/s (util %.2f%% of %.0f)\n",
                   w_span, (unsigned long)(slave_0w_total_bytes - bw_snap_win_w_bytes),
                   wb, wb / peak_port_GBs * 100.0, peak_port_GBs);
        }
        const double inj_end = (iommu_in_w_last_ns > iommu_in_r_last_ns)
                                   ? iommu_in_w_last_ns : iommu_in_r_last_ns;
        const double inj_span = inj_end - conc_win_start_ns;
        if (inj_span > 0.0) {
            const double ib = (double)((slave_0_total_bytes + slave_0w_total_bytes) -
                                       (bw_snap_win_r_bytes + bw_snap_win_w_bytes)) / inj_span;
            printf("  [入口合计注入期 %.1f ns] -> %.3f GB/s (聚合peak %.0f GB/s, util %.2f%%)\n",
                   inj_span, ib, peak_port_GBs * 2.0, ib / (peak_port_GBs * 2.0) * 100.0);
        }
        printf("  注: 全程平均口径的分母含 %.0f ns warmup(页表构建+哨兵/预热epoch)与末尾 drain\n",
               conc_win_start_ns > 0.0 ? conc_win_start_ns : 0.0);
    }
#else
    printf("  (非读写端口分离场景: 入口单口, 全程平均口径见上段)\n");
#endif
    printf("==========================================================\n\n");

    // IOMMU Input/Output Port Interval Statistics
    printf("========== IOMMU Port Task Interval Statistics ==========\n");
    printf("  Input Port (slave_0 合计, inbound_fifo_r/w):\n");
    if (iommu_in_interval_count > 0) {
        printf("    Avg input interval:  %.2f ns\n", iommu_in_interval_total_ns / iommu_in_interval_count);
        printf("    Max input interval:  %.2f ns\n", iommu_in_interval_max_ns);
        printf("    Min input interval:  %.2f ns\n", iommu_in_interval_min_ns);
        printf("    Sample count:        %lu\n", (unsigned long)iommu_in_interval_count);
    } else {
        printf("    (no samples)\n");
    }
    // [读写端口分离] 读口/写口各自的输入间隔
    printf("  Input Read Port (slave_0r):\n");
    if (iommu_in_r_interval_count > 0) {
        printf("    Avg input interval:  %.2f ns\n", iommu_in_r_interval_total_ns / iommu_in_r_interval_count);
        printf("    Max input interval:  %.2f ns\n", iommu_in_r_interval_max_ns);
        printf("    Min input interval:  %.2f ns\n",
               iommu_in_r_interval_min_ns == 999999999.0 ? 0 : iommu_in_r_interval_min_ns);
        printf("    Sample count:        %lu\n", (unsigned long)iommu_in_r_interval_count);
    } else {
        printf("    (no samples)\n");
    }
    printf("  Input Write Port (slave_0w):\n");
    if (iommu_in_w_interval_count > 0) {
        printf("    Avg input interval:  %.2f ns\n", iommu_in_w_interval_total_ns / iommu_in_w_interval_count);
        printf("    Max input interval:  %.2f ns\n", iommu_in_w_interval_max_ns);
        printf("    Min input interval:  %.2f ns\n",
               iommu_in_w_interval_min_ns == 999999999.0 ? 0 : iommu_in_w_interval_min_ns);
        printf("    Sample count:        %lu\n", (unsigned long)iommu_in_w_interval_count);
    } else {
        printf("    (no samples)\n");
    }
    printf("  Output Port (master_0, response):\n");
    if (iommu_out_interval_count > 0) {
        printf("    Avg output interval: %.2f ns\n", iommu_out_interval_total_ns / iommu_out_interval_count);
        printf("    Max output interval: %.2f ns\n", iommu_out_interval_max_ns);
        printf("    Min output interval: %.2f ns\n", iommu_out_interval_min_ns);
        printf("    Sample count:        %lu\n", (unsigned long)iommu_out_interval_count);
    } else {
        printf("    (no samples)\n");
    }
    printf("=========================================================\n\n");

    // PT Cache 输出间隔统计
    printf("========== PT Cache Output Interval Statistics ==========\n");
    printf("  Total PT Cache outputs:  %lu (HIT + MISS)\n", (unsigned long)pt_cache_out_total_count);
    if (pt_cache_out_interval_count > 0) {
        printf("  Avg output interval:   %.2f ns\n", pt_cache_out_interval_total_ns / pt_cache_out_interval_count);
        printf("  Max output interval:   %.2f ns\n", pt_cache_out_interval_max_ns);
        printf("  Min output interval:   %.2f ns\n", pt_cache_out_interval_min_ns);
        printf("  Sample count:          %lu\n", (unsigned long)pt_cache_out_interval_count);
    } else {
        printf("  (no samples)\n");
    }
    printf("==========================================================\n\n");

    // SQ/CQ/MSI PT Cache 命中统计
    printf("========== SQ/CQ/MSI PT Cache Hit Statistics ==========\n");
    printf("  Control packets (SQ+CQ+MSI) total:  %lu\n", (unsigned long)ctrl_total_count);
    printf("    SQ count:   %lu\n", (unsigned long)ctrl_sq_count);
    printf("    CQ count:   %lu\n", (unsigned long)ctrl_cq_count);
    printf("    MSI count:  %lu\n", (unsigned long)ctrl_msi_count);
    printf("  PT Cache HIT (bypass dedup/PTW):    %lu\n", (unsigned long)ctrl_pt_cache_hit_count);
    printf("  PT Cache MISS (enter dedup/PTW):    %lu\n", (unsigned long)ctrl_pt_cache_miss_count);
    if (ctrl_total_count > 0) {
        printf("  PT Cache hit rate for ctrl:       %.2f%%\n", 
               100.0 * ctrl_pt_cache_hit_count / ctrl_total_count);
    }
    printf("=========================================================\n\n");

    // PTW 任务数占入口任务数比例
    printf("========== PTW Task Ratio Statistics ==========\n");
    printf("  IOMMU input tasks (total):    %lu\n", (unsigned long)(iommu_total_completed + peak_iommu_global_outstanding));
    printf("  PTW completed tasks:          %lu\n", (unsigned long)ptw_total_completed);
    printf("    Main tasks:                 %lu\n", (unsigned long)ptw_main_task_count);
    printf("    Prefetch tasks:             %lu\n", (unsigned long)ptw_prefetch_task_count);
    if (iommu_total_completed > 0) {
        printf("  PTW/IOMMU ratio:              %.2f%%\n", 
               100.0 * ptw_total_completed / iommu_total_completed);
    }
    printf("================================================\n\n");

    // [STAT-NEW] PTW 并发任务数与主/预取执行延时统计
    //   需求: N=8等场景补充 PTW 模块的实际平均并发/最大并发/主+预取执行延时
    printf("========== PTW Concurrency & Execution Latency ==========\n");
    {
        // --- (1) PTW 并发任务数(主任务+预取任务合并计入 outstanding) ---
        // 时间加权平均: 累计area / 采样窗口; 峰值直接取peak_ptw_outstanding
        double window_ns = 0.0;
        double area_ns = ptw_concurrency_area_ns;
        if (ptw_concurrency_first_sample_ns >= 0.0) {
            // 收尾: 加上从上次变化到当前仿真时刻的面积
            const double now_ns = sc_time_stamp().to_seconds() * 1e9;
            const double tail = now_ns - ptw_concurrency_last_change_ns;
            if (tail > 0.0) {
                area_ns += (double)ptw_outstanding_task_count * tail;
            }
            window_ns = now_ns - ptw_concurrency_first_sample_ns;
        }
        const double avg_conc = (window_ns > 0.0) ? (area_ns / window_ns) : 0.0;
        printf("  PTW concurrent tasks (main + prefetch, outstanding计数单位=单任务):\n");
        printf("    Avg concurrency (time-weighted): %.3f tasks\n", avg_conc);
        printf("    Max concurrency (peak):          %d tasks  (limit=%d)\n",
               peak_ptw_outstanding, (int)PTW_MAX_OUTSTANDING_TASKS);
        printf("    Sampling window:                 %.1f ns (%.3f us)\n",
               window_ns, window_ns / 1000.0);
        printf("    Utilization (avg/limit):         %.2f%%\n",
               PTW_MAX_OUTSTANDING_TASKS > 0
                   ? 100.0 * avg_conc / (double)PTW_MAX_OUTSTANDING_TASKS : 0.0);
        printf("    Saturation (peak/limit):         %.2f%%\n",
               PTW_MAX_OUTSTANDING_TASKS > 0
                   ? 100.0 * (double)peak_ptw_outstanding / (double)PTW_MAX_OUTSTANDING_TASKS : 0.0);

        // --- (2) PTW 执行延时: 主任务 / 预取任务 / 合并 ---
        printf("  ---\n");
        printf("  PTW execution latency (task enter PTW -> walk complete):\n");
        // 合并(主+预取)
        if (ptw_total_completed > 0) {
            printf("    [ALL]      count=%-8lu avg=%8.2f ns  max=%8.2f ns  min=%8.2f ns\n",
                   (unsigned long)ptw_total_completed,
                   ptw_total_exec_ns / ptw_total_completed,
                   ptw_max_task_latency_ns,
                   ptw_min_task_latency_ns == 999999999.0 ? 0.0 : ptw_min_task_latency_ns);
        }
        // 主任务
        if (ptw_main_task_count > 0) {
            printf("    [MAIN]     count=%-8lu avg=%8.2f ns  max=%8.2f ns  min=%8.2f ns\n",
                   (unsigned long)ptw_main_task_count,
                   ptw_main_total_exec_ns / ptw_main_task_count,
                   ptw_main_max_task_latency_ns,
                   ptw_main_min_task_latency_ns == 999999999.0 ? 0.0 : ptw_main_min_task_latency_ns);
        } else {
            printf("    [MAIN]     count=0        (no main task recorded)\n");
        }
        // 预取任务
        if (ptw_prefetch_task_count > 0) {
            printf("    [PREFETCH] count=%-8lu avg=%8.2f ns  max=%8.2f ns  min=%8.2f ns\n",
                   (unsigned long)ptw_prefetch_task_count,
                   ptw_prefetch_total_exec_ns / ptw_prefetch_task_count,
                   ptw_prefetch_max_task_latency_ns,
                   ptw_prefetch_min_task_latency_ns == 999999999.0 ? 0.0 : ptw_prefetch_min_task_latency_ns);
        } else {
            printf("    [PREFETCH] count=0        (no prefetch task recorded)\n");
        }
        // 主+预取合并再核对一次(与[ALL]口径一致, 用于交叉校验)
        const uint64_t mp_count = ptw_main_task_count + ptw_prefetch_task_count;
        if (mp_count > 0) {
            const double mp_avg = (ptw_main_total_exec_ns + ptw_prefetch_total_exec_ns) / mp_count;
            printf("    [MAIN+PF]  count=%-8lu avg=%8.2f ns  (合并校验口径)\n",
                   (unsigned long)mp_count, mp_avg);
        }

        // --- (3) 主/预取延时分布直方图(与既有ALL口径分桶一致) ---
        auto dump_bucket = [](const char* tag, const std::vector<double>& vals) {
            if (vals.empty()) return;
            uint32_t b[5] = {0};
            for (auto v : vals) {
                if (v < 100) b[0]++;
                else if (v < 500) b[1]++;
                else if (v < 1000) b[2]++;
                else if (v < 5000) b[3]++;
                else b[4]++;
            }
            const char* lbl[5] = {"0~100ns", "100~500ns", "500~1000ns", "1000~5000ns", ">5000ns"};
            printf("    %s latency distribution (n=%zu):\n", tag, vals.size());
            for (int i = 0; i < 5; i++) {
                if (b[i] > 0) {
                    printf("      %-12s %-8u %.2f%%\n", lbl[i], b[i],
                           100.0 * (double)b[i] / (double)vals.size());
                }
            }
        };
        printf("  ---\n");
        dump_bucket("[MAIN]    ", ptw_main_task_latency_values);
        dump_bucket("[PREFETCH]", ptw_prefetch_task_latency_values);

        // --- (4) CSV导出: 便于N=8等场景做曲线/对比 ---
        {
            FILE* fp = fopen("ptw_main_task_latency.csv", "w");
            if (fp) {
                fprintf(fp, "index,latency_ns\n");
                for (size_t i = 0; i < ptw_main_task_latency_values.size(); i++) {
                    fprintf(fp, "%zu,%.2f\n", i, ptw_main_task_latency_values[i]);
                }
                fclose(fp);
                printf("  Main task latency exported to: ptw_main_task_latency.csv (%zu samples)\n",
                       ptw_main_task_latency_values.size());
            }
        }
        {
            FILE* fp = fopen("ptw_prefetch_task_latency.csv", "w");
            if (fp) {
                fprintf(fp, "index,latency_ns\n");
                for (size_t i = 0; i < ptw_prefetch_task_latency_values.size(); i++) {
                    fprintf(fp, "%zu,%.2f\n", i, ptw_prefetch_task_latency_values[i]);
                }
                fclose(fp);
                printf("  Prefetch task latency exported to: ptw_prefetch_task_latency.csv (%zu samples)\n",
                       ptw_prefetch_task_latency_values.size());
            }
        }
    }
    printf("=========================================================\n\n");

    // Outstanding Peak Statistics
    printf("========== Outstanding Peak Statistics ==========\n");
    printf("  %-30s peak=%4d / max=%4d\n", "IOMMU Global:",
           peak_iommu_global_outstanding,  (int)IOMMU_GLOBAL_MAX_OUTSTANDING);
    printf("  %-30s peak=%4d / max=%4d\n", "IOMMU Read (场景13):",
           peak_iommu_read_outstanding,    (int)IOMMU_READ_MAX_OUTSTANDING);
    printf("  %-30s peak=%4d / max=%4d\n", "IOMMU Write (场景13):",
           peak_iommu_write_outstanding,   (int)IOMMU_WRITE_MAX_OUTSTANDING);
    printf("  %-30s peak=%4d / max=%4d\n", "PTW:",
           peak_ptw_outstanding,            (int)PTW_MAX_OUTSTANDING_TASKS);
    printf("  %-30s peak=%4d / max=%4d\n", "xDTW DC walk:",
           peak_xdtw_dc_outstanding,        (int)XDTW_MAX_DC_OUTSTANDING_TASKS);
    printf("  %-30s peak=%4d / max=%4d\n", "xDTW PC walk:",
           peak_xdtw_pc_outstanding,        (int)XDTW_MAX_PC_OUTSTANDING_TASKS);
    printf("  %-30s peak=%4d / max=%4d\n", "Collector DC walk:",
           peak_collector_dc_walk_outstanding, (int)COLLECTOR_MAX_DC_WALK_OUTSTANDING);
    printf("  %-30s peak=%4d / max=%4d\n", "Collector PC walk:",
           peak_collector_pc_walk_outstanding, (int)COLLECTOR_MAX_PC_WALK_OUTSTANDING);
    printf("  %-30s peak=%4d / max=%4d\n", "DDR (master_1):",
           peak_axi_master_1_outstanding,   (int)AXI_MASTER_1_TO_CMN_RND_MAX_OUTSTANDING);
    // [STAT] DDR端口(axi_master_1, 访问页表/目录表)读写分离并发峰值
    printf("  %-30s peak=%4d  (读请求总数=%lu)\n", "DDR master_1 READ(页表/目录):",
           peak_axi_master_1_read_outstanding, (unsigned long)axi_master_1_read_req_total);
    printf("  %-30s peak=%4d  (写请求总数=%lu)\n", "DDR master_1 WRITE(AD/MRIF):",
           peak_axi_master_1_write_outstanding, (unsigned long)axi_master_1_write_req_total);
    printf("  %-30s peak=%4d / max=%4d\n", "Output (master_0):",
           peak_axi_master_0_outstanding,   (int)AXI_MASTER_0_TO_PCIE_NOC_MAX_OUTSTANDING);
    printf("=================================================\n\n");

    // ============================================================
    // [STAT] 端口准入门控统计 (读写端口分离)
    //   门控已从 parser 前移到端口: 读口只在 read/global 满时阻塞, 写口只在
    //   write/global 满时阻塞, 两方向互不交叉。此处输出各口因自身方向满/全局满
    //   而阻塞的次数与时长, 以及各口准入任务总数; 预期无"另一方向满导致自身阻塞"。
    // ============================================================
    printf("========== 端口准入门控统计 (读写端口分离) ==========\n");
    printf("  门控上限: Global=%d  Read=%d  Write=%d\n",
           (int)IOMMU_GLOBAL_MAX_OUTSTANDING,
           (int)IOMMU_READ_MAX_OUTSTANDING,
           (int)IOMMU_WRITE_MAX_OUTSTANDING);
    printf("  读口(R-PORT) 准入任务总数:  %lu\n", (unsigned long)port_admit_read_total);
    printf("  写口(W-PORT) 准入任务总数:  %lu\n", (unsigned long)port_admit_write_total);
    printf("  ---\n");
    printf("  读口因(读满/全局满)阻塞: %10lu 次, 总时长 %.1f ns (%.3f us), 单次最大 %.1f ns\n",
           (unsigned long)port_blk_read_cnt, port_blk_read_ns,
           port_blk_read_ns / 1000.0, port_blk_read_max_ns);
    printf("  写口因(写满/全局满)阻塞: %10lu 次, 总时长 %.1f ns (%.3f us), 单次最大 %.1f ns\n",
           (unsigned long)port_blk_write_cnt, port_blk_write_ns,
           port_blk_write_ns / 1000.0, port_blk_write_max_ns);
    printf("  读口阻塞占准入比: %.2f%%   写口阻塞占准入比: %.2f%%\n",
           port_admit_read_total  ? 100.0 * (double)port_blk_read_cnt  / (double)port_admit_read_total  : 0.0,
           port_admit_write_total ? 100.0 * (double)port_blk_write_cnt / (double)port_admit_write_total : 0.0);
    printf("  峰值并发(端口准入点): Read=%d/%d  Write=%d/%d  Global=%d/%d\n",
           peak_iommu_read_outstanding,   (int)IOMMU_READ_MAX_OUTSTANDING,
           peak_iommu_write_outstanding,  (int)IOMMU_WRITE_MAX_OUTSTANDING,
           peak_iommu_global_outstanding, (int)IOMMU_GLOBAL_MAX_OUTSTANDING);
    printf("=========================================================\n\n");

    // ============================================================
    // [STAT] 注入窗口内的读写并发顶满程度 (读写端口分离)
    //   窗口 = RP 开始注入 -> RP 把全部请求送达端口。
    //   窗口内 RP 侧始终有请求可发(供给充足), 因此这里的 avg 与
    //   "顶满时长占比"才是判断"读写是否各自顶满 320"的正确口径。
    //   配套输出端口节拍占用率与方向失配次数, 用于归因:
    //     顶满占比高 + 节拍占用低  -> 该方向被 320 额度门控(已达设计上限)
    //     顶满占比低 + 节拍占用高 -> 该方向受端口逐包节拍(线速)限制
    // ============================================================
    printf("========== 注入窗口内读写并发顶满程度 (RP 供给充足口径) ==========\n");
    {
        const double win = (conc_win_end_ns > conc_win_start_ns)
                               ? (conc_win_end_ns - conc_win_start_ns) : 0.0;
        if (win > 0.0) {
            const double avg_r = conc_win_area_read_ns   / win;
            const double avg_w = conc_win_area_write_ns  / win;
            const double avg_g = conc_win_area_global_ns / win;
            printf("  注入窗口: %.1f ns (%.3f us)  [%.1f ~ %.1f ns], 采样 %lu 次\n",
                   win, win / 1000.0, conc_win_start_ns, conc_win_end_ns,
                   (unsigned long)conc_win_sample_cnt);
            printf("  %-22s %10s %8s %9s %8s %12s\n",
                   "项目", "avg(时间加权)", "上限", "avg占比", "peak", "顶满时长占比");
            printf("  %-22s %10.2f %8d %8.2f%% %8d %11.2f%%\n", "Read  并发",
                   avg_r, (int)IOMMU_READ_MAX_OUTSTANDING,
                   avg_r / (double)IOMMU_READ_MAX_OUTSTANDING * 100.0,
                   peak_iommu_read_outstanding,
                   conc_win_full_read_ns / win * 100.0);
            printf("  %-22s %10.2f %8d %8.2f%% %8d %11.2f%%\n", "Write 并发",
                   avg_w, (int)IOMMU_WRITE_MAX_OUTSTANDING,
                   avg_w / (double)IOMMU_WRITE_MAX_OUTSTANDING * 100.0,
                   peak_iommu_write_outstanding,
                   conc_win_full_write_ns / win * 100.0);
            printf("  %-22s %10.2f %8d %8.2f%% %8d %11.2f%%\n", "Global 并发",
                   avg_g, (int)IOMMU_GLOBAL_MAX_OUTSTANDING,
                   avg_g / (double)IOMMU_GLOBAL_MAX_OUTSTANDING * 100.0,
                   peak_iommu_global_outstanding,
                   conc_win_full_global_ns / win * 100.0);
            printf("  ---\n");
            // 端口占用三分法(节拍/额度门控/空闲), 折算到"该端口自身注入跨度":
            //   节拍+门控≈100% => 该端口已跑满(约束落在逐包节拍或 320 额度上)
            //   存在明显空闲   => RP 供给侧未跟上(请求不足)
            const double r_span = (iommu_in_r_last_ns > port_first_admit_r_ns && port_first_admit_r_ns > 0.0)
                                      ? (iommu_in_r_last_ns - port_first_admit_r_ns) : 0.0;
            const double w_span = (iommu_in_w_last_ns > port_first_admit_w_ns && port_first_admit_w_ns > 0.0)
                                      ? (iommu_in_w_last_ns - port_first_admit_w_ns) : 0.0;
            const double r_beat = r_span > 0.0 ? port_beat_read_ns  / r_span * 100.0 : 0.0;
            const double r_blk  = r_span > 0.0 ? port_blk_read_ns   / r_span * 100.0 : 0.0;
            const double r_idle = r_span > 0.0 ? (r_span - port_beat_read_ns - port_blk_read_ns) / r_span * 100.0 : 0.0;
            const double w_beat = w_span > 0.0 ? port_beat_write_ns / w_span * 100.0 : 0.0;
            const double w_blk  = w_span > 0.0 ? port_blk_write_ns  / w_span * 100.0 : 0.0;
            const double w_idle = w_span > 0.0 ? (w_span - port_beat_write_ns - port_blk_write_ns) / w_span * 100.0 : 0.0;
            printf("  [端口占用三分法: 节拍(逐包带宽)/门控(等额度)/空闲, 占各自注入跨度]\n");
            printf("    R口 跨度 %.1f ns [%.1f~%.1f]: 节拍 %.2f%% + 门控 %.2f%% + 空闲 %.2f%%\n",
                   r_span, port_first_admit_r_ns, iommu_in_r_last_ns, r_beat, r_blk, r_idle);
            printf("    W口 跨度 %.1f ns [%.1f~%.1f]: 节拍 %.2f%% + 门控 %.2f%% + 空闲 %.2f%%\n",
                   w_span, port_first_admit_w_ns, iommu_in_w_last_ns, w_beat, w_blk, w_idle);
            printf("    注: 空闲≈0 即 RP 供给充足; 门控占比高 => 320 额度为主约束; 节拍占比高 => 端口线速为主约束\n");
            // 顶满程度分级判定(以"顶满时长占比"与主约束类型联合判定)
            {
                auto verdict = [](double full_pct, double blk_pct, double beat_pct,
                                  double idle_pct) -> std::string {
                    std::string s;
                    if      (full_pct >= 90.0) s = "顶满320(额度全程门控)";
                    else if (full_pct >= 50.0) s = "基本顶满320";
                    else if (full_pct >= 10.0) s = "间歇顶满320";
                    else                       s = "未顶满320";
                    if      (idle_pct > 5.0)      s += ", 主约束=RP供给不足(端口有空闲)";
                    else if (beat_pct >= blk_pct) s += ", 主约束=端口逐包节拍(线速)";
                    else                          s += ", 主约束=320额度门控";
                    return s;
                };
                const double full_r = conc_win_full_read_ns  / win * 100.0;
                const double full_w = conc_win_full_write_ns / win * 100.0;
                printf("  判定 Read  : %s   (顶满时长 %.2f%%)\n",
                       verdict(full_r, r_blk, r_beat, r_idle).c_str(), full_r);
                printf("  判定 Write : %s   (顶满时长 %.2f%%)\n",
                       verdict(full_w, w_blk, w_beat, w_idle).c_str(), full_w);
                printf("  判定 Global: 顶满时长 %.2f%% (Global<=%d)\n",
                       conc_win_full_global_ns / win * 100.0, (int)IOMMU_GLOBAL_MAX_OUTSTANDING);
            }
            // 双口同时在注入的子窗口 = [两口都已开始, 较早结束的那一口最后一次准入]
            //   全注入窗口含"某一口已注完、只剩另一口"的尾巴, 会拖低先结束方的平均值,
            //   故该子窗口才是"读写是否各自顶满 320"的最公平口径。
            {
                const double s0 = (port_first_admit_r_ns > port_first_admit_w_ns)
                                      ? port_first_admit_r_ns : port_first_admit_w_ns;
                const double s1 = (iommu_in_r_last_ns < iommu_in_w_last_ns)
                                      ? iommu_in_r_last_ns : iommu_in_w_last_ns;
                if (s1 > s0 && !conc_win_hist.empty()) {
                    double ar, aw, ag, fr, fw, fg;
                    conc_window_slice(s0, s1, ar, aw, ag, fr, fw, fg);
                    printf("  [双口同时在注入子窗口 %.1f ns: %.1f~%.1f]\n",
                           s1 - s0, s0, s1);
                    printf("    %-22s %10.2f %8d %9.2f%% %12.2f%%\n",
                           "Read  并发", ar, (int)IOMMU_READ_MAX_OUTSTANDING,
                           ar / (double)IOMMU_READ_MAX_OUTSTANDING * 100.0, fr);
                    printf("    %-22s %10.2f %8d %9.2f%% %12.2f%%\n",
                           "Write 并发", aw, (int)IOMMU_WRITE_MAX_OUTSTANDING,
                           aw / (double)IOMMU_WRITE_MAX_OUTSTANDING * 100.0, fw);
                    printf("    %-22s %10.2f %8d %9.2f%% %12.2f%%\n",
                           "Global 并发", ag, (int)IOMMU_GLOBAL_MAX_OUTSTANDING,
                           ag / (double)IOMMU_GLOBAL_MAX_OUTSTANDING * 100.0, fg);
                    printf("    (列依次: 时间加权avg / 上限 / avg占比 / 顶满时长占比)\n");
                }
            }
            printf("  读口准入 %lu 个任务, 写口准入 %lu 个任务\n",
                   (unsigned long)port_admit_read_total, (unsigned long)port_admit_write_total);
            printf("  端口方向校验失配次数(必须=0): %lu\n",
                   (unsigned long)port_dir_mismatch_cnt);
        } else {
            printf("  (窗口未闭合或未打开: 只有使用新 RP 注入流程的场景才有此统计)\n");
            printf("  读口节拍累计占用 %.1f ns, 写口节拍累计占用 %.1f ns\n",
                   port_beat_read_ns, port_beat_write_ns);
            printf("  端口方向校验失配次数(必须=0): %lu\n",
                   (unsigned long)port_dir_mismatch_cnt);
        }
    }
    printf("====================================================================\n\n");

    // ============================================================
    // [STAT] Parser 门控快照统计
    //   验证目标: "读满或写满 -> 后续任务(含另一方向)全部进不来" 是否在稳态持续成立。
    //   采样口径: parser_thread 每弹出一个 task、方向门控 wait 之前的瞬时并发。
    // ============================================================
    printf("========== Parser 门控快照统计 (验证读满/写满队头阻塞) ==========\n");
    if (parser_recv_total > 0) {
        const double N = (double)parser_recv_total;
        printf("  门控上限: Global=%d  Read=%d  Write=%d\n",
               (int)IOMMU_GLOBAL_MAX_OUTSTANDING,
               (int)IOMMU_READ_MAX_OUTSTANDING,
               (int)IOMMU_WRITE_MAX_OUTSTANDING);
        printf("  ---\n");
        printf("  收到请求总数(弹出):       %lu  (Read=%lu, Write=%lu)\n",
               (unsigned long)parser_recv_total,
               (unsigned long)parser_recv_read,
               (unsigned long)parser_recv_write);
        printf("  ---\n");
        printf("  [快照时额度已满的次数与占比]\n");
        printf("    Read  满(%d):        %10lu  (%6.2f%%)\n",
               (int)IOMMU_READ_MAX_OUTSTANDING,
               (unsigned long)parser_snap_read_full,
               parser_snap_read_full / N * 100.0);
        printf("    Write 满(%d):        %10lu  (%6.2f%%)\n",
               (int)IOMMU_WRITE_MAX_OUTSTANDING,
               (unsigned long)parser_snap_write_full,
               parser_snap_write_full / N * 100.0);
        printf("    读满 或 写满:           %10lu  (%6.2f%%)   <-- 用户假设的判据\n",
               (unsigned long)parser_snap_either_full,
               parser_snap_either_full / N * 100.0);
        printf("    读写 同时 满:           %10lu  (%6.2f%%)\n",
               (unsigned long)parser_snap_both_full,
               parser_snap_both_full / N * 100.0);
        printf("    Global满(%d):        %10lu  (%6.2f%%)\n",
               (int)IOMMU_GLOBAL_MAX_OUTSTANDING,
               (unsigned long)parser_snap_global_full,
               parser_snap_global_full / N * 100.0);
        printf("  ---\n");
        printf("    自身方向已满(必阻塞):   %10lu  (%6.2f%%)\n",
               (unsigned long)parser_snap_selfdir_full,
               parser_snap_selfdir_full / N * 100.0);
        printf("    另一方向满而自身未满:   %10lu  (%6.2f%%)   <-- 额度不对称浪费\n",
               (unsigned long)parser_snap_otherdir_full,
               parser_snap_otherdir_full / N * 100.0);
        printf("  ---\n");
        printf("  [实际发生阻塞的次数与时长]\n");
        printf("    因 Read  满而 wait:     %10lu 次\n",
               (unsigned long)parser_blk_by_read);
        printf("    因 Write 满而 wait:     %10lu 次\n",
               (unsigned long)parser_blk_by_write);
        printf("    因 Global满而 wait:     %10lu 次\n",
               (unsigned long)parser_blk_by_global);
        printf("    方向门控阻塞总时长:     %.1f ns (%.3f us)\n",
               parser_blk_dir_ns, parser_blk_dir_ns / 1000.0);
        printf("    方向门控单次最大阻塞:   %.1f ns\n", parser_blk_dir_max_ns);
        printf("    全局门控阻塞总时长:     %.1f ns (%.3f us)\n",
               parser_blk_global_ns, parser_blk_global_ns / 1000.0);
        const uint64_t blk_cnt = parser_blk_by_read + parser_blk_by_write;
        printf("    方向阻塞平均时长:       %.3f ns  (总时长/阻塞次数)\n",
               blk_cnt > 0 ? parser_blk_dir_ns / (double)blk_cnt : 0.0);
        printf("    方向阻塞占收到请求比:   %.2f%%  (阻塞次数/请求总数)\n",
               (double)blk_cnt / N * 100.0);
        printf("  ---\n");
        printf("  [Parser 视角的快照平均并发] (按请求数加权, 非时间加权)\n");
        printf("    Read  avg = %.2f / %d  (%.2f%%)\n",
               parser_snap_read_sum / N, (int)IOMMU_READ_MAX_OUTSTANDING,
               parser_snap_read_sum / N / (double)IOMMU_READ_MAX_OUTSTANDING * 100.0);
        printf("    Write avg = %.2f / %d  (%.2f%%)\n",
               parser_snap_write_sum / N, (int)IOMMU_WRITE_MAX_OUTSTANDING,
               parser_snap_write_sum / N / (double)IOMMU_WRITE_MAX_OUTSTANDING * 100.0);
        printf("    Global avg= %.2f / %d  (%.2f%%)\n",
               parser_snap_global_sum / N, (int)IOMMU_GLOBAL_MAX_OUTSTANDING,
               parser_snap_global_sum / N / (double)IOMMU_GLOBAL_MAX_OUTSTANDING * 100.0);
        printf("    Read+Write avg 相加  = %.2f\n",
               (parser_snap_read_sum + parser_snap_write_sum) / N);
    } else {
        printf("  (无采样: parser_recv_total=0)\n");
    }
    printf("=========================================================\n\n");

    // ============================================================
    // [STAT] IOMMU 内部实际并发 - 时间加权面积法
    //   采样点与 outstanding ++/-- 严格配对:
    //     ++ 入口: reorder_register_task()
    //     -- 出口: reorder_output_thread()
    //   平均并发 = 累计面积 / 采样窗口; 此值即"IOMMU 内部实际在处理的任务并发数"。
    // ============================================================
    printf("========== IOMMU 内部实际并发 (时间加权面积法) ==========\n");
    {
        double area_r = iommu_conc_area_read_ns;
        double area_w = iommu_conc_area_write_ns;
        double area_g = iommu_conc_area_global_ns;
        double window_ns = 0.0;
        if (iommu_conc_first_sample_ns >= 0.0) {
            // 收尾: 加上从上次变化到当前仿真时刻的面积
            const double now_ns = sc_time_stamp().to_seconds() * 1e9;
            const double tail = now_ns - iommu_conc_last_change_ns;
            if (tail > 0.0) {
                area_r += (double)iommu_read_outstanding   * tail;
                area_w += (double)iommu_write_outstanding  * tail;
                area_g += (double)iommu_global_outstanding * tail;
            }
            window_ns = now_ns - iommu_conc_first_sample_ns;
        }
        if (window_ns > 0.0) {
            const double avg_r = area_r / window_ns;
            const double avg_w = area_w / window_ns;
            const double avg_g = area_g / window_ns;
            printf("  采样窗口: %.1f ns (%.3f us)   [%.1f ~ %.1f ns]\n",
                   window_ns, window_ns / 1000.0,
                   iommu_conc_first_sample_ns, iommu_conc_last_sample_ns);
            printf("  %-26s %10s %10s %8s %10s\n",
                   "项目", "时间加权avg", "peak", "上限", "avg利用率");
            printf("  %-26s %10.2f %10d %8d %9.2f%%\n", "Read  并发(实际在处理)",
                   avg_r, peak_iommu_read_outstanding,
                   (int)IOMMU_READ_MAX_OUTSTANDING,
                   avg_r / (double)IOMMU_READ_MAX_OUTSTANDING * 100.0);
            printf("  %-26s %10.2f %10d %8d %9.2f%%\n", "Write 并发(实际在处理)",
                   avg_w, peak_iommu_write_outstanding,
                   (int)IOMMU_WRITE_MAX_OUTSTANDING,
                   avg_w / (double)IOMMU_WRITE_MAX_OUTSTANDING * 100.0);
            printf("  %-26s %10.2f %10d %8d %9.2f%%\n", "Global并发(实际在处理)",
                   avg_g, peak_iommu_global_outstanding,
                   (int)IOMMU_GLOBAL_MAX_OUTSTANDING,
                   avg_g / (double)IOMMU_GLOBAL_MAX_OUTSTANDING * 100.0);
            printf("  ---\n");
            printf("  Read+Write 相加校验   = %.2f  (Global avg = %.2f, 差 %.4f)\n",
                   avg_r + avg_w, avg_g, (avg_r + avg_w) - avg_g);
            printf("  累计面积: read=%.1f  write=%.1f  global=%.1f  (task·ns)\n",
                   area_r, area_w, area_g);
            // Little's Law 交叉验证: L = λ × W
            if (iommu_total_completed > 0 && iommu_e2e_values.size() > 0) {
                double e2e_sum = 0.0;
                for (double v : iommu_e2e_values) e2e_sum += v;
                const double e2e_avg = e2e_sum / (double)iommu_e2e_values.size();
                const double lambda = (double)iommu_total_completed / window_ns * 1000.0; // M/s
                printf("  ---\n");
                printf("  [Little's Law 交叉验证] λ=%.2f M/s, IO_avg_e2e=%.2f ns -> λ×W=%.2f\n",
                       lambda, e2e_avg, lambda * e2e_avg);
                printf("    注: IO_e2e 口径为 pop->response(含出口流水线), 比 outstanding\n");
                printf("        持有区间(admit->release)长, 故 λ×W 会高于实测 Global avg。\n");
            }
        } else {
            printf("  (无采样: window_ns<=0)\n");
        }
    }
    printf("=========================================================\n\n");

    // [MONITOR] 阻塞点监控统计报告
    printf("========== [MONITOR] 阻塞点监控统计 ==========\n");
    
    // 监控点1: flush_dedup_buffer_by_iova 阻塞统计
    printf("--- 监控点1: flush_dedup_buffer_by_iova 阻塞 ---\n");
    printf("  flush调用总次数:          %lu\n", (unsigned long)monitor_flush_total_count);
    printf("  flush总耗时:              %.1f ns (%.3f us)\n", monitor_flush_total_ns, monitor_flush_total_ns / 1000.0);
    printf("  单次flush平均耗时:        %.2f ns\n", 
           monitor_flush_total_count > 0 ? monitor_flush_total_ns / monitor_flush_total_count : 0.0);
    printf("  单次flush最大耗时:        %.1f ns\n", monitor_flush_max_ns);
    printf("  FIFO阻塞次数:             %lu\n", (unsigned long)monitor_flush_fifo_block_count);
    printf("  FIFO阻塞总耗时:           %.1f ns\n", monitor_flush_fifo_block_total_ns);
    printf("  FIFO阻塞平均耗时:         %.2f ns\n",
           monitor_flush_fifo_block_count > 0 ? monitor_flush_fifo_block_total_ns / monitor_flush_fifo_block_count : 0.0);
    printf("  FIFO阻塞最大耗时:         %.1f ns\n", monitor_flush_fifo_block_max_ns);
    // [NEW] 扫描时间与FIFO阻塞时间分离
    printf("  [分解] 纯扫描总耗时:      %.1f ns (%.3f us)\n", monitor_flush_scan_total_ns, monitor_flush_scan_total_ns / 1000.0);
    printf("  [分解] 单次扫描平均耗时:  %.2f ns\n",
           monitor_flush_total_count > 0 ? monitor_flush_scan_total_ns / monitor_flush_total_count : 0.0);
    printf("  [分解] 单次扫描最大耗时:  %.1f ns\n", monitor_flush_scan_max_ns);
    printf("  [分解] FIFO阻塞占比:      %.1f%%\n",
           monitor_flush_total_ns > 0 ? monitor_flush_fifo_block_total_ns / monitor_flush_total_ns * 100.0 : 0.0);
    printf("  [分解] 扫描entry总数:     %lu (每次%u entries)\n",
           (unsigned long)monitor_flush_scanned_entries, (unsigned)PT_DEDUP_BUFFER_SIZE);
    printf("  [分解] 匹配entry总数:     %lu (平均每次%.1f个)\n",
           (unsigned long)monitor_flush_matched_entries,
           monitor_flush_total_count > 0 ? (double)monitor_flush_matched_entries / monitor_flush_total_count : 0.0);
    printf("\n");
    
    // 监控点2: PT Cache UPDATE 阻塞统计
    printf("--- 监控点2: PT Cache UPDATE 阻塞 ---\n");
    printf("  PT UPDATE写入总次数:      %lu\n", (unsigned long)monitor_pt_update_total_count);
    printf("  PT UPDATE写入总耗时:      %.1f ns (%.3f us)\n", monitor_pt_update_total_ns, monitor_pt_update_total_ns / 1000.0);
    printf("  单次PT UPDATE平均耗时:    %.2f ns\n",
           monitor_pt_update_total_count > 0 ? monitor_pt_update_total_ns / monitor_pt_update_total_count : 0.0);
    printf("  单次PT UPDATE最大耗时:    %.1f ns\n", monitor_pt_update_max_ns);
    printf("  FIFO阻塞次数:             %lu\n", (unsigned long)monitor_pt_update_fifo_block_count);
    printf("  FIFO阻塞总耗时:           %.1f ns\n", monitor_pt_update_fifo_block_ns);
    // [NEW] PT UPDATE排队等待时间
    printf("  [排队] UPDATE在FIFO中排队总时间: %.1f ns (%.3f us)\n",
           monitor_pt_update_queue_wait_total_ns, monitor_pt_update_queue_wait_total_ns / 1000.0);
    printf("  [排队] 有排队的UPDATE次数:       %lu\n", (unsigned long)monitor_pt_update_queue_wait_count);
    printf("  [排队] 平均排队等待时间:         %.2f ns\n",
           monitor_pt_update_queue_wait_count > 0 ? monitor_pt_update_queue_wait_total_ns / monitor_pt_update_queue_wait_count : 0.0);
    printf("  [排队] 最大排队等待时间:         %.1f ns\n", monitor_pt_update_queue_wait_max_ns);
    // [NEW] 从StatsCollector读取PT UPDATE排队等待统计(乒乓调度中的实际排队)
    {
        const auto& pt_stats = cache_sub.stats().get_stats("pt_cache");
        printf("  [Stats] UPDATE任务数:          %lu\n", (unsigned long)pt_stats.upd_task_count);
        printf("  [Stats] UPDATE FIFO排队总时间: %.1f ns (%.3f us)\n",
               pt_stats.upd_task_wait_ns, pt_stats.upd_task_wait_ns / 1000.0);
        printf("  [Stats] UPDATE平均排队时间:    %.2f ns\n",
               pt_stats.upd_task_count > 0 ? pt_stats.upd_task_wait_ns / pt_stats.upd_task_count : 0.0);
        printf("  [Stats] REQUEST FIFO排队总时间: %.1f ns (%.3f us)\n",
               pt_stats.req_task_wait_ns, pt_stats.req_task_wait_ns / 1000.0);
        printf("  [Stats] REQUEST任务数:          %lu\n", (unsigned long)pt_stats.req_task_count);
        printf("  [Stats] REQUEST平均排队时间:    %.2f ns\n",
               pt_stats.req_task_count > 0 ? pt_stats.req_task_wait_ns / pt_stats.req_task_count : 0.0);
    }
    printf("\n");
    
    // 监控点3: Monitor线程整体处理统计
    printf("--- 监控点3: Monitor线程整体处理 ---\n");
    printf("  处理group总数:            %lu\n", (unsigned long)monitor_group_total_count);
    printf("  group总处理耗时:          %.1f ns (%.3f us)\n", monitor_group_total_process_ns, monitor_group_total_process_ns / 1000.0);
    printf("  单个group平均处理耗时:    %.2f ns\n",
           monitor_group_total_count > 0 ? monitor_group_total_process_ns / monitor_group_total_count : 0.0);
    printf("  单个group最大处理耗时:    %.1f ns\n", monitor_group_max_process_ns);
    printf("\n");
    
    // 监控点4: 重排序乱序暂存统计
    printf("--- 监控点4: 重排序乱序暂存 ---\n");
    printf("  乱序到达任务数:           %lu\n", (unsigned long)reorder_out_of_order_count);
    printf("  标记ready总任务数:       %lu\n", (unsigned long)reorder_total_marked_count);
    printf("  乱序比例:                 %.2f%%\n",
           reorder_total_marked_count > 0 ? (double)reorder_out_of_order_count / reorder_total_marked_count * 100.0 : 0.0);
    printf("  写请求保序阻塞次数:       %lu\n", (unsigned long)reorder_write_blocked_count);
    printf("  写请求阻塞总耗时:         %.1f ns\n", reorder_write_blocked_total_ns);
    printf("  写请求阻塞平均耗时:       %.2f ns\n",
           reorder_write_blocked_count > 0 ? reorder_write_blocked_total_ns / reorder_write_blocked_count : 0.0);
    printf("  写请求阻塞最大耗时:       %.1f ns\n", reorder_write_blocked_max_ns);
    printf("  等待队头次数:             %lu\n", (unsigned long)reorder_wait_for_head_count);
    // [NEW] 读请求reorder等待延时
    printf("  [读请求] 总发送次数:       %lu\n", (unsigned long)reorder_read_total_sent_count);
    printf("  [读请求] 有等待的次数:     %lu (比例=%.2f%%)\n",
           (unsigned long)reorder_read_waited_count,
           reorder_read_total_sent_count > 0 ? (double)reorder_read_waited_count / reorder_read_total_sent_count * 100.0 : 0.0);
    printf("  [读请求] 等待总耗时:       %.1f ns (%.3f us)\n",
           reorder_read_total_wait_ns, reorder_read_total_wait_ns / 1000.0);
    printf("  [读请求] 平均等待耗时:     %.2f ns\n",
           reorder_read_waited_count > 0 ? reorder_read_total_wait_ns / reorder_read_waited_count : 0.0);
    printf("  [读请求] 最大等待耗时:     %.1f ns\n", reorder_read_max_wait_ns);
    printf("============================================\n\n");

    // Serial Cache Pipeline Busy-Time Analysis [DISABLED - causes segfault after sc_stop]
    /*
    printf("========== Serial Cache Pipeline Analysis ==========\n");
    double sim_ns = sim_time_sec * 1e9;
    if (sim_ns > 0) {
        // DC Cache (CacheSubsystem internal)
        const auto& dc_stats = cache_sub.stats().get_stats("dc_cache");
        double dc_busy_ns = dc_stats.total_execution_latency_ns;
        printf("  DC Cache (sub):     %lu reqs  exec_busy=%.0f ns (%.3f us)  util=%.1f%%\n",
               (unsigned long)dc_stats.request_latency_samples,
               dc_busy_ns, dc_busy_ns/1000.0,
               dc_busy_ns / sim_ns * 100.0);
        if (dc_stats.request_latency_samples > 0) {
            printf("    avg exec=%.1f ns  avg total=%.1f ns\n",
                   dc_stats.avg_execution_latency_ns(),
                   dc_stats.avg_request_latency_ns());
        }

        // PC Cache (CacheSubsystem internal)
        const auto& pc_stats = cache_sub.stats().get_stats("pc_cache");
        double pc_busy_ns = pc_stats.total_execution_latency_ns;
        printf("  PC Cache (sub):     %lu reqs  exec_busy=%.0f ns (%.3f us)  util=%.1f%%\n",
               (unsigned long)pc_stats.request_latency_samples,
               pc_busy_ns, pc_busy_ns/1000.0,
               pc_busy_ns / sim_ns * 100.0);
        if (pc_stats.request_latency_samples > 0) {
            printf("    avg exec=%.1f ns  avg total=%.1f ns\n",
                   pc_stats.avg_execution_latency_ns(),
                   pc_stats.avg_request_latency_ns());
        }

        // PT Cache (CacheSubsystem internal)
        const auto& pt_stats = cache_sub.stats().get_stats("pt_cache");
        double pt_busy_ns = pt_stats.total_execution_latency_ns;
        printf("  PT Cache (sub):     %lu reqs  exec_busy=%.0f ns (%.3f us)  util=%.1f%%\n",
               (unsigned long)pt_stats.request_latency_samples,
               pt_busy_ns, pt_busy_ns/1000.0,
               pt_busy_ns / sim_ns * 100.0);
        if (pt_stats.request_latency_samples > 0) {
            printf("    avg exec=%.1f ns  avg total=%.1f ns\n",
                   pt_stats.avg_execution_latency_ns(),
                   pt_stats.avg_request_latency_ns());
        }

        // Walker Cache (CacheSubsystem internal)
        const auto& wk_stats = cache_sub.stats().get_stats("walker_cache");
        double wk_busy_ns = wk_stats.total_execution_latency_ns;
        printf("  Walker Cache (sub): %lu reqs  exec_busy=%.0f ns (%.3f us)  util=%.1f%%\n",
               (unsigned long)wk_stats.request_latency_samples,
               wk_busy_ns, wk_busy_ns/1000.0,
               wk_busy_ns / sim_ns * 100.0);
        if (wk_stats.request_latency_samples > 0) {
            printf("    avg exec=%.1f ns  avg total=%.1f ns\n",
                   wk_stats.avg_execution_latency_ns(),
                   wk_stats.avg_request_latency_ns());
        }

        // Pipeline drain analysis
        // 每512B包入口占用时间 = 512*8/BANDWIDTH_MBPS*1000 ns (128GB/s时为4ns)
        double per_pkt_ns = 512.0 * 8.0 * 1000.0 / (double)AXI_SLAVE_0_BANDWIDTH_MBPS;
        double input_time_ns = (double)iommu_total_completed * per_pkt_ns;
        double drain_ns = sim_ns - input_time_ns;
        printf("  ---\n");
        printf("  Input active:       %.0f ns (%.3f us)\n", input_time_ns, input_time_ns/1000.0);
        printf("  Pipeline drain:     %.0f ns (%.3f us)\n", drain_ns, drain_ns/1000.0);
        printf("  Drain / SimTime:    %.1f%%\n", drain_ns / sim_ns * 100.0);
    }
    printf("===================================================\n\n");
    */
    fflush(stdout);
}
