#include "flac_native_writer.h"
#include "metal_analysis.h"
#include "metal_devices.h"
#include "opencl_analysis.h"
#include "opencl_analysis_test_support.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::size_t parse_device_index(std::string_view text)
{
    if (text.empty()) {
        throw std::runtime_error("empty Metal device index");
    }
    std::size_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid Metal device index: " + std::string(text));
        }
        value = (value * 10U) + static_cast<std::size_t>(ch - '0');
    }
    return value;
}

struct Options {
    std::optional<std::size_t> device_index;
};

Options parse_args(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--device") {
            if (++i >= argc) {
                throw std::runtime_error("--device requires a value");
            }
            options.device_index = parse_device_index(argv[i]);
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }
    return options;
}

std::optional<std::size_t> first_available_metal_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::metal_support_built()) {
        return std::nullopt;
    }

    const auto devices = ldcompress::list_metal_devices();
    if (requested_index.has_value()) {
        if (*requested_index >= devices.size()) {
            throw std::runtime_error("requested Metal device index is out of range");
        }
        const auto& device = devices[*requested_index];
        return device.available ? std::optional<std::size_t>(device.index) : std::nullopt;
    }

    for (const auto& device : devices) {
        if (device.available && !device.low_power) {
            return device.index;
        }
    }
    for (const auto& device : devices) {
        if (device.available) {
            return device.index;
        }
    }
    return std::nullopt;
}

void require_task_matches_task(
    const ldcompress::opencl_detail::FlacClSubframeTask& actual,
    const ldcompress::opencl_detail::FlacClSubframeTask& expected,
    const char* label)
{
    const auto require_field = [&](auto actual_value, auto expected_value, const char* field) {
        if (actual_value != expected_value) {
            std::ostringstream out;
            out << label << ": " << field << " actual=" << actual_value
                << " expected=" << expected_value
                << " actual_size=" << actual.data.size
                << " expected_size=" << expected.data.size
                << " actual_porder=" << actual.data.porder
                << " expected_porder=" << expected.data.porder;
            throw std::runtime_error(out.str());
        }
    };
    require_field(actual.data.type, expected.data.type, "type");
    require_field(actual.data.residualOrder, expected.data.residualOrder, "residualOrder");
    require_field(actual.data.samplesOffs, expected.data.samplesOffs, "samplesOffs");
    require_field(actual.data.wbits, expected.data.wbits, "wbits");
    require_field(actual.data.abits, expected.data.abits, "abits");
    require_field(actual.data.porder, expected.data.porder, "porder");
    require_field(actual.data.size, expected.data.size, "size");
}

void require_lpc_task_matches(
    const ldcompress::opencl_detail::FlacClSubframeTask& actual,
    const ldcompress::opencl_detail::FlacClSubframeTask& expected,
    const char* label)
{
    require_task_matches_task(actual, expected, label);
    if (actual.data.shift != expected.data.shift) {
        std::ostringstream out;
        out << label << ": shift actual=" << actual.data.shift
            << " expected=" << expected.data.shift;
        throw std::runtime_error(out.str());
    }
    if (actual.data.cbits != expected.data.cbits) {
        std::ostringstream out;
        out << label << ": cbits actual=" << actual.data.cbits
            << " expected=" << expected.data.cbits;
        throw std::runtime_error(out.str());
    }
    for (int i = 0; i < expected.data.residualOrder; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (actual.coefs[index] != expected.coefs[index]) {
            std::ostringstream out;
            out << label << ": coef[" << i << "] actual=" << actual.coefs[index]
                << " expected=" << expected.coefs[index];
            throw std::runtime_error(out.str());
        }
    }
}

void require_analysis_matches(
    const ldcompress::opencl_detail::OpenClMonoFixedConstantAnalysisResult& actual,
    const ldcompress::opencl_detail::OpenClMonoFixedConstantAnalysisResult& expected,
    const char* label)
{
    require(actual.analyzed_tasks.size() == expected.analyzed_tasks.size(), label);
    require(actual.best_tasks.size() == expected.best_tasks.size(), label);
    for (std::size_t i = 0; i < expected.analyzed_tasks.size(); ++i) {
        require_task_matches_task(actual.analyzed_tasks[i], expected.analyzed_tasks[i], label);
    }
    for (std::size_t i = 0; i < expected.best_tasks.size(); ++i) {
        require_task_matches_task(actual.best_tasks[i], expected.best_tasks[i], label);
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

void require_rice_parameters_valid(
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& tasks,
    const std::vector<ldcompress::opencl_detail::FlacClRiceParameterSet>& parameters,
    const char* label)
{
    require(parameters.size() == tasks.size(), label);
    for (std::size_t frame = 0; frame < tasks.size(); ++frame) {
        const auto& task = tasks[frame];
        if (task.data.type != ldcompress::opencl_detail::kFlacClSubframeFixed &&
            task.data.type != ldcompress::opencl_detail::kFlacClSubframeLpc) {
            continue;
        }
        require(task.data.porder >= 0, label);
        require(static_cast<std::size_t>(task.data.porder) <=
                ldcompress::opencl_detail::kFlacClMaxRicePartitionOrder,
            label);
        const auto partition_count =
            std::size_t {1} << static_cast<unsigned>(task.data.porder);
        for (std::size_t partition = 0; partition < partition_count; ++partition) {
            require(parameters[frame].parameters[partition] <= 14U, label);
        }
    }
}

std::int32_t shifted_sample(
    const std::vector<std::int32_t>& samples,
    const ldcompress::opencl_detail::FlacClSubframeTask& task,
    std::size_t index)
{
    const auto sample = samples.at(static_cast<std::size_t>(task.data.samplesOffs) + index);
    if (task.data.wbits == 0) {
        return sample;
    }
    const auto divisor = std::int64_t {1} << static_cast<unsigned>(task.data.wbits);
    return static_cast<std::int32_t>(static_cast<std::int64_t>(sample) / divisor);
}

std::int64_t arithmetic_shift_right(std::int64_t value, unsigned shift)
{
    if (shift == 0) {
        return value;
    }
    if (value >= 0) {
        return value >> shift;
    }
    const auto divisor = std::int64_t {1} << shift;
    return -(((-value) + divisor - 1) >> shift);
}

std::int64_t task_residual(
    const std::vector<std::int32_t>& samples,
    const ldcompress::opencl_detail::FlacClSubframeTask& task,
    std::size_t index)
{
    using namespace ldcompress::opencl_detail;

    switch (task.data.type) {
    case kFlacClSubframeFixed:
        switch (task.data.residualOrder) {
        case 0:
            return shifted_sample(samples, task, index);
        case 1:
            return static_cast<std::int64_t>(shifted_sample(samples, task, index)) -
                shifted_sample(samples, task, index - 1U);
        case 2:
            return static_cast<std::int64_t>(shifted_sample(samples, task, index)) -
                (2LL * shifted_sample(samples, task, index - 1U)) +
                shifted_sample(samples, task, index - 2U);
        case 3:
            return static_cast<std::int64_t>(shifted_sample(samples, task, index)) -
                (3LL * shifted_sample(samples, task, index - 1U)) +
                (3LL * shifted_sample(samples, task, index - 2U)) -
                shifted_sample(samples, task, index - 3U);
        case 4:
            return static_cast<std::int64_t>(shifted_sample(samples, task, index)) -
                (4LL * shifted_sample(samples, task, index - 1U)) +
                (6LL * shifted_sample(samples, task, index - 2U)) -
                (4LL * shifted_sample(samples, task, index - 3U)) +
                shifted_sample(samples, task, index - 4U);
        default:
            throw std::runtime_error("invalid fixed predictor order in Metal sidecar test");
        }
    case kFlacClSubframeLpc: {
        const auto order = static_cast<std::size_t>(task.data.residualOrder);
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < order; ++i) {
            sum += static_cast<std::int64_t>(task.coefs.at(i)) *
                shifted_sample(samples, task, index - order + i);
        }
        const auto predicted =
            arithmetic_shift_right(sum, static_cast<unsigned>(task.data.shift));
        return static_cast<std::int64_t>(shifted_sample(samples, task, index)) - predicted;
    }
    default:
        throw std::runtime_error("unsupported task type in Metal sidecar test");
    }
}

std::uint64_t fold_signed_residual(std::int64_t residual)
{
    if (residual >= 0) {
        return static_cast<std::uint64_t>(residual) << 1U;
    }
    return (static_cast<std::uint64_t>(-(residual + 1)) << 1U) + 1U;
}

std::size_t partition_residual_count(
    std::size_t block_size,
    std::size_t predictor_order,
    unsigned partition_order,
    std::size_t partition)
{
    const auto partition_samples = block_size >> partition_order;
    return partition == 0 ? partition_samples - predictor_order : partition_samples;
}

unsigned choose_task_rice_parameter(
    const std::vector<std::int32_t>& samples,
    const ldcompress::opencl_detail::FlacClSubframeTask& task,
    std::size_t residual_offset,
    std::size_t count)
{
    constexpr unsigned kMaxRiceParameter = 14;
    std::array<std::uint64_t, kMaxRiceParameter + 1U> bit_counts {};
    for (unsigned parameter = 0; parameter <= kMaxRiceParameter; ++parameter) {
        bit_counts[parameter] = static_cast<std::uint64_t>(count) * (1U + parameter);
    }

    const auto order = static_cast<std::size_t>(task.data.residualOrder);
    for (std::size_t i = 0; i < count; ++i) {
        const auto folded =
            fold_signed_residual(task_residual(samples, task, order + residual_offset + i));
        for (unsigned parameter = 0; parameter <= kMaxRiceParameter; ++parameter) {
            bit_counts[parameter] += folded >> parameter;
        }
    }

    unsigned best_parameter = 0;
    auto best_bits = bit_counts[0];
    for (unsigned parameter = 1; parameter <= kMaxRiceParameter; ++parameter) {
        if (bit_counts[parameter] < best_bits) {
            best_bits = bit_counts[parameter];
            best_parameter = parameter;
        }
    }
    return best_parameter;
}

ldcompress::opencl_detail::FlacClRiceParameterSet scalar_rice_parameters_for_task(
    const std::vector<std::int32_t>& samples,
    const ldcompress::opencl_detail::FlacClSubframeTask& task)
{
    using namespace ldcompress::opencl_detail;

    FlacClRiceParameterSet expected;
    if (task.data.type != kFlacClSubframeFixed &&
        task.data.type != kFlacClSubframeLpc) {
        return expected;
    }
    require(task.data.porder >= 0, "Metal sidecar task has negative partition order");
    require(static_cast<std::size_t>(task.data.porder) <= kFlacClMaxRicePartitionOrder,
        "Metal sidecar task partition order exceeds max");

    const auto partition_order = static_cast<unsigned>(task.data.porder);
    const auto partition_count = std::size_t {1} << partition_order;
    const auto block_size = static_cast<std::size_t>(task.data.blocksize);
    const auto order = static_cast<std::size_t>(task.data.residualOrder);
    std::size_t residual_offset = 0;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const auto residual_count =
            partition_residual_count(block_size, order, partition_order, partition);
        expected.parameters[partition] =
            choose_task_rice_parameter(samples, task, residual_offset, residual_count);
        residual_offset += residual_count;
    }
    require(residual_offset == block_size - order,
        "Metal sidecar residual partition accounting mismatch");
    return expected;
}

void require_rice_parameters_match_scalar(
    const std::vector<std::int32_t>& samples,
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& tasks,
    const std::vector<ldcompress::opencl_detail::FlacClRiceParameterSet>& parameters,
    const char* label)
{
    require_rice_parameters_valid(tasks, parameters, label);
    for (std::size_t frame = 0; frame < tasks.size(); ++frame) {
        const auto& task = tasks[frame];
        if (task.data.type != ldcompress::opencl_detail::kFlacClSubframeFixed &&
            task.data.type != ldcompress::opencl_detail::kFlacClSubframeLpc) {
            continue;
        }
        const auto expected = scalar_rice_parameters_for_task(samples, task);
        const auto partition_count =
            std::size_t {1} << static_cast<unsigned>(task.data.porder);
        for (std::size_t partition = 0; partition < partition_count; ++partition) {
            require(parameters[frame].parameters[partition] ==
                    expected.parameters[partition],
                label);
        }
    }
}

std::vector<std::int32_t> frame_samples_for_task(
    const std::vector<std::int32_t>& samples,
    const ldcompress::opencl_detail::FlacClSubframeTask& task)
{
    require(task.data.samplesOffs >= 0, "Metal selected task has negative sample offset");
    require(task.data.blocksize > 0, "Metal selected task has invalid block size");
    const auto offset = static_cast<std::size_t>(task.data.samplesOffs);
    const auto count = static_cast<std::size_t>(task.data.blocksize);
    require(offset <= samples.size() && count <= samples.size() - offset,
        "Metal selected task sample range is out of bounds");
    return std::vector<std::int32_t>(
        samples.begin() + static_cast<std::ptrdiff_t>(offset),
        samples.begin() + static_cast<std::ptrdiff_t>(offset + count));
}

ldcompress::FlacFrameInfo frame_info_for_task(
    const ldcompress::opencl_detail::FlacClSubframeTask& task,
    unsigned max_lpc_order,
    unsigned lpc_precision,
    unsigned max_rice_partition_order)
{
    return ldcompress::FlacFrameInfo {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = static_cast<unsigned>(task.data.obits),
        .max_lpc_order = max_lpc_order,
        .lpc_coefficient_precision = lpc_precision,
        .max_rice_partition_order = max_rice_partition_order,
    };
}

void require_decision_matches_task(
    const ldcompress::FlacSubframeDecision& actual,
    const ldcompress::FlacSubframeDecision& expected,
    const char* label)
{
    require(actual.kind == expected.kind, label);
    require(actual.fixed_order == expected.fixed_order, label);
    require(actual.lpc_order == expected.lpc_order, label);
    require(actual.rice_partition_order == expected.rice_partition_order, label);
    require(actual.wasted_bits == expected.wasted_bits, label);
    require(actual.estimated_bits == expected.estimated_bits, label);
}

void require_selected_writer_recovers_tasks(
    const std::vector<std::int32_t>& samples,
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& tasks,
    const std::vector<ldcompress::opencl_detail::FlacClRiceParameterSet>& rice_parameters,
    unsigned max_lpc_order,
    unsigned lpc_precision,
    unsigned max_rice_partition_order,
    const char* label)
{
    require_rice_parameters_valid(tasks, rice_parameters, label);
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const auto& task = tasks[i];
        const auto expected = ldcompress::opencl_detail::flaccl_task_to_subframe_decision(task);
        auto selected = ldcompress::opencl_detail::flaccl_task_to_selected_subframe(task);
        selected.rice_parameters =
            ldcompress::opencl_detail::flaccl_task_to_selected_rice_parameters(
                task, rice_parameters[i]);

        std::ostringstream output;
        const auto decision = ldcompress::write_mono_selected_frame(
            output,
            frame_samples_for_task(samples, task),
            frame_info_for_task(task, max_lpc_order, lpc_precision, max_rice_partition_order),
            selected);
        require(!output.str().empty(), label);
        require_decision_matches_task(decision, expected, label);
    }
}

void require_selected_writer_accepts_tasks(
    const std::vector<std::int32_t>& samples,
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& tasks,
    const std::vector<ldcompress::opencl_detail::FlacClRiceParameterSet>& rice_parameters,
    unsigned max_lpc_order,
    unsigned lpc_precision,
    unsigned max_rice_partition_order,
    const char* label)
{
    require_rice_parameters_valid(tasks, rice_parameters, label);
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const auto& task = tasks[i];
        const auto expected = ldcompress::opencl_detail::flaccl_task_to_subframe_decision(task);
        auto selected = ldcompress::opencl_detail::flaccl_task_to_selected_subframe(task);
        selected.rice_parameters =
            ldcompress::opencl_detail::flaccl_task_to_selected_rice_parameters(
                task, rice_parameters[i]);

        std::ostringstream output;
        const auto decision = ldcompress::write_mono_selected_frame(
            output,
            frame_samples_for_task(samples, task),
            frame_info_for_task(task, max_lpc_order, lpc_precision, max_rice_partition_order),
            selected);
        require(!output.str().empty(), label);
        require(decision.kind == expected.kind, label);
        require(decision.fixed_order == expected.fixed_order, label);
        require(decision.lpc_order == expected.lpc_order, label);
        require(decision.rice_partition_order == expected.rice_partition_order, label);
        require(decision.wasted_bits == expected.wasted_bits, label);
    }
}

std::vector<std::int32_t> make_lpc_friendly_samples()
{
    std::vector<std::int32_t> samples;
    samples.reserve(512);
    for (int i = 0; i < 512; ++i) {
        const auto sample =
            ((i * 448) + ((i % 17) * 384) - ((i % 5) * 512)) % 32768;
        samples.push_back(static_cast<std::int32_t>(
            (sample - 16384) & ~std::int32_t {63}));
    }
    return samples;
}

std::vector<std::int32_t> make_fixed_constant_samples()
{
    std::vector<std::int32_t> samples;
    samples.reserve(192);
    for (int i = 0; i < 64; ++i) {
        samples.push_back(1024);
    }
    for (int i = 0; i < 64; ++i) {
        samples.push_back(i * 64);
    }
    const std::vector<std::int32_t> partitioned {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
        32704, -32768, 32704, -32768, 32704, -32768, 32704, -32768,
    };
    samples.insert(samples.end(), partitioned.begin(), partitioned.end());
    return samples;
}

std::vector<std::int32_t> make_generated_mixed_samples()
{
    std::vector<std::int32_t> samples;
    samples.reserve(512 * 3);
    for (int i = 0; i < 512; ++i) {
        samples.push_back(1024);
    }
    for (int i = 0; i < 512; ++i) {
        samples.push_back(static_cast<std::int32_t>((i * 64) - 16384));
    }
    auto lpc_friendly = make_lpc_friendly_samples();
    samples.insert(samples.end(), lpc_friendly.begin(), lpc_friendly.end());
    return samples;
}

ldcompress::opencl_detail::OpenClMonoAnalysisTaskPlan make_single_lpc_task_plan(
    const ldcompress::opencl_detail::FlacClSubframeTask& task)
{
    ldcompress::opencl_detail::OpenClMonoAnalysisTaskPlan plan;
    plan.residual_tasks.push_back(task);
    plan.selected_tasks.push_back(0);
    plan.residual_tasks_per_frame = 1;
    plan.estimate_tasks_per_frame = 1;
    return plan;
}

void test_metal_fixed_constant_analysis(const Options& options)
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::metal_support_built()) {
        std::cout << "Metal fixed/constant analysis skipped: Metal support was not built\n";
        return;
    }

    const auto device_index = first_available_metal_device_index(options.device_index);
    if (!device_index.has_value()) {
        std::cout << "Metal fixed/constant analysis skipped: no Metal device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = 64;
    task_options.max_lpc_order = 0;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;

    const auto plan = build_mono_analysis_task_plan(3, task_options);
    const auto samples = make_fixed_constant_samples();
    const auto expected = analyze_mono_fixed_constant_exact(samples, plan, 4);
    const auto result = ldcompress::metal_detail::run_metal_mono_fixed_constant_analysis(
        samples, plan, device_index, 4);
    require(!result.device_name.empty(), "Metal fixed/constant result did not report a device");
    require_analysis_matches(
        result, expected, "Metal exact fixed/constant analysis diverged from scalar oracle");
    require_rice_parameters_match_scalar(samples, result.best_tasks, result.best_rice_parameters,
        "Metal fixed/constant Rice sidecar diverged from scalar recompute");

    ldcompress::metal_detail::MetalMonoAnalysisSession session(device_index);
    const auto best_only = session.run_fixed_constant_best_analysis(samples, plan, 4);
    require(best_only.best_tasks.size() == result.best_tasks.size(),
        "Metal fixed/constant best-only task count mismatch");
    for (std::size_t i = 0; i < result.best_tasks.size(); ++i) {
        require_task_matches_task(best_only.best_tasks[i], result.best_tasks[i],
            "Metal fixed/constant best-only task diverged from full result");
    }
    require_rice_parameters_match_scalar(samples, best_only.best_tasks,
        best_only.best_rice_parameters,
        "Metal fixed/constant best-only Rice sidecar diverged from scalar recompute");

    require(result.best_tasks[0].data.type == kFlacClSubframeConstant,
        "constant frame did not select constant task");
    require(result.best_tasks[1].data.type == kFlacClSubframeFixed,
        "linear frame did not select fixed task");
    require(result.best_tasks[2].data.type == kFlacClSubframeFixed,
        "partitioned frame did not select fixed task");
    require(result.best_tasks[2].data.porder == 1,
        "partitioned frame did not select Rice partition order 1");

    const auto unpartitioned =
        ldcompress::metal_detail::run_metal_mono_fixed_constant_analysis(
            samples, plan, device_index, 0);
    require(unpartitioned.best_tasks[2].data.porder == 0,
        "Metal fixed/constant did not honor max partition order 0");
}

void test_metal_lpc_analysis(const Options& options)
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::metal_support_built()) {
        std::cout << "Metal LPC analysis skipped: Metal support was not built\n";
        return;
    }

    const auto device_index = first_available_metal_device_index(options.device_index);
    if (!device_index.has_value()) {
        std::cout << "Metal LPC analysis skipped: no Metal device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = 512;
    task_options.max_lpc_order = 12;
    task_options.include_constant = false;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;

    const auto samples = make_lpc_friendly_samples();
    const ldcompress::FlacFrameInfo frame_info {
        .frame_number = 0,
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .max_lpc_order = 12,
        .lpc_coefficient_precision = 12,
        .max_rice_partition_order = 5,
    };
    const auto best_lpc = ldcompress::analyze_mono_lpc_frame(samples, frame_info);
    require(best_lpc.has_value(), "native LPC analysis did not return a candidate for Metal test");
    const auto expected_task = analyze_mono_lpc_exact_task(
        samples, 0, task_options, best_lpc->order, 12, 5);
    require(expected_task.has_value(),
        "scalar exact LPC task analysis did not return a task for Metal test");

    auto input_task = *expected_task;
    input_task.data.size = 16 * 512;
    input_task.data.wbits = expected_task->data.wbits == 0 ? 1 : 0;
    input_task.data.abits = expected_task->data.abits == 1 ? 2 : 1;
    input_task.data.porder = 0;

    const auto single_result = ldcompress::metal_detail::run_metal_mono_lpc_analysis(
        samples, make_single_lpc_task_plan(input_task), device_index, 5);
    require(!single_result.device_name.empty(), "Metal LPC result did not report a device");
    require_lpc_task_matches(single_result.analyzed_tasks[0], *expected_task,
        "Metal exact LPC analyzed task diverged from scalar oracle");
    require_lpc_task_matches(single_result.best_tasks[0], *expected_task,
        "Metal exact LPC best task diverged from scalar oracle");
    require_rice_parameters_match_scalar(
        samples, single_result.best_tasks, single_result.best_rice_parameters,
        "Metal exact LPC Rice sidecar diverged from scalar recompute");
    require_selected_writer_recovers_tasks(
        samples,
        single_result.best_tasks,
        single_result.best_rice_parameters,
        task_options.max_lpc_order,
        12,
        5,
        "Metal exact LPC selected writer recost diverged from selected task");

    const auto expected_unpartitioned = analyze_mono_lpc_exact_task(
        samples, 0, task_options, best_lpc->order, 12, 0);
    require(expected_unpartitioned.has_value(),
        "scalar exact LPC task did not return unpartitioned result");
    const auto unpartitioned_result = ldcompress::metal_detail::run_metal_mono_lpc_analysis(
        samples, make_single_lpc_task_plan(input_task), device_index, 0);
    require_lpc_task_matches(unpartitioned_result.best_tasks[0], *expected_unpartitioned,
        "Metal exact LPC task did not honor max partition order 0");
}

void test_metal_generated_lpc_analysis(const Options& options)
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::metal_support_built()) {
        std::cout << "Metal generated LPC analysis skipped: Metal support was not built\n";
        return;
    }

    const auto device_index = first_available_metal_device_index(options.device_index);
    if (!device_index.has_value()) {
        std::cout << "Metal generated LPC analysis skipped: no Metal device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = 512;
    task_options.max_lpc_order = 12;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;

    const auto plan = build_mono_analysis_task_plan(3, task_options);
    const auto samples = make_generated_mixed_samples();
    const auto result = ldcompress::metal_detail::run_metal_mono_generated_analysis(
        samples, plan, device_index, 12, 5);
    require(!result.device_name.empty(),
        "Metal generated LPC result did not report a device");
    require(result.analyzed_tasks.size() == plan.residual_tasks.size(),
        "Metal generated LPC analyzed task count mismatch");
    require(result.best_tasks.size() == 3,
        "Metal generated LPC best task count mismatch");
    require_rice_parameters_match_scalar(samples, result.best_tasks, result.best_rice_parameters,
        "Metal generated LPC Rice sidecar diverged from scalar recompute");
    require_selected_writer_recovers_tasks(
        samples,
        result.best_tasks,
        result.best_rice_parameters,
        task_options.max_lpc_order,
        12,
        5,
        "Metal generated LPC selected writer recost diverged from selected task");

    ldcompress::metal_detail::MetalMonoAnalysisSession session(device_index);
    const auto best_only = session.run_generated_best_analysis(samples, plan, 12, 5);
    require(best_only.best_tasks.size() == result.best_tasks.size(),
        "Metal generated LPC best-only task count mismatch");
    for (std::size_t i = 0; i < result.best_tasks.size(); ++i) {
        require_lpc_task_matches(best_only.best_tasks[i], result.best_tasks[i],
            "Metal generated LPC best-only task diverged from full result");
    }
    require_rice_parameters_match_scalar(samples, best_only.best_tasks,
        best_only.best_rice_parameters,
        "Metal generated LPC best-only Rice sidecar diverged from scalar recompute");
    require_selected_writer_recovers_tasks(
        samples,
        best_only.best_tasks,
        best_only.best_rice_parameters,
        task_options.max_lpc_order,
        12,
        5,
        "Metal generated LPC best-only selected writer recost diverged from selected task");

    for (const auto& task : result.analyzed_tasks) {
        if (task.data.type == kFlacClSubframeLpc) {
            require(task.data.shift >= 0 && task.data.shift <= 15,
                "Metal generated LPC task shift is invalid");
            require(task.data.cbits > 0 && task.data.cbits <= 15,
                "Metal generated LPC task coefficient precision is invalid");
            require(task.data.size > 0 && task.data.size < std::numeric_limits<std::int32_t>::max(),
                "Metal generated LPC exact size is invalid");
            require(task.data.porder >= 0 && task.data.porder <= 5,
                "Metal generated LPC partition order is invalid");
            for (int i = 0; i < task.data.residualOrder; ++i) {
                require(signed_value_fits_bits(
                            task.coefs[static_cast<std::size_t>(i)],
                            static_cast<unsigned>(task.data.cbits)),
                    "Metal generated LPC coefficient does not fit cbits");
            }
        }
    }

    require(result.best_tasks[0].data.type == kFlacClSubframeConstant,
        "Metal generated constant frame did not select constant");
    require(result.best_tasks[1].data.type == kFlacClSubframeFixed,
        "Metal generated linear frame did not select fixed");
    require(result.best_tasks[2].data.size > 0 &&
            result.best_tasks[2].data.size < std::numeric_limits<std::int32_t>::max(),
        "Metal generated mixed frame selected an invalid task size");

    const auto unpartitioned = ldcompress::metal_detail::run_metal_mono_generated_analysis(
        samples, plan, device_index, 12, 0);
    for (const auto& task : unpartitioned.best_tasks) {
        require(task.data.porder == 0,
            "Metal generated LPC did not honor max partition order 0");
    }
}

void test_metal_order_guess_mean_rice_profile_smoke(const Options& options)
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::metal_support_built()) {
        std::cout << "Metal order-guess mean Rice profile skipped: Metal support was not built\n";
        return;
    }

    const auto device_index = first_available_metal_device_index(options.device_index);
    if (!device_index.has_value()) {
        std::cout << "Metal order-guess mean Rice profile skipped: no Metal device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = 512;
    task_options.max_lpc_order = 12;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;
    task_options.use_gpu_fixed_order_guess = true;
    task_options.analysis_profile =
        ldcompress::NativeAnalysisProfile::OrderGuessMeanEstimateRice;

    const auto samples = make_generated_mixed_samples();
    const auto plan = build_mono_analysis_task_plan_for_samples(samples, 3, task_options);
    require(plan.fixed_order_guess_on_gpu,
        "Metal speed-profile plan did not request GPU fixed-order guess");
    require(plan.residual_tasks_per_frame == 9,
        "unexpected Metal speed-profile task count per frame");

    ldcompress::metal_detail::MetalMonoAnalysisSession session(device_index);
    const auto result = session.run_generated_best_analysis(samples, plan, 12, 6);
    require(result.best_tasks.size() == 3,
        "Metal speed-profile best task count mismatch");
    require_rice_parameters_valid(
        result.best_tasks,
        result.best_rice_parameters,
        "Metal speed-profile Rice sidecar is invalid");
    require_selected_writer_accepts_tasks(
        samples,
        result.best_tasks,
        result.best_rice_parameters,
        task_options.max_lpc_order,
        12,
        6,
        "Metal speed-profile selected writer recost diverged from selected task");
    for (const auto& task : result.best_tasks) {
        require(task.data.porder >= 0 && task.data.porder <= 6,
            "Metal speed-profile selected invalid partition order");
        require(task.data.size > 0 &&
                task.data.size < std::numeric_limits<std::int32_t>::max(),
            "Metal speed-profile selected invalid task size");
    }
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_args(argc, argv);
        test_metal_fixed_constant_analysis(options);
        test_metal_lpc_analysis(options);
        test_metal_generated_lpc_analysis(options);
        test_metal_order_guess_mean_rice_profile_smoke(options);
    } catch (const std::exception& ex) {
        std::cerr << "test_metal_analysis: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
