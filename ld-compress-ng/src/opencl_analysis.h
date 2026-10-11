#pragma once

#include "flac_native_writer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace ldcompress::opencl_detail {

constexpr std::int32_t kFlacClSubframeConstant = 0;
constexpr std::int32_t kFlacClSubframeVerbatim = 1;
constexpr std::int32_t kFlacClSubframeFixed = 8;
constexpr std::int32_t kFlacClSubframeLpc = 32;
constexpr std::size_t kFlacClMaxOrder = 32;
constexpr std::size_t kFlacClMaxRicePartitionOrder = 8;
constexpr std::size_t kFlacClMaxRicePartitionCount = 1U << kFlacClMaxRicePartitionOrder;

// ABI mirror of FlaLDF/CUETools.Codecs.FLACCL/flac.cl. Field names intentionally
// match FLACCL so host/device buffer dumps can be compared directly.
struct FlacClSubframeData {
    std::int32_t residualOrder = 0;
    std::int32_t samplesOffs = 0;
    std::int32_t shift = 0;
    std::int32_t cbits = 0;
    std::int32_t size = 0;
    std::int32_t type = 0;
    std::int32_t obits = 0;
    std::int32_t blocksize = 0;
    std::int32_t coding_method = 0;
    std::int32_t channel = 0;
    std::int32_t residualOffs = 0;
    std::int32_t wbits = 0;
    std::int32_t abits = 0;
    std::int32_t porder = 0;
    std::int32_t headerLen = 0;
    std::int32_t encodingOffset = 0;
};

struct FlacClSubframeTask {
    FlacClSubframeData data;
    std::array<std::int32_t, kFlacClMaxOrder> coefs {};
};

struct FlacClRiceParameterSet {
    std::array<std::uint32_t, kFlacClMaxRicePartitionCount> parameters {};
};

static_assert(std::is_standard_layout_v<FlacClSubframeData>);
static_assert(std::is_standard_layout_v<FlacClSubframeTask>);
static_assert(std::is_standard_layout_v<FlacClRiceParameterSet>);
static_assert(sizeof(FlacClSubframeData) == 64);
static_assert(sizeof(FlacClSubframeTask) == 192);
static_assert(sizeof(FlacClRiceParameterSet) == 1024);

struct OpenClMonoAnalysisTaskOptions {
    unsigned frame_samples = 4608;
    unsigned bits_per_sample = 16;
    unsigned max_lpc_order = 12;
    unsigned min_fixed_order = 0;
    unsigned max_fixed_order = 4;
    bool include_constant = true;
    bool use_gpu_fixed_order_guess = false;
    NativeAnalysisProfile analysis_profile = NativeAnalysisProfile::Exact;
};

struct OpenClMonoAnalysisTaskPlan {
    std::vector<FlacClSubframeTask> residual_tasks;
    std::vector<std::int32_t> selected_tasks;
    std::size_t residual_tasks_per_frame = 0;
    std::size_t estimate_tasks_per_frame = 0;
    NativeAnalysisProfile analysis_profile = NativeAnalysisProfile::Exact;
    unsigned max_lpc_order = 0;
    bool fixed_order_guess_on_gpu = false;
};

struct OpenClMonoBestMethodResult {
    std::vector<FlacClSubframeTask> best_tasks;
    std::vector<FlacClRiceParameterSet> best_rice_parameters;
    std::string device_name;
};

struct OpenClMonoFixedConstantAnalysisResult {
    std::vector<FlacClSubframeTask> analyzed_tasks;
    std::vector<FlacClSubframeTask> best_tasks;
    std::vector<FlacClRiceParameterSet> best_rice_parameters;
    std::string device_name;
};

struct OpenClGeneratedSetupTimings {
    std::uint64_t device_ns = 0;
    std::uint64_t context_ns = 0;
    std::uint64_t queue_ns = 0;
    std::uint64_t program_source_ns = 0;
    std::uint64_t program_build_ns = 0;
    std::uint64_t kernels_ns = 0;
};

struct OpenClTaskPlanTimings {
    std::uint64_t fixed_order_guess_ns = 0;
    std::uint64_t task_fill_ns = 0;
};

struct OpenClGeneratedAnalysisTimings {
    std::uint64_t batches = 0;
    std::uint64_t upload_ns = 0;
    std::uint64_t wasted_bits_ns = 0;
    std::uint64_t generated_autocorrelation_ns = 0;
    std::uint64_t generated_lpc_ns = 0;
    std::uint64_t generated_quantize_ns = 0;
    std::uint64_t fixed_order_guess_ns = 0;
    std::uint64_t exact_analysis_ns = 0;
    std::uint64_t choose_best_ns = 0;
    std::uint64_t readback_ns = 0;
};

std::size_t mono_analysis_tasks_per_frame(const OpenClMonoAnalysisTaskOptions& options);

OpenClMonoAnalysisTaskPlan build_mono_analysis_task_plan(
    std::size_t frame_count,
    const OpenClMonoAnalysisTaskOptions& options);

OpenClMonoAnalysisTaskPlan build_mono_analysis_task_plan_for_samples(
    const std::vector<std::int32_t>& samples,
    std::size_t frame_count,
    const OpenClMonoAnalysisTaskOptions& options,
    OpenClTaskPlanTimings* timings = nullptr);

void apply_mono_analysis_profile_to_plan(
    const std::vector<std::int32_t>& samples,
    OpenClMonoAnalysisTaskPlan& plan);

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index = std::nullopt,
    unsigned max_rice_partition_order = 5);

class OpenClMonoAnalysisSession final {
public:
    explicit OpenClMonoAnalysisSession(
        std::optional<std::size_t> requested_device_index = std::nullopt,
        OpenClGeneratedSetupTimings* setup_timings = nullptr);
    ~OpenClMonoAnalysisSession();

    OpenClMonoAnalysisSession(const OpenClMonoAnalysisSession&) = delete;
    OpenClMonoAnalysisSession& operator=(const OpenClMonoAnalysisSession&) = delete;

    OpenClMonoFixedConstantAnalysisResult run_generated_analysis(
        const std::vector<std::int32_t>& samples,
        const OpenClMonoAnalysisTaskPlan& plan,
        unsigned lpc_coefficient_precision = 12,
        unsigned max_rice_partition_order = 5,
        OpenClGeneratedAnalysisTimings* timings = nullptr);

    OpenClMonoBestMethodResult run_generated_best_analysis(
        const std::vector<std::int32_t>& samples,
        const OpenClMonoAnalysisTaskPlan& plan,
        unsigned lpc_coefficient_precision = 12,
        unsigned max_rice_partition_order = 5,
        OpenClGeneratedAnalysisTimings* timings = nullptr);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

ldcompress::FlacSubframeDecision flaccl_task_to_subframe_decision(
    const FlacClSubframeTask& task);

ldcompress::FlacSelectedSubframe flaccl_task_to_selected_subframe(
    const FlacClSubframeTask& task);

std::vector<unsigned> flaccl_task_to_selected_rice_parameters(
    const FlacClSubframeTask& task,
    const FlacClRiceParameterSet& rice_parameters);

}  // namespace ldcompress::opencl_detail
