#include "opencl_analysis.h"

#include "opencl_analysis_internal.h"

#include "flac_native_writer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef LDCOMPRESS_HAVE_OPENCL
#define LDCOMPRESS_HAVE_OPENCL 0
#endif

#if LDCOMPRESS_HAVE_OPENCL
#if defined(__APPLE__)
#include <OpenCL/opencl.h>
#else
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>
#endif
#endif

#if LDCOMPRESS_HAVE_OPENCL
#define LDCOMPRESS_OPENCL_ONLY_USED
#else
#define LDCOMPRESS_OPENCL_ONLY_USED [[maybe_unused]]
#endif

namespace ldcompress::opencl_detail {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kGeneratedLpcBaseWindowCount = 2;
constexpr std::size_t kGeneratedWelchCandidateTargetCount = 2;
constexpr double kGeneratedTukeyTaperFraction = 0.5;
constexpr double kSubdivideTukey3TaperFraction = 0.5 / 3.0;
constexpr std::size_t kSubdivideTukey3WindowCount = 9;
constexpr unsigned kOpenClExactLeafMaxRicePartitionOrder = 6;
LDCOMPRESS_OPENCL_ONLY_USED constexpr unsigned kOpenClExactLeafRiceParameterCount = 15;
using Clock = std::chrono::steady_clock;

struct GeneratedLpcPrefixShape {
    std::size_t lpc_tasks_per_window = 0;
    std::size_t window_count = 0;
    std::size_t total_lpc_tasks = 0;
};

bool generated_profile_uses_order_guess(NativeAnalysisProfile profile)
{
    return profile != NativeAnalysisProfile::Exact;
}

bool generated_profile_uses_subdivide_tukey3(NativeAnalysisProfile profile)
{
    return profile == NativeAnalysisProfile::SubdivideTukey3MeanRice ||
        profile == NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice;
}

LDCOMPRESS_OPENCL_ONLY_USED bool analysis_profile_uses_mean_rice(NativeAnalysisProfile profile)
{
    return profile == NativeAnalysisProfile::OrderGuessMeanRice ||
        profile == NativeAnalysisProfile::OrderGuessMeanEstimateRice ||
        profile == NativeAnalysisProfile::SubdivideTukey3MeanRice ||
        profile == NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice;
}

LDCOMPRESS_OPENCL_ONLY_USED bool analysis_profile_uses_mean_estimated_size(
    NativeAnalysisProfile profile)
{
    return profile == NativeAnalysisProfile::OrderGuessMeanEstimateRice ||
        profile == NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice;
}

LDCOMPRESS_OPENCL_ONLY_USED bool uses_shifted_speed_profile(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::size_t lpc_tasks_per_window,
    std::size_t generated_window_count,
    unsigned max_rice_partition_order)
{
    return plan.analysis_profile == NativeAnalysisProfile::OrderGuessMeanEstimateRice &&
        generated_window_count == 3U &&
        lpc_tasks_per_window == 1U &&
        plan.max_lpc_order <= 12U &&
        max_rice_partition_order <= kOpenClExactLeafMaxRicePartitionOrder;
}

LDCOMPRESS_OPENCL_ONLY_USED std::int32_t opencl_analysis_profile_arg(
    NativeAnalysisProfile profile)
{
    switch (profile) {
    case NativeAnalysisProfile::Exact:
        return 0;
    case NativeAnalysisProfile::OrderGuessExactRice:
        return 1;
    case NativeAnalysisProfile::OrderGuessMeanRice:
        return 2;
    case NativeAnalysisProfile::OrderGuessMeanEstimateRice:
        return 4;
    case NativeAnalysisProfile::SubdivideTukey3MeanRice:
        return 3;
    case NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice:
        return 5;
    }
    return 0;
}

std::uint64_t elapsed_ns_since(Clock::time_point start)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

LDCOMPRESS_OPENCL_ONLY_USED void add_elapsed_ns(
    std::uint64_t& counter,
    Clock::time_point start)
{
    counter += elapsed_ns_since(start);
}

std::uint64_t absolute_i64(std::int64_t value)
{
    return value < 0
        ? static_cast<std::uint64_t>(-(value + 1)) + 1U
        : static_cast<std::uint64_t>(value);
}

std::int32_t checked_i32(std::uint64_t value, const char* name)
{
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error(std::string(name) + " exceeds FLACCL int32 task field");
    }
    return static_cast<std::int32_t>(value);
}

void validate_options(const OpenClMonoAnalysisTaskOptions& options)
{
    if (options.frame_samples == 0) {
        throw std::runtime_error("OpenCL mono analysis frame_samples must be positive");
    }
    if (options.bits_per_sample == 0 || options.bits_per_sample > 32) {
        throw std::runtime_error("OpenCL mono analysis bits_per_sample must be in 1..32");
    }
    if (options.max_lpc_order > kFlacClMaxOrder) {
        throw std::runtime_error("OpenCL mono analysis max_lpc_order exceeds FLACCL max order");
    }
    if (options.max_lpc_order >= options.frame_samples && options.max_lpc_order != 0) {
        throw std::runtime_error("OpenCL mono analysis max_lpc_order must be smaller than frame_samples");
    }
    if (options.min_fixed_order > 4 || options.max_fixed_order > 4) {
        throw std::runtime_error("OpenCL mono analysis fixed predictor order must be in 0..4");
    }
    if (options.min_fixed_order > options.max_fixed_order) {
        throw std::runtime_error("OpenCL mono analysis min_fixed_order exceeds max_fixed_order");
    }
    if (static_cast<std::uint64_t>(options.bits_per_sample) * options.frame_samples >
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error("OpenCL mono analysis raw task size exceeds int32");
    }

    const auto count = mono_analysis_tasks_per_frame(options);
    if (count == 0) {
        throw std::runtime_error("OpenCL mono analysis must contain at least one residual task");
    }
    if (count > kFlacClMaxOrder) {
        throw std::runtime_error("OpenCL mono analysis has more than 32 residual tasks per frame");
    }
}

std::size_t fixed_task_count(const OpenClMonoAnalysisTaskOptions& options)
{
    const auto largest_usable = static_cast<unsigned>(options.frame_samples - 1U);
    const auto max_order = options.max_fixed_order < largest_usable ? options.max_fixed_order : largest_usable;
    if (options.min_fixed_order > max_order) {
        return 0;
    }
    return static_cast<std::size_t>(max_order - options.min_fixed_order + 1U);
}

std::size_t generated_welch_candidate_count(const OpenClMonoAnalysisTaskOptions& options)
{
    if (options.max_lpc_order == 0) {
        return 0;
    }

    const auto base_lpc_tasks =
        static_cast<std::size_t>(options.max_lpc_order) * kGeneratedLpcBaseWindowCount;
    const auto non_lpc_tasks =
        (options.include_constant ? 1U : 0U) + fixed_task_count(options);
    const auto base_tasks = base_lpc_tasks + non_lpc_tasks;
    if (base_tasks >= kFlacClMaxOrder) {
        return 0;
    }

    const auto spare_tasks = kFlacClMaxOrder - base_tasks;
    return std::min<std::size_t>(
        {kGeneratedWelchCandidateTargetCount, options.max_lpc_order, spare_tasks});
}

std::size_t generated_lpc_task_count(const OpenClMonoAnalysisTaskOptions& options)
{
    if (options.max_lpc_order == 0) {
        return 0;
    }
    if (generated_profile_uses_subdivide_tukey3(options.analysis_profile)) {
        return kSubdivideTukey3WindowCount;
    }
    if (generated_profile_uses_order_guess(options.analysis_profile)) {
        return 3;
    }
    return (static_cast<std::size_t>(options.max_lpc_order) * kGeneratedLpcBaseWindowCount) +
        generated_welch_candidate_count(options);
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

LDCOMPRESS_OPENCL_ONLY_USED std::vector<float> make_generated_lpc_windows(
    std::size_t blocksize,
    std::size_t window_count,
    NativeAnalysisProfile profile)
{
    std::vector<float> windows(blocksize * window_count, 1.0f);
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

FlacClSubframeTask make_common_task(
    const OpenClMonoAnalysisTaskOptions& options,
    std::size_t frame_index,
    std::int32_t type,
    unsigned residual_order)
{
    const auto samples_offset = checked_i32(
        static_cast<std::uint64_t>(frame_index) * options.frame_samples,
        "OpenCL mono analysis samplesOffs");
    const auto raw_bits = checked_i32(
        static_cast<std::uint64_t>(options.bits_per_sample) * options.frame_samples,
        "OpenCL mono analysis size");

    FlacClSubframeTask task;
    task.data.residualOrder = checked_i32(residual_order, "OpenCL mono analysis residualOrder");
    task.data.samplesOffs = samples_offset;
    task.data.shift = 0;
    task.data.cbits = 0;
    task.data.size = raw_bits;
    task.data.type = type;
    task.data.obits = checked_i32(options.bits_per_sample, "OpenCL mono analysis obits");
    task.data.blocksize = checked_i32(options.frame_samples, "OpenCL mono analysis blocksize");
    task.data.coding_method = options.bits_per_sample > 16 ? 1 : 0;
    task.data.channel = 0;
    task.data.residualOffs = samples_offset;
    task.data.wbits = 0;
    task.data.abits = task.data.obits;
    task.data.porder = 0;
    task.data.headerLen = 0;
    task.data.encodingOffset = 0;
    return task;
}

void populate_fixed_coefficients(FlacClSubframeTask& task, unsigned order)
{
    switch (order) {
    case 0:
        break;
    case 1:
        task.coefs[0] = 1;
        break;
    case 2:
        task.coefs[1] = 2;
        task.coefs[0] = -1;
        break;
    case 3:
        task.coefs[2] = 3;
        task.coefs[1] = -3;
        task.coefs[0] = 1;
        break;
    case 4:
        task.coefs[3] = 4;
        task.coefs[2] = -6;
        task.coefs[1] = 4;
        task.coefs[0] = -1;
        break;
    default:
        throw std::runtime_error("invalid FLAC fixed predictor order");
    }
}

constexpr std::size_t kOpenClAnalysisMaxBlockSize = 8192;
constexpr std::size_t kOpenClAnalysisBitsPerSample = 16;
LDCOMPRESS_OPENCL_ONLY_USED constexpr unsigned kExactMaxRicePartitionOrder = 8;
constexpr unsigned kExactMaxRiceParameter = 14;
LDCOMPRESS_OPENCL_ONLY_USED constexpr std::size_t kOpenClExactWorkgroupSize = 64;
LDCOMPRESS_OPENCL_ONLY_USED constexpr std::size_t kOpenClAutocorWorkgroupSize = 64;

void validate_best_method_plan(const OpenClMonoAnalysisTaskPlan& plan)
{
    if (plan.residual_tasks_per_frame == 0 || plan.estimate_tasks_per_frame == 0) {
        throw std::runtime_error("OpenCL mono analysis plan has no tasks per frame");
    }
    if (plan.residual_tasks.empty() || plan.selected_tasks.empty()) {
        throw std::runtime_error("OpenCL mono analysis plan has no task data");
    }
    if (plan.estimate_tasks_per_frame != plan.residual_tasks_per_frame) {
        throw std::runtime_error("OpenCL mono analysis best-method smoke supports full mono selection only");
    }
    if (plan.selected_tasks.size() % plan.estimate_tasks_per_frame != 0) {
        throw std::runtime_error("OpenCL mono analysis selected task count is not frame-aligned");
    }
    if (plan.residual_tasks.size() % plan.residual_tasks_per_frame != 0) {
        throw std::runtime_error("OpenCL mono analysis residual task count is not frame-aligned");
    }
    if (plan.selected_tasks.size() / plan.estimate_tasks_per_frame !=
        plan.residual_tasks.size() / plan.residual_tasks_per_frame) {
        throw std::runtime_error("OpenCL mono analysis selected/residual frame counts differ");
    }
    if (plan.estimate_tasks_per_frame >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error("OpenCL mono analysis task count exceeds OpenCL int argument");
    }
    for (const auto selected : plan.selected_tasks) {
        if (selected < 0 ||
            static_cast<std::size_t>(selected) >= plan.residual_tasks.size()) {
            throw std::runtime_error("OpenCL mono analysis selected task index is out of range");
        }
    }

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto& first_task = plan.residual_tasks.at(task_base);
        for (std::size_t i = 1; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks.at(task_base + i);
            if (task.data.samplesOffs != first_task.data.samplesOffs ||
                task.data.blocksize != first_task.data.blocksize ||
                task.data.obits != first_task.data.obits) {
                throw std::runtime_error("OpenCL mono analysis frame task group mixes sample ranges");
            }
        }

        const auto selected_base = frame * plan.estimate_tasks_per_frame;
        for (std::size_t i = 0; i < plan.estimate_tasks_per_frame; ++i) {
            const auto selected = static_cast<std::size_t>(
                plan.selected_tasks.at(selected_base + i));
            if (selected < task_base ||
                selected >= task_base + plan.residual_tasks_per_frame) {
                throw std::runtime_error("OpenCL mono analysis selected task crosses frame group");
            }
        }
    }
}

std::size_t mono_plan_frame_count(const OpenClMonoAnalysisTaskPlan& plan)
{
    validate_best_method_plan(plan);
    return plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
}

GeneratedLpcPrefixShape generated_lpc_prefix_shape(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::size_t frame)
{
    const auto task_base = frame * plan.residual_tasks_per_frame;
    std::size_t total_lpc_tasks = 0;
    while (total_lpc_tasks < plan.residual_tasks_per_frame &&
        plan.residual_tasks[task_base + total_lpc_tasks].data.type == kFlacClSubframeLpc) {
        ++total_lpc_tasks;
    }

    if (total_lpc_tasks == 0) {
        throw std::runtime_error("OpenCL mono generated analysis requires an LPC task prefix");
    }

    std::size_t lpc_tasks_per_window = 0;
    while (lpc_tasks_per_window < total_lpc_tasks &&
        plan.residual_tasks[task_base + lpc_tasks_per_window].data.residualOrder ==
            static_cast<std::int32_t>(lpc_tasks_per_window + 1U)) {
        ++lpc_tasks_per_window;
    }
    if (lpc_tasks_per_window == 0 || lpc_tasks_per_window > kFlacClMaxOrder) {
        throw std::runtime_error("OpenCL mono generated analysis has invalid LPC tasks per window");
    }
    const auto window_count =
        (total_lpc_tasks + lpc_tasks_per_window - 1U) / lpc_tasks_per_window;
    for (std::size_t window = 0; window < window_count; ++window) {
        const auto window_base = task_base + window * lpc_tasks_per_window;
        const auto remaining = total_lpc_tasks - (window * lpc_tasks_per_window);
        const auto slots_this_window = std::min(lpc_tasks_per_window, remaining);
        for (std::size_t slot = 0; slot < slots_this_window; ++slot) {
            const auto& task = plan.residual_tasks[window_base + slot];
            if (task.data.type != kFlacClSubframeLpc ||
                task.data.residualOrder <= 0 ||
                task.data.residualOrder > static_cast<std::int32_t>(kFlacClMaxOrder)) {
                throw std::runtime_error("OpenCL mono generated analysis LPC window group is invalid");
            }
            if (slots_this_window == lpc_tasks_per_window &&
                task.data.residualOrder != static_cast<std::int32_t>(slot + 1U)) {
                throw std::runtime_error("OpenCL mono generated analysis full LPC window group is not sequential");
            }
        }
    }

    return GeneratedLpcPrefixShape {
        .lpc_tasks_per_window = lpc_tasks_per_window,
        .window_count = window_count,
        .total_lpc_tasks = total_lpc_tasks,
    };
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

LDCOMPRESS_OPENCL_ONLY_USED void validate_fixed_constant_analysis_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan)
{
    const auto frame_count = mono_plan_frame_count(plan);
    if (frame_count == 0) {
        throw std::runtime_error("OpenCL mono fixed/constant analysis has no frames");
    }

    for (const auto& task : plan.residual_tasks) {
        if (task.data.type != kFlacClSubframeConstant &&
            task.data.type != kFlacClSubframeFixed) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis received non-fixed task");
        }
        if (task.data.type == kFlacClSubframeFixed &&
            (task.data.residualOrder < 0 || task.data.residualOrder > 4)) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis received invalid fixed order");
        }
        if (task.data.obits != static_cast<std::int32_t>(kOpenClAnalysisBitsPerSample)) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis currently supports 16-bit tasks only");
        }
        if (task.data.blocksize <= 0 ||
            static_cast<std::size_t>(task.data.blocksize) > kOpenClAnalysisMaxBlockSize) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis block size is unsupported");
        }
        if (task.data.type == kFlacClSubframeFixed &&
            task.data.residualOrder >= task.data.blocksize) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis fixed order exceeds block size");
        }
        if (task.data.samplesOffs < 0 ||
            static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
            static_cast<std::size_t>(task.data.blocksize) >
                samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis task samples are out of range");
        }
        if (task.data.shift < 0 || task.data.shift > 30) {
            throw std::runtime_error("OpenCL mono fixed/constant analysis task shift is unsupported");
        }
    }
}

LDCOMPRESS_OPENCL_ONLY_USED void validate_lpc_analysis_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan)
{
    const auto frame_count = mono_plan_frame_count(plan);
    if (frame_count == 0) {
        throw std::runtime_error("OpenCL mono LPC analysis has no frames");
    }

    for (const auto& task : plan.residual_tasks) {
        if (task.data.type != kFlacClSubframeConstant &&
            task.data.type != kFlacClSubframeFixed &&
            task.data.type != kFlacClSubframeLpc) {
            throw std::runtime_error("OpenCL mono LPC analysis received unsupported task");
        }
        if (task.data.type == kFlacClSubframeLpc &&
            (task.data.residualOrder <= 0 ||
                task.data.residualOrder > static_cast<std::int32_t>(kFlacClMaxOrder))) {
            throw std::runtime_error("OpenCL mono LPC analysis received invalid LPC order");
        }
        if (task.data.type == kFlacClSubframeFixed &&
            (task.data.residualOrder < 0 || task.data.residualOrder > 4)) {
            throw std::runtime_error("OpenCL mono LPC analysis received invalid fixed order");
        }
        if (task.data.obits != static_cast<std::int32_t>(kOpenClAnalysisBitsPerSample)) {
            throw std::runtime_error("OpenCL mono LPC analysis currently supports 16-bit tasks only");
        }
        if (task.data.blocksize <= 0 ||
            static_cast<std::size_t>(task.data.blocksize) > kOpenClAnalysisMaxBlockSize) {
            throw std::runtime_error("OpenCL mono LPC analysis block size is unsupported");
        }
        if ((task.data.type == kFlacClSubframeFixed ||
                task.data.type == kFlacClSubframeLpc) &&
            task.data.residualOrder >= task.data.blocksize) {
            throw std::runtime_error("OpenCL mono LPC analysis predictor order exceeds block size");
        }
        if (task.data.samplesOffs < 0 ||
            static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
            static_cast<std::size_t>(task.data.blocksize) >
                samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
            throw std::runtime_error("OpenCL mono LPC analysis task samples are out of range");
        }
        if (task.data.type == kFlacClSubframeLpc) {
            if (task.data.shift < 0 || task.data.shift > 15) {
                throw std::runtime_error("OpenCL mono LPC analysis task shift is unsupported");
            }
            if (task.data.cbits <= 0 || task.data.cbits > 15) {
                throw std::runtime_error("OpenCL mono LPC analysis coefficient precision is unsupported");
            }
            for (int i = 0; i < task.data.residualOrder; ++i) {
                if (!signed_value_fits_bits(task.coefs[static_cast<std::size_t>(i)],
                        static_cast<unsigned>(task.data.cbits))) {
                    throw std::runtime_error(
                        "OpenCL mono LPC analysis coefficient does not fit precision");
                }
            }
        } else if (task.data.shift < 0 || task.data.shift > 30) {
            throw std::runtime_error("OpenCL mono LPC analysis task shift is unsupported");
        }
    }
}

LDCOMPRESS_OPENCL_ONLY_USED void validate_lpc_generation_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision)
{
    const auto frame_count = mono_plan_frame_count(plan);
    if (frame_count == 0) {
        throw std::runtime_error("OpenCL mono generated LPC analysis has no frames");
    }
    if (plan.residual_tasks_per_frame > kFlacClMaxOrder) {
        throw std::runtime_error("OpenCL mono generated LPC analysis has too many LPC tasks per frame");
    }
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("OpenCL mono generated LPC analysis coefficient precision must be 1..15");
    }

    std::optional<std::int32_t> blocksize;
    for (const auto& task : plan.residual_tasks) {
        if (task.data.type != kFlacClSubframeLpc) {
            throw std::runtime_error("OpenCL mono generated LPC analysis received non-LPC task");
        }
        if (task.data.residualOrder <= 0 ||
            task.data.residualOrder > static_cast<std::int32_t>(kFlacClMaxOrder)) {
            throw std::runtime_error("OpenCL mono generated LPC analysis received invalid LPC order");
        }
        if (task.data.obits != static_cast<std::int32_t>(kOpenClAnalysisBitsPerSample)) {
            throw std::runtime_error("OpenCL mono generated LPC analysis currently supports 16-bit tasks only");
        }
        if (task.data.blocksize <= 0 ||
            static_cast<std::size_t>(task.data.blocksize) > kOpenClAnalysisMaxBlockSize) {
            throw std::runtime_error("OpenCL mono generated LPC analysis block size is unsupported");
        }
        if (task.data.residualOrder >= task.data.blocksize) {
            throw std::runtime_error("OpenCL mono generated LPC analysis LPC order exceeds block size");
        }
        if (task.data.samplesOffs < 0 ||
            static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
            static_cast<std::size_t>(task.data.blocksize) >
                samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
            throw std::runtime_error("OpenCL mono generated LPC analysis task samples are out of range");
        }
        if (!blocksize.has_value()) {
            blocksize = task.data.blocksize;
        } else if (*blocksize != task.data.blocksize) {
            throw std::runtime_error("OpenCL mono generated LPC analysis requires one block size per launch");
        }
    }
}

LDCOMPRESS_OPENCL_ONLY_USED GeneratedLpcPrefixShape validate_generated_analysis_inputs(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision)
{
    const auto frame_count = mono_plan_frame_count(plan);
    if (frame_count == 0) {
        throw std::runtime_error("OpenCL mono generated analysis has no frames");
    }
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("OpenCL mono generated analysis coefficient precision must be 1..15");
    }

    std::optional<GeneratedLpcPrefixShape> lpc_shape;
    std::optional<std::int32_t> blocksize;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto frame_lpc_shape = generated_lpc_prefix_shape(plan, frame);
        if (!lpc_shape.has_value()) {
            lpc_shape = frame_lpc_shape;
        } else if (lpc_shape->lpc_tasks_per_window != frame_lpc_shape.lpc_tasks_per_window ||
            lpc_shape->window_count != frame_lpc_shape.window_count ||
            lpc_shape->total_lpc_tasks != frame_lpc_shape.total_lpc_tasks) {
            throw std::runtime_error("OpenCL mono generated analysis LPC prefix differs by frame");
        }

        for (std::size_t i = 0; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks[task_base + i];
            if (task.data.obits != static_cast<std::int32_t>(kOpenClAnalysisBitsPerSample)) {
                throw std::runtime_error("OpenCL mono generated analysis currently supports 16-bit tasks only");
            }
            if (task.data.blocksize <= 0 ||
                static_cast<std::size_t>(task.data.blocksize) > kOpenClAnalysisMaxBlockSize) {
                throw std::runtime_error("OpenCL mono generated analysis block size is unsupported");
            }
            if (task.data.samplesOffs < 0 ||
                static_cast<std::size_t>(task.data.samplesOffs) > samples.size() ||
                static_cast<std::size_t>(task.data.blocksize) >
                    samples.size() - static_cast<std::size_t>(task.data.samplesOffs)) {
                throw std::runtime_error("OpenCL mono generated analysis task samples are out of range");
            }
            if (!blocksize.has_value()) {
                blocksize = task.data.blocksize;
            } else if (*blocksize != task.data.blocksize) {
                throw std::runtime_error("OpenCL mono generated analysis requires one block size per launch");
            }

            if (i < frame_lpc_shape.total_lpc_tasks) {
                if (task.data.residualOrder >= task.data.blocksize) {
                    throw std::runtime_error("OpenCL mono generated analysis LPC order exceeds block size");
                }
            } else if (task.data.type == kFlacClSubframeLpc) {
                throw std::runtime_error("OpenCL mono generated analysis received non-prefix LPC task");
            } else if (task.data.type == kFlacClSubframeFixed) {
                if (task.data.residualOrder < 0 || task.data.residualOrder > 4) {
                    throw std::runtime_error("OpenCL mono generated analysis received invalid fixed order");
                }
                if (task.data.residualOrder >= task.data.blocksize) {
                    throw std::runtime_error("OpenCL mono generated analysis fixed order exceeds block size");
                }
                if (task.data.shift < 0 || task.data.shift > 30) {
                    throw std::runtime_error("OpenCL mono generated analysis fixed task shift is unsupported");
                }
            } else if (task.data.type != kFlacClSubframeConstant) {
                throw std::runtime_error("OpenCL mono generated analysis received unsupported task type");
            }
        }
    }

    return *lpc_shape;
}

void validate_sample_range(std::int32_t sample, unsigned bits_per_sample)
{
    const auto min_value = -(std::int64_t {1} << (bits_per_sample - 1U));
    const auto max_value = (std::int64_t {1} << (bits_per_sample - 1U)) - 1;
    if (sample < min_value || sample > max_value) {
        throw std::runtime_error("OpenCL exact analysis sample is outside the selected bit depth");
    }
}

unsigned sample_trailing_zero_bits(std::int32_t sample, unsigned bits_per_sample)
{
    if (sample == 0) {
        return bits_per_sample;
    }

    const auto mask = (std::uint64_t {1} << bits_per_sample) - 1U;
    auto raw = static_cast<std::uint64_t>(sample) & mask;

    unsigned zeros = 0;
    while ((raw & 1U) == 0U && zeros < bits_per_sample) {
        raw >>= 1U;
        ++zeros;
    }
    return zeros;
}

unsigned common_wasted_bits(
    const std::vector<std::int32_t>& samples,
    std::size_t offset,
    std::size_t count,
    unsigned bits_per_sample)
{
    unsigned wasted_bits = bits_per_sample;
    for (std::size_t i = 0; i < count; ++i) {
        const auto sample = samples.at(offset + i);
        validate_sample_range(sample, bits_per_sample);
        wasted_bits = std::min(wasted_bits,
            sample_trailing_zero_bits(sample, bits_per_sample));
        if (wasted_bits == 0) {
            return 0;
        }
    }
    return std::min(wasted_bits, bits_per_sample - 1U);
}

std::vector<std::int32_t> shifted_frame_samples(
    const std::vector<std::int32_t>& samples,
    std::size_t offset,
    std::size_t count,
    unsigned wasted_bits)
{
    std::vector<std::int32_t> shifted;
    shifted.reserve(count);
    if (wasted_bits == 0) {
        shifted.insert(shifted.end(), samples.begin() + static_cast<std::ptrdiff_t>(offset),
            samples.begin() + static_cast<std::ptrdiff_t>(offset + count));
        return shifted;
    }

    const auto divisor = std::int64_t {1} << wasted_bits;
    for (std::size_t i = 0; i < count; ++i) {
        shifted.push_back(static_cast<std::int32_t>(
            static_cast<std::int64_t>(samples.at(offset + i)) / divisor));
    }
    return shifted;
}

unsigned guess_fixed_predictor_order_from_samples(
    const std::int32_t* samples,
    std::size_t count,
    unsigned max_fixed_order)
{
    const auto max_order = count > 1
        ? std::min<std::size_t>(max_fixed_order, count - 1U)
        : 0;
    std::array<std::uint64_t, 5> sums {};
    for (std::size_t i = 0; i < count; ++i) {
        const auto current = static_cast<std::int64_t>(samples[i]);
        sums[0] += absolute_i64(current);
        if (max_order >= 1U && i >= 1U) {
            sums[1] += absolute_i64(current - samples[i - 1U]);
        }
        if (max_order >= 2U && i >= 2U) {
            sums[2] += absolute_i64(
                current - (2LL * samples[i - 1U]) + samples[i - 2U]);
        }
        if (max_order >= 3U && i >= 3U) {
            sums[3] += absolute_i64(
                current -
                (3LL * samples[i - 1U]) +
                (3LL * samples[i - 2U]) -
                samples[i - 3U]);
        }
        if (max_order >= 4U && i >= 4U) {
            sums[4] += absolute_i64(
                current -
                (4LL * samples[i - 1U]) +
                (6LL * samples[i - 2U]) -
                (4LL * samples[i - 3U]) +
                samples[i - 4U]);
        }
    }

    unsigned best_order = 0;
    auto best_sum = sums[0];
    for (unsigned order = 1; order <= max_order; ++order) {
        if (sums[order] < best_sum) {
            best_sum = sums[order];
            best_order = order;
        }
    }
    return best_order;
}

unsigned guess_fixed_predictor_order(
    const std::vector<std::int32_t>& shifted_samples,
    unsigned max_fixed_order)
{
    return guess_fixed_predictor_order_from_samples(
        shifted_samples.data(),
        shifted_samples.size(),
        max_fixed_order);
}

unsigned guess_fixed_predictor_order_for_frame(
    const std::vector<std::int32_t>& samples,
    std::size_t offset,
    std::size_t count,
    unsigned max_fixed_order)
{
    if (offset > samples.size() || count > samples.size() - offset) {
        throw std::runtime_error("OpenCL profile fixed-order guess samples are out of range");
    }
    return guess_fixed_predictor_order_from_samples(
        samples.data() + offset,
        count,
        max_fixed_order);
}

void mark_task_pruned(FlacClSubframeTask& task)
{
    task.data.size = std::numeric_limits<std::int32_t>::max();
}

#if LDCOMPRESS_HAVE_OPENCL

constexpr cl_int kPlatformNotFound = -1001;

const char* mono_analysis_kernel_source()
{
    return R"CLC(
/*
 * Portions derived from CUETools.FLACCL: FLAC audio encoder using OpenCL
 * Copyright (c) 2010-2022 Gregory S. Chudov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Local modifications for ld-compress-ng, 2026-07-04:
 * - Reduced to mono analysis kernels used by the native LaserDisc RF encoder.
 * - Uses the FLACCLSubframeTask ABI but omits stereo and bitstream output
 *   kernels.
 * - Adds a mono LPC autocorrelation/coefficient-generation path with
 *   rectangular and Tukey-window candidates that feeds the exact residual
 *   analyzer without FLACCL's order-pruning pass.
 * - Keeps the CPU-style one-work-item estimate path and adds exact fixed/LPC
 *   Rice partition analysis for scalar parity testing.
 */

#pragma OPENCL FP_CONTRACT OFF

#define FLACCL_CPU
#define MAX_ORDER 32
#define GROUP_SIZE 1
#define BITS_PER_SAMPLE 16
#define MAX_BLOCKSIZE 8192

#if BITS_PER_SAMPLE > 16
#define MAX_RICE_PARAM 30
#define RICE_PARAM_BITS 5
#else
#define MAX_RICE_PARAM 14
#define RICE_PARAM_BITS 4
#endif
#define EXACT_WORKGROUP_SIZE 64
#define AUTOCOR_WORKGROUP_SIZE 64
#define MAX_RICE_PARTITION_COUNT 256
#define EXACT_LEAF_MAX_RICE_PARTITION_ORDER 6
#define EXACT_LEAF_RICE_PARAMETER_COUNT 15
#define ANALYSIS_PROFILE_EXACT 0
#define ANALYSIS_PROFILE_ORDER_GUESS_EXACT_RICE 1
#define ANALYSIS_PROFILE_ORDER_GUESS_MEAN_RICE 2
#define ANALYSIS_PROFILE_SUBDIVIDE_TUKEY3_MEAN_RICE 3
#define ANALYSIS_PROFILE_ORDER_GUESS_MEAN_ESTIMATE_RICE 4
#define ANALYSIS_PROFILE_SUBDIVIDE_TUKEY3_MEAN_ESTIMATE_RICE 5

typedef enum
{
    Constant = 0,
    Verbatim = 1,
    Fixed = 8,
    LPC = 32
} SubframeType;

typedef struct
{
    int residualOrder;
    int samplesOffs;
    int shift;
    int cbits;
    int size;
    int type;
    int obits;
    int blocksize;
    int coding_method;
    int channel;
    int residualOffs;
    int wbits;
    int abits;
    int porder;
    int headerLen;
    int encodingOffset;
} FLACCLSubframeData;

typedef struct
{
    FLACCLSubframeData data;
    int coefs[32];
} FLACCLSubframeTask;

#define __ffs(a) (32 - clz((a) & (-(a))))

__kernel __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressFindWastedBits(
    __global FLACCLSubframeTask* tasks,
    __global const int* samples,
    int tasksPerChannel,
    __global int* shiftedSamples,
    int writeShiftedSamples)
{
    __global FLACCLSubframeTask* ptask = &tasks[get_group_id(0) * tasksPerChannel];
    int w_or = 0;
    int a_or = 0;

    for (int pos = 0; pos < ptask->data.blocksize; pos++)
    {
        const int smp = samples[ptask->data.samplesOffs + pos];
        w_or |= smp;
        a_or |= smp ^ (smp >> 31);
    }

    const int w = w_or == 0 ? ptask->data.obits - 1 : max(0, __ffs(w_or) - 1);
    const int a = a_or == 0 ? 1 : max(1, 32 - clz(a_or) - w);

    for (int i = 0; i < tasksPerChannel; i++)
    {
        ptask[i].data.wbits = w;
        ptask[i].data.abits = a;
    }

    if (writeShiftedSamples)
    {
        const int offset = ptask->data.samplesOffs;
        for (int pos = 0; pos < ptask->data.blocksize; pos++)
        {
            const int smp = samples[offset + pos];
            shiftedSamples[offset + pos] = w == 0 ? smp : smp / (1 << w);
        }
    }
}

long ldcompressShiftedSample(__global const int* data, int pos, int wbits);

float ldcompressReduceSumFloat(float value, __local float* scratch)
{
    const int lane = (int)get_local_id(0);
    scratch[lane] = value;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int stride = AUTOCOR_WORKGROUP_SIZE >> 1; stride > 0; stride >>= 1)
    {
        if (lane < stride)
            scratch[lane] += scratch[lane + stride];
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    return scratch[0];
}

__kernel __attribute__((reqd_work_group_size(AUTOCOR_WORKGROUP_SIZE, 1, 1)))
void ldcompressComputeAutocor(
    __global float* output,
    __global const int* samples,
    __global const float* window,
    __global FLACCLSubframeTask* tasks,
    int tasksPerFrame,
    int samplesAlreadyShifted)
{
    __local float reduceScratch[AUTOCOR_WORKGROUP_SIZE];
    const int frame = (int)get_group_id(0);
    const int windowIndex = (int)get_group_id(1);
    const int lag = (int)get_group_id(2);
    FLACCLSubframeData task = tasks[frame * tasksPerFrame].data;
    const int blocksize = task.blocksize;
    const int windowOffset = windowIndex * blocksize;
    const int outputOffset =
        ((frame * (int)get_num_groups(1) + windowIndex) * (MAX_ORDER + 1)) + lag;

    float sum = 0.0f;
    if (samplesAlreadyShifted)
    {
        for (int pos = lag + (int)get_local_id(0);
             pos < blocksize;
             pos += AUTOCOR_WORKGROUP_SIZE)
        {
            const float sample0 =
                (float)samples[task.samplesOffs + pos] * window[windowOffset + pos];
            const float sample1 =
                (float)samples[task.samplesOffs + pos - lag] *
                window[windowOffset + pos - lag];
            sum += sample0 * sample1;
        }
    }
    else
    {
        for (int pos = lag + (int)get_local_id(0);
             pos < blocksize;
             pos += AUTOCOR_WORKGROUP_SIZE)
        {
            const float sample0 =
                (float)ldcompressShiftedSample(samples, task.samplesOffs + pos, task.wbits) *
                window[windowOffset + pos];
            const float sample1 =
                (float)ldcompressShiftedSample(samples, task.samplesOffs + pos - lag, task.wbits) *
                window[windowOffset + pos - lag];
            sum += sample0 * sample1;
        }
    }

    const float total = ldcompressReduceSumFloat(sum, reduceScratch);
    if (get_local_id(0) == 0)
        output[outputOffset] = total;
}

__kernel __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressComputeLpc(
    __global const float* autocor,
    __global float* lpcs,
    int windowCount,
    int maxLpcOrder)
{
    const int frame = get_group_id(0);
    const int window = get_group_id(1);
    const int lpcOffset =
        (frame * windowCount + window) * (MAX_ORDER + 1) * 32;
    const int autocorOffset =
        (frame * windowCount + window) * (MAX_ORDER + 1);
    __global const float* autoc = &autocor[autocorOffset];

    float ldr[32];
    float gen0[32];
    float gen1[32];
    float err[32];
    const int orderLimit = clamp(maxLpcOrder, 1, MAX_ORDER);

    for (int i = 0; i < orderLimit; i++)
    {
        gen0[i] = autoc[i + 1];
        gen1[i] = autoc[i + 1];
        ldr[i] = 0.0f;
        err[i] = 0.0f;
    }

    float error = autoc[0];
    for (int order = 0; order < orderLimit; order++)
    {
        float reflection = 0.0f;
        if (error > 0.0f)
            reflection = -gen1[0] / error;
        if (!isfinite(reflection))
            reflection = 0.0f;

        error *= 1.0f - (reflection * reflection);
        if (!isfinite(error) || error < 0.0f)
            error = 0.0f;

        for (int j = 0; j < orderLimit - 1 - order; j++)
        {
            const float nextGen1 = gen1[j + 1] + (reflection * gen0[j]);
            gen0[j] = gen1[j + 1] * reflection + gen0[j];
            gen1[j] = nextGen1;
        }

        err[order] = error;

        ldr[order] = reflection;
        for (int j = 0; j < order / 2; j++)
        {
            const float tmp = ldr[j];
            ldr[j] += reflection * ldr[order - 1 - j];
            ldr[order - 1 - j] += reflection * tmp;
        }
        if ((order & 1) != 0)
            ldr[order / 2] += ldr[order / 2] * reflection;

        for (int j = 0; j <= order; j++)
            lpcs[lpcOffset + order * 32 + j] = -ldr[order - j];
    }

    for (int j = 0; j < orderLimit; j++)
        lpcs[lpcOffset + MAX_ORDER * 32 + j] = err[j];
}

__kernel __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressQuantizeLpcOrders(
    __global FLACCLSubframeTask* tasks,
    __global const float* lpcs,
    int tasksPerFrame,
    int lpcTasksPerWindow,
    int totalLpcTasks,
    int coefficientPrecision,
    int maxLpcOrder,
    int analysisProfile)
{
    const int frame = get_group_id(0);
    const int window = get_group_id(1);
    const int lpcOffset =
        (frame * get_num_groups(1) + window) * (MAX_ORDER + 1) * 32;
    const int windowTaskOffset = window * lpcTasksPerWindow;
    const int slotsThisWindow = min(lpcTasksPerWindow, totalLpcTasks - windowTaskOffset);

    int guessedOrder = 1;
    if (analysisProfile != ANALYSIS_PROFILE_EXACT)
    {
        const int maxOrder = clamp(maxLpcOrder, 1, MAX_ORDER);
        const float errorScale = 0.5f / (float)tasks[frame * tasksPerFrame].data.blocksize;
        float bestBits = 3.402823466e+38f;
        const int overheadBitsPerOrder =
            max(1, tasks[frame * tasksPerFrame].data.obits -
                tasks[frame * tasksPerFrame].data.wbits) +
            clamp(coefficientPrecision, 1, 15);
        for (int order = 1; order <= maxOrder; order++)
        {
            const float error = lpcs[lpcOffset + MAX_ORDER * 32 + order - 1];
            float bitsPerResidual = 0.0f;
            if (error > 0.0f)
                bitsPerResidual = max(0.0f, 0.5f * log2(errorScale * error));
            else if (error < 0.0f)
                bitsPerResidual = 8.507058665e+37f;
            const float residualSamples =
                max(0.0f, (float)(tasks[frame * tasksPerFrame].data.blocksize - order));
            const float bits =
                (bitsPerResidual * residualSamples) +
                (float)(order * overheadBitsPerOrder);
            if (bits < bestBits)
            {
                bestBits = bits;
                guessedOrder = order;
            }
        }
    }

    for (int slot = 0; slot < slotsThisWindow; slot++)
    {
        const int taskNo = frame * tasksPerFrame + windowTaskOffset + slot;
        FLACCLSubframeTask task = tasks[taskNo];
        int order = task.data.residualOrder;
        if (analysisProfile != ANALYSIS_PROFILE_EXACT)
        {
            if (lpcTasksPerWindow == 1)
            {
                order = guessedOrder;
                task.data.residualOrder = guessedOrder;
            }
            else if (order != guessedOrder)
            {
                task.data.size = 0x7fffffff;
                tasks[taskNo] = task;
                continue;
            }
        }

        int maxCoefficient = 0;
        for (int i = 0; i < order; i++)
        {
            const float lpc = lpcs[lpcOffset + (order - 1) * 32 + i];
            const int coefficient = convert_int_rte(lpc * (float)(1 << 15));
            maxCoefficient |= coefficient ^ (coefficient >> 31);
        }

        const int desiredBits = clamp(coefficientPrecision, 1, 15);
        int shift = 0;
        if (maxCoefficient != 0)
            shift = clamp(clz(maxCoefficient) - 18 + desiredBits, 0, 15);
        const int limit = (1 << (desiredBits - 1)) - 1;

        int actualMaxCoefficient = 0;
        for (int i = 0; i < MAX_ORDER; i++)
            task.coefs[i] = 0;
        for (int i = 0; i < order; i++)
        {
            const float lpc = lpcs[lpcOffset + (order - 1) * 32 + i];
            const int coefficient = clamp(
                convert_int_rte(lpc * (float)(1 << shift)), -limit, limit);
            actualMaxCoefficient |= coefficient ^ (coefficient >> 31);
            task.coefs[i] = coefficient;
        }

        task.data.residualOrder = order;
        task.data.shift = shift;
        task.data.cbits = actualMaxCoefficient == 0
            ? 1
            : 1 + 32 - clz(actualMaxCoefficient);
        tasks[taskNo] = task;
    }
}

#define TEMPBLOCK 512
#define TEMPBLOCK1 TEMPBLOCK

__kernel __attribute__((vec_type_hint(int4))) __attribute__((reqd_work_group_size(1, 1, 1)))
void ldcompressEstimateResidual(
    __global const int* samples,
    __global const int* selectedTasks,
    __global FLACCLSubframeTask* tasks)
{
    const int selectedTask = selectedTasks[get_group_id(0)];
    FLACCLSubframeTask task = tasks[selectedTask];
    const int ro = task.data.residualOrder;
    const int bs = task.data.blocksize;
#define ERPARTS (MAX_BLOCKSIZE >> 6)
    float len[ERPARTS];

    __global const int* data = &samples[task.data.samplesOffs];
    for (int i = 0; i < ERPARTS; i++)
        len[i] = 0.0f;

    if (ro <= 4)
    {
        float fcoef[4];
        for (int tid = 0; tid < 4; tid++)
            fcoef[tid] = tid + ro - 4 < 0 ? 0.0f : -((float)task.coefs[tid + ro - 4]) / (1 << task.data.shift);
        float4 fc0 = vload4(0, &fcoef[0]);
        float fdata[4];
        for (int pos = 0; pos < 4; pos++)
            fdata[pos] = pos + ro - 4 < 0 ? 0.0f : (float)(data[pos + ro - 4] >> task.data.wbits);
        float4 fd0 = vload4(0, &fdata[0]);
        for (int pos = ro; pos < bs; pos++)
        {
            float4 sum4 = fc0 * fd0;
            float2 sum2 = sum4.s01 + sum4.s23;
            fd0 = fd0.s1230;
            fd0.s3 = (float)(data[pos] >> task.data.wbits);
            len[pos >> 6] += fabs(fd0.s3 + (sum2.x + sum2.y));
        }
    }
    else if (ro <= 8)
    {
        float fcoef[8];
        for (int tid = 0; tid < 8; tid++)
            fcoef[tid] = tid + ro - 8 < 0 ? 0.0f : -((float)task.coefs[tid + ro - 8]) / (1 << task.data.shift);
        float8 fc0 = vload8(0, &fcoef[0]);
        float fdata[8];
        for (int pos = 0; pos < 8; pos++)
            fdata[pos] = pos + ro - 8 < 0 ? 0.0f : (float)(data[pos + ro - 8] >> task.data.wbits);
        float8 fd0 = vload8(0, &fdata[0]);
        for (int pos = ro; pos < bs; pos++)
        {
            float8 sum8 = fc0 * fd0;
            float4 sum4 = sum8.s0123 + sum8.s4567;
            float2 sum2 = sum4.s01 + sum4.s23;
            fd0 = fd0.s12345670;
            fd0.s7 = (float)(data[pos] >> task.data.wbits);
            len[pos >> 6] += fabs(fd0.s7 + (sum2.x + sum2.y));
        }
    }
    else if (ro <= 12)
    {
        float fcoef[12];
        for (int tid = 0; tid < 12; tid++)
            fcoef[tid] = tid + ro - 12 >= 0 ? -((float)task.coefs[tid + ro - 12]) / (1 << task.data.shift) : 0.0f;
        float4 fc0 = vload4(0, &fcoef[0]);
        float4 fc1 = vload4(1, &fcoef[0]);
        float4 fc2 = vload4(2, &fcoef[0]);
        float fdata[12];
        for (int pos = 0; pos < 12; pos++)
            fdata[pos] = pos + ro - 12 < 0 ? 0.0f : (float)(data[pos + ro - 12] >> task.data.wbits);
        float4 fd0 = vload4(0, &fdata[0]);
        float4 fd1 = vload4(1, &fdata[0]);
        float4 fd2 = vload4(2, &fdata[0]);
        for (int pos = ro; pos < bs; pos++)
        {
            float4 sum4 = fc0 * fd0 + fc1 * fd1 + fc2 * fd2;
            float2 sum2 = sum4.s01 + sum4.s23;
            fd0 = fd0.s1230;
            fd1 = fd1.s1230;
            fd2 = fd2.s1230;
            fd0.s3 = fd1.s3;
            fd1.s3 = fd2.s3;
            fd2.s3 = (float)(data[pos] >> task.data.wbits);
            len[pos >> 6] += fabs(fd2.s3 + (sum2.x + sum2.y));
        }
    }
    else
    {
        float fcoef[32];
        for (int tid = 0; tid < 32; tid++)
            fcoef[tid] = tid < MAX_ORDER && tid + ro - MAX_ORDER >= 0 ? -((float)task.coefs[tid + ro - MAX_ORDER]) / (1 << task.data.shift) : 0.0f;

        float4 fc0 = vload4(0, &fcoef[0]);
        float4 fc1 = vload4(1, &fcoef[0]);
        float4 fc2 = vload4(2, &fcoef[0]);

        float fdata[MAX_ORDER + TEMPBLOCK1 + 32];
        for (int pos = 0; pos < MAX_ORDER; pos++)
            fdata[pos] = 0.0f;
        for (int pos = MAX_ORDER + TEMPBLOCK1; pos < MAX_ORDER + TEMPBLOCK1 + 32; pos++)
            fdata[pos] = 0.0f;
        for (int bpos = 0; bpos < bs; bpos += TEMPBLOCK1)
        {
            const int end = min(bpos + TEMPBLOCK1, bs);

            for (int pos = max(bpos - ro, 0); pos < max(bpos, ro); pos++)
                fdata[MAX_ORDER + pos - bpos] = (float)(data[pos] >> task.data.wbits);

            for (int pos = max(bpos, ro); pos < end; pos++)
            {
                float next = (float)(data[pos] >> task.data.wbits);
                float* dptr = fdata + pos - bpos;
                dptr[MAX_ORDER] = next;
                float4 sum =
                    fc0 * vload4(0, dptr) +
                    fc1 * vload4(1, dptr) +
                    fc2 * vload4(2, dptr);
                next += sum.x + sum.y + sum.z + sum.w;
                len[pos >> 6] += fabs(next);
            }
        }
    }

    int total = 0;
    for (int i = 0; i < ERPARTS; i++)
    {
        const int res = convert_int_sat_rte(len[i] * 2);
        const int k = clamp(31 - clz(res) - 6, 0, MAX_RICE_PARAM);
        total += (k << 6) + (res >> k);
    }
    const int partLen = min(0x7ffffff, total) + (bs - ro);
    const int obits = task.data.obits - task.data.wbits;
    const int predicted =
        task.data.type == Fixed ? ro * obits + 6 + RICE_PARAM_BITS + partLen :
        task.data.type == LPC ? ro * obits + 4 + 5 + ro * task.data.cbits + 6 + RICE_PARAM_BITS + partLen :
        task.data.type == Constant ? obits * (partLen != bs - ro ? bs : 1) :
        obits * bs;

    tasks[selectedTask].data.size = min(obits * bs, predicted);
}

ulong ldcompressFoldResidual(long residual)
{
    return residual >= 0
        ? ((ulong)residual) << 1
        : ((((ulong)(-(residual + 1))) << 1) + 1);
}

long ldcompressShiftedSample(__global const int* data, int pos, int wbits)
{
    const long sample = (long)data[pos];
    return wbits == 0 ? sample : sample / ((long)1 << wbits);
}

long ldcompressFixedResidual(__global const int* data, int pos, int order, int wbits)
{
    const long sample = ldcompressShiftedSample(data, pos, wbits);
    switch (order)
    {
    case 0:
        return sample;
    case 1:
        return sample - ldcompressShiftedSample(data, pos - 1, wbits);
    case 2:
        return sample -
            (2L * ldcompressShiftedSample(data, pos - 1, wbits)) +
            ldcompressShiftedSample(data, pos - 2, wbits);
    case 3:
        return sample -
            (3L * ldcompressShiftedSample(data, pos - 1, wbits)) +
            (3L * ldcompressShiftedSample(data, pos - 2, wbits)) -
            ldcompressShiftedSample(data, pos - 3, wbits);
    case 4:
        return sample -
            (4L * ldcompressShiftedSample(data, pos - 1, wbits)) +
            (6L * ldcompressShiftedSample(data, pos - 2, wbits)) -
            (4L * ldcompressShiftedSample(data, pos - 3, wbits)) +
            ldcompressShiftedSample(data, pos - 4, wbits);
    default:
        return 0;
    }
}

long ldcompressArithmeticShiftRight(long value, int shift)
{
    if (shift == 0)
        return value;
    if (value >= 0)
        return value >> shift;
    const long divisor = (long)1 << shift;
    return -(((-value) + divisor - 1) >> shift);
}

long ldcompressLpcResidual(__global const int* data, int pos, FLACCLSubframeTask task)
{
    const int order = task.data.residualOrder;
    long sum = 0;
    for (int i = 0; i < order; i++)
    {
        sum += (long)task.coefs[i] *
            ldcompressShiftedSample(data, pos - order + i, task.data.wbits);
    }
    const long predicted = ldcompressArithmeticShiftRight(sum, task.data.shift);
    return ldcompressShiftedSample(data, pos, task.data.wbits) - predicted;
}

long ldcompressResidual(__global const int* data, int pos, FLACCLSubframeTask task)
{
    if (task.data.type == LPC)
        return ldcompressLpcResidual(data, pos, task);
    return ldcompressFixedResidual(
        data, pos, task.data.residualOrder, task.data.wbits);
}

long ldcompressFixedResidualShifted(__global const int* data, int pos, int order)
{
    const long sample = (long)data[pos];
    switch (order)
    {
    case 0:
        return sample;
    case 1:
        return sample - (long)data[pos - 1];
    case 2:
        return sample - (2L * (long)data[pos - 1]) + (long)data[pos - 2];
    case 3:
        return sample -
            (3L * (long)data[pos - 1]) +
            (3L * (long)data[pos - 2]) -
            (long)data[pos - 3];
    case 4:
        return sample -
            (4L * (long)data[pos - 1]) +
            (6L * (long)data[pos - 2]) -
            (4L * (long)data[pos - 3]) +
            (long)data[pos - 4];
    default:
        return 0L;
    }
}

long ldcompressLpcResidualShiftedI64(
    __global const int* data,
    int pos,
    FLACCLSubframeTask task)
{
    const int order = task.data.residualOrder;
    long sum = 0L;
    for (int i = 0; i < order; i++)
        sum += (long)task.coefs[i] * (long)data[pos - order + i];
    const long predicted = ldcompressArithmeticShiftRight(sum, task.data.shift);
    return (long)data[pos] - predicted;
}

int ldcompressShiftedLpcResidualFitsInt(FLACCLSubframeTask task)
{
    if (task.data.type != LPC || task.data.abits <= 0 || task.data.abits >= 31)
        return 0;

    ulong coefficientAbsSum = 0UL;
    for (int i = 0; i < task.data.residualOrder; i++)
    {
        const int coefficient = task.coefs[i];
        coefficientAbsSum += coefficient >= 0
            ? (ulong)coefficient
            : (ulong)(-(coefficient + 1)) + 1UL;
    }
    const ulong maxSample = 1UL << (uint)task.data.abits;
    return coefficientAbsSum <= (0x7fffffffUL - maxSample) / maxSample;
}

int ldcompressLpcResidualShiftedI32(
    __global const int* data,
    int pos,
    FLACCLSubframeTask task)
{
    const int order = task.data.residualOrder;
    if (order == 12)
    {
        int sum = task.coefs[0] * data[pos - 12];
        sum += task.coefs[1] * data[pos - 11];
        sum += task.coefs[2] * data[pos - 10];
        sum += task.coefs[3] * data[pos - 9];
        sum += task.coefs[4] * data[pos - 8];
        sum += task.coefs[5] * data[pos - 7];
        sum += task.coefs[6] * data[pos - 6];
        sum += task.coefs[7] * data[pos - 5];
        sum += task.coefs[8] * data[pos - 4];
        sum += task.coefs[9] * data[pos - 3];
        sum += task.coefs[10] * data[pos - 2];
        sum += task.coefs[11] * data[pos - 1];
        return data[pos] - (int)ldcompressArithmeticShiftRight((long)sum, task.data.shift);
    }
    if (order == 11)
    {
        int sum = task.coefs[0] * data[pos - 11];
        sum += task.coefs[1] * data[pos - 10];
        sum += task.coefs[2] * data[pos - 9];
        sum += task.coefs[3] * data[pos - 8];
        sum += task.coefs[4] * data[pos - 7];
        sum += task.coefs[5] * data[pos - 6];
        sum += task.coefs[6] * data[pos - 5];
        sum += task.coefs[7] * data[pos - 4];
        sum += task.coefs[8] * data[pos - 3];
        sum += task.coefs[9] * data[pos - 2];
        sum += task.coefs[10] * data[pos - 1];
        return data[pos] - (int)ldcompressArithmeticShiftRight((long)sum, task.data.shift);
    }
    if (order == 10)
    {
        int sum = task.coefs[0] * data[pos - 10];
        sum += task.coefs[1] * data[pos - 9];
        sum += task.coefs[2] * data[pos - 8];
        sum += task.coefs[3] * data[pos - 7];
        sum += task.coefs[4] * data[pos - 6];
        sum += task.coefs[5] * data[pos - 5];
        sum += task.coefs[6] * data[pos - 4];
        sum += task.coefs[7] * data[pos - 3];
        sum += task.coefs[8] * data[pos - 2];
        sum += task.coefs[9] * data[pos - 1];
        return data[pos] - (int)ldcompressArithmeticShiftRight((long)sum, task.data.shift);
    }

    int sum = 0;
    for (int i = 0; i < order; i++)
        sum += task.coefs[i] * data[pos - order + i];
    return data[pos] - (int)ldcompressArithmeticShiftRight((long)sum, task.data.shift);
}

long ldcompressResidualShifted(
    __global const int* data,
    int pos,
    FLACCLSubframeTask task,
    int useIntLpc)
{
    if (task.data.type != LPC)
        return ldcompressFixedResidualShifted(data, pos, task.data.residualOrder);
    return useIntLpc
        ? (long)ldcompressLpcResidualShiftedI32(data, pos, task)
        : ldcompressLpcResidualShiftedI64(data, pos, task);
}

int ldcompressSignedValueFitsBits(int value, int bits)
{
    if (bits <= 0 || bits > 31)
        return 0;
    const int minValue = -(1 << (bits - 1));
    const int maxValue = (1 << (bits - 1)) - 1;
    return value >= minValue && value <= maxValue;
}

int ldcompressValidPartitionOrder(int blocksize, int predictorOrder, int partitionOrder)
{
    const int partitionCount = 1 << partitionOrder;
    if ((blocksize % partitionCount) != 0)
        return 0;
    return (blocksize / partitionCount) > predictorOrder;
}

ulong ldcompressReduceSumUlong(ulong value, __local ulong* scratch)
{
    const int lane = (int)get_local_id(0);
    scratch[lane] = value;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int stride = EXACT_WORKGROUP_SIZE >> 1; stride > 0; stride >>= 1)
    {
        if (lane < stride)
            scratch[lane] += scratch[lane + stride];
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    return scratch[0];
}

uint ldcompressReduceOrUint(uint value, __local uint* scratch)
{
    const int lane = (int)get_local_id(0);
    scratch[lane] = value;
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int stride = EXACT_WORKGROUP_SIZE >> 1; stride > 0; stride >>= 1)
    {
        if (lane < stride)
            scratch[lane] |= scratch[lane + stride];
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    return scratch[0];
}

ulong ldcompressAbsLong(long value)
{
    return value >= 0
        ? (ulong)value
        : (ulong)(-(value + 1)) + 1UL;
}

__kernel __attribute__((reqd_work_group_size(EXACT_WORKGROUP_SIZE, 1, 1)))
void ldcompressPruneFixedOrderGuess(
    __global const int* samples,
    __global const int* selectedTasks,
    __global FLACCLSubframeTask* tasks,
    int taskCount,
    int samplesAlreadyShifted)
{
    __local ulong reduceScratch[5 * EXACT_WORKGROUP_SIZE];
    const int lane = (int)get_local_id(0);
    const int frame = (int)get_group_id(0);
    const int base = frame * taskCount;

    int fixedTaskNo[5];
    for (int order = 0; order <= 4; order++)
        fixedTaskNo[order] = -1;

    for (int i = 0; i < taskCount; i++)
    {
        const int taskNo = selectedTasks[base + i];
        FLACCLSubframeTask task = tasks[taskNo];
        const int order = task.data.residualOrder;
        if (task.data.type == Fixed && order >= 0 && order <= 4)
            fixedTaskNo[order] = taskNo;
    }

    FLACCLSubframeTask firstTask = tasks[selectedTasks[base]];
    const int bs = firstTask.data.blocksize;
    const int wbits = firstTask.data.wbits;
    __global const int* data = &samples[firstTask.data.samplesOffs];

    ulong localSums[5];
    for (int order = 0; order <= 4; order++)
        localSums[order] = 0UL;

    if (samplesAlreadyShifted)
    {
        for (int pos = lane; pos < bs; pos += EXACT_WORKGROUP_SIZE)
        {
            const long sample0 = (long)data[pos];
            const long sample1 = pos >= 1 ? (long)data[pos - 1] : 0L;
            const long sample2 = pos >= 2 ? (long)data[pos - 2] : 0L;
            const long sample3 = pos >= 3 ? (long)data[pos - 3] : 0L;
            const long sample4 = pos >= 4 ? (long)data[pos - 4] : 0L;
            if (fixedTaskNo[0] >= 0)
                localSums[0] += ldcompressAbsLong(sample0);
            if (fixedTaskNo[1] >= 0 && pos >= 1)
                localSums[1] += ldcompressAbsLong(sample0 - sample1);
            if (fixedTaskNo[2] >= 0 && pos >= 2)
                localSums[2] += ldcompressAbsLong(sample0 - (2L * sample1) + sample2);
            if (fixedTaskNo[3] >= 0 && pos >= 3)
                localSums[3] += ldcompressAbsLong(
                    sample0 - (3L * sample1) + (3L * sample2) - sample3);
            if (fixedTaskNo[4] >= 0 && pos >= 4)
                localSums[4] += ldcompressAbsLong(
                    sample0 - (4L * sample1) + (6L * sample2) -
                    (4L * sample3) + sample4);
        }
    }
    else
    {
        for (int pos = lane; pos < bs; pos += EXACT_WORKGROUP_SIZE)
        {
            if (fixedTaskNo[0] >= 0)
                localSums[0] += ldcompressAbsLong(ldcompressFixedResidual(data, pos, 0, wbits));
            if (fixedTaskNo[1] >= 0 && pos >= 1)
                localSums[1] += ldcompressAbsLong(ldcompressFixedResidual(data, pos, 1, wbits));
            if (fixedTaskNo[2] >= 0 && pos >= 2)
                localSums[2] += ldcompressAbsLong(ldcompressFixedResidual(data, pos, 2, wbits));
            if (fixedTaskNo[3] >= 0 && pos >= 3)
                localSums[3] += ldcompressAbsLong(ldcompressFixedResidual(data, pos, 3, wbits));
            if (fixedTaskNo[4] >= 0 && pos >= 4)
                localSums[4] += ldcompressAbsLong(ldcompressFixedResidual(data, pos, 4, wbits));
        }
    }

    ulong sums[5];
    for (int order = 0; order <= 4; order++)
        sums[order] = ldcompressReduceSumUlong(
            localSums[order],
            reduceScratch + (order * EXACT_WORKGROUP_SIZE));

    if (lane != 0)
        return;

    int bestOrder = -1;
    ulong bestSum = 0xffffffffffffffffUL;
    for (int order = 0; order <= 4; order++)
    {
        if (fixedTaskNo[order] >= 0 && sums[order] < bestSum)
        {
            bestOrder = order;
            bestSum = sums[order];
        }
    }

    if (bestOrder < 0)
        return;

    for (int order = 0; order <= 4; order++)
    {
        if (fixedTaskNo[order] >= 0 && order != bestOrder)
        {
            FLACCLSubframeTask task = tasks[fixedTaskNo[order]];
            task.data.size = 0x7fffffff;
            tasks[fixedTaskNo[order]] = task;
        }
    }
}

int ldcompressCooperativeAllFrameSamplesEqual(
    __global const int* data,
    int blocksize,
    __local uint* reduceScratch)
{
    const int first = data[0];
    uint mismatch = 0;
    for (int pos = (int)get_local_id(0) + 1; pos < blocksize; pos += EXACT_WORKGROUP_SIZE)
    {
        if (data[pos] != first)
            mismatch = 1;
    }
    return ldcompressReduceOrUint(mismatch, reduceScratch) == 0;
}

ulong ldcompressCooperativeBestRiceBitsForPartition(
    __global const int* data,
    int partitionStart,
    int residualCount,
    FLACCLSubframeTask task,
    __local ulong* reduceScratch)
;

typedef struct
{
    ulong bits;
    uint parameter;
} RiceChoice;

typedef struct
{
    ulong exactBits;
    ulong estimatedBits;
    uint parameter;
} MeanRiceChoice;

int ldcompressProfileUsesMeanRice(int analysisProfile)
{
    return analysisProfile == ANALYSIS_PROFILE_ORDER_GUESS_MEAN_RICE ||
        analysisProfile == ANALYSIS_PROFILE_ORDER_GUESS_MEAN_ESTIMATE_RICE ||
        analysisProfile == ANALYSIS_PROFILE_SUBDIVIDE_TUKEY3_MEAN_RICE ||
        analysisProfile == ANALYSIS_PROFILE_SUBDIVIDE_TUKEY3_MEAN_ESTIMATE_RICE;
}

int ldcompressProfileUsesMeanEstimatedSize(int analysisProfile)
{
    return analysisProfile == ANALYSIS_PROFILE_ORDER_GUESS_MEAN_ESTIMATE_RICE ||
        analysisProfile == ANALYSIS_PROFILE_SUBDIVIDE_TUKEY3_MEAN_ESTIMATE_RICE;
}

uint ldcompressIlog2Ulong(ulong value)
{
    uint result = 0U;
    while (value > 1UL)
    {
        value >>= 1U;
        result++;
    }
    return result;
}

uint ldcompressMeanRiceParameter(ulong absSum, int sampleCount)
{
    if (sampleCount <= 0)
        return 0U;
    const ulong divisor = 0x40000UL / (ulong)sampleCount;
    const ulong scaledMean =
        absSum < 2UL || divisor == 0UL ? 0UL : (((absSum - 1UL) * divisor) >> 18U);
    if (scaledMean == 0UL)
        return 0U;
    return min((uint)MAX_RICE_PARAM, ldcompressIlog2Ulong(scaledMean) + 1U);
}

ulong ldcompressMeanRicePartitionBits(uint riceParameter, int sampleCount, ulong absSum)
{
    ulong bits = 4UL + ((ulong)(1U + riceParameter) * (ulong)sampleCount);
    bits += riceParameter == 0U
        ? (absSum << 1U)
        : (absSum >> (riceParameter - 1U));
    const ulong correction = (ulong)(sampleCount >> 1);
    return bits > correction ? bits - correction : 0UL;
}

RiceChoice ldcompressCooperativeBestRiceForPartition(
    __global const int* data,
    int partitionStart,
    int residualCount,
    FLACCLSubframeTask task,
    __local ulong* reduceScratch)
{
    ulong bits0 = 0UL;
    ulong bits1 = 0UL;
    ulong bits2 = 0UL;
    ulong bits3 = 0UL;
    ulong bits4 = 0UL;
    ulong bits5 = 0UL;
    ulong bits6 = 0UL;
    ulong bits7 = 0UL;
    ulong bits8 = 0UL;
    ulong bits9 = 0UL;
    ulong bits10 = 0UL;
    ulong bits11 = 0UL;
    ulong bits12 = 0UL;
    ulong bits13 = 0UL;
    ulong bits14 = 0UL;

    for (int i = (int)get_local_id(0); i < residualCount; i += EXACT_WORKGROUP_SIZE)
    {
        const long residual = ldcompressResidual(data, partitionStart + i, task);
        const ulong folded = ldcompressFoldResidual(residual);
        bits0 += folded + 1UL;
        bits1 += (folded >> 1) + 2UL;
        bits2 += (folded >> 2) + 3UL;
        bits3 += (folded >> 3) + 4UL;
        bits4 += (folded >> 4) + 5UL;
        bits5 += (folded >> 5) + 6UL;
        bits6 += (folded >> 6) + 7UL;
        bits7 += (folded >> 7) + 8UL;
        bits8 += (folded >> 8) + 9UL;
        bits9 += (folded >> 9) + 10UL;
        bits10 += (folded >> 10) + 11UL;
        bits11 += (folded >> 11) + 12UL;
        bits12 += (folded >> 12) + 13UL;
        bits13 += (folded >> 13) + 14UL;
        bits14 += (folded >> 14) + 15UL;
    }

    RiceChoice best;
    best.bits = 0xffffffffffffffffUL;
    best.parameter = 0U;
    for (int parameter = 0; parameter <= MAX_RICE_PARAM; parameter++)
    {
        ulong localBits = bits0;
        if (parameter == 1)
            localBits = bits1;
        else if (parameter == 2)
            localBits = bits2;
        else if (parameter == 3)
            localBits = bits3;
        else if (parameter == 4)
            localBits = bits4;
        else if (parameter == 5)
            localBits = bits5;
        else if (parameter == 6)
            localBits = bits6;
        else if (parameter == 7)
            localBits = bits7;
        else if (parameter == 8)
            localBits = bits8;
        else if (parameter == 9)
            localBits = bits9;
        else if (parameter == 10)
            localBits = bits10;
        else if (parameter == 11)
            localBits = bits11;
        else if (parameter == 12)
            localBits = bits12;
        else if (parameter == 13)
            localBits = bits13;
        else if (parameter == 14)
            localBits = bits14;

        const ulong bits = ldcompressReduceSumUlong(localBits, reduceScratch);
        if (bits < best.bits)
        {
            best.bits = bits;
            best.parameter = (uint)parameter;
        }
    }
    return best;
}

MeanRiceChoice ldcompressCooperativeMeanRiceForPartition(
    __global const int* data,
    int partitionStart,
    int residualCount,
    FLACCLSubframeTask task,
    __local ulong* reduceScratch,
    int includeExactBits)
{
    ulong localAbsSum = 0UL;
    for (int i = (int)get_local_id(0); i < residualCount; i += EXACT_WORKGROUP_SIZE)
    {
        const long residual = ldcompressResidual(data, partitionStart + i, task);
        localAbsSum += ldcompressAbsLong(residual);
    }

    const ulong absSum = ldcompressReduceSumUlong(localAbsSum, reduceScratch);
    const uint parameter = ldcompressMeanRiceParameter(absSum, residualCount);

    ulong localExactBits = 0UL;
    if (includeExactBits)
    {
        for (int i = (int)get_local_id(0); i < residualCount; i += EXACT_WORKGROUP_SIZE)
        {
            const long residual = ldcompressResidual(data, partitionStart + i, task);
            const ulong folded = ldcompressFoldResidual(residual);
            localExactBits += (folded >> parameter) + 1UL + (ulong)parameter;
        }
    }

    MeanRiceChoice choice;
    choice.exactBits = includeExactBits
        ? ldcompressReduceSumUlong(localExactBits, reduceScratch)
        : 0UL;
    choice.estimatedBits =
        ldcompressMeanRicePartitionBits(parameter, residualCount, absSum);
    choice.parameter = parameter;
    return choice;
}

ulong ldcompressCooperativeBestRiceBitsForPartition(
    __global const int* data,
    int partitionStart,
    int residualCount,
    FLACCLSubframeTask task,
    __local ulong* reduceScratch)
{
    const RiceChoice choice = ldcompressCooperativeBestRiceForPartition(
        data, partitionStart, residualCount, task, reduceScratch);
    return choice.bits;
}

__kernel __attribute__((reqd_work_group_size(EXACT_WORKGROUP_SIZE, 1, 1)))
void ldcompressAnalyzeSubframeExact(
    __global const int* samples,
    __global const int* selectedTasks,
    __global FLACCLSubframeTask* tasks,
    int maxRicePartitionOrder,
    int analysisProfile,
    __global uint* taskRiceParameters,
    __local ulong* exactRiceLeafSums,
    __global const int* shiftedSamples,
    int useShiftedSamples)
{
    __local ulong reduceScratchUlong[EXACT_WORKGROUP_SIZE];
    __local uint reduceScratchUint[EXACT_WORKGROUP_SIZE];
    __local uint candidateRiceParameters[MAX_RICE_PARTITION_COUNT];
    __local uint bestRiceParameters[MAX_RICE_PARTITION_COUNT];
    const int lane = (int)get_local_id(0);
    const int selectedTask = selectedTasks[get_group_id(0)];
    FLACCLSubframeTask task = tasks[selectedTask];
    const int ro = task.data.residualOrder;
    const int bs = task.data.blocksize;
    const int wbits = task.data.wbits;
    const int obits = task.data.obits - wbits;
    __global const int* originalData = &samples[task.data.samplesOffs];
    __global const int* data = useShiftedSamples
        ? &shiftedSamples[task.data.samplesOffs]
        : originalData;
    const int riceOutputBase = selectedTask * MAX_RICE_PARTITION_COUNT;

    for (int i = lane; i < MAX_RICE_PARTITION_COUNT; i += EXACT_WORKGROUP_SIZE)
        taskRiceParameters[riceOutputBase + i] = 0U;
    barrier(CLK_GLOBAL_MEM_FENCE);

    if (task.data.size == 0x7fffffff)
    {
        if (lane == 0)
            tasks[selectedTask] = task;
        return;
    }

    task.data.porder = 0;

    if (task.data.type == Constant)
    {
        const int equal =
            ldcompressCooperativeAllFrameSamplesEqual(originalData, bs, reduceScratchUint);

        if (!equal)
            task.data.size = 0x7fffffff;
        else
            task.data.size = 8 + obits + (wbits == 0 ? 0 : wbits);
        if (lane == 0)
            tasks[selectedTask] = task;
        return;
    }

    if ((task.data.type != Fixed && task.data.type != LPC) ||
        ro < 0 ||
        ro >= bs ||
        (task.data.type == Fixed && ro > 4) ||
        (task.data.type == LPC && (ro == 0 || ro > MAX_ORDER || task.data.shift < 0 || task.data.shift > 15 ||
                                      task.data.cbits <= 0 || task.data.cbits > 15)))
    {
        task.data.size = 0x7fffffff;
        if (lane == 0)
            tasks[selectedTask] = task;
        return;
    }

    if (task.data.type == LPC)
    {
        int coefficientsValid = 1;
        for (int i = 0; i < ro; i++)
            if (!ldcompressSignedValueFitsBits(task.coefs[i], task.data.cbits))
                coefficientsValid = 0;
        if (!coefficientsValid)
        {
            task.data.size = 0x7fffffff;
            if (lane == 0)
                tasks[selectedTask] = task;
            return;
        }
    }

    ulong bestBits = 0xffffffffffffffffUL;
    ulong bestEstimatedBits = 0xffffffffffffffffUL;
    int bestPartitionOrder = 0;
    const int maxPartitionOrder = min(maxRicePartitionOrder, 8);
    const int useMeanRice = ldcompressProfileUsesMeanRice(analysisProfile);
    const int useMeanEstimatedSize =
        ldcompressProfileUsesMeanEstimatedSize(analysisProfile);
    const int useLeafExactRice =
        !useMeanRice &&
        maxPartitionOrder <= EXACT_LEAF_MAX_RICE_PARTITION_ORDER &&
        ldcompressValidPartitionOrder(bs, ro, maxPartitionOrder);
    const int useLeafMeanEstimatedRice =
        useMeanRice &&
        useMeanEstimatedSize &&
        maxPartitionOrder <= EXACT_LEAF_MAX_RICE_PARTITION_ORDER &&
        ldcompressValidPartitionOrder(bs, ro, maxPartitionOrder);
    const int useIntLpc =
        useShiftedSamples && ldcompressShiftedLpcResidualFitsInt(task);
    const ulong baseBits = task.data.type == LPC
        ? 8UL +
            (wbits == 0 ? 0UL : (ulong)wbits) +
            ((ulong)ro * (ulong)obits) +
            4UL +
            5UL +
            ((ulong)ro * (ulong)task.data.cbits) +
            2UL +
            4UL
        : 8UL +
            ((ulong)ro * (ulong)obits) +
            2UL +
            4UL +
            (wbits == 0 ? 0UL : (ulong)wbits);

    if (useLeafExactRice)
    {
        const int leafPartitionOrder = maxPartitionOrder;
        const int leafCount = 1 << leafPartitionOrder;
        const int leafSamples = bs >> leafPartitionOrder;
        for (int leaf = lane; leaf < leafCount; leaf += EXACT_WORKGROUP_SIZE)
        {
            ulong sums[EXACT_LEAF_RICE_PARAMETER_COUNT];
            for (int parameter = 0; parameter < EXACT_LEAF_RICE_PARAMETER_COUNT; parameter++)
                sums[parameter] = 0UL;

            const int start = leaf == 0 ? ro : leaf * leafSamples;
            const int end = (leaf + 1) * leafSamples;
            for (int pos = start; pos < end; pos++)
            {
                const long residual = ldcompressResidual(data, pos, task);
                const ulong folded = ldcompressFoldResidual(residual);
                for (int parameter = 0; parameter < EXACT_LEAF_RICE_PARAMETER_COUNT; parameter++)
                    sums[parameter] += folded >> parameter;
            }

            const int outputBase = leaf * EXACT_LEAF_RICE_PARAMETER_COUNT;
            for (int parameter = 0; parameter < EXACT_LEAF_RICE_PARAMETER_COUNT; parameter++)
                exactRiceLeafSums[outputBase + parameter] = sums[parameter];
        }
        barrier(CLK_LOCAL_MEM_FENCE);

        if (lane == 0)
        {
            for (int partitionOrder = 0; partitionOrder <= leafPartitionOrder; partitionOrder++)
            {
                if (!ldcompressValidPartitionOrder(bs, ro, partitionOrder))
                    continue;

                const int partitionCount = 1 << partitionOrder;
                const int partitionSamples = bs >> partitionOrder;
                const int leafGroupSize = 1 << (leafPartitionOrder - partitionOrder);
                ulong bits = baseBits;
                for (int partition = 0; partition < partitionCount; partition++)
                {
                    const int residualCount = partition == 0
                        ? partitionSamples - ro
                        : partitionSamples;
                    const int leafStart = partition * leafGroupSize;
                    ulong bestParameterBits = 0xffffffffffffffffUL;
                    uint bestParameter = 0U;
                    for (int parameter = 0; parameter <= MAX_RICE_PARAM; parameter++)
                    {
                        ulong sum = 0UL;
                        for (int leaf = 0; leaf < leafGroupSize; leaf++)
                        {
                            const int index =
                                ((leafStart + leaf) * EXACT_LEAF_RICE_PARAMETER_COUNT) +
                                parameter;
                            sum += exactRiceLeafSums[index];
                        }
                        const ulong parameterBits =
                            sum + ((ulong)residualCount * (ulong)(1 + parameter));
                        if (parameterBits < bestParameterBits)
                        {
                            bestParameterBits = parameterBits;
                            bestParameter = (uint)parameter;
                        }
                    }
                    bits += 4UL + bestParameterBits;
                    candidateRiceParameters[partition] = bestParameter;
                }

                if (bits < bestBits)
                {
                    bestBits = bits;
                    bestPartitionOrder = partitionOrder;
                    for (int partition = 0; partition < partitionCount; partition++)
                        taskRiceParameters[riceOutputBase + partition] =
                            candidateRiceParameters[partition];
                }
            }

            task.data.porder = bestPartitionOrder;
            task.data.size = bestBits > 0x7fffffffUL ? 0x7fffffff : (int)bestBits;
            tasks[selectedTask] = task;
        }
        return;
    }

    if (useLeafMeanEstimatedRice)
    {
        const int leafPartitionOrder = maxPartitionOrder;
        const int leafCount = 1 << leafPartitionOrder;
        const int leafSamples = bs >> leafPartitionOrder;
        for (int leaf = lane; leaf < leafCount; leaf += EXACT_WORKGROUP_SIZE)
        {
            ulong absSum = 0UL;
            const int start = leaf == 0 ? ro : leaf * leafSamples;
            const int end = (leaf + 1) * leafSamples;
            for (int pos = start; pos < end; pos++)
            {
                const long residual = useShiftedSamples
                    ? ldcompressResidualShifted(data, pos, task, useIntLpc)
                    : ldcompressResidual(data, pos, task);
                absSum += ldcompressAbsLong(residual);
            }
            exactRiceLeafSums[leaf] = absSum;
        }
        barrier(CLK_LOCAL_MEM_FENCE);

        if (lane == 0)
        {
            for (int partitionOrderPlusOne = leafPartitionOrder + 1;
                 partitionOrderPlusOne > 0;
                 partitionOrderPlusOne--)
            {
                const int partitionOrder = partitionOrderPlusOne - 1;
                if (!ldcompressValidPartitionOrder(bs, ro, partitionOrder))
                    continue;

                const int partitionCount = 1 << partitionOrder;
                const int partitionSamples = bs >> partitionOrder;
                const int leafGroupSize = 1 << (leafPartitionOrder - partitionOrder);
                ulong estimatedBits = 2UL + 4UL;
                for (int partition = 0; partition < partitionCount; partition++)
                {
                    const int residualCount = partition == 0
                        ? partitionSamples - ro
                        : partitionSamples;
                    const int leafStart = partition * leafGroupSize;
                    ulong absSum = 0UL;
                    for (int leaf = 0; leaf < leafGroupSize; leaf++)
                        absSum += exactRiceLeafSums[leafStart + leaf];

                    const uint parameter =
                        ldcompressMeanRiceParameter(absSum, residualCount);
                    estimatedBits +=
                        ldcompressMeanRicePartitionBits(parameter, residualCount, absSum);
                    candidateRiceParameters[partition] = parameter;
                }

                if (estimatedBits < bestEstimatedBits)
                {
                    bestEstimatedBits = estimatedBits;
                    bestPartitionOrder = partitionOrder;
                    const ulong riceHeaderBits = 2UL + 4UL;
                    const ulong estimateBaseBits = baseBits - riceHeaderBits;
                    bestBits = bestEstimatedBits >= 0xffffffffffffffffUL - estimateBaseBits
                        ? 0xffffffffffffffffUL
                        : estimateBaseBits + bestEstimatedBits;
                    for (int partition = 0; partition < partitionCount; partition++)
                        taskRiceParameters[riceOutputBase + partition] =
                            candidateRiceParameters[partition];
                }
            }

            task.data.porder = bestPartitionOrder;
            task.data.size = bestBits > 0x7fffffffUL ? 0x7fffffff : (int)bestBits;
            tasks[selectedTask] = task;
        }
        return;
    }

    for (int partitionOrder = useMeanRice ? maxPartitionOrder : 0;
         useMeanRice ? partitionOrder >= 0 : partitionOrder <= maxPartitionOrder;
         partitionOrder += useMeanRice ? -1 : 1)
    {
        if (!ldcompressValidPartitionOrder(bs, ro, partitionOrder))
            continue;

        const int partitionCount = 1 << partitionOrder;
        const int partitionSamples = bs >> partitionOrder;
        ulong bits = baseBits;
        ulong estimatedBits = 2UL + 4UL;
        for (int partition = 0; partition < partitionCount; partition++)
        {
            const int residualCount = partition == 0
                ? partitionSamples - ro
                : partitionSamples;
            const int partitionStart = partition == 0
                ? ro
                : partition * partitionSamples;
            if (useMeanRice)
            {
                const MeanRiceChoice choice = ldcompressCooperativeMeanRiceForPartition(
                    data, partitionStart, residualCount, task, reduceScratchUlong, 0);
                estimatedBits += choice.estimatedBits;
                if (lane == 0)
                    candidateRiceParameters[partition] = choice.parameter;
            }
            else
            {
                const RiceChoice choice = ldcompressCooperativeBestRiceForPartition(
                    data, partitionStart, residualCount, task, reduceScratchUlong);
                bits += 4UL + choice.bits;
                if (lane == 0)
                    candidateRiceParameters[partition] = choice.parameter;
            }
        }
        barrier(CLK_LOCAL_MEM_FENCE);

        if ((useMeanRice && estimatedBits < bestEstimatedBits) ||
            (!useMeanRice && bits < bestBits))
        {
            bestEstimatedBits = estimatedBits;
            bestBits = bits;
            bestPartitionOrder = partitionOrder;
            for (int partition = lane; partition < partitionCount; partition += EXACT_WORKGROUP_SIZE)
                bestRiceParameters[partition] = candidateRiceParameters[partition];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }

    if (useMeanRice)
    {
        if (useMeanEstimatedSize)
        {
            const ulong riceHeaderBits = 2UL + 4UL;
            const ulong estimateBaseBits = baseBits - riceHeaderBits;
            bestBits = bestEstimatedBits >= 0xffffffffffffffffUL - estimateBaseBits
                ? 0xffffffffffffffffUL
                : estimateBaseBits + bestEstimatedBits;
        }
        else
        {
            bestBits = baseBits;
            const int partitionCount = 1 << bestPartitionOrder;
            const int partitionSamples = bs >> bestPartitionOrder;
            for (int partition = 0; partition < partitionCount; partition++)
            {
                const int residualCount = partition == 0
                    ? partitionSamples - ro
                    : partitionSamples;
                const int partitionStart = partition == 0
                    ? ro
                    : partition * partitionSamples;
                const MeanRiceChoice choice = ldcompressCooperativeMeanRiceForPartition(
                    data, partitionStart, residualCount, task, reduceScratchUlong, 1);
                bestBits += 4UL + choice.exactBits;
            }
        }
    }

    const int bestPartitionCount = 1 << bestPartitionOrder;
    for (int partition = lane; partition < bestPartitionCount; partition += EXACT_WORKGROUP_SIZE)
        taskRiceParameters[riceOutputBase + partition] = bestRiceParameters[partition];

    task.data.porder = bestPartitionOrder;
    task.data.size = bestBits > 0x7fffffffUL ? 0x7fffffff : (int)bestBits;
    if (lane == 0)
        tasks[selectedTask] = task;
}

__kernel void ldcompressChooseBestMethod(
    __global FLACCLSubframeTask* tasks_out,
    __global const FLACCLSubframeTask* tasks,
    __global const int* selectedTasks,
    int taskCount)
{
    const int base = (int)get_global_id(0) * taskCount;
    int best_no = selectedTasks[base];
    int best_len = tasks[best_no].data.size;

    for (int i = 1; i < taskCount; i++)
    {
        const int task_no = selectedTasks[base + i];
        const int task_len = tasks[task_no].data.size;
        if (task_len < best_len)
        {
            best_no = task_no;
            best_len = task_len;
        }
    }

    tasks_out[get_global_id(0)] = tasks[best_no];
}

__kernel __attribute__((reqd_work_group_size(EXACT_WORKGROUP_SIZE, 1, 1)))
void ldcompressWriteBestRiceParameters(
    __global const FLACCLSubframeTask* tasks,
    __global const int* selectedTasks,
    __global const uint* taskRiceParameters,
    __global uint* bestRiceParameters,
    int taskCount)
{
    const int lane = (int)get_local_id(0);
    const int frame = (int)get_group_id(0);
    const int outputBase = frame * MAX_RICE_PARTITION_COUNT;
    const int selectedBase = frame * taskCount;
    int bestTask = selectedTasks[selectedBase];
    int bestSize = tasks[bestTask].data.size;

    for (int i = 1; i < taskCount; i++)
    {
        const int task = selectedTasks[selectedBase + i];
        const int size = tasks[task].data.size;
        if (size < bestSize)
        {
            bestTask = task;
            bestSize = size;
        }
    }

    const int inputBase = bestTask * MAX_RICE_PARTITION_COUNT;
    for (int i = lane; i < MAX_RICE_PARTITION_COUNT; i += EXACT_WORKGROUP_SIZE)
        bestRiceParameters[outputBase + i] = taskRiceParameters[inputBase + i];
}
)CLC";
}

std::string cl_error_name(cl_int status)
{
    switch (status) {
    case CL_SUCCESS:
        return "CL_SUCCESS";
    case CL_DEVICE_NOT_FOUND:
        return "CL_DEVICE_NOT_FOUND";
    case CL_DEVICE_NOT_AVAILABLE:
        return "CL_DEVICE_NOT_AVAILABLE";
    case CL_COMPILER_NOT_AVAILABLE:
        return "CL_COMPILER_NOT_AVAILABLE";
    case CL_MEM_OBJECT_ALLOCATION_FAILURE:
        return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
    case CL_OUT_OF_RESOURCES:
        return "CL_OUT_OF_RESOURCES";
    case CL_OUT_OF_HOST_MEMORY:
        return "CL_OUT_OF_HOST_MEMORY";
    case CL_BUILD_PROGRAM_FAILURE:
        return "CL_BUILD_PROGRAM_FAILURE";
    case CL_INVALID_VALUE:
        return "CL_INVALID_VALUE";
    case CL_INVALID_PLATFORM:
        return "CL_INVALID_PLATFORM";
    case CL_INVALID_DEVICE:
        return "CL_INVALID_DEVICE";
    case CL_INVALID_CONTEXT:
        return "CL_INVALID_CONTEXT";
    case CL_INVALID_COMMAND_QUEUE:
        return "CL_INVALID_COMMAND_QUEUE";
    case CL_INVALID_MEM_OBJECT:
        return "CL_INVALID_MEM_OBJECT";
    case CL_INVALID_BINARY:
        return "CL_INVALID_BINARY";
    case CL_INVALID_BUILD_OPTIONS:
        return "CL_INVALID_BUILD_OPTIONS";
    case CL_INVALID_PROGRAM:
        return "CL_INVALID_PROGRAM";
    case CL_INVALID_PROGRAM_EXECUTABLE:
        return "CL_INVALID_PROGRAM_EXECUTABLE";
    case CL_INVALID_KERNEL_NAME:
        return "CL_INVALID_KERNEL_NAME";
    case CL_INVALID_KERNEL_DEFINITION:
        return "CL_INVALID_KERNEL_DEFINITION";
    case CL_INVALID_KERNEL:
        return "CL_INVALID_KERNEL";
    case CL_INVALID_ARG_INDEX:
        return "CL_INVALID_ARG_INDEX";
    case CL_INVALID_ARG_VALUE:
        return "CL_INVALID_ARG_VALUE";
    case CL_INVALID_ARG_SIZE:
        return "CL_INVALID_ARG_SIZE";
    case CL_INVALID_KERNEL_ARGS:
        return "CL_INVALID_KERNEL_ARGS";
    case CL_INVALID_WORK_DIMENSION:
        return "CL_INVALID_WORK_DIMENSION";
    case CL_INVALID_WORK_GROUP_SIZE:
        return "CL_INVALID_WORK_GROUP_SIZE";
    case CL_INVALID_GLOBAL_OFFSET:
        return "CL_INVALID_GLOBAL_OFFSET";
    case CL_INVALID_EVENT:
        return "CL_INVALID_EVENT";
    case CL_INVALID_OPERATION:
        return "CL_INVALID_OPERATION";
    case CL_INVALID_BUFFER_SIZE:
        return "CL_INVALID_BUFFER_SIZE";
    default:
        if (status == kPlatformNotFound) {
            return "CL_PLATFORM_NOT_FOUND_KHR";
        }
        return "OpenCL error " + std::to_string(status);
    }
}

void require_cl(cl_int status, const char* operation)
{
    if (status != CL_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + cl_error_name(status));
    }
}

std::string trim_trailing_nul(std::string value)
{
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string get_device_string(cl_device_id device, cl_device_info param)
{
    std::size_t size = 0;
    require_cl(clGetDeviceInfo(device, param, 0, nullptr, &size), "clGetDeviceInfo");
    std::string value(size, '\0');
    require_cl(clGetDeviceInfo(device, param, value.size(), value.data(), nullptr),
        "clGetDeviceInfo");
    return trim_trailing_nul(std::move(value));
}

template <typename T>
T get_device_value(cl_device_id device, cl_device_info param)
{
    T value {};
    require_cl(clGetDeviceInfo(device, param, sizeof(value), &value, nullptr), "clGetDeviceInfo");
    return value;
}

struct SelectedOpenClDevice {
    cl_device_id id = nullptr;
    std::string name;
};

SelectedOpenClDevice choose_opencl_device(std::optional<std::size_t> requested_index)
{
    cl_uint platform_count = 0;
    const cl_int platform_status = clGetPlatformIDs(0, nullptr, &platform_count);
    if (platform_status == kPlatformNotFound || platform_count == 0) {
        throw std::runtime_error("no OpenCL platforms found");
    }
    require_cl(platform_status, "clGetPlatformIDs");

    std::vector<cl_platform_id> platforms(platform_count);
    require_cl(clGetPlatformIDs(platform_count, platforms.data(), nullptr), "clGetPlatformIDs");

    std::size_t flat_index = 0;
    for (const auto platform : platforms) {
        cl_uint device_count = 0;
        const cl_int device_count_status =
            clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count);
        if (device_count_status == CL_DEVICE_NOT_FOUND || device_count == 0) {
            continue;
        }
        require_cl(device_count_status, "clGetDeviceIDs");

        std::vector<cl_device_id> devices(device_count);
        require_cl(clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, device_count, devices.data(), nullptr),
            "clGetDeviceIDs");

        for (const auto device : devices) {
            const bool available =
                get_device_value<cl_bool>(device, CL_DEVICE_AVAILABLE) == CL_TRUE;
            if (requested_index.has_value()) {
                if (flat_index == *requested_index) {
                    if (!available) {
                        throw std::runtime_error("selected OpenCL device is not available");
                    }
                    return SelectedOpenClDevice {device, get_device_string(device, CL_DEVICE_NAME)};
                }
            } else if (available) {
                return SelectedOpenClDevice {device, get_device_string(device, CL_DEVICE_NAME)};
            }
            ++flat_index;
        }
    }

    if (requested_index.has_value()) {
        throw std::runtime_error("OpenCL device index out of range: " +
            std::to_string(*requested_index));
    }
    throw std::runtime_error("no available OpenCL devices found");
}

template <typename Handle, cl_int (*Release)(Handle)>
class ClHandle {
public:
    ClHandle() = default;
    explicit ClHandle(Handle handle)
        : handle_(handle)
    {
    }
    ClHandle(const ClHandle&) = delete;
    ClHandle& operator=(const ClHandle&) = delete;
    ClHandle(ClHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }
    ClHandle& operator=(ClHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ~ClHandle()
    {
        reset();
    }

    void reset(Handle handle = nullptr)
    {
        if (handle_ != nullptr) {
            (void)Release(handle_);
        }
        handle_ = handle;
    }

    Handle get() const
    {
        return handle_;
    }

private:
    Handle handle_ = nullptr;
};

using ClContext = ClHandle<cl_context, clReleaseContext>;
using ClCommandQueue = ClHandle<cl_command_queue, clReleaseCommandQueue>;
using ClProgram = ClHandle<cl_program, clReleaseProgram>;
using ClKernel = ClHandle<cl_kernel, clReleaseKernel>;
using ClMem = ClHandle<cl_mem, clReleaseMemObject>;

std::string program_build_log(cl_program program, cl_device_id device)
{
    std::size_t size = 0;
    const auto status = clGetProgramBuildInfo(
        program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size);
    if (status != CL_SUCCESS || size == 0) {
        return {};
    }
    std::string log(size, '\0');
    if (clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log.size(),
            log.data(), nullptr) != CL_SUCCESS) {
        return {};
    }
    return trim_trailing_nul(std::move(log));
}

struct OpenClGeneratedAnalysisRuntime {
    SelectedOpenClDevice selected_device;
    ClContext context;
    ClCommandQueue queue;
    ClProgram program;
    ClKernel wasted_kernel;
    ClKernel autocor_kernel;
    ClKernel lpc_kernel;
    ClKernel quantize_kernel;
    ClKernel fixed_guess_kernel;
    ClKernel exact_kernel;
    ClKernel choose_kernel;
    ClKernel rice_kernel;
    ClMem samples_buffer;
    ClMem shifted_samples_buffer;
    ClMem tasks_buffer;
    ClMem selected_buffer;
    ClMem window_buffer;
    ClMem autocor_buffer;
    ClMem lpc_buffer;
    ClMem best_buffer;
    ClMem task_rice_parameters_buffer;
    ClMem best_rice_parameters_buffer;
    std::vector<float> cached_window;
    std::size_t samples_buffer_bytes = 0;
    std::size_t shifted_samples_buffer_bytes = 0;
    std::size_t tasks_buffer_bytes = 0;
    std::size_t selected_buffer_bytes = 0;
    std::size_t window_buffer_bytes = 0;
    std::size_t autocor_buffer_bytes = 0;
    std::size_t lpc_buffer_bytes = 0;
    std::size_t best_buffer_bytes = 0;
    std::size_t task_rice_parameters_buffer_bytes = 0;
    std::size_t best_rice_parameters_buffer_bytes = 0;
    std::size_t cached_window_blocksize = 0;
    std::size_t cached_window_count = 0;
    NativeAnalysisProfile cached_window_profile = NativeAnalysisProfile::Exact;
    bool cached_window_valid = false;
};

OpenClGeneratedAnalysisRuntime make_opencl_generated_analysis_runtime(
    std::optional<std::size_t> requested_device_index,
    OpenClGeneratedSetupTimings* setup_timings = nullptr)
{
    OpenClGeneratedAnalysisRuntime runtime;
    auto setup_started = Clock::now();
    runtime.selected_device = choose_opencl_device(requested_device_index);
    if (setup_timings != nullptr) {
        add_elapsed_ns(setup_timings->device_ns, setup_started);
    }

    cl_int status = CL_SUCCESS;
    setup_started = Clock::now();
    runtime.context.reset(clCreateContext(
        nullptr, 1, &runtime.selected_device.id, nullptr, nullptr, &status));
    require_cl(status, "clCreateContext");
    if (setup_timings != nullptr) {
        add_elapsed_ns(setup_timings->context_ns, setup_started);
    }

    setup_started = Clock::now();
    runtime.queue.reset(clCreateCommandQueue(
        runtime.context.get(), runtime.selected_device.id, 0, &status));
    require_cl(status, "clCreateCommandQueue");
    if (setup_timings != nullptr) {
        add_elapsed_ns(setup_timings->queue_ns, setup_started);
    }

    setup_started = Clock::now();
    const char* source = mono_analysis_kernel_source();
    const std::size_t source_length = std::char_traits<char>::length(source);
    runtime.program.reset(clCreateProgramWithSource(
        runtime.context.get(), 1, &source, &source_length, &status));
    require_cl(status, "clCreateProgramWithSource");
    if (setup_timings != nullptr) {
        add_elapsed_ns(setup_timings->program_source_ns, setup_started);
    }

    setup_started = Clock::now();
    const cl_int build_status = clBuildProgram(
        runtime.program.get(), 1, &runtime.selected_device.id, nullptr, nullptr, nullptr);
    if (build_status != CL_SUCCESS) {
        const auto log = program_build_log(runtime.program.get(), runtime.selected_device.id);
        throw std::runtime_error("clBuildProgram failed: " + cl_error_name(build_status) +
            (log.empty() ? std::string {} : "\n" + log));
    }
    if (setup_timings != nullptr) {
        add_elapsed_ns(setup_timings->program_build_ns, setup_started);
    }

    setup_started = Clock::now();
    runtime.wasted_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressFindWastedBits", &status));
    require_cl(status, "clCreateKernel(ldcompressFindWastedBits)");
    runtime.autocor_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressComputeAutocor", &status));
    require_cl(status, "clCreateKernel(ldcompressComputeAutocor)");
    runtime.lpc_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressComputeLpc", &status));
    require_cl(status, "clCreateKernel(ldcompressComputeLpc)");
    runtime.quantize_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressQuantizeLpcOrders", &status));
    require_cl(status, "clCreateKernel(ldcompressQuantizeLpcOrders)");
    runtime.fixed_guess_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressPruneFixedOrderGuess", &status));
    require_cl(status, "clCreateKernel(ldcompressPruneFixedOrderGuess)");
    runtime.exact_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressAnalyzeSubframeExact", &status));
    require_cl(status, "clCreateKernel(ldcompressAnalyzeSubframeExact)");
    runtime.choose_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressChooseBestMethod", &status));
    require_cl(status, "clCreateKernel(ldcompressChooseBestMethod)");
    runtime.rice_kernel.reset(
        clCreateKernel(runtime.program.get(), "ldcompressWriteBestRiceParameters", &status));
    require_cl(status, "clCreateKernel(ldcompressWriteBestRiceParameters)");
    if (setup_timings != nullptr) {
        add_elapsed_ns(setup_timings->kernels_ns, setup_started);
    }

    return runtime;
}

void ensure_opencl_buffer(
    const ClContext& context,
    ClMem& buffer,
    std::size_t& capacity_bytes,
    cl_mem_flags flags,
    std::size_t required_bytes,
    const char* name)
{
    if (required_bytes == 0) {
        throw std::runtime_error(std::string("OpenCL ") + name + " buffer is empty");
    }
    if (required_bytes <= capacity_bytes) {
        return;
    }

    cl_int status = CL_SUCCESS;
    buffer.reset(clCreateBuffer(context.get(), flags, required_bytes, nullptr, &status));
    require_cl(status, (std::string("clCreateBuffer(") + name + ")").c_str());
    capacity_bytes = required_bytes;
}

void write_opencl_buffer(
    const ClCommandQueue& queue,
    const ClMem& buffer,
    const void* data,
    std::size_t bytes,
    const char* name)
{
    require_cl(clEnqueueWriteBuffer(
                   queue.get(), buffer.get(), CL_TRUE, 0, bytes, data, 0, nullptr, nullptr),
        (std::string("clEnqueueWriteBuffer(") + name + ")").c_str());
}

std::size_t exact_leaf_rice_local_bytes(
    const OpenClMonoAnalysisTaskPlan& plan,
    NativeAnalysisProfile analysis_profile,
    unsigned max_rice_partition_order)
{
    if (max_rice_partition_order > kOpenClExactLeafMaxRicePartitionOrder ||
        plan.residual_tasks.empty()) {
        return sizeof(std::uint64_t);
    }
    if (analysis_profile_uses_mean_estimated_size(analysis_profile)) {
        return (std::size_t {1} << max_rice_partition_order) *
            sizeof(std::uint64_t);
    }
    if (analysis_profile_uses_mean_rice(analysis_profile)) {
        return sizeof(std::uint64_t);
    }
    return (std::size_t {1} << max_rice_partition_order) *
        kOpenClExactLeafRiceParameterCount *
        sizeof(std::uint64_t);
}

void enqueue_opencl_best_rice_parameters(
    const ClCommandQueue& queue,
    const ClKernel& kernel,
    const ClMem& tasks_buffer,
    const ClMem& selected_buffer,
    const ClMem& task_rice_parameters_buffer,
    const ClMem& best_rice_parameters_buffer,
    std::size_t frame_count,
    std::size_t tasks_per_frame)
{
    cl_mem tasks_mem = tasks_buffer.get();
    cl_mem selected_mem = selected_buffer.get();
    cl_mem task_rice_mem = task_rice_parameters_buffer.get();
    cl_mem best_rice_mem = best_rice_parameters_buffer.get();
    const auto task_count = static_cast<std::int32_t>(tasks_per_frame);

    require_cl(clSetKernelArg(kernel.get(), 0, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(rice.tasks)");
    require_cl(clSetKernelArg(kernel.get(), 1, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(rice.selectedTasks)");
    require_cl(clSetKernelArg(kernel.get(), 2, sizeof(task_rice_mem), &task_rice_mem),
        "clSetKernelArg(rice.taskRiceParameters)");
    require_cl(clSetKernelArg(kernel.get(), 3, sizeof(best_rice_mem), &best_rice_mem),
        "clSetKernelArg(rice.bestRiceParameters)");
    require_cl(clSetKernelArg(kernel.get(), 4, sizeof(task_count), &task_count),
        "clSetKernelArg(rice.taskCount)");

    const std::size_t local_work_size = kOpenClExactWorkgroupSize;
    const std::size_t global_work_size = frame_count * local_work_size;
    require_cl(clEnqueueNDRangeKernel(queue.get(), kernel.get(), 1, nullptr,
                   &global_work_size, &local_work_size, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressWriteBestRiceParameters)");
}

#endif

}  // namespace

std::size_t mono_analysis_tasks_per_frame(const OpenClMonoAnalysisTaskOptions& options)
{
    if (options.frame_samples == 0 || options.min_fixed_order > options.max_fixed_order ||
        options.min_fixed_order > 4 || options.max_fixed_order > 4) {
        return 0;
    }

    return generated_lpc_task_count(options) +
        (options.include_constant ? 1U : 0U) +
        fixed_task_count(options);
}

OpenClMonoAnalysisTaskPlan build_mono_analysis_task_plan(
    std::size_t frame_count,
    const OpenClMonoAnalysisTaskOptions& options)
{
    validate_options(options);
    const auto tasks_per_frame = mono_analysis_tasks_per_frame(options);
    if (frame_count != 0 &&
        frame_count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) / tasks_per_frame) {
        throw std::runtime_error("OpenCL mono analysis selected task index exceeds int32");
    }
    if (frame_count != 0 &&
        frame_count - 1U >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) / options.frame_samples) {
        throw std::runtime_error("OpenCL mono analysis frame offset exceeds int32");
    }

    OpenClMonoAnalysisTaskPlan plan;
    plan.residual_tasks_per_frame = tasks_per_frame;
    plan.estimate_tasks_per_frame = tasks_per_frame;
    plan.analysis_profile = options.analysis_profile;
    plan.max_lpc_order = options.max_lpc_order;
    plan.residual_tasks.reserve(frame_count * tasks_per_frame);
    plan.selected_tasks.reserve(frame_count * tasks_per_frame);

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto frame_task_base = plan.residual_tasks.size();
        if (generated_profile_uses_order_guess(options.analysis_profile)) {
            const auto lpc_window_count = generated_lpc_task_count(options);
            for (std::size_t window = 0; window < lpc_window_count; ++window) {
                plan.residual_tasks.push_back(
                    make_common_task(options, frame, kFlacClSubframeLpc, 1));
            }
        } else {
            for (std::size_t window = 0; window < kGeneratedLpcBaseWindowCount; ++window) {
                for (unsigned order = 1; order <= options.max_lpc_order; ++order) {
                    plan.residual_tasks.push_back(
                        make_common_task(options, frame, kFlacClSubframeLpc, order));
                }
            }
            const auto welch_candidate_count = generated_welch_candidate_count(options);
            const auto first_welch_order =
                options.max_lpc_order - static_cast<unsigned>(welch_candidate_count) + 1U;
            for (std::size_t i = 0; i < welch_candidate_count; ++i) {
                plan.residual_tasks.push_back(make_common_task(
                    options,
                    frame,
                    kFlacClSubframeLpc,
                    first_welch_order + static_cast<unsigned>(i)));
            }
        }

        if (options.include_constant) {
            auto task = make_common_task(options, frame, kFlacClSubframeConstant, 1);
            task.coefs[0] = 1;
            plan.residual_tasks.push_back(task);
        }

        const auto largest_usable = static_cast<unsigned>(options.frame_samples - 1U);
        const auto max_fixed = options.max_fixed_order < largest_usable ? options.max_fixed_order : largest_usable;
        for (unsigned order = options.min_fixed_order; order <= max_fixed; ++order) {
            auto task = make_common_task(options, frame, kFlacClSubframeFixed, order);
            populate_fixed_coefficients(task, order);
            plan.residual_tasks.push_back(task);
        }

        for (std::size_t i = 0; i < tasks_per_frame; ++i) {
            plan.selected_tasks.push_back(checked_i32(frame_task_base + i, "OpenCL mono analysis selected task"));
        }
    }

    return plan;
}

OpenClMonoAnalysisTaskPlan build_mono_analysis_task_plan_for_samples(
    const std::vector<std::int32_t>& samples,
    std::size_t frame_count,
    const OpenClMonoAnalysisTaskOptions& options,
    OpenClTaskPlanTimings* timings)
{
    if (!generated_profile_uses_order_guess(options.analysis_profile) ||
        options.min_fixed_order != 0) {
        auto plan = build_mono_analysis_task_plan(frame_count, options);
        apply_mono_analysis_profile_to_plan(samples, plan);
        return plan;
    }

    validate_options(options);
    if (frame_count != 0 &&
        frame_count - 1U >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) / options.frame_samples) {
        throw std::runtime_error("OpenCL mono analysis frame offset exceeds int32");
    }
    const auto required_samples = frame_count * static_cast<std::size_t>(options.frame_samples);
    if (required_samples > samples.size()) {
        throw std::runtime_error("OpenCL mono sampled task plan samples are out of range");
    }

    const auto lpc_tasks_per_frame = generated_lpc_task_count(options);
    const auto available_fixed_tasks = fixed_task_count(options);
    const bool fixed_order_guess_on_gpu =
        options.use_gpu_fixed_order_guess && available_fixed_tasks > 1;
    const auto compact_fixed_tasks = fixed_order_guess_on_gpu
        ? available_fixed_tasks
        : (available_fixed_tasks == 0 ? 0U : 1U);
    const auto tasks_per_frame =
        lpc_tasks_per_frame +
        (options.include_constant ? 1U : 0U) +
        compact_fixed_tasks;
    if (tasks_per_frame == 0 || tasks_per_frame > kFlacClMaxOrder) {
        throw std::runtime_error("OpenCL mono sampled task plan has invalid task count");
    }
    if (frame_count != 0 &&
        frame_count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) / tasks_per_frame) {
        throw std::runtime_error("OpenCL mono analysis selected task index exceeds int32");
    }

    OpenClMonoAnalysisTaskPlan plan;
    plan.residual_tasks_per_frame = tasks_per_frame;
    plan.estimate_tasks_per_frame = tasks_per_frame;
    plan.analysis_profile = options.analysis_profile;
    plan.max_lpc_order = options.max_lpc_order;
    plan.fixed_order_guess_on_gpu = fixed_order_guess_on_gpu;
    const auto total_tasks = frame_count * tasks_per_frame;
    const auto fill_started = Clock::now();
    std::uint64_t fixed_guess_ns = 0;
    plan.residual_tasks.resize(total_tasks);
    plan.selected_tasks.resize(total_tasks);
    std::iota(plan.selected_tasks.begin(), plan.selected_tasks.end(), std::int32_t {0});

    const auto largest_usable = static_cast<unsigned>(options.frame_samples - 1U);
    const auto max_fixed =
        options.max_fixed_order < largest_usable ? options.max_fixed_order : largest_usable;
    const auto raw_bits = checked_i32(
        static_cast<std::uint64_t>(options.bits_per_sample) * options.frame_samples,
        "OpenCL mono analysis size");
    const auto obits = checked_i32(options.bits_per_sample, "OpenCL mono analysis obits");
    const auto blocksize = checked_i32(options.frame_samples, "OpenCL mono analysis blocksize");
    const auto coding_method = options.bits_per_sample > 16 ? 1 : 0;
    auto make_task_for_frame = [&](
                                   std::int32_t samples_offset,
                                   std::int32_t type,
                                   unsigned residual_order) {
        FlacClSubframeTask task;
        task.data.residualOrder =
            checked_i32(residual_order, "OpenCL mono analysis residualOrder");
        task.data.samplesOffs = samples_offset;
        task.data.shift = 0;
        task.data.cbits = 0;
        task.data.size = raw_bits;
        task.data.type = type;
        task.data.obits = obits;
        task.data.blocksize = blocksize;
        task.data.coding_method = coding_method;
        task.data.channel = 0;
        task.data.residualOffs = samples_offset;
        task.data.wbits = 0;
        task.data.abits = task.data.obits;
        task.data.porder = 0;
        task.data.headerLen = 0;
        task.data.encodingOffset = 0;
        return task;
    };
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto frame_task_base = frame * tasks_per_frame;
        const auto samples_offset = checked_i32(
            static_cast<std::uint64_t>(frame) * options.frame_samples,
            "OpenCL mono analysis samplesOffs");
        for (std::size_t window = 0; window < lpc_tasks_per_frame; ++window) {
            plan.residual_tasks[frame_task_base + window] =
                make_task_for_frame(samples_offset, kFlacClSubframeLpc, 1);
        }

        auto task_index = frame_task_base + lpc_tasks_per_frame;
        if (options.include_constant) {
            auto task = make_task_for_frame(samples_offset, kFlacClSubframeConstant, 1);
            task.coefs[0] = 1;
            plan.residual_tasks[task_index] = task;
            ++task_index;
        }

        if (fixed_order_guess_on_gpu) {
            for (unsigned order = 0; order <= max_fixed; ++order) {
                auto task = make_task_for_frame(samples_offset, kFlacClSubframeFixed, order);
                populate_fixed_coefficients(task, order);
                plan.residual_tasks[task_index] = task;
                ++task_index;
            }
        } else if (compact_fixed_tasks != 0) {
            const auto offset = frame * static_cast<std::size_t>(options.frame_samples);
            unsigned order = 0;
            if (timings == nullptr) {
                order = guess_fixed_predictor_order_for_frame(
                    samples, offset, options.frame_samples, max_fixed);
            } else {
                const auto guess_started = Clock::now();
                order = guess_fixed_predictor_order_for_frame(
                    samples, offset, options.frame_samples, max_fixed);
                fixed_guess_ns += elapsed_ns_since(guess_started);
            }
            auto task = make_task_for_frame(samples_offset, kFlacClSubframeFixed, order);
            populate_fixed_coefficients(task, order);
            plan.residual_tasks[task_index] = task;
        }
    }
    if (timings != nullptr) {
        timings->fixed_order_guess_ns += fixed_guess_ns;
        timings->task_fill_ns += elapsed_ns_since(fill_started) - fixed_guess_ns;
    }

    return plan;
}

void apply_mono_analysis_profile_to_plan(
    const std::vector<std::int32_t>& samples,
    OpenClMonoAnalysisTaskPlan& plan)
{
    if (!generated_profile_uses_order_guess(plan.analysis_profile)) {
        return;
    }

    const auto frame_count = mono_plan_frame_count(plan);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        const auto& first_task = plan.residual_tasks.at(task_base);
        if (first_task.data.samplesOffs < 0 || first_task.data.blocksize <= 0) {
            throw std::runtime_error("OpenCL mono profile pruning received invalid frame task");
        }
        const auto offset = static_cast<std::size_t>(first_task.data.samplesOffs);
        const auto block_size = static_cast<std::size_t>(first_task.data.blocksize);
        if (offset > samples.size() || block_size > samples.size() - offset) {
            throw std::runtime_error("OpenCL mono profile pruning task samples are out of range");
        }
        const auto bits_per_sample = static_cast<unsigned>(first_task.data.obits);
        const auto wasted_bits = common_wasted_bits(samples, offset, block_size, bits_per_sample);
        const auto shifted = shifted_frame_samples(samples, offset, block_size, wasted_bits);

        unsigned max_fixed_order = 0;
        for (std::size_t i = 0; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = plan.residual_tasks[task_base + i];
            if (task.data.type == kFlacClSubframeFixed &&
                task.data.residualOrder >= 0 &&
                task.data.residualOrder <= 4) {
                max_fixed_order = std::max(
                    max_fixed_order,
                    static_cast<unsigned>(task.data.residualOrder));
            }
        }
        const auto guessed_fixed_order =
            guess_fixed_predictor_order(shifted, max_fixed_order);
        for (std::size_t i = 0; i < plan.residual_tasks_per_frame; ++i) {
            auto& task = plan.residual_tasks[task_base + i];
            if (task.data.type == kFlacClSubframeFixed &&
                static_cast<unsigned>(task.data.residualOrder) != guessed_fixed_order) {
                mark_task_pruned(task);
            }
        }
    }
}

FlacSubframeDecision flaccl_task_to_subframe_decision(
    const FlacClSubframeTask& task)
{
    if (task.data.size < 0 || task.data.wbits < 0 || task.data.residualOrder < 0) {
        throw std::runtime_error("OpenCL FLACCL task contains negative decision fields");
    }

    FlacSubframeDecision decision;
    decision.wasted_bits = static_cast<unsigned>(task.data.wbits);
    decision.estimated_bits = static_cast<std::uint64_t>(task.data.size);

    switch (task.data.type) {
    case kFlacClSubframeConstant:
        decision.kind = FlacSubframeKind::Constant;
        break;
    case kFlacClSubframeFixed:
        if (task.data.porder < 0) {
            throw std::runtime_error("OpenCL FLACCL fixed task has invalid partition order");
        }
        if (task.data.residualOrder > 4) {
            throw std::runtime_error("OpenCL FLACCL fixed task has invalid order");
        }
        decision.kind = FlacSubframeKind::FixedRice;
        decision.fixed_order = static_cast<unsigned>(task.data.residualOrder);
        decision.rice_partition_order = static_cast<unsigned>(task.data.porder);
        break;
    case kFlacClSubframeLpc:
        if (task.data.porder < 0) {
            throw std::runtime_error("OpenCL FLACCL LPC task has invalid partition order");
        }
        if (task.data.residualOrder == 0 ||
            task.data.residualOrder > static_cast<std::int32_t>(kFlacClMaxOrder)) {
            throw std::runtime_error("OpenCL FLACCL LPC task has invalid order");
        }
        decision.kind = FlacSubframeKind::LpcRice;
        decision.lpc_order = static_cast<unsigned>(task.data.residualOrder);
        decision.rice_partition_order = static_cast<unsigned>(task.data.porder);
        break;
    default:
        throw std::runtime_error("OpenCL FLACCL task type cannot be mapped to a native decision");
    }

    return decision;
}

FlacSelectedSubframe flaccl_task_to_selected_subframe(
    const FlacClSubframeTask& task)
{
    const auto decision = flaccl_task_to_subframe_decision(task);

    FlacSelectedSubframe selected;
    selected.kind = decision.kind;
    selected.fixed_order = decision.fixed_order;
    selected.lpc_order = decision.lpc_order;
    selected.rice_partition_order = decision.rice_partition_order;
    selected.wasted_bits = decision.wasted_bits;

    if (decision.kind == FlacSubframeKind::LpcRice) {
        if (task.data.cbits <= 0 || task.data.cbits > 15) {
            throw std::runtime_error("OpenCL FLACCL LPC task has invalid coefficient precision");
        }
        if (task.data.shift < 0 || task.data.shift > 15) {
            throw std::runtime_error("OpenCL FLACCL LPC task has invalid quantization shift");
        }
        selected.coefficient_precision = static_cast<unsigned>(task.data.cbits);
        selected.quantization_shift = task.data.shift;
        selected.coefficients.reserve(static_cast<std::size_t>(task.data.residualOrder));
        for (int i = task.data.residualOrder - 1; i >= 0; --i) {
            const auto coefficient = task.coefs[static_cast<std::size_t>(i)];
            if (!signed_value_fits_bits(coefficient, selected.coefficient_precision)) {
                throw std::runtime_error("OpenCL FLACCL LPC coefficient does not fit precision");
            }
            selected.coefficients.push_back(coefficient);
        }
    }

    return selected;
}

std::vector<unsigned> flaccl_task_to_selected_rice_parameters(
    const FlacClSubframeTask& task,
    const FlacClRiceParameterSet& rice_parameters)
{
    if (task.data.type != kFlacClSubframeFixed &&
        task.data.type != kFlacClSubframeLpc) {
        return {};
    }
    if (task.data.porder < 0 ||
        task.data.porder >
            static_cast<std::int32_t>(kFlacClMaxRicePartitionOrder)) {
        throw std::runtime_error("OpenCL selected task has invalid Rice partition order");
    }

    const auto partition_count =
        std::size_t {1} << static_cast<unsigned>(task.data.porder);
    std::vector<unsigned> selected;
    selected.reserve(partition_count);
    for (std::size_t i = 0; i < partition_count; ++i) {
        const auto parameter = rice_parameters.parameters.at(i);
        if (parameter > kExactMaxRiceParameter) {
            throw std::runtime_error("OpenCL selected task has invalid Rice parameter");
        }
        selected.push_back(static_cast<unsigned>(parameter));
    }
    return selected;
}

OpenClMonoBestMethodResult internal::execute_opencl_best_reduction(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index)
{
#if LDCOMPRESS_HAVE_OPENCL
    validate_best_method_plan(plan);

    const auto selected_device = choose_opencl_device(requested_device_index);

    cl_int status = CL_SUCCESS;
    ClContext context(clCreateContext(nullptr, 1, &selected_device.id, nullptr, nullptr, &status));
    require_cl(status, "clCreateContext");

    ClCommandQueue queue(clCreateCommandQueue(context.get(), selected_device.id, 0, &status));
    require_cl(status, "clCreateCommandQueue");

    const char* source = mono_analysis_kernel_source();
    const std::size_t source_length = std::char_traits<char>::length(source);
    ClProgram program(clCreateProgramWithSource(context.get(), 1, &source, &source_length, &status));
    require_cl(status, "clCreateProgramWithSource");

    const cl_int build_status = clBuildProgram(program.get(), 1, &selected_device.id, nullptr, nullptr, nullptr);
    if (build_status != CL_SUCCESS) {
        const auto log = program_build_log(program.get(), selected_device.id);
        throw std::runtime_error("clBuildProgram failed: " + cl_error_name(build_status) +
            (log.empty() ? std::string {} : "\n" + log));
    }

    ClKernel kernel(clCreateKernel(program.get(), "ldcompressChooseBestMethod", &status));
    require_cl(status, "clCreateKernel");

    ClMem tasks_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        plan.residual_tasks.size() * sizeof(FlacClSubframeTask),
        const_cast<FlacClSubframeTask*>(plan.residual_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(tasks)");

    ClMem selected_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        plan.selected_tasks.size() * sizeof(std::int32_t),
        const_cast<std::int32_t*>(plan.selected_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(selectedTasks)");

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    std::vector<FlacClSubframeTask> best_tasks(frame_count);
    ClMem best_buffer(clCreateBuffer(context.get(),
        CL_MEM_WRITE_ONLY,
        best_tasks.size() * sizeof(FlacClSubframeTask),
        nullptr,
        &status));
    require_cl(status, "clCreateBuffer(tasks_out)");

    cl_mem best_mem = best_buffer.get();
    cl_mem tasks_mem = tasks_buffer.get();
    cl_mem selected_mem = selected_buffer.get();
    const auto task_count = static_cast<std::int32_t>(plan.estimate_tasks_per_frame);

    require_cl(clSetKernelArg(kernel.get(), 0, sizeof(best_mem), &best_mem), "clSetKernelArg(tasks_out)");
    require_cl(clSetKernelArg(kernel.get(), 1, sizeof(tasks_mem), &tasks_mem), "clSetKernelArg(tasks)");
    require_cl(clSetKernelArg(kernel.get(), 2, sizeof(selected_mem), &selected_mem), "clSetKernelArg(selectedTasks)");
    require_cl(clSetKernelArg(kernel.get(), 3, sizeof(task_count), &task_count), "clSetKernelArg(taskCount)");

    const std::size_t global_work_size = frame_count;
    require_cl(clEnqueueNDRangeKernel(queue.get(), kernel.get(), 1, nullptr, &global_work_size,
                   nullptr, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel");
    require_cl(clEnqueueReadBuffer(queue.get(), best_buffer.get(), CL_TRUE, 0,
                   best_tasks.size() * sizeof(FlacClSubframeTask), best_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer");
    require_cl(clFinish(queue.get()), "clFinish");

    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(best_tasks),
        .best_rice_parameters = {},
        .device_name = selected_device.name,
    };
#else
    (void)plan;
    (void)requested_device_index;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult internal::execute_opencl_exact_task_batch(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order,
    bool allow_lpc)
{
#if LDCOMPRESS_HAVE_OPENCL
    if (allow_lpc) {
        validate_lpc_analysis_inputs(samples, plan);
    } else {
        validate_fixed_constant_analysis_inputs(samples, plan);
    }
    if (max_rice_partition_order > kExactMaxRicePartitionOrder) {
        throw std::runtime_error("OpenCL exact analysis max Rice partition order must be 0..8");
    }

    const auto selected_device = choose_opencl_device(requested_device_index);

    cl_int status = CL_SUCCESS;
    ClContext context(clCreateContext(nullptr, 1, &selected_device.id, nullptr, nullptr, &status));
    require_cl(status, "clCreateContext");

    ClCommandQueue queue(clCreateCommandQueue(context.get(), selected_device.id, 0, &status));
    require_cl(status, "clCreateCommandQueue");

    const char* source = mono_analysis_kernel_source();
    const std::size_t source_length = std::char_traits<char>::length(source);
    ClProgram program(clCreateProgramWithSource(context.get(), 1, &source, &source_length, &status));
    require_cl(status, "clCreateProgramWithSource");

    const cl_int build_status = clBuildProgram(program.get(), 1, &selected_device.id, nullptr, nullptr, nullptr);
    if (build_status != CL_SUCCESS) {
        const auto log = program_build_log(program.get(), selected_device.id);
        throw std::runtime_error("clBuildProgram failed: " + cl_error_name(build_status) +
            (log.empty() ? std::string {} : "\n" + log));
    }

    ClKernel wasted_kernel(clCreateKernel(program.get(), "ldcompressFindWastedBits", &status));
    require_cl(status, "clCreateKernel(ldcompressFindWastedBits)");
    ClKernel exact_kernel(clCreateKernel(program.get(), "ldcompressAnalyzeSubframeExact", &status));
    require_cl(status, "clCreateKernel(ldcompressAnalyzeSubframeExact)");
    ClKernel choose_kernel(clCreateKernel(program.get(), "ldcompressChooseBestMethod", &status));
    require_cl(status, "clCreateKernel(ldcompressChooseBestMethod)");
    ClKernel rice_kernel(
        clCreateKernel(program.get(), "ldcompressWriteBestRiceParameters", &status));
    require_cl(status, "clCreateKernel(ldcompressWriteBestRiceParameters)");

    ClMem samples_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        samples.size() * sizeof(std::int32_t),
        const_cast<std::int32_t*>(samples.data()),
        &status));
    require_cl(status, "clCreateBuffer(samples)");

    ClMem tasks_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
        plan.residual_tasks.size() * sizeof(FlacClSubframeTask),
        const_cast<FlacClSubframeTask*>(plan.residual_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(tasks)");

    ClMem selected_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
        plan.selected_tasks.size() * sizeof(std::int32_t),
        const_cast<std::int32_t*>(plan.selected_tasks.data()),
        &status));
    require_cl(status, "clCreateBuffer(selectedTasks)");

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    std::vector<FlacClSubframeTask> analyzed_tasks(plan.residual_tasks.size());
    std::vector<FlacClSubframeTask> best_tasks(frame_count);
    std::vector<FlacClRiceParameterSet> best_rice_parameters(frame_count);
    const auto task_rice_parameters_bytes =
        plan.residual_tasks.size() * sizeof(FlacClRiceParameterSet);
    ClMem best_buffer(clCreateBuffer(context.get(),
        CL_MEM_WRITE_ONLY,
        best_tasks.size() * sizeof(FlacClSubframeTask),
        nullptr,
        &status));
    require_cl(status, "clCreateBuffer(tasks_out)");
    ClMem task_rice_parameters_buffer(clCreateBuffer(context.get(),
        CL_MEM_READ_WRITE,
        task_rice_parameters_bytes,
        nullptr,
        &status));
    require_cl(status, "clCreateBuffer(taskRiceParameters)");
    ClMem best_rice_parameters_buffer(clCreateBuffer(context.get(),
        CL_MEM_WRITE_ONLY,
        best_rice_parameters.size() * sizeof(FlacClRiceParameterSet),
        nullptr,
        &status));
    require_cl(status, "clCreateBuffer(bestRiceParameters)");

    cl_mem tasks_mem = tasks_buffer.get();
    cl_mem samples_mem = samples_buffer.get();
    cl_mem shifted_samples_mem = tasks_mem;
    cl_mem selected_mem = selected_buffer.get();
    cl_mem best_mem = best_buffer.get();
    cl_mem task_rice_mem = task_rice_parameters_buffer.get();
    const auto task_count = static_cast<std::int32_t>(plan.estimate_tasks_per_frame);
    const std::int32_t use_shifted_samples = 0;

    require_cl(clSetKernelArg(wasted_kernel.get(), 0, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(wasted.tasks)");
    require_cl(clSetKernelArg(wasted_kernel.get(), 1, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(wasted.samples)");
    require_cl(clSetKernelArg(wasted_kernel.get(), 2, sizeof(task_count), &task_count),
        "clSetKernelArg(wasted.tasksPerChannel)");
    require_cl(clSetKernelArg(wasted_kernel.get(), 3, sizeof(shifted_samples_mem),
                   &shifted_samples_mem),
        "clSetKernelArg(wasted.shiftedSamples)");
    require_cl(clSetKernelArg(wasted_kernel.get(), 4, sizeof(use_shifted_samples),
                   &use_shifted_samples),
        "clSetKernelArg(wasted.writeShiftedSamples)");

    const std::size_t one = 1;
    const std::size_t frame_global_work_size = frame_count;
    require_cl(clEnqueueNDRangeKernel(queue.get(), wasted_kernel.get(), 1, nullptr,
                   &frame_global_work_size, &one, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressFindWastedBits)");

    const auto max_rice_partition_order_arg =
        static_cast<std::int32_t>(max_rice_partition_order);
    const auto exact_analysis_profile_arg =
        opencl_analysis_profile_arg(plan.analysis_profile);
    const auto exact_leaf_rice_bytes =
        exact_leaf_rice_local_bytes(
            plan, plan.analysis_profile, max_rice_partition_order);
    require_cl(clSetKernelArg(exact_kernel.get(), 0, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(exact.samples)");
    require_cl(clSetKernelArg(exact_kernel.get(), 1, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(exact.selectedTasks)");
    require_cl(clSetKernelArg(exact_kernel.get(), 2, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(exact.tasks)");
    require_cl(clSetKernelArg(exact_kernel.get(), 3, sizeof(max_rice_partition_order_arg),
                   &max_rice_partition_order_arg),
        "clSetKernelArg(exact.maxRicePartitionOrder)");
    require_cl(clSetKernelArg(exact_kernel.get(), 4, sizeof(exact_analysis_profile_arg),
                   &exact_analysis_profile_arg),
        "clSetKernelArg(exact.analysisProfile)");
    require_cl(clSetKernelArg(exact_kernel.get(), 5, sizeof(task_rice_mem), &task_rice_mem),
        "clSetKernelArg(exact.taskRiceParameters)");
    require_cl(clSetKernelArg(exact_kernel.get(), 6, exact_leaf_rice_bytes, nullptr),
        "clSetKernelArg(exact.leafRiceSums)");
    require_cl(clSetKernelArg(exact_kernel.get(), 7, sizeof(shifted_samples_mem),
                   &shifted_samples_mem),
        "clSetKernelArg(exact.shiftedSamples)");
    require_cl(clSetKernelArg(exact_kernel.get(), 8, sizeof(use_shifted_samples),
                   &use_shifted_samples),
        "clSetKernelArg(exact.useShiftedSamples)");

    const std::size_t exact_local_work_size = kOpenClExactWorkgroupSize;
    const std::size_t estimate_global_work_size =
        plan.selected_tasks.size() * exact_local_work_size;
    require_cl(clEnqueueNDRangeKernel(queue.get(), exact_kernel.get(), 1, nullptr,
                   &estimate_global_work_size, &exact_local_work_size, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressAnalyzeSubframeExact)");

    require_cl(clSetKernelArg(choose_kernel.get(), 0, sizeof(best_mem), &best_mem),
        "clSetKernelArg(choose.tasks_out)");
    require_cl(clSetKernelArg(choose_kernel.get(), 1, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(choose.tasks)");
    require_cl(clSetKernelArg(choose_kernel.get(), 2, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(choose.selectedTasks)");
    require_cl(clSetKernelArg(choose_kernel.get(), 3, sizeof(task_count), &task_count),
        "clSetKernelArg(choose.taskCount)");

    require_cl(clEnqueueNDRangeKernel(queue.get(), choose_kernel.get(), 1, nullptr,
                   &frame_global_work_size, nullptr, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressChooseBestMethod)");

    enqueue_opencl_best_rice_parameters(
        queue,
        rice_kernel,
        tasks_buffer,
        selected_buffer,
        task_rice_parameters_buffer,
        best_rice_parameters_buffer,
        frame_count,
        plan.estimate_tasks_per_frame);

    require_cl(clEnqueueReadBuffer(queue.get(), tasks_buffer.get(), CL_TRUE, 0,
                   analyzed_tasks.size() * sizeof(FlacClSubframeTask), analyzed_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer(tasks)");
    require_cl(clEnqueueReadBuffer(queue.get(), best_buffer.get(), CL_TRUE, 0,
                   best_tasks.size() * sizeof(FlacClSubframeTask), best_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer(tasks_out)");
    require_cl(clEnqueueReadBuffer(queue.get(), best_rice_parameters_buffer.get(), CL_TRUE, 0,
                   best_rice_parameters.size() * sizeof(FlacClRiceParameterSet),
                   best_rice_parameters.data(), 0, nullptr, nullptr),
        "clEnqueueReadBuffer(bestRiceParameters)");
    require_cl(clFinish(queue.get()), "clFinish");

    return OpenClMonoFixedConstantAnalysisResult {
        .analyzed_tasks = std::move(analyzed_tasks),
        .best_tasks = std::move(best_tasks),
        .best_rice_parameters = std::move(best_rice_parameters),
        .device_name = selected_device.name,
    };
#else
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)max_rice_partition_order;
    (void)allow_lpc;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_fixed_constant_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    return internal::execute_opencl_exact_task_batch(
        samples, plan, requested_device_index, max_rice_partition_order, false);
}

#if LDCOMPRESS_HAVE_OPENCL
OpenClMonoFixedConstantAnalysisResult execute_opencl_generated_analysis_validated(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    std::size_t lpc_tasks_per_window,
    std::size_t total_lpc_tasks,
    std::size_t generated_window_count,
    OpenClGeneratedAnalysisRuntime& runtime,
    bool read_analyzed_tasks,
    OpenClGeneratedAnalysisTimings* timings)
{
    if (lpc_tasks_per_window == 0 || lpc_tasks_per_window > kFlacClMaxOrder ||
        generated_window_count == 0 ||
        total_lpc_tasks == 0 ||
        total_lpc_tasks > plan.residual_tasks_per_frame ||
        total_lpc_tasks > lpc_tasks_per_window * generated_window_count) {
        throw std::runtime_error("OpenCL generated analysis LPC task count is invalid");
    }
    if (max_rice_partition_order > kExactMaxRicePartitionOrder) {
        throw std::runtime_error("OpenCL generated analysis max Rice partition order must be 0..8");
    }
    const bool use_shifted_samples = uses_shifted_speed_profile(
        plan,
        lpc_tasks_per_window,
        generated_window_count,
        max_rice_partition_order);

    if (timings != nullptr) {
        ++timings->batches;
    }
    auto finish_timed = [&](
                            std::uint64_t OpenClGeneratedAnalysisTimings::*counter,
                            Clock::time_point started,
                            const char* label) {
        if (timings != nullptr) {
            require_cl(clFinish(runtime.queue.get()), label);
            add_elapsed_ns(timings->*counter, started);
        }
    };

    const auto upload_started = Clock::now();
    const auto samples_bytes = samples.size() * sizeof(std::int32_t);
    ensure_opencl_buffer(runtime.context, runtime.samples_buffer,
        runtime.samples_buffer_bytes, CL_MEM_READ_ONLY, samples_bytes, "samples");
    write_opencl_buffer(
        runtime.queue, runtime.samples_buffer, samples.data(), samples_bytes, "samples");
    if (use_shifted_samples) {
        ensure_opencl_buffer(runtime.context, runtime.shifted_samples_buffer,
            runtime.shifted_samples_buffer_bytes, CL_MEM_READ_WRITE, samples_bytes,
            "shiftedSamples");
    }

    const auto tasks_bytes = plan.residual_tasks.size() * sizeof(FlacClSubframeTask);
    ensure_opencl_buffer(runtime.context, runtime.tasks_buffer,
        runtime.tasks_buffer_bytes, CL_MEM_READ_WRITE, tasks_bytes, "tasks");
    write_opencl_buffer(
        runtime.queue, runtime.tasks_buffer, plan.residual_tasks.data(), tasks_bytes, "tasks");

    const auto selected_bytes = plan.selected_tasks.size() * sizeof(std::int32_t);
    ensure_opencl_buffer(runtime.context, runtime.selected_buffer,
        runtime.selected_buffer_bytes, CL_MEM_READ_ONLY, selected_bytes, "selectedTasks");
    write_opencl_buffer(runtime.queue, runtime.selected_buffer,
        plan.selected_tasks.data(), selected_bytes, "selectedTasks");

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    const auto blocksize = static_cast<std::size_t>(plan.residual_tasks.front().data.blocksize);
    const bool window_cache_miss =
        !runtime.cached_window_valid ||
        runtime.cached_window_blocksize != blocksize ||
        runtime.cached_window_count != generated_window_count ||
        runtime.cached_window_profile != plan.analysis_profile;
    if (window_cache_miss) {
        runtime.cached_window =
            make_generated_lpc_windows(blocksize, generated_window_count, plan.analysis_profile);
        runtime.cached_window_blocksize = blocksize;
        runtime.cached_window_count = generated_window_count;
        runtime.cached_window_profile = plan.analysis_profile;
        runtime.cached_window_valid = true;
    }
    const auto window_bytes = runtime.cached_window.size() * sizeof(float);
    const bool upload_window =
        window_cache_miss || window_bytes > runtime.window_buffer_bytes;
    ensure_opencl_buffer(runtime.context, runtime.window_buffer,
        runtime.window_buffer_bytes, CL_MEM_READ_ONLY, window_bytes, "window");
    if (upload_window) {
        write_opencl_buffer(runtime.queue, runtime.window_buffer,
            runtime.cached_window.data(), window_bytes, "window");
    }
    if (timings != nullptr) {
        add_elapsed_ns(timings->upload_ns, upload_started);
    }

    const auto autocor_count = frame_count * generated_window_count * (kFlacClMaxOrder + 1U);
    const auto autocor_bytes = autocor_count * sizeof(float);
    ensure_opencl_buffer(runtime.context, runtime.autocor_buffer,
        runtime.autocor_buffer_bytes, CL_MEM_READ_WRITE, autocor_bytes, "autocor");

    const auto lpc_count =
        frame_count * generated_window_count * (kFlacClMaxOrder + 1U) * kFlacClMaxOrder;
    const auto lpc_bytes = lpc_count * sizeof(float);
    ensure_opencl_buffer(runtime.context, runtime.lpc_buffer,
        runtime.lpc_buffer_bytes, CL_MEM_READ_WRITE, lpc_bytes, "lpc");

    std::vector<FlacClSubframeTask> analyzed_tasks;
    if (read_analyzed_tasks) {
        analyzed_tasks.resize(plan.residual_tasks.size());
    }
    std::vector<FlacClSubframeTask> best_tasks(frame_count);
    const auto best_bytes = best_tasks.size() * sizeof(FlacClSubframeTask);
    ensure_opencl_buffer(runtime.context, runtime.best_buffer,
        runtime.best_buffer_bytes, CL_MEM_WRITE_ONLY, best_bytes, "tasks_out");
    const auto task_rice_parameters_bytes =
        plan.residual_tasks.size() * sizeof(FlacClRiceParameterSet);
    ensure_opencl_buffer(runtime.context, runtime.task_rice_parameters_buffer,
        runtime.task_rice_parameters_buffer_bytes, CL_MEM_READ_WRITE,
        task_rice_parameters_bytes, "taskRiceParameters");
    std::vector<FlacClRiceParameterSet> best_rice_parameters(frame_count);
    const auto best_rice_parameters_bytes =
        best_rice_parameters.size() * sizeof(FlacClRiceParameterSet);
    ensure_opencl_buffer(runtime.context, runtime.best_rice_parameters_buffer,
        runtime.best_rice_parameters_buffer_bytes, CL_MEM_WRITE_ONLY,
        best_rice_parameters_bytes, "bestRiceParameters");

    cl_mem tasks_mem = runtime.tasks_buffer.get();
    cl_mem samples_mem = runtime.samples_buffer.get();
    cl_mem shifted_samples_mem = use_shifted_samples
        ? runtime.shifted_samples_buffer.get()
        : tasks_mem;
    cl_mem selected_mem = runtime.selected_buffer.get();
    cl_mem window_mem = runtime.window_buffer.get();
    cl_mem autocor_mem = runtime.autocor_buffer.get();
    cl_mem lpc_mem = runtime.lpc_buffer.get();
    cl_mem best_mem = runtime.best_buffer.get();
    cl_mem task_rice_mem = runtime.task_rice_parameters_buffer.get();
    const auto residual_tasks_per_frame =
        static_cast<std::int32_t>(plan.residual_tasks_per_frame);
    const auto estimate_tasks_per_frame =
        static_cast<std::int32_t>(plan.estimate_tasks_per_frame);
    const std::int32_t use_shifted_samples_arg = use_shifted_samples ? 1 : 0;

    require_cl(clSetKernelArg(runtime.wasted_kernel.get(), 0, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(wasted.tasks)");
    require_cl(clSetKernelArg(runtime.wasted_kernel.get(), 1, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(wasted.samples)");
    require_cl(clSetKernelArg(runtime.wasted_kernel.get(), 2, sizeof(residual_tasks_per_frame),
                   &residual_tasks_per_frame),
        "clSetKernelArg(wasted.tasksPerChannel)");
    require_cl(clSetKernelArg(runtime.wasted_kernel.get(), 3, sizeof(shifted_samples_mem),
                   &shifted_samples_mem),
        "clSetKernelArg(wasted.shiftedSamples)");
    require_cl(clSetKernelArg(runtime.wasted_kernel.get(), 4, sizeof(use_shifted_samples_arg),
                   &use_shifted_samples_arg),
        "clSetKernelArg(wasted.writeShiftedSamples)");

    const std::size_t one = 1;
    const std::size_t frame_global_work_size = frame_count;
    const auto wasted_started = Clock::now();
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.wasted_kernel.get(), 1, nullptr,
                   &frame_global_work_size, &one, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressFindWastedBits)");
    finish_timed(
        &OpenClGeneratedAnalysisTimings::wasted_bits_ns,
        wasted_started,
        "clFinish(ldcompressFindWastedBits)");

    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 0, sizeof(autocor_mem), &autocor_mem),
        "clSetKernelArg(autocor.output)");
    const cl_mem autocor_samples_mem = use_shifted_samples ? shifted_samples_mem : samples_mem;
    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 1, sizeof(autocor_samples_mem),
                   &autocor_samples_mem),
        "clSetKernelArg(autocor.samples)");
    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 2, sizeof(window_mem), &window_mem),
        "clSetKernelArg(autocor.window)");
    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 3, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(autocor.tasks)");
    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 4, sizeof(residual_tasks_per_frame),
                   &residual_tasks_per_frame),
        "clSetKernelArg(autocor.tasksPerFrame)");
    require_cl(clSetKernelArg(runtime.autocor_kernel.get(), 5, sizeof(use_shifted_samples_arg),
                   &use_shifted_samples_arg),
        "clSetKernelArg(autocor.samplesAlreadyShifted)");

    const std::array<std::size_t, 2> generation_global {
        frame_count,
        generated_window_count,
    };
    const std::array<std::size_t, 2> generation_local { 1, 1 };
    const auto lpc_order_limit =
        std::min<std::size_t>(kFlacClMaxOrder, std::max<unsigned>(1U, plan.max_lpc_order));
    const std::array<std::size_t, 3> autocor_global {
        frame_count * kOpenClAutocorWorkgroupSize,
        generated_window_count,
        lpc_order_limit + 1U,
    };
    const std::array<std::size_t, 3> autocor_local {
        kOpenClAutocorWorkgroupSize,
        1,
        1,
    };
    const auto autocor_started = Clock::now();
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.autocor_kernel.get(), 3, nullptr,
                   autocor_global.data(), autocor_local.data(), 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressComputeAutocor)");
    finish_timed(
        &OpenClGeneratedAnalysisTimings::generated_autocorrelation_ns,
        autocor_started,
        "clFinish(ldcompressComputeAutocor)");

    const auto window_count_arg = static_cast<std::int32_t>(generated_window_count);
    const auto lpc_order_limit_arg = static_cast<std::int32_t>(lpc_order_limit);
    require_cl(clSetKernelArg(runtime.lpc_kernel.get(), 0, sizeof(autocor_mem), &autocor_mem),
        "clSetKernelArg(lpc.autocor)");
    require_cl(clSetKernelArg(runtime.lpc_kernel.get(), 1, sizeof(lpc_mem), &lpc_mem),
        "clSetKernelArg(lpc.lpcs)");
    require_cl(clSetKernelArg(runtime.lpc_kernel.get(), 2, sizeof(window_count_arg), &window_count_arg),
        "clSetKernelArg(lpc.windowCount)");
    require_cl(clSetKernelArg(runtime.lpc_kernel.get(), 3, sizeof(lpc_order_limit_arg),
                   &lpc_order_limit_arg),
        "clSetKernelArg(lpc.maxLpcOrder)");
    const auto lpc_started = Clock::now();
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.lpc_kernel.get(), 2, nullptr,
                   generation_global.data(), generation_local.data(), 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressComputeLpc)");
    finish_timed(
        &OpenClGeneratedAnalysisTimings::generated_lpc_ns,
        lpc_started,
        "clFinish(ldcompressComputeLpc)");

    const auto lpc_tasks_per_window_arg =
        static_cast<std::int32_t>(lpc_tasks_per_window);
    const auto total_lpc_tasks_arg =
        static_cast<std::int32_t>(total_lpc_tasks);
    const auto lpc_coefficient_precision_arg =
        static_cast<std::int32_t>(lpc_coefficient_precision);
    const auto max_lpc_order_arg =
        static_cast<std::int32_t>(plan.max_lpc_order);
    const auto analysis_profile_arg =
        opencl_analysis_profile_arg(plan.analysis_profile);
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 0, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(quantize.tasks)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 1, sizeof(lpc_mem), &lpc_mem),
        "clSetKernelArg(quantize.lpcs)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 2, sizeof(residual_tasks_per_frame),
                   &residual_tasks_per_frame),
        "clSetKernelArg(quantize.tasksPerFrame)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 3, sizeof(lpc_tasks_per_window_arg),
                   &lpc_tasks_per_window_arg),
        "clSetKernelArg(quantize.lpcTasksPerWindow)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 4, sizeof(total_lpc_tasks_arg),
                   &total_lpc_tasks_arg),
        "clSetKernelArg(quantize.totalLpcTasks)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 5, sizeof(lpc_coefficient_precision_arg),
                   &lpc_coefficient_precision_arg),
        "clSetKernelArg(quantize.coefficientPrecision)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 6, sizeof(max_lpc_order_arg),
                   &max_lpc_order_arg),
        "clSetKernelArg(quantize.maxLpcOrder)");
    require_cl(clSetKernelArg(runtime.quantize_kernel.get(), 7, sizeof(analysis_profile_arg),
                   &analysis_profile_arg),
        "clSetKernelArg(quantize.analysisProfile)");
    const auto quantize_started = Clock::now();
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.quantize_kernel.get(), 2, nullptr,
                   generation_global.data(), generation_local.data(), 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressQuantizeLpcOrders)");
    finish_timed(
        &OpenClGeneratedAnalysisTimings::generated_quantize_ns,
        quantize_started,
        "clFinish(ldcompressQuantizeLpcOrders)");

    if (plan.fixed_order_guess_on_gpu) {
        const cl_mem fixed_guess_samples_mem =
            use_shifted_samples ? shifted_samples_mem : samples_mem;
        require_cl(clSetKernelArg(runtime.fixed_guess_kernel.get(), 0,
                       sizeof(fixed_guess_samples_mem), &fixed_guess_samples_mem),
            "clSetKernelArg(fixedGuess.samples)");
        require_cl(clSetKernelArg(runtime.fixed_guess_kernel.get(), 1, sizeof(selected_mem),
                       &selected_mem),
            "clSetKernelArg(fixedGuess.selectedTasks)");
        require_cl(clSetKernelArg(runtime.fixed_guess_kernel.get(), 2, sizeof(tasks_mem),
                       &tasks_mem),
            "clSetKernelArg(fixedGuess.tasks)");
        require_cl(clSetKernelArg(runtime.fixed_guess_kernel.get(), 3,
                       sizeof(estimate_tasks_per_frame), &estimate_tasks_per_frame),
            "clSetKernelArg(fixedGuess.taskCount)");
        require_cl(clSetKernelArg(runtime.fixed_guess_kernel.get(), 4,
                       sizeof(use_shifted_samples_arg), &use_shifted_samples_arg),
            "clSetKernelArg(fixedGuess.samplesAlreadyShifted)");

        const std::size_t fixed_guess_local_work_size = kOpenClExactWorkgroupSize;
        const std::size_t fixed_guess_global_work_size =
            frame_count * fixed_guess_local_work_size;
        const auto fixed_guess_started = Clock::now();
        require_cl(clEnqueueNDRangeKernel(runtime.queue.get(),
                       runtime.fixed_guess_kernel.get(), 1, nullptr,
                       &fixed_guess_global_work_size, &fixed_guess_local_work_size,
                       0, nullptr, nullptr),
            "clEnqueueNDRangeKernel(ldcompressPruneFixedOrderGuess)");
        finish_timed(
            &OpenClGeneratedAnalysisTimings::fixed_order_guess_ns,
            fixed_guess_started,
            "clFinish(ldcompressPruneFixedOrderGuess)");
    }

    const auto max_rice_partition_order_arg =
        static_cast<std::int32_t>(max_rice_partition_order);
    const auto exact_leaf_rice_bytes =
        exact_leaf_rice_local_bytes(
            plan, plan.analysis_profile, max_rice_partition_order);
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 0, sizeof(samples_mem), &samples_mem),
        "clSetKernelArg(exact.samples)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 1, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(exact.selectedTasks)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 2, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(exact.tasks)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 3, sizeof(max_rice_partition_order_arg),
                   &max_rice_partition_order_arg),
        "clSetKernelArg(exact.maxRicePartitionOrder)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 4, sizeof(analysis_profile_arg),
                   &analysis_profile_arg),
        "clSetKernelArg(exact.analysisProfile)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 5, sizeof(task_rice_mem),
                   &task_rice_mem),
        "clSetKernelArg(exact.taskRiceParameters)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 6, exact_leaf_rice_bytes, nullptr),
        "clSetKernelArg(exact.leafRiceSums)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 7, sizeof(shifted_samples_mem),
                   &shifted_samples_mem),
        "clSetKernelArg(exact.shiftedSamples)");
    require_cl(clSetKernelArg(runtime.exact_kernel.get(), 8, sizeof(use_shifted_samples_arg),
                   &use_shifted_samples_arg),
        "clSetKernelArg(exact.useShiftedSamples)");

    const std::size_t exact_local_work_size = kOpenClExactWorkgroupSize;
    const std::size_t estimate_global_work_size =
        plan.selected_tasks.size() * exact_local_work_size;
    const auto exact_started = Clock::now();
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.exact_kernel.get(), 1, nullptr,
                   &estimate_global_work_size, &exact_local_work_size, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressAnalyzeSubframeExact)");
    finish_timed(
        &OpenClGeneratedAnalysisTimings::exact_analysis_ns,
        exact_started,
        "clFinish(ldcompressAnalyzeSubframeExact)");

    require_cl(clSetKernelArg(runtime.choose_kernel.get(), 0, sizeof(best_mem), &best_mem),
        "clSetKernelArg(choose.tasks_out)");
    require_cl(clSetKernelArg(runtime.choose_kernel.get(), 1, sizeof(tasks_mem), &tasks_mem),
        "clSetKernelArg(choose.tasks)");
    require_cl(clSetKernelArg(runtime.choose_kernel.get(), 2, sizeof(selected_mem), &selected_mem),
        "clSetKernelArg(choose.selectedTasks)");
    require_cl(clSetKernelArg(runtime.choose_kernel.get(), 3, sizeof(estimate_tasks_per_frame),
                   &estimate_tasks_per_frame),
        "clSetKernelArg(choose.taskCount)");

    const auto choose_started = Clock::now();
    require_cl(clEnqueueNDRangeKernel(runtime.queue.get(), runtime.choose_kernel.get(), 1, nullptr,
                   &frame_global_work_size, nullptr, 0, nullptr, nullptr),
        "clEnqueueNDRangeKernel(ldcompressChooseBestMethod)");
    finish_timed(
        &OpenClGeneratedAnalysisTimings::choose_best_ns,
        choose_started,
        "clFinish(ldcompressChooseBestMethod)");

    const auto rice_started = Clock::now();
    enqueue_opencl_best_rice_parameters(
        runtime.queue,
        runtime.rice_kernel,
        runtime.tasks_buffer,
        runtime.selected_buffer,
        runtime.task_rice_parameters_buffer,
        runtime.best_rice_parameters_buffer,
        frame_count,
        plan.estimate_tasks_per_frame);
    finish_timed(
        &OpenClGeneratedAnalysisTimings::choose_best_ns,
        rice_started,
        "clFinish(ldcompressWriteBestRiceParameters)");

    const auto readback_started = Clock::now();
    if (read_analyzed_tasks) {
        require_cl(clEnqueueReadBuffer(runtime.queue.get(), runtime.tasks_buffer.get(), CL_TRUE, 0,
                       analyzed_tasks.size() * sizeof(FlacClSubframeTask), analyzed_tasks.data(),
                       0, nullptr, nullptr),
            "clEnqueueReadBuffer(tasks)");
    }
    require_cl(clEnqueueReadBuffer(runtime.queue.get(), runtime.best_buffer.get(), CL_TRUE, 0,
                   best_tasks.size() * sizeof(FlacClSubframeTask), best_tasks.data(),
                   0, nullptr, nullptr),
        "clEnqueueReadBuffer(tasks_out)");
    require_cl(clEnqueueReadBuffer(
                   runtime.queue.get(), runtime.best_rice_parameters_buffer.get(), CL_TRUE, 0,
                   best_rice_parameters.size() * sizeof(FlacClRiceParameterSet),
                   best_rice_parameters.data(), 0, nullptr, nullptr),
        "clEnqueueReadBuffer(bestRiceParameters)");
    require_cl(clFinish(runtime.queue.get()), "clFinish");
    if (timings != nullptr) {
        add_elapsed_ns(timings->readback_ns, readback_started);
    }

    return OpenClMonoFixedConstantAnalysisResult {
        .analyzed_tasks = std::move(analyzed_tasks),
        .best_tasks = std::move(best_tasks),
        .best_rice_parameters = std::move(best_rice_parameters),
        .device_name = runtime.selected_device.name,
    };
}

OpenClMonoFixedConstantAnalysisResult execute_opencl_generated_analysis_once(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    std::size_t lpc_tasks_per_window,
    std::size_t total_lpc_tasks,
    std::size_t generated_window_count)
{
    auto runtime = make_opencl_generated_analysis_runtime(requested_device_index);
    return execute_opencl_generated_analysis_validated(
        samples,
        plan,
        lpc_coefficient_precision,
        max_rice_partition_order,
        lpc_tasks_per_window,
        total_lpc_tasks,
        generated_window_count,
        runtime,
        true,
        nullptr);
}
#endif

class OpenClMonoAnalysisSession::Impl final {
public:
#if LDCOMPRESS_HAVE_OPENCL
    explicit Impl(
        std::optional<std::size_t> requested_device_index,
        OpenClGeneratedSetupTimings* setup_timings)
        : runtime_(make_opencl_generated_analysis_runtime(requested_device_index, setup_timings))
    {
    }

    OpenClMonoFixedConstantAnalysisResult run_generated_analysis(
        const std::vector<std::int32_t>& samples,
        const OpenClMonoAnalysisTaskPlan& plan,
        unsigned lpc_coefficient_precision,
        unsigned max_rice_partition_order,
        const GeneratedLpcPrefixShape& lpc_shape,
        bool read_analyzed_tasks,
        OpenClGeneratedAnalysisTimings* timings)
    {
        return execute_opencl_generated_analysis_validated(
            samples,
            plan,
            lpc_coefficient_precision,
            max_rice_partition_order,
            lpc_shape.lpc_tasks_per_window,
            lpc_shape.total_lpc_tasks,
            lpc_shape.window_count,
            runtime_,
            read_analyzed_tasks,
            timings);
    }

private:
    OpenClGeneratedAnalysisRuntime runtime_;
#endif
};

OpenClMonoAnalysisSession::OpenClMonoAnalysisSession(
    std::optional<std::size_t> requested_device_index,
    OpenClGeneratedSetupTimings* setup_timings)
{
#if LDCOMPRESS_HAVE_OPENCL
    impl_ = std::make_unique<Impl>(requested_device_index, setup_timings);
#else
    (void)requested_device_index;
    (void)setup_timings;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoAnalysisSession::~OpenClMonoAnalysisSession() = default;

OpenClMonoFixedConstantAnalysisResult OpenClMonoAnalysisSession::run_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    OpenClGeneratedAnalysisTimings* timings)
{
#if LDCOMPRESS_HAVE_OPENCL
    const auto lpc_shape =
        validate_generated_analysis_inputs(samples, plan, lpc_coefficient_precision);
    return impl_->run_generated_analysis(
        samples,
        plan,
        lpc_coefficient_precision,
        max_rice_partition_order,
        lpc_shape,
        true,
        timings);
#else
    (void)samples;
    (void)plan;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    (void)timings;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoBestMethodResult OpenClMonoAnalysisSession::run_generated_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    OpenClGeneratedAnalysisTimings* timings)
{
#if LDCOMPRESS_HAVE_OPENCL
    const auto lpc_shape =
        validate_generated_analysis_inputs(samples, plan, lpc_coefficient_precision);
    auto analysis = impl_->run_generated_analysis(
        samples,
        plan,
        lpc_coefficient_precision,
        max_rice_partition_order,
        lpc_shape,
        false,
        timings);
    return OpenClMonoBestMethodResult {
        .best_tasks = std::move(analysis.best_tasks),
        .best_rice_parameters = std::move(analysis.best_rice_parameters),
        .device_name = std::move(analysis.device_name),
    };
#else
    (void)samples;
    (void)plan;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    (void)timings;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult internal::execute_opencl_lpc_generation_batch(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
#if LDCOMPRESS_HAVE_OPENCL
    validate_lpc_generation_inputs(samples, plan, lpc_coefficient_precision);
    return execute_opencl_generated_analysis_once(
        samples,
        plan,
        requested_device_index,
        lpc_coefficient_precision,
        max_rice_partition_order,
        plan.residual_tasks_per_frame,
        plan.residual_tasks_per_frame,
        1);
#else
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

OpenClMonoFixedConstantAnalysisResult internal::execute_opencl_mixed_generation_batch(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
#if LDCOMPRESS_HAVE_OPENCL
    const auto lpc_shape =
        validate_generated_analysis_inputs(samples, plan, lpc_coefficient_precision);
    return execute_opencl_generated_analysis_once(
        samples,
        plan,
        requested_device_index,
        lpc_coefficient_precision,
        max_rice_partition_order,
        lpc_shape.lpc_tasks_per_window,
        lpc_shape.total_lpc_tasks,
        lpc_shape.window_count);
#else
    (void)samples;
    (void)plan;
    (void)requested_device_index;
    (void)lpc_coefficient_precision;
    (void)max_rice_partition_order;
    throw std::runtime_error("OpenCL support was not built");
#endif
}

}  // namespace ldcompress::opencl_detail
