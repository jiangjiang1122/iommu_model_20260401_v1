#include "common/json_config.h"
#include "common/mini_json.h"
#include <fstream>
#include <stdexcept>

using json = mini_json::Value;

namespace iommu {

static void parse_cache_timing_config(const json& j, CacheConfig& cfg) {
    if (j.contains("arbiter_latency_cycles"))    cfg.arbiter_latency_cycles = static_cast<uint32_t>(j["arbiter_latency_cycles"]);
    if (j.contains("hash_latency_cycles"))       cfg.hash_latency_cycles = static_cast<uint32_t>(j["hash_latency_cycles"]);
    if (j.contains("read_set_latency_cycles"))   cfg.read_set_latency_cycles = static_cast<uint32_t>(j["read_set_latency_cycles"]);
    if (j.contains("compare_latency_cycles"))    cfg.compare_latency_cycles = static_cast<uint32_t>(j["compare_latency_cycles"]);
    if (j.contains("update_way_select_latency_cycles")) cfg.update_way_select_latency_cycles = static_cast<uint32_t>(j["update_way_select_latency_cycles"]);
    if (j.contains("fill_compute_index_hit_cycles")) cfg.fill_compute_index_hit_cycles = static_cast<uint32_t>(j["fill_compute_index_hit_cycles"]);
    if (j.contains("fill_compute_index_invalid_cycles")) cfg.fill_compute_index_invalid_cycles = static_cast<uint32_t>(j["fill_compute_index_invalid_cycles"]);
    if (j.contains("fill_compute_index_replacement_cycles")) cfg.fill_compute_index_replacement_cycles = static_cast<uint32_t>(j["fill_compute_index_replacement_cycles"]);
    if (j.contains("write_way_latency_cycles"))  cfg.write_way_latency_cycles = static_cast<uint32_t>(j["write_way_latency_cycles"]);
    if (j.contains("invalidation_compare_per_way_cycles")) cfg.invalidation_compare_per_way_cycles = static_cast<uint32_t>(j["invalidation_compare_per_way_cycles"]);
}

static CacheConfig parse_cache_config(const json& j, const CacheConfig& defaults) {
    CacheConfig cfg = defaults;
    if (j.contains("num_sets"))                  cfg.num_sets = static_cast<uint32_t>(j["num_sets"]);
    if (j.contains("num_ways"))                  cfg.num_ways = static_cast<uint32_t>(j["num_ways"]);
    if (j.contains("replacement"))               cfg.replacement = static_cast<std::string>(j["replacement"]);
    if (j.contains("srrip_m_bits"))              cfg.srrip_m_bits = static_cast<uint32_t>(j["srrip_m_bits"]);
    if (j.contains("num_rams"))                  cfg.num_rams = static_cast<uint32_t>(j["num_rams"]);
    if (j.contains("ram_fifo_depth"))            cfg.ram_fifo_depth = static_cast<uint32_t>(j["ram_fifo_depth"]);
    if (j.contains("hash_mode"))                 cfg.hash_mode = static_cast<std::string>(j["hash_mode"]);
    parse_cache_timing_config(j, cfg);
    return cfg;
}

GlobalConfig parse_config(const std::string& json_str) {
    json j = mini_json::Parser::parse(json_str);
    GlobalConfig cfg;

    // Global
    if (j.contains("global")) {
        const auto& g = j["global"];
        if (g.contains("clock_period_ns")) cfg.clock_period_ns = static_cast<double>(g["clock_period_ns"]);
        if (g.contains("random_seed"))     cfg.random_seed = static_cast<uint32_t>(g["random_seed"]);
    }

    CacheConfig common_cache_def;
    if (j.contains("cache_timing")) {
        parse_cache_timing_config(j["cache_timing"], common_cache_def);
    }

    // DC Cache
    if (j.contains("dc_cache")) {
        CacheConfig dc_def = common_cache_def;
        dc_def.num_sets = 64; dc_def.num_ways = 4; dc_def.replacement = "plru";
        cfg.dc_cache = parse_cache_config(j["dc_cache"], dc_def);
    }

    // PC Cache
    if (j.contains("pc_cache")) {
        CacheConfig pc_def = common_cache_def;
        pc_def.num_sets = 64; pc_def.num_ways = 4; pc_def.replacement = "plru";
        cfg.pc_cache = parse_cache_config(j["pc_cache"], pc_def);
    }

    // MSIPT Cache
    if (j.contains("msipt_cache")) {
        CacheConfig msipt_def = common_cache_def;
        msipt_def.num_sets = 64; msipt_def.num_ways = 4; msipt_def.replacement = "plru";
        cfg.msipt_cache = parse_cache_config(j["msipt_cache"], msipt_def);
    }

    // PT Cache
    if (j.contains("pt_cache")) {
        CacheConfig pt_def = common_cache_def;
        pt_def.num_sets = 1024; pt_def.num_ways = 8; pt_def.replacement = "srrip";
        pt_def.srrip_m_bits = 2;
        cfg.pt_cache = parse_cache_config(j["pt_cache"], pt_def);
    }

    // [dedup多RAM] 去重Cache: 默认几何复用 pt_cache, num_rams=4, ram_fifo_depth=8
    {
        CacheConfig dedup_def = common_cache_def;
        dedup_def.num_sets = cfg.pt_cache.num_sets;
        dedup_def.num_ways = cfg.pt_cache.num_ways;
        dedup_def.num_rams = 4;
        dedup_def.ram_fifo_depth = 8;
        if (j.contains("dedup_cache")) {
            cfg.dedup_cache = parse_cache_config(j["dedup_cache"], dedup_def);
        } else {
            cfg.dedup_cache = dedup_def;
        }
    }

    // Walker Cache
    if (j.contains("walker_cache")) {
        const auto& wc = j["walker_cache"];
        if (wc.contains("base_sets")) cfg.walker_base_sets = static_cast<uint32_t>(wc["base_sets"]);

        CacheConfig wc1_def = common_cache_def;
        wc1_def.num_sets = cfg.walker_base_sets; wc1_def.num_ways = 1;
        wc1_def.replacement = "none";
        if (wc.contains("ptw_c1")) {
            cfg.walker_ptw_c1 = parse_cache_config(wc["ptw_c1"], wc1_def);
            cfg.walker_ptw_c1.num_sets = cfg.walker_base_sets;
        } else {
            cfg.walker_ptw_c1 = wc1_def;
        }

        CacheConfig wc2_def = common_cache_def;
        wc2_def.num_sets = cfg.walker_base_sets; wc2_def.num_ways = 2;
        wc2_def.replacement = "plru";
        if (wc.contains("ptw_c2")) {
            cfg.walker_ptw_c2 = parse_cache_config(wc["ptw_c2"], wc2_def);
            cfg.walker_ptw_c2.num_sets = cfg.walker_base_sets;
        } else {
            cfg.walker_ptw_c2 = wc2_def;
        }

        CacheConfig wc3_def = common_cache_def;
        wc3_def.num_sets = cfg.walker_base_sets; wc3_def.num_ways = 4;
        wc3_def.replacement = "plru";
        if (wc.contains("ptw_c3")) {
            cfg.walker_ptw_c3 = parse_cache_config(wc["ptw_c3"], wc3_def);
            cfg.walker_ptw_c3.num_sets = cfg.walker_base_sets;
        } else {
            cfg.walker_ptw_c3 = wc3_def;
        }
    }

    // [失效] 失效处理与延迟失效(LIB/VN)配置
    if (j.contains("invalidation")) {
        const auto& iv = j["invalidation"];
        if (iv.contains("enable"))           cfg.invalidation.enable = static_cast<bool>(iv["enable"]);
        if (iv.contains("lazy_enable"))      cfg.invalidation.lazy_enable = static_cast<bool>(iv["lazy_enable"]);
        if (iv.contains("lib_size"))         cfg.invalidation.lib_size = static_cast<uint32_t>(iv["lib_size"]);
        if (iv.contains("vn_bits"))          cfg.invalidation.vn_bits = static_cast<uint32_t>(iv["vn_bits"]);
        if (iv.contains("lib_match_cycles")) cfg.invalidation.lib_match_cycles = static_cast<uint32_t>(iv["lib_match_cycles"]);
    }

    // Statistics
    if (j.contains("statistics")) {
        const auto& st = j["statistics"];
        const bool has_output_file = st.contains("output_file");
        if (has_output_file)                         cfg.statistics.output_file = static_cast<std::string>(st["output_file"]);
        if (st.contains("unified_log_file")) {
            if (!has_output_file) {
                cfg.statistics.output_file = static_cast<std::string>(st["unified_log_file"]);
            }
        }
        if (st.contains("enable_latency_histogram")) cfg.statistics.enable_latency_histogram = static_cast<bool>(st["enable_latency_histogram"]);
        if (st.contains("enable_task_trace"))       cfg.statistics.enable_task_trace = static_cast<bool>(st["enable_task_trace"]);
        if (st.contains("task_trace_level"))        cfg.statistics.task_trace_level = static_cast<std::string>(st["task_trace_level"]);
        if (st.contains("histogram_bin_width_ns"))  cfg.statistics.histogram_bin_width_ns = static_cast<double>(st["histogram_bin_width_ns"]);
    }

    return cfg;
}

GlobalConfig load_config(const std::string& json_path) {
    std::ifstream ifs(json_path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open config file: " + json_path);
    }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    return parse_config(content);
}

} // namespace iommu
