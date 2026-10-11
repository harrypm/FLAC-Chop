#include "opencl_analysis.h"
#include "opencl_analysis_test_support.h"
#include "flac_native_writer.h"
#include "vulkan_analysis.h"
#include "vulkan_analysis_test_support.h"
#include "vulkan_devices.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
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
        throw std::runtime_error("empty Vulkan device index");
    }
    std::size_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error("invalid Vulkan device index: " + std::string(text));
        }
        value = (value * 10U) + static_cast<std::size_t>(ch - '0');
    }
    return value;
}

enum class TestCase {
    FixedConstant,
    Lpc,
    GeneratedLpc,
    MeanRiceProfiles,
};

std::string_view test_case_name(TestCase test_case)
{
    switch (test_case) {
    case TestCase::FixedConstant:
        return "fixed-constant";
    case TestCase::Lpc:
        return "lpc";
    case TestCase::GeneratedLpc:
        return "generated-lpc";
    case TestCase::MeanRiceProfiles:
        return "mean-rice-profiles";
    }
    throw std::runtime_error("unknown Vulkan analysis test case");
}

TestCase parse_test_case(std::string_view text)
{
    if (text == "fixed-constant") {
        return TestCase::FixedConstant;
    }
    if (text == "lpc") {
        return TestCase::Lpc;
    }
    if (text == "generated-lpc") {
        return TestCase::GeneratedLpc;
    }
    if (text == "mean-rice-profiles") {
        return TestCase::MeanRiceProfiles;
    }
    throw std::runtime_error(
        "unknown Vulkan analysis test case: " + std::string(text));
}

struct Options {
    std::optional<std::size_t> device_index;
    std::optional<TestCase> selected_case;
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
        } else if (arg == "--case") {
            if (++i >= argc) {
                throw std::runtime_error("--case requires a value");
            }
            options.selected_case = parse_test_case(argv[i]);
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }
    return options;
}

class ProgressSpan {
public:
    explicit ProgressSpan(std::string_view label)
        : label_(label),
          start_(std::chrono::steady_clock::now())
    {
        std::cout << "Vulkan analysis progress: start " << label_ << '\n' << std::flush;
    }

    ~ProgressSpan()
    {
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_);
        std::cout << "Vulkan analysis progress: done " << label_ << " ("
                  << std::fixed << std::setprecision(3) << elapsed.count()
                  << "s)\n" << std::flush;
    }

    ProgressSpan(const ProgressSpan&) = delete;
    ProgressSpan& operator=(const ProgressSpan&) = delete;

private:
    std::string_view label_;
    std::chrono::steady_clock::time_point start_;
};

std::optional<std::size_t> first_available_vulkan_analysis_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::vulkan_support_built()) {
        return std::nullopt;
    }

    const auto devices = ldcompress::list_vulkan_devices();
    if (requested_index.has_value()) {
        if (*requested_index >= devices.size()) {
            throw std::runtime_error("requested Vulkan device index is out of range");
        }
        const auto& device = devices[*requested_index];
        if (device.available && device.shader_int64) {
            return device.index;
        }
        return std::nullopt;
    }

    for (const auto& device : devices) {
        if (device.available && device.shader_int64 && device.device_type != "cpu") {
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
    require(actual.data.type == expected.data.type, label);
    require(actual.data.residualOrder == expected.data.residualOrder, label);
    require(actual.data.samplesOffs == expected.data.samplesOffs, label);
    require(actual.data.wbits == expected.data.wbits, label);
    require(actual.data.abits == expected.data.abits, label);
    require(actual.data.porder == expected.data.porder, label);
    require(actual.data.size == expected.data.size, label);
}

void require_lpc_task_matches(
    const ldcompress::opencl_detail::FlacClSubframeTask& actual,
    const ldcompress::opencl_detail::FlacClSubframeTask& expected,
    const char* label)
{
    require_task_matches_task(actual, expected, label);
    require(actual.data.shift == expected.data.shift, label);
    require(actual.data.cbits == expected.data.cbits, label);
    for (int i = 0; i < expected.data.residualOrder; ++i) {
        require(actual.coefs[static_cast<std::size_t>(i)] ==
                expected.coefs[static_cast<std::size_t>(i)],
            label);
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

void require_mean_estimate_sizes_diverge(
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& recosted_tasks,
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& estimated_tasks,
    const char* label)
{
    using namespace ldcompress::opencl_detail;

    require(recosted_tasks.size() == estimated_tasks.size(), label);
    bool saw_estimated_predictive_size = false;
    for (std::size_t i = 0; i < recosted_tasks.size(); ++i) {
        const auto& recosted = recosted_tasks[i];
        const auto& estimated = estimated_tasks[i];
        require(estimated.data.type == recosted.data.type, label);
        require(estimated.data.residualOrder == recosted.data.residualOrder, label);
        require(estimated.data.samplesOffs == recosted.data.samplesOffs, label);
        require(estimated.data.porder == recosted.data.porder, label);
        if ((estimated.data.type == kFlacClSubframeFixed ||
                estimated.data.type == kFlacClSubframeLpc) &&
            recosted.data.size != std::numeric_limits<std::int32_t>::max()) {
            require(estimated.data.size > 0, label);
            require(estimated.data.size < std::numeric_limits<std::int32_t>::max(), label);
            if (estimated.data.size != recosted.data.size) {
                saw_estimated_predictive_size = true;
            }
        } else {
            require(estimated.data.size == recosted.data.size, label);
        }
    }
    require(saw_estimated_predictive_size, label);
}

void require_rice_parameters_match(
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& tasks,
    const std::vector<ldcompress::opencl_detail::FlacClRiceParameterSet>& actual,
    const std::vector<ldcompress::opencl_detail::FlacClRiceParameterSet>& expected,
    const char* label)
{
    require_rice_parameters_valid(tasks, actual, label);
    require_rice_parameters_valid(tasks, expected, label);
    for (std::size_t frame = 0; frame < tasks.size(); ++frame) {
        const auto& task = tasks[frame];
        if (task.data.type != ldcompress::opencl_detail::kFlacClSubframeFixed &&
            task.data.type != ldcompress::opencl_detail::kFlacClSubframeLpc) {
            continue;
        }
        const auto partition_count =
            std::size_t {1} << static_cast<unsigned>(task.data.porder);
        for (std::size_t partition = 0; partition < partition_count; ++partition) {
            require(actual[frame].parameters[partition] ==
                    expected[frame].parameters[partition],
                label);
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
            throw std::runtime_error("invalid fixed predictor order in Vulkan sidecar test");
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
        throw std::runtime_error("unsupported task type in Vulkan sidecar test");
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
    require(task.data.porder >= 0, "Vulkan sidecar task has negative partition order");
    require(static_cast<std::size_t>(task.data.porder) <= kFlacClMaxRicePartitionOrder,
        "Vulkan sidecar task partition order exceeds max");

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
        "Vulkan sidecar residual partition accounting mismatch");
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

std::vector<std::int32_t> make_lpc_friendly_alternate_samples()
{
    std::vector<std::int32_t> samples;
    samples.reserve(512);
    for (int i = 0; i < 512; ++i) {
        const auto sample =
            ((i * 704) + ((i % 23) * 256) + ((i % 7) * 640)) % 32768;
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

std::vector<std::int32_t> make_order_guess_profile_samples()
{
    auto samples = make_generated_mixed_samples();
    const auto alternate = make_lpc_friendly_alternate_samples();
    samples.insert(samples.end(), alternate.begin(), alternate.end());
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

ldcompress::opencl_detail::OpenClMonoAnalysisTaskPlan make_lpc_order_task_plan(
    const std::vector<std::int32_t>& samples,
    std::size_t frame_count,
    const ldcompress::opencl_detail::OpenClMonoAnalysisTaskOptions& options,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& expected_tasks,
    std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& expected_best_tasks)
{
    using namespace ldcompress::opencl_detail;

    OpenClMonoAnalysisTaskPlan plan;
    plan.residual_tasks_per_frame = options.max_lpc_order;
    plan.estimate_tasks_per_frame = options.max_lpc_order;

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto frame_task_base = plan.residual_tasks.size();
        auto best_index = expected_tasks.size();
        auto best_size = std::numeric_limits<std::int32_t>::max();
        for (unsigned order = 1; order <= options.max_lpc_order; ++order) {
            const auto expected = analyze_mono_lpc_exact_task(
                samples, frame, options, order, lpc_coefficient_precision,
                max_rice_partition_order);
            require(expected.has_value(), "scalar exact LPC per-order task was not produced");

            auto input = *expected;
            input.data.size = 16 * static_cast<int>(options.frame_samples);
            input.data.abits = 0;
            input.data.porder = 0;

            if (expected->data.size < best_size) {
                best_index = expected_tasks.size();
                best_size = expected->data.size;
            }

            plan.residual_tasks.push_back(input);
            expected_tasks.push_back(*expected);
        }

        for (std::size_t i = 0; i < plan.estimate_tasks_per_frame; ++i) {
            plan.selected_tasks.push_back(static_cast<std::int32_t>(frame_task_base + i));
        }
        expected_best_tasks.push_back(expected_tasks.at(best_index));
    }

    return plan;
}

void test_vulkan_fixed_constant_analysis(const Options& options)
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::vulkan_support_built()) {
        std::cout << "Vulkan fixed/constant analysis skipped: Vulkan support was not built\n";
        return;
    }

    const auto device_index =
        first_available_vulkan_analysis_device_index(options.device_index);
    if (!device_index.has_value()) {
        std::cout << "Vulkan fixed/constant analysis skipped: no suitable Vulkan device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = 64;
    task_options.max_lpc_order = 0;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;

    const auto tasks_per_frame = mono_analysis_tasks_per_frame(task_options);
    require(tasks_per_frame == 6, "unexpected fixed/constant task count");
    const auto plan = build_mono_analysis_task_plan(3, task_options);
    const auto samples = make_fixed_constant_samples();

    const auto expected = analyze_mono_fixed_constant_exact(samples, plan, 4);
    const auto result = ldcompress::vulkan_detail::run_vulkan_mono_fixed_constant_analysis(
        samples, plan, device_index, 4);
    require(!result.device_name.empty(), "Vulkan fixed/constant result did not report a device");
    require_analysis_matches(
        result, expected, "Vulkan exact fixed/constant analysis diverged from scalar oracle");
    ldcompress::vulkan_detail::VulkanMonoExactAnalysisSession session(device_index);
    const auto best_only = session.run_fixed_constant_best_analysis(samples, plan, 4);
    require(best_only.best_tasks.size() == result.best_tasks.size(),
        "Vulkan fixed/constant best-only task count mismatch");
    for (std::size_t i = 0; i < result.best_tasks.size(); ++i) {
        require_task_matches_task(best_only.best_tasks[i], result.best_tasks[i],
            "Vulkan fixed/constant best-only task diverged from full result");
    }
    require_rice_parameters_match(
        result.best_tasks,
        best_only.best_rice_parameters,
        result.best_rice_parameters,
        "Vulkan fixed/constant Rice parameter sidecar mismatch");
    require_rice_parameters_match_scalar(samples, result.best_tasks, result.best_rice_parameters,
        "Vulkan fixed/constant Rice parameter sidecar diverged from scalar recompute");

    require(result.best_tasks[0].data.type == kFlacClSubframeConstant,
        "constant frame did not select constant task");
    require(result.best_tasks[0].data.size == 24,
        "constant frame selected size mismatch");
    require(result.best_tasks[1].data.type == kFlacClSubframeFixed,
        "linear frame did not select fixed task");
    require(result.best_tasks[1].data.residualOrder == 2,
        "linear frame did not select fixed order 2");
    require(result.best_tasks[2].data.type == kFlacClSubframeFixed,
        "partitioned frame did not select fixed task");
    require(result.best_tasks[2].data.residualOrder == 0,
        "partitioned frame did not select fixed order 0");
    require(result.best_tasks[2].data.porder == 1,
        "partitioned frame did not select Rice partition order 1");

    const auto expected_unpartitioned = analyze_mono_fixed_constant_exact(samples, plan, 0);
    const auto result_unpartitioned =
        ldcompress::vulkan_detail::run_vulkan_mono_fixed_constant_analysis(
            samples, plan, device_index, 0);
    require_analysis_matches(result_unpartitioned, expected_unpartitioned,
        "Vulkan exact fixed/constant analysis did not honor max partition order 0");
    require_rice_parameters_match_scalar(
        samples, result_unpartitioned.best_tasks, result_unpartitioned.best_rice_parameters,
        "Vulkan unpartitioned fixed/constant Rice sidecar diverged from scalar recompute");
    require(result_unpartitioned.best_tasks[2].data.porder == 0,
        "partitioned frame did not honor max partition order 0");
    require(result_unpartitioned.best_tasks[2].data.size > result.best_tasks[2].data.size,
        "Vulkan exact partition search did not improve fixed task size");
}

void test_vulkan_lpc_analysis(const Options& options)
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::vulkan_support_built()) {
        std::cout << "Vulkan LPC analysis skipped: Vulkan support was not built\n";
        return;
    }

    const auto device_index =
        first_available_vulkan_analysis_device_index(options.device_index);
    if (!device_index.has_value()) {
        std::cout << "Vulkan LPC analysis skipped: no suitable Vulkan device\n";
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
    require(best_lpc.has_value(), "native LPC analysis did not return a candidate for Vulkan test");
    const auto expected_task = analyze_mono_lpc_exact_task(
        samples, 0, task_options, best_lpc->order, 12, 5);
    require(expected_task.has_value(),
        "scalar exact LPC task analysis did not return a task for Vulkan test");

    auto input_task = *expected_task;
    input_task.data.size = 16 * 512;
    input_task.data.wbits = expected_task->data.wbits == 0 ? 1 : 0;
    input_task.data.abits = expected_task->data.abits == 1 ? 2 : 1;
    input_task.data.porder = 0;

    auto single_plan = make_single_lpc_task_plan(input_task);
    const auto single_result = ldcompress::vulkan_detail::run_vulkan_mono_lpc_analysis(
        samples, single_plan, device_index, 5);
    require(!single_result.device_name.empty(),
        "Vulkan LPC result did not report a device");
    require_lpc_task_matches(single_result.analyzed_tasks[0], *expected_task,
        "Vulkan exact LPC analyzed task diverged from scalar oracle");
    require_lpc_task_matches(single_result.best_tasks[0], *expected_task,
        "Vulkan exact LPC best task diverged from scalar oracle");
    require_rice_parameters_match_scalar(
        samples, single_result.best_tasks, single_result.best_rice_parameters,
        "Vulkan exact LPC Rice parameter sidecar diverged from scalar recompute");

    const auto expected_unpartitioned = analyze_mono_lpc_exact_task(
        samples, 0, task_options, best_lpc->order, 12, 0);
    require(expected_unpartitioned.has_value(),
        "scalar exact LPC task did not return unpartitioned result");
    const auto unpartitioned_result = ldcompress::vulkan_detail::run_vulkan_mono_lpc_analysis(
        samples, single_plan, device_index, 0);
    require_lpc_task_matches(unpartitioned_result.best_tasks[0], *expected_unpartitioned,
        "Vulkan exact LPC task did not honor max partition order 0");
    require_rice_parameters_match_scalar(
        samples, unpartitioned_result.best_tasks, unpartitioned_result.best_rice_parameters,
        "Vulkan unpartitioned LPC Rice sidecar diverged from scalar recompute");

    auto two_frame_samples = samples;
    const auto alternate = make_lpc_friendly_alternate_samples();
    two_frame_samples.insert(two_frame_samples.end(), alternate.begin(), alternate.end());

    std::vector<FlacClSubframeTask> expected_tasks;
    std::vector<FlacClSubframeTask> expected_best_tasks;
    const auto multi_plan = make_lpc_order_task_plan(
        two_frame_samples, 2, task_options, 12, 5, expected_tasks, expected_best_tasks);

    const auto multi_result = ldcompress::vulkan_detail::run_vulkan_mono_lpc_analysis(
        two_frame_samples, multi_plan, device_index, 5);
    require(multi_result.analyzed_tasks.size() == expected_tasks.size(),
        "Vulkan LPC multi-order analyzed task count mismatch");
    require(multi_result.best_tasks.size() == expected_best_tasks.size(),
        "Vulkan LPC multi-order best task count mismatch");
    for (std::size_t i = 0; i < expected_tasks.size(); ++i) {
        require_lpc_task_matches(multi_result.analyzed_tasks[i], expected_tasks[i],
            "Vulkan exact LPC multi-order analyzed task diverged from scalar oracle");
    }
    for (std::size_t i = 0; i < expected_best_tasks.size(); ++i) {
        require_lpc_task_matches(multi_result.best_tasks[i], expected_best_tasks[i],
            "Vulkan exact LPC multi-order best task diverged from scalar oracle");
    }
    require_rice_parameters_match_scalar(
        two_frame_samples, multi_result.best_tasks, multi_result.best_rice_parameters,
        "Vulkan exact LPC multi-order Rice sidecar diverged from scalar recompute");
}

void test_vulkan_generated_lpc_analysis(const Options& options)
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::vulkan_support_built()) {
        std::cout << "Vulkan generated LPC analysis skipped: Vulkan support was not built\n";
        return;
    }

    const auto device_index =
        first_available_vulkan_analysis_device_index(options.device_index);
    if (!device_index.has_value()) {
        std::cout << "Vulkan generated LPC analysis skipped: no suitable Vulkan device\n";
        return;
    }

    OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = 512;
    task_options.max_lpc_order = 12;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;

    const auto plan = build_mono_analysis_task_plan(3, task_options);
    require(plan.residual_tasks_per_frame == 32,
        "unexpected generated Vulkan task count per frame");
    const auto samples = make_generated_mixed_samples();

    const auto result = ldcompress::vulkan_detail::run_vulkan_mono_generated_analysis(
        samples, plan, device_index, 12, 5);
    require(!result.device_name.empty(),
        "Vulkan generated LPC result did not report a device");
    require(result.analyzed_tasks.size() == plan.residual_tasks.size(),
        "Vulkan generated LPC analyzed task count mismatch");
    require(result.best_tasks.size() == 3,
        "Vulkan generated LPC best task count mismatch");
    ldcompress::vulkan_detail::VulkanMonoExactAnalysisSession session(device_index);
    const auto best_only = session.run_generated_best_analysis(samples, plan, 12, 5);
    require(best_only.best_tasks.size() == result.best_tasks.size(),
        "Vulkan generated LPC best-only task count mismatch");
    for (std::size_t i = 0; i < result.best_tasks.size(); ++i) {
        require_lpc_task_matches(best_only.best_tasks[i], result.best_tasks[i],
            "Vulkan generated LPC best-only task diverged from full result");
    }
    require_rice_parameters_match(
        result.best_tasks,
        best_only.best_rice_parameters,
        result.best_rice_parameters,
        "Vulkan generated LPC Rice parameter sidecar mismatch");
    require_rice_parameters_match_scalar(samples, result.best_tasks, result.best_rice_parameters,
        "Vulkan generated LPC Rice parameter sidecar diverged from scalar recompute");

    for (std::size_t frame = 0; frame < 3; ++frame) {
        const auto task_base = frame * plan.residual_tasks_per_frame;
        for (std::size_t task_offset = 0; task_offset < 26; ++task_offset) {
            const auto& task = result.analyzed_tasks[task_base + task_offset];
            require(task.data.type == kFlacClSubframeLpc,
                "Vulkan generated LPC prefix task changed type");
            require(task.data.shift >= 0 && task.data.shift <= 15,
                "Vulkan generated LPC task shift is invalid");
            require(task.data.cbits > 0 && task.data.cbits <= 15,
                "Vulkan generated LPC task coefficient precision is invalid");
            require(task.data.size > 0 && task.data.size < std::numeric_limits<std::int32_t>::max(),
                "Vulkan generated LPC exact size is invalid");
            require(task.data.porder >= 0 && task.data.porder <= 5,
                "Vulkan generated LPC partition order is invalid");
            for (int i = 0; i < task.data.residualOrder; ++i) {
                require(signed_value_fits_bits(
                            task.coefs[static_cast<std::size_t>(i)],
                            static_cast<unsigned>(task.data.cbits)),
                    "Vulkan generated LPC coefficient does not fit cbits");
            }
        }
    }

    require(result.best_tasks[0].data.type == kFlacClSubframeConstant,
        "Vulkan generated constant frame did not select constant");
    require(result.best_tasks[1].data.type == kFlacClSubframeFixed,
        "Vulkan generated linear frame did not select fixed");
    require(result.best_tasks[2].data.type == kFlacClSubframeLpc ||
            result.best_tasks[2].data.type == kFlacClSubframeFixed,
        "Vulkan generated mixed frame selected an unexpected task type");
    require(result.best_tasks[2].data.size > 0 &&
            result.best_tasks[2].data.size < std::numeric_limits<std::int32_t>::max(),
        "Vulkan generated mixed frame selected an invalid task size");

    OpenClMonoAnalysisTaskPlan exact_plan = plan;
    exact_plan.residual_tasks = result.analyzed_tasks;
    const auto exact_result = ldcompress::vulkan_detail::run_vulkan_mono_lpc_analysis(
        samples, exact_plan, device_index, 5);
    for (std::size_t i = 0; i < result.analyzed_tasks.size(); ++i) {
        require_lpc_task_matches(exact_result.analyzed_tasks[i], result.analyzed_tasks[i],
            "Vulkan generated LPC exact re-analysis changed a task");
    }
    for (std::size_t i = 0; i < result.best_tasks.size(); ++i) {
        require_lpc_task_matches(exact_result.best_tasks[i], result.best_tasks[i],
            "Vulkan generated LPC exact re-analysis changed a best task");
    }
    require_rice_parameters_match_scalar(
        samples, exact_result.best_tasks, exact_result.best_rice_parameters,
        "Vulkan generated LPC exact re-analysis Rice sidecar diverged from scalar recompute");

    const auto unpartitioned = ldcompress::vulkan_detail::run_vulkan_mono_generated_analysis(
        samples, plan, device_index, 12, 0);
    for (const auto& task : unpartitioned.best_tasks) {
        require(task.data.porder == 0,
            "Vulkan generated LPC did not honor max partition order 0");
    }
}

void test_vulkan_order_guess_mean_rice_profile_smoke(const Options& options)
{
    using namespace ldcompress::opencl_detail;

    if (!ldcompress::vulkan_support_built()) {
        std::cout << "Vulkan order-guess mean Rice profile skipped: Vulkan support was not built\n";
        return;
    }

    const auto device_index =
        first_available_vulkan_analysis_device_index(options.device_index);
    if (!device_index.has_value()) {
        std::cout << "Vulkan order-guess mean Rice profile skipped: no suitable Vulkan device\n";
        return;
    }

    const auto samples = make_order_guess_profile_samples();

    OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = 512;
    task_options.max_lpc_order = 12;
    task_options.include_constant = true;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;
    task_options.use_gpu_fixed_order_guess = true;
    task_options.analysis_profile = ldcompress::NativeAnalysisProfile::OrderGuessMeanRice;
    const auto plan =
        build_mono_analysis_task_plan_for_samples(samples, 4, task_options);

    require(plan.fixed_order_guess_on_gpu,
        "Vulkan mean Rice smoke did not retain GPU fixed-order pruning");
    require(plan.analysis_profile == ldcompress::NativeAnalysisProfile::OrderGuessMeanRice,
        "Vulkan mean Rice smoke did not retain analysis profile");
    require(plan.residual_tasks_per_frame == 9,
        "Vulkan mean Rice smoke task shape mismatch");
    require(plan.selected_tasks.size() == plan.residual_tasks.size(),
        "Vulkan mean Rice smoke selected task count mismatch");

    auto estimate_options = task_options;
    estimate_options.analysis_profile =
        ldcompress::NativeAnalysisProfile::OrderGuessMeanEstimateRice;
    const auto estimate_plan =
        build_mono_analysis_task_plan_for_samples(samples, 4, estimate_options);
    require(estimate_plan.fixed_order_guess_on_gpu,
        "Vulkan mean-estimate Rice smoke did not retain GPU fixed-order pruning");
    require(estimate_plan.analysis_profile ==
            ldcompress::NativeAnalysisProfile::OrderGuessMeanEstimateRice,
        "Vulkan mean-estimate Rice smoke did not retain analysis profile");
    require(estimate_plan.residual_tasks_per_frame == plan.residual_tasks_per_frame,
        "Vulkan mean-estimate Rice smoke task shape mismatch");
    require(estimate_plan.residual_tasks.size() == plan.residual_tasks.size(),
        "Vulkan mean-estimate Rice smoke residual task count mismatch");
    require(estimate_plan.selected_tasks.size() == plan.selected_tasks.size(),
        "Vulkan mean-estimate Rice smoke selected task count mismatch");

    const auto result = ldcompress::vulkan_detail::run_vulkan_mono_generated_analysis(
        samples, plan, device_index, 12, 5);
    require(result.best_tasks.size() == 4,
        "Vulkan mean Rice smoke best task count mismatch");
    require(result.analyzed_tasks.size() == plan.residual_tasks.size(),
        "Vulkan mean Rice smoke did not return analyzed tasks");
    for (std::size_t frame = 0; frame < result.best_tasks.size(); ++frame) {
        std::size_t live_fixed_tasks = 0;
        const auto task_base = frame * plan.residual_tasks_per_frame;
        for (std::size_t i = 0; i < plan.residual_tasks_per_frame; ++i) {
            const auto& task = result.analyzed_tasks[task_base + i];
            if (task.data.type == kFlacClSubframeFixed &&
                task.data.size != std::numeric_limits<std::int32_t>::max()) {
                ++live_fixed_tasks;
            }
        }
        require(live_fixed_tasks == 1,
            "Vulkan mean Rice smoke did not prune fixed tasks to one order");
    }
    require_rice_parameters_valid(result.best_tasks, result.best_rice_parameters,
        "Vulkan mean Rice smoke sidecar was invalid");

    ldcompress::vulkan_detail::VulkanMonoExactAnalysisSession session(device_index);
    const auto best_only = session.run_generated_best_analysis(samples, plan, 12, 5);
    require(best_only.best_tasks.size() == result.best_tasks.size(),
        "Vulkan mean Rice smoke best-only task count mismatch");
    require_rice_parameters_match(
        result.best_tasks,
        best_only.best_rice_parameters,
        result.best_rice_parameters,
        "Vulkan mean Rice smoke Rice parameter sidecar mismatch");

    for (std::size_t i = 0; i < result.best_tasks.size(); ++i) {
        require_task_matches_task(best_only.best_tasks[i], result.best_tasks[i],
            "Vulkan mean Rice smoke best-only task diverged from full result");
        require(result.best_tasks[i].data.size > 0,
            "Vulkan mean Rice smoke did not populate selected task size");
        require(result.best_tasks[i].data.porder >= 0 && result.best_tasks[i].data.porder <= 5,
            "Vulkan mean Rice smoke partition order out of range");
        if (result.best_tasks[i].data.type == kFlacClSubframeFixed ||
            result.best_tasks[i].data.type == kFlacClSubframeLpc) {
            (void)flaccl_task_to_selected_rice_parameters(
                result.best_tasks[i], result.best_rice_parameters[i]);
        }
    }

    const auto estimate_result =
        ldcompress::vulkan_detail::run_vulkan_mono_generated_analysis(
            samples, estimate_plan, device_index, 12, 5);
    require(estimate_result.best_tasks.size() == 4,
        "Vulkan mean-estimate Rice best task count mismatch");
    require(estimate_result.analyzed_tasks.size() == result.analyzed_tasks.size(),
        "Vulkan mean-estimate Rice analyzed task count mismatch");
    require_rice_parameters_valid(
        estimate_result.best_tasks, estimate_result.best_rice_parameters,
        "Vulkan mean-estimate Rice sidecar was invalid");
    require_mean_estimate_sizes_diverge(result.analyzed_tasks, estimate_result.analyzed_tasks,
        "Vulkan mean-estimate Rice profile still returned exact-recost predictive sizes");

    auto generic_reanalysis_plan = estimate_plan;
    generic_reanalysis_plan.residual_tasks = estimate_result.analyzed_tasks;
    const auto generic_reanalysis = ldcompress::vulkan_detail::run_vulkan_mono_lpc_analysis(
        samples, generic_reanalysis_plan, device_index, 5);
    require(generic_reanalysis.analyzed_tasks.size() == estimate_result.analyzed_tasks.size(),
        "Vulkan mean-estimate generic reanalysis task count mismatch");
    require(generic_reanalysis.best_tasks.size() == estimate_result.best_tasks.size(),
        "Vulkan mean-estimate generic reanalysis best task count mismatch");
    for (std::size_t i = 0; i < estimate_result.analyzed_tasks.size(); ++i) {
        const auto& expected = estimate_result.analyzed_tasks[i];
        const auto& actual = generic_reanalysis.analyzed_tasks[i];
        if (expected.data.type == kFlacClSubframeLpc) {
            require_lpc_task_matches(actual, expected,
                "Vulkan mean-estimate generic reanalysis LPC task diverged from fast path");
        } else {
            require_task_matches_task(actual, expected,
                "Vulkan mean-estimate generic reanalysis task diverged from fast path");
        }
    }
    for (std::size_t i = 0; i < estimate_result.best_tasks.size(); ++i) {
        const auto& expected = estimate_result.best_tasks[i];
        const auto& actual = generic_reanalysis.best_tasks[i];
        if (expected.data.type == kFlacClSubframeLpc) {
            require_lpc_task_matches(actual, expected,
                "Vulkan mean-estimate generic reanalysis best LPC task diverged from fast path");
        } else {
            require_task_matches_task(actual, expected,
                "Vulkan mean-estimate generic reanalysis best task diverged from fast path");
        }
    }
    require_rice_parameters_match(
        estimate_result.best_tasks,
        generic_reanalysis.best_rice_parameters,
        estimate_result.best_rice_parameters,
        "Vulkan mean-estimate generic reanalysis Rice sidecar diverged from fast path");

    const auto estimate_best =
        session.run_generated_best_analysis(samples, estimate_plan, 12, 5);
    require(estimate_best.best_tasks.size() == estimate_result.best_tasks.size(),
        "Vulkan mean-estimate Rice best-only task count mismatch");
    require_rice_parameters_match(
        estimate_result.best_tasks,
        estimate_best.best_rice_parameters,
        estimate_result.best_rice_parameters,
        "Vulkan mean-estimate Rice Rice parameter sidecar mismatch");
    for (std::size_t i = 0; i < estimate_result.best_tasks.size(); ++i) {
        require_task_matches_task(estimate_best.best_tasks[i], estimate_result.best_tasks[i],
            "Vulkan mean-estimate Rice best-only task diverged from full result");
        if (estimate_result.best_tasks[i].data.type == kFlacClSubframeFixed ||
            estimate_result.best_tasks[i].data.type == kFlacClSubframeLpc) {
            (void)flaccl_task_to_selected_rice_parameters(
                estimate_result.best_tasks[i], estimate_result.best_rice_parameters[i]);
        }
    }

    auto subdivide_options = task_options;
    subdivide_options.analysis_profile =
        ldcompress::NativeAnalysisProfile::SubdivideTukey3MeanRice;
    const auto subdivide_plan =
        build_mono_analysis_task_plan_for_samples(samples, 4, subdivide_options);
    auto subdivide_estimate_options = task_options;
    subdivide_estimate_options.analysis_profile =
        ldcompress::NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice;
    const auto subdivide_estimate_plan =
        build_mono_analysis_task_plan_for_samples(samples, 4, subdivide_estimate_options);
    require(subdivide_plan.residual_tasks_per_frame ==
            subdivide_estimate_plan.residual_tasks_per_frame,
        "Vulkan subdivide mean-estimate Rice smoke task shape mismatch");

    const auto subdivide_result =
        ldcompress::vulkan_detail::run_vulkan_mono_generated_analysis(
            samples, subdivide_plan, device_index, 12, 5);
    const auto subdivide_estimate_result =
        ldcompress::vulkan_detail::run_vulkan_mono_generated_analysis(
            samples, subdivide_estimate_plan, device_index, 12, 5);
    require_rice_parameters_valid(
        subdivide_estimate_result.best_tasks,
        subdivide_estimate_result.best_rice_parameters,
        "Vulkan subdivide mean-estimate Rice sidecar was invalid");
    require_mean_estimate_sizes_diverge(
        subdivide_result.analyzed_tasks,
        subdivide_estimate_result.analyzed_tasks,
        "Vulkan subdivide mean-estimate Rice profile still returned exact-recost predictive sizes");
}

void run_test_case(TestCase test_case, const Options& options)
{
    const ProgressSpan progress(test_case_name(test_case));
    switch (test_case) {
    case TestCase::FixedConstant:
        test_vulkan_fixed_constant_analysis(options);
        return;
    case TestCase::Lpc:
        test_vulkan_lpc_analysis(options);
        return;
    case TestCase::GeneratedLpc:
        test_vulkan_generated_lpc_analysis(options);
        return;
    case TestCase::MeanRiceProfiles:
        test_vulkan_order_guess_mean_rice_profile_smoke(options);
        return;
    }
    throw std::runtime_error("unknown Vulkan analysis test case");
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_args(argc, argv);
        if (options.selected_case.has_value()) {
            run_test_case(*options.selected_case, options);
        } else {
            run_test_case(TestCase::FixedConstant, options);
            run_test_case(TestCase::Lpc, options);
            run_test_case(TestCase::GeneratedLpc, options);
            run_test_case(TestCase::MeanRiceProfiles, options);
        }
    } catch (const std::exception& ex) {
        std::cerr << "test_vulkan_analysis: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
