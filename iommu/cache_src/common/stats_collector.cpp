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
    }
    for (auto& [name, hist] : histograms_) {
        hist.clear();
    }
    for (auto& [name, hist] : queue_histograms_) {
        hist.clear();
    }
}

} // namespace iommu
