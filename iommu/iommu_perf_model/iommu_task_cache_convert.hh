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

// Convert CacheMessage response back to iommu_task_t
void dc_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task);
void pc_response_to_task(iommu::CacheMessage& resp, iommu_task_t* task);

#endif // IOMMU_TASK_CACHE_CONVERT_HH
