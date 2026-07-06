// IOMMU Performance Model - Task to CacheMessage Conversion Header
#ifndef IOMMU_TASK_CACHE_CONVERT_HH
#define IOMMU_TASK_CACHE_CONVERT_HH

#include "iommu_task.hh"
#include "common/types.h"

// Convert iommu_task_t to CacheMessage for DC/PC lookup requests
iommu::CacheMessage task_to_dc_request(iommu_task_t* task);
iommu::CacheMessage task_to_pc_request(iommu_task_t* task);

// Convert iommu_task_t to CacheMessage for DC/PC update requests
iommu::CacheMessage task_to_dc_update(iommu_task_t* task);
iommu::CacheMessage task_to_pc_update(iommu_task_t* task);

// Convert iommu_task_t to CacheMessage for PT lookup request
iommu::CacheMessage task_to_pt_request(iommu_task_t* task);

// Convert PT CacheMessage response back to iommu_task_t
void pt_hit_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task);
void pt_miss_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task);

// Convert iommu_task_t to CacheMessage for PT update
iommu::CacheMessage task_to_pt_update(iommu_task_t* task);

// Convert CacheMessage response back to iommu_task_t
void dc_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task);
void pc_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task);

// Convert iommu_task_t to CacheMessage for Walker Cache lookup request
iommu::CacheMessage task_to_walker_request(iommu_task_t* task);

// Convert Walker Cache Message response back to iommu_task_t
void walker_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task);

// Convert iommu_task_t to CacheMessage for Walker Cache update
iommu::CacheMessage task_to_walker_update(iommu_task_t* task);

// [S2] S2 Walker Cache lookup request (G-stage explicit, GPA as key)
iommu::CacheMessage task_to_s2_walker_request(iommu_task_t* task, uint64_t gpa);

// [S2] S2 Walker Cache update request (G-stage explicit, GPA as key)
iommu::CacheMessage task_to_s2_walker_update(iommu_task_t* task, uint64_t gpa);

#endif // IOMMU_TASK_CACHE_CONVERT_HH
