#include "subsystem/cache_subsystem.h"
#include <algorithm>
#include <cctype>
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
      dc_response_fifo(16),
      pc_response_fifo(16),
      pt_hit_response_fifo(16),
      pt_miss_response_fifo(16),
      walker_response_fifo(1),
      msi_response_fifo(1),
      dc_update_fifo(1),
      pc_update_fifo(1),
      pt_update_fifo(1),
      walker_update_fifo(1),
      msi_update_fifo(1),
      dc_invalidate_fifo(1),
      pc_invalidate_fifo(1),
      pt_invalidate_fifo(1),
      walker_invalidate_fifo(1),
      msi_invalidate_fifo(1),
      dc_invalidate_response_fifo(1),
      pc_invalidate_response_fifo(1),
      pt_invalidate_response_fifo(1),
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

    SC_THREAD(dc_worker_thread);
    SC_THREAD(pc_worker_thread);
    SC_THREAD(pt_worker_thread);
    SC_THREAD(walker_scheduler_thread);  // 统一的walker cache轮询调度线程
    SC_THREAD(msi_worker_thread);
    SC_THREAD(dc_update_worker_thread);
    SC_THREAD(pc_update_worker_thread);
    SC_THREAD(pt_update_worker_thread);
    SC_THREAD(msi_update_worker_thread);
    SC_THREAD(dc_invalidate_worker_thread);
    SC_THREAD(pc_invalidate_worker_thread);
    SC_THREAD(pt_invalidate_worker_thread);
    SC_THREAD(msi_invalidate_worker_thread);
    SC_THREAD(invalidation_worker_thread);
    
    // [NEW] 初始化PT Cache去重功能
    dedup_buffer_ = new DedupBuffer();
    pt_dedup_enabled_ = true;
    printf("[CACHE_SUBSYSTEM] PT Cache dedup enabled, Buffer created\n");
    fflush(stdout);
}

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

void CacheSubsystem::dc_worker_thread() {
    while (true) {
        CacheMessage req = dc_request_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "dc_cache", "lookup", req, start);
        CacheMessage resp = execute_dc_request(req);
        record_task_completion("dc_cache", start, sc_time_stamp());
        trace_task_event("end", "dc_cache", "lookup", req, start, &resp);
        push_fifo(dc_response_fifo, resp);
    }
}

void CacheSubsystem::pc_worker_thread() {
    while (true) {
        CacheMessage req = pc_request_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pc_cache", "lookup", req, start);
        CacheMessage resp = execute_pc_request(req);
        record_task_completion("pc_cache", start, sc_time_stamp());
        trace_task_event("end", "pc_cache", "lookup", req, start, &resp);
        push_fifo(pc_response_fifo, resp);
    }
}

void CacheSubsystem::pt_worker_thread() {
    while (true) {
        CacheMessage req = pt_request_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pt_cache", "lookup", req, start);
        CacheMessage resp = execute_pt_request(req);
        record_task_completion("pt_cache", start, sc_time_stamp());
        trace_task_event("end", "pt_cache", "lookup", req, start, &resp);
        if (resp.hit) {
            push_fifo(pt_hit_response_fifo, resp);
        } else {
            push_fifo(pt_miss_response_fifo, resp);
        }
    }
}

// Walker Cache 统一轮询调度线程
// 按 lookup → update → invalidate 顺序轮询，避免互锁死锁
void CacheSubsystem::walker_scheduler_thread() {
    while (true) {
        bool processed = false;
        
        // 1. 优先处理 lookup 请求
        CacheMessage req;
        if (walker_request_fifo.num_available() > 0) {
            req = walker_request_fifo.read();
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "walker_cache", "lookup", req, start);
            CacheMessage resp = execute_walker_request(req);
            record_task_completion("walker_cache", start, sc_time_stamp());
            trace_task_event("end", "walker_cache", "lookup", req, start, &resp);
            push_fifo(walker_response_fifo, resp);
            processed = true;
        }
        // 2. 处理 update 请求
        else if (walker_update_fifo.num_available() > 0) {
            req = walker_update_fifo.read();
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "walker_cache", "update", req, start);
            CacheMessage resp = execute_walker_update_request(req);
            record_task_completion("walker_cache", start, sc_time_stamp());
            trace_task_event("end", "walker_cache", "update", req, start, &resp);
            processed = true;
        }
        // 3. 处理 invalidate 请求
        else if (walker_invalidate_fifo.num_available() > 0) {
            req = walker_invalidate_fifo.read();
            const sc_time start = sc_time_stamp();
            trace_task_event("begin", "walker_cache", "invalidate", req, start);
            CacheMessage resp = execute_walker_invalidate_request(req);
            record_task_completion("walker_cache", start, sc_time_stamp());
            trace_task_event("end", "walker_cache", "invalidate", req, start, &resp);
            push_fifo(walker_invalidate_response_fifo, resp);
            processed = true;
        }
        
        // 如果没有处理任何请求，等待任意一个FIFO有数据
        if (!processed) {
            // 等待任意一个FIFO有数据（使用event通知）
            wait(walker_request_fifo.data_written_event() | 
                 walker_update_fifo.data_written_event() | 
                 walker_invalidate_fifo.data_written_event());
        }
    }
}

void CacheSubsystem::msi_worker_thread() {
    while (true) {
        CacheMessage req = msi_request_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "msipt_cache", "lookup", req, start);
        CacheMessage resp = execute_msi_request(req);
        record_task_completion("msipt_cache", start, sc_time_stamp());
        trace_task_event("end", "msipt_cache", "lookup", req, start, &resp);
        push_fifo(msi_response_fifo, resp);
    }
}

void CacheSubsystem::dc_update_worker_thread() {
    while (true) {
        CacheMessage req = dc_update_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "dc_cache", "update", req, start);
        CacheMessage resp = execute_dc_update_request(req);
        record_task_completion("dc_cache", start, sc_time_stamp());
        trace_task_event("end", "dc_cache", "update", req, start, &resp);
    }
}

void CacheSubsystem::pc_update_worker_thread() {
    while (true) {
        CacheMessage req = pc_update_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pc_cache", "update", req, start);
        CacheMessage resp = execute_pc_update_request(req);
        record_task_completion("pc_cache", start, sc_time_stamp());
        trace_task_event("end", "pc_cache", "update", req, start, &resp);
    }
}

void CacheSubsystem::pt_update_worker_thread() {
    while (true) {
        CacheMessage req = pt_update_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pt_cache", "update", req, start);
        CacheMessage resp = execute_pt_update_request(req);
        record_task_completion("pt_cache", start, sc_time_stamp());
        trace_task_event("end", "pt_cache", "update", req, start, &resp);
    }
}

void CacheSubsystem::msi_update_worker_thread() {
    while (true) {
        CacheMessage req = msi_update_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "msipt_cache", "update", req, start);
        CacheMessage resp = execute_msi_update_request(req);
        record_task_completion("msipt_cache", start, sc_time_stamp());
        trace_task_event("end", "msipt_cache", "update", req, start, &resp);
    }
}

void CacheSubsystem::dc_invalidate_worker_thread() {
    while (true) {
        CacheMessage req = dc_invalidate_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "dc_cache", "invalidate", req, start);
        CacheMessage resp = execute_dc_invalidate_request(req);
        record_task_completion("dc_cache", start, sc_time_stamp());
        trace_task_event("end", "dc_cache", "invalidate", req, start, &resp);
        push_fifo(dc_invalidate_response_fifo, resp);
    }
}

void CacheSubsystem::pc_invalidate_worker_thread() {
    while (true) {
        CacheMessage req = pc_invalidate_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pc_cache", "invalidate", req, start);
        CacheMessage resp = execute_pc_invalidate_request(req);
        record_task_completion("pc_cache", start, sc_time_stamp());
        trace_task_event("end", "pc_cache", "invalidate", req, start, &resp);
        push_fifo(pc_invalidate_response_fifo, resp);
    }
}

void CacheSubsystem::pt_invalidate_worker_thread() {
    while (true) {
        CacheMessage req = pt_invalidate_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "pt_cache", "invalidate", req, start);
        CacheMessage resp = execute_pt_invalidate_request(req);
        record_task_completion("pt_cache", start, sc_time_stamp());
        trace_task_event("end", "pt_cache", "invalidate", req, start, &resp);
        push_fifo(pt_invalidate_response_fifo, resp);
    }
}

void CacheSubsystem::msi_invalidate_worker_thread() {
    while (true) {
        CacheMessage req = msi_invalidate_fifo.read();
        const sc_time start = sc_time_stamp();
        trace_task_event("begin", "msipt_cache", "invalidate", req, start);
        CacheMessage resp = execute_msi_invalidate_request(req);
        record_task_completion("msipt_cache", start, sc_time_stamp());
        trace_task_event("end", "msipt_cache", "invalidate", req, start, &resp);
        push_fifo(msi_invalidate_response_fifo, resp);
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
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_RESPONSE;
    resp.task_id = req.task_id;
    resp.iova = req.iova;  // FIX: 复制IOVA到响应
    PTData data;
    resp.hit = pt_cache_->lookup_pt(req.gscid, req.pscid, req.iova, req.stage,
                                    req.pt_sv48,
                                    req.pt_gstage_x4,
                                    data, resp.latency);
    resp.gscid = req.gscid;
    resp.pscid = req.pscid;
    
    if (resp.hit) {
        resp.stage = pt_stage(data);
        resp.from_prefetch = pt_from_prefetch(data);
        resp.pt_data = data;
        
        // [NEW] 检查是否为占位CL HIT
        if (data.reserved.is_ph == 1 && pt_dedup_enabled_ && dedup_buffer_ != nullptr) {
            uint8_t is_req = data.reserved.is_req;
            
            // [NEW] Phase 1: 分支3 - 预取占位CL (is_req=0),首次有任务访问
            if (is_req == 0) {
                printf("[PT_CACHE_EXECUTE] task_id=%u -> HIT prefetch placeholder (is_req=0)\n",
                       req.task_id);
                fflush(stdout);
                
                // 分配新Buffer Entry
                uint8_t new_idx = dedup_buffer_->allocate_entry();
                if (new_idx != DEDUP_BUFFER_INVALID_IDX) {
                    // 填充新Entry
                    auto& entry = dedup_buffer_->entries[new_idx];
                    entry.gscid = req.gscid;
                    entry.pscid = req.pscid;
                    entry.iova = req.iova;
                    entry.stage = req.stage;
                    entry.sv48 = req.pt_sv48;
                    entry.gstage_x4 = req.pt_gstage_x4;
                    entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);
                    entry.next_index = DEDUP_BUFFER_INVALID_IDX;
                    
                    // [NOTE] 更新PT Cache: head=new, tail=new, is_req=1
                    // 这需要PT Cache支持update_placeholder接口
                    // 简化方案: 不更新PT Cache,仅在Buffer中标记
                    // flush时使用next_index遍历
                    
                    printf("[PT_CACHE_EXECUTE] task_id=%u -> Prefetch placeholder HIT, allocated new entry (idx=%u), set as new head\n",
                           req.task_id, new_idx);
                    fflush(stdout);
                    
                    // 返回响应
                    resp.dedup_head_index = new_idx;
                    resp.dedup_tail_index = new_idx;
                    resp.dedup_new_index = new_idx;
                } else {
                    // Buffer满，降级处理
                    printf("[PT_CACHE_EXECUTE] task_id=%u -> Prefetch placeholder HIT, but Buffer full\n",
                           req.task_id);
                    fflush(stdout);
                }
            }
            // 分支2 - 主任务占位CL (is_req=1),已有任务
            else {
                // 占位CL HIT：需要分配新Buffer Entry、填充task_ptr、挂接到链表
                uint8_t head_idx = data.reserved.head_index;
                uint8_t tail_idx = data.reserved.tail_index;
                
                // 分配新Entry
                uint8_t new_idx = dedup_buffer_->allocate_entry();
                if (new_idx != DEDUP_BUFFER_INVALID_IDX) {
                    // 填充新Entry
                    auto& new_entry = dedup_buffer_->entries[new_idx];
                    new_entry.gscid = req.gscid;
                    new_entry.pscid = req.pscid;
                    new_entry.iova = req.iova;
                    new_entry.stage = req.stage;
                    new_entry.sv48 = req.pt_sv48;
                    new_entry.gstage_x4 = req.pt_gstage_x4;
                    new_entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);  // [NEW] 类型转换
                    new_entry.next_index = DEDUP_BUFFER_INVALID_IDX;
                    
                    // [NEW] 挂接到链表尾部（在execute_pt_request中完成）
                    auto& tail_entry = dedup_buffer_->entries[tail_idx];
                    tail_entry.next_index = new_idx;
                    
                    // 更新占位CL的tail_index
                    // [NOTE] 这里需要更新PT Cache中的占位CL，但当前无法直接修改
                    // 简化方案：不更新tail_index，flush时使用next_index遍历
                    
                    printf("[PT_CACHE_EXECUTE] task_id=%u -> Placeholder HIT, allocated+linked (head=%u, tail=%u, new=%u)\n",
                           req.task_id, head_idx, tail_idx, new_idx);
                    fflush(stdout);
                    
                    // 返回响应（不包含Buffer信息，collector不关心）
                    resp.dedup_head_index = head_idx;
                    resp.dedup_tail_index = tail_idx;
                    resp.dedup_new_index = new_idx;
                } else {
                    // Buffer满，降级处理：返回HIT但不挂接，collector直接转发到PTW
                    printf("[PT_CACHE_EXECUTE] task_id=%u -> Placeholder HIT, but Buffer full (head=%u)\n",
                           req.task_id, head_idx);
                    fflush(stdout);
                }
            }
        }
    } else if (pt_dedup_enabled_ && dedup_buffer_ != nullptr) {
        // [NEW] MISS + 去重使能：自动插入占位CL（lookup+insert原子操作）
        uint64_t page_iova = req.iova & ~0xFFFULL;  // 4KB对齐
        
        // 步骤1: 分配Buffer Entry
        uint8_t head_idx = dedup_buffer_->allocate_entry();
        if (head_idx != DEDUP_BUFFER_INVALID_IDX) {
            // 步骤2: 填充Buffer Entry
            auto& entry = dedup_buffer_->entries[head_idx];
            entry.gscid = req.gscid;
            entry.pscid = req.pscid;
            entry.iova = req.iova;
            entry.stage = req.stage;
            entry.sv48 = req.pt_sv48;
            entry.gstage_x4 = req.pt_gstage_x4;
            entry.task_ptr = static_cast<iommu_task_t*>(req.task_ptr);  // [NEW] 类型转换
            entry.next_index = DEDUP_BUFFER_INVALID_IDX;
            
            // 步骤3: 插入占位CL到PT Cache
            sc_time insert_latency;
            bool insert_success = pt_cache_->insert_placeholder(
                req.gscid, req.pscid, page_iova,
                req.stage, req.pt_sv48, req.pt_gstage_x4,
                head_idx, head_idx,  // head_index = tail_index = head_idx
                true,  // is_req = 1（主任务）
                &insert_latency
            );
            
            if (insert_success) {
                // 步骤4: 返回HIT（占位CL已插入）
                resp.hit = true;
                resp.dedup_head_index = head_idx;  // 传递Buffer索引给collector
                resp.latency += insert_latency;
                
                // 填充resp.pt_data，标记为占位CL
                resp.pt_data.reserved.is_ph = 1;
                resp.pt_data.reserved.head_index = head_idx;
                resp.pt_data.reserved.tail_index = head_idx;
                resp.pt_data.reserved.is_req = 1;
                resp.pt_data.reserved.valid = 1;
                
                // [NEW] Phase 1: 步骤5 - 检查预取参数,插入D个预取占位CL
                if (req.prefetch_enabled && req.prefetch_depth > 0) {
                    uint32_t D = req.prefetch_depth;
                    printf("[PT_CACHE_EXECUTE] task_id=%u -> Prefetch ENABLED (D=%u), inserting %u prefetch placeholders\n",
                           req.task_id, D, D);
                    fflush(stdout);
                    
                    for (uint32_t d = 1; d <= D; d++) {
                        uint64_t prefetch_iova = page_iova + (d * 0x1000);  // 4KB步长
                        
                        sc_time prefetch_latency;
                        bool prefetch_success = pt_cache_->insert_placeholder(
                            req.gscid, req.pscid, prefetch_iova,
                            req.stage, req.pt_sv48, req.pt_gstage_x4,
                            0xFF, 0xFF,  // head/tail = 0xFF (无Buffer链表)
                            false,       // is_req = 0 (预取占位CL)
                            &prefetch_latency
                        );
                        
                        if (prefetch_success) {
                            printf("[PT_CACHE_EXECUTE]   -> Inserted prefetch placeholder at iova=0x%lx (is_req=0)\n",
                                   prefetch_iova);
                        } else {
                            printf("[PT_CACHE_EXECUTE]   -> Failed to insert prefetch placeholder at iova=0x%lx\n",
                                   prefetch_iova);
                        }
                    }
                }
                
                printf("[PT_CACHE_EXECUTE] task_id=%u -> MISS, inserted placeholder (head_idx=%u, iova=0x%lx)\n",
                       req.task_id, head_idx, page_iova);
                fflush(stdout);
            } else {
                // 插入失败（Cache满），释放Buffer Entry
                dedup_buffer_->free_entry(head_idx);
                printf("[PT_CACHE_EXECUTE] task_id=%u -> MISS, placeholder insert FAILED (Cache full)\n",
                       req.task_id);
                fflush(stdout);
            }
        } else {
            printf("[PT_CACHE_EXECUTE] task_id=%u -> MISS, Buffer full\n", req.task_id);
            fflush(stdout);
        }
    }
    
    return resp;
}

CacheMessage CacheSubsystem::execute_walker_request(const CacheMessage& req) {
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_RESPONSE;
    resp.task_id = req.task_id;
    resp.iova = req.iova;  // FIX: 复制IOVA到响应
    WalkerData data;
    uint8_t hit_level = 0;
    resp.hit = walker_cache_->lookup(req.gscid, req.pscid, req.iova,
                                     req.walker_addr_is_va,
                                     req.walker_from_two_stage,
                                     req.walker_sv48,
                                     req.walker_x4_mode,
                                     data, hit_level, resp.latency);
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

    auto result = walker_cache_->update(req.gscid, req.pscid, req.iova,
                                        req.walker_update_kind,
                                        ptwc1_data, ptwc2_data, ptwc3_data);
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_UPDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.latency = result.latency;
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

    // DC 失效后，关联 PT/Walker/MSIPT 失效通过 FIFO 传递
    for (auto& ctx : contexts) {
        enqueue_cascade_invalidations(ctx.gscid, ctx.pscid, req.device_id,
                                      true, ctx.pscid != 0, true);
    }

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
    for (auto& ctx : contexts) {
        enqueue_cascade_invalidations(0, ctx.pscid, req.device_id,
                                      false, true, false);
    }

    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = static_cast<uint32_t>(contexts.size());
    resp.latency = latency;
    return resp;
}

CacheMessage CacheSubsystem::execute_pt_invalidate_request(const CacheMessage& req) {
    sc_time latency = SC_ZERO_TIME;
    uint32_t affected = 0;
    if (req.cmd_type == InvalidCmdType::IOTINVAL_GVMA) {
        affected = pt_cache_->invalidate_gvma(req.gscid, req.iova,
                                              req.has_gscid, req.has_iova,
                                              req.invalidate_mode, &latency);
    } else {
        affected = pt_cache_->invalidate_vma(req.gscid, req.pscid, req.iova,
                                             req.has_gscid, req.has_pscid,
                                             req.has_iova, req.invalidate_mode,
                                             &latency);
    }
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = affected;
    resp.latency = latency;
    return resp;
}

CacheMessage CacheSubsystem::execute_walker_invalidate_request(const CacheMessage& req) {
    sc_time latency = SC_ZERO_TIME;
    uint32_t affected = 0;
    if (req.cmd_type == InvalidCmdType::IOTINVAL_GVMA) {
        affected = walker_cache_->invalidate_gvma(req.gscid, req.has_gscid,
                                                  req.invalidate_mode, &latency);
    } else {
        affected = walker_cache_->invalidate_vma(req.gscid, req.pscid, req.iova,
                                                 req.has_gscid, req.has_pscid,
                                                 req.has_iova,
                                                 req.invalidate_mode, &latency);
    }
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = affected;
    resp.latency = latency;
    return resp;
}

CacheMessage CacheSubsystem::execute_msi_invalidate_request(const CacheMessage& req) {
    sc_time latency = SC_ZERO_TIME;
    uint32_t affected = 0;
    if (req.invalidate_mode == CacheInvalidateMode::GLOBAL) {
        affected = msipt_cache_->invalidate_global(&latency);
    } else {
        affected = msipt_cache_->invalidate_by_device(req.device_id, &latency);
    }
    CacheMessage resp;
    resp.msg_type = CacheMsgType::CACHE_INVALIDATE_RESPONSE;
    resp.task_id = req.task_id;
    resp.affected_entries = affected;
    resp.latency = latency;
    return resp;
}

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

    if (msg.cmd_type == InvalidCmdType::GLOBAL_INVAL ||
        msg.invalidate_mode == CacheInvalidateMode::GLOBAL) {
        CacheMessage req = msg;
        req.invalidate_mode = CacheInvalidateMode::GLOBAL;

        req.msg_type = CacheMsgType::DC_INVALIDATE;
        accumulate(drive_invalidate_and_wait(dc_invalidate_fifo,
                                             dc_invalidate_response_fifo, req));
        req.msg_type = CacheMsgType::PC_INVALIDATE;
        accumulate(drive_invalidate_and_wait(pc_invalidate_fifo,
                                             pc_invalidate_response_fifo, req));
        req.msg_type = CacheMsgType::PT_INVALIDATE;
        accumulate(drive_invalidate_and_wait(pt_invalidate_fifo,
                                             pt_invalidate_response_fifo, req));
        req.msg_type = CacheMsgType::WALKER_INVALIDATE;
        accumulate(drive_invalidate_and_wait(walker_invalidate_fifo,
                                             walker_invalidate_response_fifo, req));
        req.msg_type = CacheMsgType::MSI_INVALIDATE;
        accumulate(drive_invalidate_and_wait(msi_invalidate_fifo,
                                             msi_invalidate_response_fifo, req));
        return resp;
    }

    switch (msg.cmd_type) {
        case InvalidCmdType::IODIR_INVAL_DDT: {
            CacheMessage req = msg;
            req.msg_type = CacheMsgType::DC_INVALIDATE;
            req.invalidate_mode = CacheInvalidateMode::PRECISE;
            uint32_t pt_responses = 0;
            uint32_t walker_responses = 0;
            uint32_t msi_responses = 0;
            auto drain_cascade_responses = [&]() {
                CacheMessage partial;
                while (pop_fifo(pt_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    pt_responses++;
                }
                while (pop_fifo(walker_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    walker_responses++;
                }
                while (pop_fifo(msi_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    msi_responses++;
                }
            };

            push_fifo(dc_invalidate_fifo, req);
            while (dc_invalidate_response_fifo.num_available() == 0) {
                drain_cascade_responses();
                wait(clock_period_);
            }
            CacheMessage dc_resp = dc_invalidate_response_fifo.read();
            accumulate(dc_resp);
            while (pt_responses < dc_resp.affected_entries ||
                   walker_responses < dc_resp.affected_entries ||
                   msi_responses < dc_resp.affected_entries) {
                drain_cascade_responses();
                if (pt_responses < dc_resp.affected_entries ||
                    walker_responses < dc_resp.affected_entries ||
                    msi_responses < dc_resp.affected_entries) {
                    wait(clock_period_);
                }
            }
            break;
        }
        case InvalidCmdType::IODIR_INVAL_PDT: {
            CacheMessage req = msg;
            req.msg_type = CacheMsgType::PC_INVALIDATE;
            req.invalidate_mode = msg.has_process_id ?
                CacheInvalidateMode::PRECISE : CacheInvalidateMode::SCAN;
            uint32_t pt_responses = 0;
            uint32_t walker_responses = 0;
            auto drain_cascade_responses = [&]() {
                CacheMessage partial;
                while (pop_fifo(pt_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    pt_responses++;
                }
                while (pop_fifo(walker_invalidate_response_fifo, partial)) {
                    accumulate(partial);
                    walker_responses++;
                }
            };

            push_fifo(pc_invalidate_fifo, req);
            while (pc_invalidate_response_fifo.num_available() == 0) {
                drain_cascade_responses();
                wait(clock_period_);
            }
            CacheMessage pc_resp = pc_invalidate_response_fifo.read();
            accumulate(pc_resp);
            while (pt_responses < pc_resp.affected_entries ||
                   walker_responses < pc_resp.affected_entries) {
                drain_cascade_responses();
                if (pt_responses < pc_resp.affected_entries ||
                    walker_responses < pc_resp.affected_entries) {
                    wait(clock_period_);
                }
            }
            break;
        }
        case InvalidCmdType::IOTINVAL_VMA: {
            const bool mode1 = msg.has_gscid && msg.has_pscid && msg.has_iova;
            const bool mode2 = !msg.has_gscid && msg.has_pscid && msg.has_iova;
            CacheMessage pt_req = msg;
            pt_req.msg_type = CacheMsgType::PT_INVALIDATE;
            pt_req.invalidate_mode = (mode1 || mode2) ?
                CacheInvalidateMode::PRECISE : CacheInvalidateMode::SCAN;
            accumulate(drive_invalidate_and_wait(pt_invalidate_fifo,
                                                 pt_invalidate_response_fifo,
                                                 pt_req));

            CacheMessage walker_req = msg;
            walker_req.msg_type = CacheMsgType::WALKER_INVALIDATE;
            walker_req.invalidate_mode = mode1 ?
                CacheInvalidateMode::PRECISE : CacheInvalidateMode::SCAN;
            accumulate(drive_invalidate_and_wait(walker_invalidate_fifo,
                                                 walker_invalidate_response_fifo,
                                                 walker_req));
            break;
        }
        case InvalidCmdType::IOTINVAL_GVMA: {
            CacheMessage pt_req = msg;
            pt_req.msg_type = CacheMsgType::PT_INVALIDATE;
            pt_req.invalidate_mode = CacheInvalidateMode::SCAN;
            accumulate(drive_invalidate_and_wait(pt_invalidate_fifo,
                                                 pt_invalidate_response_fifo,
                                                 pt_req));

            CacheMessage walker_req = msg;
            walker_req.msg_type = CacheMsgType::WALKER_INVALIDATE;
            walker_req.invalidate_mode = CacheInvalidateMode::SCAN;
            accumulate(drive_invalidate_and_wait(walker_invalidate_fifo,
                                                 walker_invalidate_response_fifo,
                                                 walker_req));
            break;
        }
        default:
            break;
    }
    return resp;
}

void CacheSubsystem::enqueue_cascade_invalidations(gscid_t gscid, pscid_t pscid,
                                                   device_id_t device_id,
                                                   bool has_gscid,
                                                   bool has_pscid,
                                                   bool include_msi) {
    // 1. 关联 PT Cache 失效: 按 gscid (+ pscid) 失效
    CacheMessage pt_cascade_msg;
    pt_cascade_msg.msg_type = CacheMsgType::PT_INVALIDATE;
    pt_cascade_msg.cmd_type = InvalidCmdType::IOTINVAL_VMA;
    pt_cascade_msg.gscid = gscid;
    pt_cascade_msg.pscid = pscid;
    pt_cascade_msg.invalidate_mode = CacheInvalidateMode::SCAN;
    pt_cascade_msg.has_gscid = has_gscid;
    pt_cascade_msg.has_pscid = has_pscid;
    pt_cascade_msg.has_iova = false;
    push_fifo(pt_invalidate_fifo, pt_cascade_msg);

    // 2. 关联 Walker Cache 失效: 按 gscid (+ pscid) 失效
    CacheMessage walker_cascade_msg;
    walker_cascade_msg.msg_type = CacheMsgType::WALKER_INVALIDATE;
    walker_cascade_msg.cmd_type = InvalidCmdType::IOTINVAL_VMA;
    walker_cascade_msg.gscid = gscid;
    walker_cascade_msg.pscid = pscid;
    walker_cascade_msg.invalidate_mode = CacheInvalidateMode::SCAN;
    walker_cascade_msg.has_gscid = has_gscid;
    walker_cascade_msg.has_pscid = has_pscid;
    walker_cascade_msg.has_iova = false;
    push_fifo(walker_invalidate_fifo, walker_cascade_msg);

    // 3. 关联 MSIPT Cache 失效: 按 device_id 失效
    if (include_msi) {
        CacheMessage msi_cascade_msg;
        msi_cascade_msg.msg_type = CacheMsgType::MSI_INVALIDATE;
        msi_cascade_msg.device_id = device_id;
        msi_cascade_msg.invalidate_mode = CacheInvalidateMode::SCAN;
        push_fifo(msi_invalidate_fifo, msi_cascade_msg);
    }
}

} // namespace iommu
