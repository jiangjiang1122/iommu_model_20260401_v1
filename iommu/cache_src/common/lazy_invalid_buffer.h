#ifndef IOMMU_LAZY_INVALID_BUFFER_H
#define IOMMU_LAZY_INVALID_BUFFER_H

#include "types.h"

#include <cstdint>

namespace iommu {

// ============================================================
// [失效] 基于版本号(VN)的延迟失效 buffer (LIB, lazy invalid buffer)
//
// 硬件语义: 小容量寄存器 CAM, 1~2 个时钟完成匹配并输出匹配结果/未命中结论。
// 用途: 仅指定 GSCID/PSCID 的扫表失效指令不立即扫表, 而是记录到 LIB 并递增
//       全局 VN; 实际失效在以下两个时机完成:
//         1) 缓存查询命中后旁路比对 (CL.VN < LIB.VN -> 命中数据已失效)
//         2) 全局 VN 回绕到 VN_MAX 时的批量扫表
//
// LIB Entry 字段(对应架构文档 图4-17):
//   V:1     条目有效位
//   C:2     扫表失效指令参数 0=仅GSCID, 1=仅PSCID, 2=GSCID&PSCID
//   TAG1:20 PSCID (无对应项时置占位符全0)
//   TAG2:16 GSCID (无对应项时置占位符全0)
//   VN:4    该笔扫表失效指令的版本号
//
// PT Cache 与 Walker Cache 共享同一个 LIB 与全局 VN: 同一条 PTE 失效指令
// 需要被二者同步响应。
// ============================================================

enum class LibMatchClass : uint8_t {
    GSCID_ONLY = 0,   // C=0
    PSCID_ONLY = 1,   // C=1
    GSCID_PSCID = 2   // C=2
};

struct LibEntry {
    bool          valid = false;
    LibMatchClass cls   = LibMatchClass::GSCID_ONLY;
    pscid_t       tag1  = 0;   // PSCID (占位符 0)
    gscid_t       tag2  = 0;   // GSCID (占位符 0)
    uint8_t       vn    = 0;
};

class LazyInvalidBuffer {
public:
    void configure(uint32_t lib_size, uint32_t vn_bits) {
        size_ = (lib_size == 0) ? 1u : lib_size;
        if (size_ > kMaxEntries) size_ = kMaxEntries;
        vn_max_ = (vn_bits >= 8) ? 255u : ((1u << vn_bits) - 1u);
        if (vn_max_ == 0) vn_max_ = 15u;
        clear();
    }

    uint32_t size() const { return size_; }
    uint32_t vn_max() const { return vn_max_; }
    uint8_t global_vn() const { return global_vn_; }
    uint32_t valid_count() const { return valid_count_; }
    // 热路径快速判定: LIB 为空时查询侧无需做任何旁路比对
    bool empty() const { return valid_count_ == 0; }

    void clear() {
        for (uint32_t i = 0; i < kMaxEntries; ++i) entries_[i] = LibEntry{};
        valid_count_ = 0;
        global_vn_ = 0;
    }

    // 记录一笔扫表失效指令。
    // 流程(架构文档 图4-18 左侧): VN=VN+1 -> 按 tag 更新/写入 LIB entry ->
    //   若 VN 达到 VN_MAX 则复位 VN=0 并要求调用方执行批量扫表。
    // 返回 true 表示需要批量扫表(VN 回绕); false 表示记录完成直接返回。
    // 出参 out_full: LIB 已满且无匹配 entry, 调用方需降级为立即扫表。
    bool record(LibMatchClass cls, gscid_t gscid, pscid_t pscid, bool& out_full) {
        out_full = false;
        const gscid_t tag2 = (cls == LibMatchClass::PSCID_ONLY) ? 0 : gscid;
        const pscid_t tag1 = (cls == LibMatchClass::GSCID_ONLY) ? 0 : pscid;

        global_vn_ = static_cast<uint8_t>(global_vn_ + 1);

        int hit = find_entry(cls, tag2, tag1);
        if (hit >= 0) {
            entries_[hit].vn = global_vn_;
        } else {
            int free_idx = -1;
            for (uint32_t i = 0; i < size_; ++i) {
                if (!entries_[i].valid) { free_idx = static_cast<int>(i); break; }
            }
            if (free_idx < 0) {
                // LIB 满: 由调用方降级为立即扫表(功能正确优先)。VN 已递增,
                // 回滚以保持 CL.VN/LIB.VN 的比较语义一致。
                global_vn_ = static_cast<uint8_t>(global_vn_ - 1);
                out_full = true;
                return false;
            }
            entries_[free_idx].valid = true;
            entries_[free_idx].cls = cls;
            entries_[free_idx].tag1 = tag1;
            entries_[free_idx].tag2 = tag2;
            entries_[free_idx].vn = global_vn_;
            valid_count_++;
        }

        if (global_vn_ >= vn_max_) {
            global_vn_ = 0;
            return true;   // 触发批量扫表
        }
        return false;
    }

    // CAM 匹配: 查找与 (gscid, pscid) 关联的 entry, 返回其 VN。
    // 多个 entry 命中时取最大 VN(最严格的失效版本)。
    bool match(gscid_t gscid, pscid_t pscid, uint8_t& out_vn) const {
        bool matched = false;
        uint8_t max_vn = 0;
        for (uint32_t i = 0; i < size_; ++i) {
            const LibEntry& e = entries_[i];
            if (!e.valid) continue;
            bool ok = false;
            switch (e.cls) {
                case LibMatchClass::GSCID_ONLY:
                    ok = (e.tag2 == gscid);
                    break;
                case LibMatchClass::PSCID_ONLY:
                    ok = (e.tag1 == pscid);
                    break;
                case LibMatchClass::GSCID_PSCID:
                    ok = (e.tag2 == gscid && e.tag1 == pscid);
                    break;
            }
            if (!ok) continue;
            matched = true;
            if (e.vn > max_vn) max_vn = e.vn;
        }
        out_vn = max_vn;
        return matched;
    }

    const LibEntry& entry(uint32_t idx) const { return entries_[idx]; }

private:
    static constexpr uint32_t kMaxEntries = 64;

    int find_entry(LibMatchClass cls, gscid_t tag2, pscid_t tag1) const {
        for (uint32_t i = 0; i < size_; ++i) {
            const LibEntry& e = entries_[i];
            if (e.valid && e.cls == cls && e.tag2 == tag2 && e.tag1 == tag1) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    LibEntry entries_[kMaxEntries];
    uint32_t size_ = 16;
    uint32_t vn_max_ = 15;
    uint32_t valid_count_ = 0;
    uint8_t  global_vn_ = 0;
};

} // namespace iommu

#endif // IOMMU_LAZY_INVALID_BUFFER_H
