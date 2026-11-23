#include "subsystem/cache_subsystem.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
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
        // [失效] 补齐新增模式, 避免 task trace 打印 UNKNOWN
        case CacheInvalidateMode::SCAN_RANGE: return "SCAN_RANGE";
        case CacheInvalidateMode::LAZY: return "LAZY";
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
      walker_front_request_fifo(16),
      walker_front_response_fifo(16),
      walker_join_fifo(64),
      walker_serial_fifo(64),
#if TEST_CFG_DEDUP_UNIT_V1123
      // [去重单元机制优化 1123] 外层队列深度: req=8, inter=1,
      //   dedup_update_fifo 退化为 drain->upd_hash 内部衔接(深度24, 假设A2)。
      dedup_request_fifo(DEDUP_OUT_REQ_FIFO_DEPTH),
      dedup_update_fifo(DEDUP_UPD_HANDOFF_DEPTH),
      dedup_inside_request_fifo(DEDUP_OUT_INTER_FIFO_DEPTH),
#else
      dedup_request_fifo(64),
      dedup_update_fifo(64),
      dedup_inside_request_fifo(32),
#endif
#if TEST_CFG_DEDUP_DELAYED_UPDATE
      delayed_upd_timer_eq_("delayed_upd_timer_eq"),
#endif
      dedup_hash_in_fifo(1),
#if TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
      dedup_upd_hash_in_fifo(1),   // [释放优先通道] UPDATE 专用出口, 同深度1
#endif
      dc_response_fifo(16),
      pc_response_fifo(16),
      pt_hit_response_fifo(32),
      pt_miss_response_fifo(32),
      walker_response_fifo(1),
      msi_response_fifo(1024),
      dc_update_fifo(1),
      pc_update_fifo(1),
      pt_update_fifo(PT_UPDATE_FIFO_DEPTH),   // [防死锁] 深度可经 TEST_CFG_PT_UPDATE_FIFO_DEPTH 覆盖(默认32, 基线不变)
      walker_update_fifo(1024),
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

    SC_THREAD(dc_scheduler_thread);
    SC_THREAD(pc_scheduler_thread);
    // [多RAM] PT Cache 多 RAM 改造: Hash单元线程 + 每组RAM独立worker(sc_spawn)
    pt_num_rams_ = pt_cache_->num_rams();
    assert(pt_num_rams_ >= 1 && pt_num_rams_ <= PT_MAX_RAMS);
    {
        const int fifo_depth = static_cast<int>(pt_cache_->ram_fifo_depth());
        for (uint32_t i = 0; i < pt_num_rams_; ++i) {
            char fname[32];
            snprintf(fname, sizeof(fname), "pt_ram_fifo_%u", i);
            pt_ram_fifo_.emplace_back(
                std::make_unique<sc_fifo<CacheMessage>>(fname, fifo_depth));
        }
        for (uint32_t i = 0; i < pt_num_rams_; ++i) {
            sc_spawn(sc_bind(&CacheSubsystem::pt_ram_worker_thread, this,
                             static_cast<int>(i)));
        }
    }
    SC_THREAD(pt_hash_thread);
    // [dedup多RAM] 去重Cache多RAM改造: Scheduler + Hash单元 + 每组RAM独立worker(sc_spawn)
#if TEST_CFG_DEDUP_UNIT_V1123
    // [去重单元机制优化 1123] 新拓扑: 取消中央scheduler与单Hash, 改为
    //   3个类型化Hash单元(req/inter/upd, 各1ns) + 延时更新排空线程(标准机制)
    //   + 每bank响应写出线程(G3)。旧线程不注册(定义保留供V1123=0 A/B臂)。
    SC_THREAD(dedup_req_hash_thread);
    SC_THREAD(dedup_inter_hash_thread);
    SC_THREAD(dedup_upd_hash_thread);
    SC_THREAD(dedup_delayed_update_drain_thread);
#else
    SC_THREAD(dedup_scheduler_thread);
#if TEST_CFG_DEDUP_DELAYED_UPDATE
    // [去重延迟更新] 延迟FIFO排空线程: 三条件(定时器/压力/Buffer满)转入dedup_update_fifo
    SC_THREAD(dedup_delayed_update_drain_thread);
#endif
    SC_THREAD(dedup_hash_process_thread);
#endif
    // [walker多RAM] Walker Cache 多RAM改造: Hash线程 + 每子表x每RAM组独立worker + Join线程
    // 物理拓扑: C1/C2/C3三子表各自独立 num_rams 组RAM, 共 3*num_rams 个worker,
    // 同一lookup的三级子查询天然并行; worker索引=(level-1)*num_rams+ram_id
    walker_num_rams_ = walker_cache_->num_rams();
    assert(walker_num_rams_ >= 1 && walker_num_rams_ <= WALKER_MAX_RAMS);
    walker_total_workers_ = WALKER_LEVELS * walker_num_rams_;
    {
        const int fifo_depth = static_cast<int>(walker_cache_->ram_fifo_depth());
        for (uint32_t w = 0; w < walker_total_workers_; ++w) {
            char fname[40];
            snprintf(fname, sizeof(fname), "walker_ram_fifo_L%u_%u",
                     w / walker_num_rams_ + 1, w % walker_num_rams_);
            walker_ram_fifo_.emplace_back(
                std::make_unique<sc_fifo<CacheMessage>>(fname, fifo_depth));
            char uname[44];
            snprintf(uname, sizeof(uname), "walker_ram_upd_fifo_L%u_%u",
                     w / walker_num_rams_ + 1, w % walker_num_rams_);
            walker_ram_upd_fifo_.emplace_back(
                std::make_unique<sc_fifo<CacheMessage>>(uname, fifo_depth));
        }
        for (uint32_t w = 0; w < walker_total_workers_; ++w) {
            sc_spawn(sc_bind(&CacheSubsystem::walker_ram_worker_thread, this,
                             static_cast<int>(w)));
        }
    }
    SC_THREAD(walker_hash_thread);
    SC_THREAD(walker_join_thread);
    SC_THREAD(msi_scheduler_thread);
    SC_THREAD(invalidation_worker_thread);
    // [STAT需求] 入口/去重 4个FIFO占用采样线程(每拍读 num_available, 峰值+时间加权平均)
    SC_THREAD(fifo_util_sampler_thread);
    
    // [NEW] 初始化PT Cache去重功能
    dedup_buffer_ = new DedupBuffer();
#if TEST_CFG_DEDUP_UNIT_V1123
    // [去重单元机制优化 1123] dedup_buffer_=聚合影子(全4bank合计口径, 占用率分母512);
    //   4个bank实例与去重RAM一一绑定(每bank 128 entry, alloc/free自动同步累计到影子)。
    dedup_buffer_->init(0xFFFFFFFFu, nullptr, PT_DEDUP_BUFFER_SIZE);
    for (uint32_t b = 0; b < DEDUP_NUM_BANKS; ++b) {
        dedup_banks_[b] = new DedupBuffer();
        dedup_banks_[b]->init(b, dedup_buffer_, DEDUP_BUFFER_PER_BANK);
    }
#endif
    pt_dedup_enabled_ = true;
    // [dedup多RAM] 创建独立 dedup_cache (几何由 cfg.dedup_cache 配置) + RAM分组
    dedup_cache_ = std::make_unique<DedupCache>(cfg.dedup_cache.num_sets, cfg.dedup_cache.num_ways);
    dedup_cache_->configure_multi_ram(cfg.dedup_cache.num_rams);
    dedup_num_rams_ = dedup_cache_->num_rams();
    assert(dedup_num_rams_ >= 1 && dedup_num_rams_ <= DEDUP_MAX_RAMS);
#if TEST_CFG_DEDUP_UNIT_V1123
    // [1123] 几何约束: 4 Bank x 64 CL = 256 (num_sets=64 x num_ways=4 x num_rams=4)
    assert(dedup_num_rams_ == DEDUP_NUM_BANKS && "V1123 要求 dedup_cache.num_rams=4");
    assert(dedup_cache_->total_lines() == 256 && "V1123 要求去重Cache总cacheline=256");
#endif
    {
#if TEST_CFG_DEDUP_UNIT_V1123
        // [1123] 每RAM三条类型化前置FIFO: req(32)/inter(16)/upd(2) + 响应staging(64)
        const int req_depth  = static_cast<int>(DEDUP_RAM_REQ_FIFO_DEPTH);
        const int pf_depth   = static_cast<int>(DEDUP_RAM_INTER_FIFO_DEPTH);
        const int upd_depth  = static_cast<int>(DEDUP_RAM_UPD_FIFO_DEPTH);
        const int stage_depth = static_cast<int>(DEDUP_RSP_STAGE_DEPTH);
        for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
            char fname[36];
            snprintf(fname, sizeof(fname), "dedup_ram_fifo_%u", i);
            dedup_ram_fifo_.emplace_back(
                std::make_unique<sc_fifo<CacheMessage>>(fname, req_depth));
            snprintf(fname, sizeof(fname), "dedup_ram_inter_fifo_%u", i);
            dedup_ram_inter_fifo_.emplace_back(
                std::make_unique<sc_fifo<CacheMessage>>(fname, pf_depth));
            snprintf(fname, sizeof(fname), "dedup_ram_upd_fifo_%u", i);
            dedup_ram_upd_fifo_.emplace_back(
                std::make_unique<sc_fifo<CacheMessage>>(fname, upd_depth));
            snprintf(fname, sizeof(fname), "dedup_rsp_stage_%u", i);
            dedup_rsp_stage_fifo_.emplace_back(
                std::make_unique<sc_fifo<CacheMessage>>(fname, stage_depth));
        }
        for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
            sc_spawn(sc_bind(&CacheSubsystem::dedup_ram_worker_thread, this,
                             static_cast<int>(i)));
        }
        for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
            sc_spawn(sc_bind(&CacheSubsystem::dedup_rsp_writer_thread, this,
                             static_cast<int>(i)));
        }
#else
        const int fifo_depth = static_cast<int>(cfg.dedup_cache.ram_fifo_depth);
        for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
            char fname[36];
            snprintf(fname, sizeof(fname), "dedup_ram_fifo_%u", i);
            dedup_ram_fifo_.emplace_back(
                std::make_unique<sc_fifo<CacheMessage>>(fname, fifo_depth));
#if TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
            // [释放优先通道] 每 RAM 一条 UPDATE 专用前置 FIFO(深度与 request 通道同值,
            //   不引入新容量参数); 对齐 walker_ram_upd_fifo_ 的命名与建法。
            char uname[40];
            snprintf(uname, sizeof(uname), "dedup_ram_upd_fifo_%u", i);
            dedup_ram_upd_fifo_.emplace_back(
                std::make_unique<sc_fifo<CacheMessage>>(uname, fifo_depth));
#endif
        }
        for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
            sc_spawn(sc_bind(&CacheSubsystem::dedup_ram_worker_thread, this,
                             static_cast<int>(i)));
        }
#endif
    }
#if TEST_CFG_DEDUP_UNIT_V1123
    printf("[CACHE_SUBSYSTEM][1123] dedup new topology ON: banks=4 x (req%u/inter%u/upd%u), "
           "outer req=%u/inter=%u/delayed_upd=%u(timer=%uns,margin=%u), hash=3x%uns, "
           "atomic=%uns, Buffer=4x%u(total %u), rsp_stage=%u/bank, CL=%u(sets%u x ways%u)\n",
           DEDUP_RAM_REQ_FIFO_DEPTH, DEDUP_RAM_INTER_FIFO_DEPTH, DEDUP_RAM_UPD_FIFO_DEPTH,
           DEDUP_OUT_REQ_FIFO_DEPTH, DEDUP_OUT_INTER_FIFO_DEPTH,
           DEDUP_DELAYED_UPDATE_FIFO_DEPTH, DEDUP_DELAYED_UPDATE_TIMER_NS,
           DEDUP_DELAYED_PRESSURE_MARGIN, DEDUP_HASH_CYCLES, DEDUP_ATOMIC_CYCLES,
           DEDUP_BUFFER_PER_BANK, PT_DEDUP_BUFFER_SIZE, DEDUP_RSP_STAGE_DEPTH,
           dedup_cache_->total_lines(), cfg.dedup_cache.num_sets, cfg.dedup_cache.num_ways);
#if defined(TEST_CFG_PT_DEDUP_BUFFER_SIZE)
    printf("[CACHE_SUBSYSTEM][1123] NOTE: TEST_CFG_PT_DEDUP_BUFFER_SIZE=%u ignored "
           "(v1123 Buffer fixed = 4 x %u = %u)\n",
           (unsigned)TEST_CFG_PT_DEDUP_BUFFER_SIZE, DEDUP_BUFFER_PER_BANK, PT_DEDUP_BUFFER_SIZE);
#endif
    fflush(stdout);
#else
    printf("[CACHE_SUBSYSTEM] PT Cache dedup enabled, Buffer + dedup_cache created (sets=%u, ways=%u, num_rams=%u, ram_fifo_depth=%u, upd_prio_lane=%d, upd_atomic=%u+chain*%uns, prio=UPD>INS>REQ)\n",
           cfg.dedup_cache.num_sets, cfg.dedup_cache.num_ways,
           dedup_num_rams_, cfg.dedup_cache.ram_fifo_depth,
           static_cast<int>(TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE),
           DEDUP_UPD_BASE_CYCLES, DEDUP_UPD_FLUSH_CYCLES);
    fflush(stdout);
#endif

    // [失效] 配置并注入共享 LIB: PT 与 Walker 共用同一个 LIB 与全局 VN
    inval_enabled_ = cfg.invalidation.enable;
    lazy_enabled_ = cfg.invalidation.lazy_enable;
    lib_.configure(cfg.invalidation.lib_size, cfg.invalidation.vn_bits);
    pt_cache_->set_lazy_invalid_buffer(&lib_);
    walker_cache_->set_lazy_invalid_buffer(&lib_);
    printf("[CACHE_SUBSYSTEM] Invalidation: enable=%d, lazy=%d, LIB size=%u, VN_MAX=%u, lib_match_cycles=%u\n",
           inval_enabled_, lazy_enabled_, lib_.size(), lib_.vn_max(),
           cfg.invalidation.lib_match_cycles);
    fflush(stdout);
}

#if TEST_CFG_MULTI_DEVICE_SCENE
bool CacheSubsystem::md_quiescent() const {
#if TEST_CFG_DEDUP_UNIT_V1123
    // [1123] 在途判据: Σbank预留 + 聚合影子占用(dedup_buffer_ 已含全部bank)
    uint32_t reserved_sum = 0;
    for (uint32_t b = 0; b < DEDUP_NUM_BANKS; ++b) reserved_sum += dedup_reserved_bank_[b];
    if (md_active_ || !walker_join_pending_.empty() || pt_inval_remaining_ || walker_inval_remaining_ ||
        reserved_sum || (dedup_buffer_ && dedup_buffer_->get_valid_count())) return false;
#else
    if (md_active_ || !walker_join_pending_.empty() || pt_inval_remaining_ || walker_inval_remaining_ ||
        dedup_reserved_count_ || (dedup_buffer_ && dedup_buffer_->get_valid_count())) return false;
#endif
    const sc_fifo<CacheMessage>* fifos[] = {
        &dc_request_fifo,&pc_request_fifo,&pt_request_fifo,&walker_request_fifo,&msi_request_fifo,
        &walker_front_request_fifo,&walker_front_response_fifo,&dedup_request_fifo,&dedup_update_fifo,
        &dedup_inside_request_fifo,&dedup_hash_in_fifo,&dc_response_fifo,&pc_response_fifo,
        &pt_hit_response_fifo,&pt_miss_response_fifo,&walker_response_fifo,&msi_response_fifo,
        &dc_update_fifo,&pc_update_fifo,&pt_update_fifo,&walker_update_fifo,&msi_update_fifo,
        &dc_invalidate_fifo,&pc_invalidate_fifo,&pt_invalidate_fifo,&walker_invalidate_fifo,&msi_invalidate_fifo,
        &dc_invalidate_response_fifo,&pc_invalidate_response_fifo,&pt_invalidate_response_fifo,
        &walker_invalidate_response_fifo,&msi_invalidate_response_fifo,&invalidation_request_fifo,
        &invalidation_response_fifo,&walker_join_fifo,&walker_serial_fifo};
    for(auto* f:fifos) if(f->num_available()) return false;
#if TEST_CFG_DEDUP_UNIT_V1123
    // [1123] 新拓扑全部在途通道都必须排空: per-RAM upd/inter + 响应staging
    for (const auto& f : dedup_ram_upd_fifo_) if (f->num_available()) return false;
    for (const auto& f : dedup_ram_inter_fifo_) if (f->num_available()) return false;
    for (const auto& f : dedup_rsp_stage_fifo_) if (f->num_available()) return false;
#elif TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
    // [释放优先通道] 新增通道也必须排空, 否则会在释放消息未刷完时就判定静止。
    if (dedup_upd_hash_in_fifo.num_available()) return false;
    for (const auto& f : dedup_ram_upd_fifo_) if (f->num_available()) return false;
#endif
    for(const auto* banks:{&pt_ram_fifo_,&dedup_ram_fifo_,&walker_ram_fifo_,&walker_ram_upd_fifo_})
        for(const auto& f:*banks) if(f->num_available()) return false;
    return true;
}
void CacheSubsystem::md_record(const CacheMessage& msg, const std::string& source,
                               const std::string& op, double start, bool hit,
                               int bank, int level, uint64_t value) {
    if (!md_trace_) return;
    const bool context_cache = source.find("pt_cache") == 0 || source.find("dedup") == 0 ||
                               source.find("walker") == 0;
    if (!msg.task_id) { md_trace_->progress(); return; }
    unsigned did = msg.device_id;
    if (context_cache) {
        iommu_md::require(msg.gscid > 0 && msg.gscid <= md_trace_->devices, "Cache消息GSCID非法");
        did = msg.gscid - 1;
    }
    md_trace_->operation(msg.task_id, source, op, did, start, hit, bank, level,
                         start - msg.timestamp.to_seconds()*1e9, value);
}
#endif

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
#if TEST_CFG_MULTI_DEVICE_SCENE
    iommu_md::BusyGuard md_busy(md_active_);
#endif
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
#if TEST_CFG_MULTI_DEVICE_SCENE
    if (std::string(phase) == "end" && op_name != "invalidate") {
        const std::string source = cache_name == "walker_cache" ?
            (msg.walker_is_s2_lookup ? "walker_s2" : "walker_vs") : cache_name;
        md_record(msg, source, op_name, start_time.to_seconds()*1e9,
                  resp && resp->hit, -1, resp ? resp->walker_level : 0);
    }
#endif
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

// ============================================================
// [失效] DC Cache 合并调度线程
// 优先级: invalidate(最高) > update > lookup; 单线程串行避免阵列竞态。
// ============================================================
void CacheSubsystem::dc_scheduler_thread() {
    while (true) {
        if (dc_invalidate_fifo.num_available() > 0) {
            CacheMessage req = dc_invalidate_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "dc_cache", "invalidate", req, start);
            CacheMessage resp = execute_dc_invalidate_request(req);
            record_task_completion("dc_cache", start, sc_time_stamp());
            trace_task_event("end", "dc_cache", "invalidate", req, start, &resp);
            push_fifo(dc_invalidate_response_fifo, resp);
            continue;
        }
        if (dc_update_fifo.num_available() > 0) {
            CacheMessage req = dc_update_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "dc_cache", "update", req, start);
            CacheMessage resp = execute_dc_update_request(req);
            record_task_completion("dc_cache", start, sc_time_stamp());
            trace_task_event("end", "dc_cache", "update", req, start, &resp);
            continue;
        }
        if (dc_request_fifo.num_available() > 0) {
            CacheMessage req = dc_request_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "dc_cache", "lookup", req, start);
            CacheMessage resp = execute_dc_request(req);
            record_task_completion("dc_cache", start, sc_time_stamp());
            trace_task_event("end", "dc_cache", "lookup", req, start, &resp);
            push_fifo(dc_response_fifo, resp);
            
            // [STAT] DC 单线程串行忙时(含 arbiter+hash+read_set+compare)
            dc_busy_ns_ += (sc_time_stamp() - start).to_seconds() * 1e9;

            // [STAT] DC Cache 查询完成时间间隔统计
            dc_query_total_count_++;
            const double end_ns = sc_time_stamp().to_seconds() * 1e9;
            if (dc_query_last_end_ns_ > 0.0) {
                double interval = end_ns - dc_query_last_end_ns_;
                dc_query_interval_total_ns_ += interval;
                if (interval > dc_query_interval_max_ns_)
                    dc_query_interval_max_ns_ = interval;
                if (interval < dc_query_interval_min_ns_)
                    dc_query_interval_min_ns_ = interval;
                dc_query_interval_count_++;
            }
            dc_query_last_end_ns_ = end_ns;
            
            continue;
        }
        wait(dc_invalidate_fifo.data_written_event() |
             dc_update_fifo.data_written_event() |
             dc_request_fifo.data_written_event());
    }
}

// ============================================================
// [失效] PC Cache 合并调度线程 (invalidate > update > lookup)
// ============================================================
void CacheSubsystem::pc_scheduler_thread() {
    while (true) {
        if (pc_invalidate_fifo.num_available() > 0) {
            CacheMessage req = pc_invalidate_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "pc_cache", "invalidate", req, start);
            CacheMessage resp = execute_pc_invalidate_request(req);
            record_task_completion("pc_cache", start, sc_time_stamp());
            trace_task_event("end", "pc_cache", "invalidate", req, start, &resp);
            push_fifo(pc_invalidate_response_fifo, resp);
            continue;
        }
        if (pc_update_fifo.num_available() > 0) {
            CacheMessage req = pc_update_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "pc_cache", "update", req, start);
            CacheMessage resp = execute_pc_update_request(req);
            record_task_completion("pc_cache", start, sc_time_stamp());
            trace_task_event("end", "pc_cache", "update", req, start, &resp);
            continue;
        }
        if (pc_request_fifo.num_available() > 0) {
            CacheMessage req = pc_request_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "pc_cache", "lookup", req, start);
            CacheMessage resp = execute_pc_request(req);
            record_task_completion("pc_cache", start, sc_time_stamp());
            trace_task_event("end", "pc_cache", "lookup", req, start, &resp);
            push_fifo(pc_response_fifo, resp);

            // [STAT] PC 单线程串行忙时/查询间隔
            //   PC 与 DC 均为单线程单 RAM, 但 PC 每次查询
            //   = arbiter(1) + hash(1) + read_set(1) + compare(1) = 4ns;
            //   小包场景每包无条件查一次, 是"每包固定4ns"的直接取证点。
            {
                const double pc_end_ns = sc_time_stamp().to_seconds() * 1e9;
                const double pc_start_ns = start.to_seconds() * 1e9;
                pc_busy_ns_ += (pc_end_ns - pc_start_ns);
                pc_query_total_count_++;
                if (pc_query_last_end_ns_ >= 0.0) {
                    const double interval = pc_end_ns - pc_query_last_end_ns_;
                    pc_query_interval_total_ns_ += interval;
                    if (interval > pc_query_interval_max_ns_)
                        pc_query_interval_max_ns_ = interval;
                    if (interval < pc_query_interval_min_ns_)
                        pc_query_interval_min_ns_ = interval;
                    pc_query_interval_count_++;
                }
                pc_query_last_end_ns_ = pc_end_ns;
            }

            continue;
        }
        wait(pc_invalidate_fifo.data_written_event() |
             pc_update_fifo.data_written_event() |
             pc_request_fifo.data_written_event());
    }
}

// ============================================================
// [多RAM] pt_hash_thread - PT Cache Hash 单元线程
// 乒乓读 pt_request_fifo/pt_update_fifo(仲裁策略不变);
// 与旧 pt_scheduler_thread 的关键差异: Hash 单元空闲即读下一任务,
// 不再等待整个任务执行完成; RAM 原子段由 pt_ram_worker_thread 并发消耗。
// Hash 流程: 读任务 -> wait(hash 1拍) -> ram_id = raw_hash & (num_rams-1)
//          -> 写入 pt_ram_fifo_[ram_id] (满则阻塞, Hash保持忙, 反压上游FIFO)
// ============================================================
void CacheSubsystem::pt_hash_thread() {
    while (true) {
        // [失效] 最高优先: 处理失效指令(拆分子失效到RAM worker + join, 阻塞等待)
        if (pt_invalidate_fifo.num_available() > 0) {
            CacheMessage inv = pt_invalidate_fifo.read();
            dispatch_pt_invalidate(inv);
            continue;
        }

        // 乒乓调度: 根据 pt_sched_next_is_request_ 决定本轮优先处理哪类
        bool has_req = (pt_request_fifo.num_available() > 0);
        bool has_upd = (pt_update_fifo.num_available() > 0);

        if (!has_req && !has_upd) {
            wait(pt_request_fifo.data_written_event() |
                 pt_update_fifo.data_written_event() |
                 pt_invalidate_fifo.data_written_event());
            continue;
        }

        bool do_request;
        if (has_req && has_upd) {
            do_request = pt_sched_next_is_request_;  // 两者都有，按乒乓选择
        } else {
            do_request = has_req;  // 只有一个可用，处理它
        }

        CacheMessage req;
        if (do_request) {
            req = pt_request_fifo.read();
            pt_sched_next_is_request_ = false;
        } else {
            req = pt_update_fifo.read();
            pt_sched_next_is_request_ = true;
        }

#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::BusyGuard md_busy(md_active_);
#endif
        const sc_time dequeue_time = sc_time_stamp();
        const double dequeue_ns = dequeue_time.to_seconds() * 1e9;
        double fifo_wait_ns = dequeue_ns - req.timestamp.to_seconds() * 1e9;
        stats_.accumulate_phase_task_wait("pt_cache", do_request ? 0 : 1, fifo_wait_ns);

        if (do_request) {
            // [DEBUG] 保留原[PT_SCHED]日志口径(供现有分析脚本使用)
            printf("[PT_SCHED] task_id=%u -> dequeue from pt_request_fifo [entry=%.1f ns, dequeue=%.1f ns, fifo_wait=%.2f ns]\n",
                   (unsigned)req.task_id, req.timestamp.to_seconds() * 1e9, dequeue_ns, fifo_wait_ns);
            fflush(stdout);
        }

        // Hash 单元执行 1 拍 (忙)
        wait(pt_cache_->hash_stage_latency());

        uint32_t ram_id = pt_cache_->compute_ram_id(req.gscid, req.pscid, req.iova);

        // 写目标 RAM FIFO: 满则阻塞(Hash保持忙, 反压 pt_request/pt_update)
        const double wr_start_ns = sc_time_stamp().to_seconds() * 1e9;

        // [STAT] 多 RAM 分配均匀性 / 同 RAM 背靠背冲突监控
        //   same_ram_streak: 本次目标 RAM 与上一任务相同(潜在被该 RAM 原子段串行限制)
        //   b2b           : 本 RAM 相邻两次到达间隔 < 本 RAM 原子段延时(真实背靠背排队)
        {
            const double ram_atomic_ns =
                (pt_cache_->config().read_set_latency_cycles +
                 pt_cache_->config().compare_latency_cycles) *
                clock_period_.to_seconds() * 1e9;
            if (pt_last_hash_ram_id_ == static_cast<int>(ram_id))
                pt_same_ram_streak_cnt_++;
            pt_last_hash_ram_id_ = static_cast<int>(ram_id);

            if (ram_id < PT_MAX_RAMS) {
                if (pt_ram_hash_seen_[ram_id]) {
                    const double gap = wr_start_ns - pt_ram_last_hash_ns_[ram_id];
                    pt_ram_gap_total_ns_[ram_id] += gap;
                    pt_ram_gap_count_[ram_id]++;
                    if (gap < pt_ram_gap_min_ns_[ram_id]) pt_ram_gap_min_ns_[ram_id] = gap;
                    if (gap > pt_ram_gap_max_ns_[ram_id]) pt_ram_gap_max_ns_[ram_id] = gap;
                    if (gap < ram_atomic_ns) pt_ram_b2b_cnt_[ram_id]++;
                } else {
                    pt_ram_hash_seen_[ram_id] = 1;
                    pt_ram_gap_min_ns_[ram_id] = 1e18;
                }
                pt_ram_last_hash_ns_[ram_id] = wr_start_ns;
            }
        }

        if (pt_ram_fifo_[ram_id]->num_free() == 0) {
            pt_hash_backpressure_cnt_++;
            printf("[PT_HASH_BP] task_id=%u -> RAM%u FIFO full, hash unit blocked\n",
                   (unsigned)req.task_id, ram_id);
            fflush(stdout);
        }
        pt_ram_fifo_[ram_id]->write(req);  // 阻塞写 = 反压
        const double wr_end_ns = sc_time_stamp().to_seconds() * 1e9;
        pt_hash_backpressure_ns_ += (wr_end_ns - wr_start_ns);

        // [STAT] FIFO占用峰值与Hash单元统计
        int occ = pt_ram_fifo_[ram_id]->num_available();
        if (occ > pt_ram_fifo_peak_[ram_id]) pt_ram_fifo_peak_[ram_id] = occ;
        pt_hash_task_count_++;
        pt_hash_busy_ns_ += (wr_end_ns - dequeue_ns);
    }
}

// ============================================================
// [多RAM] pt_ram_worker_thread - 每组 RAM 独立 worker
// 同一 RAM FIFO 内任务串行(单线程天然保证原子段不可分割);
// 不同 RAM 组并发, 稳态下理想情况接近每周期输出一个查询结果。
// RAM 原子段: LOOKUP = read_set+compare; FILL = read_set+compute+write (不含hash)
// RAM i 独占 set 区间 [i*sets_per_ram, (i+1)*sets_per_ram), 无数据竞争。
// [STAT] 原 pt_scheduler_thread 的任务级统计(间隔/32组/区间命中率)迁移至此,
//        多worker并发下gap统计语义变为"全RAM聚合间隙", 仅供参考。
// ============================================================
void CacheSubsystem::pt_ram_worker_thread(int ram_id) {
    sc_fifo<CacheMessage>& my_fifo = *pt_ram_fifo_[ram_id];
    uint64_t pt_req_counter = 0;  // [STAT] 本RAM的REQUEST区间统计计数(区间号全局另算)
    (void)pt_req_counter;
    while (true) {
        CacheMessage req = my_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::BusyGuard md_busy(md_active_);
#endif
        const sc_time dequeue_time = sc_time_stamp();
        const double start_ns = dequeue_time.to_seconds() * 1e9;
        const bool is_update = (req.msg_type == CacheMsgType::PT_UPDATE);

        // [失效] 失效子任务: 在本 RAM 串行执行(与 in-flight lookup/fill 原子)
        if (req.msg_type == CacheMsgType::PT_INVALIDATE) {
            sc_time inv_latency = SC_ZERO_TIME;
            uint32_t affected = 0;
            if (req.inval_lazy_sweep) {
                affected = pt_cache_->lazy_sweep_ram(static_cast<uint32_t>(ram_id),
                                                     pt_inval_sweep_new_vn_, inv_latency);
            } else if (req.inval_sub_has_set) {
                affected = pt_cache_->invalidate_set_ram(req.inval_sub_set, req, inv_latency);
            } else {
                affected = pt_cache_->invalidate_ram_range(static_cast<uint32_t>(ram_id),
                                                           req, inv_latency);
            }
            wait(inv_latency);
            pt_inval_affected_ += affected;
            if (pt_inval_remaining_ > 0) pt_inval_remaining_--;
            pt_inval_sub_done_event_.notify(SC_ZERO_TIME);

            const double iend_ns = sc_time_stamp().to_seconds() * 1e9;
            pt_ram_task_count_[ram_id]++;
            pt_ram_busy_ns_[ram_id] += (iend_ns - start_ns);
            if (pt_ram_first_start_ns_ < 0.0) pt_ram_first_start_ns_ = start_ns;
            if (iend_ns > pt_ram_last_end_ns_) pt_ram_last_end_ns_ = iend_ns;
            continue;
        }

        if (!is_update) {
            // =============================================================
            // 处理 REQUEST (LOOKUP)
            // =============================================================
            trace_task_event("begin", "pt_cache", "lookup", req, dequeue_time);
            pt_cache_->set_current_phase(0);

            CacheMessage resp;
            resp.msg_type = CacheMsgType::CACHE_RESPONSE;
            resp.task_id = req.task_id;
            resp.iova = req.iova;  // FIX: 复制IOVA到响应
            resp.gscid = req.gscid;
            resp.pscid = req.pscid;

            PTData data;
            sc_time ram_latency = SC_ZERO_TIME;
            resp.hit = pt_cache_->lookup_pt_ram(req.gscid, req.pscid, req.iova,
                                                req.stage, req.pt_sv48,
                                                req.pt_gstage_x4,
                                                data, ram_latency);
            if (resp.hit) {
                resp.stage = pt_stage(data);
                resp.from_prefetch = pt_from_prefetch(data);
                resp.pt_data = data;
            }
            resp.latency = pt_cache_->hash_stage_latency() + ram_latency;

            // RAM 原子段延时(同RAM串行, 跨RAM并发)
            wait(ram_latency);

            const sc_time end = sc_time_stamp();
            const double end_ns = end.to_seconds() * 1e9;
            double exec_ns = end_ns - start_ns;

            // [STAT] 全局间隔分析(多RAM下为聚合口径)
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
#if TEST_CFG_MULTI_DEVICE_SCENE
            md_record(req, "pt_cache_ram", "lookup", start_ns, resp.hit, ram_id);
#endif

            // 区间命中率统计(全局REQUEST序号)
            int interval_idx = static_cast<int>(pt_sched_req_count_ / 1000);
            if (resp.hit) stats_.record_interval_hit("pt_cache", interval_idx);
            else           stats_.record_interval_miss("pt_cache", interval_idx);

            if (resp.hit) {
                // HIT 常规CL: 直接返回
                push_fifo(pt_hit_response_fifo, resp);
            } else if (req.dedup_bypass) {
                // [MSI] MSI使能设备旁路dedup: 不插占位CL/不挂Buffer, 直接产生
                // MISS响应送PTW; MSI命中后任务改道MSIPT路径, 不依赖Buffer刷新
                CacheMessage bypass_resp;
                bypass_resp.msg_type = CacheMsgType::CACHE_RESPONSE;
                bypass_resp.task_id = req.task_id;
                bypass_resp.iova = req.iova;
                bypass_resp.gscid = req.gscid;
                bypass_resp.pscid = req.pscid;
                bypass_resp.stage = req.stage;
                bypass_resp.dedup_bypass = true;
                bypass_resp.dedup_head_index = 0xFFFF;
                bypass_resp.prefetch_enabled = false;
                bypass_resp.prefetch_depth = 0;
                push_fifo(pt_miss_response_fifo, bypass_resp);
            } else {
                // [重构] MISS: 转发到 dedup_request_fifo (携带原请求上下文与预取参数)
                CacheMessage dedup_req = req;
                dedup_req.timestamp = sc_time_stamp();
                push_fifo(dedup_request_fifo, dedup_req);
            }

            // =============================================================
            // [STAT] 32任务组统计 - REQUEST完成(跨RAM共享, 协作式调度下安全)
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

            // [STAT] 本RAM统计
            pt_ram_lookup_count_[ram_id]++;
            // [STAT] 仅数据包计数(SQ/CQ/MSI 控制包不计入)
            if (!req.is_ctrl_pkt) pt_ram_data_lookup_count_[ram_id]++;
            // [STAT] per-RAM 真实命中计数(全量 + 仅数据包)
            if (resp.hit) {
                pt_ram_lookup_hit_count_[ram_id]++;
                if (!req.is_ctrl_pkt) pt_ram_data_lookup_hit_count_[ram_id]++;
            }
        } else {
            // =============================================================
            // 处理 UPDATE (FILL)
            // =============================================================
            trace_task_event("begin", "pt_cache", "update", req, dequeue_time);
            pt_cache_->set_current_phase(1);

            sc_time ram_latency = SC_ZERO_TIME;
            pt_cache_->fill_pt_ram(req.gscid, req.pscid, req.iova, req.stage,
                                   req.pt_data, req.from_prefetch, ram_latency);

            CacheMessage upd_resp;
            upd_resp.msg_type = CacheMsgType::CACHE_UPDATE_RESPONSE;
            upd_resp.task_id = req.task_id;

            // RAM 原子段延时
            wait(ram_latency);

            const sc_time upd_end = sc_time_stamp();
            const double upd_end_ns = upd_end.to_seconds() * 1e9;
            double upd_exec_ns = upd_end_ns - start_ns;

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
            pt_sched_last_end_ns_ = upd_end_ns;
            pt_sched_total_exec_ns_ += upd_exec_ns;
            pt_sched_task_count_++;
            pt_sched_upd_count_++;
            pt_sched_last_task_end_ = upd_end_ns;

            record_task_completion("pt_cache", dequeue_time, upd_end);
            stats_.record_pt_phase_timestamp("pt_cache", 1, start_ns, upd_end_ns);
            trace_task_event("end", "pt_cache", "update", req, dequeue_time, &upd_resp);

            // [STAT] 累加到32任务组的排队统计（UPDATE执行时间计入REQUEST的排队延时）
            pt_group_upd_since_last_ += upd_exec_ns;
            pt_group_upd_cnt_since_++;

            // [STAT] 本RAM统计
            pt_ram_update_count_[ram_id]++;
            // [STAT] 仅数据包计数(SQ/CQ/MSI 控制包不计入)
            if (!req.is_ctrl_pkt) pt_ram_data_update_count_[ram_id]++;
        }

        // [STAT] 本RAM公共统计
        const double task_end_ns = sc_time_stamp().to_seconds() * 1e9;
        pt_ram_task_count_[ram_id]++;
        pt_ram_busy_ns_[ram_id] += (task_end_ns - start_ns);
        if (pt_ram_first_start_ns_ < 0.0) pt_ram_first_start_ns_ = start_ns;
        if (task_end_ns > pt_ram_last_end_ns_) pt_ram_last_end_ns_ = task_end_ns;
    }
}

// ============================================================
// [dedup多RAM] dedup_scheduler_thread - 去重 Cache 纯调度线程
// 轮询3入口FIFO: dedup_request_fifo / dedup_inside_request_fifo / dedup_update_fifo
//   - [机制v2] 严格优先级仲裁: UPDATE > inside(inter预取) > REQUEST(原乒乓废除)
//   - UPDATE为释放路径, 严格最高优先保证Buffer释放不被查询饿死
// 选中后阻塞写 dedup_hash_in_fifo(深度1): 写满=Hash忙=反压三个入口FIFO。
// Hash单元空闲即可读下一任务(不等前一任务RAM原子段完成)。
// ============================================================
void CacheSubsystem::dedup_scheduler_thread() {
    while (true) {
        bool has_req = (dedup_request_fifo.num_available() > 0);
        bool has_ins = (dedup_inside_request_fifo.num_available() > 0);
        bool has_upd = (dedup_update_fifo.num_available() > 0);

        // [STAT] dedup_request_fifo占用峰值(准入控制模式下反压积压观测)
        {
            int occ = dedup_request_fifo.num_available();
            if (occ > dedup_request_fifo_peak_) dedup_request_fifo_peak_ = occ;
        }

        // [准入控制] Buffer满(valid+reserved>=SIZE)时本轮不出队request:
        //   任务滞留dedup_request_fifo形成反压(非阻塞跳过), scheduler继续服务inside/update,
        //   保证释放路径(update->flush->free_entry)始终畅通, 打破环形等待死锁环。
        const bool req_pending_raw = has_req;
#if TEST_CFG_DEDUP_ADMISSION_CTRL
        if (has_req && !dedup_admission_ok()) {
            has_req = false;  // 仲裁视角本轮request不可用(跳过)
#if TEST_CFG_DEDUP_DELAYED_UPDATE
            // [去重延迟更新] 条件3: Buffer满, 通知排空线程加速转出延迟FIFO释放Buffer
            dedup_buffer_full_notify_ev_.notify();
#endif
        }
#endif

        if (!has_req && !has_ins && !has_upd) {
#if TEST_CFG_DEDUP_ADMISSION_CTRL
            if (req_pending_raw) {
                // 准入停滞: request积压但Buffer满, 组合事件等待(释放事件|三FIFO写入),
                // 避免只等free_event时错过update到达(update须经本线程才能变成free_entry)
                dedup_admission_wait_cnt_++;
                const double t0 = sc_time_stamp().to_seconds() * 1e9;
                wait(dedup_request_fifo.data_written_event() |
                     dedup_inside_request_fifo.data_written_event() |
                     dedup_update_fifo.data_written_event() |
                     dedup_buffer_->free_event);
                dedup_admission_wait_ns_ += sc_time_stamp().to_seconds() * 1e9 - t0;
                continue;
            }
#endif
            wait(dedup_request_fifo.data_written_event() |
                 dedup_inside_request_fifo.data_written_event() |
                 dedup_update_fifo.data_written_event());
            continue;
        }

        // [机制v2] 严格优先级仲裁: UPDATE > REQUEST类(类内 inside 优先于 request)
        bool any_request = has_req || has_ins;
        bool do_request;
#if TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
        // ============================================================
        // [释放优先通道 20261109] 出口按类型分流 + "先验空位, 再出队"
        //   旧行为: 无论选中哪一类, 都阻塞写同一个深度1的 dedup_hash_in_fifo。
        //     一旦 hash 线程被某个 RAM 的 request FIFO 顶住, scheduler 就卡死在这次
        //     写上, 连 UPDATE 也取不出来 => free_entry 停摆 => Buffer 永不释放
        //     => 准入永久关闭 => 环形等待死锁(8盘8通道/16盘8通道实测)。
        //   新行为: UPDATE 走 dedup_upd_hash_in_fifo(独立深度1), REQUEST 走原出口;
        //     且先确认目标出口有空位再从源 FIFO 出队 => 出口满时本轮不下发、
        //     不阻塞, 回到仲裁重选。因为两个出口都**只由本线程写入**, 且
        //     num_free() 与 write() 之间无任何 wait, 故 write() 结构上不会阻塞
        //     => "深度1出口绝不能被堵住" 得到保证。
        //   仲裁优先级为[机制v2]严格优先(UPDATE>REQUEST类), 准入前置条件保持原语义。
        // ============================================================
        const bool can_req = any_request && (dedup_hash_in_fifo.num_free() > 0);
        const bool can_upd = has_upd     && (dedup_upd_hash_in_fifo.num_free() > 0);
        if (!can_req && !can_upd) {
            // 有消息但两侧出口均满 => 等任一出口腾出空位或任一源写入(纯事件等待,
            //   无忙轮询)。注: 三个入口 FIFO 均仅由本线程消费, 不会被他人抢走。
            wait(dedup_hash_in_fifo.data_read_event() |
                 dedup_upd_hash_in_fifo.data_read_event() |
                 dedup_request_fifo.data_written_event() |
                 dedup_inside_request_fifo.data_written_event() |
                 dedup_update_fifo.data_written_event());
            continue;
        }
        // [机制v2] UPDATE严格优先(各开关分支统一, 原乒乓废除)
        do_request = can_req && !can_upd;
#else   // !TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
        // [机制v2] UPDATE严格优先(各开关分支统一, 原乒乓废除);
        //   REQUEST类内inside优先不变, 合成为 UPD > INS > REQ。
        do_request = any_request && !has_upd;
#endif  // TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE

        CacheMessage msg;
        if (do_request) {
            // [机制v2] REQUEST类内: inside(inter预取)严格优先于主查询
            if (has_ins) {
                msg = dedup_inside_request_fifo.read();
            } else {
                msg = dedup_request_fifo.read();
#if TEST_CFG_DEDUP_ADMISSION_CTRL
                // 准入预留: 预占1个未来Buffer槽(read前已判准入, 此处无wait, 天然原子)
                dedup_reserved_count_++;
                if (dedup_reserved_count_ > dedup_reserved_peak_) {
                    dedup_reserved_peak_ = dedup_reserved_count_;
                }
#endif
            }
        } else {
            msg = dedup_update_fifo.read();
        }

        // 下发Hash单元
#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::BusyGuard md_busy(md_active_);
#endif
#if TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
        // 分流: UPDATE 走独立释放出口。上面已验过空位, 两个 write 均不会阻塞。
        if (do_request) dedup_hash_in_fifo.write(msg);
        else            dedup_upd_hash_in_fifo.write(msg);
#else
        dedup_hash_in_fifo.write(msg);   // 阻塞写 = hash忙时scheduler阻塞(反压上游)
#endif
    }
}

// ============================================================
// [去重延迟更新] 统一入队封装 dedup_update_enqueue
//   开关off: 直写 dedup_update_fifo(旧方案, 逐位不变)。
//   开关on : 写入带每条目定时器的延迟FIFO(deque), 由排空线程按三条件转出:
//     c1 每条目定时器到期(入队即起表, DEDUP_DELAYED_UPDATE_TIMER_NS);
//     c2 入队后占用 > (深度-margin): 置压力标志异步排出一组(D+1)笔;
//     c3 去重Buffer满(scheduler/worker通知, 排空线程唤醒后复核)。
//   硬满(=深度)时push端等space_ev(极端反压, 计数观测), 尽量不反压PTW写入。
// ============================================================
void CacheSubsystem::dedup_update_enqueue(CacheMessage& msg) {
#if TEST_CFG_DEDUP_DELAYED_UPDATE
    const double now_ns = sc_time_stamp().to_seconds() * 1e9;
    // 极端反压: 硬满且排空线程尚未让位, 等待space_ev
    if (delayed_upd_deque_.size() >= DEDUP_DELAYED_UPDATE_FIFO_DEPTH) {
        delayed_upd_push_block_cnt_++;
        const double t0 = now_ns;
        while (delayed_upd_deque_.size() >= DEDUP_DELAYED_UPDATE_FIFO_DEPTH)
            wait(delayed_upd_space_ev_);
        const double blk = sc_time_stamp().to_seconds() * 1e9 - t0;
        if (blk > delayed_upd_push_block_max_ns_) delayed_upd_push_block_max_ns_ = blk;
    }
    delayed_upd_sample_occ();  // 结算入队前占用积分段
    DelayedUpdEntry e;
    e.msg      = msg;
    e.enq_ns   = now_ns;
    e.deadline = sc_time_stamp() + sc_time(DEDUP_DELAYED_UPDATE_TIMER_NS, SC_NS);
    delayed_upd_deque_.push_back(e);
    delayed_upd_enq_total_++;
    // [STAT] 写入时采样当前占用(含本笔, 到达加权平均; 出队不采样)
    delayed_upd_push_occ_sum_ += delayed_upd_deque_.size();
    delayed_upd_push_cnt_++;
    if (delayed_upd_first_enq_ns_ < 0.0) delayed_upd_first_enq_ns_ = now_ns;
    if ((int)delayed_upd_deque_.size() > delayed_upd_occ_peak_)
        delayed_upd_occ_peak_ = (int)delayed_upd_deque_.size();
    // c1: 每笔入队同步起表(PEQ式有序定时事件, 到期触发排空线程复核队头deadline)
    delayed_upd_timer_eq_.notify(sc_time(DEDUP_DELAYED_UPDATE_TIMER_NS, SC_NS));
    // c2: 占用超过(深度-margin)阈值, 置压力标志并唤醒排空线程排出一组
    if (delayed_upd_deque_.size() >
        (DEDUP_DELAYED_UPDATE_FIFO_DEPTH - DEDUP_DELAYED_PRESSURE_MARGIN)) {
        delayed_pressure_pending_ = true;
        delayed_upd_pressure_ev_.notify();
    }
#else
    dedup_update_fifo.write(msg);  // 旧方案: PTW 直写 dedup_update_fifo
#endif
}

#if TEST_CFG_DEDUP_DELAYED_UPDATE
// ============================================================
// [去重延迟更新] 排空线程: dedup_delayed_update_fifo -> dedup_update_fifo
// 单OR等待同时兼顾三条件(回答"PEQ定时器如何兼顾条件2/3"):
//   定时器事件(sc_event_queue每笔入队notify一次) | 压力事件 | Buffer满事件。
//   唤醒后按 c2 -> c3 -> c1 顺序处理:
//     c2 消费压力标志, 排出一组(D+1)笔;
//     c3 复核 !dedup_admission_ok()(防事件残留误排空), 排出一组;
//     c1 队头优先, 排出全部已到期条目(过期空醒时deadline复核天然幂等)。
// 死锁自检: 本线程只等自有事件; 写dedup_update_fifo虽阻塞但消费者是独立
//   scheduler线程; push端只等space_ev(由本线程notify)。无循环等待。
// ============================================================
void CacheSubsystem::dedup_delayed_update_drain_thread() {
    const uint32_t grp = PT_DEDUP_PREFETCH_DEPTH + 1;  // 主任务+D预取=4
    while (true) {
        wait(delayed_upd_timer_eq_.default_event() |
             delayed_upd_pressure_ev_ |
             dedup_buffer_full_notify_ev_);
        // c2: 压力阈值(入队端置标志)
        if (delayed_pressure_pending_) {
            delayed_pressure_pending_ = false;
            if (!delayed_upd_deque_.empty()) {
                const uint32_t sz = (uint32_t)delayed_upd_deque_.size();
                delayed_upd_drain_group(grp < sz ? grp : sz, 2);
                continue;
            }
        }
        // c3: Buffer满(唤醒时复核)
#if TEST_CFG_DEDUP_UNIT_V1123
        // [1123] 条件3口径: Buffer已拆4 bank, 需"全部bank满"才触发加速排空
        //   (任一bank满时由该bank worker门控req/inter, 释放仍走无门控的upd通道)。
        if (!delayed_upd_deque_.empty() && banks_all_full()) {
#else
        if (!delayed_upd_deque_.empty() && !dedup_admission_ok()) {
#endif
            const uint32_t sz = (uint32_t)delayed_upd_deque_.size();
            delayed_upd_drain_group(grp < sz ? grp : sz, 3);
            continue;
        }
        // c1: 队头优先, 排出全部到期条目
        while (!delayed_upd_deque_.empty() &&
               sc_time_stamp() >= delayed_upd_deque_.front().deadline) {
            delayed_upd_drain_group(1, 1);
        }
    }
}

// [去重延迟更新] 从队头弹出n笔转入dedup_update_fifo(阻塞写: 消费者scheduler独立);
//   每弹一笔notify space_ev给硬满push端让位; 统计出队条件分类与条目驻留时长。
void CacheSubsystem::delayed_upd_drain_group(uint32_t n, int cond) {
    delayed_upd_sample_occ();  // 结算弹出前占用积分段
    for (uint32_t i = 0; i < n && !delayed_upd_deque_.empty(); i++) {
        DelayedUpdEntry e = delayed_upd_deque_.front();
        delayed_upd_deque_.pop_front();
        const double now_ns = sc_time_stamp().to_seconds() * 1e9;
        const double resid  = now_ns - e.enq_ns;
        delayed_upd_residency_total_ns_ += resid;
        if (resid > delayed_upd_residency_max_ns_) delayed_upd_residency_max_ns_ = resid;
        if (cond == 1)      delayed_upd_drain_c1_++;
        else if (cond == 2) delayed_upd_drain_c2_++;
        else                delayed_upd_drain_c3_++;
        dedup_update_fifo.write(e.msg);
        delayed_upd_space_ev_.notify();
    }
    if (cond != 1) delayed_upd_group_cnt_++;
}

// [去重延迟更新] 占用时间加权积分: 每次size变化前结算上一段(旧size x 时长)
void CacheSubsystem::delayed_upd_sample_occ() {
    const double now_ns = sc_time_stamp().to_seconds() * 1e9;
    if (delayed_upd_occ_last_ns_ >= 0.0)
        delayed_upd_occ_weighted_ns_ +=
            (double)delayed_upd_deque_.size() * (now_ns - delayed_upd_occ_last_ns_);
    delayed_upd_occ_last_ns_ = now_ns;
}
#endif // TEST_CFG_DEDUP_DELAYED_UPDATE

// ============================================================
// [dedup多RAM] dedup_hash_process_thread - 去重 Cache Hash 单元线程
// 读 dedup_hash_in_fifo -> wait(hash 1拍) -> ram_id = org_hash & (num_rams-1)
//   -> 写 dedup_ram_fifo_[ram_id] (满则阻塞, Hash保持忙, 反压scheduler)
// [释放优先通道] 开关on时额外服务 dedup_upd_hash_in_fifo(UPDATE 严格优先),
//   并按消息类型投递到 dedup_ram_upd_fifo_[ram_id] => UPDATE 不会排在某个
//   已满的 request RAM FIFO 之后(消除共享单 hash 线程带来的队头阻塞)。
// ============================================================
void CacheSubsystem::dedup_hash_process_thread() {
    while (true) {
#if TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
        // UPDATE 严格优先(对齐 walker_ram_worker_thread 的 nb_read + 组合事件写法);
        //   两侧都空则等任一写入。本线程是两条 hash_in 的唯一消费者。
        CacheMessage msg;
        bool is_upd = dedup_upd_hash_in_fifo.nb_read(msg);
        if (!is_upd && !dedup_hash_in_fifo.nb_read(msg)) {
            wait(dedup_upd_hash_in_fifo.data_written_event() |
                 dedup_hash_in_fifo.data_written_event());
            continue;
        }
#else
        CacheMessage msg = dedup_hash_in_fifo.read();
#endif
#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::BusyGuard md_busy(md_active_);
#endif
        const double dequeue_ns = sc_time_stamp().to_seconds() * 1e9;

        // Hash 单元执行 1 拍 (忙)
        wait(clock_period_ * static_cast<int>(DEDUP_HASH_CYCLES));

        uint32_t ram_id = dedup_cache_->compute_ram_id(msg.gscid, msg.pscid, msg.iova);

#if TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
        // 按通道类型选目标 FIFO: update 有自己的 per-RAM FIFO, 不与 request 争位。
        sc_fifo<CacheMessage>& target =
            is_upd ? *dedup_ram_upd_fifo_[ram_id] : *dedup_ram_fifo_[ram_id];
        // 写目标 RAM FIFO: 满则阻塞(Hash保持忙, 反压上游)
        const double wr_start_ns = sc_time_stamp().to_seconds() * 1e9;
        if (target.num_free() == 0) {
            dedup_hash_backpressure_cnt_++;
            printf("[DEDUP_HASH_BP] task_id=%u -> RAM%u %sFIFO full, hash unit blocked\n",
                   (unsigned)msg.task_id, ram_id, is_upd ? "UPD " : "");
            fflush(stdout);
        }
        target.write(msg);  // 阻塞写 = 反压
        const double wr_end_ns = sc_time_stamp().to_seconds() * 1e9;
        dedup_hash_backpressure_ns_ += (wr_end_ns - wr_start_ns);

        // [STAT] FIFO占用峰值与Hash单元统计(峰值只统计 request 通道, 与旧口径一致)
        if (!is_upd) {
            int occ = dedup_ram_fifo_[ram_id]->num_available();
            if (occ > dedup_ram_fifo_peak_[ram_id]) dedup_ram_fifo_peak_[ram_id] = occ;
            dedup_ram_push_occ_sum_[ram_id] += occ;
            dedup_ram_push_cnt_[ram_id]++;
        }
        dedup_hash_task_count_++;
        dedup_hash_busy_ns_ += (wr_end_ns - dequeue_ns);
#else
        // 写目标 RAM FIFO: 满则阻塞(Hash保持忙, 反压上游)
        const double wr_start_ns = sc_time_stamp().to_seconds() * 1e9;
        if (dedup_ram_fifo_[ram_id]->num_free() == 0) {
            dedup_hash_backpressure_cnt_++;
            printf("[DEDUP_HASH_BP] task_id=%u -> RAM%u FIFO full, hash unit blocked\n",
                   (unsigned)msg.task_id, ram_id);
            fflush(stdout);
        }
        dedup_ram_fifo_[ram_id]->write(msg);  // 阻塞写 = 反压
        const double wr_end_ns = sc_time_stamp().to_seconds() * 1e9;
        dedup_hash_backpressure_ns_ += (wr_end_ns - wr_start_ns);

        // [STAT] FIFO占用峰值与Hash单元统计
        int occ = dedup_ram_fifo_[ram_id]->num_available();
        if (occ > dedup_ram_fifo_peak_[ram_id]) dedup_ram_fifo_peak_[ram_id] = occ;
        dedup_ram_push_occ_sum_[ram_id] += occ;
        dedup_ram_push_cnt_[ram_id]++;
        dedup_hash_task_count_++;
        dedup_hash_busy_ns_ += (wr_end_ns - dequeue_ns);
#endif
    }
}

#if TEST_CFG_DEDUP_UNIT_V1123
// ============================================================
// [去重单元机制优化 1123] 三个类型化 Hash 单元线程
//   req:   dedup_request_fifo(8)  -> hash 1ns -> dedup_ram_fifo_[ram](32)
//   inter: dedup_inside_request_fifo(1) -> hash 1ns -> dedup_ram_inter_fifo_[ram](8)
//   upd:   dedup_update_fifo(24衔接, 由drain线程写入) -> hash 1ns -> dedup_ram_upd_fifo_[ram](2)
// 门控(需求4(6)): req/inter hash 在"4 bank Buffer全满"时不出队(反压最外层队列);
//   upd hash 永无门控(G2: 释放通路独立)。目标RAM FIFO满则阻塞写=自然反压上游
//   (req hash 被堵住会经 dedup_request_fifo(8) 传导至 pt_ram_worker 的MISS转发)。
// 死锁自检: worker 不阻塞在任何写入上(预取生成nb_write满则丢G1, 响应写经staging
//   解耦G3), 故任一RAM FIFO最多被"上游hash+在途原子段"有限顶住, 无环形等待。
// ============================================================
void CacheSubsystem::dedup_req_hash_thread() {
    while (true) {
        if (dedup_request_fifo.num_available() == 0) {
            wait(dedup_request_fifo.data_written_event());
            continue;
        }
        if (banks_all_full()) {
            // 外层req出队反压(4 bank全满); 同时唤醒排空线程复核条件3
            dedup_buffer_full_notify_ev_.notify();
            wait(dedup_banks_[0]->free_event | dedup_banks_[1]->free_event |
                 dedup_banks_[2]->free_event | dedup_banks_[3]->free_event |
                 dedup_admission_released_ev_ |
                 dedup_request_fifo.data_written_event());
            continue;
        }
        CacheMessage msg = dedup_request_fifo.read();
        {
            int occ = dedup_request_fifo.num_available();
            if (occ > dedup_request_fifo_peak_) dedup_request_fifo_peak_ = occ;
        }
        const double dequeue_ns = sc_time_stamp().to_seconds() * 1e9;
        wait(clock_period_ * static_cast<int>(DEDUP_HASH_CYCLES));
        uint32_t ram_id = dedup_cache_->compute_ram_id(msg.gscid, msg.pscid, msg.iova);
        sc_fifo<CacheMessage>& target = *dedup_ram_fifo_[ram_id];
        const double wr_start_ns = sc_time_stamp().to_seconds() * 1e9;
        if (target.num_free() == 0) {
            dedup_v1123_hash_bp_cnt_[0]++;
            printf("[DEDUP_HASH_BP] task_id=%u -> RAM%u reqFIFO full, req-hash blocked\n",
                   (unsigned)msg.task_id, ram_id);
            fflush(stdout);
        }
        target.write(msg);  // 阻塞写 = 反压
        const double wr_end_ns = sc_time_stamp().to_seconds() * 1e9;
        dedup_v1123_hash_bp_ns_[0] += (wr_end_ns - wr_start_ns);
        dedup_v1123_hash_cnt_[0]++;
        dedup_v1123_hash_busy_ns_[0] += (wr_end_ns - dequeue_ns);
        int occ = target.num_available();
        if (occ > dedup_ram_fifo_peak_[ram_id]) dedup_ram_fifo_peak_[ram_id] = occ;
        dedup_ram_push_occ_sum_[ram_id] += occ;
        dedup_ram_push_cnt_[ram_id]++;
    }
}

void CacheSubsystem::dedup_inter_hash_thread() {
    while (true) {
        if (dedup_inside_request_fifo.num_available() == 0) {
            wait(dedup_inside_request_fifo.data_written_event());
            continue;
        }
        if (banks_all_full()) {
            // 外层inter(预取)出队反压(4 bank全满, 需求4(6)); 预取不占Buffer但仍受门控(A3)
            dedup_buffer_full_notify_ev_.notify();
            wait(dedup_banks_[0]->free_event | dedup_banks_[1]->free_event |
                 dedup_banks_[2]->free_event | dedup_banks_[3]->free_event |
                 dedup_admission_released_ev_ |
                 dedup_inside_request_fifo.data_written_event());
            continue;
        }
        CacheMessage msg = dedup_inside_request_fifo.read();
        const double dequeue_ns = sc_time_stamp().to_seconds() * 1e9;
        wait(clock_period_ * static_cast<int>(DEDUP_HASH_CYCLES));
        uint32_t ram_id = dedup_cache_->compute_ram_id(msg.gscid, msg.pscid, msg.iova);
        sc_fifo<CacheMessage>& target = *dedup_ram_inter_fifo_[ram_id];
        const double wr_start_ns = sc_time_stamp().to_seconds() * 1e9;
        if (target.num_free() == 0) {
            dedup_v1123_hash_bp_cnt_[1]++;
            printf("[DEDUP_HASH_BP] task_id=%u -> RAM%u interFIFO full, inter-hash blocked\n",
                   (unsigned)msg.task_id, ram_id);
            fflush(stdout);
        }
        target.write(msg);
        const double wr_end_ns = sc_time_stamp().to_seconds() * 1e9;
        dedup_v1123_hash_bp_ns_[1] += (wr_end_ns - wr_start_ns);
        dedup_v1123_hash_cnt_[1]++;
        dedup_v1123_hash_busy_ns_[1] += (wr_end_ns - dequeue_ns);
        int occ = target.num_available();
        if (occ > dedup_ram_inter_fifo_peak_[ram_id]) dedup_ram_inter_fifo_peak_[ram_id] = occ;
        dedup_ram_inter_push_occ_sum_[ram_id] += occ;   // [写入加权] push后采样占用
        dedup_ram_inter_push_cnt_[ram_id]++;
    }
}

void CacheSubsystem::dedup_upd_hash_thread() {
    while (true) {
        CacheMessage msg = dedup_update_fifo.read();  // 阻塞读(生产者=delayed排空线程)
        const double dequeue_ns = sc_time_stamp().to_seconds() * 1e9;
        wait(clock_period_ * static_cast<int>(DEDUP_HASH_CYCLES));
        uint32_t ram_id = dedup_cache_->compute_ram_id(msg.gscid, msg.pscid, msg.iova);
        sc_fifo<CacheMessage>& target = *dedup_ram_upd_fifo_[ram_id];
        const double wr_start_ns = sc_time_stamp().to_seconds() * 1e9;
        if (target.num_free() == 0) {
            dedup_v1123_hash_bp_cnt_[2]++;
            printf("[DEDUP_HASH_BP] task_id=%u -> RAM%u updFIFO full, upd-hash blocked\n",
                   (unsigned)msg.task_id, ram_id);
            fflush(stdout);
        }
        target.write(msg);
        const double wr_end_ns = sc_time_stamp().to_seconds() * 1e9;
        dedup_v1123_hash_bp_ns_[2] += (wr_end_ns - wr_start_ns);
        dedup_v1123_hash_cnt_[2]++;
        dedup_v1123_hash_busy_ns_[2] += (wr_end_ns - dequeue_ns);
        int occ = target.num_available();
        if (occ > dedup_ram_upd_fifo_peak_[ram_id]) dedup_ram_upd_fifo_peak_[ram_id] = occ;
        dedup_ram_upd_push_occ_sum_[ram_id] += occ;     // [写入加权] push后采样占用
        dedup_ram_upd_push_cnt_[ram_id]++;
    }
}

// [G3] 每bank响应写出线程: staging -> pt_hit/pt_miss_response_fifo。
//   worker 的响应写只落在 staging(64, nb_write)上, 本线程承担对响应FIFO的阻塞反压;
//   本线程不持有任何 upd 出队路径所需资源, 环不闭合。bank内保序, 零延时衔接。
void CacheSubsystem::dedup_rsp_writer_thread(int bank_id) {
    sc_fifo<CacheMessage>& stage = *dedup_rsp_stage_fifo_[bank_id];
    while (true) {
        CacheMessage resp = stage.read();
        if (resp.dedup_suspended) {
            push_fifo(pt_hit_response_fifo, resp);
        } else {
            push_fifo(pt_miss_response_fifo, resp);
        }
    }
}
#endif // TEST_CFG_DEDUP_UNIT_V1123

// ============================================================
// [dedup多RAM] dedup_ram_worker_thread - 每组 RAM 独立 worker
// 同一 RAM FIFO 内任务串行(单线程天然保证原子段不可分割), 跨 RAM 并发。
// [机制v2] REQUEST/inter原子段 = get_set(2)+get_free_line(2)+write(1) = 5cyc;
//   UPDATE原子段 = 3cyc(get set/hit判断) + 刷新链长 × DEDUP_UPD_FLUSH_CYCLES,
//   原子段期间独占本RAM, 阻塞同RAM后续查询。
// 任务类型:
//   UPDATE(msg_type=PT_UPDATE): lookup + (主占位)flush回调 + clear_line
//   预取REQUEST(dedup_is_prefetch=1): lookup已存在则跳过, 否则insert预取占位; 无响应
//   普通REQUEST: 分支1/2/3处理(execute_dedup_request) + 响应路由
// RAM i 独占 set 区间 [i*sets_per_ram, (i+1)*sets_per_ram), 无数据竞争。
// Buffer满阻塞仅影响本RAM(其余RAM继续工作)。
// ============================================================
void CacheSubsystem::dedup_ram_worker_thread(int ram_id) {
    sc_fifo<CacheMessage>& my_fifo = *dedup_ram_fifo_[ram_id];
#if TEST_CFG_DEDUP_UNIT_V1123
    sc_fifo<CacheMessage>& my_upd_fifo = *dedup_ram_upd_fifo_[ram_id];
    sc_fifo<CacheMessage>& my_pf_fifo  = *dedup_ram_inter_fifo_[ram_id];
    DedupBuffer* my_bank               = dedup_banks_[ram_id];
#elif TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
    sc_fifo<CacheMessage>& my_upd_fifo = *dedup_ram_upd_fifo_[ram_id];
#endif
    while (true) {
#if TEST_CFG_DEDUP_UNIT_V1123
        // ============================================================
        // [1123] RAM内严格优先仲裁: upd > inter(预取) > req(主请求), 同RAM完全顺序。
        //   门控(需求4(6)): 本bank Buffer满(valid+reserved>=128)时 req/inter 不出队
        //     (非阻塞跳过, 任务滞留各自RAM FIFO); upd 出队永不受门控(G2, 释放通路)。
        //   准入预留: req 出队时 reserved++(检查与出队间无wait, 协作式原子),
        //     execute_dedup_request 内转 valid 或 bypass 归还。
        // ============================================================
        CacheMessage msg;
        bool have = false;
        if ((have = my_upd_fifo.nb_read(msg))) {
            // upd 最高优先且无门控
        } else if (bank_admission_ok(ram_id) && my_pf_fifo.nb_read(msg)) {
            have = true;
        } else if (bank_admission_ok(ram_id) && my_fifo.nb_read(msg)) {
            have = true;
            dedup_reserved_bank_[ram_id]++;
            if (dedup_reserved_bank_[ram_id] > dedup_reserved_bank_peak_[ram_id])
                dedup_reserved_bank_peak_[ram_id] = dedup_reserved_bank_[ram_id];
        }
        if (!have) {
            const bool gated = !bank_admission_ok(ram_id);
            if (gated) {
                dedup_bank_gate_cnt_[ram_id]++;
                // 条件3唤醒(排空线程复核"4 bank全满"才加速转出)
                dedup_buffer_full_notify_ev_.notify();
                wait(my_upd_fifo.data_written_event() | my_fifo.data_written_event() |
                     my_pf_fifo.data_written_event() | my_bank->free_event |
                     dedup_admission_released_ev_);
            } else {
                wait(my_upd_fifo.data_written_event() | my_fifo.data_written_event() |
                     my_pf_fifo.data_written_event());
            }
            continue;
        }
#elif TEST_CFG_DEDUP_UPDATE_PRIORITY_LANE
        // [释放优先通道] update 高优先(对齐 walker_ram_worker_thread 的同名机制:
        //   "保证Walker更新不被积压的lookup延迟") => Buffer 刷新/清占位不会被
        //   积压的 lookup 拖死, 释放通路在单个 RAM 内部也得到保障。
        CacheMessage msg;
        if (!my_upd_fifo.nb_read(msg) && !my_fifo.nb_read(msg)) {
            wait(my_upd_fifo.data_written_event() | my_fifo.data_written_event());
            continue;
        }
#else
        CacheMessage msg = my_fifo.read();
#endif
#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::BusyGuard md_busy(md_active_);
#endif
        const sc_time dequeue_time = sc_time_stamp();
        const double start_ns = dequeue_time.to_seconds() * 1e9;

        // [STAT] 记录第一笔任务开始时刻(全RAM聚合)
        if (dedup_first_start_ns_ < 0) dedup_first_start_ns_ = start_ns;
        if (dedup_ram_first_start_ns_ < 0) dedup_ram_first_start_ns_ = start_ns;

        const bool is_update = (msg.msg_type == CacheMsgType::PT_UPDATE);

        if (is_update) {
            // ============ UPDATE: 查表 + 刷Buffer + 清占位 ============
            trace_task_event("begin", "dedup_cache", "update", msg, dequeue_time);
            const uint32_t flushed_chain = execute_dedup_update(msg);
            // [机制v2] UPDATE可变原子段 = 3拍(get set/hit判断) + 链长 × 每entry刷新拍数;
            //   无链可刷(无占位/预取占位/foreign跳过)仅计3拍; Buffer刷新的FIFO转发
            //   阻塞已在回调内推进时间; 原子段期间独占本RAM, 阻塞同RAM后续查询
            const uint32_t upd_cycles =
                DEDUP_UPD_BASE_CYCLES + flushed_chain * DEDUP_UPD_FLUSH_CYCLES;
            wait(clock_period_ * static_cast<int>(upd_cycles));

            const sc_time end_time = sc_time_stamp();
            const double end_ns = end_time.to_seconds() * 1e9;
            const double exec_ns = end_ns - start_ns;

            dedup_upd_count_++;
            dedup_upd_total_exec_ns_ += exec_ns;
            if (exec_ns > dedup_upd_max_exec_ns_) dedup_upd_max_exec_ns_ = exec_ns;
            if (exec_ns < dedup_upd_min_exec_ns_) dedup_upd_min_exec_ns_ = exec_ns;
            dedup_last_end_ns_ = end_ns;

            record_task_completion("dedup_cache", dequeue_time, end_time);
            dedup_ram_upd_count_[ram_id]++;
        } else if (msg.dedup_is_prefetch) {
            // ============ 预取REQUEST: 插预取占位CL(无响应输出) ============
            uint64_t page_iova = msg.iova & ~0xFFFULL;
            if (dedup_cache_->lookup(msg.gscid, msg.pscid, page_iova) == nullptr) {
                bool ins_ok = dedup_cache_->insert(msg.gscid, msg.pscid, page_iova,
                                     0xFFFF, false /*is_req=0 预取占位*/);
                if (ins_ok) {
                    printf("[DEDUP_PREFETCH] task_id=%u -> prefetch placeholder inserted (iova=0x%lx, RAM%d)\n",
                           (unsigned)msg.task_id, (unsigned long)page_iova, ram_id);
                } else {
                    // [hash冲突] set无空位置可写: 跳过预取占位插入(仅性能降级, 不影响功能)
                    dedup_pf_setfull_skip_++;
                    printf("[DEDUP_PREFETCH] task_id=%u -> no writable way (hash conflict), prefetch placeholder skipped (iova=0x%lx, RAM%d)\n",
                           (unsigned)msg.task_id, (unsigned long)page_iova, ram_id);
                }
            } else {
                printf("[DEDUP_PREFETCH] task_id=%u -> placeholder exists, skip (iova=0x%lx, RAM%d)\n",
                       (unsigned)msg.task_id, (unsigned long)page_iova, ram_id);
            }
            fflush(stdout);
            // 预取任务同样消耗1个原子段
            wait(clock_period_ * static_cast<int>(DEDUP_ATOMIC_CYCLES));
            dedup_last_end_ns_ = sc_time_stamp().to_seconds() * 1e9;
#if TEST_CFG_MULTI_DEVICE_SCENE
            md_record(msg, "dedup_ram", "prefetch_placeholder", start_ns, false, ram_id);
#endif
            dedup_ram_prefetch_count_[ram_id]++;
        } else {
            // ============ 普通REQUEST: 分支1/2/3 + 响应路由 ============
            trace_task_event("begin", "dedup_cache", "lookup", msg, dequeue_time);
            CacheMessage resp = execute_dedup_request(msg);
            // 原子段延时(预取占位已异步化, 每任务固定1个原子段)
            wait(clock_period_ * static_cast<int>(DEDUP_ATOMIC_CYCLES));

            const sc_time end_time = sc_time_stamp();
            const double end_ns = end_time.to_seconds() * 1e9;
            const double exec_ns = end_ns - start_ns;

            dedup_req_count_++;
            dedup_req_total_exec_ns_ += exec_ns;
            if (exec_ns > dedup_req_max_exec_ns_) dedup_req_max_exec_ns_ = exec_ns;
            if (exec_ns < dedup_req_min_exec_ns_) dedup_req_min_exec_ns_ = exec_ns;
            dedup_last_end_ns_ = end_ns;

            record_task_completion("dedup_cache", dequeue_time, end_time);
            trace_task_event("end", "dedup_cache", "lookup", msg, dequeue_time, &resp);
#if TEST_CFG_MULTI_DEVICE_SCENE
            md_record(msg, "dedup_ram", "lookup", start_ns, resp.hit, ram_id);
#endif

            // 响应路由:
            //   dedup_suspended: 任务已挂Buffer -> pt_hit_response_fifo(collector识别后挂起)
            //   否则(MISS/降级): pt_miss_response_fifo -> collector -> PTW
#if TEST_CFG_DEDUP_UNIT_V1123
            // [1123/G3] 经本bank staging FIFO 交给 writer 线程推送 —— worker 对响应
            //   反压零阻塞, upd 出队通道结构性畅通(死锁护栏P3)。staging满回退阻塞写
            //   仅理论兜底(在途响应<=准入上限128/bank << 64+writer即时排空, 计数应恒0)。
            if (!dedup_rsp_stage_fifo_[ram_id]->nb_write(resp)) {
                dedup_rsp_stage_full_cnt_++;
                dedup_rsp_stage_fifo_[ram_id]->write(resp);
            }
#else
            if (resp.dedup_suspended) {
                push_fifo(pt_hit_response_fifo, resp);
            } else {
                push_fifo(pt_miss_response_fifo, resp);
            }
#endif
            dedup_ram_req_count_[ram_id]++;
        }

        // [STAT] 本RAM公共统计
        const double task_end_ns = sc_time_stamp().to_seconds() * 1e9;
        dedup_ram_task_count_[ram_id]++;
        dedup_ram_busy_ns_[ram_id] += (task_end_ns - start_ns);
        if (task_end_ns > dedup_ram_last_end_ns_) dedup_ram_last_end_ns_ = task_end_ns;
    }
}

// ============================================================
// [失效] dispatch_pt_invalidate - PT Cache 失效拆分到 RAM worker + join
// 由 pt_hash_thread 调用(hash 1拍后拆分); 本函数阻塞直至所有子失效完成,
// 保证失效为最高优先(期间不下发新 lookup/update)。
// 子失效写入对应 pt_ram_fifo_, 与已排队的 lookup/fill 在同 RAM 内串行(原子)。
//   LAZY 扫表(VN回绕): 每 RAM 一个 sweep 子任务
//   PRECISE:   单一候选 set -> 所属 RAM
//   SCAN_RANGE:枚举8候选 set(legacy降级全扫) -> 各自 RAM
//   SCAN/GLOBAL: 每 RAM 扫本区间
// ============================================================
void CacheSubsystem::dispatch_pt_invalidate(const CacheMessage& cmd) {
#if TEST_CFG_MULTI_DEVICE_SCENE
    iommu_md::BusyGuard md_busy(md_active_);
#endif
    const sc_time start = sc_time_stamp();
    trace_task_event("begin", "pt_cache", "invalidate", cmd, start);

    // Hash 单元 1 拍
    wait(pt_cache_->hash_stage_latency());

    pt_inval_affected_ = 0;
    pt_inval_sweep_new_vn_ = lib_.global_vn();  // VN回绕后 pipeline 已置0

    std::vector<CacheMessage> subs;
    auto make_sub = [&]() {
        CacheMessage s = cmd;
        s.msg_type = CacheMsgType::PT_INVALIDATE;
        return s;
    };

    if (cmd.inval_lazy_sweep) {
        for (uint32_t r = 0; r < pt_num_rams_; ++r) {
            CacheMessage s = make_sub();
            s.inval_sub_ram_id = r;
            s.inval_sub_has_set = false;
            subs.push_back(s);
        }
    } else if (cmd.invalidate_mode == CacheInvalidateMode::PRECISE) {
        uint32_t set = pt_cache_->precise_candidate_set(cmd.gscid, cmd.pscid, cmd.iova);
        CacheMessage s = make_sub();
        s.inval_sub_set = set;
        s.inval_sub_has_set = true;
        s.inval_sub_ram_id = pt_cache_->ram_of_set(set);
        subs.push_back(s);
    } else if (cmd.invalidate_mode == CacheInvalidateMode::SCAN_RANGE) {
        uint32_t cand[8];
        uint32_t n = pt_cache_->enum_candidate_sets(cmd.iova, cand);
        if (n == 0) {
            // legacy 哈希无法枚举 -> 降级全表扫描
            for (uint32_t r = 0; r < pt_num_rams_; ++r) {
                CacheMessage s = make_sub();
                s.invalidate_mode = CacheInvalidateMode::SCAN;
                s.inval_sub_ram_id = r;
                s.inval_sub_has_set = false;
                subs.push_back(s);
            }
        } else {
            for (uint32_t i = 0; i < n; ++i) {
                CacheMessage s = make_sub();
                s.inval_sub_set = cand[i];
                s.inval_sub_has_set = true;
                s.inval_sub_ram_id = pt_cache_->ram_of_set(cand[i]);
                subs.push_back(s);
            }
        }
    } else {
        // SCAN / GLOBAL
        for (uint32_t r = 0; r < pt_num_rams_; ++r) {
            CacheMessage s = make_sub();
            s.inval_sub_ram_id = r;
            s.inval_sub_has_set = false;
            subs.push_back(s);
        }
    }

    pt_inval_remaining_ = static_cast<int>(subs.size());
    for (auto& s : subs) {
        pt_ram_fifo_[s.inval_sub_ram_id]->write(s);  // 阻塞写 = 反压
    }
    while (pt_inval_remaining_ > 0) {
        wait(pt_inval_sub_done_event_);
    }

    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = cmd.task_id;
    resp.cmd_type = cmd.cmd_type;
    resp.affected_entries = pt_inval_affected_;
    record_task_completion("pt_cache", start, sc_time_stamp());
    trace_task_event("end", "pt_cache", "invalidate", cmd, start, &resp);
    push_fifo(pt_invalidate_response_fifo, resp);
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

// [STAT] Dedup Scheduler 执行延时统计报告
void CacheSubsystem::print_dedup_scheduler_report() const {
    printf("\n========== Dedup Scheduler Execution Latency Report ==========\n");
    
    uint64_t total_count = dedup_req_count_ + dedup_upd_count_;
    if (total_count == 0) {
        printf("  No tasks processed.\n");
        printf("============================================================\n\n");
        return;
    }
    
    double window_ns = dedup_last_end_ns_ - dedup_first_start_ns_;
    double total_exec_ns = dedup_req_total_exec_ns_ + dedup_upd_total_exec_ns_;
    
    printf("  First task start:     %.1f ns\n", dedup_first_start_ns_);
    printf("  Last task end:        %.1f ns\n", dedup_last_end_ns_);
    printf("  Window (first~last):  %.1f ns  (%.3f us)\n", window_ns, window_ns / 1000.0);
    printf("  ---\n");
    printf("  Total tasks:          %lu  (REQUEST=%lu, UPDATE=%lu)\n",
           (unsigned long)total_count,
           (unsigned long)dedup_req_count_,
           (unsigned long)dedup_upd_count_);
    printf("  Total exec time:      %.1f ns  (%.3f us)\n",
           total_exec_ns, total_exec_ns / 1000.0);
    printf("  Avg exec per task:    %.3f ns\n",
           total_count > 0 ? total_exec_ns / total_count : 0.0);
    printf("  ---\n");
    
    // REQUEST 统计
    if (dedup_req_count_ > 0) {
        double req_avg = dedup_req_total_exec_ns_ / dedup_req_count_;
        printf("  [REQUEST]\n");
        printf("    Count:              %lu\n", (unsigned long)dedup_req_count_);
        printf("    Total exec time:    %.1f ns  (%.3f us)\n",
               dedup_req_total_exec_ns_, dedup_req_total_exec_ns_ / 1000.0);
        printf("    Avg exec latency:   %.3f ns\n", req_avg);
        printf("    Max exec latency:   %.1f ns\n", dedup_req_max_exec_ns_);
        printf("    Min exec latency:   %.1f ns\n",
               dedup_req_min_exec_ns_ < 999999999.0 ? dedup_req_min_exec_ns_ : 0.0);
        printf("    ---\n");
    }
    
    // UPDATE 统计
    if (dedup_upd_count_ > 0) {
        double upd_avg = dedup_upd_total_exec_ns_ / dedup_upd_count_;
        printf("  [UPDATE]\n");
        printf("    Count:              %lu\n", (unsigned long)dedup_upd_count_);
        printf("    Total exec time:    %.1f ns  (%.3f us)\n",
               dedup_upd_total_exec_ns_, dedup_upd_total_exec_ns_ / 1000.0);
        printf("    Avg exec latency:   %.3f ns\n", upd_avg);
        printf("    Max exec latency:   %.1f ns\n", dedup_upd_max_exec_ns_);
        printf("    Min exec latency:   %.1f ns\n",
               dedup_upd_min_exec_ns_ < 999999999.0 ? dedup_upd_min_exec_ns_ : 0.0);
        printf("    ---\n");
    }
    
    // 利用率统计
    printf("  [Utilization]\n");
    printf("    Exec ratio:         %.2f%%  (exec / window)\n",
           window_ns > 0 ? total_exec_ns / window_ns * 100.0 : 0.0);
    printf("    Idle ratio:         %.2f%%  (gap / window)\n",
           window_ns > 0 ? (window_ns - total_exec_ns) / window_ns * 100.0 : 0.0);
    printf("============================================================\n\n");
}

// [多RAM] 多 RAM 统计报告: Hash单元/每组RAM利用率/FIFO峰值/吞吐
void CacheSubsystem::print_pt_multi_ram_report() const {
    printf("\n========== PT Cache Multi-RAM Report ==========\n");
    printf("  Config:               num_rams=%u, ram_fifo_depth=%u\n",
           pt_num_rams_, pt_cache_->ram_fifo_depth());

    if (pt_hash_task_count_ == 0) {
        printf("  No tasks processed.\n");
        printf("===============================================\n\n");
        return;
    }

    // Hash 单元
    printf("  [Hash Unit]\n");
    printf("    Tasks hashed:        %lu\n", (unsigned long)pt_hash_task_count_);
    printf("    Busy time:           %.1f ns\n", pt_hash_busy_ns_);
    printf("    Backpressure count:  %lu\n", (unsigned long)pt_hash_backpressure_cnt_);
    printf("    Backpressure time:   %.1f ns (write-block on full RAM FIFO)\n",
           pt_hash_backpressure_ns_);

    // 每组 RAM
    double window_ns = pt_ram_last_end_ns_ - pt_ram_first_start_ns_;
    printf("  [Per-RAM Workers]  (window=%.1f ns)\n", window_ns);
    printf("    %-5s %-10s %-10s %-10s %-12s %-9s %-10s\n",
           "RAM", "Tasks", "Lookups", "Updates", "Busy(ns)", "Util(%)", "FIFO_Peak");
    uint64_t total_tasks = 0;
    double total_busy = 0.0;
    for (uint32_t i = 0; i < pt_num_rams_; ++i) {
        double util = (window_ns > 0) ? pt_ram_busy_ns_[i] / window_ns * 100.0 : 0.0;
        printf("    %-5u %-10lu %-10lu %-10lu %-12.1f %-9.2f %-10d\n",
               i,
               (unsigned long)pt_ram_task_count_[i],
               (unsigned long)pt_ram_lookup_count_[i],
               (unsigned long)pt_ram_update_count_[i],
               pt_ram_busy_ns_[i], util, pt_ram_fifo_peak_[i]);
        total_tasks += pt_ram_task_count_[i];
        total_busy += pt_ram_busy_ns_[i];
    }
    printf("    %-5s %-10lu %-10s %-10s %-12.1f\n",
           "SUM", (unsigned long)total_tasks, "-", "-", total_busy);

    // 并发吞吐: 窗口内每周期输出结果数(稳态目标接近1)
    if (window_ns > 0) {
        printf("  [Throughput]\n");
        printf("    Results/cycle:       %.3f  (total_tasks / window, clock=1ns)\n",
               (double)total_tasks / window_ns);
        printf("    Aggregate busy/win:  %.2f  (>1 means cross-RAM concurrency)\n",
               total_busy / window_ns);
    }
    printf("===============================================\n\n");
}

// ============================================================
// [STAT需求] 入口/去重 4个FIFO(pt_request/pt_update/dedup_request/dedup_update)占用采样
//   每时钟拍读 num_available() 记录占用峰值与时间加权平均。
//   窗口对每个FIFO独立取[首包非空~末包非空], 避免 warmup/drain 稀释平均值。
// ============================================================
void CacheSubsystem::sample_fifo_occ(FifoOccStat& s, int occ, double now, double dt) {
    if (occ > s.peak) s.peak = occ;
    if (occ > 0) {
        if (s.first_nonempty_ns < 0.0) s.first_nonempty_ns = now;
        s.last_nonempty_ns = now;
    }
    // 仅在窗口开启后才积分(含窗口内的瞬时空拍, 它们对 integral 贡献 0)
    if (s.first_nonempty_ns >= 0.0) s.integral_occ_ns += occ * dt;
}

void CacheSubsystem::fifo_util_sampler_thread() {
    // 深度取自构造初始化列表值(sc_fifo::size() 为 protected, 不可直接读)
    fifo_stat_pt_req_    = FifoOccStat{"pt_request_fifo",    16,   0, 0.0, -1.0, -1.0};
    fifo_stat_pt_upd_    = FifoOccStat{"pt_update_fifo",     (int)PT_UPDATE_FIFO_DEPTH, 0, 0.0, -1.0, -1.0};
#if TEST_CFG_DEDUP_UNIT_V1123
    fifo_stat_dedup_req_ = FifoOccStat{"dedup_request_fifo", (int)DEDUP_OUT_REQ_FIFO_DEPTH,  0, 0.0, -1.0, -1.0};
    fifo_stat_dedup_upd_ = FifoOccStat{"dedup_update_fifo",  (int)DEDUP_UPD_HANDOFF_DEPTH,   0, 0.0, -1.0, -1.0};
#else
    fifo_stat_dedup_req_ = FifoOccStat{"dedup_request_fifo", 64,   0, 0.0, -1.0, -1.0};
    fifo_stat_dedup_upd_ = FifoOccStat{"dedup_update_fifo",  64,   0, 0.0, -1.0, -1.0};
#endif
    fifo_stat_pt_hit_rsp_  = FifoOccStat{"pt_hit_response_fifo",  32, 0, 0.0, -1.0, -1.0};
    fifo_stat_pt_miss_rsp_ = FifoOccStat{"pt_miss_response_fifo", 32, 0, 0.0, -1.0, -1.0};
    // [STAT需求] 去重 per-RAM ram_fifo 采样初始化(深度=ram_fifo_depth, 名字静态缓冲保活)
    static char ram_fifo_names[DEDUP_MAX_RAMS][24];
#if TEST_CFG_DEDUP_UNIT_V1123
    static char ram_inter_names[DEDUP_MAX_RAMS][28];
    static char ram_upd_names[DEDUP_MAX_RAMS][28];
    static char rsp_stage_names[DEDUP_MAX_RAMS][28];
    const int ram_depth = (int)DEDUP_RAM_REQ_FIFO_DEPTH;
#else
    const int ram_depth = (int)cfg_.dedup_cache.ram_fifo_depth;
#endif
    for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
        snprintf(ram_fifo_names[i], sizeof(ram_fifo_names[i]), "dedup_ram_fifo[%u]", i);
        fifo_stat_dedup_ram_[i] = FifoOccStat{ram_fifo_names[i], ram_depth, 0, 0.0, -1.0, -1.0};
#if TEST_CFG_DEDUP_UNIT_V1123
        snprintf(ram_inter_names[i], sizeof(ram_inter_names[i]), "dedup_ram_inter[%u]", i);
        fifo_stat_dedup_ram_inter_[i] = FifoOccStat{ram_inter_names[i], (int)DEDUP_RAM_INTER_FIFO_DEPTH, 0, 0.0, -1.0, -1.0};
        snprintf(ram_upd_names[i], sizeof(ram_upd_names[i]), "dedup_ram_upd[%u]", i);
        fifo_stat_dedup_ram_upd_[i] = FifoOccStat{ram_upd_names[i], (int)DEDUP_RAM_UPD_FIFO_DEPTH, 0, 0.0, -1.0, -1.0};
        snprintf(rsp_stage_names[i], sizeof(rsp_stage_names[i]), "dedup_rsp_stage[%u]", i);
        fifo_stat_dedup_rsp_[i] = FifoOccStat{rsp_stage_names[i], (int)DEDUP_RSP_STAGE_DEPTH, 0, 0.0, -1.0, -1.0};
#endif
    }
    const double dt = clock_period_.to_seconds() * 1e9;
    while (true) {
        wait(clock_period_);
        const double now = sc_time_stamp().to_seconds() * 1e9;
        sample_fifo_occ(fifo_stat_pt_req_,    pt_request_fifo.num_available(),    now, dt);
        sample_fifo_occ(fifo_stat_pt_upd_,    pt_update_fifo.num_available(),     now, dt);
        sample_fifo_occ(fifo_stat_dedup_req_, dedup_request_fifo.num_available(), now, dt);
        sample_fifo_occ(fifo_stat_dedup_upd_, dedup_update_fifo.num_available(),  now, dt);
        sample_fifo_occ(fifo_stat_pt_hit_rsp_,  pt_hit_response_fifo.num_available(),  now, dt);
        sample_fifo_occ(fifo_stat_pt_miss_rsp_, pt_miss_response_fifo.num_available(), now, dt);
        for (uint32_t i = 0; i < dedup_num_rams_; ++i)
            sample_fifo_occ(fifo_stat_dedup_ram_[i], dedup_ram_fifo_[i]->num_available(), now, dt);
#if TEST_CFG_DEDUP_UNIT_V1123
        // [1123] per-RAM inter/upd 前置FIFO + 响应staging 每拍采样
        for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
            sample_fifo_occ(fifo_stat_dedup_ram_inter_[i], dedup_ram_inter_fifo_[i]->num_available(), now, dt);
            sample_fifo_occ(fifo_stat_dedup_ram_upd_[i],   dedup_ram_upd_fifo_[i]->num_available(),   now, dt);
            sample_fifo_occ(fifo_stat_dedup_rsp_[i],       dedup_rsp_stage_fifo_[i]->num_available(), now, dt);
        }
#endif
    }
}

void CacheSubsystem::print_fifo_util_report() const {
    const FifoOccStat* all[6] = {&fifo_stat_pt_req_, &fifo_stat_pt_upd_,
                                 &fifo_stat_dedup_req_, &fifo_stat_dedup_upd_,
                                 &fifo_stat_pt_hit_rsp_, &fifo_stat_pt_miss_rsp_};
    printf("\n========== Entry/Dedup FIFO Utilization (每拍采样, 时间加权) ==========\n");
    printf("  %-22s %7s %7s %10s %9s %10s\n",
           "FIFO", "Depth", "Peak", "PeakUtil", "AvgOcc", "AvgUtil");
    printf("  ----------------------------------------------------------------------\n");
    for (int i = 0; i < 6; ++i) {
        const FifoOccStat& s = *all[i];
        const double win = s.last_nonempty_ns - s.first_nonempty_ns;
        const double avg_occ = (win > 0.0) ? s.integral_occ_ns / win : 0.0;
        const double peak_util = s.depth > 0 ? 100.0 * s.peak / s.depth : 0.0;
        const double avg_util  = s.depth > 0 ? 100.0 * avg_occ / s.depth : 0.0;
        printf("  %-22s %7d %7d %9.1f%% %9.2f %9.1f%%\n",
               s.name, s.depth, s.peak, peak_util, avg_occ, avg_util);
    }
    // [STAT需求] 去重 per-RAM ram_fifo 占用(峰值+时间加权平均) + 请求分布均匀性
    if (dedup_num_rams_ > 0) {
#if TEST_CFG_DEDUP_UNIT_V1123
        printf("  ---- Dedup per-RAM ram_fifo (depth=%d) ----\n", (int)DEDUP_RAM_REQ_FIFO_DEPTH);
#else
        printf("  ---- Dedup per-RAM ram_fifo (depth=%d) ----\n", (int)cfg_.dedup_cache.ram_fifo_depth);
#endif
        uint64_t tot_req = 0;
        for (uint32_t i = 0; i < dedup_num_rams_; ++i) tot_req += dedup_ram_req_count_[i];
        const double exp_req = (double)tot_req / dedup_num_rams_;
        double chi2 = 0.0;
        uint64_t mx = 0, mn = ~0ULL;
        for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
            const FifoOccStat& s = fifo_stat_dedup_ram_[i];
            const double win = s.last_nonempty_ns - s.first_nonempty_ns;
            const double avg_occ = (win > 0.0) ? s.integral_occ_ns / win : 0.0;
            const double peak_util = s.depth > 0 ? 100.0 * s.peak / s.depth : 0.0;
            const double avg_util  = s.depth > 0 ? 100.0 * avg_occ / s.depth : 0.0;
            const uint64_t rq = dedup_ram_req_count_[i];
            if (rq > mx) mx = rq;
            if (rq < mn) mn = rq;
            if (exp_req > 0.0) chi2 += (rq - exp_req) * (rq - exp_req) / exp_req;
            const double pavg = dedup_ram_push_cnt_[i]
                ? (double)dedup_ram_push_occ_sum_[i] / dedup_ram_push_cnt_[i] : 0.0;
            printf("  %-22s %7d %7d %9.1f%% %9.2f %9.1f%%   req=%llu (%.2f%%)  push=%.2f (%.1f%%)\n",
                   s.name, s.depth, s.peak, peak_util, avg_occ, avg_util,
                   (unsigned long long)rq, tot_req ? 100.0 * rq / tot_req : 0.0,
                   pavg, s.depth > 0 ? 100.0 * pavg / s.depth : 0.0);
        }
        const double thr = (dedup_num_rams_ == 4) ? 7.81 : 14.07;  // df=3 / df=7, p<0.05
        const double imbalance = (mn > 0 && mn != ~0ULL) ? 100.0 * (mx - mn) / (double)mn : 0.0;
        printf("  Request uniformity: total=%llu max=%llu min=%llu max/min=%.3f imbalance=%.2f%% chi2=%.2f (df=%u, thr=%.2f) => %s\n",
               (unsigned long long)tot_req, (unsigned long long)mx,
               (mn == ~0ULL ? 0ULL : (unsigned long long)mn),
               (mn > 0 && mn != ~0ULL) ? (double)mx / mn : 0.0,
               imbalance, chi2, dedup_num_rams_ - 1, thr,
               (chi2 <= thr) ? "BALANCED" : "SKEWED");
#if TEST_CFG_DEDUP_UNIT_V1123
        // [1123] per-RAM inter/upd 前置FIFO 与 响应staging 占用(峰值+时间加权平均)
        printf("  ---- Dedup per-RAM inter(%d)/upd(%d) FIFO & rsp_stage(%d) ----\n",
               (int)DEDUP_RAM_INTER_FIFO_DEPTH, (int)DEDUP_RAM_UPD_FIFO_DEPTH, (int)DEDUP_RSP_STAGE_DEPTH);
        for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
            for (int k = 0; k < 3; ++k) {
                const FifoOccStat& s = (k == 0) ? fifo_stat_dedup_ram_inter_[i]
                                   : (k == 1) ? fifo_stat_dedup_ram_upd_[i]
                                              : fifo_stat_dedup_rsp_[i];
                const double win = s.last_nonempty_ns - s.first_nonempty_ns;
                const double avg_occ = (win > 0.0) ? s.integral_occ_ns / win : 0.0;
                printf("  %-22s depth=%-4d peak=%-4d avg=%.2f util=%.1f%%\n",
                       s.name, s.depth, s.peak, avg_occ,
                       s.depth > 0 ? 100.0 * avg_occ / s.depth : 0.0);
            }
        }
#endif
    }
    printf("  注: 采样步长=%.1fns; AvgOcc/AvgUtil 时间加权窗口=[首包非空~末包非空], 各FIFO独立。\n",
           clock_period_.to_seconds() * 1e9);
    printf("==============================================================\n");
    fflush(stdout);
}

// ============================================================
// [STAT] 串行环节预算表
//   汇总所有"per-task 串行"环节的 busy/窗口比与单线程理论上限, 并给出
//   PT Cache 多 RAM 的分布均匀性与同 RAM 背靠背冲突比例, 用于回答:
//   "128B 小包场景每包固定 4ns 到底卡在哪一级、是否是多 RAM 不均造成"。
// ============================================================
void CacheSubsystem::print_serial_stage_budget_report() const {
    const double window_ns = pt_ram_last_end_ns_ - pt_ram_first_start_ns_;
    if (window_ns <= 0.0) {
        printf("\n========== Serial Stage Budget ==========\n");
        printf("  No window available.\n");
        printf("========================================\n\n");
        return;
    }

    // 各环节窗口可能不同: 统一用 PT 全窗口做分母, 并同时给出各自实际窗口
    const double dedup_window_ns = dedup_ram_last_end_ns_ - dedup_ram_first_start_ns_;
    const double walker_window_ns = walker_ram_last_end_ns_ - walker_ram_first_start_ns_;

    // PT / Dedup per-RAM 最大忙时(即最拥堆的那组 RAM 对聚合吞吐的约束)
    double pt_ram_max_busy = 0.0, dedup_ram_max_busy = 0.0, walker_max_busy = 0.0;
    uint32_t pt_busiest = 0, dedup_busiest = 0;
    for (uint32_t i = 0; i < pt_num_rams_ && i < PT_MAX_RAMS; ++i) {
        if (pt_ram_busy_ns_[i] > pt_ram_max_busy) { pt_ram_max_busy = pt_ram_busy_ns_[i]; pt_busiest = i; }
    }
    for (uint32_t i = 0; i < dedup_num_rams_ && i < DEDUP_MAX_RAMS; ++i) {
        if (dedup_ram_busy_ns_[i] > dedup_ram_max_busy) { dedup_ram_max_busy = dedup_ram_busy_ns_[i]; dedup_busiest = i; }
    }
    for (int i = 0; i < WALKER_MAX_WORKERS; ++i) {
        if (walker_ram_busy_ns_[i] > walker_max_busy) walker_max_busy = walker_ram_busy_ns_[i];
    }

    printf("\n========== Serial Stage Budget (per-task fixed cost) ==========\n");
    printf("  Window used:          %.1f ns (PT Cache first-start~last-end)\n", window_ns);
    printf("  %-36s %10s %12s %10s %10s %12s\n",
           "Stage", "Tasks", "Busy(ns)", "Busy/Win", "ns/task", "Peak(Mt/s)");
    printf("  %s\n", std::string(98, '-').c_str());

    struct Row {
        const char* name;
        uint64_t tasks;
        double busy;
        double win;
    };
    const Row rows[] = {
        {"DC  cache lookup (1 thread)",     dc_query_total_count_, dc_busy_ns_,       window_ns},
        {"PC  cache lookup (1 thread)",     pc_query_total_count_, pc_busy_ns_,       window_ns},
        {"PT  cache hash   (1 thread)",     pt_hash_task_count_,   pt_hash_busy_ns_,  window_ns},
        {"PT  cache RAM    (busiest RAM)",  pt_ram_task_count_[pt_busiest], pt_ram_max_busy, window_ns},
        {"Dedup hash       (1 thread)",     dedup_hash_task_count_, dedup_hash_busy_ns_, dedup_window_ns},
        {"Dedup RAM        (busiest RAM)",  dedup_ram_task_count_[dedup_busiest], dedup_ram_max_busy, dedup_window_ns},
        {"Walker RAM       (busiest worker)", 0, walker_max_busy, walker_window_ns},
    };

    const char* bottleneck = "none";
    double bottleneck_ratio = 0.0;
    for (const auto& r : rows) {
        const double ratio = (r.win > 0) ? r.busy / r.win : 0.0;
        const double ns_per_task = (r.tasks > 0) ? r.busy / (double)r.tasks : 0.0;
        const double peak = (ns_per_task > 0) ? 1000.0 / ns_per_task : 0.0;
        printf("  %-36s %10lu %12.1f %9.1f%% %10.2f %12.1f\n",
               r.name, (unsigned long)r.tasks, r.busy, ratio * 100.0, ns_per_task, peak);
        if (ratio > bottleneck_ratio) { bottleneck_ratio = ratio; bottleneck = r.name; }
    }
    printf("  %s\n", std::string(98, '-').c_str());
    printf("  [Verdict] highest-utilization serial stage: %s (%.1f%% of window)\n",
           bottleneck, bottleneck_ratio * 100.0);
    printf("  NOTE: ns/task = Busy/Tasks; Peak = 1000/nspertask = single-thread\n");
    printf("        capability; Busy/Win approaching 100%% means this stage sets\n");
    printf("        the per-packet steady-state cadence.\n");

    // ---- PT Cache 多 RAM 分布与同 RAM 背靠背 ----
    printf("  [PT RAM Distribution & Same-RAM Back-to-Back]\n");
    printf("    %-4s %10s %8s %12s %12s %14s %8s\n",
           "RAM", "Tasks", "Share%", "ArrGap_avg", "ArrGap_min", "B2B(<3ns)", "B2B%");
    uint64_t total_pt = 0;
    for (uint32_t i = 0; i < pt_num_rams_ && i < PT_MAX_RAMS; ++i) total_pt += pt_ram_task_count_[i];
    for (uint32_t i = 0; i < pt_num_rams_ && i < PT_MAX_RAMS; ++i) {
        const uint64_t n = pt_ram_task_count_[i];
        const double share = (total_pt > 0) ? 100.0 * n / total_pt : 0.0;
        const double gap_avg = (pt_ram_gap_count_[i] > 0)
                                   ? pt_ram_gap_total_ns_[i] / pt_ram_gap_count_[i] : 0.0;
        const double gap_min = (pt_ram_gap_count_[i] > 0) ? pt_ram_gap_min_ns_[i] : 0.0;
        const double b2b_pct = (pt_ram_gap_count_[i] > 0)
                                   ? 100.0 * pt_ram_b2b_cnt_[i] / pt_ram_gap_count_[i] : 0.0;
        printf("    %-4u %10lu %7.2f%% %12.2f %12.2f %14lu %7.2f%%\n",
               i, (unsigned long)n, share, gap_avg, gap_min,
               (unsigned long)pt_ram_b2b_cnt_[i], b2b_pct);
    }
    const double same_ratio = (pt_hash_task_count_ > 0)
                                  ? 100.0 * pt_same_ram_streak_cnt_ / pt_hash_task_count_ : 0.0;
    printf("    Same-RAM-as-previous-task: %lu / %lu (%.2f%%), uniform ideal=25.00%%\n",
           (unsigned long)pt_same_ram_streak_cnt_, (unsigned long)pt_hash_task_count_, same_ratio);

    // ---- [DATA-ONLY] PT Cache 仅数据包的 4 组 RAM 分布均衡度 ----
    // 仅统计 is_ctrl_pkt=false 的任务(排除 SQ/CQ/MSI 控制包);
    // 评估 hash(gscid,pscid,iova)>>低位 在数据流量上是否均匀分桶。
    // 均衡度指标:
    //   - Share%   : 各 RAM 占数据总访问的百分比(理想 25.00%)
    //   - Max/Min  : 最热 RAM / 最冷 RAM 的比值(1.00=完全均衡)
    //   - Imbalance: (max-min)/avg × 100%(0=完全均衡)
    //   - Chi^2    : 卡方统计量, 自由度=3; >7.81 则 p<0.05 显著不均衡
    {
        uint64_t data_lu[PT_MAX_RAMS] = {0}, data_up[PT_MAX_RAMS] = {0};
        uint64_t total_lu = 0, total_up = 0;
        for (uint32_t i = 0; i < pt_num_rams_ && i < PT_MAX_RAMS; ++i) {
            data_lu[i] = pt_ram_data_lookup_count_[i];
            data_up[i] = pt_ram_data_update_count_[i];
            total_lu += data_lu[i];
            total_up += data_up[i];
        }
        const uint64_t total_all = total_lu + total_up;
        printf("  [PT RAM Distribution - DATA ONLY (is_ctrl_pkt=false)]\n");
        printf("    Total data accesses: %lu (lookup=%lu, update=%lu)\n",
               (unsigned long)total_all, (unsigned long)total_lu, (unsigned long)total_up);
        printf("    %-4s %12s %10s %12s %10s %12s %10s\n",
               "RAM", "DataLookups", "Share%", "DataUpdates", "Share%",
               "DataTotal", "Share%");
        uint64_t dmin = UINT64_MAX, dmax = 0;
        double chi2 = 0.0;
        const double expected = (pt_num_rams_ > 0) ? (double)total_all / pt_num_rams_ : 0.0;
        for (uint32_t i = 0; i < pt_num_rams_ && i < PT_MAX_RAMS; ++i) {
            const uint64_t n_lu = data_lu[i];
            const uint64_t n_up = data_up[i];
            const uint64_t n_all = n_lu + n_up;
            const double sh_lu = (total_lu > 0) ? 100.0 * n_lu / total_lu : 0.0;
            const double sh_up = (total_up > 0) ? 100.0 * n_up / total_up : 0.0;
            const double sh_all = (total_all > 0) ? 100.0 * n_all / total_all : 0.0;
            printf("    %-4u %12lu %9.2f%% %12lu %9.2f%% %12lu %9.2f%%\n",
                   i, (unsigned long)n_lu, sh_lu,
                   (unsigned long)n_up, sh_up,
                   (unsigned long)n_all, sh_all);
            if (n_all < dmin) dmin = n_all;
            if (n_all > dmax) dmax = n_all;
            if (expected > 0.0) {
                const double d = (double)n_all - expected;
                chi2 += d * d / expected;
            }
        }
        if (total_all > 0 && pt_num_rams_ > 0) {
            const double avg = (double)total_all / pt_num_rams_;
            const double imbalance = (avg > 0) ? 100.0 * (dmax - dmin) / avg : 0.0;
            const double mm = (dmin > 0) ? (double)dmax / dmin : 0.0;
            printf("    Evenness: Max=%lu, Min=%lu, Avg=%.1f, Max/Min=%.3f, Imbalance=%.2f%%, Chi2=%.2f (df=%u, p<0.05 threshold=7.81)\n",
                   (unsigned long)dmax, (unsigned long)dmin, avg, mm, imbalance,
                   chi2, pt_num_rams_ - 1);
            if (chi2 > 7.81) {
                printf("    Verdict: NOT BALANCED (chi2 > 7.81, p<0.05)\n");
            } else {
                printf("    Verdict: BALANCED (chi2 <= 7.81, p>=0.05)\n");
            }
        } else {
            printf("    No data-packet accesses recorded.\n");
        }
        // [Q2] 仅数据包 per-RAM 真实命中率 = data_lookup_hits / data_lookups
        printf("    [Data-only per-RAM TRUE Hit Rate] (HitRate = DataHits / DataLookups)\n");
        printf("    %-4s %12s %12s %10s\n", "RAM", "DataLookups", "DataHits", "HitRate%");
        {
            uint64_t agg_lu = 0, agg_hit = 0;
            double hr_min = 1e9, hr_max = -1e9;
            for (uint32_t i = 0; i < pt_num_rams_ && i < PT_MAX_RAMS; ++i) {
                const uint64_t lu = pt_ram_data_lookup_count_[i];
                const uint64_t ht = pt_ram_data_lookup_hit_count_[i];
                const double hr = (lu > 0) ? 100.0 * ht / lu : 0.0;
                printf("    %-4u %12lu %12lu %9.2f%%\n", i,
                       (unsigned long)lu, (unsigned long)ht, hr);
                agg_lu += lu; agg_hit += ht;
                if (lu > 0) { if (hr < hr_min) hr_min = hr; if (hr > hr_max) hr_max = hr; }
            }
            const double agg_hr = (agg_lu > 0) ? 100.0 * agg_hit / agg_lu : 0.0;
            printf("    %-4s %12lu %12lu %9.2f%%  (aggregate; per-RAM spread max-min=%.2f pp)\n",
                   "ALL", (unsigned long)agg_lu, (unsigned long)agg_hit, agg_hr,
                   (hr_max >= hr_min) ? (hr_max - hr_min) : 0.0);
        }
    }

    // ---- [ALL PACKETS] PT Cache 每RAM 真实命中率(控制包+数据包) ----
    // [Q3] 含 SQ/CQ/MSI 控制包; 控制包 IOVA 固定(集中落 RAM0/1), 合并后看每RAM总命中率。
    printf("  [PT RAM TRUE Hit Rate - ALL PACKETS (ctrl+data)]\n");
    printf("    %-4s %12s %12s %10s\n", "RAM", "Lookups", "Hits", "HitRate%");
    {
        uint64_t agg_lu = 0, agg_hit = 0;
        for (uint32_t i = 0; i < pt_num_rams_ && i < PT_MAX_RAMS; ++i) {
            const uint64_t lu = pt_ram_lookup_count_[i];
            const uint64_t ht = pt_ram_lookup_hit_count_[i];
            const double hr = (lu > 0) ? 100.0 * ht / lu : 0.0;
            printf("    %-4u %12lu %12lu %9.2f%%\n", i,
                   (unsigned long)lu, (unsigned long)ht, hr);
            agg_lu += lu; agg_hit += ht;
        }
        const double agg_hr = (agg_lu > 0) ? 100.0 * agg_hit / agg_lu : 0.0;
        printf("    %-4s %12lu %12lu %9.2f%%  (aggregate)\n",
               "ALL", (unsigned long)agg_lu, (unsigned long)agg_hit, agg_hr);
    }

    // ---- [PT RAM Lookup/Update 数目与占比] DATA-ONLY 与 ALL 两种口径 ----
    //   每 RAM 分别给出 lookup 数目+占比、update 数目+占比(占比=本RAM/四RAM合计)。
    printf("  [PT RAM Lookup/Update Count & Share]\n");
    for (int mode = 0; mode < 2; ++mode) {
        const bool data_only = (mode == 0);
        uint64_t tl = 0, tu = 0;
        for (uint32_t i = 0; i < pt_num_rams_ && i < PT_MAX_RAMS; ++i) {
            tl += data_only ? pt_ram_data_lookup_count_[i] : pt_ram_lookup_count_[i];
            tu += data_only ? pt_ram_data_update_count_[i] : pt_ram_update_count_[i];
        }
        printf("    -- %s : total lookup=%lu, total update=%lu --\n",
               data_only ? "DATA ONLY (is_ctrl_pkt=false)" : "ALL PACKETS (ctrl+data)",
               (unsigned long)tl, (unsigned long)tu);
        printf("    %-4s %10s %9s %10s %9s\n", "RAM", "Lookups", "Share%", "Updates", "Share%");
        for (uint32_t i = 0; i < pt_num_rams_ && i < PT_MAX_RAMS; ++i) {
            const uint64_t lu = data_only ? pt_ram_data_lookup_count_[i] : pt_ram_lookup_count_[i];
            const uint64_t up = data_only ? pt_ram_data_update_count_[i] : pt_ram_update_count_[i];
            printf("    %-4u %10lu %8.2f%% %10lu %8.2f%%\n", i,
                   (unsigned long)lu, (tl > 0) ? 100.0 * lu / tl : 0.0,
                   (unsigned long)up, (tu > 0) ? 100.0 * up / tu : 0.0);
        }
    }
    printf("==============================================================\n\n");
}

// [dedup多RAM] 去重Cache多RAM统计报告: Hash单元/每组RAM利用率/FIFO峰值/预取丢弃
void CacheSubsystem::print_dedup_multi_ram_report() const {
    printf("\n========== Dedup Cache Multi-RAM Report ==========\n");
#if TEST_CFG_DEDUP_UNIT_V1123
    printf("  Config[1123]:         banks=4, per-RAM fifo req=%u/inter=%u/upd=%u, outer req=%u/inter=%u/delayed_upd=%u, atomic=%uns, hash=3x%uns, Buffer=4x%u(total %u), rsp_stage=%u\n",
           DEDUP_RAM_REQ_FIFO_DEPTH, DEDUP_RAM_INTER_FIFO_DEPTH, DEDUP_RAM_UPD_FIFO_DEPTH,
           DEDUP_OUT_REQ_FIFO_DEPTH, DEDUP_OUT_INTER_FIFO_DEPTH, DEDUP_DELAYED_UPDATE_FIFO_DEPTH,
           DEDUP_ATOMIC_CYCLES, DEDUP_HASH_CYCLES, DEDUP_BUFFER_PER_BANK,
           PT_DEDUP_BUFFER_SIZE, DEDUP_RSP_STAGE_DEPTH);
#else
    printf("  Config:               num_rams=%u, ram_fifo_depth=%u, inside_fifo_depth=32\n",
           dedup_num_rams_, cfg_.dedup_cache.ram_fifo_depth);
#endif

#if TEST_CFG_DEDUP_UNIT_V1123
    const uint64_t v1123_hash_total =
        dedup_v1123_hash_cnt_[0] + dedup_v1123_hash_cnt_[1] + dedup_v1123_hash_cnt_[2];
    if (v1123_hash_total == 0) {
#else
    if (dedup_hash_task_count_ == 0) {
#endif
        printf("  No tasks processed.\n");
        printf("==================================================\n\n");
        return;
    }

    // Hash 单元
#if TEST_CFG_DEDUP_UNIT_V1123
    printf("  [Hash Units x3] (req/inter/upd 各自独立线程, 1ns/笔)\n");
    printf("    %-8s %-10s %-12s %-12s %-14s\n", "Unit", "Tasks", "Busy(ns)", "BP_count", "BP_time(ns)");
    for (int k = 0; k < 3; ++k) {
        printf("    %-8s %-10lu %-12.1f %-12lu %-14.1f\n",
               k == 0 ? "req" : (k == 1 ? "inter" : "upd"),
               (unsigned long)dedup_v1123_hash_cnt_[k], dedup_v1123_hash_busy_ns_[k],
               (unsigned long)dedup_v1123_hash_bp_cnt_[k], dedup_v1123_hash_bp_ns_[k]);
    }
#else
    printf("  [Hash Unit]\n");
    printf("    Tasks hashed:        %lu\n", (unsigned long)dedup_hash_task_count_);
    printf("    Busy time:           %.1f ns\n", dedup_hash_busy_ns_);
    printf("    Backpressure count:  %lu\n", (unsigned long)dedup_hash_backpressure_cnt_);
    printf("    Backpressure time:   %.1f ns (write-block on full RAM FIFO)\n",
           dedup_hash_backpressure_ns_);
#endif

    // 每组 RAM
    double window_ns = dedup_ram_last_end_ns_ - dedup_ram_first_start_ns_;
    printf("  [Per-RAM Workers]  (window=%.1f ns)\n", window_ns);
#if TEST_CFG_DEDUP_UNIT_V1123
    printf("    %-5s %-10s %-10s %-10s %-10s %-12s %-9s %-10s %-7s %-7s %-7s\n",
           "RAM", "Tasks", "Requests", "Updates", "Prefetch", "Busy(ns)", "Util(%)",
           "ReqFPk", "IntFPk", "UpdFPk", "GateSkip");
#else
    printf("    %-5s %-10s %-10s %-10s %-10s %-12s %-9s %-10s\n",
           "RAM", "Tasks", "Requests", "Updates", "Prefetch", "Busy(ns)", "Util(%)", "FIFO_Peak");
#endif
    uint64_t total_tasks = 0;
    double total_busy = 0.0;
    for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
        double util = (window_ns > 0) ? dedup_ram_busy_ns_[i] / window_ns * 100.0 : 0.0;
#if TEST_CFG_DEDUP_UNIT_V1123
        printf("    %-5u %-10lu %-10lu %-10lu %-10lu %-12.1f %-9.2f %-10d %-7d %-7d %-7lu\n",
               i,
               (unsigned long)dedup_ram_task_count_[i],
               (unsigned long)dedup_ram_req_count_[i],
               (unsigned long)dedup_ram_upd_count_[i],
               (unsigned long)dedup_ram_prefetch_count_[i],
               dedup_ram_busy_ns_[i], util, dedup_ram_fifo_peak_[i],
               dedup_ram_inter_fifo_peak_[i], dedup_ram_upd_fifo_peak_[i],
               (unsigned long)dedup_bank_gate_cnt_[i]);
#else
        printf("    %-5u %-10lu %-10lu %-10lu %-10lu %-12.1f %-9.2f %-10d\n",
               i,
               (unsigned long)dedup_ram_task_count_[i],
               (unsigned long)dedup_ram_req_count_[i],
               (unsigned long)dedup_ram_upd_count_[i],
               (unsigned long)dedup_ram_prefetch_count_[i],
               dedup_ram_busy_ns_[i], util, dedup_ram_fifo_peak_[i]);
#endif
        total_tasks += dedup_ram_task_count_[i];
        total_busy += dedup_ram_busy_ns_[i];
    }
    printf("    %-5s %-10lu %-10s %-10s %-10s %-12.1f\n",
           "SUM", (unsigned long)total_tasks, "-", "-", "-", total_busy);
#if TEST_CFG_DEDUP_UNIT_V1123
    // [写入加权] per-RAM 3条前置FIFO: push时采样占用(到达加权, 非时间加权)
    //   Pushes=该FIFO被hash单元写入的任务数(=对应RAM该类型访问量), Peak=写入时占用峰值,
    //   AvgPush=Σ(写入时占用)/Pushes, Util=AvgPush/depth
    printf("  [Per-RAM FIFO Write-Weighted] (push-sampled, 写入加权口径)\n");
    printf("    %-5s %-10s %-10s %-8s %-10s %-8s\n", "RAM", "FIFO", "Pushes", "Peak", "AvgPush", "Util%");
    for (uint32_t i = 0; i < dedup_num_rams_; ++i) {
        const uint64_t cnts[3]   = { dedup_ram_push_cnt_[i], dedup_ram_inter_push_cnt_[i], dedup_ram_upd_push_cnt_[i] };
        const int      pks[3]    = { dedup_ram_fifo_peak_[i], dedup_ram_inter_fifo_peak_[i], dedup_ram_upd_fifo_peak_[i] };
        const double   sums[3]   = { (double)dedup_ram_push_occ_sum_[i], (double)dedup_ram_inter_push_occ_sum_[i], (double)dedup_ram_upd_push_occ_sum_[i] };
        const int      deps[3]   = { (int)DEDUP_RAM_REQ_FIFO_DEPTH, (int)DEDUP_RAM_INTER_FIFO_DEPTH, (int)DEDUP_RAM_UPD_FIFO_DEPTH };
        static const char* kNms[3] = { "req", "inter", "upd" };
        char nms[3][12];
        for (int k = 0; k < 3; ++k) snprintf(nms[k], sizeof(nms[k]), "%s(%d)", kNms[k], deps[k]);
        for (int k = 0; k < 3; ++k) {
            const double avg = cnts[k] ? sums[k] / (double)cnts[k] : 0.0;
            printf("    %-5u %-10s %-10llu %-8d %-10.2f %-8.1f\n",
                   i, nms[k], (unsigned long long)cnts[k], pks[k], avg,
                   deps[k] > 0 ? 100.0 * avg / deps[k] : 0.0);
        }
    }
#if TEST_CFG_DEDUP_PREFETCH_BLOCK
    printf("  [Prefetch Push Mode] BLOCKING (G1 drop removed): blocks=%lu, block_total=%.1f ns, block_max=%.1f ns\n",
           (unsigned long)dedup_pf_block_cnt_, dedup_pf_block_ns_, dedup_pf_block_max_ns_);
#else
    printf("  [Prefetch Push Mode] DROP-ON-FULL (G1 kept): dropped=%lu\n",
           (unsigned long)dedup_prefetch_dropped_);
#endif
#endif

    // 预取丢弃(inside_fifo满)
    printf("  [Prefetch]\n");
    printf("    Dropped (inside_fifo full): %lu\n", (unsigned long)dedup_prefetch_dropped_);
    printf("  [Buffer Full Bypass]\n");
    printf("    Bypass to PTW count:        %lu\n", (unsigned long)dedup_buffer_full_bypass_count_);
#if TEST_CFG_DEDUP_UNIT_V1123
    printf("  [Admission Control] ENABLED per-bank (1123: bank满门控本RAM req/inter出队, upd不受门控; 4bank全满反压外层req/inter)\n");
    {
        uint64_t gate_sum = 0;
        for (uint32_t b = 0; b < DEDUP_NUM_BANKS; ++b) gate_sum += dedup_bank_gate_cnt_[b];
        printf("    Bank gate skips (sum):      %lu\n", (unsigned long)gate_sum);
        printf("    dedup_request_fifo peak:    %d / %u\n",
               dedup_request_fifo_peak_, (unsigned)DEDUP_OUT_REQ_FIFO_DEPTH);
        printf("    rsp_stage full fallback:    %lu (理论应为0, G3兜底计数)\n",
               (unsigned long)dedup_rsp_stage_full_cnt_);
    }
    // [1123] per-Bank Buffer 块: 每bank容量128, 链不跨bank; 聚合行为全4bank合计
    printf("  [Per-Bank Buffer] (cap=%u/bank, total=%u)\n",
           DEDUP_BUFFER_PER_BANK, PT_DEDUP_BUFFER_SIZE);
    printf("    %-6s %-8s %-10s %-10s %-12s %-12s %-10s %-12s\n",
           "Bank", "Peak", "AvgOcc", "AvgUtil%", "HoldCnt", "HoldAvg", "HoldMax", "ResvPeak");
    for (uint32_t b = 0; b < DEDUP_NUM_BANKS; ++b) {
        DedupBuffer* bk = dedup_banks_[b];
        if (!bk) continue;
        printf("    B%-5u %-8u %-10.2f %-10.1f %-12lu %-12.1f %-10.1f %-12u\n",
               b, (unsigned)bk->get_peak_valid_count(), bk->get_avg_occupancy(),
               bk->get_avg_occupancy_pct(), (unsigned long)bk->get_hold_count(),
               bk->get_avg_hold_ns(), bk->get_max_hold_ns(),
               (unsigned)dedup_reserved_bank_peak_[b]);
    }
    if (dedup_buffer_) {
        printf("    AGG   peak=%u avg_occ=%.2f util=%.1f%% hold_cnt=%lu avg=%.1f max=%.1f (final_valid=%u)\n",
               (unsigned)dedup_buffer_->get_peak_valid_count(), dedup_buffer_->get_avg_occupancy(),
               dedup_buffer_->get_avg_occupancy_pct(),
               (unsigned long)dedup_buffer_->get_hold_count(),
               dedup_buffer_->get_avg_hold_ns(), dedup_buffer_->get_max_hold_ns(),
               (unsigned)dedup_buffer_->get_valid_count());
    }
#elif TEST_CFG_DEDUP_ADMISSION_CTRL
    printf("  [Admission Control] ENABLED (Buffer满跳过出队, request FIFO排队反压)\n");
    printf("    Admission stall waits:      %lu\n", (unsigned long)dedup_admission_wait_cnt_);
    printf("    Admission stall total:      %.1f ns (%.3f us)\n",
           dedup_admission_wait_ns_, dedup_admission_wait_ns_ / 1000.0);
    printf("    dedup_request_fifo peak:    %d\n", dedup_request_fifo_peak_);
    printf("    Reserved peak:              %u\n", (unsigned)dedup_reserved_peak_);
#else
    printf("  [Admission Control] DISABLED (bypass mode)\n");
#endif
    printf("  [Hash Conflict Bypass] (步骤iv: 无空位置可写(全V=1&is_req=1) -> bypass PTW)\n");
    printf("    Conflict count (main MISS & set-full): %lu\n", (unsigned long)dedup_hash_conflict_count_);
    printf("    Conflict ratio (of main lookups %lu): %.2f%%\n",
           (unsigned long)dedup_req_lookup_total_,
           dedup_req_lookup_total_ ? 100.0 * dedup_hash_conflict_count_ / dedup_req_lookup_total_ : 0.0);
    printf("    Conflict ratio (of main MISS %lu):    %.2f%%\n",
           (unsigned long)dedup_req_lookup_miss_,
           dedup_req_lookup_miss_ ? 100.0 * dedup_hash_conflict_count_ / dedup_req_lookup_miss_ : 0.0);
    printf("    Conflict bypass to PTW tasks: %lu\n", (unsigned long)dedup_conflict_bypass_count_);
    printf("    Prefetch set-full skipped:    %lu\n", (unsigned long)dedup_pf_setfull_skip_);
    // [STAT需求2] 去重Cache命中率(总查询/命中/未命中)
    printf("  [去重Cache命中率 (execute_dedup_request lookup)]\n");
    printf("    总查询(lookup):            %lu\n", (unsigned long)dedup_lookup_total_);
    printf("    命中(占位CL存在):          %lu\n", (unsigned long)dedup_lookup_hit_);
    printf("    未命中:                    %lu\n", (unsigned long)dedup_lookup_miss_);
    printf("    总命中率:                  %.2f%%\n",
           dedup_lookup_total_ > 0 ? 100.0 * dedup_lookup_hit_ / dedup_lookup_total_ : 0.0);
    printf("    主任务去重请求:            %lu (命中 %lu, 未命中 %lu, 命中率 %.2f%%)\n",
           (unsigned long)dedup_req_lookup_total_, (unsigned long)dedup_req_lookup_hit_,
           (unsigned long)dedup_req_lookup_miss_,
           dedup_req_lookup_total_ > 0 ? 100.0 * dedup_req_lookup_hit_ / dedup_req_lookup_total_ : 0.0);

    // [STAT需求2] 去重Cache 有效 cacheline 占用采样(每次 dedup lookup 采样一次)
    {
        uint32_t tot_lines = get_dedup_total_lines();
        uint64_t samples   = get_dedup_valid_sample_count();
        double   avg_valid = get_dedup_avg_valid_lines();
        double   avg_req   = get_dedup_avg_req_lines();
        printf("  [Valid Cacheline Occupancy] (每次 dedup lookup 采样)\n");
        printf("    Total lines:        %u (sets=%u x ways=%u x rams=%u)\n",
               tot_lines, get_dedup_num_sets(), get_dedup_num_ways(), dedup_num_rams_);
        printf("    Lookup samples:     %lu\n", (unsigned long)samples);
        printf("    Avg valid lines:    %.2f  (%.1f%% of %u)\n",
               avg_valid, tot_lines > 0 ? 100.0 * avg_valid / tot_lines : 0.0, tot_lines);
        printf("    Max / Min valid:    %u / %u\n",
               get_dedup_max_valid_lines(), get_dedup_min_valid_lines());
        printf("    Avg main-req lines: %.2f  (is_req=1, 其余为预取占位)\n", avg_req);
        printf("    Final valid lines:  %u\n", get_dedup_valid_line_count());
    }

    // [去重延迟更新] Delayed Update FIFO 报告: 入队/三条件出队/占用/驻留/反压
#if TEST_CFG_DEDUP_DELAYED_UPDATE
    {
        const uint64_t drained = delayed_upd_drain_c1_ + delayed_upd_drain_c2_ + delayed_upd_drain_c3_;
        const double now_ns = sc_time_stamp().to_seconds() * 1e9;
        double integral = delayed_upd_occ_weighted_ns_;
        if (delayed_upd_occ_last_ns_ >= 0.0)
            integral += (double)delayed_upd_deque_.size() * (now_ns - delayed_upd_occ_last_ns_);
        const double span = (delayed_upd_first_enq_ns_ >= 0.0 && now_ns > delayed_upd_first_enq_ns_)
                          ? (now_ns - delayed_upd_first_enq_ns_) : 0.0;
        printf("  [Delayed Update FIFO Report] (depth=%u, timer=%uns, margin=%u, threshold=%u)\n",
               DEDUP_DELAYED_UPDATE_FIFO_DEPTH, DEDUP_DELAYED_UPDATE_TIMER_NS,
               DEDUP_DELAYED_PRESSURE_MARGIN,
               DEDUP_DELAYED_UPDATE_FIFO_DEPTH - DEDUP_DELAYED_PRESSURE_MARGIN);
        printf("    Enqueued:               %lu\n", (unsigned long)delayed_upd_enq_total_);
        printf("    Drained c1 (timer):     %lu\n", (unsigned long)delayed_upd_drain_c1_);
        printf("    Drained c2 (pressure):  %lu\n", (unsigned long)delayed_upd_drain_c2_);
#if TEST_CFG_DEDUP_UNIT_V1123
        printf("    Drained c3 (4bank-all-full):%lu\n", (unsigned long)delayed_upd_drain_c3_);
#else
        printf("    Drained c3 (bufferfull):%lu\n", (unsigned long)delayed_upd_drain_c3_);
#endif
        printf("    Group drains (c2/c3):   %lu\n", (unsigned long)delayed_upd_group_cnt_);
        printf("    Pending at report:      %u\n", (unsigned)delayed_upd_deque_.size());
        printf("    Occ peak:               %d / %u\n", delayed_upd_occ_peak_, DEDUP_DELAYED_UPDATE_FIFO_DEPTH);
        printf("    Occ avg (enq->report):  %.2f\n", span > 0.0 ? integral / span : 0.0);
        printf("    Occ avg (push-sampled): %.2f / %u (%.1f%%)  [n=%lu]\n",
               delayed_upd_push_cnt_ ? (double)delayed_upd_push_occ_sum_ / delayed_upd_push_cnt_ : 0.0,
               DEDUP_DELAYED_UPDATE_FIFO_DEPTH,
               delayed_upd_push_cnt_ ? 100.0 * delayed_upd_push_occ_sum_ / delayed_upd_push_cnt_ / DEDUP_DELAYED_UPDATE_FIFO_DEPTH : 0.0,
               (unsigned long)delayed_upd_push_cnt_);
        printf("    Push hard-full blocks:  %lu (max %.1f ns)\n",
               (unsigned long)delayed_upd_push_block_cnt_, delayed_upd_push_block_max_ns_);
        printf("    Residency avg/max:      %.1f / %.1f ns\n",
               drained > 0 ? delayed_upd_residency_total_ns_ / (double)drained : 0.0,
               delayed_upd_residency_max_ns_);
    }
#endif

    // 并发吞吐
    if (window_ns > 0) {
        printf("  [Throughput]\n");
        printf("    Results/cycle:       %.3f  (total_tasks / window, clock=1ns)\n",
               (double)total_tasks / window_ns);
        printf("    Aggregate busy/win:  %.2f  (>1 means cross-RAM concurrency)\n",
               total_busy / window_ns);
    }
    printf("==================================================\n\n");
}

// [walker多RAM] Walker Cache多RAM统计报告: Hash单元/每组RAM利用率/前置查询计数
void CacheSubsystem::print_walker_multi_ram_report() const {
    printf("\n========== Walker Cache Multi-RAM Report ==========\n");
    printf("  Config:               num_rams=%u/subtable, workers=%u (三级子表独立分RAM), ram_fifo_depth=%u\n",
           walker_num_rams_, walker_total_workers_, walker_cache_->ram_fifo_depth());
    printf("  Front lookups:        %lu (前置查询总数)\n",
           (unsigned long)walker_front_lookup_count_);

    if (walker_hash_task_count_ == 0) {
        printf("  No tasks processed.\n");
        printf("===================================================\n\n");
        return;
    }

    printf("  [Hash Unit]\n");
    printf("    Tasks hashed:        %lu (lookup+update, 不含S2/invalidate内联)\n",
           (unsigned long)walker_hash_task_count_);
    printf("    Busy time:           %.1f ns\n", walker_hash_busy_ns_);
    printf("    Backpressure count:  %lu\n", (unsigned long)walker_hash_backpressure_cnt_);
    printf("    Backpressure time:   %.1f ns (write-block on full RAM FIFO)\n",
           walker_hash_backpressure_ns_);

    double window_ns = walker_ram_last_end_ns_ - walker_ram_first_start_ns_;
    printf("  [Per-Worker: SubTable x RAM]  (window=%.1f ns)\n", window_ns);
    printf("    %-8s %-10s %-10s %-10s %-12s %-9s %-10s\n",
           "Worker", "SubOps", "Lookups", "Updates", "Busy(ns)", "Util(%)", "FIFO_Peak");
    uint64_t total_tasks = 0;
    double total_busy = 0.0;
    for (uint32_t w = 0; w < walker_total_workers_; ++w) {
        double util = (window_ns > 0) ? walker_ram_busy_ns_[w] / window_ns * 100.0 : 0.0;
        char wname[16];
        snprintf(wname, sizeof(wname), "C%u-R%u",
                 w / walker_num_rams_ + 1, w % walker_num_rams_);
        printf("    %-8s %-10lu %-10lu %-10lu %-12.1f %-9.2f %-10d\n",
               wname,
               (unsigned long)walker_ram_task_count_[w],
               (unsigned long)walker_ram_lookup_count_[w],
               (unsigned long)walker_ram_update_count_[w],
               walker_ram_busy_ns_[w], util, walker_ram_fifo_peak_[w]);
        total_tasks += walker_ram_task_count_[w];
        total_busy += walker_ram_busy_ns_[w];
    }
    printf("    %-8s %-10lu %-10s %-10s %-12.1f\n",
           "SUM", (unsigned long)total_tasks, "-", "-", total_busy);

    if (window_ns > 0) {
        printf("  [Throughput]\n");
        printf("    SubOps/cycle:        %.3f  (total_subops / window, clock=1ns)\n",
               (double)total_tasks / window_ns);
        printf("    Aggregate busy/win:  %.2f  (>1 means cross-RAM concurrency)\n",
               total_busy / window_ns);
    }

    // ---- [DATA-ONLY] Walker Cache C1/C2/C3 各级 4 组 RAM 分布均衡度 ----
    // 仅统计 is_ctrl_pkt=false 的子操作(排除 SQ/CQ/MSI 控制包);
    // 评估各级子表的 hash(va_segment, gscid, pscid)>>低位 在数据流量上是否均匀分桶。
    // 均衡度指标:
    //   - Share%   : 各 RAM 占本级数据总访问的百分比(理想 25.00%)
    //   - Max/Min  : 最热 RAM / 最冷 RAM 的比值(1.00=完全均衡)
    //   - Imbalance: (max-min)/avg × 100%(0=完全均衡)
    //   - Chi^2    : 卡方统计量, 自由度=3; >7.81 则 p<0.05 显著不均衡
    printf("  [Walker RAM Distribution - DATA ONLY (is_ctrl_pkt=false)]\n");
    for (uint32_t lv = 1; lv <= WALKER_LEVELS; ++lv) {
        uint64_t data_lu[WALKER_MAX_RAMS] = {0}, data_up[WALKER_MAX_RAMS] = {0};
        uint64_t total_lu = 0, total_up = 0;
        for (uint32_t r = 0; r < walker_num_rams_ && r < WALKER_MAX_RAMS; ++r) {
            const uint32_t w = (lv - 1) * walker_num_rams_ + r;
            data_lu[r] = walker_ram_data_lookup_count_[w];
            data_up[r] = walker_ram_data_update_count_[w];
            total_lu += data_lu[r];
            total_up += data_up[r];
        }
        const uint64_t total_all = total_lu + total_up;
        printf("    [C%u] Total data accesses: %lu (lookup=%lu, update=%lu)\n",
               lv, (unsigned long)total_all, (unsigned long)total_lu, (unsigned long)total_up);
        printf("      %-4s %12s %10s %12s %10s %12s %10s\n",
               "RAM", "DataLookups", "Share%", "DataUpdates", "Share%",
               "DataTotal", "Share%");
        uint64_t dmin = UINT64_MAX, dmax = 0;
        double chi2 = 0.0;
        const double expected = (walker_num_rams_ > 0) ? (double)total_all / walker_num_rams_ : 0.0;
        for (uint32_t r = 0; r < walker_num_rams_ && r < WALKER_MAX_RAMS; ++r) {
            const uint64_t n_lu = data_lu[r];
            const uint64_t n_up = data_up[r];
            const uint64_t n_all = n_lu + n_up;
            const double sh_lu = (total_lu > 0) ? 100.0 * n_lu / total_lu : 0.0;
            const double sh_up = (total_up > 0) ? 100.0 * n_up / total_up : 0.0;
            const double sh_all = (total_all > 0) ? 100.0 * n_all / total_all : 0.0;
            printf("      %-4u %12lu %9.2f%% %12lu %9.2f%% %12lu %9.2f%%\n",
                   r, (unsigned long)n_lu, sh_lu,
                   (unsigned long)n_up, sh_up,
                   (unsigned long)n_all, sh_all);
            if (n_all < dmin) dmin = n_all;
            if (n_all > dmax) dmax = n_all;
            if (expected > 0.0) {
                const double d = (double)n_all - expected;
                chi2 += d * d / expected;
            }
        }
        if (total_all > 0 && walker_num_rams_ > 0) {
            const double avg = (double)total_all / walker_num_rams_;
            const double imbalance = (avg > 0) ? 100.0 * (dmax - dmin) / avg : 0.0;
            const double mm = (dmin > 0) ? (double)dmax / dmin : 0.0;
            printf("      Evenness: Max=%lu, Min=%lu, Avg=%.1f, Max/Min=%.3f, Imbalance=%.2f%%, Chi2=%.2f (df=%u, p<0.05 threshold=7.81)\n",
                   (unsigned long)dmax, (unsigned long)dmin, avg, mm, imbalance,
                   chi2, walker_num_rams_ - 1);
            if (chi2 > 7.81) {
                printf("      Verdict: NOT BALANCED (chi2 > 7.81, p<0.05)\n");
            } else {
                printf("      Verdict: BALANCED (chi2 <= 7.81, p>=0.05)\n");
            }
        } else {
            printf("      No data-packet accesses recorded.\n");
        }
        // [Q2] 本级(C%u) 仅数据包 per-RAM 真实命中率 = data_lookup_hits / data_lookups
        printf("      [C%u Data-only per-RAM TRUE Hit Rate] (HitRate = DataHits / DataLookups)\n", lv);
        printf("      %-4s %12s %12s %10s\n", "RAM", "DataLookups", "DataHits", "HitRate%");
        {
            uint64_t agg_lu = 0, agg_hit = 0;
            for (uint32_t r = 0; r < walker_num_rams_ && r < WALKER_MAX_RAMS; ++r) {
                const uint32_t w = (lv - 1) * walker_num_rams_ + r;
                const uint64_t lu = walker_ram_data_lookup_count_[w];
                const uint64_t ht = walker_ram_data_lookup_hit_count_[w];
                const double hr = (lu > 0) ? 100.0 * ht / lu : 0.0;
                printf("      %-4u %12lu %12lu %9.2f%%\n", r,
                       (unsigned long)lu, (unsigned long)ht, hr);
                agg_lu += lu; agg_hit += ht;
            }
            const double agg_hr = (agg_lu > 0) ? 100.0 * agg_hit / agg_lu : 0.0;
            printf("      %-4s %12lu %12lu %9.2f%%  (C%u aggregate)\n",
                   "ALL", (unsigned long)agg_lu, (unsigned long)agg_hit, agg_hr, lv);
        }
    }

    // ---- [Walker RAM Lookup/Update 数目与占比 - DATA ONLY] ----
    //   C1/C2/C3 每子表分别给出 4 RAM 的 lookup 数目+占比、update 数目+占比(仅数据包)。
    printf("  [Walker RAM Lookup/Update Count & Share - DATA ONLY]\n");
    for (uint32_t lv = 1; lv <= WALKER_LEVELS; ++lv) {
        uint64_t tl = 0, tu = 0;
        for (uint32_t r = 0; r < walker_num_rams_ && r < WALKER_MAX_RAMS; ++r) {
            const uint32_t w = (lv - 1) * walker_num_rams_ + r;
            tl += walker_ram_data_lookup_count_[w];
            tu += walker_ram_data_update_count_[w];
        }
        printf("    -- C%u : total lookup=%lu, total update=%lu --\n",
               lv, (unsigned long)tl, (unsigned long)tu);
        printf("    %-4s %10s %9s %10s %9s\n", "RAM", "Lookups", "Share%", "Updates", "Share%");
        for (uint32_t r = 0; r < walker_num_rams_ && r < WALKER_MAX_RAMS; ++r) {
            const uint32_t w = (lv - 1) * walker_num_rams_ + r;
            const uint64_t lu = walker_ram_data_lookup_count_[w];
            const uint64_t up = walker_ram_data_update_count_[w];
            printf("    %-4u %10lu %8.2f%% %10lu %8.2f%%\n", r,
                   (unsigned long)lu, (tl > 0) ? 100.0 * lu / tl : 0.0,
                   (unsigned long)up, (tu > 0) ? 100.0 * up / tu : 0.0);
        }
    }
    printf("===================================================\n\n");
}

// ============================================================
// [walker多RAM] Walker Cache Hash 线程
// 轮询四入口: front_request(前置lookup) > request(S2/旧路径lookup)
//           > update > invalidate
// - VS lookup/update: hash 1拍后按级拆分子操作分发 walker_ram_fifo_[id]
//   (目标FIFO满则阻塞反压, Hash保持忙)
// - S2 lookup/update: 零延时模型, 内联执行(与旧调度器时序一致)
// - invalidate: 内联执行(低频操作)
// ============================================================
void CacheSubsystem::walker_hash_thread() {
    while (true) {
        bool processed = false;
        CacheMessage req;

        // 0. [失效] 最高优先: 失效指令(拆分子失效到RAM worker + join, 阻塞等待)
        if (walker_invalidate_fifo.num_available() > 0) {
            req = walker_invalidate_fifo.read();
            dispatch_walker_invalidate(req);
            processed = true;
        }
        // 1. update优先: 保证Walker更新及时可见(后续同段任务二次校验可命中)
        else if (walker_update_fifo.num_available() > 0) {
            req = walker_update_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            if (req.walker_is_s2_lookup) {
                // S2 Cache更新: 零延时, 内联执行
                const sc_time start = sc_time_stamp();
                trace_task_event("begin", "walker_cache", "update", req, start);
                CacheMessage resp = execute_walker_update_request(req);
                record_task_completion("walker_cache", start, sc_time_stamp());
                trace_task_event("end", "walker_cache", "update", req, start, &resp);
            } else {
                dispatch_walker_update(req);
            }
            processed = true;
        }
        // 2. [串行查询] 续查优先: 完成 in-flight 查询的下一级(有界: 每lookup至多2次续查)
        else if (walker_serial_fifo.num_available() > 0) {
            req = walker_serial_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            // 续查hash 1拍后分发单级子查询
            wait(clock_period_);
            dispatch_walker_sub_lookup(req, req.walker_sub_level);
            processed = true;
        }
        // 3. PTW关键路径lookup(S2/二次校验/旧路径): PTW在互斥锁内同步等待响应,
        // 优先于前置lookup(前置不在关键路径, 结果早于dedup路径完成即可),
        // 避免高速前置流饿死PTW查询
        else if (walker_request_fifo.num_available() > 0) {
            req = walker_request_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            req.walker_origin = 0;
            if (req.walker_is_s2_lookup) {
                // S2 Cache查询: 零延时, 内联执行
                const sc_time start = sc_time_stamp();
                trace_task_event("begin", "walker_cache", "lookup", req, start);
                CacheMessage resp = execute_walker_request(req);
                record_task_completion("walker_cache", start, sc_time_stamp());
                trace_task_event("end", "walker_cache", "lookup", req, start, &resp);
                push_fifo(walker_response_fifo, resp);
            } else {
                // 二次校验/旧路径VS lookup
                dispatch_walker_lookup(req);
            }
            processed = true;
        }
        // 4. 前置lookup(主数据通路, 非关键路径)
        else if (walker_front_request_fifo.num_available() > 0) {
            req = walker_front_request_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            req.walker_origin = 1;
            walker_front_lookup_count_++;
            dispatch_walker_lookup(req);
            processed = true;
        }

        if (!processed) {
            wait(walker_front_request_fifo.data_written_event() |
                 walker_serial_fifo.data_written_event() |
                 walker_request_fifo.data_written_event() |
                 walker_update_fifo.data_written_event() |
                 walker_invalidate_fifo.data_written_event());
        }
    }
}

// [walker多RAM] lookup串行分发: hash 1拍 + 注册join表项 + 仅分发C3首级
// [串行查询] C3→C2→C1 顺序查询: 首级仅查C3, miss后由join线程发续查
void CacheSubsystem::dispatch_walker_lookup(CacheMessage& req) {
    const sc_time start = sc_time_stamp();
    const double start_ns = start.to_seconds() * 1e9;
    trace_task_event("begin", "walker_cache", "lookup", req, start);

    // Hash 单元 1 拍
    wait(clock_period_);

    // 注册 join 表项: 串行模式每阶段仅1个子响应
    const uint64_t key = (static_cast<uint64_t>(req.walker_origin) << 56) |
                         req.task_id;
    WalkerJoinEntry entry;
    entry.base = req;
    entry.expected = 1;
    entry.next_level = walker_serial_next_level(3, req);  // C3 miss后续查C2
    entry.start_time = start;
    walker_join_pending_[key] = entry;

    // 仅分发C3首级子查询
    CacheMessage sub = req;
    sub.walker_sub_level = 3;
    dispatch_walker_sub_lookup(sub, 3);

    walker_hash_task_count_++;
    walker_hash_busy_ns_ += (sc_time_stamp().to_seconds() * 1e9 - start_ns);
}

// [串行查询] 级链: 3→2→(1仅Sv48非Sv39)→0
uint8_t CacheSubsystem::walker_serial_next_level(uint8_t level, const CacheMessage& req) {
    if (level == 3) return 2;
    if (level == 2) return (!walker_cache_->sv39_mode() && req.walker_sv48) ? 1 : 0;
    return 0;
}

// [walker多RAM] 单级子查询分发: worker索引 = (level-1)*num_rams + ram_id
// [优先级] origin=0(PTW关键路径)走高优先通道; origin=1(前置)走普通通道
void CacheSubsystem::dispatch_walker_sub_lookup(const CacheMessage& req, uint8_t level) {
    uint32_t ram_id = walker_cache_->compute_ram_id(
        level, req.gscid, req.pscid, req.iova,
        req.walker_addr_is_va, req.walker_from_two_stage,
        req.walker_sv48, req.walker_x4_mode);
    uint32_t worker_id = (level - 1) * walker_num_rams_ + ram_id;
    sc_fifo<CacheMessage>& target =
        (req.walker_origin == 0) ? *walker_ram_upd_fifo_[worker_id]
                                 : *walker_ram_fifo_[worker_id];
    const double wr_start_ns = sc_time_stamp().to_seconds() * 1e9;
    if (target.num_free() == 0) {
        walker_hash_backpressure_cnt_++;
    }
    CacheMessage sub = req;
    sub.walker_sub_level = level;
    target.write(sub);  // 阻塞写 = 反压
    const double wr_end_ns = sc_time_stamp().to_seconds() * 1e9;
    walker_hash_backpressure_ns_ += (wr_end_ns - wr_start_ns);
    int occ = walker_ram_fifo_[worker_id]->num_available();
    if (occ > walker_ram_fifo_peak_[worker_id]) walker_ram_fifo_peak_[worker_id] = occ;
}

// [walker多RAM] update拆分分发: hash 1拍 + 按kind/valid拆子更新(无响应)
void CacheSubsystem::dispatch_walker_update(CacheMessage& req) {
    walker_cache_->record_update_kind(req.walker_update_kind);
    if (req.walker_update_kind == WalkerUpdateKind::NONE) {
        printf("[t=%llu ns][WALKER_CACHE] UPDATE: NONE (skip), gscid=%u, pscid=%u, iova=0x%lx\n",
               (unsigned long long)sc_time_stamp().value()/1000,
               req.gscid, req.pscid, (unsigned long)req.iova);
        fflush(stdout);
        return;
    }

    const sc_time start = sc_time_stamp();
    const double start_ns = start.to_seconds() * 1e9;
    trace_task_event("begin", "walker_cache", "update", req, start);

    // Hash 单元 1 拍
    wait(clock_period_);

    const bool update_ptwc1 = req.walker_update_kind == WalkerUpdateKind::PTWC_1_2_3;
    const bool update_ptwc2 = req.walker_update_kind == WalkerUpdateKind::PTWC_1_2_3 ||
                              req.walker_update_kind == WalkerUpdateKind::PTWC_2_3;

    printf("[t=%llu ns][WALKER_CACHE] UPDATE: gscid=%u, pscid=%u, iova=0x%lx, kind=%d, ptwc1_valid=%d, ptwc2_valid=%d, ptwc3_valid=%d\n",
           (unsigned long long)sc_time_stamp().value()/1000,
           req.gscid, req.pscid, (unsigned long)req.iova,
           static_cast<int>(req.walker_update_kind),
           req.walker_data_ptwc1.reserved.valid,
           req.walker_data_ptwc2.reserved.valid,
           req.walker_data_ptwc3.reserved.valid);
    fflush(stdout);

    // 按级分发子更新: valid=0跳过(与旧update_entry一致, 防缓存污染)
    // worker索引 = (level-1)*num_rams + ram_id; 写入update高优先FIFO
    auto dispatch_one = [&](uint8_t level, const WalkerData& data) {
        if (!data.reserved.valid) return;
        CacheMessage sub = req;
        sub.walker_sub_level = level;
        uint32_t ram_id = walker_cache_->compute_ram_id(
            level, req.gscid, req.pscid, req.iova,
            walker_va_pa_flag(data), walker_stage_flag(data),
            walker_sv48_flag(data), walker_x4_mode_flag(data));
        uint32_t worker_id = (level - 1) * walker_num_rams_ + ram_id;
        const double wr_start_ns = sc_time_stamp().to_seconds() * 1e9;
        if (walker_ram_upd_fifo_[worker_id]->num_free() == 0) {
            walker_hash_backpressure_cnt_++;
        }
        walker_ram_upd_fifo_[worker_id]->write(sub);
        const double wr_end_ns = sc_time_stamp().to_seconds() * 1e9;
        walker_hash_backpressure_ns_ += (wr_end_ns - wr_start_ns);
    };

    if (update_ptwc1 && !walker_cache_->sv39_mode() &&
        walker_sv48_flag(req.walker_data_ptwc1)) {
        dispatch_one(1, req.walker_data_ptwc1);
    }
    if (update_ptwc2) {
        dispatch_one(2, req.walker_data_ptwc2);
    }
    dispatch_one(3, req.walker_data_ptwc3);

    walker_hash_task_count_++;
    walker_hash_busy_ns_ += (sc_time_stamp().to_seconds() * 1e9 - start_ns);
}

// ============================================================
// [walker多RAM] 每组 RAM 独立 worker
// 同一 RAM FIFO 内子操作串行(单线程天然保证原子段不可分割);
// 不同 RAM 组并发。lookup子响应写 walker_join_fifo; update无响应。
// ============================================================
void CacheSubsystem::walker_ram_worker_thread(int ram_id) {
    sc_fifo<CacheMessage>& my_fifo = *walker_ram_fifo_[ram_id];
    sc_fifo<CacheMessage>& my_upd_fifo = *walker_ram_upd_fifo_[ram_id];
    while (true) {
        // update高优先(写口优先): 保证Walker更新不被积压的lookup延迟
        CacheMessage sub;
        bool is_upd_channel = my_upd_fifo.nb_read(sub);
        if (!is_upd_channel && !my_fifo.nb_read(sub)) {
            wait(my_upd_fifo.data_written_event() | my_fifo.data_written_event());
            continue;
        }
#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::BusyGuard md_busy(md_active_);
#endif
        const double start_ns = sc_time_stamp().to_seconds() * 1e9;
        if (walker_ram_first_start_ns_ < 0.0) walker_ram_first_start_ns_ = start_ns;

        sc_time ram_latency = SC_ZERO_TIME;
        if (sub.msg_type == CacheMsgType::WALKER_INVALIDATE) {
            // [失效] 失效子任务: 在本(级xRAM)worker 串行执行(与 in-flight 子查询原子)
            const uint8_t level = sub.walker_sub_level;
            const uint32_t rid = sub.inval_sub_ram_id;
            uint32_t affected = 0;
            if (sub.inval_lazy_sweep) {
                affected = walker_cache_->lazy_sweep_ram(level, rid,
                                                         walker_inval_sweep_new_vn_,
                                                         ram_latency);
            } else if (sub.inval_sub_has_set) {
                affected = walker_cache_->invalidate_set_ram(level, sub.inval_sub_set,
                                                             sub, ram_latency);
            } else {
                affected = walker_cache_->invalidate_ram_range(level, rid, sub,
                                                               ram_latency);
            }
            wait(ram_latency);
            walker_inval_affected_ += affected;
            if (walker_inval_remaining_ > 0) walker_inval_remaining_--;
            walker_inval_sub_done_event_.notify(SC_ZERO_TIME);
            const double iend_ns = sc_time_stamp().to_seconds() * 1e9;
            walker_ram_task_count_[ram_id]++;
            walker_ram_busy_ns_[ram_id] += (iend_ns - start_ns);
            if (iend_ns > walker_ram_last_end_ns_) walker_ram_last_end_ns_ = iend_ns;
            continue;
        }
        if (sub.msg_type == CacheMsgType::WALKER_LOOKUP) {
            WalkerData data;
            bool hit = walker_cache_->lookup_level_ram(
                sub.walker_sub_level, sub.gscid, sub.pscid, sub.iova,
                sub.walker_addr_is_va, sub.walker_from_two_stage,
                sub.walker_sv48, sub.walker_x4_mode, data, ram_latency);
            wait(ram_latency);  // RAM原子段延时(同RAM串行, 跨RAM并发)
#if TEST_CFG_MULTI_DEVICE_SCENE
            md_record(sub, "walker_vs_ram", "lookup", start_ns, hit, ram_id, sub.walker_sub_level);
#endif
            CacheMessage jr = sub;
            jr.hit = hit;
            if (hit) jr.walker_data = data;
            jr.latency = ram_latency;
            walker_join_fifo.write(jr);
            walker_ram_lookup_count_[ram_id]++;
            // [STAT] 仅数据包计数(SQ/CQ/MSI 控制包不计入)
            if (!sub.is_ctrl_pkt) walker_ram_data_lookup_count_[ram_id]++;
            // [STAT] per-(level x RAM) 真实命中计数(全量 + 仅数据包)
            if (hit) {
                walker_ram_lookup_hit_count_[ram_id]++;
                if (!sub.is_ctrl_pkt) walker_ram_data_lookup_hit_count_[ram_id]++;
            }
        } else {
            // WALKER_UPDATE 子更新: 按级选择payload
            const WalkerData& d =
                (sub.walker_sub_level == 1) ? sub.walker_data_ptwc1 :
                (sub.walker_sub_level == 2) ? sub.walker_data_ptwc2 :
                                              sub.walker_data_ptwc3;
            walker_cache_->update_level_ram(sub.walker_sub_level, sub.gscid,
                                            sub.pscid, sub.iova, d, ram_latency);
            wait(ram_latency);
#if TEST_CFG_MULTI_DEVICE_SCENE
            md_record(sub, "walker_vs_ram", "update", start_ns, false, ram_id, sub.walker_sub_level);
#endif
            walker_ram_update_count_[ram_id]++;
            // [STAT] 仅数据包计数(SQ/CQ/MSI 控制包不计入)
            if (!sub.is_ctrl_pkt) walker_ram_data_update_count_[ram_id]++;
        }

        const double end_ns = sc_time_stamp().to_seconds() * 1e9;
        walker_ram_task_count_[ram_id]++;
        walker_ram_busy_ns_[ram_id] += (end_ns - start_ns);
        if (end_ns > walker_ram_last_end_ns_) walker_ram_last_end_ns_ = end_ns;
    }
}

// ============================================================
// [walker多RAM] Join 线程: 聚合子响应, C3>C2>C1 仲裁, 按origin回响应
// ============================================================
void CacheSubsystem::walker_join_thread() {
    while (true) {
        CacheMessage jr = walker_join_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
        iommu_md::BusyGuard md_busy(md_active_);
#endif
        const uint64_t key = (static_cast<uint64_t>(jr.walker_origin) << 56) |
                             jr.task_id;
        auto it = walker_join_pending_.find(key);
        if (it == walker_join_pending_.end()) {
            printf("[WALKER_JOIN] WARNING: task_id=%u origin=%u sub-response without join entry!\n",
                   (unsigned)jr.task_id, jr.walker_origin);
            fflush(stdout);
            continue;
        }
        WalkerJoinEntry& e = it->second;
        e.received++;
        if (jr.walker_sub_level >= 1 && jr.walker_sub_level <= 3) {
            e.hit[jr.walker_sub_level] = jr.hit;
            e.data[jr.walker_sub_level] = jr.walker_data;
        }
        // [串行查询] 累计各级原子段延时(原为取max的并行口径)
        e.max_ram_latency = e.max_ram_latency + jr.latency;

        // [串行查询] C3→C2→C1 顺序短路: 命中即结束; miss且有下一级→发续查
        uint8_t hit_level = 0;
        if (jr.hit) {
            hit_level = jr.walker_sub_level;
        } else if (e.next_level != 0) {
            CacheMessage cont = e.base;
            cont.walker_sub_level = e.next_level;
            e.next_level = walker_serial_next_level(e.next_level, e.base);
            e.max_ram_latency = e.max_ram_latency + clock_period_;  // 续查hash 1拍
            e.received = 0;   // 串行模式expected恒为1, 等待下一级子响应
            push_fifo(walker_serial_fifo, cont);
            continue;
        }
        // 否则: 全级miss, hit_level=0
        CacheMessage resp;
        resp.msg_type = CacheMsgType::CACHE_RESPONSE;
        resp.task_id = e.base.task_id;
        resp.iova = e.base.iova;
        resp.gscid = e.base.gscid;
        resp.pscid = e.base.pscid;
        resp.walker_origin = e.base.walker_origin;
        resp.hit = (hit_level > 0);
        if (resp.hit) {
            resp.walker_level = hit_level;
            resp.walker_data = e.data[hit_level];
        }
        resp.latency = clock_period_ + e.max_ram_latency;  // hash + 最慢子查询

        walker_cache_->record_vs_lookup_result(
            hit_level, resp.hit && walker_is_leaf(resp.walker_data));
        if (resp.hit) {
            printf("[t=%llu ns][WALKER_CACHE] HIT: ptw_c%u (level=%u), gscid=%u, pscid=%u, iova=0x%lx, next_ppn=0x%lx%s\n",
                   (unsigned long long)sc_time_stamp().value()/1000,
                   hit_level, hit_level, e.base.gscid, e.base.pscid,
                   (unsigned long)e.base.iova,
                   (unsigned long)resp.walker_data.next_ppn,
                   walker_is_leaf(resp.walker_data) ? " [LEAF]" : "");
        } else {
            printf("[t=%llu ns][WALKER_CACHE] MISS: all levels, gscid=%u, pscid=%u, iova=0x%lx\n",
                   (unsigned long long)sc_time_stamp().value()/1000,
                   e.base.gscid, e.base.pscid, (unsigned long)e.base.iova);
        }
        fflush(stdout);

        record_task_completion("walker_cache", e.start_time, sc_time_stamp());
        trace_task_event("end", "walker_cache", "lookup", e.base, e.start_time, &resp);

        const uint8_t origin = e.base.walker_origin;
        walker_join_pending_.erase(it);

        if (origin == 1) {
            push_fifo(walker_front_response_fifo, resp);
        } else {
            push_fifo(walker_response_fifo, resp);
        }
    }
}

// ============================================================
// [失效] dispatch_walker_invalidate - Walker Cache 失效拆分到 (级xRAM) worker + join
// 由 walker_hash_thread 调用; 阻塞直至所有子失效完成。
// 涉及级: C3/C2 必, C1 仅 Sv48(非 Sv39) 模式。
// 非 LAZY 统一走 invalidate_ram_range(扫本级RAM区间, 内部按 line_matches_inval
// 处理 GLOBAL/gscid/pscid/addr): Walker 子表仅 64 set, 扫描代价低且正确
// 处理 va_pa/sv48/x4 多组合(按每条 tag 自身 flag 比较段)。
// 小范围地址失效: 由于同一 addr 在不同 flag 组合下映射到不同 va_segment,
// 这里以 SCAN 覆盖全部组合(与旧 invalidate_vma 的多组合枚举等效)。
// ============================================================
void CacheSubsystem::dispatch_walker_invalidate(const CacheMessage& cmd) {
#if TEST_CFG_MULTI_DEVICE_SCENE
    iommu_md::BusyGuard md_busy(md_active_);
#endif
    const sc_time start = sc_time_stamp();
    trace_task_event("begin", "walker_cache", "invalidate", cmd, start);

    wait(clock_period_);  // Hash 1 拍

    walker_inval_affected_ = 0;
    walker_inval_sweep_new_vn_ = lib_.global_vn();

    uint8_t levels[3];
    uint8_t lc = 0;
    levels[lc++] = 3;
    levels[lc++] = 2;
    if (!walker_cache_->sv39_mode()) levels[lc++] = 1;

    std::vector<CacheMessage> subs;
    for (uint8_t li = 0; li < lc; ++li) {
        const uint8_t level = levels[li];
        for (uint32_t r = 0; r < walker_num_rams_; ++r) {
            CacheMessage s = cmd;
            s.msg_type = CacheMsgType::WALKER_INVALIDATE;
            s.walker_sub_level = level;
            s.inval_sub_ram_id = r;
            s.inval_sub_has_set = false;   // 扫本(级xRAM)区间
            subs.push_back(s);
        }
    }

    walker_inval_remaining_ = static_cast<int>(subs.size());
    for (auto& s : subs) {
        const uint32_t worker_id =
            (s.walker_sub_level - 1) * walker_num_rams_ + s.inval_sub_ram_id;
        walker_ram_upd_fifo_[worker_id]->write(s);  // 高优先通道, 阻塞写=反压
    }
    while (walker_inval_remaining_ > 0) {
        wait(walker_inval_sub_done_event_);
    }

    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = cmd.task_id;
    resp.cmd_type = cmd.cmd_type;
    resp.affected_entries = walker_inval_affected_;
    record_task_completion("walker_cache", start, sc_time_stamp());
    trace_task_event("end", "walker_cache", "invalidate", cmd, start, &resp);
    push_fifo(walker_invalidate_response_fifo, resp);
}

// ============================================================
// [MSI] MSIPT Cache 调度线程 (invalidate > update > lookup)
// 失效由失效 pipeline 经 msi_invalidate_fifo 下发; 单线程串行处理,
// 与 lookup/update 无阵列竞态, 失效响应经 msi_invalidate_response_fifo 返回。
// ============================================================
void CacheSubsystem::msi_scheduler_thread() {
    while (true) {
        // 0. [失效] 最高优先: MSIPT 失效(GLOBAL全清 / 按device失效),
        //    与 lookup/update 同线程串行, 消除阵列竞态并保证失效及时可见。
        if (msi_invalidate_fifo.num_available() > 0) {
            CacheMessage req = msi_invalidate_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "msipt_cache", "invalidate", req, start);
            CacheMessage resp = execute_msi_invalidate_request(req);
            record_task_completion("msipt_cache", start, sc_time_stamp());
            trace_task_event("end", "msipt_cache", "invalidate", req, start, &resp);
            push_fifo(msi_invalidate_response_fifo, resp);
            continue;
        }
        if (msi_update_fifo.num_available() > 0) {
            CacheMessage req = msi_update_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "msipt_cache", "update", req, start);
            CacheMessage resp = execute_msi_update_request(req);
            record_task_completion("msipt_cache", start, sc_time_stamp());
            trace_task_event("end", "msipt_cache", "update", req, start, &resp);
            continue;
        }
        if (msi_request_fifo.num_available() > 0) {
            CacheMessage req = msi_request_fifo.read();
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::BusyGuard md_busy(md_active_);
#endif
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "msipt_cache", "lookup", req, start);
            CacheMessage resp = execute_msi_request(req);
            record_task_completion("msipt_cache", start, sc_time_stamp());
            trace_task_event("end", "msipt_cache", "lookup", req, start, &resp);
            push_fifo(msi_response_fifo, resp);
            continue;
        }
        wait(msi_update_fifo.data_written_event() |
             msi_request_fifo.data_written_event() |
             msi_invalidate_fifo.data_written_event());
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
// [dedup多RAM] 运行于 dedup_ram_worker_thread (进程上下文, 允许 wait 反压;
//   Buffer满阻塞仅影响本RAM, 其余RAM继续工作)
// 分支:
//   HIT 主占位(is_req=1)   -> 挂 Buffer 链尾, 任务挂起(dedup_suspended)
//   HIT 预取占位(is_req=0) -> 分配 Buffer, 升级 is_req=1, 任务挂起
//   MISS                   -> 插主占位 + 生成D个预取任务写inside_fifo, 返回 MISS -> PTW
//   降级(insert 失败)       -> 释放 Buffer, dedup_bypass, 返回 MISS -> PTW
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

    uint64_t page_iova = req.iova & ~0xFFFULL;
#if TEST_CFG_DEDUP_UNIT_V1123
    // [1123] bank绑定: 页hash决定bank, 本任务全部Buffer操作走该bank(链不跨bank);
    //   buf 即本RAM worker 服务的 bank(与 hash 单元同一 compute_ram_id 口径)。
    const uint32_t bank = dedup_cache_->compute_ram_id(req.gscid, req.pscid, page_iova);
    DedupBuffer* buf = dedup_banks_[bank];
#else
    DedupBuffer* buf = dedup_buffer_;
#endif
    DedupCacheLine* line = dedup_cache_->lookup(req.gscid, req.pscid, page_iova);

    // [STAT需求2] 去重Cache命中率统计: 每次lookup计一次访问, line!=nullptr为命中
    dedup_lookup_total_++;
    if (line != nullptr) dedup_lookup_hit_++; else dedup_lookup_miss_++;
    if (!req.dedup_is_prefetch) {  // 主任务去重请求(非预取占位)
        dedup_req_lookup_total_++;
        if (line != nullptr) dedup_req_lookup_hit_++; else dedup_req_lookup_miss_++;
    }

    if (line != nullptr) {
        // ============ HIT 占位CL ============
        if (line->is_req) {
            // -------- 分支1: 主占位CL (is_req=1) 挂 Buffer 链尾 --------
            uint16_t head_idx = line->head_index;
#if TEST_CFG_MULTI_DEVICE_SCENE
            iommu_md::require(head_idx < DEDUP_BUFFER_ENTRIES, "去重链头越界");
            auto check_entry = [&](uint16_t idx) {
                iommu_md::require(idx < DEDUP_BUFFER_ENTRIES, "去重链尾越界");
                const auto& e = buf->entries[idx];
                iommu_md::require(e.is_valid() && e.task_ptr && e.gscid == req.gscid &&
                    e.pscid == req.pscid && (e.iova & ~0xFFFULL) == page_iova, "去重挂链上下文不匹配");
#if TEST_CFG_DEDUP_UNIT_V1123
                // [1123] 链不跨bank不变量: 链上entry的页hash必须属于本bank
                iommu_md::require(dedup_cache_->compute_ram_id(e.gscid, e.pscid, e.iova & ~0xFFFULL)
                                  == bank, "去重链entry跨bank违例");
#endif
            };
            check_entry(head_idx);
            check_entry(buf->entries[head_idx].tail_index);
#endif
            uint16_t tail_idx = buf->entries[head_idx].tail_index;

            uint16_t new_idx = buf->allocate_entry();
#if TEST_CFG_DEDUP_ADMISSION_CTRL || TEST_CFG_DEDUP_UNIT_V1123
            if (new_idx == DEDUP_BUFFER_INVALID_IDX) {
                // 准入控制模式: scheduler预留保证有空槽, 理论不可达; 若到达为计数异常, 回退等释放兕底(仅诊断)
                printf("[DEDUP_ADM_ERR] task_id=%u -> allocate failed under admission (main HIT, valid=%u reserved=%u)\n",
                       (unsigned)req.task_id, buf->get_valid_count(),
#if TEST_CFG_DEDUP_UNIT_V1123
                       (unsigned)dedup_reserved_bank_[bank]);
#else
                       (unsigned)dedup_reserved_count_);
#endif
                fflush(stdout);
                while (buf->is_full()) wait(buf->free_event);
                new_idx = buf->allocate_entry();
            }
#if TEST_CFG_DEDUP_UNIT_V1123
            dedup_reserved_bank_[bank]--;  // reserved -> valid
            dedup_admission_released_ev_.notify();
#else
            dedup_reserved_count_--;  // reserved -> valid
#endif
#else
            if (new_idx == DEDUP_BUFFER_INVALID_IDX) {
                // [FIX] Buffer满时等待PTW并发释放: 阻塞当前dedup worker直到PTW有空闲槽位
                // 避免通过多级FIFO转发后再在PTW入口阻塞, 直接在源头控制流量
                while (ptw_capacity_check_cb_ && !ptw_capacity_check_cb_()) {
                    printf("[DEDUP_CACHE_BYPASS] task_id=%u -> Buffer full (main placeholder HIT), PTW full, waiting...\n",
                           (unsigned)req.task_id);
                    fflush(stdout);
                    wait(dedup_buffer_full_event_);
                }
                // PTW有空闲槽位, 执行bypass
                dedup_buffer_full_bypass_count_++;
#if TEST_CFG_MULTI_DEVICE_SCENE
                md_record(req, "dedup", "buffer_full_bypass", iommu_md::now_ns());
#endif
                resp.hit = false;
                resp.dedup_bypass = true;
                // [bypass预取] bypass任务保留预取能力(不禁用), PTW完成后走late-spawn真正执行预取;
                //   组标记is_bypass, 刷新时跳过dedup_update(不碰去重cache/buffer), 仅更新PT Cache并直接转发主任务
                resp.prefetch_enabled = req.prefetch_enabled;
                resp.prefetch_depth = req.prefetch_depth;
                printf("[DEDUP_CACHE_BYPASS] task_id=%u -> Buffer full (main placeholder HIT), bypass to PTW (bypass_count=%lu)\n",
                       (unsigned)req.task_id, (unsigned long)dedup_buffer_full_bypass_count_);
                fflush(stdout);
                return resp;
            }
#endif
            if (new_idx != DEDUP_BUFFER_INVALID_IDX) {
                auto& new_entry = buf->entries[new_idx];
                new_entry.gscid = req.gscid;
                new_entry.pscid = req.pscid;
                new_entry.iova = req.iova;
                new_entry.stage = req.stage;
                new_entry.sv48 = req.pt_sv48;
                new_entry.gstage_x4 = req.pt_gstage_x4;
                new_entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);
                new_entry.next_index = DEDUP_BUFFER_INVALID_IDX;
                new_entry.tail_index = DEDUP_BUFFER_INVALID_IDX;  // 非链头
                new_entry.alloc_src = 2;  // [STAT] 被去重挂起的任务

                buf->entries[tail_idx].next_index = new_idx;
                buf->entries[head_idx].tail_index = new_idx;  // 仅写 Buffer

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
            uint16_t new_idx = buf->allocate_entry();
#if TEST_CFG_DEDUP_ADMISSION_CTRL || TEST_CFG_DEDUP_UNIT_V1123
            if (new_idx == DEDUP_BUFFER_INVALID_IDX) {
                printf("[DEDUP_ADM_ERR] task_id=%u -> allocate failed under admission (prefetch HIT, valid=%u reserved=%u)\n",
                       (unsigned)req.task_id, buf->get_valid_count(),
#if TEST_CFG_DEDUP_UNIT_V1123
                       (unsigned)dedup_reserved_bank_[bank]);
#else
                       (unsigned)dedup_reserved_count_);
#endif
                fflush(stdout);
                while (buf->is_full()) wait(buf->free_event);
                new_idx = buf->allocate_entry();
            }
#if TEST_CFG_DEDUP_UNIT_V1123
            dedup_reserved_bank_[bank]--;  // reserved -> valid
            dedup_admission_released_ev_.notify();
#else
            dedup_reserved_count_--;  // reserved -> valid
#endif
#else
            if (new_idx == DEDUP_BUFFER_INVALID_IDX) {
                // [FIX] Buffer满时等待PTW并发释放: 阻塞当前dedup worker直到PTW有空闲槽位
                while (ptw_capacity_check_cb_ && !ptw_capacity_check_cb_()) {
                    printf("[DEDUP_CACHE_BYPASS] task_id=%u -> Buffer full (prefetch placeholder HIT), PTW full, waiting...\n",
                           (unsigned)req.task_id);
                    fflush(stdout);
                    wait(dedup_buffer_full_event_);
                }
                // PTW有空闲槽位, 执行bypass
                dedup_buffer_full_bypass_count_++;
#if TEST_CFG_MULTI_DEVICE_SCENE
                md_record(req, "dedup", "buffer_full_bypass", iommu_md::now_ns());
#endif
                resp.hit = false;
                resp.dedup_bypass = true;
                // [bypass预取] bypass任务保留预取能力(不禁用), PTW完成后走late-spawn真正执行预取;
                //   组标记is_bypass, 刷新时跳过dedup_update(不碰去重cache/buffer), 仅更新PT Cache并直接转发主任务
                resp.prefetch_enabled = req.prefetch_enabled;
                resp.prefetch_depth = req.prefetch_depth;
                printf("[DEDUP_CACHE_BYPASS] task_id=%u -> Buffer full (prefetch placeholder HIT), bypass to PTW (bypass_count=%lu)\n",
                       (unsigned)req.task_id, (unsigned long)dedup_buffer_full_bypass_count_);
                fflush(stdout);
                return resp;
            }
#endif
            if (new_idx != DEDUP_BUFFER_INVALID_IDX) {
                auto& entry = buf->entries[new_idx];
                entry.gscid = req.gscid;
                entry.pscid = req.pscid;
                entry.iova = req.iova;
                entry.stage = req.stage;
                entry.sv48 = req.pt_sv48;
                entry.gstage_x4 = req.pt_gstage_x4;
                entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);
                entry.next_index = DEDUP_BUFFER_INVALID_IDX;
                entry.tail_index = new_idx;  // 链头自引用
                entry.alloc_src = 1;  // [STAT] 链头, 由预取占位行(is_req=0)升级而来

                // 升级 dedup_cache 该 line 为主占位
                line->head_index = new_idx;
                line->is_req = true;
                // [FIX-UAF] 升级路径的owner挂buffer等待(不发PTW), 可被任意组的
                //   该页翻译结果flush("预取路径受益")
                line->owner_task_id = req.task_id;
                line->owner_suspended = true;

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
        // [hash冲突(步骤iv)] 目标set无空位置可写(全为 V=1&is_req=1, 无 V=0 且无可淘汰预取占位) ->
        //   该笔任务直接bypass给PTW(Walk): 不写去重cacheline, 不占去重Buffer;
        //   PTW完成后走late-spawn预取, 刷新时跳过dedup(is_bypass组)
        if (!dedup_cache_->set_has_writable_way(req.gscid, req.pscid, page_iova)) {
            dedup_hash_conflict_count_++;
            dedup_conflict_bypass_count_++;
#if TEST_CFG_MULTI_DEVICE_SCENE
            md_record(req, "dedup", "hash_conflict_bypass", iommu_md::now_ns());
#endif
#if TEST_CFG_DEDUP_ADMISSION_CTRL || TEST_CFG_DEDUP_UNIT_V1123
#if TEST_CFG_DEDUP_UNIT_V1123
            dedup_reserved_bank_[bank]--;  // 归还准入预留(该任务不占Buffer)
            dedup_admission_released_ev_.notify();
#else
            dedup_reserved_count_--;  // 归还准入预留(该任务不占Buffer)
#endif
#endif
            resp.hit = false;
            resp.dedup_bypass = true;
            resp.prefetch_enabled = req.prefetch_enabled;
            resp.prefetch_depth = req.prefetch_depth;
            printf("[DEDUP_HASH_CONFLICT] task_id=%u -> no writable way (all V=1&is_req=1), bypass to PTW (conflict=%lu)\n",
                   (unsigned)req.task_id, (unsigned long)dedup_hash_conflict_count_);
            fflush(stdout);
            return resp;
        }
        uint16_t head_idx = buf->allocate_entry();
#if TEST_CFG_DEDUP_ADMISSION_CTRL || TEST_CFG_DEDUP_UNIT_V1123
        if (head_idx == DEDUP_BUFFER_INVALID_IDX) {
            printf("[DEDUP_ADM_ERR] task_id=%u -> allocate failed under admission (MISS, valid=%u reserved=%u)\n",
                   (unsigned)req.task_id, buf->get_valid_count(),
#if TEST_CFG_DEDUP_UNIT_V1123
                   (unsigned)dedup_reserved_bank_[bank]);
#else
                   (unsigned)dedup_reserved_count_);
#endif
            fflush(stdout);
#if TEST_CFG_DEDUP_DELAYED_UPDATE
            // [去重延迟更新] 条件3: Buffer满, 通知排空线程加速转出延迟FIFO
            dedup_buffer_full_notify_ev_.notify();
#endif
            while (buf->is_full()) wait(buf->free_event);
            head_idx = buf->allocate_entry();
        }
#if TEST_CFG_DEDUP_UNIT_V1123
        dedup_reserved_bank_[bank]--;  // reserved -> valid
        dedup_admission_released_ev_.notify();
#else
        dedup_reserved_count_--;  // reserved -> valid
#endif
#else
        if (head_idx == DEDUP_BUFFER_INVALID_IDX) {
            // [FIX] Buffer满时等待PTW并发释放: 阻塞当前dedup worker直到PTW有空闲槽位
            while (ptw_capacity_check_cb_ && !ptw_capacity_check_cb_()) {
                printf("[DEDUP_CACHE_BYPASS] task_id=%u -> Buffer full (%u/%u, MISS), PTW full, waiting...\n",
                       (unsigned)req.task_id, dedup_buffer_->get_valid_count(), PT_DEDUP_BUFFER_SIZE);
                fflush(stdout);
                wait(dedup_buffer_full_event_);
            }
            // PTW有空闲槽位, 执行bypass
            dedup_buffer_full_bypass_count_++;
#if TEST_CFG_MULTI_DEVICE_SCENE
            md_record(req, "dedup", "buffer_full_bypass", iommu_md::now_ns());
#endif
            resp.hit = false;
            resp.dedup_bypass = true;
            // [bypass预取] bypass任务保留预取能力, PTW完成后走late-spawn预取(组标记is_bypass)
            resp.prefetch_enabled = req.prefetch_enabled;
            resp.prefetch_depth = req.prefetch_depth;
            printf("[DEDUP_CACHE_BYPASS] task_id=%u -> Buffer full (%u/%u, MISS), bypass to PTW (bypass_count=%lu)\n",
                   (unsigned)req.task_id, dedup_buffer_->get_valid_count(), PT_DEDUP_BUFFER_SIZE,
                   (unsigned long)dedup_buffer_full_bypass_count_);
            fflush(stdout);
            return resp;
        }
#endif

        if (head_idx != DEDUP_BUFFER_INVALID_IDX) {
            auto& entry = buf->entries[head_idx];
            entry.gscid = req.gscid;
            entry.pscid = req.pscid;
            entry.iova = req.iova;
            entry.stage = req.stage;
            entry.sv48 = req.pt_sv48;
            entry.gstage_x4 = req.pt_gstage_x4;
            entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);
            entry.next_index = DEDUP_BUFFER_INVALID_IDX;
            entry.tail_index = head_idx;  // 单 entry, tail=head
            entry.alloc_src = 0;  // [STAT] 链头, 由去重 MISS 新建

            bool insert_success = dedup_cache_->insert(
                req.gscid, req.pscid, page_iova, head_idx, true /*is_req*/,
                req.task_id, false /*owner行走中, 仅自己的组可flush*/);

            if (insert_success) {
                resp.hit = false;
                resp.dedup_head_index = head_idx;
                resp.prefetch_enabled = req.prefetch_enabled;
                resp.prefetch_depth = req.prefetch_depth;

                // [dedup多RAM] 预取异步化: 生成D个预取任务写 dedup_inside_request_fifo,
                // 经scheduler重新调度分发到不同RAM并行执行(不占用本任务原子段)
                uint32_t prefetch_issued = 0;
                if (req.prefetch_enabled && req.prefetch_depth > 0) {
                    uint32_t D = req.prefetch_depth;
                    uint32_t vpn0_in_page = (page_iova >> 12) & 0x1FF;
                    uint32_t max_D = 511 - vpn0_in_page;
                    if (D > max_D) D = max_D;

                    for (uint32_t d = 1; d <= D; d++) {
                        CacheMessage pf;
                        pf.msg_type = CacheMsgType::PT_LOOKUP;
                        pf.task_id = req.task_id;
                        pf.gscid = req.gscid;
                        pf.pscid = req.pscid;
                        pf.iova = page_iova + (d * 0x1000ULL);
                        pf.stage = req.stage;
                        pf.dedup_is_prefetch = true;
                        pf.timestamp = sc_time_stamp();
#if TEST_CFG_DEDUP_UNIT_V1123 && TEST_CFG_DEDUP_PREFETCH_BLOCK
                        // [P9->P1实验] 满则阻塞写(废除G1丢弃护栏): 阻塞发生在 worker
                        //   任务处理中途, 本bank upd 消费同时停摆 —— 死锁环复现点。
                        {
                            const double blk_t0 = sc_time_stamp().to_seconds() * 1e9;
                            const bool was_full = (dedup_inside_request_fifo.num_free() == 0);
                            if (was_full) {
                                dedup_pf_block_cnt_++;
                                printf("[DEDUP_PF_BLOCK] t=%.1f ns task_id=%u -> inter_fifo(1) full, worker blocked mid-task, prefetch iova=0x%lx\n",
                                       blk_t0, (unsigned)req.task_id, (unsigned long)pf.iova);
                                fflush(stdout);
                            }
                            dedup_inside_request_fifo.write(pf);  // 阻塞写
                            const double blk_ns = sc_time_stamp().to_seconds() * 1e9 - blk_t0;
                            if (blk_ns > 0.0) {
                                dedup_pf_block_ns_ += blk_ns;
                                if (blk_ns > dedup_pf_block_max_ns_) dedup_pf_block_max_ns_ = blk_ns;
                            }
                            prefetch_issued++;
                        }
#else
                        // nb_write: inside_fifo满则丢弃预取(保功能防死锁, 仅损失性能)
                        if (dedup_inside_request_fifo.nb_write(pf)) {
                            prefetch_issued++;
                        } else {
                            dedup_prefetch_dropped_++;
#if TEST_CFG_MULTI_DEVICE_SCENE
                            md_record(req, "dedup", "prefetch_drop", iommu_md::now_ns());
#endif
                            printf("[DEDUP_PREFETCH_DROP] task_id=%u -> inside_fifo full, prefetch iova=0x%lx dropped\n",
                                   (unsigned)req.task_id, (unsigned long)pf.iova);
                            fflush(stdout);
                        }
#endif
                    }
                }
                printf("[DEDUP_CACHE] task_id=%u -> MISS, inserted main placeholder (head=%u, iova=0x%lx), prefetch_issued=%u\n",
                       (unsigned)req.task_id, head_idx, (unsigned long)page_iova, prefetch_issued);
                fflush(stdout);
            } else {
                // 降级: dedup_cache set 全部为 is_req=1 受保护 -> 直接转发 PTW
                buf->free_entry(head_idx);
                resp.hit = false;
                resp.dedup_bypass = true;
                // [bypass预取] bypass任务保留预取能力(不禁用), PTW完成后走late-spawn真正执行预取;
                //   组标记is_bypass, 刷新时跳过dedup_update(不碰去重cache/buffer), 仅更新PT Cache并直接转发主任务
                resp.prefetch_enabled = req.prefetch_enabled;
                resp.prefetch_depth = req.prefetch_depth;
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
// [机制v2] 返回实际刷新的链长(无占位/预取占位/foreign跳过返回0),
//   供 worker 计算可变原子段 3 + 链长×DEDUP_UPD_FLUSH_CYCLES。
// ============================================================
uint32_t CacheSubsystem::execute_dedup_update(const CacheMessage& upd) {
    uint64_t page_iova = upd.iova & ~0xFFFULL;
#if TEST_CFG_DEDUP_UNIT_V1123
    // [1123] upd 与 req 同口径: 页hash决定bank, 链遍历只碰本bank Buffer。
    const uint32_t bank = dedup_cache_->compute_ram_id(upd.gscid, upd.pscid, page_iova);
    DedupBuffer* buf = dedup_banks_[bank];
#else
    DedupBuffer* buf = dedup_buffer_;
#endif
    DedupCacheLine* line = dedup_cache_->lookup(upd.gscid, upd.pscid, page_iova);
    if (line == nullptr) {
        // 无占位(降级任务/无等待任务的预取): 无操作
        return 0;
    }
    uint16_t head_index = line->head_index;
    bool is_req = line->is_req;

    if (!is_req) {
        // 预取占位, Buffer 未记录实际任务 -> 直接清除
        dedup_cache_->clear_line(line);
        printf("[DEDUP_UPDATE] iova=0x%lx -> prefetch placeholder cleared (no waiting task)\n", (unsigned long)page_iova);
        fflush(stdout);
        return 0;
    }

    // [FIX-UAF] 主占位owner竞态防护: fresh MISS的owner正在PTW行走(可能已建预取组),
    //   其Buffer链只能由owner自己的组(update.task_id==owner_task_id)flush;
    //   外国组的预取结果update(同页, 如他组预取页恰好等于本页)若提前flush,
    //   会转发/释放仍在PTW使用的task对象, 导致其预取组main_task悬垂(use-after-free,
    //   曾引发agg_flush"两阶段聚合结果PTE无效"确定性崩溃)。
    //   挂起等待的owner(owner_suspended=true, 升级路径)不受此限, 由任意组的
    //   该页翻译结果释放("预取路径受益")。
    //   跳过时不flush也不clear: 占位须保留给owner自己的组, 否则链上任务永不被释放。
    if (!line->owner_suspended && line->owner_task_id != 0 &&
        line->owner_task_id != static_cast<uint32_t>(upd.task_id)) {
        printf("[DEDUP_UPDATE] iova=0x%lx -> SKIP foreign update (owner task_id=%u walking in PTW, upd from task_id=%llu)\n",
               (unsigned long)page_iova, line->owner_task_id, (unsigned long long)upd.task_id);
        fflush(stdout);
        return 0;
    }

    // [机制v2] flush前先统计实际链长(回调内逐entry释放, flush后无法再数),
    //   链长非固定值, 按刷新时刻Buffer链上实际挂起的任务数动态判定:
    //   512B场景典型=8(4KB页=8×512B连续包); PTW提前返回时链可能不足8;
    //   128B场景最长可达32(4KB=32×128B)。UPDATE原子延时=3+实际链长×每entry刷新拍数。
    //   上限PT_DEDUP_BUFFER_SIZE防链异常死循环(回调内另有成环断言)
    uint32_t chain_len = 0;
    for (uint16_t cur = head_index;
         cur != DEDUP_BUFFER_INVALID_IDX && chain_len < DEDUP_BUFFER_ENTRIES;
         cur = buf->entries[cur].next_index) {
        chain_len++;
    }

    // 主占位: 先刷 Buffer 链(回调), 完成后再清除 dedup_cache line
    if (dedup_flush_cb_) {
        dedup_flush_cb_(head_index, upd);
    }
    dedup_cache_->clear_line(line);
    printf("[DEDUP_UPDATE] iova=0x%lx -> main placeholder flushed (head=%u, chain=%u) and cleared\n",
           (unsigned long)page_iova, head_index, chain_len);
    fflush(stdout);
    return chain_len;
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

    // [失效] 按 spec V1.0.1: IODIR.INVAL_DDT 仅失效 DDT/PDT 目录缓存,
    // 不再级联 PT/Walker/MSIPT(那是 IOTINVAL 的职责)。DC→PC 关联失效
    // 由 execute_invalidation_pipeline 统一编排。
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
    // [失效] 按 spec: PC 失效不再级联 PT/Walker(IOTINVAL 职责)。
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = static_cast<uint32_t>(contexts.size());
    resp.latency = latency;
    return resp;
}

// [失效][MSI] MSIPT Cache 失效执行:
//   GLOBAL  -> 全清(invalidate_global)
//   其余    -> 按 device_id 失效(invalidate_by_device); 本模型 MSI 页表指针
//             仅来自 DC(设备级), 故按设备失效即覆盖 spec 要求的关联失效范围。
CacheMessage CacheSubsystem::execute_msi_invalidate_request(const CacheMessage& req) {
    sc_time latency = SC_ZERO_TIME;
    uint32_t affected = 0;
    if (req.invalidate_mode == CacheInvalidateMode::GLOBAL) {
        affected = msipt_cache_->invalidate_global(&latency);
    } else {
        affected = msipt_cache_->invalidate_by_device(req.device_id, &latency);
    }
    wait(latency);
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.cmd_type = req.cmd_type;
    resp.affected_entries = affected;
    resp.latency = latency;
    return resp;
}

// [失效] execute_pt_invalidate_request / execute_walker_invalidate_request
// 已删除(死代码): 由 dispatch_pt_invalidate / dispatch_walker_invalidate 取代,
// 后者在 hash 线程内拆分子失效到各 RAM worker, 保证与 in-flight lookup/fill
// 的同 RAM 原子性, 并支持 SCAN_RANGE 枚举与 LIB 批量扫表。

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
    // 向 PT + Walker 发送同一失效(可选含 Walker), 各自经 hash 线程拆分到 RAM worker
    auto drive_pt = [&](const CacheMessage& base, CacheInvalidateMode mode,
                        bool lazy_sweep) {
        CacheMessage r = base;
        r.msg_type = CacheMsgType::PT_INVALIDATE;
        r.invalidate_mode = mode;
        r.inval_lazy_sweep = lazy_sweep;
        accumulate(drive_invalidate_and_wait(pt_invalidate_fifo,
                                             pt_invalidate_response_fifo, r));
    };
    auto drive_walker = [&](const CacheMessage& base, CacheInvalidateMode mode,
                            bool lazy_sweep) {
        CacheMessage r = base;
        r.msg_type = CacheMsgType::WALKER_INVALIDATE;
        r.invalidate_mode = mode;
        r.inval_lazy_sweep = lazy_sweep;
        accumulate(drive_invalidate_and_wait(walker_invalidate_fifo,
                                             walker_invalidate_response_fifo, r));
    };
    // [失效][MSI] 向 MSIPT Cache 下发失效(由 msi_scheduler_thread 最高优先执行)
    auto drive_msi = [&](const CacheMessage& base, CacheInvalidateMode mode) {
        CacheMessage r = base;
        r.msg_type = CacheMsgType::MSI_INVALIDATE;
        r.invalidate_mode = mode;
        accumulate(drive_invalidate_and_wait(msi_invalidate_fifo,
                                             msi_invalidate_response_fifo, r));
    };
    // [失效] LAZY 记录: 向共享 LIB 记录一次(递增全局VN); 未回绕则真正延迟(不下发),
    //   回绕则向 PT+Walker 下发批量扫表; LIB满降级为立即 SCAN。
    //   lazy_enabled_=false 时直接立即 SCAN。
    auto handle_lazy = [&](LibMatchClass cls) {
        if (!lazy_enabled_) {
            drive_pt(msg, CacheInvalidateMode::SCAN, false);
            drive_walker(msg, CacheInvalidateMode::SCAN, false);
            return;
        }
        bool full = false;
        const bool wrap = lib_.record(cls, msg.gscid, msg.pscid, full);
        if (full) {
            // LIB 满: 降级为立即扫表失效(保功能正确)
            drive_pt(msg, CacheInvalidateMode::SCAN, false);
            drive_walker(msg, CacheInvalidateMode::SCAN, false);
        } else if (wrap) {
            // VN 回绕: 批量扫表 PT + Walker
            drive_pt(msg, CacheInvalidateMode::SCAN, true);
            drive_walker(msg, CacheInvalidateMode::SCAN, true);
            // [FIX] 扫表完成后必须把 LIB entry 的 VN 重定基到回绕后的全局 VN(0)。
            //   否则 entry.vn 停留在 VN_MAX, 而扫表后新填入的行 vn=0, 查询旁路
            //   比对 0 < VN_MAX 成立 -> 新行被误判为 stale 并丢弃(人为 miss 风暴),
            //   直到下一笔失效命令到来才恢复。两个 drive_* 均为阻塞等待, 此处
            //   PT 与 Walker 的 sweep 子任务都已 join 完成, 重定基时序安全。
            lib_.rebase_after_sweep();
        }
        // else: 已记录到 LIB, 真正延迟失效(不下发, 查询旁路时生效)
    };

    // GLOBAL_INVAL: 所有 cache 全清 + 清 LIB/VN
    if (msg.cmd_type == InvalidCmdType::GLOBAL_INVAL) {
        lib_.clear();
        CacheMessage req = msg;
        req.invalidate_mode = CacheInvalidateMode::GLOBAL;
        req.inval_lazy_sweep = false;
        req.msg_type = CacheMsgType::DC_INVALIDATE;
        accumulate(drive_invalidate_and_wait(dc_invalidate_fifo,
                                             dc_invalidate_response_fifo, req));
        req.msg_type = CacheMsgType::PC_INVALIDATE;
        accumulate(drive_invalidate_and_wait(pc_invalidate_fifo,
                                             pc_invalidate_response_fifo, req));
        drive_pt(msg, CacheInvalidateMode::GLOBAL, false);
        drive_walker(msg, CacheInvalidateMode::GLOBAL, false);
        // [MSI] MSIPT Cache 全清
        drive_msi(msg, CacheInvalidateMode::GLOBAL);
        return resp;
    }

    switch (msg.cmd_type) {
        case InvalidCmdType::IODIR_INVAL_DDT: {
            // [失效] DC 失效(DV=1精准/DV=0全清) + PC 关联失效 + MSIPT 关联失效;
            // 不触及 PT/Walker。按 spec V1.0.1, INVAL_DDT 需同时失效与该设备关联的
            // MSI 页表缓存条目; 本模型 MSI 页表指针仅来自 DC(设备级),
            // 故按 device_id 失效即满足关联失效要求。
            CacheMessage dc_req = msg;
            dc_req.msg_type = CacheMsgType::DC_INVALIDATE;
            dc_req.invalidate_mode = msg.has_device_id ?
                CacheInvalidateMode::PRECISE : CacheInvalidateMode::GLOBAL;
            accumulate(drive_invalidate_and_wait(dc_invalidate_fifo,
                                                 dc_invalidate_response_fifo, dc_req));
            // PC 关联: DV=1 扫 DID 匹配(SCAN, has_process_id=false), DV=0 全清
            CacheMessage pc_req = msg;
            pc_req.msg_type = CacheMsgType::PC_INVALIDATE;
            pc_req.has_process_id = false;
            pc_req.invalidate_mode = msg.has_device_id ?
                CacheInvalidateMode::SCAN : CacheInvalidateMode::GLOBAL;
            accumulate(drive_invalidate_and_wait(pc_invalidate_fifo,
                                                 pc_invalidate_response_fifo, pc_req));
            // MSIPT 关联: DV=1 按 DID 失效, DV=0 全清(同 DC 失效范围)
            drive_msi(msg, msg.has_device_id ?
                CacheInvalidateMode::PRECISE : CacheInvalidateMode::GLOBAL);
            break;
        }
        case InvalidCmdType::IODIR_INVAL_PDT: {
            // [失效] PC 精准失效(DID+PID); 不触及 PT/Walker。
            // [MSI] spec 要求同时失效与进程上下文关联的 MSI 页表缓存条目;
            // 本模型 MSI 页表指针仅来自 DC(设备级), 不存在进程级 MSI 页表,
            // 故无对应 MSIPT 条目需失效(不动作即语义正确)。
            CacheMessage pc_req = msg;
            pc_req.msg_type = CacheMsgType::PC_INVALIDATE;
            pc_req.invalidate_mode = CacheInvalidateMode::PRECISE;
            accumulate(drive_invalidate_and_wait(pc_invalidate_fifo,
                                                 pc_invalidate_response_fifo, pc_req));
            break;
        }
        case InvalidCmdType::IOTINVAL_VMA: {
            // 方案 5.2 模式表, 位序(GV=has_gscid, AV=has_iova, PSCV=has_pscid)
            const bool GV = msg.has_gscid;
            const bool AV = msg.has_iova;
            const bool PSCV = msg.has_pscid;
            if (!AV) {
                if (!GV && !PSCV) {
                    // 000: 清表
                    lib_.clear();
                    drive_pt(msg, CacheInvalidateMode::GLOBAL, false);
                    drive_walker(msg, CacheInvalidateMode::GLOBAL, false);
                } else {
                    // 001/100/101: 扫表 -> LAZY
                    LibMatchClass cls =
                        (GV && PSCV) ? LibMatchClass::GSCID_PSCID :
                        (GV ? LibMatchClass::GSCID_ONLY : LibMatchClass::PSCID_ONLY);
                    handle_lazy(cls);
                }
            } else {
                // AV=1 (010/110/011/111): 小范围枚举扫表; PT 必, Walker 仅 NL=1
                drive_pt(msg, CacheInvalidateMode::SCAN_RANGE, false);
                if (msg.inval_nl) {
                    drive_walker(msg, CacheInvalidateMode::SCAN_RANGE, false);
                }
            }
            break;
        }
        case InvalidCmdType::IOTINVAL_GVMA: {
            // 方案 5.4: GV=0 清表; GV=1 转为按 GSCID 扫表 -> LAZY GSCID_ONLY
            if (!msg.has_gscid) {
                lib_.clear();
                drive_pt(msg, CacheInvalidateMode::GLOBAL, false);
                drive_walker(msg, CacheInvalidateMode::GLOBAL, false);
            } else {
                handle_lazy(LibMatchClass::GSCID_ONLY);
            }
            break;
        }
        default:
            break;
    }
    return resp;
}

// [失效] enqueue_cascade_invalidations 已删除(死代码):
// 按 spec V1.0.1, IODIR.INVAL_DDT/PDT 只失效 DDT/PDT 目录缓存, 不再级联
// PT/Walker/MSIPT(那是 IOTINVAL 的职责)。DC→PC 硬件关联失效已由
// execute_invalidation_pipeline 的 IODIR_INVAL_DDT 分支统一编排。

} // namespace iommu
