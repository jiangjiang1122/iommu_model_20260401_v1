#include "common/stats_collector.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace iommu {

const CacheStats StatsCollector::empty_stats_ = {};
const std::vector<uint64_t> StatsCollector::empty_histogram_ = {};

StatsCollector::StatsCollector() {}

void StatsCollector::register_cache(const std::string& cache_name) {
    stats_map_[cache_name].name = cache_name;
    histograms_[cache_name] = {};
    queue_histograms_[cache_name] = {};
}

void StatsCollector::record_access(const std::string& cache_name) {
    stats_map_[cache_name].total_accesses++;
}

void StatsCollector::record_hit(const std::string& cache_name) {
    stats_map_[cache_name].hits++;
}

void StatsCollector::record_miss(const std::string& cache_name) {
    stats_map_[cache_name].misses++;
}

void StatsCollector::record_eviction(const std::string& cache_name) {
    stats_map_[cache_name].evictions++;
}

void StatsCollector::record_invalidation(const std::string& cache_name) {
    stats_map_[cache_name].invalidations++;
}

void StatsCollector::record_prefetch_issued(const std::string& cache_name) {
    stats_map_[cache_name].prefetch_issued++;
}

void StatsCollector::record_prefetch_hit(const std::string& cache_name) {
    stats_map_[cache_name].prefetch_hits++;
}

void StatsCollector::record_lookup(const std::string& cache_name) {
    stats_map_[cache_name].lookup_count++;
}

void StatsCollector::record_fill_hit(const std::string& cache_name) {
    stats_map_[cache_name].fill_hit_count++;
}

void StatsCollector::record_fill_invalid(const std::string& cache_name) {
    stats_map_[cache_name].fill_invalid_count++;
}

void StatsCollector::record_fill_replace(const std::string& cache_name) {
    stats_map_[cache_name].fill_replace_count++;
}

void StatsCollector::record_phase_lookup(const std::string& cache_name, int phase) {
    auto& s = stats_map_[cache_name];
    if      (phase == 0) s.req_lookup_count++;
    else if (phase == 1) s.upd_lookup_count++;
    else                 s.mon_lookup_count++;
}

void StatsCollector::record_phase_fill_hit(const std::string& cache_name, int phase) {
    auto& s = stats_map_[cache_name];
    if      (phase == 0) s.req_fill_hit_count++;
    else if (phase == 1) s.upd_fill_hit_count++;
    else                 s.mon_fill_hit_count++;
}

void StatsCollector::record_phase_fill_invalid(const std::string& cache_name, int phase) {
    auto& s = stats_map_[cache_name];
    if      (phase == 0) s.req_fill_inv_count++;
    else if (phase == 1) s.upd_fill_inv_count++;
    else                 s.mon_fill_inv_count++;
}

void StatsCollector::record_phase_fill_replace(const std::string& cache_name, int phase) {
    auto& s = stats_map_[cache_name];
    if      (phase == 0) s.req_fill_repl_count++;
    else if (phase == 1) s.upd_fill_repl_count++;
    else                 s.mon_fill_repl_count++;
}

void StatsCollector::record_latency(const std::string& cache_name, double latency_ns) {
    record_execution_latency(cache_name, latency_ns);
}

void StatsCollector::record_execution_latency(const std::string& cache_name, double latency_ns) {
    auto& stats = stats_map_[cache_name];
    stats.execution_latency_samples++;
    stats.total_execution_latency_ns += latency_ns;

    auto& hist = histograms_[cache_name];
    size_t bin = static_cast<size_t>(latency_ns / bin_width_ns_);
    if (bin >= hist.size()) {
        hist.resize(bin + 1, 0);
    }
    hist[bin]++;
}

void StatsCollector::record_queue_latency(const std::string& cache_name, double latency_ns) {
    auto& stats = stats_map_[cache_name];
    stats.queue_latency_samples++;
    stats.total_queue_latency_ns += latency_ns;

    auto& hist = queue_histograms_[cache_name];
    size_t bin = static_cast<size_t>(latency_ns / bin_width_ns_);
    if (bin >= hist.size()) {
        hist.resize(bin + 1, 0);
    }
    hist[bin]++;
}

void StatsCollector::record_request_latency(const std::string& cache_name, double latency_ns,
                                            double start_time_ns, double end_time_ns) {
    auto& stats = stats_map_[cache_name];
    stats.name = cache_name;
    stats.request_latency_samples++;
    stats.total_request_latency_ns += latency_ns;
    if (stats.first_request_start_ns < 0.0 || start_time_ns < stats.first_request_start_ns) {
        stats.first_request_start_ns = start_time_ns;
    }
    if (end_time_ns > stats.last_request_end_ns) {
        stats.last_request_end_ns = end_time_ns;
    }
}

// [STAT] 记录访问时间戳 (用于PT Cache任务放大系数计算)
void StatsCollector::record_access_timestamp(const std::string& cache_name, double current_time_ns) {
    auto& stats = stats_map_[cache_name];
    if (stats.first_access_time_ns < 0.0 || current_time_ns < stats.first_access_time_ns) {
        stats.first_access_time_ns = current_time_ns;
    }
    if (current_time_ns > stats.last_access_time_ns) {
        stats.last_access_time_ns = current_time_ns;
    }
}

// [STAT] 记录PT Cache阶段入口/出口时间戳
// phase: 0=REQUEST, 1=UPDATE
void StatsCollector::record_pt_phase_timestamp(const std::string& cache_name, int phase,
                                               double start_time_ns, double end_time_ns) {
    auto& stats = stats_map_[cache_name];
    double latency_ns = end_time_ns - start_time_ns;
    if (phase == 0) {
        // REQUEST阶段
        if (stats.req_first_start_ns < 0.0 || start_time_ns < stats.req_first_start_ns) {
            stats.req_first_start_ns = start_time_ns;
        }
        if (end_time_ns > stats.req_last_end_ns) {
            stats.req_last_end_ns = end_time_ns;
        }
        stats.req_task_count++;
        // [FIX] 单独统计REQUEST阶段的任务墙钟时间
        stats.req_total_latency_ns += latency_ns;
        stats.req_latency_samples++;
    } else if (phase == 1) {
        // UPDATE阶段
        if (stats.upd_first_start_ns < 0.0 || start_time_ns < stats.upd_first_start_ns) {
            stats.upd_first_start_ns = start_time_ns;
        }
        if (end_time_ns > stats.upd_last_end_ns) {
            stats.upd_last_end_ns = end_time_ns;
        }
        stats.upd_task_count++;
        // [FIX] 单独统计UPDATE阶段的任务墙钟时间
        stats.upd_total_latency_ns += latency_ns;
        stats.upd_latency_samples++;
    }
}

void StatsCollector::accumulate_phase_wait(const std::string& cache_name, int phase, double wait_ns) {
    auto& stats = stats_map_[cache_name];
    if (phase == 0) {
        stats.req_wait_ns += wait_ns;
    } else if (phase == 1) {
        stats.upd_wait_ns += wait_ns;
    }
}

void StatsCollector::accumulate_phase_task_wait(const std::string& cache_name, int phase, double wait_ns) {
    auto& stats = stats_map_[cache_name];
    if (phase == 0) {
        stats.req_task_wait_ns += wait_ns;
    } else if (phase == 1) {
        stats.upd_task_wait_ns += wait_ns;
    }
}

const CacheStats& StatsCollector::get_stats(const std::string& cache_name) const {
    auto it = stats_map_.find(cache_name);
    if (it != stats_map_.end()) return it->second;
    return empty_stats_;
}

const std::vector<uint64_t>& StatsCollector::get_histogram(const std::string& cache_name) const {
    auto it = histograms_.find(cache_name);
    if (it != histograms_.end()) return it->second;
    return empty_histogram_;
}

const std::vector<uint64_t>& StatsCollector::get_queue_histogram(const std::string& cache_name) const {
    auto it = queue_histograms_.find(cache_name);
    if (it != queue_histograms_.end()) return it->second;
    return empty_histogram_;
}

void StatsCollector::print_summary(std::ostream& os) const {
    os << "\n========== IOMMU Cache Statistics Summary ==========\n";
    os << std::left << std::setw(16) << "Cache"
       << std::right
       << std::setw(12) << "Accesses"
       << std::setw(12) << "Hits"
       << std::setw(12) << "Misses"
       << std::setw(10) << "HitRate"
       << std::setw(10) << "Evicts"
       << std::setw(10) << "Invals"
       << std::setw(10) << "PF_Issue"
       << std::setw(10) << "PF_Hit"
       << std::setw(10) << "PFRate"
       << std::setw(12) << "ExecAvg"
       << std::setw(12) << "QueueAvg"
       << std::setw(12) << "TotalAvg"
       << std::setw(12) << "Reqs"
       << std::setw(14) << "IOPS"
       << "\n";
    os << std::string(184, '-') << "\n";

    for (auto& [name, st] : stats_map_) {
        os << std::left << std::setw(16) << name
           << std::right
           << std::setw(12) << st.total_accesses
           << std::setw(12) << st.hits
           << std::setw(12) << st.misses
           << std::setw(10) << std::fixed << std::setprecision(4) << st.hit_rate()
           << std::setw(10) << st.evictions
           << std::setw(10) << st.invalidations
           << std::setw(10) << st.prefetch_issued
           << std::setw(10) << st.prefetch_hits
           << std::setw(10) << st.prefetch_accuracy()
           << std::setw(12) << st.avg_execution_latency_ns()
           << std::setw(12) << st.avg_queue_latency_ns()
           << std::setw(12) << st.avg_request_latency_ns()
           << std::setw(12) << st.request_latency_samples
           << std::setw(14) << st.iops()
           << "\n";
    }
    os << "====================================================\n";

    // 打印ExecAvg/QueueAvg的精确样本数
    os << "\n========== Latency Sample Counts ==========\n";
    os << std::left << std::setw(16) << "Cache"
       << std::right
       << std::setw(16) << "ExecSamples"
       << std::setw(16) << "QueueSamples"
       << std::setw(16) << "ReqSamples"
       << "\n";
    os << std::string(64, '-') << "\n";
    for (auto& [name, st] : stats_map_) {
        os << std::left << std::setw(16) << name
           << std::right
           << std::setw(16) << st.execution_latency_samples
           << std::setw(16) << st.queue_latency_samples
           << std::setw(16) << st.request_latency_samples
           << "\n";
    }
    os << "=============================================\n";

    // 打印4种RAM访问路径统计
    os << "\n========== RAM Access Path Breakdown ==========\n";
    os << std::left << std::setw(16) << "Cache"
       << std::right
       << std::setw(12) << "Lookup"
       << std::setw(12) << "FillHit"
       << std::setw(12) << "FillInv"
       << std::setw(12) << "FillRepl"
       << std::setw(14) << "TotalRAM"
       << std::setw(14) << "ReadSet"
       << std::setw(14) << "WriteWay"
       << "\n";
    os << std::string(106, '-') << "\n";
    for (auto& [name, st] : stats_map_) {
        uint64_t total_ram = st.lookup_count + st.fill_hit_count + st.fill_invalid_count + st.fill_replace_count;
        uint64_t read_set_total = total_ram;  // 每次RAM访问都有1次read_set
        uint64_t write_way_total = st.fill_hit_count + st.fill_invalid_count + st.fill_replace_count;
        os << std::left << std::setw(16) << name
           << std::right
           << std::setw(12) << st.lookup_count
           << std::setw(12) << st.fill_hit_count
           << std::setw(12) << st.fill_invalid_count
           << std::setw(12) << st.fill_replace_count
           << std::setw(14) << total_ram
           << std::setw(14) << read_set_total
           << std::setw(14) << write_way_total
           << "\n";
    }
    os << "=================================================\n";

    // 打印按阶段分类的RAM访问路径统计
    auto pt_it = stats_map_.find("pt_cache");
    if (pt_it != stats_map_.end()) {
        const auto& pt = pt_it->second;
        os << "\n========== PT Cache Access by Phase =========="
           << "\n  REQUEST phase (execute_pt_request):"
           << "\n    Lookup       = " << pt.req_lookup_count
           << "\n    FillHit      = " << pt.req_fill_hit_count  << "  (update_placeholder)"
           << "\n    FillInv      = " << pt.req_fill_inv_count  << "  (insert_placeholder)"
           << "\n    FillRepl     = " << pt.req_fill_repl_count
           << "\n    Subtotal     = "
           << (pt.req_lookup_count + pt.req_fill_hit_count +
               pt.req_fill_inv_count + pt.req_fill_repl_count)
           << "\n  UPDATE phase (execute_pt_update_request):"
           << "\n    Lookup       = " << pt.upd_lookup_count
           << "\n    FillHit      = " << pt.upd_fill_hit_count  << "  (fill_pt)"
           << "\n    FillInv      = " << pt.upd_fill_inv_count
           << "\n    FillRepl     = " << pt.upd_fill_repl_count
           << "\n    Subtotal     = "
           << (pt.upd_lookup_count + pt.upd_fill_hit_count +
               pt.upd_fill_inv_count + pt.upd_fill_repl_count)
           << "\n  PTW_MONITOR phase (chain_head collection):"
           << "\n    Lookup       = " << pt.mon_lookup_count
           << "\n    FillHit      = " << pt.mon_fill_hit_count
           << "\n    FillInv      = " << pt.mon_fill_inv_count
           << "\n    FillRepl     = " << pt.mon_fill_repl_count
           << "\n    Subtotal     = "
           << (pt.mon_lookup_count + pt.mon_fill_hit_count +
               pt.mon_fill_inv_count + pt.mon_fill_repl_count)
           << "\n=============================================\n";

        // [STAT] PT Cache时间戳与任务放大系数
        if (pt.first_access_time_ns >= 0.0 && pt.last_access_time_ns > pt.first_access_time_ns) {
            double window_ns = pt.last_access_time_ns - pt.first_access_time_ns;
            uint64_t total_ram_accesses = pt.lookup_count + pt.fill_hit_count + 
                                          pt.fill_invalid_count + pt.fill_replace_count;
            uint64_t original_tasks = pt.request_latency_samples;  // 原始请求数
            double task_amplification = original_tasks > 0 ? 
                static_cast<double>(total_ram_accesses) / original_tasks : 0.0;
            
            os << "\n========== PT Cache Timing & Amplification =========="
               << "\n  First access time:  " << pt.first_access_time_ns << " ns"
               << "\n  Last access time:   " << pt.last_access_time_ns << " ns"
               << "\n  Access window:      " << window_ns << " ns"
               << "\n  Total RAM accesses: " << total_ram_accesses
               << "\n  Original tasks:     " << original_tasks
               << "\n  Task amplification: " << std::fixed << std::setprecision(3) 
               << task_amplification << "x  (RAM accesses / original tasks)"
               << "\n=====================================================\n";
        }

        // [STAT] PT Cache累计延时精确值 (用于日志验证)
        os << "\n========== PT Cache Cumulative Latency (Raw) =========="
           << "\n  [Queue Latency]"
           << "\n    total_queue_latency_ns:    " << std::fixed << std::setprecision(4) << pt.total_queue_latency_ns << " ns"
           << "\n    queue_latency_samples:     " << pt.queue_latency_samples
           << "\n    QueueAvg:                  " << std::fixed << std::setprecision(4) << pt.avg_queue_latency_ns() << " ns"
           << "\n  [REQUEST Phase Task Latency - FIXED]"
           << "\n    req_total_latency_ns:      " << std::fixed << std::setprecision(4) << pt.req_total_latency_ns << " ns"
           << "\n    req_latency_samples:       " << pt.req_latency_samples
           << "\n    ReqAvg:                    " << std::fixed << std::setprecision(4) << (pt.req_latency_samples > 0 ? pt.req_total_latency_ns / pt.req_latency_samples : 0.0) << " ns"
           << "\n    req_first_start_ns:        " << std::fixed << std::setprecision(4) << pt.req_first_start_ns << " ns"
           << "\n    req_last_end_ns:           " << std::fixed << std::setprecision(4) << pt.req_last_end_ns << " ns"
           << "\n    req_window_ns:             " << std::fixed << std::setprecision(4) << (pt.req_last_end_ns - pt.req_first_start_ns) << " ns"
           << "\n  [UPDATE Phase Task Latency - FIXED]"
           << "\n    upd_total_latency_ns:      " << std::fixed << std::setprecision(4) << pt.upd_total_latency_ns << " ns"
           << "\n    upd_latency_samples:       " << pt.upd_latency_samples
           << "\n    UpdAvg:                    " << std::fixed << std::setprecision(4) << (pt.upd_latency_samples > 0 ? pt.upd_total_latency_ns / pt.upd_latency_samples : 0.0) << " ns"
           << "\n    upd_first_start_ns:        " << std::fixed << std::setprecision(4) << pt.upd_first_start_ns << " ns"
           << "\n    upd_last_end_ns:           " << std::fixed << std::setprecision(4) << pt.upd_last_end_ns << " ns"
           << "\n    upd_window_ns:             " << std::fixed << std::setprecision(4) << (pt.upd_last_end_ns - pt.upd_first_start_ns) << " ns"
           << "\n  [Legacy - Mixed REQUEST+UPDATE, DO NOT USE]"
           << "\n    total_request_latency_ns:  " << std::fixed << std::setprecision(4) << pt.total_request_latency_ns << " ns (MIXED!)"
           << "\n    request_latency_samples:   " << pt.request_latency_samples
           << "\n  [Execution Latency]"
           << "\n    total_execution_latency_ns:" << std::fixed << std::setprecision(4) << pt.total_execution_latency_ns << " ns"
           << "\n    execution_latency_samples: " << pt.execution_latency_samples
           << "\n    ExecAvg:                   " << std::fixed << std::setprecision(4) << pt.avg_execution_latency_ns() << " ns"
           << "\n  [Consistency Check]"
           << "\n    current_sc_time_stamp:     " << std::fixed << std::setprecision(4) << (sc_core::sc_time_stamp().to_seconds() * 1e9) << " ns"
           << "\n    req_sum_vs_req_window:     " << (pt.req_total_latency_ns > (pt.req_last_end_ns - pt.req_first_start_ns) ? "EXCEEDS!" : "OK")
           << "\n    req_sum_vs_simtime:        " << (pt.req_total_latency_ns > (sc_core::sc_time_stamp().to_seconds() * 1e9) ? "EXCEEDS!" : "OK")
           << "\n=========================================================\n";

        // [STAT] PT Cache REQUEST/UPDATE阶段入口/出口时间戳
        {
            os << "\n========== PT Cache Phase Timestamps ==========";
            
            // REQUEST阶段
            if (pt.req_first_start_ns >= 0.0 && pt.req_last_end_ns > pt.req_first_start_ns) {
                double req_window = pt.req_last_end_ns - pt.req_first_start_ns;
                os << "\n  [REQUEST Phase]"
                   << "\n    First start:  " << pt.req_first_start_ns << " ns"
                   << "\n    Last end:     " << pt.req_last_end_ns << " ns"
                   << "\n    Window:       " << req_window << " ns"
                   << "\n    Task count:   " << pt.req_task_count;
            }
            
            // UPDATE阶段
            if (pt.upd_first_start_ns >= 0.0 && pt.upd_last_end_ns > pt.upd_first_start_ns) {
                double upd_window = pt.upd_last_end_ns - pt.upd_first_start_ns;
                os << "\n  [UPDATE Phase]"
                   << "\n    First start:  " << pt.upd_first_start_ns << " ns"
                   << "\n    Last end:     " << pt.upd_last_end_ns << " ns"
                   << "\n    Window:       " << upd_window << " ns"
                   << "\n    Task count:   " << pt.upd_task_count;
            }
            
            // 总体窗口: 从REQUEST首次进入到最后一次响应退出
            double overall_first = pt.req_first_start_ns;
            double overall_last = pt.req_last_end_ns;
            if (pt.upd_first_start_ns >= 0.0 && pt.upd_first_start_ns < overall_first) {
                overall_first = pt.upd_first_start_ns;
            }
            if (pt.upd_last_end_ns > overall_last) {
                overall_last = pt.upd_last_end_ns;
            }
            if (overall_first >= 0.0 && overall_last > overall_first) {
                double overall_window = overall_last - overall_first;
                os << "\n  [Overall Window]"
                   << "\n    First start:  " << overall_first << " ns"
                   << "\n    Last end:     " << overall_last << " ns"
                   << "\n    Window:       " << overall_window << " ns";
            }
            
            // [NEW] 任务级等待/忙碌时间统计 (基于单线程调度器)
            // FIFO等待时间 = 写入FIFO到被调度器读出的时间
            // 执行时间 = 读出到执行完成的时间 (req_total_latency_ns)
            // 总时间 = FIFO等待时间 + 执行时间
            
            double req_fifo_wait_sum = pt.req_task_wait_ns;
            double req_exec_sum      = pt.req_total_latency_ns;
            double req_lifecycle_sum = req_fifo_wait_sum + req_exec_sum;
            uint64_t req_cnt = pt.req_latency_samples;
            
            double upd_fifo_wait_sum = pt.upd_task_wait_ns;
            double upd_exec_sum      = pt.upd_total_latency_ns;
            double upd_lifecycle_sum = upd_fifo_wait_sum + upd_exec_sum;
            uint64_t upd_cnt = pt.upd_latency_samples;
            
            double total_execute   = req_exec_sum + upd_exec_sum;
            double total_fifo_wait = req_fifo_wait_sum + upd_fifo_wait_sum;
            double total_lifecycle = total_execute + total_fifo_wait;
            uint64_t total_tasks   = req_cnt + upd_cnt;
            
            // 计算overall window用于IOPS (复用上方已计算的overall_first/overall_last)
            double iops_window_ns = (overall_first >= 0.0 && overall_last > overall_first) ?
                                    (overall_last - overall_first) : 1.0;
            
            os << "\n\n  [REQUEST Phase - FIFO Wait / Execute Breakdown]"
               << "\n    FIFO wait time (sum):     " << std::fixed << std::setprecision(4) << req_fifo_wait_sum << " ns"
               << "\n    Execute time (sum):       " << req_exec_sum << " ns"
               << "\n    Total lifecycle:          " << req_lifecycle_sum << " ns"
               << "\n    FIFO wait ratio:          " << (req_lifecycle_sum > 0 ? (req_fifo_wait_sum / req_lifecycle_sum * 100.0) : 0.0) << " %"
               << "\n    Avg FIFO wait per task:   " << (req_cnt > 0 ? req_fifo_wait_sum / req_cnt : 0.0) << " ns"
               << "\n    Avg execute per task:     " << (req_cnt > 0 ? req_exec_sum / req_cnt : 0.0) << " ns"
               << "\n    Avg lifecycle per task:   " << (req_cnt > 0 ? req_lifecycle_sum / req_cnt : 0.0) << " ns";
            
            os << "\n\n  [UPDATE Phase - FIFO Wait / Execute Breakdown]"
               << "\n    FIFO wait time (sum):     " << std::fixed << std::setprecision(4) << upd_fifo_wait_sum << " ns"
               << "\n    Execute time (sum):       " << upd_exec_sum << " ns"
               << "\n    Total lifecycle:          " << upd_lifecycle_sum << " ns"
               << "\n    FIFO wait ratio:          " << (upd_lifecycle_sum > 0 ? (upd_fifo_wait_sum / upd_lifecycle_sum * 100.0) : 0.0) << " %"
               << "\n    Avg FIFO wait per task:   " << (upd_cnt > 0 ? upd_fifo_wait_sum / upd_cnt : 0.0) << " ns"
               << "\n    Avg execute per task:     " << (upd_cnt > 0 ? upd_exec_sum / upd_cnt : 0.0) << " ns"
               << "\n    Avg lifecycle per task:   " << (upd_cnt > 0 ? upd_lifecycle_sum / upd_cnt : 0.0) << " ns";
            
            os << "\n\n  [Combined Task Summary]"
               << "\n    Total tasks:        " << total_tasks
               << "\n    REQUEST execute:    " << std::fixed << std::setprecision(4) << req_exec_sum << " ns"
               << "\n    UPDATE  execute:    " << upd_exec_sum << " ns"
               << "\n    Total execute:      " << total_execute << " ns"
               << "\n    REQUEST FIFO wait:  " << req_fifo_wait_sum << " ns"
               << "\n    UPDATE  FIFO wait:  " << upd_fifo_wait_sum << " ns"
               << "\n    Total FIFO wait:    " << total_fifo_wait << " ns"
               << "\n    Total lifecycle:    " << total_lifecycle << " ns"
               << "\n    Avg execute/task:   " << (total_tasks > 0 ? total_execute / total_tasks : 0.0) << " ns"
               << "\n    Avg FIFO wait/task: " << (total_tasks > 0 ? total_fifo_wait / total_tasks : 0.0) << " ns"
               << "\n    Avg lifecycle/task: " << (total_tasks > 0 ? total_lifecycle / total_tasks : 0.0) << " ns";
            
            os << "\n\n  [PT Cache Performance Summary]"
               << "\n    Overall window:       " << std::fixed << std::setprecision(0) << iops_window_ns << " ns"
               << "\n    Total tasks:          " << total_tasks << "  (REQUEST=" << req_cnt << ", UPDATE=" << upd_cnt << ")"
               << "\n    PT Cache IOPS:        " << std::fixed << std::setprecision(2) 
                                                  << (total_tasks / (iops_window_ns * 1e-9) / 1e6) << " M tasks/s"
               << "\n    RAM port util:        " << std::fixed << std::setprecision(2) 
                                                  << (total_execute / iops_window_ns * 100.0) << " %"
               << "\n    RAM port idle:        " << std::fixed << std::setprecision(2) 
                                                  << (iops_window_ns - total_execute) << " ns"
               << "\n    Task amplification:   " << std::fixed << std::setprecision(3) 
                                                  << (static_cast<double>(pt.lookup_count + pt.fill_hit_count + pt.fill_invalid_count + pt.fill_replace_count) / (total_tasks > 0 ? total_tasks : 1)) << "x RAM accesses/task";
            
            os << "\n=================================================\n";
        }
    }
}

void StatsCollector::set_output_file(const std::string& path) {
    output_file_ = path;
    if (output_stream_.is_open()) {
        output_stream_.close();
    }
    output_stream_.open(path);
    if (!output_stream_.is_open()) {
        std::cerr << "Warning: Cannot open output file: " << path << "\n";
    }
}

void StatsCollector::write_log(const std::string& msg) {
    if (output_stream_.is_open()) {
        output_stream_ << msg << "\n";
        output_stream_.flush();
    }
}

void StatsCollector::report() {
    if (!output_stream_.is_open()) {
        std::cerr << "Warning: output file not open. Call set_output_file() first.\n";
        return;
    }
    print_summary(output_stream_);
    if (latency_histogram_enabled_) {
        output_stream_ << "\n";
        write_histogram_report(output_stream_);
    }
    output_stream_ << "\n========== End of Report ==========\n";
    output_stream_.flush();
}

void StatsCollector::write_histogram_report(std::ostream& os) const {
    os << "---------- Execution Latency Histograms ----------\n";
    for (auto& [name, hist] : histograms_) {
        if (hist.empty()) continue;
        os << "\n[" << name << "] (bin_width=" << bin_width_ns_ << " ns)\n";
        os << std::left << std::setw(16) << "bin_start_ns" << std::setw(12) << "count" << "\n";
        os << std::string(28, '-') << "\n";
        for (size_t i = 0; i < hist.size(); ++i) {
            if (hist[i] > 0) {
                os << std::left << std::setw(16) << (i * bin_width_ns_)
                   << std::setw(12) << hist[i] << "\n";
            }
        }
    }

    os << "\n---------- Queue Latency Histograms ----------\n";
    for (auto& [name, hist] : queue_histograms_) {
        if (hist.empty()) continue;
        os << "\n[" << name << "] (bin_width=" << bin_width_ns_ << " ns)\n";
        os << std::left << std::setw(16) << "bin_start_ns" << std::setw(12) << "count" << "\n";
        os << std::string(28, '-') << "\n";
        for (size_t i = 0; i < hist.size(); ++i) {
            if (hist[i] > 0) {
                os << std::left << std::setw(16) << (i * bin_width_ns_)
                   << std::setw(12) << hist[i] << "\n";
            }
        }
    }
}

void StatsCollector::reset() {
    for (auto& [name, st] : stats_map_) {
        st.total_accesses = 0;
        st.hits = 0;
        st.misses = 0;
        st.evictions = 0;
        st.invalidations = 0;
        st.prefetch_issued = 0;
        st.prefetch_hits = 0;
        st.execution_latency_samples = 0;
        st.total_execution_latency_ns = 0.0;
        st.queue_latency_samples = 0;
        st.total_queue_latency_ns = 0.0;
        st.request_latency_samples = 0;
        st.total_request_latency_ns = 0.0;
        st.first_request_start_ns = -1.0;
        st.last_request_end_ns = 0.0;
        st.lookup_count = 0; st.fill_hit_count = 0;
        st.fill_invalid_count = 0; st.fill_replace_count = 0;
        st.req_lookup_count = 0; st.req_fill_hit_count = 0;
        st.req_fill_inv_count = 0; st.req_fill_repl_count = 0;
        st.upd_lookup_count = 0; st.upd_fill_hit_count = 0;
        st.upd_fill_inv_count = 0; st.upd_fill_repl_count = 0;
        st.mon_lookup_count = 0; st.mon_fill_hit_count = 0;
        st.mon_fill_inv_count = 0; st.mon_fill_repl_count = 0;
    }
    for (auto& [name, hist] : histograms_) {
        hist.clear();
    }
    for (auto& [name, hist] : queue_histograms_) {
        hist.clear();
    }
}

} // namespace iommu
