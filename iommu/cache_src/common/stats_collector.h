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
        const double observed_window_ns = last_request_end_ns - first_request_start_ns;
        if (first_request_start_ns >= 0.0 && observed_window_ns > 0.0) {
            return static_cast<double>(request_latency_samples) / (observed_window_ns * 1e-9);
        }
        if (total_request_latency_ns > 0.0) {
            return static_cast<double>(request_latency_samples) / (total_request_latency_ns * 1e-9);
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

    // 记录延迟
    void record_latency(const std::string& cache_name, double latency_ns);
    void record_execution_latency(const std::string& cache_name, double latency_ns);
    void record_queue_latency(const std::string& cache_name, double latency_ns);
    void record_request_latency(const std::string& cache_name, double latency_ns,
                                double start_time_ns, double end_time_ns);

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
