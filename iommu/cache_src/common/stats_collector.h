#ifndef IOMMU_STATS_COLLECTOR_H
#define IOMMU_STATS_COLLECTOR_H

#include <systemc.h>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <fstream>

namespace iommu {

struct CacheStats {
    std::string name;
    uint64_t    total_accesses      = 0;
    uint64_t    hits                = 0;
    uint64_t    misses              = 0;
    uint64_t    evictions           = 0;
    uint64_t    invalidations       = 0;
    uint64_t    prefetch_issued     = 0;
    uint64_t    prefetch_hits       = 0;
    uint64_t    execution_latency_samples = 0;
    double      total_execution_latency_ns = 0.0;
    uint64_t    queue_latency_samples = 0;
    double      total_queue_latency_ns = 0.0;
    uint64_t    request_latency_samples = 0;
    double      total_request_latency_ns = 0.0;
    double      first_request_start_ns = -1.0;
    double      last_request_end_ns = 0.0;
    // [STAT] 4种RAM访问路径计数
    uint64_t    lookup_count        = 0;  // lookup (hit+miss)
    uint64_t    fill_hit_count      = 0;  // fill: 命中更新
    uint64_t    fill_invalid_count  = 0;  // fill: 新条目
    uint64_t    fill_replace_count  = 0;  // fill: 替换
    // [STAT] 按阶段分类: 申请阶段(REQUEST) vs 更新阶段(UPDATE)
    uint64_t    req_lookup_count    = 0;  // 申请阶段 lookup
    uint64_t    req_fill_hit_count  = 0;  // 申请阶段 fill_hit (update_placeholder)
    uint64_t    req_fill_inv_count  = 0;  // 申请阶段 fill_invalid (insert_placeholder)
    uint64_t    req_fill_repl_count = 0;  // 申请阶段 fill_replace
    uint64_t    upd_lookup_count    = 0;  // 更新阶段 lookup
    uint64_t    upd_fill_hit_count  = 0;  // 更新阶段 fill_hit (fill_pt)
    uint64_t    upd_fill_inv_count  = 0;  // 更新阶段 fill_invalid
    uint64_t    upd_fill_repl_count = 0;  // 更新阶段 fill_replace
    // [STAT] PTW Monitor阶段 (phase=2)
    uint64_t    mon_lookup_count    = 0;  // PTW Monitor lookup
    uint64_t    mon_fill_hit_count  = 0;
    uint64_t    mon_fill_inv_count  = 0;
    uint64_t    mon_fill_repl_count = 0;
    // [STAT] PT Cache时间戳统计 (用于计算任务放大系数)
    double      first_access_time_ns = -1.0;  // 首次访问时间
    double      last_access_time_ns  = 0.0;   // 最后访问时间
    // [STAT] PT Cache REQUEST阶段入口/出口时间戳
    double      req_first_start_ns = -1.0;    // REQUEST阶段首次进入时刻
    double      req_last_end_ns    = 0.0;     // REQUEST阶段最后退出时刻
    uint64_t    req_task_count     = 0;       // REQUEST任务数
    double      req_total_latency_ns = 0.0;   // REQUEST任务墙钟时间累加
    uint64_t    req_latency_samples = 0;      // REQUEST任务样本数
    double      req_wait_ns          = 0.0;   // REQUEST阶段RAM端口等待时间累加
    double      req_task_wait_ns     = 0.0;   // REQUEST阶段任务级互斥等待时间累加
    // [STAT] 区间命中率统计 (每1000笔REQUEST)
    static constexpr int NUM_INTERVALS = 4;
    uint64_t    interval_hits[NUM_INTERVALS]   = {0,0,0,0};
    uint64_t    interval_misses[NUM_INTERVALS] = {0,0,0,0};
    // [STAT] PT Cache UPDATE阶段入口/出口时间戳
    double      upd_first_start_ns = -1.0;    // UPDATE阶段首次进入时刻
    double      upd_last_end_ns    = 0.0;     // UPDATE阶段最后退出时刻
    uint64_t    upd_task_count     = 0;       // UPDATE任务数
    double      upd_total_latency_ns = 0.0;   // UPDATE任务墙钟时间累加
    uint64_t    upd_latency_samples = 0;      // UPDATE任务样本数
    double      upd_wait_ns          = 0.0;   // UPDATE阶段RAM端口等待时间累加
    double      upd_task_wait_ns     = 0.0;   // UPDATE阶段任务级互斥等待时间累加

    double hit_rate() const {
        return total_accesses > 0 ? static_cast<double>(hits) / total_accesses : 0.0;
    }
    double miss_rate() const {
        return total_accesses > 0 ? static_cast<double>(misses) / total_accesses : 0.0;
    }
    double prefetch_accuracy() const {
        return prefetch_issued > 0 ? static_cast<double>(prefetch_hits) / prefetch_issued : 0.0;
    }
    double avg_execution_latency_ns() const {
        return execution_latency_samples > 0 ?
            total_execution_latency_ns / execution_latency_samples : 0.0;
    }
    double avg_queue_latency_ns() const {
        return queue_latency_samples > 0 ?
            total_queue_latency_ns / queue_latency_samples : 0.0;
    }
    double avg_request_latency_ns() const {
        return request_latency_samples > 0 ?
            total_request_latency_ns / request_latency_samples : 0.0;
    }
    double iops() const {
        if (request_latency_samples == 0) return 0.0;
        // [FIX] 使用RAM执行时间（不含排队）计算IOPS
        // total_execution_latency_ns = 各RAM操作的纯执行延时之和 = 端口实际忙碌时间
        // total_request_latency_ns 包含排队等待时间，会高估分母、低估IOPS
        // observed_window_ns 包含空闲间隔，也会高估分母、高估IOPS
        if (total_execution_latency_ns > 0.0) {
            return static_cast<double>(request_latency_samples) / (total_execution_latency_ns * 1e-9);
        }
        return 0.0;
    }
};

class StatsCollector {
public:
    StatsCollector();

    // 注册一个缓存通道
    void register_cache(const std::string& cache_name);

    // 记录事件
    void record_access(const std::string& cache_name);
    void record_hit(const std::string& cache_name);
    void record_miss(const std::string& cache_name);
    void record_eviction(const std::string& cache_name);
    void record_invalidation(const std::string& cache_name);
    void record_prefetch_issued(const std::string& cache_name);
    void record_prefetch_hit(const std::string& cache_name);

    // 记录4种RAM访问路径
    void record_lookup(const std::string& cache_name);
    void record_fill_hit(const std::string& cache_name);
    void record_fill_invalid(const std::string& cache_name);
    void record_fill_replace(const std::string& cache_name);

    // 记录按阶段分类的RAM访问路径 (phase: 0=REQUEST, 1=UPDATE)
    void record_phase_lookup(const std::string& cache_name, int phase);
    void record_phase_fill_hit(const std::string& cache_name, int phase);
    void record_phase_fill_invalid(const std::string& cache_name, int phase);
    void record_phase_fill_replace(const std::string& cache_name, int phase);

    // 记录延迟
    void record_latency(const std::string& cache_name, double latency_ns);
    void record_execution_latency(const std::string& cache_name, double latency_ns);
    void record_queue_latency(const std::string& cache_name, double latency_ns);
    void record_request_latency(const std::string& cache_name, double latency_ns,
                                double start_time_ns, double end_time_ns);
    // [STAT] 记录访问时间戳 (用于PT Cache任务放大系数计算)
    void record_access_timestamp(const std::string& cache_name, double current_time_ns);
    // [STAT] 记录PT Cache阶段入口/出口时间戳
    void record_pt_phase_timestamp(const std::string& cache_name, int phase,
                                   double start_time_ns, double end_time_ns);
    // [STAT] 区间命中率记录 (cache_name="pt_cache", req_index=0~3999)
    void record_interval_hit(const std::string& cache_name, int interval_idx);
    void record_interval_miss(const std::string& cache_name, int interval_idx);
    // [STAT] 按阶段累加RAM端口等待时间
    void accumulate_phase_wait(const std::string& cache_name, int phase, double wait_ns);
    // [STAT] 按阶段累加任务级互斥等待时间
    void accumulate_phase_task_wait(const std::string& cache_name, int phase, double wait_ns);

    // 获取统计
    const CacheStats& get_stats(const std::string& cache_name) const;
    std::map<std::string, CacheStats>& all_stats() { return stats_map_; }

    // 延迟直方图
    const std::vector<uint64_t>& get_histogram(const std::string& cache_name) const;
    const std::vector<uint64_t>& get_queue_histogram(const std::string& cache_name) const;

    // 配置
    void set_histogram_bin_width(double bin_width_ns) { bin_width_ns_ = bin_width_ns; }
    void set_latency_histogram_enabled(bool enabled) { latency_histogram_enabled_ = enabled; }
    void set_output_file(const std::string& path);

    void write_log(const std::string& msg);
    bool has_output_file() const { return output_stream_.is_open(); }

    // 输出报告
    void print_summary(std::ostream& os) const;

    // 统一报告：将 summary 和可选 histogram 以文本形式写入输出文件
    void report();

    void reset();

private:
    std::map<std::string, CacheStats> stats_map_;
    std::map<std::string, std::vector<uint64_t>> histograms_;
    std::map<std::string, std::vector<uint64_t>> queue_histograms_;
    double bin_width_ns_ = 1.0;
    bool latency_histogram_enabled_ = true;
    std::string output_file_ = "cache_sim.log";
    std::ofstream output_stream_;

    static const CacheStats empty_stats_;
    static const std::vector<uint64_t> empty_histogram_;

    void write_histogram_report(std::ostream& os) const;
};

} // namespace iommu

#endif // IOMMU_STATS_COLLECTOR_H
