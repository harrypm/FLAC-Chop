#pragma once

#include "vulkan_analysis.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ldcompress::vulkan_detail {

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_vulkan_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned max_rice_partition_order = 5);

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_vulkan_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned max_rice_partition_order = 5);

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_vulkan_mono_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned lpc_coefficient_precision = 12,
    unsigned max_rice_partition_order = 5);

}  // namespace ldcompress::vulkan_detail
