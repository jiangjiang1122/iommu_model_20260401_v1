#ifndef IOMMU_DEDUP_BUFFER_H
#define IOMMU_DEDUP_BUFFER_H

// 包含基础类型定义
#include "types.h"
#include "iommu_perf_params.hh"

// Forward declaration for iommu_task_t (defined in global namespace)
struct iommu_task_t;

namespace iommu {

/**
 * @brief Dedup Buffer Entry结构
 * 
 * 用于管理等待PTW完成的任务链表
 * - 简化版: tail_index和is_req已移至PT Cache的pt_reserved_t
 * - 核心字段: valid, iova, next_index, task_ptr
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
    
    // 链表指针
    uint8_t   next_index = 0xFF;           // 后继Buffer下标，无后继=0xFF
    
    // 任务指针
    iommu_task_t* task_ptr = nullptr;
    
    bool is_valid() const { return valid != 0; }
    
    void clear() {
        valid = 0;
        gscid = 0;
        pscid = 0;
        iova = 0;
        next_index = 0xFF;
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
    uint8_t          valid_count = 0;       // 当前有效Entry数量
    
    /**
     * @brief 分配Entry（按顺序线性查找第一个空闲）
     * @return Entry索引（0~255），失败返回0xFF
     */
    uint8_t allocate_entry() {
        if (valid_count >= PT_DEDUP_BUFFER_SIZE) {
            std::cout << "[DEDUP_BUFFER] Buffer full! Cannot allocate entry." << std::endl;
            return DEDUP_BUFFER_INVALID_IDX;
        }
        
        // 从0开始线性查找第一个空闲Entry
        for (uint16_t i = 0; i < PT_DEDUP_BUFFER_SIZE; i++) {
            if (!entries[i].is_valid()) {
                entries[i].clear();
                entries[i].valid = 1;
                valid_count++;
                return static_cast<uint8_t>(i);  // 返回索引（0, 1, 2, ...顺序）
            }
        }
        
        std::cout << "[DEDUP_BUFFER] No free entry found (should not happen)!" << std::endl;
        return DEDUP_BUFFER_INVALID_IDX;
    }
    
    /**
     * @brief 释放Entry
     * @param idx Entry索引
     */
    void free_entry(uint8_t idx) {
        if (idx == DEDUP_BUFFER_INVALID_IDX || idx >= PT_DEDUP_BUFFER_SIZE) return;
        if (entries[idx].is_valid()) {
            entries[idx].clear();
            valid_count--;
        }
    }
    
    /**
     * @brief 检查Buffer是否已满
     */
    bool is_full() const { return valid_count >= PT_DEDUP_BUFFER_SIZE; }
    
    /**
     * @brief 获取当前有效Entry数量
     */
    uint8_t get_valid_count() const { return valid_count; }
    
    /**
     * @brief 重置Buffer（清空所有Entry）
     */
    void reset() {
        for (uint16_t i = 0; i < PT_DEDUP_BUFFER_SIZE; i++) {
            entries[i].clear();
        }
        valid_count = 0;
    }
};

} // namespace iommu

#endif // IOMMU_DEDUP_BUFFER_H
