#pragma once

#include "opencl_analysis.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ldcompress::opencl_detail {

struct OpenClMonoGeneratedFrameAnalysisResult {
    std::vector<FlacClSubframeTask> analyzed_tasks;
    std::vector<FlacClSubframeTask> best_tasks;
    std::vector<FlacClRiceParameterSet> best_rice_parameters;
    std::vector<ldcompress::FlacSubframeDecision> decisions;
    std::vector<ldcompress::FlacSelectedSubframe> selected_subframes;
    std::string device_name;
};

OpenClMonoBestMethodResult run_opencl_mono_best_method(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt);

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned max_rice_partition_order = 5);

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_lpc_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned lpc_coefficient_precision = 12,
    unsigned max_rice_partition_order = 5);

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned lpc_coefficient_precision = 12,
    unsigned max_rice_partition_order = 5);

OpenClMonoBestMethodResult run_opencl_mono_generated_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned lpc_coefficient_precision = 12,
    unsigned max_rice_partition_order = 5,
    OpenClGeneratedAnalysisTimings* timings = nullptr);

OpenClMonoGeneratedFrameAnalysisResult analyze_opencl_mono_generated_frames(
    const std::vector<std::int32_t>& samples,
    const ldcompress::FlacFrameInfo& frame_info,
    unsigned frame_samples,
    std::optional<std::size_t> requested_device_index = std::nullopt);

OpenClMonoFixedConstantAnalysisResult analyze_mono_fixed_constant_exact(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order = 5);

std::optional<FlacClSubframeTask> analyze_mono_lpc_exact_task(
    const std::vector<std::int32_t>& samples,
    std::size_t frame_index,
    const OpenClMonoAnalysisTaskOptions& options,
    unsigned lpc_order,
    unsigned lpc_coefficient_precision = 12,
    unsigned max_rice_partition_order = 5);

}  // namespace ldcompress::opencl_detail
