#ifndef IOMMU_CACHE_SUBSYSTEM_H
#define IOMMU_CACHE_SUBSYSTEM_H

#include <systemc.h>

#include "common/types.h"
#include "common/stats_collector.h"
#include "common/dedup_buffer.h"
#include "common/lazy_invalid_buffer.h"
#include "cache/dc_cache.h"
#include "cache/pc_cache.h"
#include "cache/msipt_cache.h"
#include "cache/pt_cache.h"
#include "cache/dedup_cache.h"
#include "cache/walker_cache.h"

#include <memory>
#include <functional>

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

    // [前置] Walker Cache 前置查询通道: 所有输入请求在PT Cache查询发起点
    // 同时写入此FIFO; 与 walker_request_fifo(PTW S2/旧路径)物理隔离,
    // 响应按 walker_origin 路由, 消除跨线程响应错配
    sc_fifo<CacheMessage> walker_front_request_fifo;
    sc_fifo<CacheMessage> walker_front_response_fifo;

    // [重构] 去重 Cache 请求/更新入口: PT Cache MISS 转发至 dedup_request_fifo;
    // PTW 完成后 Monitor 写 dedup_update_fifo 驱动 dedup_cache 清除 + Buffer 刷新。
    sc_fifo<CacheMessage> dedup_request_fifo;
    sc_fifo<CacheMessage> dedup_update_fifo;
    // [dedup多RAM] 内部预取任务FIFO: MISS时RAM Worker生成D个预取占位任务,
    // 经 scheduler 重新调度分发到不同RAM并行执行 (深度32, 满则丢弃预取防死锁)
    sc_fifo<CacheMessage> dedup_inside_request_fifo;

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
    // [MSI] MSIPT Cache 失效由失效 pipeline 驱动: GLOBAL_INVAL 与
    //       IODIR.INVAL_DDT 联动, msi_scheduler_thread 最高优先处理。
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
    uint64_t get_dedup_buffer_full_bypass_count() const { return dedup_buffer_full_bypass_count_; }
    // [FIX] Buffer满时等待PTW并发释放的机制:
    // - ptw_capacity_check_cb_: 回调函数, 检查PTW是否有空闲并发槽位
    // - dedup_buffer_full_event_: 事件, PTW每完成一个任务时notify
    using PtwCapacityCheck = std::function<bool()>;
    void set_ptw_capacity_check(PtwCapacityCheck cb) { ptw_capacity_check_cb_ = std::move(cb); }
    sc_event& get_dedup_buffer_full_event() { return dedup_buffer_full_event_; }
    void set_pt_dedup_enabled(bool enabled) { pt_dedup_enabled_ = enabled; }

    // [重构] 绑定转发 FIFO(iommu_top::pt_cache_to_fwd_fifo), 供 dedup_update 刷新 Buffer 时转发任务
    void set_forward_fifo(sc_fifo<iommu_task_t*>* fifo) { forward_fifo_ = fifo; }
    // [重构] 绑定 Buffer 刷新回调(由 iommu_top 实现: 遍历链、算PA、转发)
    // 参数: (head_index, dedup_update消息) - 消息携带已解析PTE与dedup_pa_base
    using DedupFlushCallback = std::function<void(uint16_t, const CacheMessage&)>;
    void set_dedup_flush_callback(DedupFlushCallback cb) { dedup_flush_cb_ = std::move(cb); }

    // [STAT] PT Scheduler任务间隔分析报告
    void print_pt_scheduler_gap_report() const;

    // [多RAM] 多 RAM 统计报告: 每组RAM利用率/FIFO峰值/Hash单元忙与反压
    void print_pt_multi_ram_report() const;

    // [多RAM] 内部 RAM FIFO 在途任务数(供终局统计前 drain 判断)
    int pt_ram_fifo_pending() const {
        int n = 0;
        for (const auto& f : pt_ram_fifo_) n += f->num_available();
        return n;
    }

    // [dedup多RAM] dedup 内部在途任务数(hash_in + RAM FIFO + inside预取)
    int dedup_ram_fifo_pending() const {
        int n = dedup_hash_in_fifo.num_available() +
                dedup_inside_request_fifo.num_available();
        for (const auto& f : dedup_ram_fifo_) n += f->num_available();
        return n;
    }

    // [dedup多RAM] 多 RAM 统计报告
    void print_dedup_multi_ram_report() const;

    // [walker多RAM] Walker Cache 多 RAM 统计报告: Hash单元/每组RAM利用率/前置查询计数
    void print_walker_multi_ram_report() const;

    // [失效] enqueue_cascade_invalidations 已删除: 按 spec V1.0.1,
    // IODIR 只失效 DDT/PDT 目录缓存; DC→PC 关联失效由失效 pipeline 统一编排。

    // 获取配置
    const GlobalConfig& config() const { return cfg_; }

    // [失效] 失效通路总开关(CQ 桥接依据; false 时不下发失效任务)
    bool invalidation_enabled() const { return inval_enabled_; }

    // [STAT] 打印32任务组统计报告
    void print_pt_group_report() const;

    // [STAT] DC Cache 查询间隔统计访问器
    double   get_dc_query_interval_avg_ns() const { return dc_query_interval_count_ > 0 ? dc_query_interval_total_ns_ / dc_query_interval_count_ : 0.0; }
    double   get_dc_query_interval_max_ns() const { return dc_query_interval_max_ns_; }
    double   get_dc_query_interval_min_ns() const { return dc_query_interval_min_ns_ == 999999999.0 ? 0.0 : dc_query_interval_min_ns_; }
    uint64_t get_dc_query_interval_count() const { return dc_query_interval_count_; }
    uint64_t get_dc_query_total_count() const { return dc_query_total_count_; }

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

    // Walker Cache 多RAM架构: Hash线程 + N个RAM Worker + Join线程
    // (替代原统一轮询调度线程 walker_scheduler_thread)
    //   walker_hash_thread: 轮询 front_request/request/update/invalidate 四入口;
    //     lookup/update拆分为按级子操作分发到 walker_ram_fifo_[id];
    //     S2 lookup/update(零延时)与invalidate内联执行(与旧调度器时序一致)
    //   walker_ram_worker_thread(i): 串行消耗本RAM子操作原子段延时
    //   walker_join_thread: 按(origin,task_id)聚合子响应, C3>C2>C1仲裁后回响应
    void walker_hash_thread();
    void walker_ram_worker_thread(int ram_id);
    void walker_join_thread();

    // invalidate 响应出口：返回失效是否完成、影响条目数和处理延迟。
    sc_fifo<CacheMessage> dc_invalidate_response_fifo;
    sc_fifo<CacheMessage> pc_invalidate_response_fifo;
    sc_fifo<CacheMessage> pt_invalidate_response_fifo;
    sc_fifo<CacheMessage> walker_invalidate_response_fifo;
    sc_fifo<CacheMessage> msi_invalidate_response_fifo;

    // [失效] DC/PC/MSI 合并为单一调度线程: 每轮先查 invalidate FIFO(最高优先),
    //   再处理 update/lookup; 单线程串行消除 lookup/invalidate 对同一阵列的竞态。
    void dc_scheduler_thread();
    void pc_scheduler_thread();
    // [多RAM] PT Cache 多 RAM 改造: Hash单元线程 + 每组RAM独立worker
    //   pt_hash_thread: 最高优先处理 pt_invalidate_fifo(拆分子失效到RAM worker+join),
    //     否则乒乓读 pt_request/pt_update, hash 1拍后按 ram_id 分发;
    //   目标 RAM FIFO 满则阻塞(反压上游), Hash保持忙
    void pt_hash_thread();
    //   pt_ram_worker_thread: 同 RAM 串行消耗原子段延时, 跨 RAM 并发
    void pt_ram_worker_thread(int ram_id);
    // [dedup多RAM] 去重Cache多RAM改造: Scheduler(纯调度) + Hash单元 + 每组RAM独立worker
    void dedup_scheduler_thread();
    void dedup_hash_process_thread();
    void dedup_ram_worker_thread(int ram_id);
    void msi_scheduler_thread();
    void walker_update_worker_thread();
    void invalidation_worker_thread();
    // [失效] PT/Walker 失效拆分到 RAM worker 并 join(hash线程调用, 阻塞等待子失效完成)
    void dispatch_pt_invalidate(const CacheMessage& cmd);
    void dispatch_walker_invalidate(const CacheMessage& cmd);
    void end_of_simulation() override;
    void process_next_invalidation_request();
    void push_fifo(sc_fifo<CacheMessage>& fifo, const CacheMessage& msg);
    bool pop_fifo(sc_fifo<CacheMessage>& fifo, CacheMessage& msg);
    
    // NEW: Public accessor for PT Cache (用于去重+预取模块)
    PTCache* get_pt_cache() { return pt_cache_.get(); }
    
    CacheMessage execute_dc_request(const CacheMessage& req);
    CacheMessage execute_pc_request(const CacheMessage& req);
    CacheMessage execute_pt_request(const CacheMessage& req);
    CacheMessage execute_dedup_request(const CacheMessage& req);
    void         execute_dedup_update(const CacheMessage& upd);
    CacheMessage execute_walker_request(const CacheMessage& req);
    CacheMessage execute_msi_request(const CacheMessage& req);
    CacheMessage execute_dc_update_request(const CacheMessage& req);
    CacheMessage execute_pc_update_request(const CacheMessage& req);
    CacheMessage execute_pt_update_request(const CacheMessage& req);
    CacheMessage execute_walker_update_request(const CacheMessage& req);
    CacheMessage execute_msi_update_request(const CacheMessage& req);
    CacheMessage execute_dc_invalidate_request(const CacheMessage& req);
    CacheMessage execute_pc_invalidate_request(const CacheMessage& req);
    // [失效] execute_pt/walker_invalidate_request 已删除(由 dispatch_* 取代);
    // [MSI] MSIPT 失效: GLOBAL=全清, 其余=按 device_id 失效(由 msi_scheduler 执行)
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

    // [重构] 独立 dedup_cache 实例(几何由 cfg.dedup_cache 配置)
    std::unique_ptr<DedupCache> dedup_cache_;
    // [重构] 转发 FIFO 句柄 + Buffer 刷新回调(由 iommu_top 绑定)
    sc_fifo<iommu_task_t*>* forward_fifo_ = nullptr;
    DedupFlushCallback      dedup_flush_cb_;
    // [dedup多RAM] 乒乓调度状态(scheduler使用)
    bool     dedup_sched_next_is_request_ = true;

    // ============================================================
    // [失效] 延迟失效(LIB/VN) + PT/Walker 失效子任务 join 状态
    // ============================================================
    // PT 与 Walker 共享同一个 LIB 与全局 VN(同一条 PTE 失效指令需二者同步响应)
    LazyInvalidBuffer lib_;
    bool     inval_enabled_ = true;
    bool     lazy_enabled_ = true;

    // PT 失效 join: hash 线程一次只处理一条失效指令并阻塞等待, 故单一计数器安全
    int      pt_inval_remaining_ = 0;
    uint32_t pt_inval_affected_ = 0;
    uint8_t  pt_inval_sweep_new_vn_ = 0;   // VN回绕批量扫表后的新全局VN
    sc_event pt_inval_sub_done_event_;
    // Walker 失效 join(同理)
    int      walker_inval_remaining_ = 0;
    uint32_t walker_inval_affected_ = 0;
    uint8_t  walker_inval_sweep_new_vn_ = 0;
    sc_event walker_inval_sub_done_event_;

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

    // [STAT] Dedup Scheduler 执行延时统计 (多RAM下由RAM Worker更新, 聚合口径)
    uint64_t dedup_req_count_        = 0;        // REQUEST 任务数
    uint64_t dedup_upd_count_        = 0;        // UPDATE 任务数
    double   dedup_req_total_exec_ns_ = 0.0;     // REQUEST 总执行时间
    double   dedup_upd_total_exec_ns_ = 0.0;     // UPDATE 总执行时间
    double   dedup_req_max_exec_ns_   = 0.0;     // REQUEST 最大执行时间
    double   dedup_upd_max_exec_ns_   = 0.0;     // UPDATE 最大执行时间
    double   dedup_req_min_exec_ns_   = 999999999.0; // REQUEST 最小执行时间
    double   dedup_upd_min_exec_ns_   = 999999999.0; // UPDATE 最小执行时间
    double   dedup_first_start_ns_    = -1.0;    // 第一笔任务开始时刻
    double   dedup_last_end_ns_       = 0.0;     // 最后一笔任务结束时刻

    // ============================================================
    // [dedup多RAM] 去重Cache 多 RAM 方案状态与统计
    // ============================================================
    static constexpr uint32_t DEDUP_MAX_RAMS = 16;
    uint32_t dedup_num_rams_ = 1;
    // Scheduler -> Hash 单元接口FIFO(深度1): 写满即代表Hash忙, 天然反压
    sc_fifo<CacheMessage> dedup_hash_in_fifo;
    // 每组 RAM 前置 FIFO (深度 ram_fifo_depth, 满则反压 Hash 单元)
    std::vector<std::unique_ptr<sc_fifo<CacheMessage>>> dedup_ram_fifo_;

    // [STAT] Hash 单元统计
    uint64_t dedup_hash_task_count_       = 0;    // Hash处理任务总数
    double   dedup_hash_busy_ns_          = 0.0;  // Hash执行总时长(含反压阻塞)
    double   dedup_hash_backpressure_ns_  = 0.0;  // 因RAM FIFO满阻塞总时长
    uint64_t dedup_hash_backpressure_cnt_ = 0;    // 反压发生次数

    // [STAT] 每组 RAM worker 统计
    uint64_t dedup_ram_task_count_[DEDUP_MAX_RAMS]     = {};  // 处理任务数(req+upd+prefetch)
    uint64_t dedup_ram_req_count_[DEDUP_MAX_RAMS]      = {};  // 普通REQUEST任务数
    uint64_t dedup_ram_upd_count_[DEDUP_MAX_RAMS]      = {};  // UPDATE任务数
    uint64_t dedup_ram_prefetch_count_[DEDUP_MAX_RAMS] = {};  // 预取任务数
    double   dedup_ram_busy_ns_[DEDUP_MAX_RAMS]        = {};  // RAM原子段忙总时长
    int      dedup_ram_fifo_peak_[DEDUP_MAX_RAMS]      = {};  // FIFO占用峰值
    double   dedup_ram_first_start_ns_ = -1.0;                // 首任务开始(全RAM)
    double   dedup_ram_last_end_ns_    = 0.0;                 // 末任务结束(全RAM)
    // [STAT] 预取丢弃计数(inside_fifo满时nb_write失败, 保功能防死锁)
    uint64_t dedup_prefetch_dropped_ = 0;
    // [STAT] Buffer满旁路计数: Buffer<全局outstanding时, Buffer满则任务旁路直转PTW(防死锁)
    uint64_t dedup_buffer_full_bypass_count_ = 0;
    // [STAT需求2] 去重Cache命中率统计(execute_dedup_request的lookup)
    uint64_t dedup_lookup_total_ = 0;      // 全部lookup次数(主任务请求+预取占位)
    uint64_t dedup_lookup_hit_ = 0;        // 命中占位CL次数(line!=nullptr)
    uint64_t dedup_lookup_miss_ = 0;       // 未命中次数(line==nullptr)
    uint64_t dedup_req_lookup_total_ = 0;  // 主任务去重请求(!dedup_is_prefetch)lookup次数
    uint64_t dedup_req_lookup_hit_ = 0;    // 主任务去重请求命中次数
    uint64_t dedup_req_lookup_miss_ = 0;   // 主任务去重请求未命中次数
    // [FIX] Buffer满时dedup worker阻塞等待PTW并发释放的事件
    // PTW每完成一个任务时由iommu_top调用notify, dedup worker在此事件上wait
    sc_event dedup_buffer_full_event_;
    // [FIX] PTW并发容量检查回调: 返回true表示PTW有空闲槽位
    PtwCapacityCheck ptw_capacity_check_cb_;

    // ============================================================
    // [STAT] DC Cache 查询完成时间间隔统计
    // ============================================================
    double   dc_query_last_end_ns_ = 0.0;       // 上一次查询完成时刻(ns)
    double   dc_query_interval_total_ns_ = 0.0; // 间隔总和(ns)
    double   dc_query_interval_max_ns_ = 0.0;   // 最大间隔(ns)
    double   dc_query_interval_min_ns_ = 999999999.0; // 最小间隔(ns)
    uint64_t dc_query_interval_count_ = 0;      // 间隔采样次数
    uint64_t dc_query_total_count_ = 0;         // DC查询总次数

    // ============================================================
    // [多RAM] PT Cache 多 RAM 方案状态与统计
    // ============================================================
    static constexpr uint32_t PT_MAX_RAMS = 16;
    uint32_t pt_num_rams_ = 1;
    // 每组 RAM 前置 FIFO (深度 ram_fifo_depth, 满则反压 Hash 单元)
    std::vector<std::unique_ptr<sc_fifo<CacheMessage>>> pt_ram_fifo_;

    // [STAT] Hash 单元统计
    uint64_t pt_hash_task_count_       = 0;    // Hash处理任务总数
    double   pt_hash_busy_ns_          = 0.0;  // Hash执行总时长(含反压阻塞)
    double   pt_hash_backpressure_ns_  = 0.0;  // 因RAM FIFO满阻塞总时长
    uint64_t pt_hash_backpressure_cnt_ = 0;    // 反压发生次数

    // [STAT] 每组 RAM worker 统计
    uint64_t pt_ram_task_count_[PT_MAX_RAMS]   = {};  // 处理任务数(lookup+update)
    uint64_t pt_ram_lookup_count_[PT_MAX_RAMS] = {};  // lookup任务数
    uint64_t pt_ram_update_count_[PT_MAX_RAMS] = {};  // update任务数
    double   pt_ram_busy_ns_[PT_MAX_RAMS]      = {};  // RAM原子段忙总时长
    int      pt_ram_fifo_peak_[PT_MAX_RAMS]    = {};  // FIFO占用峰值
    double   pt_ram_first_start_ns_ = -1.0;           // 首任务开始(全RAM)
    double   pt_ram_last_end_ns_    = 0.0;            // 末任务结束(全RAM)

    // ============================================================
    // [walker多RAM] Walker Cache 三级子表独立分RAM 状态与统计
    // 物理拓扑: 每个子表(C1/C2/C3)拥有独立的 num_rams 组RAM bank,
    // 共 3*num_rams 个worker; 同一lookup的三级子查询天然落在不同子表的
    // RAM上完全并行; 同子表内按本级set哈希低位分4组
    // worker索引 = (level-1)*num_rams + ram_id
    // ============================================================
    static constexpr uint32_t WALKER_MAX_RAMS = 16;   // 每子表最大RAM组数
    static constexpr uint32_t WALKER_LEVELS = 3;      // 子表数(C1/C2/C3)
    uint32_t walker_num_rams_ = 1;                    // 每子表RAM组数
    uint32_t walker_total_workers_ = 3;               // = WALKER_LEVELS * num_rams
    // 每组 RAM 前置 FIFO (深度 ram_fifo_depth, 满则反压 Hash 线程)
    std::vector<std::unique_ptr<sc_fifo<CacheMessage>>> walker_ram_fifo_;
    // [优先级] 每个worker独立的update高优先FIFO: update不排在积压的lookup之后,
    // worker每轮优先消费update(硬件语义=写口优先), 保证Walker更新及时可见
    std::vector<std::unique_ptr<sc_fifo<CacheMessage>>> walker_ram_upd_fifo_;
    // RAM worker -> Join 线程的子响应汇聚FIFO
    sc_fifo<CacheMessage> walker_join_fifo;
    // [串行查询] Join线程 miss 后续查请求 -> Hash线程 的续查FIFO
    // (C3→C2→C1 顺序查询: 每级 miss 后才发起下一级, 每级各付1拍hash)
    sc_fifo<CacheMessage> walker_serial_fifo;

    // Join 待聚合表项: 按 key=(origin<<56)|task_id 索引
    struct WalkerJoinEntry {
        CacheMessage base;                     // 原始lookup请求(携带origin/路由信息)
        uint8_t  expected = 0;                 // 期望子响应数(串行模式恒为1)
        uint8_t  received = 0;                 // 已收子响应数
        uint8_t  next_level = 0;               // [串行] 本级miss后待查询的下一级(0=结束)
        bool     hit[4] = {false, false, false, false};   // 按级命中标志[1..3]
        WalkerData data[4];                    // 按级命中数据[1..3]
        sc_time  max_ram_latency = SC_ZERO_TIME;  // [串行] 各级原子段延时+续查hash拍 累计
        sc_time  start_time = SC_ZERO_TIME;    // hash入队时刻(统计用)
    };
    std::map<uint64_t, WalkerJoinEntry> walker_join_pending_;

    // 内部辅助: lookup/update 拆分分发(含hash 1拍与反压统计)
    void dispatch_walker_lookup(CacheMessage& req);
    void dispatch_walker_update(CacheMessage& req);
    // [串行查询] 单级子查询分发(目标RAM FIFO满则阻塞反压)
    void dispatch_walker_sub_lookup(const CacheMessage& req, uint8_t level);
    // [串行查询] 级链: 3→2→(1仅Sv48非Sv39)→0
    uint8_t walker_serial_next_level(uint8_t level, const CacheMessage& req);

    // [STAT] Walker Hash 单元统计
    uint64_t walker_hash_task_count_       = 0;
    double   walker_hash_busy_ns_          = 0.0;
    double   walker_hash_backpressure_ns_  = 0.0;
    uint64_t walker_hash_backpressure_cnt_ = 0;

    // [STAT] Walker 每个 worker(子表x RAM组) 统计, 索引=(level-1)*num_rams+ram_id
    static constexpr uint32_t WALKER_MAX_WORKERS = WALKER_LEVELS * WALKER_MAX_RAMS;
    uint64_t walker_ram_task_count_[WALKER_MAX_WORKERS]   = {};
    uint64_t walker_ram_lookup_count_[WALKER_MAX_WORKERS] = {};
    uint64_t walker_ram_update_count_[WALKER_MAX_WORKERS] = {};
    double   walker_ram_busy_ns_[WALKER_MAX_WORKERS]      = {};
    int      walker_ram_fifo_peak_[WALKER_MAX_WORKERS]    = {};
    double   walker_ram_first_start_ns_ = -1.0;
    double   walker_ram_last_end_ns_    = 0.0;

    // [STAT] 前置查询计数(walker_front_request_fifo入口)
    uint64_t walker_front_lookup_count_ = 0;

public:
    void print_dedup_scheduler_report() const;   // 打印 dedup scheduler 统计报告

};

} // namespace iommu

#endif // IOMMU_CACHE_SUBSYSTEM_H
