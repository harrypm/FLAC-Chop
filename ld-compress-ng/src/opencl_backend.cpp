#include "opencl_backend.h"

#include "accelerated_native_backend.h"
#include "compressor.h"
#include "opencl_analysis.h"
#include "opencl_devices.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ldcompress {
namespace {

constexpr std::size_t kOpenClBatchFrames = 2048;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;
constexpr unsigned kMaxOpenClFrameSamples = 4608;
constexpr unsigned kMaxOpenClLpcOrder = 12;
constexpr unsigned kMinOpenClLpcPrecision = 1;
constexpr unsigned kMaxOpenClLpcPrecision = 15;
constexpr unsigned kMaxOpenClRicePartitionOrder = 8;
using Clock = std::chrono::steady_clock;

void add_elapsed_ns(std::uint64_t& counter, Clock::time_point start)
{
    counter += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

void add_opencl_timings(
    NativeCompressionStats& stats,
    const opencl_detail::OpenClGeneratedAnalysisTimings& timings)
{
    stats.opencl_timed_batches += timings.batches;
    stats.opencl_upload_ns += timings.upload_ns;
    stats.opencl_wasted_bits_ns += timings.wasted_bits_ns;
    stats.opencl_generated_autocorrelation_ns += timings.generated_autocorrelation_ns;
    stats.opencl_generated_lpc_ns += timings.generated_lpc_ns;
    stats.opencl_generated_quantize_ns += timings.generated_quantize_ns;
    stats.opencl_fixed_order_guess_ns += timings.fixed_order_guess_ns;
    stats.opencl_exact_analysis_ns += timings.exact_analysis_ns;
    stats.opencl_choose_best_ns += timings.choose_best_ns;
    stats.opencl_readback_ns += timings.readback_ns;
}

void add_opencl_setup_timings(
    NativeCompressionStats& stats,
    const opencl_detail::OpenClGeneratedSetupTimings& timings)
{
    stats.opencl_setup_device_ns += timings.device_ns;
    stats.opencl_setup_context_ns += timings.context_ns;
    stats.opencl_setup_queue_ns += timings.queue_ns;
    stats.opencl_setup_program_source_ns += timings.program_source_ns;
    stats.opencl_setup_program_build_ns += timings.program_build_ns;
    stats.opencl_setup_kernels_ns += timings.kernels_ns;
}

void add_opencl_task_plan_timings(
    NativeCompressionStats& stats,
    const opencl_detail::OpenClTaskPlanTimings& timings)
{
    stats.opencl_task_plan_fixed_guess_ns += timings.fixed_order_guess_ns;
    stats.opencl_task_plan_fill_ns += timings.task_fill_ns;
}

AcceleratedSelectedFrameAnalysis analyze_opencl_selected_frames(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    std::optional<std::size_t> device_index,
    opencl_detail::OpenClMonoAnalysisSession* generated_session,
    NativeAnalysisProfile analysis_profile,
    NativeCompressionStats* stats)
{
    if (frame_info.max_lpc_order > 0) {
        if (generated_session == nullptr) {
            throw std::runtime_error("OpenCL generated analysis session was not initialized");
        }
        if (frame_samples == 0) {
            throw std::runtime_error("OpenCL generated frame analysis frame_samples must be positive");
        }
        if (samples.empty()) {
            throw std::runtime_error("OpenCL generated frame analysis requires at least one frame");
        }
        if ((samples.size() % frame_samples) != 0) {
            throw std::runtime_error("OpenCL generated frame analysis samples are not frame-aligned");
        }

        opencl_detail::OpenClMonoAnalysisTaskOptions task_options;
        task_options.frame_samples = frame_samples;
        task_options.bits_per_sample = frame_info.bits_per_sample;
        task_options.max_lpc_order = frame_info.max_lpc_order;
        task_options.min_fixed_order = 0;
        task_options.max_fixed_order = 4;
        task_options.include_constant = true;
        task_options.use_gpu_fixed_order_guess = true;
        task_options.analysis_profile = analysis_profile;

        const auto frame_count = samples.size() / frame_samples;
        const auto plan_started = Clock::now();
        opencl_detail::OpenClTaskPlanTimings plan_timings;
        auto plan = opencl_detail::build_mono_analysis_task_plan_for_samples(
            samples,
            frame_count,
            task_options,
            stats == nullptr ? nullptr : &plan_timings);
        if (stats != nullptr) {
            add_elapsed_ns(stats->accelerated_task_plan_ns, plan_started);
            add_opencl_task_plan_timings(*stats, plan_timings);
        }
        const auto analysis_started = Clock::now();
        opencl_detail::OpenClGeneratedAnalysisTimings opencl_timings;
        auto result = generated_session->run_generated_best_analysis(
            samples,
            plan,
            frame_info.lpc_coefficient_precision,
            frame_info.max_rice_partition_order,
            stats == nullptr ? nullptr : &opencl_timings);
        if (stats != nullptr) {
            add_elapsed_ns(stats->accelerated_exact_analysis_ns, analysis_started);
            add_opencl_timings(*stats, opencl_timings);
        }

        AcceleratedSelectedFrameAnalysis selected;
        if (!result.best_rice_parameters.empty() &&
            result.best_rice_parameters.size() != result.best_tasks.size()) {
            throw std::runtime_error("OpenCL Rice parameter result count did not match best task count");
        }
        selected.decisions.reserve(result.best_tasks.size());
        selected.selected_subframes.reserve(result.best_tasks.size());
        for (std::size_t i = 0; i < result.best_tasks.size(); ++i) {
            const auto& task = result.best_tasks[i];
            selected.decisions.push_back(opencl_detail::flaccl_task_to_subframe_decision(task));
            auto selected_subframe = opencl_detail::flaccl_task_to_selected_subframe(task);
            if (!result.best_rice_parameters.empty()) {
                selected_subframe.rice_parameters =
                    opencl_detail::flaccl_task_to_selected_rice_parameters(
                        task, result.best_rice_parameters[i]);
            }
            selected.selected_subframes.push_back(std::move(selected_subframe));
        }
        return selected;
    }

    opencl_detail::OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = frame_samples;
    task_options.bits_per_sample = frame_info.bits_per_sample;
    task_options.max_lpc_order = 0;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;
    task_options.analysis_profile = analysis_profile;

    const auto frame_count = samples.size() / frame_samples;
    const auto plan_started = Clock::now();
    opencl_detail::OpenClTaskPlanTimings plan_timings;
    auto plan = opencl_detail::build_mono_analysis_task_plan_for_samples(
        samples,
        frame_count,
        task_options,
        stats == nullptr ? nullptr : &plan_timings);
    if (stats != nullptr) {
        add_elapsed_ns(stats->accelerated_task_plan_ns, plan_started);
        add_opencl_task_plan_timings(*stats, plan_timings);
    }
    const auto analysis_started = Clock::now();
    auto result = opencl_detail::run_opencl_mono_fixed_constant_analysis(
        samples, plan, device_index, frame_info.max_rice_partition_order);
    if (stats != nullptr) {
        add_elapsed_ns(stats->accelerated_exact_analysis_ns, analysis_started);
    }

    AcceleratedSelectedFrameAnalysis selected;
    if (!result.best_rice_parameters.empty() &&
        result.best_rice_parameters.size() != result.best_tasks.size()) {
        throw std::runtime_error("OpenCL Rice parameter result count did not match best task count");
    }
    selected.decisions.reserve(result.best_tasks.size());
    selected.selected_subframes.reserve(result.best_tasks.size());
    for (std::size_t i = 0; i < result.best_tasks.size(); ++i) {
        const auto& task = result.best_tasks[i];
        selected.decisions.push_back(opencl_detail::flaccl_task_to_subframe_decision(task));
        auto selected_subframe = opencl_detail::flaccl_task_to_selected_subframe(task);
        if (!result.best_rice_parameters.empty()) {
            selected_subframe.rice_parameters =
                opencl_detail::flaccl_task_to_selected_rice_parameters(
                    task, result.best_rice_parameters[i]);
        }
        selected.selected_subframes.push_back(std::move(selected_subframe));
    }
    return selected;
}

void validate_opencl_options(const OpenClCompressionOptions& options)
{
    if (options.container != FlacContainer::Native) {
        throw std::runtime_error("opencl backend writes native FLAC only");
    }
    if (options.frame_samples < kMinimumStreamInfoBlockSize ||
        options.frame_samples > kMaxOpenClFrameSamples) {
        throw std::runtime_error("OpenCL FLAC frame sample count must be 16..4608");
    }
    if (options.max_lpc_order > kMaxOpenClLpcOrder) {
        throw std::runtime_error("OpenCL FLAC max LPC order must be 0..12");
    }
    if (options.lpc_precision < kMinOpenClLpcPrecision ||
        options.lpc_precision > kMaxOpenClLpcPrecision) {
        throw std::runtime_error("OpenCL FLAC LPC coefficient precision must be 1..15");
    }
    if (options.max_rice_partition_order > kMaxOpenClRicePartitionOrder) {
        throw std::runtime_error("OpenCL FLAC max Rice partition order must be 0..8");
    }
}

void validate_opencl_session_device(
    const OpenClCompressionOptions& options,
    std::size_t session_device_index)
{
    if (!options.device_index.has_value()) {
        return;
    }
    if (*options.device_index != session_device_index) {
        throw std::runtime_error("OpenCL compression session device does not match requested OpenCL device");
    }
}

ConversionStats compress_lds_to_opencl_native_flac_with_session(
    std::istream& lds_input,
    const std::string& output_path,
    const OpenClCompressionOptions& options,
    opencl_detail::OpenClMonoAnalysisSession* generated_session,
    Clock::time_point total_started)
{
    const auto device_index = options.device_index;

    const AcceleratedNativeCompressionOptions accelerated_options {
        .backend_label = "OpenCL",
        .container = options.container,
        .sample_rate = options.sample_rate,
        .thread_count = options.thread_count,
        .frame_samples = options.frame_samples,
        .max_lpc_order = options.max_lpc_order,
        .lpc_precision = options.lpc_precision,
        .max_rice_partition_order = options.max_rice_partition_order,
        .batch_frames = kOpenClBatchFrames,
        .native_stats = options.native_stats,
        .progress_callback = options.progress_callback,
    };

    auto stats = compress_lds_to_accelerated_native_flac(
        lds_input,
        output_path,
        accelerated_options,
        [device_index, generated_session,
            analysis_profile = options.analysis_profile,
            native_stats = options.native_stats](
            const std::vector<std::int32_t>& samples,
            const FlacFrameInfo& frame_info,
            unsigned frame_samples) {
            return analyze_opencl_selected_frames(
                samples, frame_info, frame_samples, device_index, generated_session,
                analysis_profile, native_stats);
        });
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_total_ns, total_started);
    }
    return stats;
}

}  // namespace

OpenClCompressionSession::OpenClCompressionSession(
    std::optional<std::size_t> requested_device_index)
{
    const auto selected_device = select_opencl_device(requested_device_index);
    device_index_ = selected_device.flat_index;
    analysis_session_ =
        std::make_unique<opencl_detail::OpenClMonoAnalysisSession>(
            std::optional<std::size_t>(device_index_));
}

OpenClCompressionSession::~OpenClCompressionSession() = default;

ConversionStats OpenClCompressionSession::compress_lds_to_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const OpenClCompressionOptions& options)
{
    const auto total_started = Clock::now();
    OpenClCompressionOptions session_options = options;
    if (!session_options.device_index.has_value()) {
        session_options.device_index = device_index_;
    }
    validate_opencl_options(session_options);
    validate_opencl_session_device(session_options, device_index_);
    if (session_options.max_lpc_order == 0) {
        (void)select_opencl_device(session_options.device_index);
    }
    if (session_options.native_stats != nullptr) {
        add_elapsed_ns(session_options.native_stats->accelerated_setup_ns, total_started);
    }
    return compress_lds_to_opencl_native_flac_with_session(
        lds_input, output_path, session_options, analysis_session_.get(), total_started);
}

ConversionStats compress_lds_to_opencl_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const OpenClCompressionOptions& options)
{
    const auto total_started = Clock::now();
    validate_opencl_options(options);
    const auto device_index = options.device_index;
    std::unique_ptr<opencl_detail::OpenClMonoAnalysisSession> generated_session;
    if (options.max_lpc_order > 0) {
        opencl_detail::OpenClGeneratedSetupTimings setup_timings;
        generated_session =
            std::make_unique<opencl_detail::OpenClMonoAnalysisSession>(
                device_index,
                options.native_stats == nullptr ? nullptr : &setup_timings);
        if (options.native_stats != nullptr) {
            add_opencl_setup_timings(*options.native_stats, setup_timings);
        }
    } else {
        (void)select_opencl_device(device_index);
    }
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_setup_ns, total_started);
    }

    return compress_lds_to_opencl_native_flac_with_session(
        lds_input, output_path, options, generated_session.get(), total_started);
}

}  // namespace ldcompress
