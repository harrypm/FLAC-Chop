#pragma once

#include "opencl_analysis.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ldcompress::metal_detail {

struct MetalGpuTimingStats {
    std::uint64_t batches = 0;
    std::uint64_t upload_ns = 0;
    std::uint64_t generated_total_ns = 0;
    std::uint64_t wasted_bits_ns = 0;
    std::uint64_t generated_autocorrelation_ns = 0;
    std::uint64_t generated_lpc_ns = 0;
    std::uint64_t generated_quantize_ns = 0;
    std::uint64_t fixed_order_guess_ns = 0;
    std::uint64_t exact_analysis_ns = 0;
    std::uint64_t choose_best_ns = 0;
    std::uint64_t readback_ns = 0;
};

class MetalMonoAnalysisSession final {
public:
    explicit MetalMonoAnalysisSession(
        std::optional<std::size_t> requested_device_index = std::nullopt);
    ~MetalMonoAnalysisSession();

    MetalMonoAnalysisSession(const MetalMonoAnalysisSession&) = delete;
    MetalMonoAnalysisSession& operator=(const MetalMonoAnalysisSession&) = delete;

    opencl_detail::OpenClMonoFixedConstantAnalysisResult run_fixed_constant_analysis(
        const std::vector<std::int32_t>& samples,
        const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
        unsigned max_rice_partition_order = 5);

    opencl_detail::OpenClMonoBestMethodResult run_fixed_constant_best_analysis(
        const std::vector<std::int32_t>& samples,
        const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
        unsigned max_rice_partition_order = 5,
        MetalGpuTimingStats* gpu_timings = nullptr);

    opencl_detail::OpenClMonoFixedConstantAnalysisResult run_lpc_analysis(
        const std::vector<std::int32_t>& samples,
        const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
        unsigned max_rice_partition_order = 5);

    opencl_detail::OpenClMonoFixedConstantAnalysisResult run_generated_analysis(
        const std::vector<std::int32_t>& samples,
        const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
        unsigned lpc_coefficient_precision = 12,
        unsigned max_rice_partition_order = 5);

    opencl_detail::OpenClMonoBestMethodResult run_generated_best_analysis(
        const std::vector<std::int32_t>& samples,
        const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
        unsigned lpc_coefficient_precision = 12,
        unsigned max_rice_partition_order = 5,
        MetalGpuTimingStats* gpu_timings = nullptr);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_metal_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned max_rice_partition_order = 5);

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_metal_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned max_rice_partition_order = 5);

opencl_detail::OpenClMonoFixedConstantAnalysisResult run_metal_mono_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const opencl_detail::OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned lpc_coefficient_precision = 12,
    unsigned max_rice_partition_order = 5);

}  // namespace ldcompress::metal_detail
