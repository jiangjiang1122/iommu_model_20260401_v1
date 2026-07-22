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
//   - 无 LRU/替换算法, 命中时不更新替换信息
//   - 自定义替换策略(见 insert):
//       1) 有 V=0 的 way -> 直接写入
//       2) 否则有 V=1 & is_req=0(预取占位) 的 way -> 随机淘汰其一并写入
//       3) 否则(全部 is_req=1 受保护) -> 插入失败(调用方降级直接转发 PTW)
// ============================================================
class DedupCache {
public:
    DedupCache(uint32_t num_sets, uint32_t num_ways);

    // 查询: 命中返回可修改的 line 指针, 未命中返回 nullptr
    // 匹配键: gscid + pscid + iova(4KB 页对齐)
    DedupCacheLine* lookup(gscid_t gscid, pscid_t pscid, iova_t iova);

    // 插入占位(自定义替换策略)
    // 返回: true=插入成功, false=失败(该 set 全部为 is_req=1 受保护占位)
    bool insert(gscid_t gscid, pscid_t pscid, iova_t iova,
                uint16_t head_index, bool is_req);

    // 清除 line (置 V=0)
    void clear_line(DedupCacheLine* line);

    // 散列函数: 由 gscid/pscid/(iova>>12) 计算 set index
    uint32_t hash_function(gscid_t gscid, pscid_t pscid, iova_t iova) const;

    uint32_t num_sets() const { return num_sets_; }
    uint32_t num_ways() const { return num_ways_; }

private:
    static iova_t align_iova_4k(iova_t iova) {
        return iova & ~static_cast<iova_t>(0xFFF);
    }

    uint32_t num_sets_;
    uint32_t num_ways_;
    std::vector<std::vector<DedupCacheLine>> cache_array_;  // [set][way]
};

} // namespace iommu

#endif // IOMMU_DEDUP_CACHE_H
