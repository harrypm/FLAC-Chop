#include "vulkan_backend.h"

#include "accelerated_native_backend.h"
#include "compressor.h"
#include "opencl_analysis.h"
#include "vulkan_analysis.h"
#include "vulkan_devices.h"

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

constexpr std::size_t kVulkanBatchFrames = 512;
constexpr unsigned kMinimumStreamInfoBlockSize = 16;
constexpr unsigned kMaxVulkanFrameSamples = 4608;
constexpr unsigned kMaxVulkanLpcOrder = 12;
constexpr unsigned kMinVulkanLpcPrecision = 1;
constexpr unsigned kMaxVulkanLpcPrecision = 15;
constexpr unsigned kMaxVulkanRicePartitionOrder = 8;
using Clock = std::chrono::steady_clock;

void add_elapsed_ns(std::uint64_t& counter, Clock::time_point start)
{
    counter += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

void add_vulkan_gpu_timings(
    NativeCompressionStats& stats,
    const vulkan_detail::VulkanGpuTimingStats& gpu_timings)
{
    stats.vulkan_gpu_timed_batches += gpu_timings.batches;
    stats.vulkan_gpu_total_ns += gpu_timings.total_ns;
    stats.vulkan_gpu_upload_ns += gpu_timings.upload_ns;
    stats.vulkan_gpu_prepare_ns += gpu_timings.prepare_ns;
    stats.vulkan_gpu_generated_autocorrelation_ns +=
        gpu_timings.generated_autocorrelation_ns;
    stats.vulkan_gpu_generated_lpc_ns += gpu_timings.generated_lpc_ns;
    stats.vulkan_gpu_generated_quantize_ns += gpu_timings.generated_quantize_ns;
    stats.vulkan_gpu_fixed_order_guess_ns += gpu_timings.fixed_order_guess_ns;
    stats.vulkan_gpu_exact_analysis_ns += gpu_timings.exact_analysis_ns;
    stats.vulkan_gpu_choose_best_ns += gpu_timings.choose_best_ns;
    stats.vulkan_gpu_readback_ns += gpu_timings.readback_ns;
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
        throw std::runtime_error("Vulkan selected task has invalid Rice partition order");
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

opencl_detail::OpenClMonoAnalysisTaskPlan build_vulkan_fixed_constant_task_plan(
    const std::vector<std::int32_t>& samples,
    std::size_t frame_count,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    NativeAnalysisProfile analysis_profile)
{
    opencl_detail::OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = frame_samples;
    task_options.bits_per_sample = frame_info.bits_per_sample;
    task_options.max_lpc_order = 0;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;
    task_options.analysis_profile = analysis_profile;
    task_options.use_gpu_fixed_order_guess = true;

    return opencl_detail::build_mono_analysis_task_plan_for_samples(
        samples, frame_count, task_options);
}

opencl_detail::OpenClMonoAnalysisTaskPlan build_vulkan_mixed_lpc_task_plan(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    NativeAnalysisProfile analysis_profile)
{
    const auto frame_count = samples.size() / frame_samples;
    if (frame_info.max_lpc_order == 0 || frame_samples < 256) {
        return build_vulkan_fixed_constant_task_plan(
            samples, frame_count, frame_info, frame_samples, analysis_profile);
    }

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
        samples, frame_count, task_options);
}

AcceleratedSelectedFrameAnalysis analyze_vulkan_selected_frames(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    vulkan_detail::VulkanMonoExactAnalysisSession& session,
    NativeAnalysisProfile analysis_profile,
    NativeCompressionStats* stats)
{
    const auto frame_count = samples.size() / frame_samples;
    const auto plan_started = Clock::now();
    auto plan = frame_info.max_lpc_order == 0
        ? build_vulkan_fixed_constant_task_plan(
            samples, frame_count, frame_info, frame_samples, analysis_profile)
        : build_vulkan_mixed_lpc_task_plan(
            samples, frame_info, frame_samples, analysis_profile);
    if (stats != nullptr) {
        add_elapsed_ns(stats->accelerated_task_plan_ns, plan_started);
    }

    vulkan_detail::VulkanGpuTimingStats gpu_timings;
    const auto analysis_started = Clock::now();
    auto result = frame_info.max_lpc_order == 0
        ? session.run_fixed_constant_best_analysis(
            samples,
            plan,
            frame_info.max_rice_partition_order,
            stats != nullptr ? &gpu_timings : nullptr)
        : session.run_generated_best_analysis(
            samples,
            plan,
            frame_info.lpc_coefficient_precision,
            frame_info.max_rice_partition_order,
            stats != nullptr ? &gpu_timings : nullptr);
    if (stats != nullptr) {
        add_elapsed_ns(stats->accelerated_exact_analysis_ns, analysis_started);
        add_vulkan_gpu_timings(*stats, gpu_timings);
    }

    AcceleratedSelectedFrameAnalysis selected;
    if (!result.best_rice_parameters.empty() &&
        result.best_rice_parameters.size() != result.best_tasks.size()) {
        throw std::runtime_error("Vulkan Rice parameter result count did not match best task count");
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

void validate_vulkan_options(const VulkanCompressionOptions& options)
{
    if (options.container != FlacContainer::Native) {
        throw std::runtime_error("vulkan backend writes native FLAC only");
    }
    if (options.frame_samples < kMinimumStreamInfoBlockSize ||
        options.frame_samples > kMaxVulkanFrameSamples) {
        throw std::runtime_error("Vulkan FLAC frame sample count must be 16..4608");
    }
    if (options.max_lpc_order > kMaxVulkanLpcOrder) {
        throw std::runtime_error("Vulkan FLAC max LPC order must be 0..12");
    }
    if (options.lpc_precision < kMinVulkanLpcPrecision ||
        options.lpc_precision > kMaxVulkanLpcPrecision) {
        throw std::runtime_error("Vulkan FLAC LPC coefficient precision must be 1..15");
    }
    if (options.max_rice_partition_order > kMaxVulkanRicePartitionOrder) {
        throw std::runtime_error("Vulkan FLAC max Rice partition order must be 0..8");
    }
}

bool vulkan_analysis_device_usable(const VulkanDeviceInfo& device)
{
    return device.available && device.shader_int64;
}

VulkanDeviceInfo select_vulkan_analysis_device(std::optional<std::size_t> requested_index)
{
    if (requested_index.has_value()) {
        auto device = select_vulkan_device(requested_index);
        if (!device.shader_int64) {
            throw std::runtime_error("selected Vulkan device does not support shaderInt64: " +
                device.device_name);
        }
        return device;
    }

    const auto devices = list_vulkan_devices();
    if (devices.empty()) {
        throw std::runtime_error("no Vulkan devices found");
    }
    for (const auto& device : devices) {
        if (vulkan_analysis_device_usable(device) && device.device_type == "discrete-gpu") {
            return device;
        }
    }
    for (const auto& device : devices) {
        if (vulkan_analysis_device_usable(device) && device.device_type != "cpu") {
            return device;
        }
    }
    throw std::runtime_error(
        "no available non-CPU Vulkan compute devices with shaderInt64 found");
}

void validate_vulkan_session_device(
    const VulkanCompressionOptions& options,
    std::size_t session_device_index)
{
    if (options.device_index.has_value() && *options.device_index != session_device_index) {
        throw std::runtime_error(
            "Vulkan compression session device does not match requested device");
    }
}

ConversionStats compress_lds_to_vulkan_native_flac_with_session(
    std::istream& lds_input,
    const std::string& output_path,
    const VulkanCompressionOptions& options,
    vulkan_detail::VulkanMonoExactAnalysisSession& session,
    Clock::time_point total_started)
{
    const AcceleratedNativeCompressionOptions accelerated_options {
        .backend_label = "Vulkan",
        .container = options.container,
        .sample_rate = options.sample_rate,
        .thread_count = options.thread_count,
        .frame_samples = options.frame_samples,
        .max_lpc_order = options.max_lpc_order,
        .lpc_precision = options.lpc_precision,
        .max_rice_partition_order = options.max_rice_partition_order,
        .batch_frames = kVulkanBatchFrames,
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
            return analyze_vulkan_selected_frames(
                samples, frame_info, frame_samples, session, analysis_profile, native_stats);
        });
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_total_ns, total_started);
    }
    return stats;
}

}  // namespace

VulkanCompressionSession::VulkanCompressionSession(
    std::optional<std::size_t> requested_device_index)
{
    const auto selected_device = select_vulkan_analysis_device(requested_device_index);
    device_index_ = selected_device.index;
    analysis_session_ =
        std::make_unique<vulkan_detail::VulkanMonoExactAnalysisSession>(
            std::optional<std::size_t>(device_index_));
}

VulkanCompressionSession::~VulkanCompressionSession() = default;

ConversionStats VulkanCompressionSession::compress_lds_to_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const VulkanCompressionOptions& options)
{
    const auto total_started = Clock::now();
    validate_vulkan_options(options);
    validate_vulkan_session_device(options, device_index_);
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_setup_ns, total_started);
    }
    return compress_lds_to_vulkan_native_flac_with_session(
        lds_input, output_path, options, *analysis_session_, total_started);
}

ConversionStats compress_lds_to_vulkan_native_flac(
    std::istream& lds_input,
    const std::string& output_path,
    const VulkanCompressionOptions& options)
{
    const auto total_started = Clock::now();
    validate_vulkan_options(options);
    const auto selected_device = select_vulkan_analysis_device(options.device_index);
    const auto device_index = std::optional<std::size_t>(selected_device.index);
    vulkan_detail::VulkanMonoExactAnalysisSession session(device_index);
    if (options.native_stats != nullptr) {
        add_elapsed_ns(options.native_stats->accelerated_setup_ns, total_started);
    }

    return compress_lds_to_vulkan_native_flac_with_session(
        lds_input, output_path, options, session, total_started);
}

}  // namespace ldcompress
