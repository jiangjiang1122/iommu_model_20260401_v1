#ifndef IOMMU_MULTIDEV_TRACE_HH
#define IOMMU_MULTIDEV_TRACE_HH

#include "iommu_perf_params.hh"
#if TEST_CFG_MULTI_DEVICE_SCENE
#include <systemc.h>
#include <tlm.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace iommu_md {
constexpr unsigned MAX_DEVICE_COUNT = 16;
constexpr unsigned MEASUREMENT_EPOCH = 3;
inline double now_ns() { return sc_core::sc_time_stamp().to_seconds() * 1e9; }
inline void require(bool condition, const std::string& text) {
    if (!condition) SC_REPORT_FATAL("MULTIDEV", text.c_str());
}
// 用作用域覆盖取出FIFO后仍在执行的工作，不增加任何仿真延时。
struct BusyGuard {
    unsigned& count;
    explicit BusyGuard(unsigned& c) : count(c) { ++count; }
    ~BusyGuard() { --count; }
    BusyGuard(const BusyGuard&) = delete;
    BusyGuard& operator=(const BusyGuard&) = delete;
};

enum class Point : unsigned { ACCEPT, POP, ADMIT, READY_OUT, OUT_LAUNCH, RESPONSE, RELEASE, COUNT };
struct Request {
    unsigned device = 0, epoch = 0, type = 0, bytes = 0;
    uint64_t sequence = 0, group = 0, iova = 0, expected_pa = 0, result_pa = 0;
    uint32_t task_id = 0;
    bool write = false, rp_done = false, ok = false;
    double ready_ns = 0, grant_ns = 0, rp_response_ns = -1;
    std::array<double, static_cast<unsigned>(Point::COUNT)> time;
    Request() { time.fill(-1); }
    double at(Point p) const { return time[static_cast<unsigned>(p)]; }
};
struct Operation {
    uint32_t task_id;
    unsigned device, epoch;
    std::string source, operation;
    double start_ns, end_ns, queue_ns;
    bool hit;
    int bank, level;
    uint64_t value;
};
struct HolInterval {
    uint32_t task, head;
    unsigned device, head_device, epoch;
    double begin, end;
};

class Trace {
public:
    unsigned devices = 0;
    unsigned input_pending = 0, output_pending = 0, aggregate_pending = 0;
    unsigned req_peq_pending = 0, rsp_peq_pending = 0, active = 0;
    double last_progress_ns = 0;
    uint64_t progress_serial = 0;
    std::map<tlm::tlm_generic_payload*, Request> requests;
    std::map<uint32_t, Request*> tasks;
    std::set<uint32_t> prefetch_ids;
    std::vector<Operation> operations;
    std::vector<HolInterval> hol_intervals;
    std::map<uint32_t, std::pair<uint32_t, double>> open_hol;

    void progress() { last_progress_ns = now_ns(); ++progress_serial; }
    void configure(unsigned n) {
        require(n > 0 && n <= MAX_DEVICE_COUNT, "设备数必须为1..16");
        devices = n;
        progress();
    }
    Request& register_request(tlm::tlm_generic_payload* trans, const Request& value) {
        require(value.device < devices, "请求DID超出配置范围");
        auto result = requests.emplace(trans, value);
        require(result.second, "payload仍在使用，不能重复登记");
        progress();
        return result.first->second;
    }
    Request& request(tlm::tlm_generic_payload* trans) {
        auto it = requests.find(trans);
        require(it != requests.end(), "响应payload没有RP身份记录");
        return it->second;
    }
    Request& task(uint32_t id) {
        auto it = tasks.find(id);
        require(it != tasks.end(), "task_id没有身份记录: " + std::to_string(id));
        return *it->second;
    }
    void accept(tlm::tlm_generic_payload* trans, uint32_t id, unsigned did, uint64_t iova) {
        auto& r = request(trans);
        require(r.device == did && r.iova == iova && !r.task_id, "入口身份或地址不一致");
        require(tasks.emplace(id, &r).second, "全局task_id重复");
        r.task_id = id;
        stamp(id, Point::ACCEPT);
    }
    void add_prefetch(uint32_t id, uint32_t parent, unsigned did) {
        auto& r = task(parent);
        require(r.device == did, "预取设备身份不一致");
        require(tasks.emplace(id, &r).second, "预取task_id重复");
        prefetch_ids.insert(id);
        operation(id, "ptw", "prefetch_spawn", did, now_ns());
    }
    void stamp(uint32_t id, Point point) {
        require(!prefetch_ids.count(id), "预取不能计作RP请求完成");
        auto& r = task(id);
        auto& t = r.time[static_cast<unsigned>(point)];
        require(t < 0, "请求阶段重复: task=" + std::to_string(id));
        t = now_ns();
        progress();
    }
    void response(tlm::tlm_generic_payload& trans, unsigned did) {
        auto& r = request(&trans);
        require(r.device == did && !r.rp_done, "重复响应或响应DID错误");
        require(r.at(Point::RESPONSE) >= 0, "RP响应早于IOMMU完成事件");
        r.rp_done = true;
        r.rp_response_ns = now_ns();
        r.result_pa = trans.get_address();
        r.ok = trans.get_response_status() == tlm::TLM_OK_RESPONSE && r.result_pa == r.expected_pa;
        progress();
        if (!r.ok) {
            std::ostringstream text;
            text << "翻译校验失败 DID=" << did << " epoch=" << r.epoch << " seq=" << r.sequence
                 << " IOVA=0x" << std::hex << r.iova << " expected=0x" << r.expected_pa
                 << " actual=0x" << r.result_pa << " status=" << trans.get_response_string();
            require(false, text.str());
        }
    }
    void operation(uint32_t id, const std::string& source, const std::string& op,
                   unsigned did, double start, bool hit = false, int bank = -1,
                   int level = 0, double queue = 0, uint64_t value = 1) {
        progress();
        if (!id) return; // 配置/失效命令不属于某个RP业务请求。
        auto& r = task(id);
        require(did < devices && did == r.device, "Cache/DDR事件设备归属错误");
        operations.push_back({id, did, r.epoch, source, op, start, now_ns(),
                              std::max(0.0, queue), hit, bank, level, value});
    }
    // 每次写ready状态变化及出口出队时重新结算，区分跨设备写HOL与出口带宽等待。
    void update_hol(const std::map<uint32_t, uint32_t>& blocked) {
        const double now = now_ns();
        for (auto it = open_hol.begin(); it != open_hol.end();) {
            auto current = blocked.find(it->first);
            if (current != blocked.end() && current->second == it->second.first) { ++it; continue; }
            auto& r = task(it->first);
            auto& head = task(it->second.first);
            hol_intervals.push_back({it->first, it->second.first, r.device, head.device,
                                     r.epoch, it->second.second, now});
            it = open_hol.erase(it);
        }
        for (const auto& pair : blocked) open_hol.emplace(pair.first, std::make_pair(pair.second, now));
    }
    bool all_responded() const {
        for (const auto& pair : requests) if (!pair.second.rp_done) return false;
        return true;
    }
    void diagnostic() const {
        std::array<uint64_t, MAX_DEVICE_COUNT> accepted{}, completed{};
        std::array<double, MAX_DEVICE_COUNT> oldest{};
        for (const auto& pair : requests) {
            const auto& r = pair.second;
            if (r.at(Point::ACCEPT) >= 0) ++accepted[r.device];
            if (r.rp_done) ++completed[r.device];
            else oldest[r.device] = std::max(oldest[r.device], now_ns() - r.grant_ns);
        }
        for (unsigned d = 0; d < devices; ++d)
            std::cerr << "[MD_WATCHDOG] DID=" << d << " accepted=" << accepted[d]
                      << " completed=" << completed[d] << " oldest_ns=" << oldest[d] << '\n';
        std::cerr << "[MD_WATCHDOG] input=" << input_pending << " output=" << output_pending
                  << " aggregate=" << aggregate_pending << " req_peq=" << req_peq_pending
                  << " rsp_peq=" << rsp_peq_pending << " active=" << active << '\n';
    }
    static double percentile(std::vector<double> values, double fraction) {
        if (values.empty()) return 0;
        std::sort(values.begin(), values.end());
        const size_t index = static_cast<size_t>(std::ceil(fraction * values.size())) - 1;
        return values[std::min(index, values.size() - 1)];
    }
    // 从事件时间线计算峰值和积分，不能把parser outstanding与端口在途混为一谈。
    static std::pair<unsigned, double> occupancy(std::vector<std::pair<double, int>> events,
                                                double begin, double end) {
        std::sort(events.begin(), events.end());
        int count = 0;
        unsigned peak = 0;
        double integral = 0, last = begin;
        for (size_t i = 0; i < events.size();) {
            const double t = events[i].first;
            integral += count * std::max(0.0, std::min(end, t) - std::max(begin, last));
            int delta = 0;
            do { delta += events[i++].second; } while (i < events.size() && events[i].first == t);
            count += delta;
            require(count >= 0, "在途计数为负");
            if (t >= begin && t <= end) peak = std::max(peak, static_cast<unsigned>(count));
            last = t;
        }
        require(count == 0, "统计导出时仍有未释放请求");
        return {peak, end > begin ? integral / (end - begin) : 0};
    }
    void report(const std::string& prefix) const {
        require(all_responded(), "尚有未完成请求，不能导出成功报告");
        std::ofstream out(prefix + "_requests.csv");
        require(out.good(), "无法创建请求CSV");
        out << "epoch,did,gscid,seq,group,type,write,bytes,task_id,iova,expected_pa,actual_pa,ok,ready_ns,grant_ns,accept_ns,pop_ns,admit_ns,ready_out_ns,out_launch_ns,response_ns,release_ns,rp_response_ns\n";
        out << std::fixed << std::setprecision(3);
        std::vector<const Request*> measured;
        for (const auto& pair : requests) {
            const auto& r = pair.second;
            out << r.epoch << ',' << r.device << ',' << r.device+1 << ',' << r.sequence << ','
                << r.group << ',' << r.type << ',' << r.write << ',' << r.bytes << ',' << r.task_id
                << ',' << r.iova << ',' << r.expected_pa << ',' << r.result_pa << ',' << r.ok
                << ',' << r.ready_ns << ',' << r.grant_ns;
            for (double t : r.time) out << ',' << t;
            out << ',' << r.rp_response_ns << '\n';
            if (r.epoch == MEASUREMENT_EPOCH) measured.push_back(&r);
        }
        require(!measured.empty(), "正式请求为空");
        std::sort(measured.begin(), measured.end(), [](const Request* a, const Request* b) {
            return a->at(Point::RESPONSE) < b->at(Point::RESPONSE);
        });
        double first = measured.front()->grant_ns, last = measured.back()->at(Point::RESPONSE);
        for (auto* r : measured) first = std::min(first, r->grant_ns);
        double t0 = first, t1 = last;
        bool steady = measured.size() >= 100;
        if (steady) {
            t0 = measured[measured.size()/10 - 1]->at(Point::RESPONSE);
            t1 = measured[measured.size()*9/10 - 1]->at(Point::RESPONSE);
            if (t1 <= t0) { steady = false; t0 = first; t1 = last; }
        }
        require(t1 > t0, "吞吐统计窗口为空");
        std::cout << "[MD_WINDOW] mode=" << (steady ? "steady" : "full") << " begin=" << t0 << " end=" << t1 << " ns\n";
        std::ofstream summary(prefix + "_summary.csv"), buckets(prefix + "_buckets.csv");
        require(summary.good() && buckets.good(), "无法创建统计CSV");
        summary << "did,type,completed,window_count,window_mreq_s,avg_port_ns,p50_ns,p95_ns,p99_ns,max_ns,avg_rp_ns,max_grant_gap_ns,max_response_gap_ns\n";
        buckets << "bucket_start_ns,did,completed\n";
        std::array<uint64_t, MAX_DEVICE_COUNT> window_counts{}, data_counts{};
        for (unsigned d = 0; d < devices; ++d) {
            std::vector<std::pair<double,int>> port_events, slot_events;
            for (auto* r : measured) if (r->device == d) {
                port_events.emplace_back(r->at(Point::ACCEPT), 1);
                port_events.emplace_back(r->at(Point::RESPONSE), -1);
                slot_events.emplace_back(r->at(Point::ADMIT), 1);
                slot_events.emplace_back(r->at(Point::RELEASE), -1);
            }
            auto port = occupancy(port_events, first, last);
            auto slot = occupancy(slot_events, first, last);
            std::cout << "[MD_INFLIGHT] did=" << d << " port_peak=" << port.first << " port_avg=" << port.second
                      << " slot_peak=" << slot.first << " slot_avg=" << slot.second << '\n';
            for (unsigned type = 0; type < 5; ++type) {
                std::vector<double> latencies, grants, responses;
                uint64_t count = 0, window = 0;
                double port_sum = 0, rp_sum = 0;
                for (auto* r : measured) if (r->device == d && (type == 4 || r->type == type)) {
                    ++count;
                    const double response = r->at(Point::RESPONSE);
                    grants.push_back(r->grant_ns);
                    responses.push_back(response);
                    if (response > t0 && response <= t1) {
                        ++window;
                        latencies.push_back(response - r->at(Point::ACCEPT));
                        port_sum += latencies.back();
                        rp_sum += response - r->ready_ns;
                    }
                }
                auto gap = [](std::vector<double> v) {
                    std::sort(v.begin(), v.end()); double result = 0;
                    for (size_t i=1;i<v.size();++i) result=std::max(result,v[i]-v[i-1]);
                    return result;
                };
                summary << d << ',' << type << ',' << count << ',' << window << ',' << window*1000.0/(t1-t0)
                        << ',' << (window ? port_sum/window : 0) << ',' << percentile(latencies,.5)
                        << ',' << percentile(latencies,.95) << ',' << percentile(latencies,.99)
                        << ',' << percentile(latencies,1) << ',' << (window ? rp_sum/window : 0)
                        << ',' << gap(grants) << ',' << gap(responses) << '\n';
                if (type == 4) window_counts[d] = window;
                if (type == 0) data_counts[d] = window;
            }
            for (double b=t0;b<t1;b+=1000) {
                uint64_t count=0;
                for(auto* r:measured) if(r->device==d && r->at(Point::RESPONSE)>b && r->at(Point::RESPONSE)<=std::min(b+1000,t1)) ++count;
                buckets << b << ',' << d << ',' << count << '\n';
            }
        }
        auto fairness = [&](const char* name, const auto& counts) {
            double sum=0,squares=0;
            for(unsigned d=0;d<devices;++d) { sum+=counts[d]; squares+=double(counts[d])*counts[d]; }
            double j=squares ? sum*sum/(devices*squares) : 0;
            std::cout << "[MD_FAIRNESS] " << name << " Mreq/s=" << sum*1000/(t1-t0) << " Jain=" << j << '\n';
            for(unsigned d=0;d<devices;++d) {
                double bias=sum ? counts[d]*devices/sum-1 : 0;
                std::cout << "[MD_DEVICE] did=" << d << " kind=" << name << " window_count=" << counts[d]
                          << " Mreq/s=" << counts[d]*1000.0/(t1-t0) << " relative_bias=" << bias
                          << " warning=" << (j<.99 || std::abs(bias)>.05) << '\n';
            }
        };
        fairness("ALL", window_counts); fairness("DATA", data_counts);
        std::ofstream ops(prefix + "_operations.csv"), hol(prefix + "_hol.csv");
        require(ops.good() && hol.good(), "无法创建Cache/HOL CSV");
        ops << "epoch,did,task_id,source,operation,bank,level,hit,value,start_ns,end_ns,queue_ns,in_window\n";
        for(const auto& e:operations) ops << e.epoch << ',' << e.device << ',' << e.task_id << ',' << e.source
            << ',' << e.operation << ',' << e.bank << ',' << e.level << ',' << e.hit << ',' << e.value
            << ',' << e.start_ns << ',' << e.end_ns << ',' << e.queue_ns << ','
            << (e.epoch==MEASUREMENT_EPOCH && e.end_ns>t0 && e.end_ns<=t1) << '\n';
        hol << "epoch,did,head_did,task_id,head_task_id,begin_ns,end_ns,duration_ns\n";
        for(const auto& e:hol_intervals) hol << e.epoch << ',' << e.device << ',' << e.head_device << ','
            << e.task << ',' << e.head << ',' << e.begin << ',' << e.end << ',' << e.end-e.begin << '\n';
        require(out.good() && summary.good() && buckets.good() && ops.good() && hol.good(), "CSV写入失败");
    }
};
} // namespace iommu_md
#endif
#endif
