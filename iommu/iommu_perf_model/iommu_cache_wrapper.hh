// Wrapper header to include cache types after IOMMU types
// This avoids namespace conflicts between global and iommu:: types

// First, include all IOMMU types (global namespace)
#include "iommu_task.hh"

// Then include cache types (iommu namespace)
#include "common/types.h"
