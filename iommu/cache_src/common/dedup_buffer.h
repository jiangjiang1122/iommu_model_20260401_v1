#ifndef IOMMU_DEDUP_BUFFER_H
#define IOMMU_DEDUP_BUFFER_H

// 包含基础类型定义
#include "types.h"
#include "iommu_perf_params.hh"

// Forward declaration for iommu_task_t (defined in global namespace)
struct iommu_task_t;

namespace iommu {

/**
 * @brief Dedup Buffer Entry结构 (V3.0)
 * 
 * 用于管理等待PTW完成的任务链表
 * - V3.0: tail_index从PT Cache cacheline移至Buffer entry（仅链头有效）
 * - 核心字段: valid, iova, next_index, tail_index, task_ptr
 */
struct DedupBufferEntry {
    uint8_t  valid = 0;                    // V: 1=有效占用，0=空闲
    uint8_t  reserved1[3] = {0};           // 对齐到4字节
    
    // Command（完整翻译任务）
    iommu::gscid_t   gscid = 0;
    iommu::pscid_t   pscid = 0;
    iommu::iova_t    iova = 0;             // 原始iova（含offset）
    iommu::TransStage stage = iommu::TransStage::STAGE1_AND_2;
    bool      sv48 = true;
    bool      gstage_x4 = false;
    bool      reserved2 = false;           // 对齐
    
    // 链表指针（扩大为uint16_t支持512 entries）
    uint16_t  next_index = 0xFFFF;         // 后继Buffer下标，无后继=0xFFFF
    uint16_t  tail_index = 0xFFFF;         // [V3.0] 任务链尾编号（仅链头有效，非链头=0xFFFF）
    
    // 任务指针
    iommu_task_t* task_ptr = nullptr;
    
    bool is_valid() const { return valid != 0; }
    
    void clear() {
        valid = 0;
        gscid = 0;
        pscid = 0;
        iova = 0;
        next_index = 0xFFFF;
        tail_index = 0xFFFF;
        task_ptr = nullptr;
    }
};

/**
 * @brief Dedup Buffer管理类
 * 
 * 管理256个Buffer Entry，支持：
 * - 顺序分配（0, 1, 2...）
 * - 链表挂接
 * - 释放Entry
 */
struct DedupBuffer {
    DedupBufferEntry entries[PT_DEDUP_BUFFER_SIZE];
    uint16_t         valid_count = 0;       // 当前有效Entry数量
    uint16_t         peak_valid_count = 0;  // [STAT] 峰值有效Entry数量
    sc_event         free_event;            // [P3/P5] Buffer释放事件（反压通知）
    
    /**
     * @brief 分配Entry（按顺序线性查找第一个空闲）
     * @return Entry索引（0~255），失败返回0xFF
     */
    uint16_t allocate_entry() {
        if (valid_count >= PT_DEDUP_BUFFER_SIZE) {
            std::cout << "[DEDUP_BUFFER] Buffer full! Cannot allocate entry." << std::endl;
            return DEDUP_BUFFER_INVALID_IDX;
        }
        
        // 从0开始线性查找第一个空闲Entry
        for (uint32_t i = 0; i < PT_DEDUP_BUFFER_SIZE; i++) {
            if (!entries[i].is_valid()) {
                entries[i].clear();
                entries[i].valid = 1;
                valid_count++;
                if (valid_count > peak_valid_count) peak_valid_count = valid_count;
                return static_cast<uint16_t>(i);  // 返回索引（0, 1, 2, ...顺序）
            }
        }
        
        std::cout << "[DEDUP_BUFFER] No free entry found (should not happen)!" << std::endl;
        return DEDUP_BUFFER_INVALID_IDX;
    }
    
    /**
     * @brief 释放Entry
     * @param idx Entry索引
     */
    void free_entry(uint16_t idx) {
        if (idx == DEDUP_BUFFER_INVALID_IDX || idx >= PT_DEDUP_BUFFER_SIZE) return;
        if (entries[idx].is_valid()) {
            entries[idx].clear();
            valid_count--;
            // [P5] 通知反压等待者：有Buffer Entry被释放
            free_event.notify(SC_ZERO_TIME);
        }
    }
    
    /**
     * @brief 检查Buffer是否已满
     */
    bool is_full() const { return valid_count >= PT_DEDUP_BUFFER_SIZE; }
    
    /**
     * @brief 获取当前有效Entry数量
     */
    uint16_t get_valid_count() const { return valid_count; }
    uint16_t get_peak_valid_count() const { return peak_valid_count; }
    
    /**
     * @brief 重置Buffer（清空所有Entry）
     */
    void reset() {
        for (uint32_t i = 0; i < PT_DEDUP_BUFFER_SIZE; i++) {
            entries[i].clear();
        }
        valid_count = 0;
    }
};

} // namespace iommu

#endif // IOMMU_DEDUP_BUFFER_H
