#include "metal_backend.h"

#include "accelerated_native_backend.h"
#include "compressor.h"
#include "metal_analysis.h"
#include "metal_devices.h"
#include "opencl_analysis.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kMetalBatchFrames = 512;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;
constexpr unsigned kMaxMetalFrameSamples = 4608;
constexpr unsigned kMaxMetalLpcOrder = 12;
constexpr unsigned kMinMetalLpcPrecision = 1;
constexpr unsigned kMaxMetalLpcPrecision = 15;
constexpr unsigned kMaxMetalRicePartitionOrder = 8;
using Clock = std::chrono::steady_clock;

void add_elapsed_ns(std::uint64_t& counter, Clock::time_point start)
{
    counter += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

void add_metal_timings(
    NativeCompressionStats& stats,
    const metal_detail::MetalGpuTimingStats& timings)
{
    stats.metal_timed_batches += timings.batches;
    stats.metal_upload_ns += timings.upload_ns;
    stats.metal_generated_total_ns += timings.generated_total_ns;
    stats.metal_wasted_bits_ns += timings.wasted_bits_ns;
    stats.metal_generated_autocorrelation_ns += timings.generated_autocorrelation_ns;
    stats.metal_generated_lpc_ns += timings.generated_lpc_ns;
    stats.metal_generated_quantize_ns += timings.generated_quantize_ns;
    stats.metal_fixed_order_guess_ns += timings.fixed_order_guess_ns;
    stats.metal_exact_analysis_ns += timings.exact_analysis_ns;
    stats.metal_choose_best_ns += timings.choose_best_ns;
    stats.metal_readback_ns += timings.readback_ns;
}

std::vector<unsigned> selected_rice_parameters_from_result(
    const opencl_detail::FlacClSubframeTask& task,
    const opencl_detail::FlacClRiceParameterSet& rice_parameters)
{
    if (task.data.type != opencl_detail::kFlacClSubframeFixed &&
        task.data.type != opencl_detail::kFlacClSubframeLpc) {
        return {};
    }
    if (task.data.porder < 0 ||
        task.data.porder >
            static_cast<std::int32_t>(opencl_detail::kFlacClMaxRicePartitionOrder)) {
        throw std::runtime_error("Metal selected task has invalid Rice partition order");
    }

    const auto partition_count =
        std::size_t {1} << static_cast<unsigned>(task.data.porder);
    std::vector<unsigned> selected;
    selected.reserve(partition_count);
    for (std::size_t i = 0; i < partition_count; ++i) {
        selected.push_back(static_cast<unsigned>(rice_parameters.parameters.at(i)));
    }
    return selected;
}

opencl_detail::OpenClMonoAnalysisTaskPlan build_metal_task_plan(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    NativeAnalysisProfile analysis_profile)
{
    opencl_detail::OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = frame_samples;
    task_options.bits_per_sample = frame_info.bits_per_sample;
    task_options.max_lpc_order = frame_info.max_lpc_order;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;
    task_options.analysis_profile = analysis_profile;
    task_options.use_gpu_fixed_order_guess = true;

    return opencl_detail::build_mono_analysis_task_plan_for_samples(
        samples,
        samples.size() / frame_samples,
        task_options);
}

AcceleratedSelectedFrameAnalysis analyze_metal_selected_frames(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    metal_detail::MetalMonoAnalysisSession& session,
    NativeAnalysisProfile analysis_profile,
    NativeCompressionStats* stats)
{
    if (frame_samples == 0) {
        throw std::runtime_error("Metal frame analysis frame_samples must be positive");
    }
    if (samples.empty()) {
        throw std::runtime_error("Metal frame analysis requires at least one frame");
    }
    if ((samples.size() % frame_samples) != 0) {
        throw std::runtime_error("Metal frame analysis samples are not frame-aligned");
    }

    const auto plan_started = Clock::now();
    auto plan = build_metal_task_plan(samples, frame_info, frame_samples, analysis_profile);
    if (stats != nullptr) {
        add_elapsed_ns(stats->accelerated_task_plan_ns, plan_started);
    }

    metal_detail::MetalGpuTimingStats metal_timings;
    const auto analysis_started = Clock::now();
    auto result = frame_info.max_lpc_order == 0
        ? session.run_fixed_constant_best_analysis(
            samples,
            plan,
            frame_info.max_rice_partition_order,
            stats == nullptr ? nullptr : &metal_timings)
        : session.run_generated_best_analysis(
            samples,
            plan,
            frame_info.lpc_coefficient_precision,
            frame_info.max_rice_partition_order,
            stats == nullptr ? nullptr : &metal_timings);
    if (stats != nullptr) {
        add_elapsed_ns(stats->accelerated_exact_analysis_ns, analysis_started);
        add_metal_timings(*stats, metal_timings);
    }

    AcceleratedSelectedFrameAnalysis selected;
    if (!result.best_rice_parameters.empty() &&
        result.best_rice_parameters.size() != result.best_tasks.size()) {
        throw std::runtime_error("Metal Rice parameter result count did not match best task count");
    }
    selected.decisions.reserve(result.best_tasks.size());
    selected.selected_subframes.reserve(result.best_tasks.size());
    for (std::size_t i = 0; i < result.best_tasks.size(); ++i) {
        const auto& task = result.best_tasks[i];
        selected.decisions.push_back(opencl_detail::flaccl_task_to_subframe_decision(task));
        auto selected_subframe = opencl_detail::flaccl_task_to_selected_subframe(task);
        if (!result.best_rice_parameters.empty()) {
            selected_subframe.rice_parameters =
                selected_rice_parameters_from_result(task, result.best_rice_parameters[i]);
        }
        selected.selected_subframes.push_back(std::move(selected_subframe));
    }
    return selected;
}

void validate_metal_options(const MetalCompressionOptions& options)
{
    if (options.container != FlacContainer::Native) {
        throw std::runtime_error("metal backend writes native FLAC only");
    }
    if (options.frame_samples < kMinimumStreamInfoBlockSize ||
        options.frame_samples > kMaxMetalFrameSamples) {
        throw std::runtime_error("Metal FLAC frame sample count must be 16..4608");
    }
    if (options.max_lpc_order > kMaxMetalLpcOrder) {
        throw std::runtime_error("Metal FLAC max LPC order must be 0..12");
    }
    if (options.lpc_precision < kMinMetalLpcPrecision ||
        options.lpc_precision > kMaxMetalLpcPrecision) {
        throw std::runtime_error("Metal FLAC LPC coefficient precision must be 1..15");
    }
    if (options.max_rice_partition_order > kMaxMetalRicePartitionOrder) {
        throw std::runtime_error("Metal FLAC max Rice partition order must be 0..8");
    }
}

void validate_metal_session_device(
    const MetalCompressionOptions& options,
    std::size_t session_device_index)
{
    if (options.device_index.has_value() && *options.device_index != session_device_index) {
        throw std::runtime_error(
            "Metal compression session device does not match requested device");
    }
}

ConversionStats compress_lds_to_metal_native_flac_with_session(
    std::istream& lds_input,
    const std::string& output_path,
    const MetalCompressionOptions& options,
    metal_detail::MetalMonoAnalysisSession& session,
    Clock::time_point total_started)
{
    const AcceleratedNativeCompressionOptions accelerated_options {
        .backend_label = "Metal",
        .container = options.container,
        .sample_rate = options.sample_rate,
        .thread_count = options.thread_count,
        .frame_samples = options.frame_samples,
        .max_lpc_order = options.max_lpc_order,
        .lpc_precision = options.lpc_precision,
        .max_rice_partition_order = options.max_rice_partition_order,
        .batch_frames = kMetalBatchFrames,
        .native_stats = options.native_stats,
        .progress_callback = options.progress_callback,
    };

    auto stats = compress_lds_to_accelerated_native_flac(
        lds_input,
        output_path,
        accelerated_options,
        [&session, analysis_profile = options.analysis_profile,
            native_stats = options.native_stats](
            const std::vector<std::int32_t>& samples,
            const FlacFrameInfo& frame_info,
            unsigned frame_samples) {
            return analyze_metal_selected_frames(
                samples, frame_info, frame_samples, session, analysis_profile, native_stats);
        });
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_total_ns, total_started);
    }
    return stats;
}

}  // namespace

MetalCompressionSession::MetalCompressionSession(
    std::optional<std::size_t> requested_device_index)
{
    const auto selected_device = select_metal_device(requested_device_index);
    device_index_ = selected_device.index;
    analysis_session_ =
        std::make_unique<metal_detail::MetalMonoAnalysisSession>(
            std::optional<std::size_t>(device_index_));
}

MetalCompressionSession::~MetalCompressionSession() = default;

ConversionStats MetalCompressionSession::compress_lds_to_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const MetalCompressionOptions& options)
{
    const auto total_started = Clock::now();
    validate_metal_options(options);
    validate_metal_session_device(options, device_index_);
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_setup_ns, total_started);
    }
    return compress_lds_to_metal_native_flac_with_session(
        lds_input, output_path, options, *analysis_session_, total_started);
}

ConversionStats compress_lds_to_metal_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const MetalCompressionOptions& options)
{
    const auto total_started = Clock::now();
    validate_metal_options(options);
    const auto selected_device = select_metal_device(options.device_index);
    metal_detail::MetalMonoAnalysisSession session(
        std::optional<std::size_t>(selected_device.index));
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_setup_ns, total_started);
    }
    MetalCompressionOptions session_options = options;
    session_options.device_index = selected_device.index;
    return compress_lds_to_metal_native_flac_with_session(
        lds_input, output_path, session_options, session, total_started);
}

}  // namespace ldcompress
