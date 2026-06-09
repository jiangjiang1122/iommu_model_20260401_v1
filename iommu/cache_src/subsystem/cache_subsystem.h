#ifndef IOMMU_CACHE_SUBSYSTEM_H
#define IOMMU_CACHE_SUBSYSTEM_H

#include <systemc.h>

#include "common/types.h"
#include "common/stats_collector.h"
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

    // 级联失效: DC/PC 失效后产生关联失效消息，推入 PT/Walker/MSIPT 的失效 FIFO
    void enqueue_cascade_invalidations(gscid_t gscid, pscid_t pscid,
                                       device_id_t device_id, bool has_gscid,
                                       bool has_pscid, bool include_msi);

    // 获取配置
    const GlobalConfig& config() const { return cfg_; }

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
    void pt_worker_thread();
    void msi_worker_thread();
    void dc_update_worker_thread();
    void pc_update_worker_thread();
    void pt_update_worker_thread();
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
};

} // namespace iommu

#endif // IOMMU_CACHE_SUBSYSTEM_H
