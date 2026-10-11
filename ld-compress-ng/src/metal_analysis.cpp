#include "metal_analysis.h"

#include <stdexcept>

namespace ldcompress::metal_detail {

class MetalMonoAnalysisSession::Impl final {};

MetalMonoAnalysisSession::MetalMonoAnalysisSession(
    std::optional<std::size_t> requested_device_index)
{
    (void)requested_device_index;
    throw std::runtime_error("Metal support was not built");
}

MetalMonoAnalysisSession::~MetalMonoAnalysisSession() = default;

opencl_detail::OpenClMonoFixedConstantAnalysisResult
MetalMonoAnalysisSession::run_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    (void)samples;
    (void)plan;
    (void)max_rice_partition_order;
    throw std::runtime_error("Metal support was not built");
}

opencl_detail::OpenClMonoBestMethodResult
MetalMonoAnalysisSession::run_fixed_constant_best_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order,
    MetalGpuTimingStats* gpu_timings)
{
    (void)samples;
    (void)plan;
    (void)max_rice_partition_order;
    (void)gpu_timings;
    throw std::runtime_error("Metal support was not built");
}

opencl_detail::OpenClMonoFixedConstantAnalysisResult
MetalMonoAnalysisSession::run_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    (void)samples;
    (void)plan;
    (void)max_rice_partition_order;
    throw std::runtime_error("Metal support was not built");
}

opencl_detail::OpenClMonoFixedConstantAnalysisResult
MetalMonoAnalysisSession::run_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    (void)samples;
    (void)plan;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    throw std::runtime_error("Metal support was not built");
}

opencl_detail::OpenClMonoBestMethodResult
MetalMonoAnalysisSession::run_generated_best_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    MetalGpuTimingStats* gpu_timings)
{
    (void)samples;
    (void)plan;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    (void)gpu_timings;
    throw std::runtime_error("Metal support was not built");
}

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_metal_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)max_rice_partition_order;
    throw std::runtime_error("Metal support was not built");
}

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_metal_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)max_rice_partition_order;
    throw std::runtime_error("Metal support was not built");
}

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_metal_mono_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    throw std::runtime_error("Metal support was not built");
}

}  // namespace ldcompress::metal_detail
