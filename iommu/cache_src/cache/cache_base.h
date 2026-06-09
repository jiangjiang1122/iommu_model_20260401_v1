#ifndef IOMMU_CACHE_BASE_H
#define IOMMU_CACHE_BASE_H

#include <systemc.h>
#include "cache/cache_line.h"
#include "replacement/replacement_policy.h"
#include "replacement/plru_policy.h"
#include "replacement/srrip_policy.h"
#include "common/stats_collector.h"
#include "common/types.h"

#include <vector>
#include <array>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace iommu {

// ============================================================
// Cache 统一基类
// 使用 SC_MODULE 建模，提供 TLM-2.0 接口 + 内部功能/性能一体化逻辑
// ============================================================

template <typename TagT, typename DataT>
class CacheBase : public sc_module {
public:
    SC_HAS_PROCESS(CacheBase);

    CacheBase(sc_module_name name, const CacheConfig& cfg,
              StatsCollector& stats, const std::string& cache_name);

    virtual ~CacheBase() = default;

    // ---- 核心接口 (供子系统/验证环境直接调用) ----

    // 查询缓存: 返回是否命中，命中时数据通过 out_data 返回
    bool lookup(const TagT& tag, DataT& out_data, sc_time& latency);

    // 填充缓存: 将数据写入缓存
    void fill(const TagT& tag, const DataT& data, bool from_prefetch = false);

    // 失效操作: 使用谓词函数匹配需要失效的 cache line
    // 返回被失效 line 的 tag 列表 (用于级联失效)
    std::vector<TagT> invalidate_by_predicate(
        std::function<bool(const TagT&)> predicate);

    // 精确失效: 通过 tag 匹配失效
    bool invalidate(const TagT& tag);

    // 获取配置
    const CacheConfig& config() const { return cfg_; }
    const std::string& cache_name() const { return cache_name_; }

    // 获取 clock period
    sc_time clock_period() const { return clock_period_; }
    void set_clock_period(const sc_time& clk) { clock_period_ = clk; }

protected:
    enum class CacheOpType : uint8_t {
        LOOKUP = 0,
        FILL = 1,
        INVALIDATE = 2,
    };

    // ---- 子类必须实现的纯虚方法 ----

    // 散列函数: 根据请求计算 set index
    virtual uint32_t hash_function(const TagT& tag) const = 0;

    // ---- 内部数据结构 ----
    CacheConfig cfg_;
    std::string cache_name_;
    sc_time clock_period_;
    uint32_t num_sets_;
    uint32_t num_ways_;

    // cache_array_[set][way]
    std::vector<std::vector<CacheLine<TagT, DataT>>> cache_array_;

    // 替换算法
    std::unique_ptr<ReplacementPolicy> replacement_;

    // 性能统计
    StatsCollector& stats_;

    // 单口 RAM 仲裁状态：lookup / fill(update) / invalidate 使用 WRR，默认 1:1:1。
    static constexpr std::array<uint32_t, 3> kWrrWeights = {1, 1, 1};
    std::array<uint32_t, 3> pending_ops_ = {0, 0, 0};
    CacheOpType next_grant_ = CacheOpType::LOOKUP;
    std::array<uint32_t, 3> wrr_remaining_ = kWrrWeights;
    bool ram_port_busy_ = false;
    sc_event arbiter_event_;

    // ---- 内部方法 ----

    // 在指定 set 中查找匹配 tag 的 way，返回 -1 表示未命中
    int find_way(uint32_t set, const TagT& tag) const;

    std::vector<TagT> invalidate_by_line_predicate(
        std::function<bool(const TagT&, const DataT&)> predicate,
        sc_time* latency = nullptr);

    uint32_t invalidate_all_entries(sc_time* latency = nullptr);

    template <typename Predicate, typename OnInvalidated>
    uint32_t invalidate_scan_by_line(Predicate&& predicate,
                                     OnInvalidated&& on_invalidated,
                                     sc_time* latency = nullptr);

    template <typename Predicate, typename OnInvalidated>
    uint32_t invalidate_precise_by_line(const TagT& hash_tag,
                                        Predicate&& predicate,
                                        OnInvalidated&& on_invalidated,
                                        sc_time* latency = nullptr);

    bool lookup_with_hash_tag(const TagT& hash_tag, const TagT& compare_tag,
                              DataT& out_data, sc_time& latency);
    void fill_with_hash_tag(const TagT& hash_tag, const TagT& stored_tag,
                            const DataT& data, bool from_prefetch = false);

    // 在指定 set 中查找空闲 way，返回 -1 表示全满
    int find_empty_way(uint32_t set) const;

    sc_time request_ram_port(CacheOpType op);
    void release_ram_port(CacheOpType op);
    bool can_grant(CacheOpType op) const;
    bool has_pending_ops() const;
    void advance_wrr_after_grant(CacheOpType op);
    static constexpr size_t op_index(CacheOpType op) {
        return static_cast<size_t>(op);
    }
    static constexpr CacheOpType next_op(CacheOpType op) {
        return static_cast<CacheOpType>((op_index(op) + 1U) % 3U);
    }

    template <typename Fn>
    auto arbitrate_ram_access(CacheOpType op, Fn&& fn)
        -> std::invoke_result_t<Fn>;

    sc_time cycles_to_time(uint32_t cycles) const {
        return sc_time(cycles * clock_period_.to_seconds() * 1e9, SC_NS);
    }

    sc_time arbiter_stage_latency() const {
        return cycles_to_time(cfg_.arbiter_latency_cycles);
    }

    sc_time execution_latency_with_arbiter(const sc_time& access_latency) const {
        return arbiter_stage_latency() + access_latency;
    }

    sc_time lookup_hit_access_latency() const {
        const uint32_t staged_cycles =
            cfg_.hash_latency_cycles +
            cfg_.read_set_latency_cycles +
            cfg_.compare_latency_cycles;
        return cycles_to_time(staged_cycles);
    }

    sc_time lookup_miss_access_latency() const {
        const uint32_t staged_cycles =
            cfg_.hash_latency_cycles +
            cfg_.read_set_latency_cycles +
            cfg_.compare_latency_cycles;
        return cycles_to_time(staged_cycles);
    }

    sc_time fill_hit_access_latency() const {
        const uint32_t staged_cycles =
            cfg_.hash_latency_cycles +
            cfg_.read_set_latency_cycles +
            cfg_.fill_compute_index_hit_cycles +
            cfg_.write_way_latency_cycles;
        return cycles_to_time(staged_cycles);
    }

    sc_time fill_invalid_way_access_latency() const {
        const uint32_t staged_cycles =
            cfg_.hash_latency_cycles +
            cfg_.read_set_latency_cycles +
            cfg_.fill_compute_index_invalid_cycles +
            cfg_.write_way_latency_cycles;
        return cycles_to_time(staged_cycles);
    }

    sc_time fill_replacement_access_latency() const {
        const uint32_t staged_cycles =
            cfg_.hash_latency_cycles +
            cfg_.read_set_latency_cycles +
            cfg_.fill_compute_index_replacement_cycles +
            cfg_.write_way_latency_cycles;
        return cycles_to_time(staged_cycles);
    }

    sc_time invalidation_set_latency(bool matched) const {
        const uint32_t read_cycles = cfg_.read_set_latency_cycles;
        const uint32_t compare_cycles = cfg_.invalidation_compare_per_way_cycles;
        const uint32_t write_cycles = matched ? cfg_.write_way_latency_cycles : 0;
        return cycles_to_time(read_cycles + compare_cycles + write_cycles);
    }

    sc_time invalidation_global_latency() const {
        return cycles_to_time(1);
    }

    void consume_delay(const sc_time& latency) {
        if (latency == SC_ZERO_TIME) {
            return;
        }
        if (sc_get_current_process_handle().valid()) {
            wait(latency);
        }
    }
};

// ============================================================
// 模板实现
// ============================================================

template <typename TagT, typename DataT>
CacheBase<TagT, DataT>::CacheBase(
    sc_module_name name, const CacheConfig& cfg,
    StatsCollector& stats, const std::string& cache_name)
    : sc_module(name), cfg_(cfg), cache_name_(cache_name),
      num_sets_(cfg.num_sets), num_ways_(cfg.num_ways), stats_(stats)
{
    clock_period_ = sc_time(1.0, SC_NS);  // 默认，由外部设置

    // 初始化缓存阵列
    cache_array_.resize(num_sets_,
        std::vector<CacheLine<TagT, DataT>>(num_ways_));

    // 创建替换算法
    if (cfg.replacement == "srrip") {
        replacement_ = std::make_unique<SRRIPPolicy>(
            num_sets_, num_ways_, cfg.srrip_m_bits);
    } else if (cfg.replacement == "plru" && num_ways_ > 1) {
        replacement_ = std::make_unique<PLRUPolicy>(num_sets_, num_ways_);
    } else if (cfg.replacement == "none" || num_ways_ == 1) {
        // 直接映射：不需要替换算法，使用简单的 pLRU(1-way 退化)
        // 或者不创建
        if (num_ways_ > 1) {
            replacement_ = std::make_unique<PLRUPolicy>(num_sets_, num_ways_);
        }
    } else {
        // 默认 pLRU
        if (num_ways_ > 1) {
            replacement_ = std::make_unique<PLRUPolicy>(num_sets_, num_ways_);
        }
    }

    // 注册统计通道
    stats_.register_cache(cache_name_);
}

template <typename TagT, typename DataT>
bool CacheBase<TagT, DataT>::lookup(const TagT& tag, DataT& out_data, sc_time& latency) {
    return arbitrate_ram_access(CacheOpType::LOOKUP, [&]() {
        uint32_t set = hash_function(tag);
        assert(set < num_sets_);

        stats_.record_access(cache_name_);

        int way = find_way(set, tag);
        if (way >= 0) {
            // Hit
            out_data = cache_array_[set][way].data;
            if (replacement_) replacement_->access(set, way);

            // 检查是否是预取命中
            if (cache_array_[set][way].from_prefetch) {
                stats_.record_prefetch_hit(cache_name_);
                cache_array_[set][way].from_prefetch = false;
            }
            cache_array_[set][way].access_count++;

            stats_.record_hit(cache_name_);
            const sc_time access_latency = lookup_hit_access_latency();
            latency = execution_latency_with_arbiter(access_latency);
            stats_.record_latency(cache_name_, latency.to_seconds() * 1e9);
            consume_delay(access_latency);
            return true;
        }

        // Miss
        stats_.record_miss(cache_name_);
        const sc_time access_latency = lookup_miss_access_latency();
        latency = execution_latency_with_arbiter(access_latency);
        stats_.record_latency(cache_name_, latency.to_seconds() * 1e9);
        consume_delay(access_latency);
        return false;
    });
}

template <typename TagT, typename DataT>
void CacheBase<TagT, DataT>::fill(const TagT& tag, const DataT& data, bool from_prefetch) {
    fill_with_hash_tag(tag, tag, data, from_prefetch);
}

template <typename TagT, typename DataT>
bool CacheBase<TagT, DataT>::lookup_with_hash_tag(const TagT& hash_tag,
                                                  const TagT& compare_tag,
                                                  DataT& out_data,
                                                  sc_time& latency) {
    return arbitrate_ram_access(CacheOpType::LOOKUP, [&]() {
        uint32_t set = hash_function(hash_tag);
        assert(set < num_sets_);

        stats_.record_access(cache_name_);

        int way = find_way(set, compare_tag);
        if (way >= 0) {
            out_data = cache_array_[set][way].data;
            if (replacement_) replacement_->access(set, way);

            if (cache_array_[set][way].from_prefetch) {
                stats_.record_prefetch_hit(cache_name_);
                cache_array_[set][way].from_prefetch = false;
            }
            cache_array_[set][way].access_count++;

            stats_.record_hit(cache_name_);
            const sc_time access_latency = lookup_hit_access_latency();
            latency = execution_latency_with_arbiter(access_latency);
            stats_.record_latency(cache_name_, latency.to_seconds() * 1e9);
            consume_delay(access_latency);
            return true;
        }

        stats_.record_miss(cache_name_);
        const sc_time access_latency = lookup_miss_access_latency();
        latency = execution_latency_with_arbiter(access_latency);
        stats_.record_latency(cache_name_, latency.to_seconds() * 1e9);
        consume_delay(access_latency);
        return false;
    });
}

template <typename TagT, typename DataT>
void CacheBase<TagT, DataT>::fill_with_hash_tag(const TagT& hash_tag,
                                                const TagT& stored_tag,
                                                const DataT& data,
                                                bool from_prefetch) {
    arbitrate_ram_access(CacheOpType::FILL, [&]() {
        uint32_t set = hash_function(hash_tag);
        assert(set < num_sets_);

        // 先检查是否已存在（更新）
        int existing_way = find_way(set, stored_tag);
        if (existing_way >= 0) {
            const sc_time access_latency = fill_hit_access_latency();
            const sc_time latency = execution_latency_with_arbiter(access_latency);
            cache_array_[set][existing_way].data = data;
            cache_array_[set][existing_way].valid = true;
            cache_array_[set][existing_way].from_prefetch = from_prefetch;
            cache_array_[set][existing_way].access_count = 0;
            if (replacement_) replacement_->access(set, existing_way);
            if (from_prefetch) stats_.record_prefetch_issued(cache_name_);
            stats_.record_latency(cache_name_, latency.to_seconds() * 1e9);
            consume_delay(access_latency);
            return;
        }

        // 查找空闲 way
        int empty_way = find_empty_way(set);
        if (empty_way >= 0) {
            const sc_time access_latency = fill_invalid_way_access_latency();
            const sc_time latency = execution_latency_with_arbiter(access_latency);
            cache_array_[set][empty_way].fill(stored_tag, data, from_prefetch);
            if (replacement_) replacement_->access(set, empty_way);
            if (from_prefetch) stats_.record_prefetch_issued(cache_name_);
            stats_.record_latency(cache_name_, latency.to_seconds() * 1e9);
            consume_delay(access_latency);
            return;
        }

        // 需要替换
        const sc_time access_latency = fill_replacement_access_latency();
        const sc_time latency = execution_latency_with_arbiter(access_latency);
        uint32_t victim_way = 0;
        if (replacement_) {
            // [PT Cache去重+预取] 特殊处理: 保护is_req=1的占位CL不被替换
            // 先尝试找到可替换的victim (跳过is_req=1的占位CL)
            bool found_safe_victim = false;
            for (uint32_t try_way = 0; try_way < num_ways_; try_way++) {
                uint32_t candidate_way = replacement_->find_victim(set);
                
                // 检查该way是否为PT Cache的占位CL且is_req=1
                bool is_protected = false;
                if constexpr (std::is_same_v<DataT, PTData>) {
                    if (cache_array_[set][candidate_way].valid && 
                        cache_array_[set][candidate_way].data.reserved.is_ph == 1 &&
                        cache_array_[set][candidate_way].data.reserved.is_req == 1) {
                        is_protected = true;
                        // 这个占位CL有实际任务在等待,不能替换
                        std::cout << "[PT_CACHE_PROTECT] Set " << set << " Way " << candidate_way 
                                  << " is protected (is_req=1 placeholder), trying next..." << std::endl;
                    }
                }
                
                if (!is_protected) {
                    victim_way = candidate_way;
                    found_safe_victim = true;
                    break;
                }
            }
            
            if (!found_safe_victim) {
                // 所有way都是受保护的占位CL或常规CL,无法安全替换
                std::cout << "[PT_CACHE_WARN] Set " << set << " full, no safe victim found! Insertion skipped." << std::endl;
                stats_.record_latency(cache_name_, latency.to_seconds() * 1e9);
                consume_delay(access_latency);
                return;  // 放弃插入,避免数据丢失
            }
        }

        // 统计淘汰
        if (cache_array_[set][victim_way].valid) {
            stats_.record_eviction(cache_name_);
        }

        cache_array_[set][victim_way].fill(stored_tag, data, from_prefetch);
        if (replacement_) {
            // SRRIP 特殊处理: 新插入使用 on_insert
            auto* srrip = dynamic_cast<SRRIPPolicy*>(replacement_.get());
            if (srrip) {
                srrip->on_insert(set, victim_way);
            } else {
                replacement_->access(set, victim_way);
            }
        }
        if (from_prefetch) stats_.record_prefetch_issued(cache_name_);
        stats_.record_latency(cache_name_, latency.to_seconds() * 1e9);
        consume_delay(access_latency);
    });
}

template <typename TagT, typename DataT>
std::vector<TagT> CacheBase<TagT, DataT>::invalidate_by_predicate(
    std::function<bool(const TagT&)> predicate) {
    return invalidate_by_line_predicate(
        [&predicate](const TagT& tag, const DataT&) { return predicate(tag); });
}

template <typename TagT, typename DataT>
std::vector<TagT> CacheBase<TagT, DataT>::invalidate_by_line_predicate(
    std::function<bool(const TagT&, const DataT&)> predicate,
    sc_time* latency) {
    std::vector<TagT> invalidated_tags;
    invalidate_scan_by_line(
        [&predicate](const TagT& tag, const DataT& data) {
            return predicate(tag, data);
        },
        [&invalidated_tags](const TagT& tag, const DataT&) {
            invalidated_tags.push_back(tag);
        },
        latency);
    return invalidated_tags;
}

template <typename TagT, typename DataT>
bool CacheBase<TagT, DataT>::invalidate(const TagT& tag) {
    return invalidate_precise_by_line(
        tag,
        [&tag](const TagT& line_tag, const DataT&) {
            return line_tag == tag;
        },
        [](const TagT&, const DataT&) {}) > 0;
}

template <typename TagT, typename DataT>
uint32_t CacheBase<TagT, DataT>::invalidate_all_entries(sc_time* latency) {
    uint32_t affected = 0;
    sc_time total_latency = SC_ZERO_TIME;

    arbitrate_ram_access(CacheOpType::INVALIDATE, [&]() {
        const sc_time access_latency = invalidation_global_latency();
        const sc_time execution_latency = execution_latency_with_arbiter(access_latency);
        for (uint32_t s = 0; s < num_sets_; ++s) {
            for (uint32_t w = 0; w < num_ways_; ++w) {
                auto& line = cache_array_[s][w];
                if (!line.valid) continue;
                line.invalidate();
                if (replacement_) replacement_->invalidate(s, w);
                stats_.record_invalidation(cache_name_);
                affected++;
            }
        }
        total_latency += execution_latency;
        stats_.record_latency(cache_name_, execution_latency.to_seconds() * 1e9);
        consume_delay(access_latency);
    });

    if (latency) *latency += total_latency;
    return affected;
}

template <typename TagT, typename DataT>
template <typename Predicate, typename OnInvalidated>
uint32_t CacheBase<TagT, DataT>::invalidate_scan_by_line(
    Predicate&& predicate,
    OnInvalidated&& on_invalidated,
    sc_time* latency) {
    uint32_t affected = 0;
    sc_time total_latency = SC_ZERO_TIME;

    for (uint32_t s = 0; s < num_sets_; ++s) {
        arbitrate_ram_access(CacheOpType::INVALIDATE, [&]() {
            bool matched_in_set = false;
            for (uint32_t w = 0; w < num_ways_; ++w) {
                auto& line = cache_array_[s][w];
                if (!line.valid || !predicate(line.tag, line.data)) continue;
                on_invalidated(line.tag, line.data);
                line.invalidate();
                if (replacement_) replacement_->invalidate(s, w);
                stats_.record_invalidation(cache_name_);
                matched_in_set = true;
                affected++;
            }

            const sc_time access_latency = invalidation_set_latency(matched_in_set);
            const sc_time execution_latency = execution_latency_with_arbiter(access_latency);
            total_latency += execution_latency;
            stats_.record_latency(cache_name_, execution_latency.to_seconds() * 1e9);
            consume_delay(access_latency);
        });
    }

    if (latency) *latency += total_latency;
    return affected;
}

template <typename TagT, typename DataT>
template <typename Predicate, typename OnInvalidated>
uint32_t CacheBase<TagT, DataT>::invalidate_precise_by_line(
    const TagT& hash_tag,
    Predicate&& predicate,
    OnInvalidated&& on_invalidated,
    sc_time* latency) {
    uint32_t affected = 0;
    sc_time total_latency = SC_ZERO_TIME;

    arbitrate_ram_access(CacheOpType::INVALIDATE, [&]() {
        uint32_t set = hash_function(hash_tag);
        assert(set < num_sets_);

        bool matched_in_set = false;
        for (uint32_t w = 0; w < num_ways_; ++w) {
            auto& line = cache_array_[set][w];
            if (!line.valid || !predicate(line.tag, line.data)) continue;
            on_invalidated(line.tag, line.data);
            line.invalidate();
            if (replacement_) replacement_->invalidate(set, w);
            stats_.record_invalidation(cache_name_);
            matched_in_set = true;
            affected++;
        }

        const sc_time access_latency = invalidation_set_latency(matched_in_set);
        const sc_time execution_latency = execution_latency_with_arbiter(access_latency);
        total_latency += execution_latency;
        stats_.record_latency(cache_name_, execution_latency.to_seconds() * 1e9);
        consume_delay(access_latency);
    });

    if (latency) *latency += total_latency;
    return affected;
}

template <typename TagT, typename DataT>
int CacheBase<TagT, DataT>::find_way(uint32_t set, const TagT& tag) const {
    for (uint32_t w = 0; w < num_ways_; ++w) {
        if (cache_array_[set][w].valid && cache_array_[set][w].tag == tag) {
            return static_cast<int>(w);
        }
    }
    return -1;
}

template <typename TagT, typename DataT>
int CacheBase<TagT, DataT>::find_empty_way(uint32_t set) const {
    for (uint32_t w = 0; w < num_ways_; ++w) {
        if (!cache_array_[set][w].valid) {
            return static_cast<int>(w);
        }
    }
    return -1;
}

template <typename TagT, typename DataT>
sc_time CacheBase<TagT, DataT>::request_ram_port(CacheOpType op) {
    const sc_time requested_at = sc_time_stamp();
    pending_ops_[op_index(op)]++;

    while (ram_port_busy_ || !can_grant(op)) {
        if (!sc_get_current_process_handle().valid()) {
            // 非 SystemC 进程上下文无法安全 wait；现有单线程直接调用天然串行。
            break;
        }
        wait(arbiter_event_);
    }

    pending_ops_[op_index(op)]--;
    ram_port_busy_ = true;
    return sc_time_stamp() - requested_at;
}

template <typename TagT, typename DataT>
void CacheBase<TagT, DataT>::release_ram_port(CacheOpType op) {
    ram_port_busy_ = false;
    advance_wrr_after_grant(op);
    arbiter_event_.notify(SC_ZERO_TIME);
}

template <typename TagT, typename DataT>
bool CacheBase<TagT, DataT>::can_grant(CacheOpType op) const {
    if (!has_pending_ops()) {
        return true;
    }

    for (size_t offset = 0; offset < pending_ops_.size(); ++offset) {
        CacheOpType current = static_cast<CacheOpType>(
            (op_index(next_grant_) + offset) % pending_ops_.size());
        if (pending_ops_[op_index(current)] == 0) continue;
        if (wrr_remaining_[op_index(current)] == 0) continue;
        return current == op;
    }

    // 正常 release 路径会在额度耗尽时刷新权重；这里兜底避免异常状态饿死请求。
    for (size_t offset = 0; offset < pending_ops_.size(); ++offset) {
        CacheOpType current = static_cast<CacheOpType>(
            (op_index(next_grant_) + offset) % pending_ops_.size());
        if (pending_ops_[op_index(current)] == 0) continue;
        return current == op;
    }
    return true;
}

template <typename TagT, typename DataT>
bool CacheBase<TagT, DataT>::has_pending_ops() const {
    for (uint32_t count : pending_ops_) {
        if (count != 0) return true;
    }
    return false;
}

template <typename TagT, typename DataT>
void CacheBase<TagT, DataT>::advance_wrr_after_grant(CacheOpType op) {
    const size_t granted = op_index(op);
    if (wrr_remaining_[granted] > 0) {
        wrr_remaining_[granted]--;
    }

    if (pending_ops_[granted] != 0 && wrr_remaining_[granted] != 0) {
        next_grant_ = op;
        return;
    }

    if (wrr_remaining_[granted] == 0) {
        wrr_remaining_[granted] = kWrrWeights[granted];
    }

    for (size_t offset = 1; offset <= pending_ops_.size(); ++offset) {
        CacheOpType candidate = static_cast<CacheOpType>(
            (granted + offset) % pending_ops_.size());
        const size_t idx = op_index(candidate);
        if (pending_ops_[idx] == 0) continue;
        if (wrr_remaining_[idx] == 0) {
            wrr_remaining_[idx] = kWrrWeights[idx];
        }
        next_grant_ = candidate;
        return;
    }

    next_grant_ = next_op(op);
    wrr_remaining_ = kWrrWeights;
}

template <typename TagT, typename DataT>
template <typename Fn>
auto CacheBase<TagT, DataT>::arbitrate_ram_access(CacheOpType op, Fn&& fn)
    -> std::invoke_result_t<Fn> {
    using ResultT = std::invoke_result_t<Fn>;

    const sc_time queue_latency = request_ram_port(op);
    stats_.record_queue_latency(cache_name_, queue_latency.to_seconds() * 1e9);
    consume_delay(arbiter_stage_latency());
    try {
        if constexpr (std::is_void_v<ResultT>) {
            std::forward<Fn>(fn)();
            release_ram_port(op);
        } else {
            ResultT result = std::forward<Fn>(fn)();
            release_ram_port(op);
            return result;
        }
    } catch (...) {
        release_ram_port(op);
        throw;
    }
}

} // namespace iommu

#endif // IOMMU_CACHE_BASE_H
