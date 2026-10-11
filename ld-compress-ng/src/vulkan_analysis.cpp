#include "vulkan_analysis.h"

#include "vulkan_devices.h"
#include "vulkan_shaders.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef LDCOMPRESS_HAVE_VULKAN
#define LDCOMPRESS_HAVE_VULKAN 0
#endif

#if LDCOMPRESS_HAVE_VULKAN
#include <vulkan/vulkan.h>
#endif

#if LDCOMPRESS_HAVE_VULKAN
#define LDCOMPRESS_VULKAN_ONLY_USED
#else
#define LDCOMPRESS_VULKAN_ONLY_USED [[maybe_unused]]
#endif

namespace ldcompress::vulkan_detail {
namespace {

using opencl_detail::FlacClSubframeTask;
using opencl_detail::OpenClMonoAnalysisTaskPlan;
using opencl_detail::OpenClMonoBestMethodResult;
using opencl_detail::OpenClMonoFixedConstantAnalysisResult;

LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint64_t kFenceTimeoutNs = 10'000'000'000ULL;
constexpr std::size_t kVulkanAnalysisMaxBlockSize = 8192;
constexpr std::size_t kVulkanAnalysisBitsPerSample = 16;
constexpr unsigned kVulkanExactMaxRicePartitionOrder = 8;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kWorkGroupSize = 64;
constexpr double kPi = 3.14159265358979323846;
constexpr double kGeneratedTukeyTaperFraction = 0.5;
constexpr double kSubdivideTukey3TaperFraction = 0.5 / 3.0;
constexpr std::size_t kSubdivideTukey3WindowCount = 9;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampBegin = 0;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampAfterUpload = 1;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampAfterPrepare = 2;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampAfterGeneratedAutocorrelation = 3;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampAfterGeneratedLpc = 4;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampAfterGeneratedQuantize = 5;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampAfterFixedOrderGuess = 6;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampAfterExactAnalysis = 7;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampAfterChooseBest = 8;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampAfterReadback = 9;
LDCOMPRESS_VULKAN_ONLY_USED constexpr std::uint32_t kTimestampQueryCount = 10;

struct GeneratedLpcConfig {
    std::size_t lpc_tasks_per_window = 0;
    std::size_t window_count = 0;
    std::size_t total_lpc_tasks = 0;
    unsigned coefficient_precision = 0;
    NativeAnalysisProfile analysis_profile = NativeAnalysisProfile::Exact;
    unsigned max_lpc_order = 0;
};

[[maybe_unused]] std::uint64_t checked_buffer_bytes(
    std::size_t count,
    std::size_t element_size,
    const char* name)
{
    if (count == 0) {
        throw std::runtime_error(std::string("Vulkan analysis ") + name + " buffer is empty");
    }
    if (count > std::numeric_limits<std::uint64_t>::max() / element_size) {
        throw std::runtime_error(std::string("Vulkan analysis ") + name + " buffer is too large");
    }
    return static_cast<std::uint64_t>(count) * element_size;
}

[[maybe_unused]] std::uint32_t checked_u32(std::size_t value, const char* name)
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string("Vulkan analysis ") + name + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

[[maybe_unused]] std::uint64_t timestamp_delta_ticks(
    std::uint64_t begin,
    std::uint64_t end,
    std::uint32_t valid_bits)
{
    if (valid_bits == 0) {
        return 0;
    }
    if (valid_bits >= 64) {
        return end - begin;
    }
    const auto mask = (std::uint64_t {1} << valid_bits) - 1U;
    return ((end & mask) - (begin & mask)) & mask;
}

[[maybe_unused]] std::uint64_t timestamp_delta_ns(
    std::uint64_t begin,
    std::uint64_t end,
    std::uint32_t valid_bits,
    float timestamp_period_ns)
{
    const auto ticks = timestamp_delta_ticks(begin, end, valid_bits);
    const auto nanoseconds =
        static_cast<double>(ticks) * static_cast<double>(timestamp_period_ns);
    if (nanoseconds >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(nanoseconds);
}

void validate_best_method_plan(const OpenClMonoAnalysisTaskPlan& plan)
{
    if (plan.residual_tasks_per_frame == 0 || plan.estimate_tasks_per_frame == 0) {
        throw std::runtime_error("Vulkan mono analysis plan has no tasks per frame");
    }
    if (plan.residual_tasks.empty() || plan.selected_tasks.empty()) {
        throw std::runtime_error("Vulkan mono analysis plan has no task data");
    }
    if (plan.estimate_tasks_per_frame != plan.residual_tasks_per_frame) {
        throw std::runtime_error("Vulkan mono analysis currently supports full mono selection only");
    }
    if ((plan.selected_tasks.size() % plan.estimate_tasks_per_frame) != 0 ||
        (plan.residual_tasks.size() % plan.residual_tasks_per_frame) != 0) {
        throw std::runtime_error("Vulkan mono analysis task plan is not frame-aligned");
    }
    if (plan.selected_tasks.size() / plan.estimate_tasks_per_frame !=
        plan.residual_tasks.size() / plan.residual_tasks_per_frame) {
        throw std::runtime_error("Vulkan mono analysis selected/residual frame counts differ");
    }

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    for (const auto selected : plan.selected_tasks) {
        if (selected < 0 || static_cast<std::size_t>(selected) >= plan.residual_tasks.size()) {
            throw std::runtime_error("Vulkan mono analysis selected task index is out of range");
        }
    }

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto& first_task = plan.residual_tasks.at(task_base);
        for (std::size_t i = 1; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks.at(task_base + i);
            if (task.data.samplesOffs != first_task.data.samplesOffs ||
                task.data.blocksize != first_task.data.blocksize ||
                task.data.obits != first_task.data.obits) {
                throw std::runtime_error("Vulkan mono analysis frame task group mixes sample ranges");
            }
        }

        const auto selected_base = frame * plan.estimate_tasks_per_frame;
        for (std::size_t i = 0; i < plan.estimate_tasks_per_frame; ++i) {
            const auto selected = static_cast<std::size_t>(
                plan.selected_tasks.at(selected_base + i));
            if (selected < task_base || selected >= task_base + plan.residual_tasks_per_frame) {
                throw std::runtime_error("Vulkan mono analysis selected task crosses frame group");
            }
        }
    }
}

void validate_sample_range(std::int32_t sample, unsigned bits_per_sample)
{
    const auto min_value = -(std::int64_t {1} << (bits_per_sample - 1U));
    const auto max_value = (std::int64_t {1} << (bits_per_sample - 1U)) - 1;
    if (sample < min_value || sample > max_value) {
        throw std::runtime_error("Vulkan exact analysis sample is outside the selected bit depth");
    }
}

bool signed_value_fits_bits(std::int32_t value, unsigned bits)
{
    if (bits == 0 || bits > 31) {
        return false;
    }
    const auto min_value = -(std::int64_t {1} << (bits - 1U));
    const auto max_value = (std::int64_t {1} << (bits - 1U)) - 1;
    return value >= min_value && value <= max_value;
}

bool generated_profile_uses_subdivide_tukey3(NativeAnalysisProfile profile)
{
    return profile == NativeAnalysisProfile::SubdivideTukey3MeanRice ||
        profile == NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice;
}

LDCOMPRESS_VULKAN_ONLY_USED std::uint32_t vulkan_analysis_profile_arg(
    NativeAnalysisProfile profile)
{
    switch (profile) {
    case NativeAnalysisProfile::Exact:
        return 0;
    case NativeAnalysisProfile::OrderGuessExactRice:
        return 1;
    case NativeAnalysisProfile::OrderGuessMeanRice:
        return 2;
    case NativeAnalysisProfile::SubdivideTukey3MeanRice:
        return 3;
    case NativeAnalysisProfile::OrderGuessMeanEstimateRice:
        return 4;
    case NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice:
        return 5;
    }
    return 0;
}

float tukey_weight(std::size_t n, std::size_t blocksize, double taper_fraction)
{
    if (blocksize <= 1 || taper_fraction <= 0.0) {
        return 1.0F;
    }
    if (taper_fraction >= 1.0) {
        const auto denominator = static_cast<double>(blocksize - 1U);
        const auto phase = 2.0 * kPi * static_cast<double>(n) / denominator;
        return static_cast<float>(0.5 - (0.5 * std::cos(phase)));
    }

    const auto edge_width = static_cast<std::size_t>(
        (taper_fraction / 2.0) * static_cast<double>(blocksize));
    if (edge_width == 0) {
        return 1.0F;
    }
    const auto np = edge_width - 1U;
    if (np == 0) {
        return (n == 0 || n + 1U == blocksize) ? 0.0F : 1.0F;
    }
    if (n <= np) {
        return static_cast<float>(
            0.5 - (0.5 * std::cos(kPi * static_cast<double>(n) / static_cast<double>(np))));
    }
    if (n >= blocksize - np - 1U) {
        const auto right_n = n - (blocksize - np - 1U);
        return static_cast<float>(0.5 -
            (0.5 * std::cos(
                kPi * static_cast<double>(right_n + np) / static_cast<double>(np))));
    }
    return 1.0F;
}

float partial_tukey_weight(
    std::size_t n,
    std::size_t blocksize,
    double taper_fraction,
    double start,
    double end)
{
    if (blocksize == 0 || end <= start) {
        return 0.0F;
    }
    const auto start_n = std::min<std::size_t>(
        blocksize, static_cast<std::size_t>(start * static_cast<double>(blocksize)));
    const auto end_n = std::min<std::size_t>(
        blocksize, static_cast<std::size_t>(end * static_cast<double>(blocksize)));
    const auto width = end_n > start_n ? end_n - start_n : 0;
    if (width == 0 || n < start_n || n >= end_n) {
        return 0.0F;
    }

    const auto taper = std::clamp(taper_fraction, 0.05, 0.95);
    const auto edge_width = static_cast<std::size_t>(
        (taper / 2.0) * static_cast<double>(width));
    double weight = 1.0;
    if (edge_width != 0 && n < start_n + edge_width) {
        const auto i = n - start_n + 1U;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(edge_width)));
    } else if (edge_width != 0 && n >= end_n - edge_width) {
        const auto i = end_n - n;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(edge_width)));
    }
    return static_cast<float>(weight);
}

float punchout_tukey_weight(
    std::size_t n,
    std::size_t blocksize,
    double taper_fraction,
    double start,
    double end)
{
    if (blocksize == 0 || end <= start) {
        return 1.0F;
    }

    const auto start_n = std::min<std::size_t>(
        blocksize, static_cast<std::size_t>(start * static_cast<double>(blocksize)));
    const auto end_n = std::min<std::size_t>(
        blocksize, static_cast<std::size_t>(end * static_cast<double>(blocksize)));
    const auto taper = std::clamp(taper_fraction, 0.05, 0.95);
    const auto left_edge = static_cast<std::size_t>(
        (taper / 2.0) * static_cast<double>(start_n));
    const auto right_span = blocksize > end_n ? blocksize - end_n : 0;
    const auto right_edge = static_cast<std::size_t>(
        (taper / 2.0) * static_cast<double>(right_span));

    double weight = 1.0;
    if (n >= start_n && n < end_n) {
        weight = 0.0;
    } else if (left_edge != 0 && n < left_edge) {
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(n + 1U) / static_cast<double>(left_edge)));
    } else if (left_edge != 0 && n >= start_n - left_edge && n < start_n) {
        const auto i = start_n - n;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(left_edge)));
    } else if (right_edge != 0 && n >= end_n && n < end_n + right_edge) {
        const auto i = n - end_n + 1U;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(right_edge)));
    } else if (right_edge != 0 && n >= blocksize - right_edge) {
        const auto i = blocksize - n;
        weight = 0.5 -
            (0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(right_edge)));
    }
    return static_cast<float>(weight);
}

[[maybe_unused]] std::vector<float> make_generated_lpc_windows(
    std::size_t blocksize,
    std::size_t window_count,
    NativeAnalysisProfile profile)
{
    std::vector<float> windows(blocksize * window_count, 1.0F);
    if (generated_profile_uses_subdivide_tukey3(profile)) {
        if (window_count != kSubdivideTukey3WindowCount || blocksize == 0) {
            return windows;
        }
        for (std::size_t n = 0; n < blocksize; ++n) {
            windows[n] = tukey_weight(n, blocksize, kSubdivideTukey3TaperFraction);
        }
        std::size_t window = 1;
        for (unsigned parts : {2U, 3U}) {
            for (unsigned part = 0; part < parts; ++part, ++window) {
                const auto start = static_cast<double>(part) / static_cast<double>(parts);
                const auto end = static_cast<double>(part + 1U) / static_cast<double>(parts);
                auto* values = windows.data() + (window * blocksize);
                for (std::size_t n = 0; n < blocksize; ++n) {
                    values[n] = partial_tukey_weight(
                        n, blocksize, kSubdivideTukey3TaperFraction, start, end);
                }
            }
        }
        for (unsigned part = 0; part < 3; ++part, ++window) {
            const auto start = static_cast<double>(part) / 3.0;
            const auto end = static_cast<double>(part + 1U) / 3.0;
            auto* values = windows.data() + (window * blocksize);
            for (std::size_t n = 0; n < blocksize; ++n) {
                values[n] = punchout_tukey_weight(
                    n, blocksize, kSubdivideTukey3TaperFraction, start, end);
            }
        }
        return windows;
    }

    if (window_count < 2 || blocksize <= 1) {
        return windows;
    }

    auto* tukey = windows.data() + blocksize;
    for (std::size_t n = 0; n < blocksize; ++n) {
        tukey[n] = tukey_weight(n, blocksize, kGeneratedTukeyTaperFraction);
    }
    if (window_count < 3) {
        return windows;
    }

    auto* welch = windows.data() + (2U * blocksize);
    const auto endpoint = static_cast<double>(blocksize - 1U);
    const auto midpoint = endpoint / 2.0;
    for (std::size_t n = 0; n < blocksize; ++n) {
        const auto k = (static_cast<double>(n) - midpoint) / midpoint;
        welch[n] = static_cast<float>(1.0 - (k * k));
    }
    return windows;
}

GeneratedLpcConfig generated_lpc_prefix_shape(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::size_t frame,
    unsigned coefficient_precision)
{
    const auto task_base = frame * plan.residual_tasks_per_frame;
    std::size_t total_lpc_tasks = 0;
    while (total_lpc_tasks < plan.residual_tasks_per_frame &&
        plan.residual_tasks[task_base + total_lpc_tasks].data.type ==
            opencl_detail::kFlacClSubframeLpc) {
        ++total_lpc_tasks;
    }

    if (total_lpc_tasks == 0) {
        throw std::runtime_error("Vulkan generated analysis requires an LPC task prefix");
    }

    std::size_t lpc_tasks_per_window = 0;
    while (lpc_tasks_per_window < total_lpc_tasks &&
        plan.residual_tasks[task_base + lpc_tasks_per_window].data.residualOrder ==
            static_cast<std::int32_t>(lpc_tasks_per_window + 1U)) {
        ++lpc_tasks_per_window;
    }
    if (lpc_tasks_per_window == 0 ||
        lpc_tasks_per_window > opencl_detail::kFlacClMaxOrder) {
        throw std::runtime_error("Vulkan generated analysis has invalid LPC tasks per window");
    }

    const auto window_count =
        (total_lpc_tasks + lpc_tasks_per_window - 1U) / lpc_tasks_per_window;
    for (std::size_t window = 0; window < window_count; ++window) {
        const auto window_base = task_base + window * lpc_tasks_per_window;
        const auto remaining = total_lpc_tasks - (window * lpc_tasks_per_window);
        const auto slots_this_window = std::min(lpc_tasks_per_window, remaining);
        for (std::size_t slot = 0; slot < slots_this_window; ++slot) {
            const auto& task = plan.residual_tasks[window_base + slot];
            if (task.data.type != opencl_detail::kFlacClSubframeLpc ||
                task.data.residualOrder <= 0 ||
                task.data.residualOrder >
                    static_cast<std::int32_t>(opencl_detail::kFlacClMaxOrder)) {
                throw std::runtime_error("Vulkan generated analysis LPC window group is invalid");
            }
            if (slots_this_window == lpc_tasks_per_window &&
                task.data.residualOrder != static_cast<std::int32_t>(slot + 1U)) {
                throw std::runtime_error(
                    "Vulkan generated analysis full LPC window group is not sequential");
            }
        }
    }

    return GeneratedLpcConfig {
        .lpc_tasks_per_window = lpc_tasks_per_window,
        .window_count = window_count,
        .total_lpc_tasks = total_lpc_tasks,
        .coefficient_precision = coefficient_precision,
        .analysis_profile = plan.analysis_profile,
        .max_lpc_order = plan.max_lpc_order,
    };
}

void validate_vulkan_exact_analysis_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order,
    bool allow_lpc)
{
    validate_best_method_plan(plan);
    if (max_rice_partition_order > kVulkanExactMaxRicePartitionOrder) {
        throw std::runtime_error("Vulkan fixed/constant analysis max Rice partition order must be 0..8");
    }

    for (const auto sample : samples) {
        validate_sample_range(sample, kVulkanAnalysisBitsPerSample);
    }

    for (const auto& task : plan.residual_tasks) {
        if (task.data.type != opencl_detail::kFlacClSubframeConstant &&
            task.data.type != opencl_detail::kFlacClSubframeFixed &&
            (!allow_lpc ||
                (task.data.type != opencl_detail::kFlacClSubframeLpc &&
                 task.data.type != opencl_detail::kFlacClSubframeVerbatim))) {
            throw std::runtime_error("Vulkan mono analysis received unsupported task type");
        }
        if (task.data.type == opencl_detail::kFlacClSubframeFixed &&
            (task.data.residualOrder < 0 || task.data.residualOrder > 4)) {
            throw std::runtime_error("Vulkan mono analysis received invalid fixed order");
        }
        if (task.data.type == opencl_detail::kFlacClSubframeLpc &&
            (task.data.residualOrder <= 0 ||
                task.data.residualOrder > static_cast<std::int32_t>(opencl_detail::kFlacClMaxOrder))) {
            throw std::runtime_error("Vulkan mono analysis received invalid LPC order");
        }
        if (task.data.obits != static_cast<std::int32_t>(kVulkanAnalysisBitsPerSample)) {
            throw std::runtime_error("Vulkan mono analysis currently supports 16-bit tasks only");
        }
        if (task.data.blocksize <= 0 ||
            static_cast<std::size_t>(task.data.blocksize) > kVulkanAnalysisMaxBlockSize) {
            throw std::runtime_error("Vulkan mono analysis block size is unsupported");
        }
        if ((task.data.type == opencl_detail::kFlacClSubframeFixed ||
                task.data.type == opencl_detail::kFlacClSubframeLpc) &&
            task.data.residualOrder >= task.data.blocksize) {
            throw std::runtime_error("Vulkan mono analysis predictor order exceeds block size");
        }
        if (task.data.samplesOffs < 0 ||
            static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
            static_cast<std::size_t>(task.data.blocksize) >
                samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
            throw std::runtime_error("Vulkan mono analysis task samples are out of range");
        }
        if (task.data.type == opencl_detail::kFlacClSubframeLpc) {
            if (task.data.shift < 0 || task.data.shift > 15) {
                throw std::runtime_error("Vulkan mono analysis LPC task shift is unsupported");
            }
            if (task.data.cbits <= 0 || task.data.cbits > 15) {
                throw std::runtime_error("Vulkan mono analysis LPC coefficient precision is unsupported");
            }
            for (int i = 0; i < task.data.residualOrder; ++i) {
                if (!signed_value_fits_bits(
                        task.coefs[static_cast<std::size_t>(i)],
                        static_cast<unsigned>(task.data.cbits))) {
                    throw std::runtime_error("Vulkan mono analysis LPC coefficient does not fit precision");
                }
            }
        } else if (task.data.shift < 0 || task.data.shift > 30) {
            throw std::runtime_error("Vulkan mono analysis task shift is unsupported");
        }
    }
}

GeneratedLpcConfig validate_vulkan_generated_analysis_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    validate_best_method_plan(plan);
    if (max_rice_partition_order > kVulkanExactMaxRicePartitionOrder) {
        throw std::runtime_error("Vulkan generated analysis max Rice partition order must be 0..8");
    }
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("Vulkan generated analysis coefficient precision must be 1..15");
    }

    for (const auto sample : samples) {
        validate_sample_range(sample, kVulkanAnalysisBitsPerSample);
    }

    std::optional<GeneratedLpcConfig> lpc_shape;
    std::optional<std::int32_t> blocksize;
    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto frame_lpc_shape =
            generated_lpc_prefix_shape(plan, frame, lpc_coefficient_precision);
        if (!lpc_shape.has_value()) {
            lpc_shape = frame_lpc_shape;
        } else if (
            lpc_shape->lpc_tasks_per_window != frame_lpc_shape.lpc_tasks_per_window ||
            lpc_shape->window_count != frame_lpc_shape.window_count ||
            lpc_shape->total_lpc_tasks != frame_lpc_shape.total_lpc_tasks) {
            throw std::runtime_error("Vulkan generated analysis LPC prefix differs by frame");
        }

        for (std::size_t i = 0; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks[task_base + i];
            if (task.data.obits != static_cast<std::int32_t>(kVulkanAnalysisBitsPerSample)) {
                throw std::runtime_error("Vulkan generated analysis currently supports 16-bit tasks only");
            }
            if (task.data.blocksize <= 0 ||
                static_cast<std::size_t>(task.data.blocksize) > kVulkanAnalysisMaxBlockSize) {
                throw std::runtime_error("Vulkan generated analysis block size is unsupported");
            }
            if (task.data.samplesOffs < 0 ||
                static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
                static_cast<std::size_t>(task.data.blocksize) >
                    samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
                throw std::runtime_error("Vulkan generated analysis task samples are out of range");
            }
            if (!blocksize.has_value()) {
                blocksize = task.data.blocksize;
            } else if (*blocksize != task.data.blocksize) {
                throw std::runtime_error("Vulkan generated analysis requires one block size per launch");
            }

            if (i < frame_lpc_shape.total_lpc_tasks) {
                if (task.data.type != opencl_detail::kFlacClSubframeLpc) {
                    throw std::runtime_error("Vulkan generated analysis received non-LPC prefix task");
                }
                if (task.data.residualOrder >= task.data.blocksize) {
                    throw std::runtime_error("Vulkan generated analysis LPC order exceeds block size");
                }
            } else if (task.data.type == opencl_detail::kFlacClSubframeLpc) {
                throw std::runtime_error("Vulkan generated analysis received non-prefix LPC task");
            } else if (task.data.type == opencl_detail::kFlacClSubframeFixed) {
                if (task.data.residualOrder < 0 || task.data.residualOrder > 4) {
                    throw std::runtime_error("Vulkan generated analysis received invalid fixed order");
                }
                if (task.data.residualOrder >= task.data.blocksize) {
                    throw std::runtime_error("Vulkan generated analysis fixed order exceeds block size");
                }
                if (task.data.shift < 0 || task.data.shift > 30) {
                    throw std::runtime_error("Vulkan generated analysis fixed task shift is unsupported");
                }
            } else if (task.data.type != opencl_detail::kFlacClSubframeConstant) {
                throw std::runtime_error("Vulkan generated analysis received unsupported task type");
            }
        }
    }

    return *lpc_shape;
}

#if LDCOMPRESS_HAVE_VULKAN

std::string vk_result_name(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    default:
        return "Vulkan error " + std::to_string(static_cast<int>(result));
    }
}

void require_vk(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + vk_result_name(result));
    }
}

template <typename Fn>
class ScopeExit final {
public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit()
    {
        if (active_) {
            fn_();
        }
    }

private:
    Fn fn_;
    bool active_ = true;
};

template <typename Fn>
ScopeExit<Fn> scope_exit(Fn fn)
{
    return ScopeExit<Fn>(std::move(fn));
}

VkInstance create_instance()
{
    const VkApplicationInfo app_info {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "ld-compress-ng",
        .applicationVersion = VK_MAKE_VERSION(1, 1, 0),
        .pEngineName = "ld-compress-ng",
        .engineVersion = VK_MAKE_VERSION(1, 1, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo create_info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
    };

    VkInstance instance = VK_NULL_HANDLE;
    require_vk(vkCreateInstance(&create_info, nullptr, &instance), "vkCreateInstance");
    return instance;
}

std::vector<VkPhysicalDevice> enumerate_physical_devices(VkInstance instance)
{
    std::uint32_t count = 0;
    require_vk(vkEnumeratePhysicalDevices(instance, &count, nullptr),
        "vkEnumeratePhysicalDevices");
    if (count == 0) {
        return {};
    }

    std::vector<VkPhysicalDevice> devices(count);
    require_vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
        "vkEnumeratePhysicalDevices");
    devices.resize(count);
    return devices;
}

struct ComputeQueueFamily {
    std::uint32_t index = 0;
    VkQueueFamilyProperties properties {};
};

std::optional<ComputeQueueFamily> compute_queue_family(VkPhysicalDevice device)
{
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    if (count == 0) {
        return std::nullopt;
    }

    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
            families[i].queueCount > 0) {
            return ComputeQueueFamily {
                .index = i,
                .properties = families[i],
            };
        }
    }
    return std::nullopt;
}

bool shader_int64_supported(VkPhysicalDevice device)
{
    VkPhysicalDeviceFeatures features {};
    vkGetPhysicalDeviceFeatures(device, &features);
    return features.shaderInt64 == VK_TRUE;
}

struct SelectedDevice {
    std::uint32_t index = 0;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    std::uint32_t queue_family_index = 0;
    std::uint32_t timestamp_valid_bits = 0;
    float timestamp_period_ns = 0.0F;
    std::string name;
};

SelectedDevice select_physical_device(
    VkInstance instance,
    std::optional<std::size_t> requested_device_index)
{
    const auto devices = enumerate_physical_devices(instance);
    if (devices.empty()) {
        throw std::runtime_error("no Vulkan devices found");
    }

    auto make_selection = [&](std::size_t index) {
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(devices[index], &properties);
        const auto queue_family = compute_queue_family(devices[index]);
        if (!queue_family.has_value()) {
            throw std::runtime_error("selected Vulkan device has no compute queue: " +
                std::string(properties.deviceName));
        }
        if (!shader_int64_supported(devices[index])) {
            throw std::runtime_error("selected Vulkan device does not support shaderInt64: " +
                std::string(properties.deviceName));
        }
        return SelectedDevice {
            .index = static_cast<std::uint32_t>(index),
            .physical_device = devices[index],
            .queue_family_index = queue_family->index,
            .timestamp_valid_bits = queue_family->properties.timestampValidBits,
            .timestamp_period_ns = properties.limits.timestampPeriod,
            .name = properties.deviceName,
        };
    };

    if (requested_device_index.has_value()) {
        if (*requested_device_index >= devices.size()) {
            throw std::runtime_error("Vulkan analysis device index out of range: " +
                std::to_string(*requested_device_index) + " (visible devices: " +
                std::to_string(devices.size()) + ")");
        }
        return make_selection(*requested_device_index);
    }

    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (compute_queue_family(devices[i]).has_value() && shader_int64_supported(devices[i])) {
            return make_selection(i);
        }
    }
    throw std::runtime_error("no available Vulkan compute devices with shaderInt64 found");
}

std::uint32_t find_memory_type(
    VkPhysicalDevice physical_device,
    std::uint32_t type_bits,
    VkMemoryPropertyFlags required_flags,
    VkMemoryPropertyFlags preferred_flags)
{
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    std::optional<std::uint32_t> fallback;
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((type_bits & (1U << i)) == 0) {
            continue;
        }
        const auto flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((flags & required_flags) != required_flags) {
            continue;
        }
        if ((flags & preferred_flags) == preferred_flags) {
            return i;
        }
        if (!fallback.has_value()) {
            fallback = i;
        }
    }

    if (fallback.has_value()) {
        return *fallback;
    }
    throw std::runtime_error("no compatible Vulkan memory type found");
}

bool memory_type_is_coherent(VkPhysicalDevice physical_device, std::uint32_t memory_type_index)
{
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    return (memory_properties.memoryTypes[memory_type_index].propertyFlags &
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

class HostBuffer final {
public:
    HostBuffer(
        VkDevice device,
        VkPhysicalDevice physical_device,
        VkDeviceSize size,
        VkBufferUsageFlags usage)
        : device_(device), size_(size)
    {
        const VkBufferCreateInfo buffer_info {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        require_vk(vkCreateBuffer(device_, &buffer_info, nullptr, &buffer_), "vkCreateBuffer");

        VkMemoryRequirements memory_requirements {};
        vkGetBufferMemoryRequirements(device_, buffer_, &memory_requirements);
        const auto memory_type_index = find_memory_type(
            physical_device,
            memory_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        coherent_ = memory_type_is_coherent(physical_device, memory_type_index);

        const VkMemoryAllocateInfo allocate_info {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = memory_requirements.size,
            .memoryTypeIndex = memory_type_index,
        };
        require_vk(vkAllocateMemory(device_, &allocate_info, nullptr, &memory_),
            "vkAllocateMemory");
        require_vk(vkBindBufferMemory(device_, buffer_, memory_, 0), "vkBindBufferMemory");
        require_vk(vkMapMemory(device_, memory_, 0, memory_requirements.size, 0, &mapped_),
            "vkMapMemory");
    }

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    ~HostBuffer()
    {
        if (mapped_ != nullptr) {
            vkUnmapMemory(device_, memory_);
        }
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer_, nullptr);
        }
        if (memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory_, nullptr);
        }
    }

    VkBuffer get() const { return buffer_; }
    VkDeviceSize size() const { return size_; }

    template <typename T>
    void copy_from(const std::vector<T>& values)
    {
        const auto bytes = checked_buffer_bytes(values.size(), sizeof(T), "host copy");
        if (bytes > size_) {
            throw std::runtime_error("Vulkan analysis host copy exceeds buffer size");
        }
        std::memcpy(mapped_, values.data(), static_cast<std::size_t>(bytes));
    }

    template <typename T>
    void copy_to(std::vector<T>& values)
    {
        const auto bytes = checked_buffer_bytes(values.size(), sizeof(T), "host read");
        if (bytes > size_) {
            throw std::runtime_error("Vulkan analysis host read exceeds buffer size");
        }
        std::memcpy(values.data(), mapped_, static_cast<std::size_t>(bytes));
    }

    void flush() const
    {
        if (coherent_) {
            return;
        }
        const VkMappedMemoryRange range {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .pNext = nullptr,
            .memory = memory_,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        require_vk(vkFlushMappedMemoryRanges(device_, 1, &range),
            "vkFlushMappedMemoryRanges");
    }

    void invalidate() const
    {
        if (coherent_) {
            return;
        }
        const VkMappedMemoryRange range {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .pNext = nullptr,
            .memory = memory_,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        require_vk(vkInvalidateMappedMemoryRanges(device_, 1, &range),
            "vkInvalidateMappedMemoryRanges");
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize size_ = 0;
    bool coherent_ = false;
};

class DeviceBuffer final {
public:
    DeviceBuffer(
        VkDevice device,
        VkPhysicalDevice physical_device,
        VkDeviceSize size,
        VkBufferUsageFlags usage)
        : device_(device), size_(size)
    {
        const VkBufferCreateInfo buffer_info {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        require_vk(vkCreateBuffer(device_, &buffer_info, nullptr, &buffer_), "vkCreateBuffer");

        VkMemoryRequirements memory_requirements {};
        vkGetBufferMemoryRequirements(device_, buffer_, &memory_requirements);
        const auto memory_type_index = find_memory_type(
            physical_device,
            memory_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            0);

        const VkMemoryAllocateInfo allocate_info {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = nullptr,
            .allocationSize = memory_requirements.size,
            .memoryTypeIndex = memory_type_index,
        };
        require_vk(vkAllocateMemory(device_, &allocate_info, nullptr, &memory_),
            "vkAllocateMemory");
        require_vk(vkBindBufferMemory(device_, buffer_, memory_, 0), "vkBindBufferMemory");
    }

    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    ~DeviceBuffer()
    {
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer_, nullptr);
        }
        if (memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, memory_, nullptr);
        }
    }

    VkBuffer get() const { return buffer_; }
    VkDeviceSize size() const { return size_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
};

struct PushConstants {
    std::uint32_t mode = 0;
    std::uint32_t task_count = 0;
    std::uint32_t frame_count = 0;
    std::uint32_t selected_tasks_per_frame = 0;
    std::uint32_t residual_tasks_per_frame = 0;
    std::uint32_t max_rice_partition_order = 0;
    std::uint32_t lpc_coefficient_precision = 0;
    std::uint32_t lpc_tasks_per_window = 0;
    std::uint32_t total_lpc_tasks = 0;
    std::uint32_t generated_window_count = 0;
    std::uint32_t max_lpc_order = 0;
    std::uint32_t analysis_profile = 0;
    std::uint32_t use_shifted_samples = 0;
};

bool uses_shifted_speed_profile(
    const std::optional<GeneratedLpcConfig>& generated_lpc,
    unsigned max_rice_partition_order)
{
    return generated_lpc.has_value() &&
        generated_lpc->analysis_profile == NativeAnalysisProfile::OrderGuessMeanEstimateRice &&
        generated_lpc->window_count == 3U &&
        generated_lpc->lpc_tasks_per_window == 1U &&
        generated_lpc->max_lpc_order <= 12U &&
        max_rice_partition_order <= 6U;
}

std::uint32_t dispatch_groups(std::uint32_t items)
{
    return (items + kWorkGroupSize - 1U) / kWorkGroupSize;
}

#endif

}  // namespace

#if LDCOMPRESS_HAVE_VULKAN

class VulkanMonoExactAnalysisSession::Impl final {
public:
    explicit Impl(std::optional<std::size_t> requested_device_index)
    {
        if (!vulkan_support_built()) {
            throw std::runtime_error("Vulkan support was not built");
        }

        try {
            instance_ = create_instance();
            selected_ = select_physical_device(instance_, requested_device_index);

            const float queue_priority = 1.0F;
            const VkDeviceQueueCreateInfo queue_create_info {
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queueFamilyIndex = selected_.queue_family_index,
                .queueCount = 1,
                .pQueuePriorities = &queue_priority,
            };
            VkPhysicalDeviceFeatures enabled_features {};
            enabled_features.shaderInt64 = VK_TRUE;
            const VkDeviceCreateInfo device_create_info {
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queueCreateInfoCount = 1,
                .pQueueCreateInfos = &queue_create_info,
                .enabledLayerCount = 0,
                .ppEnabledLayerNames = nullptr,
                .enabledExtensionCount = 0,
                .ppEnabledExtensionNames = nullptr,
                .pEnabledFeatures = &enabled_features,
            };
            require_vk(
                vkCreateDevice(selected_.physical_device, &device_create_info, nullptr, &device_),
                "vkCreateDevice");
            vkGetDeviceQueue(device_, selected_.queue_family_index, 0, &queue_);

            const auto shader = fixed_constant_shader_spirv();
            if (shader.data == nullptr || shader.size_bytes == 0 ||
                (shader.size_bytes % sizeof(std::uint32_t)) != 0) {
                throw std::runtime_error("embedded Vulkan fixed/constant shader is invalid");
            }
            const VkShaderModuleCreateInfo shader_module_info {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .codeSize = shader.size_bytes,
                .pCode = shader.data,
            };
            require_vk(
                vkCreateShaderModule(device_, &shader_module_info, nullptr, &shader_module_),
                "vkCreateShaderModule");

            const auto generated_shader = generated_shader_spirv();
            if (generated_shader.data == nullptr || generated_shader.size_bytes == 0 ||
                (generated_shader.size_bytes % sizeof(std::uint32_t)) != 0) {
                throw std::runtime_error("embedded Vulkan generated shader is invalid");
            }
            const VkShaderModuleCreateInfo generated_shader_module_info {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .codeSize = generated_shader.size_bytes,
                .pCode = generated_shader.data,
            };
            require_vk(
                vkCreateShaderModule(
                    device_, &generated_shader_module_info, nullptr, &generated_shader_module_),
                "vkCreateShaderModule(generated)");

            std::array<VkDescriptorSetLayoutBinding, 9> layout_bindings {};
            for (std::uint32_t i = 0; i < layout_bindings.size(); ++i) {
                layout_bindings[i] = VkDescriptorSetLayoutBinding {
                    .binding = i,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                    .pImmutableSamplers = nullptr,
                };
            }
            const VkDescriptorSetLayoutCreateInfo descriptor_layout_info {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .bindingCount = static_cast<std::uint32_t>(layout_bindings.size()),
                .pBindings = layout_bindings.data(),
            };
            require_vk(vkCreateDescriptorSetLayout(
                           device_, &descriptor_layout_info, nullptr, &descriptor_set_layout_),
                "vkCreateDescriptorSetLayout");

            const VkPushConstantRange push_constant_range {
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(PushConstants),
            };
            const VkPipelineLayoutCreateInfo pipeline_layout_info {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .setLayoutCount = 1,
                .pSetLayouts = &descriptor_set_layout_,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &push_constant_range,
            };
            require_vk(
                vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr, &pipeline_layout_),
                "vkCreatePipelineLayout");

            const VkPipelineShaderStageCreateInfo shader_stage_info {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = shader_module_,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            };
            const VkComputePipelineCreateInfo pipeline_info {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = shader_stage_info,
                .layout = pipeline_layout_,
                .basePipelineHandle = VK_NULL_HANDLE,
                .basePipelineIndex = 0,
            };
            require_vk(vkCreateComputePipelines(
                           device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_),
                "vkCreateComputePipelines");

            const VkPipelineShaderStageCreateInfo generated_shader_stage_info {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = generated_shader_module_,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            };
            const VkComputePipelineCreateInfo generated_pipeline_info {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = generated_shader_stage_info,
                .layout = pipeline_layout_,
                .basePipelineHandle = VK_NULL_HANDLE,
                .basePipelineIndex = 0,
            };
            require_vk(vkCreateComputePipelines(
                           device_, VK_NULL_HANDLE, 1, &generated_pipeline_info, nullptr,
                           &generated_pipeline_),
                "vkCreateComputePipelines(generated)");

            const VkDescriptorPoolSize pool_size {
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = static_cast<std::uint32_t>(layout_bindings.size()),
            };
            const VkDescriptorPoolCreateInfo descriptor_pool_info {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .maxSets = 1,
                .poolSizeCount = 1,
                .pPoolSizes = &pool_size,
            };
            require_vk(vkCreateDescriptorPool(
                           device_, &descriptor_pool_info, nullptr, &descriptor_pool_),
                "vkCreateDescriptorPool");

            const VkDescriptorSetAllocateInfo descriptor_allocate_info {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = nullptr,
                .descriptorPool = descriptor_pool_,
                .descriptorSetCount = 1,
                .pSetLayouts = &descriptor_set_layout_,
            };
            require_vk(
                vkAllocateDescriptorSets(device_, &descriptor_allocate_info, &descriptor_set_),
                "vkAllocateDescriptorSets");

            const VkCommandPoolCreateInfo command_pool_info {
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = selected_.queue_family_index,
            };
            require_vk(vkCreateCommandPool(device_, &command_pool_info, nullptr, &command_pool_),
                "vkCreateCommandPool");

            const VkCommandBufferAllocateInfo command_buffer_info {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = command_pool_,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            require_vk(vkAllocateCommandBuffers(device_, &command_buffer_info, &command_buffer_),
                "vkAllocateCommandBuffers");

            const VkFenceCreateInfo fence_info {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            };
            require_vk(vkCreateFence(device_, &fence_info, nullptr, &fence_), "vkCreateFence");

            if (selected_.timestamp_valid_bits != 0) {
                const VkQueryPoolCreateInfo query_pool_info {
                    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .queryType = VK_QUERY_TYPE_TIMESTAMP,
                    .queryCount = kTimestampQueryCount,
                    .pipelineStatistics = 0,
                };
                require_vk(
                    vkCreateQueryPool(device_, &query_pool_info, nullptr, &query_pool_),
                    "vkCreateQueryPool");
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Impl()
    {
        cleanup();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    OpenClMonoFixedConstantAnalysisResult run_validated(
        const std::vector<std::int32_t>& samples,
        const OpenClMonoAnalysisTaskPlan& plan,
        unsigned max_rice_partition_order,
        bool allow_lpc,
        std::optional<GeneratedLpcConfig> generated_lpc = std::nullopt,
        bool read_analyzed_tasks = true,
        VulkanGpuTimingStats* gpu_timings = nullptr)
    {
        (void)allow_lpc;

        const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
        const auto task_count_u32 = checked_u32(plan.residual_tasks.size(), "task count");
        const bool use_shifted_samples =
            uses_shifted_speed_profile(generated_lpc, max_rice_partition_order);
        const PushConstants base_push {
            .mode = 0,
            .task_count = task_count_u32,
            .frame_count = checked_u32(frame_count, "frame count"),
            .selected_tasks_per_frame = checked_u32(
                plan.estimate_tasks_per_frame, "selected task count"),
            .residual_tasks_per_frame = checked_u32(
                plan.residual_tasks_per_frame, "residual task count"),
            .max_rice_partition_order = checked_u32(
                max_rice_partition_order, "max Rice partition order"),
            .lpc_coefficient_precision = generated_lpc.has_value()
                ? checked_u32(generated_lpc->coefficient_precision, "LPC coefficient precision")
                : 0,
            .lpc_tasks_per_window = generated_lpc.has_value()
                ? checked_u32(generated_lpc->lpc_tasks_per_window, "LPC tasks per window")
                : 0,
            .total_lpc_tasks = generated_lpc.has_value()
                ? checked_u32(generated_lpc->total_lpc_tasks, "total LPC task count")
                : 0,
            .generated_window_count = generated_lpc.has_value()
                ? checked_u32(generated_lpc->window_count, "generated LPC window count")
                : 0,
            .max_lpc_order = generated_lpc.has_value()
                ? checked_u32(generated_lpc->max_lpc_order, "max LPC order")
                : 0,
            .analysis_profile = vulkan_analysis_profile_arg(plan.analysis_profile),
            .use_shifted_samples = use_shifted_samples ? 1U : 0U,
        };

        std::vector<FlacClSubframeTask> best_tasks(frame_count);
        std::vector<opencl_detail::FlacClRiceParameterSet> best_rice_parameters(frame_count);
        std::vector<float> dummy_floats(1, 0.0F);
        std::vector<float> generated_windows;
        const auto* window_values = &dummy_floats;
        std::size_t autocor_count = 1;
        std::size_t lpc_count = 1;
        if (generated_lpc.has_value()) {
            const auto blocksize =
                static_cast<std::size_t>(plan.residual_tasks.front().data.blocksize);
            generated_windows = make_generated_lpc_windows(
                blocksize, generated_lpc->window_count, generated_lpc->analysis_profile);
            window_values = &generated_windows;
            autocor_count =
                frame_count * generated_lpc->window_count * (opencl_detail::kFlacClMaxOrder + 1U);
            lpc_count =
                frame_count *
                generated_lpc->window_count *
                (opencl_detail::kFlacClMaxOrder + 1U) *
                opencl_detail::kFlacClMaxOrder;
        }

        const auto samples_bytes =
            checked_buffer_bytes(samples.size(), sizeof(std::int32_t), "samples");
        const auto tasks_bytes =
            checked_buffer_bytes(plan.residual_tasks.size(), sizeof(FlacClSubframeTask), "tasks");
        const auto selected_bytes =
            checked_buffer_bytes(plan.selected_tasks.size(), sizeof(std::int32_t), "selected tasks");
        const auto best_bytes =
            checked_buffer_bytes(best_tasks.size(), sizeof(FlacClSubframeTask), "best tasks");
        const auto best_rice_parameter_bytes = checked_buffer_bytes(
            best_rice_parameters.size(),
            sizeof(opencl_detail::FlacClRiceParameterSet),
            "best Rice parameters");
        if (plan.residual_tasks.size() >
            std::numeric_limits<std::size_t>::max() - best_rice_parameters.size()) {
            throw std::runtime_error("Vulkan analysis Rice parameter storage is too large");
        }
        const auto rice_parameter_count =
            best_rice_parameters.size() + plan.residual_tasks.size();
        const auto rice_parameter_bytes = checked_buffer_bytes(
            rice_parameter_count,
            sizeof(opencl_detail::FlacClRiceParameterSet),
            "Vulkan Rice parameter storage");
        const auto window_bytes =
            checked_buffer_bytes(window_values->size(), sizeof(float), "generated windows");
        const auto autocor_bytes =
            checked_buffer_bytes(autocor_count, sizeof(float), "generated autocorrelations");
        const auto lpc_bytes =
            checked_buffer_bytes(lpc_count, sizeof(float), "generated LPC coefficients");

        DeviceBuffer& samples_buffer = ensure_device_buffer(
            samples_buffer_,
            samples_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        DeviceBuffer& shifted_samples_buffer = use_shifted_samples
            ? ensure_device_buffer(
                shifted_samples_buffer_,
                samples_bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
            : samples_buffer;
        DeviceBuffer& tasks_buffer = ensure_device_buffer(
            tasks_buffer_,
            tasks_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        DeviceBuffer& selected_buffer = ensure_device_buffer(
            selected_buffer_,
            selected_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        DeviceBuffer& best_buffer = ensure_device_buffer(
            best_buffer_,
            best_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        DeviceBuffer& best_rice_parameter_buffer = ensure_device_buffer(
            best_rice_parameter_buffer_,
            rice_parameter_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        DeviceBuffer& window_buffer = ensure_device_buffer(
            window_buffer_,
            window_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        DeviceBuffer& autocor_buffer = ensure_device_buffer(
            autocor_buffer_,
            autocor_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        DeviceBuffer& lpc_buffer = ensure_device_buffer(
            lpc_buffer_,
            lpc_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        HostBuffer& samples_upload_buffer = ensure_host_buffer(
            samples_upload_buffer_, samples_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        HostBuffer& tasks_upload_buffer = ensure_host_buffer(
            tasks_upload_buffer_, tasks_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        HostBuffer& selected_upload_buffer = ensure_host_buffer(
            selected_upload_buffer_, selected_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        HostBuffer& window_upload_buffer = ensure_host_buffer(
            window_upload_buffer_, window_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        HostBuffer& best_readback_buffer = ensure_host_buffer(
            best_readback_buffer_, best_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        HostBuffer& best_rice_parameter_readback_buffer = ensure_host_buffer(
            best_rice_parameter_readback_buffer_,
            best_rice_parameter_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        HostBuffer* tasks_readback_buffer = nullptr;
        if (read_analyzed_tasks) {
            tasks_readback_buffer = &ensure_host_buffer(
                tasks_readback_buffer_, tasks_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        const bool collect_timestamps =
            gpu_timings != nullptr && query_pool_ != VK_NULL_HANDLE;

        samples_upload_buffer.copy_from(samples);
        tasks_upload_buffer.copy_from(plan.residual_tasks);
        selected_upload_buffer.copy_from(plan.selected_tasks);
        window_upload_buffer.copy_from(*window_values);
        samples_upload_buffer.flush();
        tasks_upload_buffer.flush();
        selected_upload_buffer.flush();
        window_upload_buffer.flush();

        const std::array<VkDescriptorBufferInfo, 9> descriptor_buffers {{
            VkDescriptorBufferInfo {
                .buffer = samples_buffer.get(),
                .offset = 0,
                .range = samples_buffer.size(),
            },
            VkDescriptorBufferInfo {
                .buffer = tasks_buffer.get(),
                .offset = 0,
                .range = tasks_buffer.size(),
            },
            VkDescriptorBufferInfo {
                .buffer = selected_buffer.get(),
                .offset = 0,
                .range = selected_buffer.size(),
            },
            VkDescriptorBufferInfo {
                .buffer = best_buffer.get(),
                .offset = 0,
                .range = best_buffer.size(),
            },
            VkDescriptorBufferInfo {
                .buffer = window_buffer.get(),
                .offset = 0,
                .range = window_buffer.size(),
            },
            VkDescriptorBufferInfo {
                .buffer = autocor_buffer.get(),
                .offset = 0,
                .range = autocor_buffer.size(),
            },
            VkDescriptorBufferInfo {
                .buffer = lpc_buffer.get(),
                .offset = 0,
                .range = lpc_buffer.size(),
            },
            VkDescriptorBufferInfo {
                .buffer = best_rice_parameter_buffer.get(),
                .offset = 0,
                .range = best_rice_parameter_buffer.size(),
            },
            VkDescriptorBufferInfo {
                .buffer = shifted_samples_buffer.get(),
                .offset = 0,
                .range = shifted_samples_buffer.size(),
            },
        }};
        std::array<VkWriteDescriptorSet, 9> descriptor_writes {};
        for (std::uint32_t i = 0; i < descriptor_writes.size(); ++i) {
            descriptor_writes[i] = VkWriteDescriptorSet {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = descriptor_set_,
                .dstBinding = i,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pImageInfo = nullptr,
                .pBufferInfo = &descriptor_buffers[i],
                .pTexelBufferView = nullptr,
            };
        }
        vkUpdateDescriptorSets(
            device_,
            static_cast<std::uint32_t>(descriptor_writes.size()),
            descriptor_writes.data(),
            0,
            nullptr);

        require_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer");
        const VkCommandBufferBeginInfo begin_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        };
        require_vk(vkBeginCommandBuffer(command_buffer_, &begin_info), "vkBeginCommandBuffer");
        auto write_timestamp = [&](
                                   std::uint32_t index,
                                   VkPipelineStageFlagBits stage) {
            if (collect_timestamps) {
                vkCmdWriteTimestamp(command_buffer_, stage, query_pool_, index);
            }
        };
        if (collect_timestamps) {
            vkCmdResetQueryPool(command_buffer_, query_pool_, 0, kTimestampQueryCount);
            write_timestamp(kTimestampBegin, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        }
        auto copy_buffer = [&](
                               VkBuffer source,
                               VkBuffer destination,
                               VkDeviceSize bytes) {
            const VkBufferCopy region {
                .srcOffset = 0,
                .dstOffset = 0,
                .size = bytes,
            };
            vkCmdCopyBuffer(command_buffer_, source, destination, 1, &region);
        };

        copy_buffer(samples_upload_buffer.get(), samples_buffer.get(), samples_bytes);
        copy_buffer(tasks_upload_buffer.get(), tasks_buffer.get(), tasks_bytes);
        copy_buffer(selected_upload_buffer.get(), selected_buffer.get(), selected_bytes);
        copy_buffer(window_upload_buffer.get(), window_buffer.get(), window_bytes);

        const VkMemoryBarrier upload_barrier {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        vkCmdPipelineBarrier(
            command_buffer_,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            1,
            &upload_barrier,
            0,
            nullptr,
            0,
            nullptr);
        write_timestamp(kTimestampAfterUpload, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        vkCmdBindDescriptorSets(
            command_buffer_,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            pipeline_layout_,
            0,
            1,
            &descriptor_set_,
            0,
            nullptr);

        VkPipeline bound_pipeline = VK_NULL_HANDLE;
        auto dispatch_mode = [&](VkPipeline pipeline,
                                 std::uint32_t mode,
                                 std::uint32_t groups_x,
                                 std::uint32_t groups_y = 1,
                                 std::uint32_t groups_z = 1) {
            if (bound_pipeline != pipeline) {
                vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                bound_pipeline = pipeline;
            }
            auto push = base_push;
            push.mode = mode;
            vkCmdPushConstants(
                command_buffer_,
                pipeline_layout_,
                VK_SHADER_STAGE_COMPUTE_BIT,
                0,
                sizeof(push),
                &push);
            vkCmdDispatch(command_buffer_, groups_x, groups_y, groups_z);
        };
        auto shader_write_to_read_barrier = [&]() {
            const VkMemoryBarrier barrier {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            };
            vkCmdPipelineBarrier(
                command_buffer_,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                1,
                &barrier,
                0,
                nullptr,
                0,
                nullptr);
        };

        dispatch_mode(generated_pipeline_, 2, base_push.frame_count);
        shader_write_to_read_barrier();
        write_timestamp(kTimestampAfterPrepare, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        if (generated_lpc.has_value()) {
            dispatch_mode(
                generated_pipeline_,
                3,
                base_push.frame_count,
                base_push.generated_window_count,
                checked_u32(opencl_detail::kFlacClMaxOrder + 1U, "autocorrelation lag count"));
            shader_write_to_read_barrier();
            write_timestamp(
                kTimestampAfterGeneratedAutocorrelation,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            dispatch_mode(
                generated_pipeline_, 4, base_push.frame_count, base_push.generated_window_count);
            shader_write_to_read_barrier();
            write_timestamp(kTimestampAfterGeneratedLpc, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            dispatch_mode(
                generated_pipeline_, 5, base_push.frame_count, base_push.generated_window_count);
            shader_write_to_read_barrier();
            write_timestamp(
                kTimestampAfterGeneratedQuantize, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        } else {
            write_timestamp(
                kTimestampAfterGeneratedAutocorrelation,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            write_timestamp(kTimestampAfterGeneratedLpc, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            write_timestamp(
                kTimestampAfterGeneratedQuantize, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }

        if (plan.fixed_order_guess_on_gpu) {
            dispatch_mode(pipeline_, 7, base_push.frame_count);
            shader_write_to_read_barrier();
        }
        write_timestamp(kTimestampAfterFixedOrderGuess, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        dispatch_mode(pipeline_, 0, base_push.task_count);
        shader_write_to_read_barrier();
        write_timestamp(kTimestampAfterExactAnalysis, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        dispatch_mode(pipeline_, 1, dispatch_groups(base_push.frame_count));
        shader_write_to_read_barrier();
        dispatch_mode(pipeline_, 6, base_push.frame_count);

        const VkMemoryBarrier shader_to_transfer_barrier {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        vkCmdPipelineBarrier(
            command_buffer_,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            1,
            &shader_to_transfer_barrier,
            0,
            nullptr,
            0,
            nullptr);
        write_timestamp(kTimestampAfterChooseBest, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        copy_buffer(best_buffer.get(), best_readback_buffer.get(), best_bytes);
        copy_buffer(
            best_rice_parameter_buffer.get(),
            best_rice_parameter_readback_buffer.get(),
            best_rice_parameter_bytes);
        if (tasks_readback_buffer != nullptr) {
            copy_buffer(tasks_buffer.get(), tasks_readback_buffer->get(), tasks_bytes);
        }

        const VkMemoryBarrier transfer_to_host_barrier {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        vkCmdPipelineBarrier(
            command_buffer_,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            1,
            &transfer_to_host_barrier,
            0,
            nullptr,
            0,
            nullptr);
        write_timestamp(kTimestampAfterReadback, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        require_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer");
        require_vk(vkResetFences(device_, 1, &fence_), "vkResetFences");

        const VkSubmitInfo submit_info {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &command_buffer_,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        require_vk(vkQueueSubmit(queue_, 1, &submit_info, fence_), "vkQueueSubmit");
        submission_pending_ = true;
        const VkResult wait_result =
            vkWaitForFences(device_, 1, &fence_, VK_TRUE, kFenceTimeoutNs);
        // Vulkan treats VK_ERROR_DEVICE_LOST as completed work for object
        // lifetime purposes.  Keep the pending marker only when the wait did
        // not establish either successful completion or device loss.
        if (wait_result == VK_SUCCESS || wait_result == VK_ERROR_DEVICE_LOST) {
            submission_pending_ = false;
        }
        require_vk(wait_result, "vkWaitForFences");

        if (collect_timestamps) {
            std::array<std::uint64_t, kTimestampQueryCount> timestamps {};
            require_vk(
                vkGetQueryPoolResults(
                    device_,
                    query_pool_,
                    0,
                    kTimestampQueryCount,
                    sizeof(timestamps),
                    timestamps.data(),
                    sizeof(timestamps[0]),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                "vkGetQueryPoolResults");
            auto delta_ns = [&](std::uint32_t begin, std::uint32_t end) {
                return timestamp_delta_ns(
                    timestamps.at(begin),
                    timestamps.at(end),
                    selected_.timestamp_valid_bits,
                    selected_.timestamp_period_ns);
            };
            ++gpu_timings->batches;
            gpu_timings->total_ns += delta_ns(kTimestampBegin, kTimestampAfterReadback);
            gpu_timings->upload_ns += delta_ns(kTimestampBegin, kTimestampAfterUpload);
            gpu_timings->prepare_ns += delta_ns(kTimestampAfterUpload, kTimestampAfterPrepare);
            if (generated_lpc.has_value()) {
                gpu_timings->generated_autocorrelation_ns += delta_ns(
                    kTimestampAfterPrepare,
                    kTimestampAfterGeneratedAutocorrelation);
                gpu_timings->generated_lpc_ns += delta_ns(
                    kTimestampAfterGeneratedAutocorrelation,
                    kTimestampAfterGeneratedLpc);
                gpu_timings->generated_quantize_ns += delta_ns(
                    kTimestampAfterGeneratedLpc,
                    kTimestampAfterGeneratedQuantize);
            }
            gpu_timings->fixed_order_guess_ns += delta_ns(
                kTimestampAfterGeneratedQuantize, kTimestampAfterFixedOrderGuess);
            gpu_timings->exact_analysis_ns += delta_ns(
                kTimestampAfterFixedOrderGuess, kTimestampAfterExactAnalysis);
            gpu_timings->choose_best_ns += delta_ns(
                kTimestampAfterExactAnalysis, kTimestampAfterChooseBest);
            gpu_timings->readback_ns +=
                delta_ns(kTimestampAfterChooseBest, kTimestampAfterReadback);
        }

        best_readback_buffer.invalidate();
        best_readback_buffer.copy_to(best_tasks);
        best_rice_parameter_readback_buffer.invalidate();
        best_rice_parameter_readback_buffer.copy_to(best_rice_parameters);
        std::vector<FlacClSubframeTask> analyzed_tasks;
        if (tasks_readback_buffer != nullptr) {
            analyzed_tasks.resize(plan.residual_tasks.size());
            tasks_readback_buffer->invalidate();
            tasks_readback_buffer->copy_to(analyzed_tasks);
        }

        return OpenClMonoFixedConstantAnalysisResult {
            .analyzed_tasks = std::move(analyzed_tasks),
            .best_tasks = std::move(best_tasks),
            .best_rice_parameters = std::move(best_rice_parameters),
            .device_name = selected_.name,
        };
    }

private:
    HostBuffer& ensure_host_buffer(
        std::unique_ptr<HostBuffer>& buffer,
        VkDeviceSize required_size,
        VkBufferUsageFlags usage)
    {
        if (buffer == nullptr || buffer->size() < required_size) {
            buffer = std::make_unique<HostBuffer>(
                device_, selected_.physical_device, required_size, usage);
        }
        return *buffer;
    }

    DeviceBuffer& ensure_device_buffer(
        std::unique_ptr<DeviceBuffer>& buffer,
        VkDeviceSize required_size,
        VkBufferUsageFlags usage)
    {
        if (buffer == nullptr || buffer->size() < required_size) {
            buffer = std::make_unique<DeviceBuffer>(
                device_, selected_.physical_device, required_size, usage);
        }
        return *buffer;
    }

    void abandon_device_resources() noexcept
    {
        // Do not run the buffer destructors after an unexpected idle-wait
        // failure: each one destroys a Vulkan handle that may still be in
        // use.  This path is limited to terminal driver failures, so defer
        // reclamation to process teardown instead.
        (void)samples_buffer_.release();
        (void)shifted_samples_buffer_.release();
        (void)tasks_buffer_.release();
        (void)selected_buffer_.release();
        (void)best_buffer_.release();
        (void)best_rice_parameter_buffer_.release();
        (void)window_buffer_.release();
        (void)autocor_buffer_.release();
        (void)lpc_buffer_.release();
        (void)samples_upload_buffer_.release();
        (void)tasks_upload_buffer_.release();
        (void)selected_upload_buffer_.release();
        (void)window_upload_buffer_.release();
        (void)best_readback_buffer_.release();
        (void)best_rice_parameter_readback_buffer_.release();
        (void)tasks_readback_buffer_.release();
    }

    void cleanup() noexcept
    {
        if (device_ != VK_NULL_HANDLE) {
            if (submission_pending_) {
                // This runs only when normal fence completion failed, so it
                // does not add a synchronization point to the encode path.
                // On VK_ERROR_DEVICE_LOST Vulkan considers submitted work
                // complete for lifetime purposes.  For any other failure,
                // leave the device graph intact for process teardown rather
                // than destroying handles while the driver may still use
                // them.
                const VkResult idle_result = vkDeviceWaitIdle(device_);
                if (idle_result == VK_SUCCESS || idle_result == VK_ERROR_DEVICE_LOST) {
                    submission_pending_ = false;
                } else {
                    abandon_device_resources();
                    device_ = VK_NULL_HANDLE;
                    instance_ = VK_NULL_HANDLE;
                    return;
                }
            }
            samples_buffer_.reset();
            shifted_samples_buffer_.reset();
            tasks_buffer_.reset();
            selected_buffer_.reset();
            best_buffer_.reset();
            best_rice_parameter_buffer_.reset();
            window_buffer_.reset();
            autocor_buffer_.reset();
            lpc_buffer_.reset();
            samples_upload_buffer_.reset();
            tasks_upload_buffer_.reset();
            selected_upload_buffer_.reset();
            window_upload_buffer_.reset();
            best_readback_buffer_.reset();
            best_rice_parameter_readback_buffer_.reset();
            tasks_readback_buffer_.reset();
            if (fence_ != VK_NULL_HANDLE) {
                vkDestroyFence(device_, fence_, nullptr);
                fence_ = VK_NULL_HANDLE;
            }
            if (command_pool_ != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, command_pool_, nullptr);
                command_pool_ = VK_NULL_HANDLE;
            }
            if (query_pool_ != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device_, query_pool_, nullptr);
                query_pool_ = VK_NULL_HANDLE;
            }
            if (descriptor_pool_ != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
                descriptor_pool_ = VK_NULL_HANDLE;
                descriptor_set_ = VK_NULL_HANDLE;
            }
            if (pipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, pipeline_, nullptr);
                pipeline_ = VK_NULL_HANDLE;
            }
            if (generated_pipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, generated_pipeline_, nullptr);
                generated_pipeline_ = VK_NULL_HANDLE;
            }
            if (pipeline_layout_ != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
                pipeline_layout_ = VK_NULL_HANDLE;
            }
            if (descriptor_set_layout_ != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
                descriptor_set_layout_ = VK_NULL_HANDLE;
            }
            if (shader_module_ != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, shader_module_, nullptr);
                shader_module_ = VK_NULL_HANDLE;
            }
            if (generated_shader_module_ != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, generated_shader_module_, nullptr);
                generated_shader_module_ = VK_NULL_HANDLE;
            }
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    SelectedDevice selected_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkShaderModule shader_module_ = VK_NULL_HANDLE;
    VkShaderModule generated_shader_module_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline generated_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    bool submission_pending_ = false;
    std::unique_ptr<DeviceBuffer> samples_buffer_;
    std::unique_ptr<DeviceBuffer> shifted_samples_buffer_;
    std::unique_ptr<DeviceBuffer> tasks_buffer_;
    std::unique_ptr<DeviceBuffer> selected_buffer_;
    std::unique_ptr<DeviceBuffer> best_buffer_;
    std::unique_ptr<DeviceBuffer> best_rice_parameter_buffer_;
    std::unique_ptr<DeviceBuffer> window_buffer_;
    std::unique_ptr<DeviceBuffer> autocor_buffer_;
    std::unique_ptr<DeviceBuffer> lpc_buffer_;
    std::unique_ptr<HostBuffer> samples_upload_buffer_;
    std::unique_ptr<HostBuffer> tasks_upload_buffer_;
    std::unique_ptr<HostBuffer> selected_upload_buffer_;
    std::unique_ptr<HostBuffer> window_upload_buffer_;
    std::unique_ptr<HostBuffer> best_readback_buffer_;
    std::unique_ptr<HostBuffer> best_rice_parameter_readback_buffer_;
    std::unique_ptr<HostBuffer> tasks_readback_buffer_;
};

#else

class VulkanMonoExactAnalysisSession::Impl final {};

#endif

VulkanMonoExactAnalysisSession::VulkanMonoExactAnalysisSession(
    std::optional<std::size_t> requested_device_index)
{
#if LDCOMPRESS_HAVE_VULKAN
    impl_ = std::make_unique<Impl>(requested_device_index);
#else
    (void)requested_device_index;
    throw std::runtime_error("Vulkan support was not built");
#endif
}

VulkanMonoExactAnalysisSession::~VulkanMonoExactAnalysisSession() = default;

OpenClMonoFixedConstantAnalysisResult VulkanMonoExactAnalysisSession::run_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    validate_vulkan_exact_analysis_inputs(samples, plan, max_rice_partition_order, false);
#if LDCOMPRESS_HAVE_VULKAN
    return impl_->run_validated(samples, plan, max_rice_partition_order, false);
#else
    throw std::runtime_error("Vulkan support was not built");
#endif
}

OpenClMonoBestMethodResult VulkanMonoExactAnalysisSession::run_fixed_constant_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order,
    VulkanGpuTimingStats* gpu_timings)
{
    validate_vulkan_exact_analysis_inputs(samples, plan, max_rice_partition_order, false);
#if LDCOMPRESS_HAVE_VULKAN
    auto result = impl_->run_validated(
        samples,
        plan,
        max_rice_partition_order,
        false,
        std::nullopt,
        false,
        gpu_timings);
    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(result.best_tasks),
        .best_rice_parameters = std::move(result.best_rice_parameters),
        .device_name = std::move(result.device_name),
    };
#else
    (void)gpu_timings;
    throw std::runtime_error("Vulkan support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult VulkanMonoExactAnalysisSession::run_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    validate_vulkan_exact_analysis_inputs(samples, plan, max_rice_partition_order, true);
#if LDCOMPRESS_HAVE_VULKAN
    return impl_->run_validated(samples, plan, max_rice_partition_order, true);
#else
    throw std::runtime_error("Vulkan support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult VulkanMonoExactAnalysisSession::run_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    const auto generated_lpc = validate_vulkan_generated_analysis_inputs(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order);
#if LDCOMPRESS_HAVE_VULKAN
    return impl_->run_validated(samples, plan, max_rice_partition_order, true, generated_lpc);
#else
    (void)generated_lpc;
    throw std::runtime_error("Vulkan support was not built");
#endif
}

OpenClMonoBestMethodResult VulkanMonoExactAnalysisSession::run_generated_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    VulkanGpuTimingStats* gpu_timings)
{
    const auto generated_lpc = validate_vulkan_generated_analysis_inputs(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order);
#if LDCOMPRESS_HAVE_VULKAN
    auto result = impl_->run_validated(
        samples,
        plan,
        max_rice_partition_order,
        true,
        generated_lpc,
        false,
        gpu_timings);
    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(result.best_tasks),
        .best_rice_parameters = std::move(result.best_rice_parameters),
        .device_name = std::move(result.device_name),
    };
#else
    (void)generated_lpc;
    (void)gpu_timings;
    throw std::runtime_error("Vulkan support was not built");
#endif
}

}  // namespace ldcompress::vulkan_detail
