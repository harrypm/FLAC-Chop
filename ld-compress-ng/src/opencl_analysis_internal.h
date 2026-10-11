#pragma once

#include "opencl_analysis.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ldcompress::opencl_detail::internal {

OpenClMonoBestMethodResult execute_opencl_best_reduction(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt);

OpenClMonoFixedConstantAnalysisResult execute_opencl_exact_task_batch(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order,
    bool allow_lpc);

OpenClMonoFixedConstantAnalysisResult execute_opencl_lpc_generation_batch(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order);

OpenClMonoFixedConstantAnalysisResult execute_opencl_mixed_generation_batch(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order);

}  // namespace ldcompress::opencl_detail::internal
