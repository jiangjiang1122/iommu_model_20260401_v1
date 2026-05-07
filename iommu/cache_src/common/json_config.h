#ifndef IOMMU_JSON_CONFIG_H
#define IOMMU_JSON_CONFIG_H

#include "common/types.h"
#include <string>

namespace iommu {

// 从JSON文件加载全局配置
GlobalConfig load_config(const std::string& json_path);

// 从JSON字符串解析配置
GlobalConfig parse_config(const std::string& json_str);

} // namespace iommu

#endif // IOMMU_JSON_CONFIG_H
