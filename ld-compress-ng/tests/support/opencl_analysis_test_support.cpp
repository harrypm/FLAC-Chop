#include "opencl_analysis_test_support.h"

#include "opencl_analysis_internal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <stdexcept>
#include <utility>

namespace ldcompress::opencl_detail {
namespace {

constexpr unsigned kExactMaxRicePartitionOrder = 8;
constexpr unsigned kExactMaxRiceParameter = 14;
constexpr std::size_t kOpenClAnalysisMaxBlockSize = 8192;
constexpr std::size_t kOpenClAnalysisBitsPerSample = 16;

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

unsigned bit_width_u32(std::uint32_t value)
{
    unsigned bits = 0;
    while (value != 0) {
        value >>= 1U;
        ++bits;
    }
    return bits;
}

unsigned frame_amplitude_bits(
    const std::vector<std::int32_t>& samples,
    std::size_t offset,
    std::size_t count,
    unsigned wasted_bits)
{
    std::uint32_t amplitude_or = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto sample = samples.at(offset + i);
        const auto folded = sample < 0
            ? static_cast<std::uint32_t>(-(static_cast<std::int64_t>(sample) + 1))
            : static_cast<std::uint32_t>(sample);
        amplitude_or |= folded;
    }
    if (amplitude_or == 0) {
        return 1;
    }
    const auto bits = bit_width_u32(amplitude_or);
    return std::max(1U, bits > wasted_bits ? bits - wasted_bits : 0U);
}

bool all_frame_samples_equal(
    const std::vector<std::int32_t>& samples,
    std::size_t offset,
    std::size_t count)
{
    const auto first = samples.at(offset);
    for (std::size_t i = 1; i < count; ++i) {
        if (samples.at(offset + i) != first) {
            return false;
        }
    }
    return true;
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

std::uint64_t subframe_wasted_bits_overhead(unsigned wasted_bits)
{
    return wasted_bits == 0 ? 0 : wasted_bits;
}

std::uint64_t constant_subframe_bits(unsigned bits_per_sample, unsigned wasted_bits)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    return kSubframeHeaderBits + (bits_per_sample - wasted_bits) +
        subframe_wasted_bits_overhead(wasted_bits);
}

std::int64_t fixed_prediction_residual(
    const std::vector<std::int32_t>& samples,
    std::size_t index,
    unsigned order)
{
    switch (order) {
    case 0:
        return samples[index];
    case 1:
        return static_cast<std::int64_t>(samples[index]) - samples[index - 1];
    case 2:
        return static_cast<std::int64_t>(samples[index]) -
            (2LL * samples[index - 1]) + samples[index - 2];
    case 3:
        return static_cast<std::int64_t>(samples[index]) -
            (3LL * samples[index - 1]) + (3LL * samples[index - 2]) -
            samples[index - 3];
    case 4:
        return static_cast<std::int64_t>(samples[index]) -
            (4LL * samples[index - 1]) + (6LL * samples[index - 2]) -
            (4LL * samples[index - 3]) + samples[index - 4];
    default:
        throw std::runtime_error("OpenCL exact analysis received invalid fixed order");
    }
}

std::vector<std::int64_t> fixed_residuals(
    const std::vector<std::int32_t>& samples,
    unsigned order)
{
    std::vector<std::int64_t> residuals;
    residuals.reserve(samples.size() - order);
    for (std::size_t i = order; i < samples.size(); ++i) {
        residuals.push_back(fixed_prediction_residual(samples, i, order));
    }
    return residuals;
}

std::uint64_t fold_signed_residual(std::int64_t residual)
{
    if (residual >= 0) {
        return static_cast<std::uint64_t>(residual) << 1U;
    }
    return (static_cast<std::uint64_t>(-(residual + 1)) << 1U) + 1U;
}

std::uint64_t rice_bits(
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count,
    unsigned parameter)
{
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto folded = fold_signed_residual(residuals.at(offset + i));
        bits += (folded >> parameter) + 1U + parameter;
    }
    return bits;
}

unsigned choose_rice_parameter(
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count)
{
    if (offset > residuals.size() || count > residuals.size() - offset) {
        throw std::runtime_error("OpenCL exact analysis residual partition range error");
    }

    std::array<std::uint64_t, kExactMaxRiceParameter + 1U> bit_counts {};
    for (unsigned parameter = 0; parameter <= kExactMaxRiceParameter; ++parameter) {
        bit_counts[parameter] = static_cast<std::uint64_t>(count) * (1U + parameter);
    }

    const auto end = offset + count;
    for (std::size_t i = offset; i < end; ++i) {
        const auto folded = fold_signed_residual(residuals[i]);
        for (unsigned parameter = 0; parameter <= kExactMaxRiceParameter; ++parameter) {
            bit_counts[parameter] += folded >> parameter;
        }
    }

    unsigned best_parameter = 0;
    auto best_bits = bit_counts[0];
    for (unsigned parameter = 1; parameter <= kExactMaxRiceParameter; ++parameter) {
        if (bit_counts[parameter] < best_bits) {
            best_bits = bit_counts[parameter];
            best_parameter = parameter;
        }
    }
    return best_parameter;
}

bool valid_partition_order(std::size_t block_size, unsigned predictor_order, unsigned partition_order)
{
    const auto partition_count = std::size_t {1} << partition_order;
    if ((block_size % partition_count) != 0) {
        return false;
    }
    const auto partition_samples = block_size / partition_count;
    return partition_samples > predictor_order;
}

std::size_t partition_residual_count(
    std::size_t block_size,
    unsigned predictor_order,
    unsigned partition_order,
    std::size_t partition)
{
    const auto partition_samples = block_size >> partition_order;
    return partition == 0
        ? partition_samples - predictor_order
        : partition_samples;
}

std::uint64_t fixed_rice_subframe_bits(
    unsigned order,
    unsigned partition_order,
    unsigned wasted_bits,
    const std::vector<unsigned>& rice_parameters,
    const std::vector<std::int64_t>& residuals,
    std::size_t block_size,
    unsigned bits_per_sample)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    constexpr unsigned kRiceMethodBits = 2;
    constexpr unsigned kPartitionOrderBits = 4;
    constexpr unsigned kRiceParameterBits = 4;

    std::uint64_t bits =
        kSubframeHeaderBits + (static_cast<std::uint64_t>(order) * bits_per_sample) +
        kRiceMethodBits + kPartitionOrderBits + subframe_wasted_bits_overhead(wasted_bits);

    std::size_t residual_offset = 0;
    const auto partition_count = std::size_t {1} << partition_order;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const auto residual_count = partition_residual_count(
            block_size, order, partition_order, partition);
        bits += kRiceParameterBits;
        bits += rice_bits(residuals, residual_offset, residual_count,
            rice_parameters.at(partition));
        residual_offset += residual_count;
    }
    if (residual_offset != residuals.size()) {
        throw std::runtime_error("OpenCL exact analysis residual partition accounting error");
    }
    return bits;
}

struct ExactFixedRiceChoice {
    std::uint64_t bits = std::numeric_limits<std::uint64_t>::max();
    unsigned partition_order = 0;
};

ExactFixedRiceChoice choose_exact_fixed_rice(
    const std::vector<std::int64_t>& residuals,
    std::size_t block_size,
    unsigned order,
    unsigned wasted_bits,
    unsigned effective_bits_per_sample,
    unsigned max_rice_partition_order)
{
    if (max_rice_partition_order > kExactMaxRicePartitionOrder) {
        throw std::runtime_error("OpenCL exact analysis max Rice partition order must be 0..8");
    }

    ExactFixedRiceChoice best;
    for (unsigned partition_order = 0; partition_order <= max_rice_partition_order; ++partition_order) {
        if (!valid_partition_order(block_size, order, partition_order)) {
            continue;
        }

        const auto partition_count = std::size_t {1} << partition_order;
        std::vector<unsigned> rice_parameters;
        rice_parameters.reserve(partition_count);
        std::size_t residual_offset = 0;
        for (std::size_t partition = 0; partition < partition_count; ++partition) {
            const auto residual_count = partition_residual_count(
                block_size, order, partition_order, partition);
            rice_parameters.push_back(choose_rice_parameter(
                residuals, residual_offset, residual_count));
            residual_offset += residual_count;
        }
        if (residual_offset != residuals.size()) {
            throw std::runtime_error("OpenCL exact analysis residual partition accounting error");
        }

        const auto bits = fixed_rice_subframe_bits(
            order, partition_order, wasted_bits, rice_parameters, residuals,
            block_size, effective_bits_per_sample);
        if (bits < best.bits) {
            best.bits = bits;
            best.partition_order = partition_order;
        }
    }

    if (best.bits == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("OpenCL exact analysis found no valid Rice partition order");
    }
    return best;
}

void analyze_fixed_constant_task_exact(
    const std::vector<std::int32_t>& samples,
    FlacClSubframeTask& task,
    unsigned max_rice_partition_order)
{
    const auto offset = static_cast<std::size_t>(task.data.samplesOffs);
    const auto block_size = static_cast<std::size_t>(task.data.blocksize);
    const auto bits_per_sample = static_cast<unsigned>(task.data.obits);
    const auto wasted_bits = common_wasted_bits(samples, offset, block_size, bits_per_sample);
    const auto effective_bits_per_sample = bits_per_sample - wasted_bits;

    task.data.wbits = checked_i32(wasted_bits, "OpenCL exact analysis wbits");
    task.data.abits = checked_i32(
        frame_amplitude_bits(samples, offset, block_size, wasted_bits),
        "OpenCL exact analysis abits");
    if (task.data.size == std::numeric_limits<std::int32_t>::max()) {
        return;
    }
    task.data.porder = 0;

    if (task.data.type == kFlacClSubframeConstant) {
        if (!all_frame_samples_equal(samples, offset, block_size)) {
            task.data.size = std::numeric_limits<std::int32_t>::max();
            return;
        }
        task.data.size = checked_i32(
            constant_subframe_bits(bits_per_sample, wasted_bits),
            "OpenCL exact analysis constant size");
        return;
    }

    const auto order = static_cast<unsigned>(task.data.residualOrder);
    const auto shifted = shifted_frame_samples(samples, offset, block_size, wasted_bits);
    const auto residuals = fixed_residuals(shifted, order);
    const auto choice = choose_exact_fixed_rice(
        residuals, block_size, order, wasted_bits, effective_bits_per_sample,
        max_rice_partition_order);
    task.data.porder = checked_i32(choice.partition_order, "OpenCL exact analysis porder");
    task.data.size = checked_i32(choice.bits, "OpenCL exact analysis fixed size");
}

std::vector<FlacClSubframeTask> choose_best_tasks(
    const std::vector<FlacClSubframeTask>& tasks,
    const OpenClMonoAnalysisTaskPlan& plan)
{
    validate_best_method_plan(plan);
    if (tasks.size() != plan.residual_tasks.size()) {
        throw std::runtime_error("OpenCL exact analysis task count mismatch");
    }

    const auto frame_count = plan.selected_tasks.size() / plan.estimate_tasks_per_frame;
    std::vector<FlacClSubframeTask> best_tasks;
    best_tasks.reserve(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto selected_base = frame * plan.estimate_tasks_per_frame;
        auto best_index = plan.selected_tasks.at(selected_base);
        auto best_size = tasks.at(static_cast<std::size_t>(best_index)).data.size;
        for (std::size_t i = 1; i < plan.estimate_tasks_per_frame; ++i) {
            const auto task_index = plan.selected_tasks.at(selected_base + i);
            const auto task_size = tasks.at(static_cast<std::size_t>(task_index)).data.size;
            if (task_size < best_size) {
                best_index = task_index;
                best_size = task_size;
            }
        }
        best_tasks.push_back(tasks.at(static_cast<std::size_t>(best_index)));
    }
    return best_tasks;
}

FlacClRiceParameterSet exact_rice_parameters_for_best_fixed_task(
    const std::vector<std::int32_t>& samples,
    const FlacClSubframeTask& task)
{
    FlacClRiceParameterSet rice_parameters;
    if (task.data.type != kFlacClSubframeFixed) {
        return rice_parameters;
    }
    if (task.data.porder < 0 ||
        task.data.porder >
            static_cast<std::int32_t>(kFlacClMaxRicePartitionOrder)) {
        throw std::runtime_error("OpenCL scalar exact best task has invalid Rice partition order");
    }

    const auto offset = static_cast<std::size_t>(task.data.samplesOffs);
    const auto block_size = static_cast<std::size_t>(task.data.blocksize);
    const auto order = static_cast<unsigned>(task.data.residualOrder);
    const auto partition_order = static_cast<unsigned>(task.data.porder);
    const auto shifted = shifted_frame_samples(
        samples, offset, block_size, static_cast<unsigned>(task.data.wbits));
    const auto residuals = fixed_residuals(shifted, order);

    std::size_t residual_offset = 0;
    const auto partition_count = std::size_t {1} << partition_order;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const auto residual_count = partition_residual_count(
            block_size, order, partition_order, partition);
        rice_parameters.parameters[partition] = choose_rice_parameter(
            residuals, residual_offset, residual_count);
        residual_offset += residual_count;
    }
    if (residual_offset != residuals.size()) {
        throw std::runtime_error("OpenCL scalar exact Rice sidecar partition accounting error");
    }
    return rice_parameters;
}

std::vector<FlacClRiceParameterSet> exact_rice_parameters_for_best_fixed_tasks(
    const std::vector<std::int32_t>& samples,
    const std::vector<FlacClSubframeTask>& best_tasks)
{
    std::vector<FlacClRiceParameterSet> best_rice_parameters;
    best_rice_parameters.reserve(best_tasks.size());
    for (const auto& task : best_tasks) {
        best_rice_parameters.push_back(
            exact_rice_parameters_for_best_fixed_task(samples, task));
    }
    return best_rice_parameters;
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

void validate_fixed_constant_analysis_inputs(
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

}  // namespace

OpenClMonoBestMethodResult run_opencl_mono_best_method(
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index)
{
    return internal::execute_opencl_best_reduction(plan, requested_device_index);
}

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_lpc_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned max_rice_partition_order)
{
    return internal::execute_opencl_exact_task_batch(
        samples, plan, requested_device_index, max_rice_partition_order, true);
}

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_lpc_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    return internal::execute_opencl_lpc_generation_batch(
        samples,
        plan,
        requested_device_index,
        lpc_coefficient_precision,
        max_rice_partition_order);
}

OpenClMonoFixedConstantAnalysisResult run_opencl_mono_generated_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    return internal::execute_opencl_mixed_generation_batch(
        samples,
        plan,
        requested_device_index,
        lpc_coefficient_precision,
        max_rice_partition_order);
}

OpenClMonoBestMethodResult run_opencl_mono_generated_best_analysis(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    std::optional<std::size_t> requested_device_index,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    OpenClGeneratedAnalysisTimings* timings)
{
    OpenClMonoAnalysisSession session(requested_device_index);
    return session.run_generated_best_analysis(
        samples, plan, lpc_coefficient_precision, max_rice_partition_order, timings);
}

OpenClMonoGeneratedFrameAnalysisResult analyze_opencl_mono_generated_frames(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& frame_info,
    unsigned frame_samples,
    std::optional<std::size_t> requested_device_index)
{
    if (frame_samples == 0) {
        throw std::runtime_error("OpenCL generated frame analysis frame_samples must be positive");
    }
    if (samples.empty()) {
        throw std::runtime_error("OpenCL generated frame analysis requires at least one frame");
    }
    if ((samples.size() % frame_samples) != 0) {
        throw std::runtime_error("OpenCL generated frame analysis samples are not frame-aligned");
    }

    OpenClMonoAnalysisTaskOptions options;
    options.frame_samples = frame_samples;
    options.bits_per_sample = frame_info.bits_per_sample;
    options.max_lpc_order = frame_info.max_lpc_order;
    options.min_fixed_order = 0;
    options.max_fixed_order = 4;
    options.include_constant = true;

    const auto frame_count = samples.size() / frame_samples;
    const auto plan = build_mono_analysis_task_plan(frame_count, options);
    auto analysis = run_opencl_mono_generated_analysis(
        samples,
        plan,
        requested_device_index,
        frame_info.lpc_coefficient_precision,
        frame_info.max_rice_partition_order);

    std::vector<FlacSubframeDecision> decisions;
    std::vector<FlacSelectedSubframe> selected_subframes;
    if (!analysis.best_rice_parameters.empty() &&
        analysis.best_rice_parameters.size() != analysis.best_tasks.size()) {
        throw std::runtime_error("OpenCL Rice parameter result count did not match best task count");
    }
    decisions.reserve(analysis.best_tasks.size());
    selected_subframes.reserve(analysis.best_tasks.size());
    for (std::size_t i = 0; i < analysis.best_tasks.size(); ++i) {
        const auto& task = analysis.best_tasks[i];
        decisions.push_back(flaccl_task_to_subframe_decision(task));
        auto selected = flaccl_task_to_selected_subframe(task);
        if (!analysis.best_rice_parameters.empty()) {
            selected.rice_parameters =
                flaccl_task_to_selected_rice_parameters(
                    task, analysis.best_rice_parameters[i]);
        }
        selected_subframes.push_back(std::move(selected));
    }

    return OpenClMonoGeneratedFrameAnalysisResult {
        .analyzed_tasks = std::move(analysis.analyzed_tasks),
        .best_tasks = std::move(analysis.best_tasks),
        .best_rice_parameters = std::move(analysis.best_rice_parameters),
        .decisions = std::move(decisions),
        .selected_subframes = std::move(selected_subframes),
        .device_name = std::move(analysis.device_name),
    };
}

OpenClMonoFixedConstantAnalysisResult analyze_mono_fixed_constant_exact(
    const std::vector<std::int32_t>& samples,
    const OpenClMonoAnalysisTaskPlan& plan,
    unsigned max_rice_partition_order)
{
    validate_fixed_constant_analysis_inputs(samples, plan);

    std::vector<FlacClSubframeTask> analyzed_tasks = plan.residual_tasks;
    for (auto& task : analyzed_tasks) {
        analyze_fixed_constant_task_exact(samples, task, max_rice_partition_order);
    }
    auto best_tasks = choose_best_tasks(analyzed_tasks, plan);
    auto best_rice_parameters =
        exact_rice_parameters_for_best_fixed_tasks(samples, best_tasks);

    return OpenClMonoFixedConstantAnalysisResult {
        .analyzed_tasks = std::move(analyzed_tasks),
        .best_tasks = std::move(best_tasks),
        .best_rice_parameters = std::move(best_rice_parameters),
        .device_name = "scalar-exact",
    };
}

std::optional<FlacClSubframeTask> analyze_mono_lpc_exact_task(
    const std::vector<std::int32_t>& samples,
    std::size_t frame_index,
    const OpenClMonoAnalysisTaskOptions& options,
    unsigned lpc_order,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    validate_options(options);
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("OpenCL LPC exact analysis coefficient precision must be 1..15");
    }
    if (max_rice_partition_order > kExactMaxRicePartitionOrder) {
        throw std::runtime_error("OpenCL LPC exact analysis max Rice partition order must be 0..8");
    }
    if (frame_index >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) / options.frame_samples) {
        throw std::runtime_error("OpenCL LPC exact analysis frame offset exceeds int32");
    }

    const auto offset = frame_index * static_cast<std::size_t>(options.frame_samples);
    if (offset > samples.size() ||
        static_cast<std::size_t>(options.frame_samples) > samples.size() - offset) {
        throw std::runtime_error("OpenCL LPC exact analysis frame samples are out of range");
    }

    const std::vector<std::int32_t> frame_samples(
        samples.begin() + static_cast<std::ptrdiff_t>(offset),
        samples.begin() + static_cast<std::ptrdiff_t>(offset + options.frame_samples));
    const FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 0,
        .bits_per_sample = options.bits_per_sample,
        .max_lpc_order = options.max_lpc_order,
        .lpc_coefficient_precision = lpc_coefficient_precision,
        .max_rice_partition_order = max_rice_partition_order,
    };
    const auto lpc = analyze_mono_lpc_order(frame_samples, frame_info, lpc_order);
    if (!lpc.has_value()) {
        return std::nullopt;
    }
    if (lpc->order == 0 || lpc->order > kFlacClMaxOrder ||
        lpc->coefficients.size() != lpc->order) {
        throw std::runtime_error("OpenCL LPC exact analysis produced invalid coefficients");
    }

    auto task = make_common_task(options, frame_index, kFlacClSubframeLpc, lpc->order);
    task.data.shift = lpc->quantization_shift;
    task.data.cbits = checked_i32(lpc->coefficient_precision, "OpenCL LPC exact analysis cbits");
    task.data.size = checked_i32(lpc->estimated_bits, "OpenCL LPC exact analysis size");
    task.data.wbits = checked_i32(lpc->wasted_bits, "OpenCL LPC exact analysis wbits");
    task.data.abits = checked_i32(
        frame_amplitude_bits(samples, offset, options.frame_samples, lpc->wasted_bits),
        "OpenCL LPC exact analysis abits");
    task.data.porder = checked_i32(lpc->rice_partition_order, "OpenCL LPC exact analysis porder");

    for (std::size_t i = 0; i < lpc->coefficients.size(); ++i) {
        task.coefs[i] = lpc->coefficients[lpc->coefficients.size() - i - 1U];
    }
    return task;
}

}  // namespace ldcompress::opencl_detail
