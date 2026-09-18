#ifndef IOMMU_DEDUP_CACHE_H
#define IOMMU_DEDUP_CACHE_H

#include <vector>
#include <cstdint>

#include "common/types.h"

namespace iommu {

// ============================================================
// 去重 Cache line 数据结构
// 仅存放占位信息: V=1 即代表该 iova 存在占位(不再单设 is_ph)
//   - is_req=1: 主任务占位(有实际等待任务, head_index 指向 Buffer 链头)
//   - is_req=0: 预取占位(无等待任务, head_index=0xFFFF)
// ============================================================
struct DedupCacheLine {
    bool     valid      = false;   // V: 1=有效占位, 0=空闲
    gscid_t  gscid      = 0;
    pscid_t  pscid      = 0;
    iova_t   iova       = 0;       // 4KB 页对齐
    uint16_t head_index = 0xFFFF;  // Buffer 任务链头索引
    bool     is_req     = false;   // 1=主任务占位, 0=预取占位

    void clear() {
        valid = false;
        gscid = 0;
        pscid = 0;
        iova = 0;
        head_index = 0xFFFF;
        is_req = false;
    }
};

// ============================================================
// 去重 Cache 管理类
//
// 特点(与常规 Cache 不同):
//   - 无常规替换算法(LRU等), 命中时不更新任何替换信息
//   - 架构定义的条件替换策略(见 insert):
//       i.   按 Tag hash 找 set 并读数据
//       ii.  有 V=0 的 way -> 直接写入
//       iii. 有 V=1 & is_req=0(预取占位) 的 way -> 随机淘汰其一并写入
//       iv.  无空位置可写(全为 V=1 & is_req=1) = hash冲突 -> 插入失败,
//            调用方将地址翻译任务直转 Walk(PTW), 不记录在 Cache 和 Buffer 中
// ============================================================
class DedupCache {
public:
    DedupCache(uint32_t num_sets, uint32_t num_ways);

    // 查询: 命中返回可修改的 line 指针, 未命中返回 nullptr
    // 匹配键: gscid + pscid + iova(4KB 页对齐)
    DedupCacheLine* lookup(gscid_t gscid, pscid_t pscid, iova_t iova);

    // 插入占位(架构定义的条件替换: V=0直写 -> 淘汰预取占位 -> 失败=hash冲突)
    // 返回: true=插入成功, false=失败(set 内全为 V=1&is_req=1, 无空位置可写)
    bool insert(gscid_t gscid, pscid_t pscid, iova_t iova,
                uint16_t head_index, bool is_req);
    
    // [hash冲突判定] 目标 set 是否还有可写位置(V=0 空 way 或 V=1&is_req=0 可淘汰预取占位),
    //   无延时预检, 供调用方在分配 Buffer 前判定; false = hash冲突(步骤iv)
    bool set_has_writable_way(gscid_t gscid, pscid_t pscid, iova_t iova) const;

    // 清除 line (置 V=0)
    void clear_line(DedupCacheLine* line);

    // 散列函数: 由 gscid/pscid/(iova>>12) 计算 set index
    // [多RAM] num_rams>1 时: 低 log2(num_rams) 位选 RAM, 高位作 RAM 内索引,
    //          global_set = ram_id * sets_per_ram + set_in_ram (每RAM独占连续set区间)
    uint32_t hash_function(gscid_t gscid, pscid_t pscid, iova_t iova) const;

    // ============================================================
    // [多RAM] 多 RAM 方案接口 (参照 PT Cache 多RAM模式)
    //   - Hash 单元由 dedup_hash_process_thread 消耗 1cyc
    //   - RAM 原子段(get_set 2 + get_free_line 2 + write 1 = 5cyc)
    //     由对应 dedup_ram_worker_thread 消耗, 同RAM串行/跨RAM并发
    // ============================================================

    // 配置 RAM 分组(构造后调用一次): num_rams 必须为2的幂且整除 num_sets
    void configure_multi_ram(uint32_t num_rams);

    // 未掩码原始hash值(供 ram_id/set 拆分)
    uint32_t raw_hash(gscid_t gscid, pscid_t pscid, iova_t iova) const;

    // 计算任务所属 RAM 组号 (无延时)
    uint32_t compute_ram_id(gscid_t gscid, pscid_t pscid, iova_t iova) const;

    uint32_t num_rams() const { return num_rams_; }
    uint32_t num_sets() const { return num_sets_; }
    uint32_t num_ways() const { return num_ways_; }

private:
    static iova_t align_iova_4k(iova_t iova) {
        return iova & ~static_cast<iova_t>(0xFFF);
    }

    uint32_t num_sets_;
    uint32_t num_ways_;
    // [多RAM] RAM分组参数(configure_multi_ram初始化; 默认1=单RAM旧行为)
    uint32_t num_rams_ = 1;
    uint32_t log2_num_rams_ = 0;
    uint32_t sets_per_ram_ = 0;
    std::vector<std::vector<DedupCacheLine>> cache_array_;  // [set][way]
};

} // namespace iommu

#endif // IOMMU_DEDUP_CACHE_H
