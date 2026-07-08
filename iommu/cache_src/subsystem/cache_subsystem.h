#ifndef IOMMU_CACHE_SUBSYSTEM_H
#define IOMMU_CACHE_SUBSYSTEM_H

#include <systemc.h>

#include "common/types.h"
#include "common/stats_collector.h"
#include "common/dedup_buffer.h"
#include "cache/dc_cache.h"
#include "cache/pc_cache.h"
#include "cache/msipt_cache.h"
#include "cache/pt_cache.h"
#include "cache/walker_cache.h"

#include <memory>

namespace iommu {

// CacheSubsystem: 顶层模块，管理所有 Cache 实例和失效 pipeline
// 对外通过显式请求/响应队列连接。
class CacheSubsystem : public sc_module {
public:
    enum class TaskTraceLevel {
        OFF,
        BASIC,
        DETAIL
    };

    SC_HAS_PROCESS(CacheSubsystem);

    CacheSubsystem(sc_module_name name, const GlobalConfig& cfg);
    ~CacheSubsystem() override = default;

    // lookup 请求入口：外部模块把查找请求送到对应 cache 的 worker。
    sc_fifo<CacheMessage> dc_request_fifo;
    sc_fifo<CacheMessage> pc_request_fifo;
    sc_fifo<CacheMessage> pt_request_fifo;
    sc_fifo<CacheMessage> walker_request_fifo;
    sc_fifo<CacheMessage> msi_request_fifo;

    // lookup 响应出口：各 cache 独立返回，避免共用一个 depth=1 FIFO 造成串行化。
    sc_fifo<CacheMessage> dc_response_fifo;
    sc_fifo<CacheMessage> pc_response_fifo;
    sc_fifo<CacheMessage> pt_hit_response_fifo;
    sc_fifo<CacheMessage> pt_miss_response_fifo;
    sc_fifo<CacheMessage> walker_response_fifo;
    sc_fifo<CacheMessage> msi_response_fifo;

    // update 请求入口：外部模块发起更新，请求由对应 cache 的 update worker 消费。
    sc_fifo<CacheMessage> dc_update_fifo;
    sc_fifo<CacheMessage> pc_update_fifo;
    sc_fifo<CacheMessage> pt_update_fifo;
    sc_fifo<CacheMessage> walker_update_fifo;
    sc_fifo<CacheMessage> msi_update_fifo;


    // invalidate 请求入口：外部模块把定点失效请求送到对应 cache。
    sc_fifo<CacheMessage> dc_invalidate_fifo;
    sc_fifo<CacheMessage> pc_invalidate_fifo;
    sc_fifo<CacheMessage> pt_invalidate_fifo;
    sc_fifo<CacheMessage> walker_invalidate_fifo;
    sc_fifo<CacheMessage> msi_invalidate_fifo;

    // 全局 invalidation pipeline：走失效控制器的专用请求/响应通道。
    sc_fifo<CacheMessage> invalidation_request_fifo;
    sc_fifo<CacheMessage> invalidation_response_fifo;

    // 获取各组件引用
    DCCache&     dc_cache()     { return *dc_cache_; }
    PCCache&     pc_cache()     { return *pc_cache_; }
    MSIPTCache&  msipt_cache()  { return *msipt_cache_; }
    PTCache&     pt_cache()     { return *pt_cache_; }
    WalkerCache& walker_cache() { return *walker_cache_; }
    StatsCollector& stats()     { return stats_; }
    void set_task_trace_enabled(bool enabled);
    bool task_trace_enabled() const { return task_trace_level_ != TaskTraceLevel::OFF; }
    
    // NEW: PT Cache去重功能接口
    void set_dedup_buffer(DedupBuffer* buffer) { dedup_buffer_ = buffer; }
    DedupBuffer* get_pt_dedup_buffer() { return dedup_buffer_; }
    void set_pt_dedup_enabled(bool enabled) { pt_dedup_enabled_ = enabled; }

    // [STAT] PT Scheduler任务间隔分析报告
    void print_pt_scheduler_gap_report() const;

    // 级联失效: DC/PC 失效后产生关联失效消息，推入 PT/Walker/MSIPT 的失效 FIFO
    void enqueue_cascade_invalidations(gscid_t gscid, pscid_t pscid,
                                       device_id_t device_id, bool has_gscid,
                                       bool has_pscid, bool include_msi);

    // 获取配置
    const GlobalConfig& config() const { return cfg_; }

    // [STAT] 打印32任务组统计报告
    void print_pt_group_report() const;

private:
    GlobalConfig cfg_;
    sc_time clock_period_;
    StatsCollector stats_;
    TaskTraceLevel task_trace_level_ = TaskTraceLevel::OFF;
    bool final_report_written_ = false;

    // 子模块
    std::unique_ptr<DCCache>     dc_cache_;
    std::unique_ptr<PCCache>     pc_cache_;
    std::unique_ptr<MSIPTCache>  msipt_cache_;
    std::unique_ptr<PTCache>     pt_cache_;
    std::unique_ptr<WalkerCache> walker_cache_;

    // Walker Cache 统一轮询调度线程：替代原有的三个独立worker线程，
    // 按 lookup → update → invalidate 轮询调度，避免互锁死锁问题。
    void walker_scheduler_thread();

    // invalidate 响应出口：返回失效是否完成、影响条目数和处理延迟。
    sc_fifo<CacheMessage> dc_invalidate_response_fifo;
    sc_fifo<CacheMessage> pc_invalidate_response_fifo;
    sc_fifo<CacheMessage> pt_invalidate_response_fifo;
    sc_fifo<CacheMessage> walker_invalidate_response_fifo;
    sc_fifo<CacheMessage> msi_invalidate_response_fifo;

    void dc_worker_thread();
    void pc_worker_thread();
    void pt_scheduler_thread();  // [NEW] 合并pt_worker_thread和pt_update_worker_thread
    void msi_worker_thread();
    void dc_update_worker_thread();
    void pc_update_worker_thread();
    void walker_update_worker_thread();
    void msi_update_worker_thread();
    void dc_invalidate_worker_thread();
    void pc_invalidate_worker_thread();
    void pt_invalidate_worker_thread();
    void walker_invalidate_worker_thread();
    void msi_invalidate_worker_thread();
    void invalidation_worker_thread();
    void end_of_simulation() override;
    void process_next_invalidation_request();
    void push_fifo(sc_fifo<CacheMessage>& fifo, const CacheMessage& msg);
    bool pop_fifo(sc_fifo<CacheMessage>& fifo, CacheMessage& msg);
    
    // NEW: Public accessor for PT Cache (用于去重+预取模块)
    PTCache* get_pt_cache() { return pt_cache_.get(); }
    
    CacheMessage execute_dc_request(const CacheMessage& req);
    CacheMessage execute_pc_request(const CacheMessage& req);
    CacheMessage execute_pt_request(const CacheMessage& req);
    CacheMessage execute_walker_request(const CacheMessage& req);
    CacheMessage execute_msi_request(const CacheMessage& req);
    CacheMessage execute_dc_update_request(const CacheMessage& req);
    CacheMessage execute_pc_update_request(const CacheMessage& req);
    CacheMessage execute_pt_update_request(const CacheMessage& req);
    CacheMessage execute_walker_update_request(const CacheMessage& req);
    CacheMessage execute_msi_update_request(const CacheMessage& req);
    CacheMessage execute_dc_invalidate_request(const CacheMessage& req);
    CacheMessage execute_pc_invalidate_request(const CacheMessage& req);
    CacheMessage execute_pt_invalidate_request(const CacheMessage& req);
    CacheMessage execute_walker_invalidate_request(const CacheMessage& req);
    CacheMessage execute_msi_invalidate_request(const CacheMessage& req);
    void record_task_completion(const std::string& cache_name,
                                const sc_time& start_time,
                                const sc_time& end_time);
    void trace_task_event(const char* phase, const std::string& cache_name,
                          const std::string& op_name,
                          const CacheMessage& msg,
                          const sc_time& start_time = SC_ZERO_TIME,
                          const CacheMessage* resp = nullptr);
    CacheMessage drive_invalidate_and_wait(sc_fifo<CacheMessage>& request_fifo,
                                           sc_fifo<CacheMessage>& response_fifo,
                                           const CacheMessage& req);
    CacheMessage execute_invalidation_pipeline(const CacheMessage& cmd);
    
    // NEW: PT Cache去重相关成员
    DedupBuffer* dedup_buffer_ = nullptr;
    bool pt_dedup_enabled_ = false;

    // [STAT] PT Scheduler任务间隔分析 (gap = 当前任务start - 上一任务end)
    double   pt_sched_first_start_ns_ = -1.0;   // 第一笔任务开始时刻
    double   pt_sched_last_end_ns_    = 0.0;    // 最后一笔任务结束时刻
    double   pt_sched_total_gap_ns_   = 0.0;    // 总空闲时间(gap之和)
    double   pt_sched_total_exec_ns_  = 0.0;    // 总执行时间(每笔任务耗时之和)
    uint64_t pt_sched_task_count_     = 0;      // 总处理任务数
    uint64_t pt_sched_gap_count_      = 0;      // 间隔次数(=task_count-1)
    double   pt_sched_max_gap_ns_     = 0.0;    // 最大单次空闲
    double   pt_sched_last_task_end_  = -1.0;   // 上一笔任务结束时刻
    uint64_t pt_sched_req_count_      = 0;      // REQUEST任务数
    uint64_t pt_sched_upd_count_      = 0;      // UPDATE任务数

    // [STAT] 乒乓调度状态
    bool pt_sched_next_is_request_ = true;       // 乒乓标志: true=下一轮优先REQUEST

    // [STAT] 32任务组REQUEST排队/执行延时统计
    static constexpr int PT_GROUP_SIZE = 32;
    double   pt_group_exec_ns_[32]   = {};       // 当前组每个REQUEST的执行延时
    double   pt_group_queue_ns_[32]  = {};       // 当前组每个REQUEST的排队延时
    double   pt_group_e2e_ns_[32]    = {};       // 当前组每个REQUEST的端到端延时
    int      pt_group_upd_count_[32] = {};       // 当前组每个REQUEST间隔的UPDATE数
    int      pt_group_pos_           = 0;        // 当前组已累积REQUEST数(0~31)
    double   pt_group_last_req_end_  = -1.0;     // 上一个REQUEST结束时刻
    double   pt_group_upd_since_last_ = 0.0;     // 自上个REQUEST以来的UPDATE执行时间
    int      pt_group_upd_cnt_since_ = 0;        // 自上个REQUEST以来的UPDATE计数

    // 历史汇总(稳态)
    uint64_t pt_group_total_groups_  = 0;        // 完成的组数
    double   pt_group_sum_exec_[32]  = {};       // 所有组position i的执行延时累加
    double   pt_group_sum_queue_[32] = {};       // 所有组position i的排队延时累加
    double   pt_group_sum_e2e_[32]   = {};       // 所有组position i的端到端延时累加
    int      pt_group_sum_upd_[32]   = {};       // 所有组position i的UPDATE数累加

};

} // namespace iommu

#endif // IOMMU_CACHE_SUBSYSTEM_H
