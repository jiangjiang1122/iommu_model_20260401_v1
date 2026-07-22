#include "subsystem/cache_subsystem.h"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace iommu {

namespace {

CacheMsgType invalidate_type_from_cmd(InvalidCmdType cmd_type) {
    switch (cmd_type) {
        case InvalidCmdType::GLOBAL_INVAL: return CacheMsgType::DC_INVALIDATE;
        case InvalidCmdType::IODIR_INVAL_DDT: return CacheMsgType::DC_INVALIDATE;
        case InvalidCmdType::IODIR_INVAL_PDT: return CacheMsgType::PC_INVALIDATE;
        case InvalidCmdType::IOTINVAL_VMA: return CacheMsgType::PT_INVALIDATE;
        case InvalidCmdType::IOTINVAL_GVMA: return CacheMsgType::PT_INVALIDATE;
        default: return CacheMsgType::PT_INVALIDATE;
    }
}

const char* msg_type_name(CacheMsgType type) {
    switch (type) {
        case CacheMsgType::DC_LOOKUP: return "DC_LOOKUP";
        case CacheMsgType::PC_LOOKUP: return "PC_LOOKUP";
        case CacheMsgType::PT_LOOKUP: return "PT_LOOKUP";
        case CacheMsgType::WALKER_LOOKUP: return "WALKER_LOOKUP";
        case CacheMsgType::MSI_LOOKUP: return "MSI_LOOKUP";
        case CacheMsgType::DC_UPDATE: return "DC_UPDATE";
        case CacheMsgType::PC_UPDATE: return "PC_UPDATE";
        case CacheMsgType::PT_UPDATE: return "PT_UPDATE";
        case CacheMsgType::WALKER_UPDATE: return "WALKER_UPDATE";
        case CacheMsgType::MSI_UPDATE: return "MSI_UPDATE";
        case CacheMsgType::DC_INVALIDATE: return "DC_INVALIDATE";
        case CacheMsgType::PC_INVALIDATE: return "PC_INVALIDATE";
        case CacheMsgType::PT_INVALIDATE: return "PT_INVALIDATE";
        case CacheMsgType::WALKER_INVALIDATE: return "WALKER_INVALIDATE";
        case CacheMsgType::MSI_INVALIDATE: return "MSI_INVALIDATE";
        case CacheMsgType::CACHE_RESPONSE: return "CACHE_RESPONSE";
        case CacheMsgType::CACHE_UPDATE_RESPONSE: return "CACHE_UPDATE_RESPONSE";
        case CacheMsgType::CACHE_INVALIDATE_RESPONSE: return "CACHE_INVALIDATE_RESPONSE";
        case CacheMsgType::TRANSLATE: return "TRANSLATE";
        case CacheMsgType::PREFETCH: return "PREFETCH";
        default: return "UNKNOWN";
    }
}

const char* invalid_cmd_type_name(InvalidCmdType type) {
    switch (type) {
        case InvalidCmdType::GLOBAL_INVAL: return "GLOBAL_INVAL";
        case InvalidCmdType::IODIR_INVAL_DDT: return "IODIR_INVAL_DDT";
        case InvalidCmdType::IODIR_INVAL_PDT: return "IODIR_INVAL_PDT";
        case InvalidCmdType::IOTINVAL_VMA: return "IOTINVAL_VMA";
        case InvalidCmdType::IOTINVAL_GVMA: return "IOTINVAL_GVMA";
        default: return "UNKNOWN";
    }
}

const char* stage_name(TransStage stage) {
    switch (stage) {
        case TransStage::STAGE1_ONLY: return "STAGE1_ONLY";
        case TransStage::STAGE2_ONLY: return "STAGE2_ONLY";
        case TransStage::STAGE1_AND_2: return "STAGE1_AND_2";
        default: return "UNKNOWN";
    }
}

const char* invalidate_mode_name(CacheInvalidateMode mode) {
    switch (mode) {
        case CacheInvalidateMode::PRECISE: return "PRECISE";
        case CacheInvalidateMode::SCAN: return "SCAN";
        case CacheInvalidateMode::GLOBAL: return "GLOBAL";
        default: return "UNKNOWN";
    }
}

CacheSubsystem::TaskTraceLevel parse_task_trace_level(std::string level) {
    std::transform(level.begin(), level.end(), level.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (level == "detail" || level == "detailed" || level == "debug" || level == "verbose") {
        return CacheSubsystem::TaskTraceLevel::DETAIL;
    }
    if (level == "basic" || level == "summary" || level == "info") {
        return CacheSubsystem::TaskTraceLevel::BASIC;
    }
    return CacheSubsystem::TaskTraceLevel::OFF;
}

const char* task_trace_level_name(CacheSubsystem::TaskTraceLevel level) {
    switch (level) {
        case CacheSubsystem::TaskTraceLevel::DETAIL: return "detail";
        case CacheSubsystem::TaskTraceLevel::BASIC: return "basic";
        case CacheSubsystem::TaskTraceLevel::OFF:
        default:
            return "off";
    }
}

}

CacheSubsystem::CacheSubsystem(sc_module_name name, const GlobalConfig& cfg)
    : sc_module(name),
      cfg_(cfg),
      dc_request_fifo(16),
      pc_request_fifo(16),
      pt_request_fifo(16),
      walker_request_fifo(1),
      msi_request_fifo(1),
      dedup_request_fifo(16),
      dedup_update_fifo(16),
      dc_response_fifo(16),
      pc_response_fifo(16),
      pt_hit_response_fifo(16),
      pt_miss_response_fifo(16),
      walker_response_fifo(1),
      msi_response_fifo(1),
      dc_update_fifo(1),
      pc_update_fifo(1),
      pt_update_fifo(16),
      walker_update_fifo(1),
      msi_update_fifo(1),
      dc_invalidate_fifo(1),
      pc_invalidate_fifo(1),
      pt_invalidate_fifo(16),
      walker_invalidate_fifo(1),
      msi_invalidate_fifo(1),
      dc_invalidate_response_fifo(1),
      pc_invalidate_response_fifo(1),
      pt_invalidate_response_fifo(16),
      walker_invalidate_response_fifo(1),
      msi_invalidate_response_fifo(1),
      invalidation_request_fifo(1),
      invalidation_response_fifo(1)
{
    clock_period_ = sc_time(cfg.clock_period_ns, SC_NS);
    task_trace_level_ = parse_task_trace_level(cfg.statistics.task_trace_level);
    if (cfg.statistics.enable_task_trace && task_trace_level_ == TaskTraceLevel::OFF) {
        task_trace_level_ = TaskTraceLevel::DETAIL;
    }

    dc_cache_ = std::make_unique<DCCache>("dc_cache", cfg.dc_cache, stats_);
    dc_cache_->set_clock_period(clock_period_);
    pc_cache_ = std::make_unique<PCCache>("pc_cache", cfg.pc_cache, stats_);
    pc_cache_->set_clock_period(clock_period_);
    msipt_cache_ = std::make_unique<MSIPTCache>("msipt_cache", cfg.msipt_cache, stats_);
    msipt_cache_->set_clock_period(clock_period_);
    pt_cache_ = std::make_unique<PTCache>("pt_cache", cfg.pt_cache, stats_);
    pt_cache_->set_clock_period(clock_period_);
    walker_cache_ = std::make_unique<WalkerCache>(
        "walker_cache", cfg.walker_ptw_c1, cfg.walker_ptw_c2, cfg.walker_ptw_c3, stats_);
    walker_cache_->set_clock_period(clock_period_);

    stats_.set_histogram_bin_width(cfg.statistics.histogram_bin_width_ns);
    stats_.set_latency_histogram_enabled(cfg.statistics.enable_latency_histogram);
    stats_.set_output_file(cfg.statistics.output_file);

    SC_THREAD(dc_worker_thread);
    SC_THREAD(pc_worker_thread);
    SC_THREAD(pt_scheduler_thread);  // [NEW] 统一的PT Cache轮询调度线程
    SC_THREAD(dedup_scheduler_thread);  // [重构] 去重 Cache 乒乓调度线程
    SC_THREAD(walker_scheduler_thread);  // 统一的walker cache轮询调度线程
    SC_THREAD(msi_worker_thread);
    SC_THREAD(dc_update_worker_thread);
    SC_THREAD(pc_update_worker_thread);
    SC_THREAD(msi_update_worker_thread);
    SC_THREAD(dc_invalidate_worker_thread);
    SC_THREAD(pc_invalidate_worker_thread);
    SC_THREAD(pt_invalidate_worker_thread);
    SC_THREAD(msi_invalidate_worker_thread);
    SC_THREAD(invalidation_worker_thread);
    
    // [NEW] 初始化PT Cache去重功能
    dedup_buffer_ = new DedupBuffer();
    pt_dedup_enabled_ = true;
    // [重构] 创建独立 dedup_cache (几何复用 pt_cache 配置)
    dedup_cache_ = std::make_unique<DedupCache>(cfg.pt_cache.num_sets, cfg.pt_cache.num_ways);
    printf("[CACHE_SUBSYSTEM] PT Cache dedup enabled, Buffer + dedup_cache created (sets=%u, ways=%u)\n",
           cfg.pt_cache.num_sets, cfg.pt_cache.num_ways);
    fflush(stdout);
}

void CacheSubsystem::set_task_trace_enabled(bool enabled) {
    task_trace_level_ = enabled ? TaskTraceLevel::DETAIL : TaskTraceLevel::OFF;
}

void CacheSubsystem::end_of_simulation() {
    if (final_report_written_) return;
    stats_.report();
    final_report_written_ = true;
}

void CacheSubsystem::push_fifo(sc_fifo<CacheMessage>& fifo, const CacheMessage& msg) {
    if (!sc_get_current_process_handle().valid()) {
        SC_REPORT_ERROR("CacheSubsystem", "push_fifo must be called from a SystemC process");
        return;
    }
    fifo.write(msg);
}

bool CacheSubsystem::pop_fifo(sc_fifo<CacheMessage>& fifo, CacheMessage& msg) {
    return fifo.nb_read(msg);
}

void CacheSubsystem::process_next_invalidation_request() {
    CacheMessage msg;
    if (!pop_fifo(invalidation_request_fifo, msg)) {
        return;
    }
    const sc_time start = sc_time_stamp();
    trace_task_event("begin", "invalidation_pipeline", "invalidate", msg, start);
    CacheMessage resp = execute_invalidation_pipeline(msg);
    record_task_completion("invalidation_pipeline", start, sc_time_stamp());
    trace_task_event("end", "invalidation_pipeline", "invalidate", msg, start, &resp);
    push_fifo(invalidation_response_fifo, resp);
}

void CacheSubsystem::record_task_completion(const std::string& cache_name,
                                            const sc_time& start_time,
                                            const sc_time& end_time) {
    const sc_time total_latency = end_time >= start_time ?
        (end_time - start_time) : SC_ZERO_TIME;
    stats_.record_request_latency(cache_name,
                                  total_latency.to_seconds() * 1e9,
                                  start_time.to_seconds() * 1e9,
                                  end_time.to_seconds() * 1e9);
}

void CacheSubsystem::trace_task_event(const char* phase,
                                      const std::string& cache_name,
                                      const std::string& op_name,
                                      const CacheMessage& msg,
                                      const sc_time& start_time,
                                      const CacheMessage* resp) {
    if (task_trace_level_ == TaskTraceLevel::OFF) return;

    const bool is_begin = std::string(phase) == "begin";
    if (task_trace_level_ == TaskTraceLevel::BASIC && is_begin) return;
    const bool is_invalidate_msg =
        msg.msg_type == CacheMsgType::DC_INVALIDATE ||
        msg.msg_type == CacheMsgType::PC_INVALIDATE ||
        msg.msg_type == CacheMsgType::PT_INVALIDATE ||
        msg.msg_type == CacheMsgType::WALKER_INVALIDATE ||
        msg.msg_type == CacheMsgType::MSI_INVALIDATE;

    std::ostringstream oss;
    oss << "[cache-task]"
        << " level=" << task_trace_level_name(task_trace_level_)
        << " phase=" << phase
        << " cache=" << cache_name
        << " op=" << op_name
        << " enter_time=" << start_time;
    if (!is_begin) {
        oss << " done_time=" << sc_time_stamp();
    }
    oss << " request_type=" << msg_type_name(msg.msg_type)
        << " task_id=" << msg.task_id
        << " msg_type=" << msg_type_name(msg.msg_type);
    if (is_invalidate_msg || op_name == "invalidate") {
        oss << " cmd_type=" << invalid_cmd_type_name(msg.cmd_type)
            << " invalidate_mode=" << invalidate_mode_name(msg.invalidate_mode);
    }
    if (msg.msg_type == CacheMsgType::PT_LOOKUP ||
        msg.msg_type == CacheMsgType::PT_UPDATE ||
        msg.msg_type == CacheMsgType::WALKER_LOOKUP ||
        msg.msg_type == CacheMsgType::WALKER_UPDATE) {
        oss << " stage=" << stage_name(msg.stage);
    }
    if (msg.device_id != 0) {
        oss << " device_id=" << msg.device_id;
    }
    if (msg.process_id != 0) {
        oss << " process_id=" << msg.process_id;
    }
    if (msg.msi_index != 0) {
        oss << " msi_index=" << msg.msi_index;
    }
    if (msg.gscid != 0 || msg.pscid != 0) {
        oss << " gscid=" << msg.gscid << " pscid=" << msg.pscid;
    }
    if (msg.iova != 0) {
        oss << " iova=0x" << std::hex << msg.iova << std::dec;
    }
    if (resp) {
        oss << " hit=" << resp->hit
            << " affected=" << resp->affected_entries
            << " exec_latency=" << resp->latency
            << " total_latency=" << (sc_time_stamp() - start_time);
    }
    stats_.write_log(oss.str());
}

void CacheSubsystem::dc_worker_thread() {
    while (true) {
        CacheMessage req = dc_request_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "dc_cache", "lookup", req, start);
        CacheMessage resp = execute_dc_request(req);
        record_task_completion("dc_cache", start, sc_time_stamp());
        trace_task_event("end", "dc_cache", "lookup", req, start, &resp);
        push_fifo(dc_response_fifo, resp);
    }
}

void CacheSubsystem::pc_worker_thread() {
    while (true) {
        CacheMessage req = pc_request_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pc_cache", "lookup", req, start);
        CacheMessage resp = execute_pc_request(req);
        record_task_completion("pc_cache", start, sc_time_stamp());
        trace_task_event("end", "pc_cache", "lookup", req, start, &resp);
        push_fifo(pc_response_fifo, resp);
    }
}

// PT Cache 乒乓调度线程
// REQUEST 和 UPDATE 交替执行（乒乓），不再优先处理某一类
// [STAT] 统计每32个REQUEST为一组的执行延时、排队延时、端到端延时
void CacheSubsystem::pt_scheduler_thread() {
    uint64_t pt_req_counter = 0;  // [STAT] REQUEST区间统计计数器
    while (true) {
        CacheMessage req;

        // =============================================================
        // 乒乓调度: 根据 pt_sched_next_is_request_ 决定本轮优先处理哪类
        // =============================================================
        bool has_req = (pt_request_fifo.num_available() > 0);
        bool has_upd = (pt_update_fifo.num_available() > 0);

        if (!has_req && !has_upd) {
            // 两个FIFO都空，等待
            wait(pt_request_fifo.data_written_event() | 
                 pt_update_fifo.data_written_event());
            continue;
        }

        // 决定本轮处理类型
        bool do_request;
        if (has_req && has_upd) {
            do_request = pt_sched_next_is_request_;  // 两者都有，按乒乓选择
        } else {
            do_request = has_req;  // 只有一个可用，处理它
        }

        if (do_request) {
            // =============================================================
            // 处理 REQUEST
            // =============================================================
            req = pt_request_fifo.read();
            const sc_time dequeue_time = sc_time_stamp();
            const double start_ns = dequeue_time.to_seconds() * 1e9;

            double fifo_wait_ns = start_ns - req.timestamp.to_seconds() * 1e9;
            stats_.accumulate_phase_task_wait("pt_cache", 0, fifo_wait_ns);

            // [DEBUG] 打印pt_request_fifo dequeue时间戳
            printf("[PT_SCHED] task_id=%u -> dequeue from pt_request_fifo [entry=%.1f ns, dequeue=%.1f ns, fifo_wait=%.2f ns]\n",
                   req.task_id, req.timestamp.to_seconds() * 1e9, start_ns, fifo_wait_ns);
            fflush(stdout);

            trace_task_event("begin", "pt_cache", "lookup", req, dequeue_time);
            CacheMessage resp = execute_pt_request(req);
            const sc_time end = sc_time_stamp();
            const double end_ns = end.to_seconds() * 1e9;
            double exec_ns = end_ns - start_ns;

            // [STAT] 全局间隔分析
            if (pt_sched_last_task_end_ >= 0.0) {
                double gap_ns = start_ns - pt_sched_last_task_end_;
                if (gap_ns > 0.0) {
                    pt_sched_total_gap_ns_ += gap_ns;
                    pt_sched_gap_count_++;
                    if (gap_ns > pt_sched_max_gap_ns_) pt_sched_max_gap_ns_ = gap_ns;
                }
            }
            if (pt_sched_first_start_ns_ < 0.0) pt_sched_first_start_ns_ = start_ns;
            pt_sched_last_end_ns_ = end_ns;
            pt_sched_total_exec_ns_ += exec_ns;
            pt_sched_task_count_++;
            pt_sched_req_count_++;
            pt_sched_last_task_end_ = end_ns;

            record_task_completion("pt_cache", dequeue_time, end);
            stats_.record_pt_phase_timestamp("pt_cache", 0, start_ns, end_ns);
            trace_task_event("end", "pt_cache", "lookup", req, dequeue_time, &resp);

            // 区间命中率统计
            int interval_idx = static_cast<int>(pt_req_counter / 1000);
            if (resp.hit) stats_.record_interval_hit("pt_cache", interval_idx);
            else           stats_.record_interval_miss("pt_cache", interval_idx);
            pt_req_counter++;

            if (resp.hit) {
                // HIT 常规CL: 直接返回
                push_fifo(pt_hit_response_fifo, resp);
            } else {
                // [重构] MISS: 转发到 dedup_request_fifo (携带原请求上下文与预取参数)
                CacheMessage dedup_req = req;
                dedup_req.timestamp = sc_time_stamp();
                push_fifo(dedup_request_fifo, dedup_req);
            }

            // =============================================================
            // [STAT] 32任务组统计 - REQUEST完成
            // =============================================================
            int pos = pt_group_pos_;
            pt_group_exec_ns_[pos]   = exec_ns;
            pt_group_e2e_ns_[pos]    = end_ns - req.timestamp.to_seconds() * 1e9;
            pt_group_queue_ns_[pos]  = pt_group_upd_since_last_;
            pt_group_upd_count_[pos] = pt_group_upd_cnt_since_;

            pt_group_sum_exec_[pos]  += exec_ns;
            pt_group_sum_queue_[pos] += pt_group_upd_since_last_;
            pt_group_sum_e2e_[pos]   += (end_ns - req.timestamp.to_seconds() * 1e9);
            pt_group_sum_upd_[pos]   += pt_group_upd_cnt_since_;

            pt_group_pos_++;
            if (pt_group_pos_ >= PT_GROUP_SIZE) {
                pt_group_total_groups_++;
                pt_group_pos_ = 0;
            }
            pt_group_upd_since_last_ = 0.0;
            pt_group_upd_cnt_since_ = 0;
            pt_group_last_req_end_ = end_ns;

            // 乒乓切换
            pt_sched_next_is_request_ = false;
        } else {
            // =============================================================
            // 处理 UPDATE
            // =============================================================
            req = pt_update_fifo.read();
            const sc_time upd_dequeue_time = sc_time_stamp();
            const double upd_start_ns = upd_dequeue_time.to_seconds() * 1e9;
            double upd_fifo_wait_ns = upd_start_ns - req.timestamp.to_seconds() * 1e9;
            stats_.accumulate_phase_task_wait("pt_cache", 1, upd_fifo_wait_ns);

            trace_task_event("begin", "pt_cache", "update", req, upd_dequeue_time);
            CacheMessage upd_resp = execute_pt_update_request(req);
            const sc_time upd_end = sc_time_stamp();
            const double upd_end_ns = upd_end.to_seconds() * 1e9;
            double upd_exec_ns = upd_end_ns - upd_start_ns;

            // [STAT] 全局间隔分析
            if (pt_sched_last_task_end_ >= 0.0) {
                double gap_ns = upd_start_ns - pt_sched_last_task_end_;
                if (gap_ns > 0.0) {
                    pt_sched_total_gap_ns_ += gap_ns;
                    pt_sched_gap_count_++;
                    if (gap_ns > pt_sched_max_gap_ns_) pt_sched_max_gap_ns_ = gap_ns;
                }
            }
            if (pt_sched_first_start_ns_ < 0.0) pt_sched_first_start_ns_ = upd_start_ns;
            pt_sched_last_end_ns_ = upd_end_ns;
            pt_sched_total_exec_ns_ += upd_exec_ns;
            pt_sched_task_count_++;
            pt_sched_upd_count_++;
            pt_sched_last_task_end_ = upd_end_ns;

            record_task_completion("pt_cache", upd_dequeue_time, upd_end);
            stats_.record_pt_phase_timestamp("pt_cache", 1, upd_start_ns, upd_end_ns);
            trace_task_event("end", "pt_cache", "update", req, upd_dequeue_time, &upd_resp);

            // [STAT] 累加到32任务组的排队统计（UPDATE执行时间计入REQUEST的排队延时）
            pt_group_upd_since_last_ += upd_exec_ns;
            pt_group_upd_cnt_since_++;

            // 乒乓切换
            pt_sched_next_is_request_ = true;
        }
    }
}

// ============================================================
// [重构] dedup_scheduler_thread - 去重 Cache 乒乓调度线程
// 乒乓处理 dedup_request_fifo / dedup_update_fifo:
//   - REQUEST: execute_dedup_request + 精确流水线时序 (原子段串行, 每任务 n*5cyc)
//   - UPDATE : execute_dedup_update (dedup_cache 查表+清除, 触发 Buffer 刷新回调)
// hash 建模为与前一任务原子段完全重叠(隐藏); 仅当流水线空闲时计一次 hash 填充。
// ============================================================
void CacheSubsystem::dedup_scheduler_thread() {
    while (true) {
        bool has_req = (dedup_request_fifo.num_available() > 0);
        bool has_upd = (dedup_update_fifo.num_available() > 0);

        bool pipeline_idle = false;
        if (!has_req && !has_upd) {
            pipeline_idle = true;
            wait(dedup_request_fifo.data_written_event() |
                 dedup_update_fifo.data_written_event());
            (void)pipeline_idle;
            continue;
        }

        bool do_request;
        if (has_req && has_upd) {
            do_request = dedup_sched_next_is_request_;  // 两者都有, 按乒乓选择
        } else {
            do_request = has_req;  // 只有一个可用, 处理它
        }

        if (do_request) {
            // ============ 处理 REQUEST ============
            CacheMessage req = dedup_request_fifo.read();
            const sc_time dequeue_time = sc_time_stamp();

            trace_task_event("begin", "dedup_cache", "lookup", req, dequeue_time);
            CacheMessage resp = execute_dedup_request(req);

            // [重构] 精确流水线时序: 每任务消耗 n_atomic 个原子段(5cyc), 单线程天然串行
            uint32_t n_atomic = last_dedup_atomic_ops_;
            wait(clock_period_ * static_cast<int>(n_atomic * DEDUP_ATOMIC_CYCLES));

            record_task_completion("dedup_cache", dequeue_time, sc_time_stamp());
            trace_task_event("end", "dedup_cache", "lookup", req, dequeue_time, &resp);

            // [重构] 路由响应:
            //   dedup_suspended: 任务已挂 Buffer -> 推 pt_hit_response_fifo(collector 识别后挂起)
            //   否则(MISS/降级): 推 pt_miss_response_fifo -> collector -> PTW
            if (resp.dedup_suspended) {
                push_fifo(pt_hit_response_fifo, resp);
            } else {
                push_fifo(pt_miss_response_fifo, resp);
            }

            dedup_sched_next_is_request_ = false;
        } else {
            // ============ 处理 UPDATE ============
            CacheMessage upd = dedup_update_fifo.read();
            const sc_time dequeue_time = sc_time_stamp();
            trace_task_event("begin", "dedup_cache", "update", upd, dequeue_time);
            execute_dedup_update(upd);
            // dedup_cache 查表+清除 计一个原子段; Buffer 刷新的 FIFO 转发阻塞已在回调内推进时间
            wait(clock_period_ * static_cast<int>(DEDUP_ATOMIC_CYCLES));
            record_task_completion("dedup_cache", dequeue_time, sc_time_stamp());
            dedup_sched_next_is_request_ = true;
        }
    }
}

// [STAT] PT Scheduler任务间隔分析报告
void CacheSubsystem::print_pt_scheduler_gap_report() const {
    printf("\n========== PT Scheduler Gap Analysis ==========\n");
    if (pt_sched_task_count_ == 0) {
        printf("  No tasks processed.\n");
        printf("=================================================\n\n");
        return;
    }
    
    double window_ns = pt_sched_last_end_ns_ - pt_sched_first_start_ns_;
    printf("  First task start:     %.1f ns\n", pt_sched_first_start_ns_);
    printf("  Last task end:        %.1f ns\n", pt_sched_last_end_ns_);
    printf("  Window (first~last):  %.1f ns  (%.3f us)\n", window_ns, window_ns / 1000.0);
    printf("  ---\n");
    printf("  Total tasks:          %lu  (REQUEST=%lu, UPDATE=%lu)\n",
           (unsigned long)pt_sched_task_count_,
           (unsigned long)pt_sched_req_count_,
           (unsigned long)pt_sched_upd_count_);
    printf("  Total exec time:      %.1f ns  (%.3f us)\n",
           pt_sched_total_exec_ns_, pt_sched_total_exec_ns_ / 1000.0);
    printf("  Avg exec per task:    %.3f ns\n",
           pt_sched_task_count_ > 0 ? pt_sched_total_exec_ns_ / pt_sched_task_count_ : 0.0);
    printf("  ---\n");
    printf("  Total gap (idle):     %.1f ns  (%.3f us)\n",
           pt_sched_total_gap_ns_, pt_sched_total_gap_ns_ / 1000.0);
    printf("  Gap count:            %lu  (times both FIFOs empty)\n",
           (unsigned long)pt_sched_gap_count_);
    printf("  Avg gap:              %.3f ns\n",
           pt_sched_gap_count_ > 0 ? pt_sched_total_gap_ns_ / pt_sched_gap_count_ : 0.0);
    printf("  Max gap:              %.1f ns\n", pt_sched_max_gap_ns_);
    printf("  ---\n");
    printf("  Idle ratio:           %.2f%%  (gap / window)\n",
           window_ns > 0 ? pt_sched_total_gap_ns_ / window_ns * 100.0 : 0.0);
    printf("  Exec ratio:           %.2f%%  (exec / window)\n",
           window_ns > 0 ? pt_sched_total_exec_ns_ / window_ns * 100.0 : 0.0);
    printf("  ---\n");
    printf("  [Idle Source Analysis]\n");
    printf("    Gap = both pt_request_fifo AND pt_update_fifo are empty.\n");
    printf("    This means PTW has not yet produced UPDATE, and Collector has not\n");
    printf("    yet produced next REQUEST. The scheduler is waiting for data.\n");
    printf("=================================================\n\n");
}

// [STAT] 32任务组REQUEST排队/执行延时统计报告
void CacheSubsystem::print_pt_group_report() const {
    printf("\n========== PT Cache 32-Task Group Latency Report ==========\n");
    if (pt_group_total_groups_ == 0 && pt_group_pos_ == 0) {
        printf("  No groups completed.\n");
        printf("=========================================================\n\n");
        return;
    }
    
    uint64_t total = pt_group_total_groups_;
    // 如果当前组未完成但已有数据，也统计
    int partial = pt_group_pos_;
    
    printf("  Completed groups:  %lu\n", (unsigned long)total);
    if (partial > 0) printf("  Partial group:     %d / %d tasks\n", partial, PT_GROUP_SIZE);
    printf("  ---\n");
    
    // 计算平均和总计
    double sum_total_exec = 0, sum_total_queue = 0, sum_total_e2e = 0;
    int sum_total_upd = 0;
    
    printf("  %-6s  %-12s  %-12s  %-12s  %-8s  %-10s\n",
           "Pos", "ExecAvg(ns)", "QueueAvg(ns)", "E2EAvg(ns)", "UpdAvg", "RAM_Access");
    printf("  %-6s  %-12s  %-12s  %-12s  %-8s  %-10s\n",
           "------", "------------", "------------", "------------", "--------", "----------");
    
    for (int i = 0; i < PT_GROUP_SIZE; i++) {
        if (total == 0) break;
        double exec_avg  = pt_group_sum_exec_[i]  / total;
        double queue_avg = pt_group_sum_queue_[i] / total;
        double e2e_avg   = pt_group_sum_e2e_[i]   / total;
        double upd_avg   = (double)pt_group_sum_upd_[i] / total;
        
        // 估算RAM访问次数: exec_avg / 4 (每次RAM访问约4ns含arbiter)
        double ram_est = exec_avg / 4.0;
        
        printf("  T%-5d  %10.2f    %10.2f    %10.2f    %6.1f    %8.1f\n",
               i + 1, exec_avg, queue_avg, e2e_avg, upd_avg, ram_est);
        
        sum_total_exec  += exec_avg;
        sum_total_queue += queue_avg;
        sum_total_e2e   += e2e_avg;
        sum_total_upd   += pt_group_sum_upd_[i];
    }
    
    if (total > 0) {
        printf("  %-6s  %-12s  %-12s  %-12s  %-8s\n",
               "------", "------------", "------------", "------------", "--------");
        printf("  TOTAL  %10.2f    %10.2f    %10.2f    %6d\n",
               sum_total_exec, sum_total_queue, sum_total_e2e, sum_total_upd);
        printf("  ---\n");
        printf("  Group interval:     %.2f ns  (exec + queue)\n", sum_total_exec + sum_total_queue);
        printf("  Exec ratio:        %.1f%%\n",
               (sum_total_exec + sum_total_queue) > 0 ?
               sum_total_exec / (sum_total_exec + sum_total_queue) * 100.0 : 0.0);
        printf("  Queue ratio:       %.1f%%\n",
               (sum_total_exec + sum_total_queue) > 0 ?
               sum_total_queue / (sum_total_exec + sum_total_queue) * 100.0 : 0.0);
    }
    printf("=========================================================\n\n");
}

// Walker Cache 统一轮询调度线程
// 按 lookup → update → invalidate 顺序轮询，避免互锁死锁
void CacheSubsystem::walker_scheduler_thread() {
    while (true) {
        bool processed = false;
        
        // 1. 优先处理 lookup 请求
        CacheMessage req;
        if (walker_request_fifo.num_available() > 0) {
            req = walker_request_fifo.read();
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "walker_cache", "lookup", req, start);
            CacheMessage resp = execute_walker_request(req);
            record_task_completion("walker_cache", start, sc_time_stamp());
            trace_task_event("end", "walker_cache", "lookup", req, start, &resp);
            push_fifo(walker_response_fifo, resp);
            processed = true;
        }
        // 2. 处理 update 请求
        else if (walker_update_fifo.num_available() > 0) {
            req = walker_update_fifo.read();
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "walker_cache", "update", req, start);
            CacheMessage resp = execute_walker_update_request(req);
            record_task_completion("walker_cache", start, sc_time_stamp());
            trace_task_event("end", "walker_cache", "update", req, start, &resp);
            processed = true;
        }
        // 3. 处理 invalidate 请求
        else if (walker_invalidate_fifo.num_available() > 0) {
            req = walker_invalidate_fifo.read();
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "walker_cache", "invalidate", req, start);
            CacheMessage resp = execute_walker_invalidate_request(req);
            record_task_completion("walker_cache", start, sc_time_stamp());
            trace_task_event("end", "walker_cache", "invalidate", req, start, &resp);
            push_fifo(walker_invalidate_response_fifo, resp);
            processed = true;
        }
        
        // 如果没有处理任何请求，等待任意一个FIFO有数据
        if (!processed) {
            // 等待任意一个FIFO有数据（使用event通知）
            wait(walker_request_fifo.data_written_event() | 
                 walker_update_fifo.data_written_event() | 
                 walker_invalidate_fifo.data_written_event());
        }
    }
}

void CacheSubsystem::msi_worker_thread() {
    while (true) {
        CacheMessage req = msi_request_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "msipt_cache", "lookup", req, start);
        CacheMessage resp = execute_msi_request(req);
        record_task_completion("msipt_cache", start, sc_time_stamp());
        trace_task_event("end", "msipt_cache", "lookup", req, start, &resp);
        push_fifo(msi_response_fifo, resp);
    }
}

void CacheSubsystem::dc_update_worker_thread() {
    while (true) {
        CacheMessage req = dc_update_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "dc_cache", "update", req, start);
        CacheMessage resp = execute_dc_update_request(req);
        record_task_completion("dc_cache", start, sc_time_stamp());
        trace_task_event("end", "dc_cache", "update", req, start, &resp);
    }
}

void CacheSubsystem::pc_update_worker_thread() {
    while (true) {
        CacheMessage req = pc_update_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pc_cache", "update", req, start);
        CacheMessage resp = execute_pc_update_request(req);
        record_task_completion("pc_cache", start, sc_time_stamp());
        trace_task_event("end", "pc_cache", "update", req, start, &resp);
    }
}

void CacheSubsystem::msi_update_worker_thread() {
    while (true) {
        CacheMessage req = msi_update_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "msipt_cache", "update", req, start);
        CacheMessage resp = execute_msi_update_request(req);
        record_task_completion("msipt_cache", start, sc_time_stamp());
        trace_task_event("end", "msipt_cache", "update", req, start, &resp);
    }
}

void CacheSubsystem::dc_invalidate_worker_thread() {
    while (true) {
        CacheMessage req = dc_invalidate_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "dc_cache", "invalidate", req, start);
        CacheMessage resp = execute_dc_invalidate_request(req);
        record_task_completion("dc_cache", start, sc_time_stamp());
        trace_task_event("end", "dc_cache", "invalidate", req, start, &resp);
        push_fifo(dc_invalidate_response_fifo, resp);
    }
}

void CacheSubsystem::pc_invalidate_worker_thread() {
    while (true) {
        CacheMessage req = pc_invalidate_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pc_cache", "invalidate", req, start);
        CacheMessage resp = execute_pc_invalidate_request(req);
        record_task_completion("pc_cache", start, sc_time_stamp());
        trace_task_event("end", "pc_cache", "invalidate", req, start, &resp);
        push_fifo(pc_invalidate_response_fifo, resp);
    }
}

void CacheSubsystem::pt_invalidate_worker_thread() {
    while (true) {
        CacheMessage req = pt_invalidate_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pt_cache", "invalidate", req, start);
        CacheMessage resp = execute_pt_invalidate_request(req);
        record_task_completion("pt_cache", start, sc_time_stamp());
        trace_task_event("end", "pt_cache", "invalidate", req, start, &resp);
        push_fifo(pt_invalidate_response_fifo, resp);
    }
}

void CacheSubsystem::msi_invalidate_worker_thread() {
    while (true) {
        CacheMessage req = msi_invalidate_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "msipt_cache", "invalidate", req, start);
        CacheMessage resp = execute_msi_invalidate_request(req);
        record_task_completion("msipt_cache", start, sc_time_stamp());
        trace_task_event("end", "msipt_cache", "invalidate", req, start, &resp);
        push_fifo(msi_invalidate_response_fifo, resp);
    }
}

void CacheSubsystem::invalidation_worker_thread() {
    while (true) {
        if (invalidation_request_fifo.num_available() == 0) {
            wait(invalidation_request_fifo.data_written_event());
        }
        process_next_invalidation_request();
    }
}

CacheMessage CacheSubsystem::execute_dc_request(const CacheMessage& req) {
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_RESPONSE;
    resp.task_id = req.task_id;
    DCData data;
    resp.hit = dc_cache_->lookup_dc(req.device_id, data, resp.latency);
    if (resp.hit) {
        resp.gscid = dc_gscid(data);
        resp.stage = dc_stage(data);
        resp.dc_data = data;
    }
    return resp;
}

CacheMessage CacheSubsystem::execute_pc_request(const CacheMessage& req) {
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_RESPONSE;
    resp.task_id = req.task_id;
    PCData data;
    resp.hit = pc_cache_->lookup_pc(req.device_id, req.process_id, data, resp.latency);
    if (resp.hit) {
        resp.pscid = pc_pscid(data);
        resp.pc_data = data;
    }
    return resp;
}

CacheMessage CacheSubsystem::execute_pt_request(const CacheMessage& req) {
    // [STAT] 设置阶段追踪器: 申请阶段(REQUEST=0)
    pt_cache_->set_current_phase(0);

    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_RESPONSE;
    resp.task_id = req.task_id;
    resp.iova = req.iova;  // FIX: 复制IOVA到响应
    resp.gscid = req.gscid;
    resp.pscid = req.pscid;

    PTData data;
    resp.hit = pt_cache_->lookup_pt(req.gscid, req.pscid, req.iova, req.stage,
                                    req.pt_sv48,
                                    req.pt_gstage_x4,
                                    data, resp.latency);

    if (resp.hit) {
        // [重构] HIT 常规 CL: 直接返回 PTE, 任务立即完成
        resp.stage = pt_stage(data);
        resp.from_prefetch = pt_from_prefetch(data);
        resp.pt_data = data;
    }
    // [重构] MISS: 不在此处处理去重/预取, 由 pt_scheduler_thread 转发到 dedup_request_fifo
    return resp;
}

// ============================================================
// [重构] execute_dedup_request - 去重 Cache 查询流程 (含 Buffer 交互)
// 运行于 dedup_scheduler_thread (进程上下文, 允许 wait 反压)
// 分支:
//   HIT 主占位(is_req=1)   -> 挂 Buffer 链尾, 任务挂起(dedup_suspended)
//   HIT 预取占位(is_req=0) -> 分配 Buffer, 升级 is_req=1, 任务挂起
//   MISS                   -> 插主占位 + D 个预取占位, 返回 MISS -> PTW
//   降级(insert 失败)       -> 释放 Buffer, dedup_bypass, 返回 MISS -> PTW
// 设置 last_dedup_atomic_ops_ (原子段个数, 供调度线程计时)
// ============================================================
CacheMessage CacheSubsystem::execute_dedup_request(const CacheMessage& req) {
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_RESPONSE;
    resp.task_id = req.task_id;
    resp.iova = req.iova;
    resp.gscid = req.gscid;
    resp.pscid = req.pscid;
    resp.stage = req.stage;
    resp.prefetch_enabled = req.prefetch_enabled;
    resp.prefetch_depth = req.prefetch_depth;

    last_dedup_atomic_ops_ = 1;  // 默认 1 个原子段

    uint64_t page_iova = req.iova & ~0xFFFULL;
    DedupCacheLine* line = dedup_cache_->lookup(req.gscid, req.pscid, page_iova);

    if (line != nullptr) {
        // ============ HIT 占位CL ============
        if (line->is_req) {
            // -------- 分支1: 主占位CL (is_req=1) 挂 Buffer 链尾 --------
            uint16_t head_idx = line->head_index;
            uint16_t tail_idx = dedup_buffer_->entries[head_idx].tail_index;

            uint16_t new_idx = dedup_buffer_->allocate_entry();
            if (new_idx == DEDUP_BUFFER_INVALID_IDX) {
                printf("[DEDUP_CACHE_BACKPRESSURE] task_id=%u -> Buffer full (main placeholder HIT), blocking...\n", (unsigned)req.task_id);
                fflush(stdout);
                wait(dedup_buffer_->free_event);
                new_idx = dedup_buffer_->allocate_entry();
            }
            if (new_idx != DEDUP_BUFFER_INVALID_IDX) {
                auto& new_entry = dedup_buffer_->entries[new_idx];
                new_entry.gscid = req.gscid;
                new_entry.pscid = req.pscid;
                new_entry.iova = req.iova;
                new_entry.stage = req.stage;
                new_entry.sv48 = req.pt_sv48;
                new_entry.gstage_x4 = req.pt_gstage_x4;
                new_entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);
                new_entry.next_index = DEDUP_BUFFER_INVALID_IDX;
                new_entry.tail_index = DEDUP_BUFFER_INVALID_IDX;  // 非链头

                dedup_buffer_->entries[tail_idx].next_index = new_idx;
                dedup_buffer_->entries[head_idx].tail_index = new_idx;  // 仅写 Buffer

                resp.dedup_head_index = head_idx;
                resp.dedup_new_index = new_idx;
                printf("[DEDUP_CACHE] task_id=%u -> HIT main placeholder, linked to buffer tail (head=%u, old_tail=%u, new=%u)\n",
                       (unsigned)req.task_id, head_idx, tail_idx, new_idx);
                fflush(stdout);
            }
            resp.hit = true;
            resp.dedup_suspended = true;  // 任务挂入 Buffer, 无需转发
        } else {
            // -------- 分支2: 预取占位CL (is_req=0) 升级为主占位 --------
            uint16_t new_idx = dedup_buffer_->allocate_entry();
            if (new_idx == DEDUP_BUFFER_INVALID_IDX) {
                printf("[DEDUP_CACHE_BACKPRESSURE] task_id=%u -> Buffer full (prefetch placeholder HIT), blocking...\n", (unsigned)req.task_id);
                fflush(stdout);
                wait(dedup_buffer_->free_event);
                new_idx = dedup_buffer_->allocate_entry();
            }
            if (new_idx != DEDUP_BUFFER_INVALID_IDX) {
                auto& entry = dedup_buffer_->entries[new_idx];
                entry.gscid = req.gscid;
                entry.pscid = req.pscid;
                entry.iova = req.iova;
                entry.stage = req.stage;
                entry.sv48 = req.pt_sv48;
                entry.gstage_x4 = req.pt_gstage_x4;
                entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);
                entry.next_index = DEDUP_BUFFER_INVALID_IDX;
                entry.tail_index = new_idx;  // 链头自引用

                // 升级 dedup_cache 该 line 为主占位
                line->head_index = new_idx;
                line->is_req = true;

                resp.dedup_head_index = new_idx;
                resp.dedup_new_index = new_idx;
                printf("[DEDUP_CACHE] task_id=%u -> HIT prefetch placeholder, upgraded to main (new=%u)\n",
                       (unsigned)req.task_id, new_idx);
                fflush(stdout);
            }
            resp.hit = true;
            resp.dedup_suspended = true;
        }
    } else {
        // ============ MISS ============
        uint16_t head_idx = dedup_buffer_->allocate_entry();
        if (head_idx == DEDUP_BUFFER_INVALID_IDX) {
            printf("[DEDUP_CACHE_BACKPRESSURE] task_id=%u -> Buffer full (%u/%u), blocking...\n",
                   (unsigned)req.task_id, dedup_buffer_->get_valid_count(), PT_DEDUP_BUFFER_SIZE);
            fflush(stdout);
            wait(dedup_buffer_->free_event);
            head_idx = dedup_buffer_->allocate_entry();
        }

        if (head_idx != DEDUP_BUFFER_INVALID_IDX) {
            auto& entry = dedup_buffer_->entries[head_idx];
            entry.gscid = req.gscid;
            entry.pscid = req.pscid;
            entry.iova = req.iova;
            entry.stage = req.stage;
            entry.sv48 = req.pt_sv48;
            entry.gstage_x4 = req.pt_gstage_x4;
            entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);
            entry.next_index = DEDUP_BUFFER_INVALID_IDX;
            entry.tail_index = head_idx;  // 单 entry, tail=head

            bool insert_success = dedup_cache_->insert(
                req.gscid, req.pscid, page_iova, head_idx, true /*is_req*/);

            if (insert_success) {
                resp.hit = false;
                resp.dedup_head_index = head_idx;
                resp.prefetch_enabled = req.prefetch_enabled;
                resp.prefetch_depth = req.prefetch_depth;

                // 插入 D 个预取占位CL(页表页边界内)
                if (req.prefetch_enabled && req.prefetch_depth > 0) {
                    uint32_t D = req.prefetch_depth;
                    uint32_t vpn0_in_page = (page_iova >> 12) & 0x1FF;
                    uint32_t max_D = 511 - vpn0_in_page;
                    if (D > max_D) D = max_D;
                    last_dedup_atomic_ops_ = 1 + D;  // 主任务 + D 个预取, 各一个原子段

                    for (uint32_t d = 1; d <= D; d++) {
                        uint64_t prefetch_iova = page_iova + (d * 0x1000ULL);
                        if (dedup_cache_->lookup(req.gscid, req.pscid, prefetch_iova) != nullptr) {
                            continue;  // 已存在占位/常规CL, 跳过
                        }
                        dedup_cache_->insert(req.gscid, req.pscid, prefetch_iova,
                                             0xFFFF, false /*is_req=0 预取占位*/);
                    }
                }
                printf("[DEDUP_CACHE] task_id=%u -> MISS, inserted main placeholder (head=%u, iova=0x%lx), atomic_ops=%u\n",
                       (unsigned)req.task_id, head_idx, (unsigned long)page_iova, last_dedup_atomic_ops_);
                fflush(stdout);
            } else {
                // 降级: dedup_cache set 全部为 is_req=1 受保护 -> 直接转发 PTW
                dedup_buffer_->free_entry(head_idx);
                resp.hit = false;
                resp.dedup_bypass = true;
                resp.prefetch_enabled = false;
                resp.prefetch_depth = 0;
                printf("[DEDUP_CACHE_FALLBACK] task_id=%u -> insert FAILED (all ways protected), fallback to direct PTW\n",
                       (unsigned)req.task_id);
                fflush(stdout);
            }
        } else {
            resp.hit = false;
            resp.dedup_bypass = true;
            printf("[DEDUP_CACHE] task_id=%u -> ERROR: Buffer allocate failed after backpressure wakeup!\n", (unsigned)req.task_id);
            fflush(stdout);
        }
    }

    return resp;
}

// ============================================================
// [重构] execute_dedup_update - PTW 完成后刷新 (由 Monitor 写 dedup_update_fifo 触发)
// 任务1: 查 dedup_cache -> 取 head_index/is_req -> (is_req时)刷 Buffer -> 置 V=0
// 任务2(Buffer 刷新): 通过 dedup_flush_cb_ (iommu_top 实现) 遍历链、算PA、转发
// ============================================================
void CacheSubsystem::execute_dedup_update(const CacheMessage& upd) {
    uint64_t page_iova = upd.iova & ~0xFFFULL;
    DedupCacheLine* line = dedup_cache_->lookup(upd.gscid, upd.pscid, page_iova);
    if (line == nullptr) {
        // 无占位(降级任务/无等待任务的预取): 无操作
        return;
    }
    uint16_t head_index = line->head_index;
    bool is_req = line->is_req;

    if (!is_req) {
        // 预取占位, Buffer 未记录实际任务 -> 直接清除
        dedup_cache_->clear_line(line);
        printf("[DEDUP_UPDATE] iova=0x%lx -> prefetch placeholder cleared (no waiting task)\n", (unsigned long)page_iova);
        fflush(stdout);
        return;
    }

    // 主占位: 先刷 Buffer 链(回调), 完成后再清除 dedup_cache line
    if (dedup_flush_cb_) {
        dedup_flush_cb_(head_index, upd);
    }
    dedup_cache_->clear_line(line);
    printf("[DEDUP_UPDATE] iova=0x%lx -> main placeholder flushed (head=%u) and cleared\n",
           (unsigned long)page_iova, head_index);
    fflush(stdout);
}

CacheMessage CacheSubsystem::execute_walker_request(const CacheMessage& req) {
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_RESPONSE;
    resp.task_id = req.task_id;
    resp.iova = req.iova;  // FIX: 复制IOVA到响应
    WalkerData data;
    uint8_t hit_level = 0;

    if (req.walker_is_s2_lookup) {
        // [S2] S2 Cache查询: 用GPA查询is_s2=1的cacheline
        resp.hit = walker_cache_->lookup_s2(req.gscid, req.pscid, req.iova,
                                            req.walker_sv48,
                                            req.walker_x4_mode,
                                            data, hit_level, resp.latency);
    } else {
        // 普通Walker Cache查询
        resp.hit = walker_cache_->lookup(req.gscid, req.pscid, req.iova,
                                         req.walker_addr_is_va,
                                         req.walker_from_two_stage,
                                         req.walker_sv48,
                                         req.walker_x4_mode,
                                         data, hit_level, resp.latency);
    }
    resp.gscid = req.gscid;
    resp.pscid = req.pscid;
    if (resp.hit) {
        resp.walker_level = hit_level;
        resp.walker_data = data;
    }
    return resp;
}

CacheMessage CacheSubsystem::execute_msi_request(const CacheMessage& req) {
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_RESPONSE;
    resp.task_id = req.task_id;
    MSIPTData data;
    resp.hit = msipt_cache_->lookup_msi(req.device_id, req.msi_index, data, resp.latency);
    if (resp.hit) {
        resp.msi_data = data;
    }
    return resp;
}

CacheMessage CacheSubsystem::execute_dc_update_request(const CacheMessage& req) {
    dc_cache_->fill_dc(req.device_id, req.dc_data);
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_UPDATE_RESPONSE;
    resp.task_id = req.task_id;
    return resp;
}

CacheMessage CacheSubsystem::execute_pc_update_request(const CacheMessage& req) {
    pc_cache_->fill_pc(req.device_id, req.process_id, req.pc_data);
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_UPDATE_RESPONSE;
    resp.task_id = req.task_id;
    return resp;
}

CacheMessage CacheSubsystem::execute_pt_update_request(const CacheMessage& req) {
    // [STAT] 设置阶段追踪器: 更新阶段(UPDATE=1)
    pt_cache_->set_current_phase(1);

    pt_cache_->fill_pt(req.gscid, req.pscid, req.iova, req.stage, req.pt_data, req.from_prefetch);
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_UPDATE_RESPONSE;
    resp.task_id = req.task_id;
    return resp;
}

CacheMessage CacheSubsystem::execute_walker_update_request(const CacheMessage& req) {
    WalkerData ptwc1_data = req.walker_data_ptwc1;
    WalkerData ptwc2_data = req.walker_data_ptwc2;
    WalkerData ptwc3_data = req.walker_data_ptwc3;

    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_UPDATE_RESPONSE;
    resp.task_id = req.task_id;

    if (req.walker_is_s2_lookup) {
        // [S2] S2 Cache更新
        auto result = walker_cache_->update_s2(req.gscid, req.pscid, req.iova,
                                               req.walker_update_kind,
                                               ptwc1_data, ptwc2_data, ptwc3_data);
        resp.latency = result.latency;
    } else {
        // 普通Walker Cache更新
        auto result = walker_cache_->update(req.gscid, req.pscid, req.iova,
                                            req.walker_update_kind,
                                            ptwc1_data, ptwc2_data, ptwc3_data);
        resp.latency = result.latency;
    }
    return resp;
}

CacheMessage CacheSubsystem::execute_msi_update_request(const CacheMessage& req) {
    msipt_cache_->fill_msi(req.device_id, req.msi_index, req.msi_data);
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_UPDATE_RESPONSE;
    resp.task_id = req.task_id;
    return resp;
}

CacheMessage CacheSubsystem::execute_dc_invalidate_request(const CacheMessage& req) {
    sc_time latency = SC_ZERO_TIME;
    if (req.invalidate_mode == CacheInvalidateMode::GLOBAL) {
        CacheMessage resp;
        resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
        resp.task_id = req.task_id;
        resp.affected_entries = dc_cache_->invalidate_global(&latency);
        resp.latency = latency;
        return resp;
    }

    auto contexts = dc_cache_->invalidate_ddt(req.device_id, &latency);

    // DC 失效后，关联 PT/Walker/MSIPT 失效通过 FIFO 传递
    for (auto& ctx : contexts) {
        enqueue_cascade_invalidations(ctx.gscid, ctx.pscid, req.device_id,
                                      true, ctx.pscid != 0, true);
    }

    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = static_cast<uint32_t>(contexts.size());
    resp.latency = latency;
    return resp;
}

CacheMessage CacheSubsystem::execute_pc_invalidate_request(const CacheMessage& req) {
    sc_time latency = SC_ZERO_TIME;
    if (req.invalidate_mode == CacheInvalidateMode::GLOBAL) {
        CacheMessage resp;
        resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
        resp.task_id = req.task_id;
        resp.affected_entries = pc_cache_->invalidate_global(&latency);
        resp.latency = latency;
        return resp;
    }

    auto contexts = pc_cache_->invalidate_pdt(req.device_id, req.process_id,
                                             req.has_process_id, &latency);
    for (auto& ctx : contexts) {
        enqueue_cascade_invalidations(0, ctx.pscid, req.device_id,
                                      false, true, false);
    }

    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = static_cast<uint32_t>(contexts.size());
    resp.latency = latency;
    return resp;
}

CacheMessage CacheSubsystem::execute_pt_invalidate_request(const CacheMessage& req) {
    sc_time latency = SC_ZERO_TIME;
    uint32_t affected = 0;
    if (req.cmd_type == InvalidCmdType::IOTINVAL_GVMA) {
        affected = pt_cache_->invalidate_gvma(req.gscid, req.iova,
                                              req.has_gscid, req.has_iova,
                                              req.invalidate_mode, &latency);
    } else {
        affected = pt_cache_->invalidate_vma(req.gscid, req.pscid, req.iova,
                                             req.has_gscid, req.has_pscid,
                                             req.has_iova, req.invalidate_mode,
                                             &latency);
    }
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = affected;
    resp.latency = latency;
    return resp;
}

CacheMessage CacheSubsystem::execute_walker_invalidate_request(const CacheMessage& req) {
    sc_time latency = SC_ZERO_TIME;
    uint32_t affected = 0;
    if (req.cmd_type == InvalidCmdType::IOTINVAL_GVMA) {
        affected = walker_cache_->invalidate_gvma(req.gscid, req.has_gscid,
                                                  req.invalidate_mode, &latency);
    } else {
        affected = walker_cache_->invalidate_vma(req.gscid, req.pscid, req.iova,
                                                 req.has_gscid, req.has_pscid,
                                                 req.has_iova,
                                                 req.invalidate_mode, &latency);
    }
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = affected;
    resp.latency = latency;
    return resp;
}

CacheMessage CacheSubsystem::execute_msi_invalidate_request(const CacheMessage& req) {
    sc_time latency = SC_ZERO_TIME;
    uint32_t affected = 0;
    if (req.invalidate_mode == CacheInvalidateMode::GLOBAL) {
        affected = msipt_cache_->invalidate_global(&latency);
    } else {
        affected = msipt_cache_->invalidate_by_device(req.device_id, &latency);
    }
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = affected;
    resp.latency = latency;
    return resp;
}

CacheMessage CacheSubsystem::drive_invalidate_and_wait(
    sc_fifo<CacheMessage>& request_fifo,
    sc_fifo<CacheMessage>& response_fifo,
    const CacheMessage& req) {
    push_fifo(request_fifo, req);
    return response_fifo.read();
}

CacheMessage CacheSubsystem::execute_invalidation_pipeline(const CacheMessage& msg) {
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = msg.task_id;
    resp.cmd_type = msg.cmd_type;

    auto accumulate = [&resp](const CacheMessage& partial) {
        resp.affected_entries += partial.affected_entries;
        resp.latency += partial.latency;
    };

    if (msg.cmd_type == InvalidCmdType::GLOBAL_INVAL ||
        msg.invalidate_mode == CacheInvalidateMode::GLOBAL) {
        CacheMessage req = msg;
        req.invalidate_mode = CacheInvalidateMode::GLOBAL;

        req.msg_type = CacheMsgType::DC_INVALIDATE;
        accumulate(drive_invalidate_and_wait(dc_invalidate_fifo,
                                             dc_invalidate_response_fifo, req));
        req.msg_type = CacheMsgType::PC_INVALIDATE;
        accumulate(drive_invalidate_and_wait(pc_invalidate_fifo,
                                             pc_invalidate_response_fifo, req));
        req.msg_type = CacheMsgType::PT_INVALIDATE;
        accumulate(drive_invalidate_and_wait(pt_invalidate_fifo,
                                             pt_invalidate_response_fifo, req));
        req.msg_type = CacheMsgType::WALKER_INVALIDATE;
        accumulate(drive_invalidate_and_wait(walker_invalidate_fifo,
                                             walker_invalidate_response_fifo, req));
        req.msg_type = CacheMsgType::MSI_INVALIDATE;
        accumulate(drive_invalidate_and_wait(msi_invalidate_fifo,
                                             msi_invalidate_response_fifo, req));
        return resp;
    }

    switch (msg.cmd_type) {
        case InvalidCmdType::IODIR_INVAL_DDT: {
            CacheMessage req = msg;
            req.msg_type = CacheMsgType::DC_INVALIDATE;
            req.invalidate_mode = CacheInvalidateMode::PRECISE;
            uint32_t pt_responses = 0;
            uint32_t walker_responses = 0;
            uint32_t msi_responses = 0;
            auto drain_cascade_responses = [&]() {
                CacheMessage partial;
                while (pop_fifo(pt_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    pt_responses++;
                }
                while (pop_fifo(walker_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    walker_responses++;
                }
                while (pop_fifo(msi_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    msi_responses++;
                }
            };

            push_fifo(dc_invalidate_fifo, req);
            while (dc_invalidate_response_fifo.num_available() == 0) {
                drain_cascade_responses();
                wait(clock_period_);
            }
            CacheMessage dc_resp = dc_invalidate_response_fifo.read();
            accumulate(dc_resp);
            while (pt_responses < dc_resp.affected_entries ||
                   walker_responses < dc_resp.affected_entries ||
                   msi_responses < dc_resp.affected_entries) {
                drain_cascade_responses();
                if (pt_responses < dc_resp.affected_entries ||
                    walker_responses < dc_resp.affected_entries ||
                    msi_responses < dc_resp.affected_entries) {
                    wait(clock_period_);
                }
            }
            break;
        }
        case InvalidCmdType::IODIR_INVAL_PDT: {
            CacheMessage req = msg;
            req.msg_type = CacheMsgType::PC_INVALIDATE;
            req.invalidate_mode = msg.has_process_id ?
                CacheInvalidateMode::PRECISE : CacheInvalidateMode::SCAN;
            uint32_t pt_responses = 0;
            uint32_t walker_responses = 0;
            auto drain_cascade_responses = [&]() {
                CacheMessage partial;
                while (pop_fifo(pt_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    pt_responses++;
                }
                while (pop_fifo(walker_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    walker_responses++;
                }
            };

            push_fifo(pc_invalidate_fifo, req);
            while (pc_invalidate_response_fifo.num_available() == 0) {
                drain_cascade_responses();
                wait(clock_period_);
            }
            CacheMessage pc_resp = pc_invalidate_response_fifo.read();
            accumulate(pc_resp);
            while (pt_responses < pc_resp.affected_entries ||
                   walker_responses < pc_resp.affected_entries) {
                drain_cascade_responses();
                if (pt_responses < pc_resp.affected_entries ||
                    walker_responses < pc_resp.affected_entries) {
                    wait(clock_period_);
                }
            }
            break;
        }
        case InvalidCmdType::IOTINVAL_VMA: {
            const bool mode1 = msg.has_gscid && msg.has_pscid && msg.has_iova;
            const bool mode2 = !msg.has_gscid && msg.has_pscid && msg.has_iova;
            CacheMessage pt_req = msg;
            pt_req.msg_type = CacheMsgType::PT_INVALIDATE;
            pt_req.invalidate_mode = (mode1 || mode2) ?
                CacheInvalidateMode::PRECISE : CacheInvalidateMode::SCAN;
            accumulate(drive_invalidate_and_wait(pt_invalidate_fifo,
                                                 pt_invalidate_response_fifo,
                                                 pt_req));

            CacheMessage walker_req = msg;
            walker_req.msg_type = CacheMsgType::WALKER_INVALIDATE;
            walker_req.invalidate_mode = mode1 ?
                CacheInvalidateMode::PRECISE : CacheInvalidateMode::SCAN;
            accumulate(drive_invalidate_and_wait(walker_invalidate_fifo,
                                                 walker_invalidate_response_fifo,
                                                 walker_req));
            break;
        }
        case InvalidCmdType::IOTINVAL_GVMA: {
            CacheMessage pt_req = msg;
            pt_req.msg_type = CacheMsgType::PT_INVALIDATE;
            pt_req.invalidate_mode = CacheInvalidateMode::SCAN;
            accumulate(drive_invalidate_and_wait(pt_invalidate_fifo,
                                                 pt_invalidate_response_fifo,
                                                 pt_req));

            CacheMessage walker_req = msg;
            walker_req.msg_type = CacheMsgType::WALKER_INVALIDATE;
            walker_req.invalidate_mode = CacheInvalidateMode::SCAN;
            accumulate(drive_invalidate_and_wait(walker_invalidate_fifo,
                                                 walker_invalidate_response_fifo,
                                                 walker_req));
            break;
        }
        default:
            break;
    }
    return resp;
}

void CacheSubsystem::enqueue_cascade_invalidations(gscid_t gscid, pscid_t pscid,
                                                   device_id_t device_id,
                                                   bool has_gscid,
                                                   bool has_pscid,
                                                   bool include_msi) {
    // 1. 关联 PT Cache 失效: 按 gscid (+ pscid) 失效
    CacheMessage pt_cascade_msg;
    pt_cascade_msg.msg_type = CacheMsgType::PT_INVALIDATE;
    pt_cascade_msg.cmd_type = InvalidCmdType::IOTINVAL_VMA;
    pt_cascade_msg.gscid = gscid;
    pt_cascade_msg.pscid = pscid;
    pt_cascade_msg.invalidate_mode = CacheInvalidateMode::SCAN;
    pt_cascade_msg.has_gscid = has_gscid;
    pt_cascade_msg.has_pscid = has_pscid;
    pt_cascade_msg.has_iova = false;
    push_fifo(pt_invalidate_fifo, pt_cascade_msg);

    // 2. 关联 Walker Cache 失效: 按 gscid (+ pscid) 失效
    CacheMessage walker_cascade_msg;
    walker_cascade_msg.msg_type = CacheMsgType::WALKER_INVALIDATE;
    walker_cascade_msg.cmd_type = InvalidCmdType::IOTINVAL_VMA;
    walker_cascade_msg.gscid = gscid;
    walker_cascade_msg.pscid = pscid;
    walker_cascade_msg.invalidate_mode = CacheInvalidateMode::SCAN;
    walker_cascade_msg.has_gscid = has_gscid;
    walker_cascade_msg.has_pscid = has_pscid;
    walker_cascade_msg.has_iova = false;
    push_fifo(walker_invalidate_fifo, walker_cascade_msg);

    // 3. 关联 MSIPT Cache 失效: 按 device_id 失效
    if (include_msi) {
        CacheMessage msi_cascade_msg;
        msi_cascade_msg.msg_type = CacheMsgType::MSI_INVALIDATE;
        msi_cascade_msg.device_id = device_id;
        msi_cascade_msg.invalidate_mode = CacheInvalidateMode::SCAN;
        push_fifo(msi_invalidate_fifo, msi_cascade_msg);
    }
}

} // namespace iommu
