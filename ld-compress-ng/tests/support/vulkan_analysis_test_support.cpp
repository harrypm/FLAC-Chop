#include "vulkan_analysis_test_support.h"

namespace ldcompress::vulkan_detail {

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_vulkan_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    VulkanMonoExactAnalysisSession session(requested_device_index);
    return session.run_fixed_constant_analysis(samples, plan, max_rice_partition_order);
}

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_vulkan_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    VulkanMonoExactAnalysisSession session(requested_device_index);
    return session.run_lpc_analysis(samples, plan, max_rice_partition_order);
}

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_vulkan_mono_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    VulkanMonoExactAnalysisSession session(requested_device_index);
    return session.run_generated_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order);
}

}  // namespace ldcompress::vulkan_detail
