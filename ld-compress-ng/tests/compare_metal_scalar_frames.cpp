#include "flac_native_writer.h"
#include "lds_codec.h"
#include "metal_analysis.h"
#include "metal_devices.h"
#include "opencl_analysis.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMetalBatchFrames = 32;

struct Options {
    std::filesystem::path input;
    std::optional<std::size_t> device_index;
    unsigned frame_samples = 4608;
    unsigned max_lpc_order = 12;
    unsigned lpc_precision = 12;
    unsigned max_rice_partition_order = 5;
    std::size_t start_frame = 0;
    std::optional<std::size_t> max_frames;
    std::size_t mismatch_limit = 20;
    bool show_coefficients = false;
};

struct DecisionView {
    std::string kind;
    unsigned order = 0;
    unsigned rice_partition_order = 0;
    unsigned wasted_bits = 0;
    unsigned coefficient_precision = 0;
    int quantization_shift = 0;
    std::string lpc_window;
    std::string lpc_quantization;
    std::optional<std::size_t> task_slot;
    std::uint64_t estimated_bits = 0;
    std::vector<std::int32_t> coefficients;
};

struct TaskShape {
    std::size_t tasks_per_frame = 0;
    std::size_t lpc_tasks_per_window = 0;
    std::size_t total_lpc_tasks = 0;
};

struct Summary {
    std::size_t frames = 0;
    std::uint64_t scalar_bits = 0;
    std::uint64_t metal_bits = 0;
    std::uint64_t metal_recost_bits = 0;
    std::size_t shape_mismatches = 0;
    std::size_t bit_mismatches = 0;
    std::size_t recost_mismatches = 0;
    std::size_t coefficient_mismatches = 0;
    std::size_t metal_larger = 0;
    std::size_t metal_smaller = 0;
    std::optional<std::size_t> first_metal_larger;
    std::size_t printed = 0;
};

void usage()
{
    std::cerr
        << "usage: compare_metal_scalar_frames [options] INPUT.lds\n"
        << "options:\n"
        << "  --device INDEX | --metal-device INDEX\n"
        << "  --frame-samples N\n"
        << "  --lpc-order N\n"
        << "  --lpc-precision N\n"
        << "  --rice-partition-order N\n"
        << "  --start-frame N\n"
        << "  --max-frames N\n"
        << "  --mismatch-limit N\n"
        << "  --show-coefficients\n";
}

std::uint64_t parse_u64(std::string_view text, std::string_view name)
{
    if (text.empty()) {
        throw std::runtime_error(std::string(name) + " cannot be empty");
    }
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            throw std::runtime_error(std::string(name) + " must be an unsigned integer: " +
                std::string(text));
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            throw std::runtime_error(std::string(name) + " is too large: " + std::string(text));
        }
        value = (value * 10U) + digit;
    }
    return value;
}

unsigned parse_bounded_unsigned(
    std::string_view text,
    std::string_view name,
    unsigned minimum,
    unsigned maximum)
{
    const auto value = parse_u64(text, name);
    if (value < minimum || value > maximum) {
        throw std::runtime_error(std::string(name) + " must be " +
            std::to_string(minimum) + ".." + std::to_string(maximum));
    }
    return static_cast<unsigned>(value);
}

Options parse_args(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        const auto require_value = [&](std::string_view option) -> std::string_view {
            if (++i >= argc) {
                throw std::runtime_error(std::string(option) + " requires a value");
            }
            return argv[i];
        };

        if (arg == "--device" || arg == "--metal-device") {
            options.device_index = static_cast<std::size_t>(
                parse_u64(require_value(arg), "Metal device index"));
        } else if (arg == "--frame-samples") {
            options.frame_samples = parse_bounded_unsigned(
                require_value(arg), "frame sample count", 16, 4608);
        } else if (arg == "--lpc-order") {
            options.max_lpc_order = parse_bounded_unsigned(
                require_value(arg), "LPC order", 1, 12);
        } else if (arg == "--lpc-precision") {
            options.lpc_precision = parse_bounded_unsigned(
                require_value(arg), "LPC precision", 1, 15);
        } else if (arg == "--rice-partition-order") {
            options.max_rice_partition_order = parse_bounded_unsigned(
                require_value(arg), "Rice partition order", 0, 8);
        } else if (arg == "--start-frame") {
            options.start_frame = static_cast<std::size_t>(
                parse_u64(require_value(arg), "start frame"));
        } else if (arg == "--max-frames") {
            options.max_frames = static_cast<std::size_t>(
                parse_u64(require_value(arg), "max frames"));
        } else if (arg == "--mismatch-limit") {
            options.mismatch_limit = static_cast<std::size_t>(
                parse_u64(require_value(arg), "mismatch limit"));
        } else if (arg == "--show-coefficients") {
            options.show_coefficients = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(0);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::runtime_error("unknown option: " + std::string(arg));
        } else if (options.input.empty()) {
            options.input = std::filesystem::path(arg);
        } else {
            throw std::runtime_error("expected only one input path");
        }
    }

    if (options.input.empty()) {
        throw std::runtime_error("missing input path");
    }
    if (options.max_frames.has_value() && *options.max_frames == 0) {
        throw std::runtime_error("max frames must be positive");
    }
    return options;
}

std::vector<std::int32_t> read_lds_samples(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input: " + path.string());
    }

    std::vector<std::int32_t> samples;
    std::vector<std::uint8_t> buffer(5 * 8192);
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got == 0) {
            break;
        }
        if ((got % 5) != 0) {
            throw std::runtime_error("truncated LDS input: byte count is not divisible by 5");
        }

        const auto groups = static_cast<std::size_t>(got / 5);
        samples.reserve(samples.size() + (groups * 4));
        for (std::size_t group = 0; group < groups; ++group) {
            ldcompress::PackedLdsGroup packed;
            std::memcpy(packed.data(), buffer.data() + (group * 5), packed.size());
            const auto unpacked = ldcompress::unpack_group(packed);
            for (const auto sample : unpacked) {
                samples.push_back(sample);
            }
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read input: " + path.string());
    }
    return samples;
}

std::optional<std::size_t> first_available_metal_device_index(
    std::optional<std::size_t> requested_index)
{
    if (!ldcompress::metal_support_built()) {
        throw std::runtime_error("Metal support was not built");
    }

    const auto devices = ldcompress::list_metal_devices();
    if (requested_index.has_value()) {
        if (*requested_index >= devices.size()) {
            throw std::runtime_error("requested Metal device index is out of range");
        }
        const auto& device = devices[*requested_index];
        if (!device.available) {
            throw std::runtime_error("requested Metal device is not available");
        }
        return device.index;
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

const char* kind_name(ldcompress::FlacSubframeKind kind)
{
    switch (kind) {
    case ldcompress::FlacSubframeKind::Constant:
        return "constant";
    case ldcompress::FlacSubframeKind::Verbatim:
        return "verbatim";
    case ldcompress::FlacSubframeKind::FixedRice:
        return "fixed";
    case ldcompress::FlacSubframeKind::LpcRice:
        return "lpc";
    }
    return "unknown";
}

const char* lpc_window_name(ldcompress::FlacLpcWindowKind window)
{
    switch (window) {
    case ldcompress::FlacLpcWindowKind::Rectangular:
        return "rectangular";
    case ldcompress::FlacLpcWindowKind::Tukey:
        return "tukey";
    case ldcompress::FlacLpcWindowKind::Welch:
        return "welch";
    }
    return "unknown";
}

const char* lpc_quantization_name(ldcompress::FlacLpcQuantizationKind quantization)
{
    switch (quantization) {
    case ldcompress::FlacLpcQuantizationKind::Independent:
        return "independent";
    case ldcompress::FlacLpcQuantizationKind::ErrorFeedback:
        return "error_feedback";
    }
    return "unknown";
}

unsigned decision_order(const ldcompress::FlacSubframeDecision& decision)
{
    switch (decision.kind) {
    case ldcompress::FlacSubframeKind::FixedRice:
        return decision.fixed_order;
    case ldcompress::FlacSubframeKind::LpcRice:
        return decision.lpc_order;
    case ldcompress::FlacSubframeKind::Constant:
    case ldcompress::FlacSubframeKind::Verbatim:
        return 0;
    }
    return 0;
}

std::string optional_slot_text(std::optional<std::size_t> slot)
{
    return slot.has_value() ? std::to_string(*slot) : std::string {};
}

std::string format_coefficients(const std::vector<std::int32_t>& coefficients)
{
    std::string formatted;
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        if (i != 0) {
            formatted += ';';
        }
        formatted += std::to_string(coefficients[i]);
    }
    return formatted;
}

std::size_t first_coefficient_mismatch(
    const std::vector<std::int32_t>& scalar,
    const std::vector<std::int32_t>& metal)
{
    const auto common = std::min(scalar.size(), metal.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (scalar[i] != metal[i]) {
            return i;
        }
    }
    return common;
}

std::uint64_t max_abs_coefficient_delta(
    const std::vector<std::int32_t>& scalar,
    const std::vector<std::int32_t>& metal)
{
    const auto common = std::min(scalar.size(), metal.size());
    std::uint64_t max_delta = 0;
    for (std::size_t i = 0; i < common; ++i) {
        const auto left = static_cast<std::int64_t>(scalar[i]);
        const auto right = static_cast<std::int64_t>(metal[i]);
        const auto delta = left > right
            ? static_cast<std::uint64_t>(left - right)
            : static_cast<std::uint64_t>(right - left);
        max_delta = std::max(max_delta, delta);
    }
    return max_delta;
}

ldcompress::opencl_detail::OpenClMonoAnalysisTaskOptions make_task_options(
    const Options& options)
{
    ldcompress::opencl_detail::OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = options.frame_samples;
    task_options.bits_per_sample = 16;
    task_options.max_lpc_order = options.max_lpc_order;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;
    task_options.include_constant = true;
    task_options.use_gpu_fixed_order_guess = true;
    return task_options;
}

TaskShape task_shape(const Options& options)
{
    const auto plan = ldcompress::opencl_detail::build_mono_analysis_task_plan(
        1, make_task_options(options));
    TaskShape shape {
        .tasks_per_frame = plan.residual_tasks_per_frame,
        .lpc_tasks_per_window = 0,
        .total_lpc_tasks = 0,
    };

    while (shape.total_lpc_tasks < plan.residual_tasks_per_frame &&
        plan.residual_tasks[shape.total_lpc_tasks].data.type ==
            ldcompress::opencl_detail::kFlacClSubframeLpc) {
        ++shape.total_lpc_tasks;
    }
    while (shape.lpc_tasks_per_window < shape.total_lpc_tasks &&
        plan.residual_tasks[shape.lpc_tasks_per_window].data.residualOrder ==
            static_cast<std::int32_t>(shape.lpc_tasks_per_window + 1U)) {
        ++shape.lpc_tasks_per_window;
    }
    if (shape.tasks_per_frame == 0 || shape.total_lpc_tasks == 0 ||
        shape.lpc_tasks_per_window == 0) {
        throw std::runtime_error("could not derive Metal generated-LPC task shape");
    }
    return shape;
}

std::optional<std::size_t> choose_best_task_slot(
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& analyzed_tasks,
    std::size_t frame,
    const TaskShape& shape)
{
    const auto frame_base = frame * shape.tasks_per_frame;
    if (frame_base + shape.tasks_per_frame > analyzed_tasks.size()) {
        return std::nullopt;
    }
    std::size_t best_slot = 0;
    auto best_size = analyzed_tasks[frame_base].data.size;
    for (std::size_t slot = 1; slot < shape.tasks_per_frame; ++slot) {
        const auto size = analyzed_tasks[frame_base + slot].data.size;
        if (size < best_size) {
            best_slot = slot;
            best_size = size;
        }
    }
    return best_slot;
}

std::string metal_lpc_window_name(std::optional<std::size_t> slot, const TaskShape& shape)
{
    if (!slot.has_value() || *slot >= shape.total_lpc_tasks) {
        return {};
    }
    const auto window = *slot / shape.lpc_tasks_per_window;
    if (window == 0) {
        return "rectangular";
    }
    if (window == 1) {
        return "tukey";
    }
    return "welch";
}

DecisionView make_scalar_view(
    const ldcompress::FlacSubframeDecision& decision,
    const std::vector<std::int32_t>& samples,
    const ldcompress::FlacFrameInfo& info)
{
    DecisionView view {
        .kind = kind_name(decision.kind),
        .order = decision_order(decision),
        .rice_partition_order = decision.rice_partition_order,
        .wasted_bits = decision.wasted_bits,
        .coefficient_precision = 0,
        .quantization_shift = 0,
        .lpc_window = {},
        .lpc_quantization = {},
        .task_slot = std::nullopt,
        .estimated_bits = decision.estimated_bits,
        .coefficients = {},
    };

    if (decision.kind == ldcompress::FlacSubframeKind::LpcRice) {
        const auto lpc = ldcompress::analyze_mono_lpc_order(samples, info, decision.lpc_order);
        if (lpc.has_value()) {
            view.coefficient_precision = lpc->coefficient_precision;
            view.quantization_shift = lpc->quantization_shift;
            view.lpc_window = lpc_window_name(lpc->window);
            view.lpc_quantization = lpc_quantization_name(lpc->quantization);
            view.coefficients = lpc->coefficients;
        }
    }
    return view;
}

DecisionView make_metal_view(
    const ldcompress::FlacSubframeDecision& decision,
    const ldcompress::FlacSelectedSubframe& selected,
    std::optional<std::size_t> task_slot,
    const TaskShape& shape)
{
    return DecisionView {
        .kind = kind_name(decision.kind),
        .order = decision_order(decision),
        .rice_partition_order = decision.rice_partition_order,
        .wasted_bits = decision.wasted_bits,
        .coefficient_precision = selected.coefficient_precision,
        .quantization_shift = selected.quantization_shift,
        .lpc_window = metal_lpc_window_name(task_slot, shape),
        .lpc_quantization = decision.kind == ldcompress::FlacSubframeKind::LpcRice
            ? "round_to_even"
            : "",
        .task_slot = task_slot,
        .estimated_bits = decision.estimated_bits,
        .coefficients = selected.coefficients,
    };
}

bool same_decision_shape(const DecisionView& scalar, const DecisionView& metal)
{
    return scalar.kind == metal.kind &&
        scalar.order == metal.order &&
        scalar.rice_partition_order == metal.rice_partition_order &&
        scalar.wasted_bits == metal.wasted_bits &&
        scalar.coefficient_precision == metal.coefficient_precision &&
        scalar.quantization_shift == metal.quantization_shift;
}

std::uint64_t recost_selected_subframe_bits(
    const std::vector<std::int32_t>& samples,
    const ldcompress::FlacFrameInfo& info,
    const ldcompress::FlacSelectedSubframe& selected)
{
    std::ostringstream sink;
    const auto recost = ldcompress::write_mono_selected_frame(sink, samples, info, selected);
    return recost.estimated_bits;
}

void print_header(bool show_coefficients)
{
    std::cout
        << "frame,sample_offset,sample_count,"
        << "scalar_kind,scalar_order,scalar_partition,scalar_wasted,"
        << "scalar_precision,scalar_shift,scalar_window,scalar_quantization,scalar_bits,"
        << "metal_kind,metal_order,metal_partition,metal_wasted,"
        << "metal_precision,metal_shift,metal_window,metal_quantization,"
        << "metal_task_slot,metal_bits,delta_bits,"
        << "metal_recost_bits,metal_recost_delta_bits,"
        << "coefficients_match,first_coefficient_mismatch,max_abs_coefficient_delta";
    if (show_coefficients) {
        std::cout << ",scalar_coefficients,metal_coefficients";
    }
    std::cout << '\n';
}

void print_row(
    std::size_t frame,
    unsigned frame_samples,
    const DecisionView& scalar,
    const DecisionView& metal,
    bool show_coefficients,
    std::uint64_t metal_recost_bits)
{
    const auto delta = static_cast<std::int64_t>(metal.estimated_bits) -
        static_cast<std::int64_t>(scalar.estimated_bits);
    const auto recost_delta = static_cast<std::int64_t>(metal_recost_bits) -
        static_cast<std::int64_t>(metal.estimated_bits);
    const auto first_mismatch = first_coefficient_mismatch(
        scalar.coefficients, metal.coefficients);
    std::cout << frame
              << ',' << (frame * static_cast<std::size_t>(frame_samples))
              << ',' << frame_samples
              << ',' << scalar.kind
              << ',' << scalar.order
              << ',' << scalar.rice_partition_order
              << ',' << scalar.wasted_bits
              << ',' << scalar.coefficient_precision
              << ',' << scalar.quantization_shift
              << ',' << scalar.lpc_window
              << ',' << scalar.lpc_quantization
              << ',' << scalar.estimated_bits
              << ',' << metal.kind
              << ',' << metal.order
              << ',' << metal.rice_partition_order
              << ',' << metal.wasted_bits
              << ',' << metal.coefficient_precision
              << ',' << metal.quantization_shift
              << ',' << metal.lpc_window
              << ',' << metal.lpc_quantization
              << ',' << optional_slot_text(metal.task_slot)
              << ',' << metal.estimated_bits
              << ',' << delta
              << ',' << metal_recost_bits
              << ',' << recost_delta
              << ',' << (scalar.coefficients == metal.coefficients ? "yes" : "no")
              << ',' << first_mismatch
              << ',' << max_abs_coefficient_delta(scalar.coefficients, metal.coefficients);
    if (show_coefficients) {
        std::cout << ',' << format_coefficients(scalar.coefficients)
                  << ',' << format_coefficients(metal.coefficients);
    }
    std::cout << '\n';
}

void update_summary(
    Summary& summary,
    std::size_t global_frame,
    const DecisionView& scalar,
    const DecisionView& metal,
    std::uint64_t metal_recost_bits)
{
    ++summary.frames;
    summary.scalar_bits += scalar.estimated_bits;
    summary.metal_bits += metal.estimated_bits;
    summary.metal_recost_bits += metal_recost_bits;

    if (!same_decision_shape(scalar, metal)) {
        ++summary.shape_mismatches;
    }
    if (scalar.estimated_bits != metal.estimated_bits) {
        ++summary.bit_mismatches;
    }
    if (metal_recost_bits != metal.estimated_bits) {
        ++summary.recost_mismatches;
    }
    if (scalar.coefficients != metal.coefficients) {
        ++summary.coefficient_mismatches;
    }
    if (metal.estimated_bits > scalar.estimated_bits) {
        ++summary.metal_larger;
        if (!summary.first_metal_larger.has_value()) {
            summary.first_metal_larger = global_frame;
        }
    } else if (metal.estimated_bits < scalar.estimated_bits) {
        ++summary.metal_smaller;
    }
}

bool should_print_mismatch(
    const DecisionView& scalar,
    const DecisionView& metal,
    std::uint64_t metal_recost_bits)
{
    return !same_decision_shape(scalar, metal) ||
        scalar.estimated_bits != metal.estimated_bits ||
        metal_recost_bits != metal.estimated_bits ||
        scalar.coefficients != metal.coefficients;
}

void analyze_batch(
    const Options& options,
    ldcompress::metal_detail::MetalMonoAnalysisSession& session,
    const TaskShape& shape,
    const std::vector<std::int32_t>& samples,
    std::size_t global_first_frame,
    Summary& summary)
{
    const auto frame_count = samples.size() / options.frame_samples;
    auto task_options = make_task_options(options);
    const auto plan = ldcompress::opencl_detail::build_mono_analysis_task_plan(
        frame_count, task_options);
    auto result = session.run_generated_analysis(
        samples, plan, options.lpc_precision, options.max_rice_partition_order);
    if (result.analyzed_tasks.size() != frame_count * shape.tasks_per_frame ||
        result.best_tasks.size() != frame_count ||
        result.best_rice_parameters.size() != frame_count) {
        throw std::runtime_error("Metal result frame count did not match input batch");
    }

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto offset = frame * static_cast<std::size_t>(options.frame_samples);
        const std::vector<std::int32_t> frame_samples(
            samples.begin() + static_cast<std::ptrdiff_t>(offset),
            samples.begin() + static_cast<std::ptrdiff_t>(offset + options.frame_samples));

        const ldcompress::FlacFrameInfo info {
            .frame_number = global_first_frame + frame,
            .sample_rate = 40000,
            .bits_per_sample = 16,
            .max_lpc_order = options.max_lpc_order,
            .lpc_coefficient_precision = options.lpc_precision,
            .max_rice_partition_order = options.max_rice_partition_order,
        };
        const auto scalar_decision = ldcompress::analyze_mono_best_frame(frame_samples, info);
        const auto scalar = make_scalar_view(scalar_decision, frame_samples, info);

        const auto& best_task = result.best_tasks[frame];
        auto selected = ldcompress::opencl_detail::flaccl_task_to_selected_subframe(best_task);
        selected.rice_parameters =
            ldcompress::opencl_detail::flaccl_task_to_selected_rice_parameters(
                best_task, result.best_rice_parameters[frame]);
        const auto metal_decision =
            ldcompress::opencl_detail::flaccl_task_to_subframe_decision(best_task);
        const auto metal = make_metal_view(
            metal_decision,
            selected,
            choose_best_task_slot(result.analyzed_tasks, frame, shape),
            shape);
        const auto metal_recost_bits =
            recost_selected_subframe_bits(frame_samples, info, selected);

        update_summary(summary, global_first_frame + frame, scalar, metal, metal_recost_bits);
        if (should_print_mismatch(scalar, metal, metal_recost_bits) &&
            summary.printed < options.mismatch_limit) {
            print_row(
                global_first_frame + frame,
                options.frame_samples,
                scalar,
                metal,
                options.show_coefficients,
                metal_recost_bits);
            ++summary.printed;
        }
    }
}

int run(const Options& options)
{
    const auto device_index = first_available_metal_device_index(options.device_index);
    if (!device_index.has_value()) {
        throw std::runtime_error("no available Metal device was found");
    }
    const auto devices = ldcompress::list_metal_devices();
    const auto device_name = *device_index < devices.size()
        ? devices[*device_index].device_name
        : std::string {"unknown"};
    std::cerr << "using Metal device " << *device_index << ": " << device_name << '\n';

    auto all_samples = read_lds_samples(options.input);
    const auto full_frame_count = all_samples.size() / options.frame_samples;
    if (options.start_frame >= full_frame_count) {
        throw std::runtime_error("start frame is beyond full-frame sample range");
    }

    const auto available_frames = full_frame_count - options.start_frame;
    const auto frame_count = std::min(
        available_frames,
        options.max_frames.value_or(available_frames));
    const auto sample_begin = options.start_frame * static_cast<std::size_t>(options.frame_samples);
    const auto sample_end = sample_begin +
        (frame_count * static_cast<std::size_t>(options.frame_samples));
    std::vector<std::int32_t> samples(
        all_samples.begin() + static_cast<std::ptrdiff_t>(sample_begin),
        all_samples.begin() + static_cast<std::ptrdiff_t>(sample_end));

    const auto tail_samples = all_samples.size() - (full_frame_count * options.frame_samples);
    std::cerr << "loaded " << all_samples.size() << " samples, comparing "
              << frame_count << " full frames starting at " << options.start_frame;
    if (tail_samples != 0) {
        std::cerr << " (" << tail_samples << " tail samples omitted)";
    }
    std::cerr << '\n';

    const auto shape = task_shape(options);
    ldcompress::metal_detail::MetalMonoAnalysisSession session(device_index);
    Summary summary;

    print_header(options.show_coefficients);
    for (std::size_t batch_frame = 0; batch_frame < frame_count;
         batch_frame += kMetalBatchFrames) {
        const auto batch_count = std::min(kMetalBatchFrames, frame_count - batch_frame);
        const auto batch_sample_begin = batch_frame * static_cast<std::size_t>(options.frame_samples);
        const auto batch_sample_end = batch_sample_begin +
            (batch_count * static_cast<std::size_t>(options.frame_samples));
        const std::vector<std::int32_t> batch_samples(
            samples.begin() + static_cast<std::ptrdiff_t>(batch_sample_begin),
            samples.begin() + static_cast<std::ptrdiff_t>(batch_sample_end));
        analyze_batch(
            options,
            session,
            shape,
            batch_samples,
            options.start_frame + batch_frame,
            summary);
    }

    const auto bit_delta = static_cast<std::int64_t>(summary.metal_bits) -
        static_cast<std::int64_t>(summary.scalar_bits);
    const auto recost_delta = static_cast<std::int64_t>(summary.metal_recost_bits) -
        static_cast<std::int64_t>(summary.metal_bits);
    std::cerr << "summary frames=" << summary.frames
              << " scalar_bits=" << summary.scalar_bits
              << " metal_bits=" << summary.metal_bits
              << " delta_bits=" << bit_delta
              << " metal_recost_bits=" << summary.metal_recost_bits
              << " metal_recost_delta_bits=" << recost_delta
              << " shape_mismatches=" << summary.shape_mismatches
              << " bit_mismatches=" << summary.bit_mismatches
              << " metal_recost_mismatches=" << summary.recost_mismatches
              << " coefficient_mismatches=" << summary.coefficient_mismatches
              << " metal_larger=" << summary.metal_larger
              << " metal_smaller=" << summary.metal_smaller;
    if (summary.first_metal_larger.has_value()) {
        std::cerr << " first_metal_larger=" << *summary.first_metal_larger;
    }
    std::cerr << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& ex) {
        std::cerr << "compare_metal_scalar_frames: " << ex.what() << '\n';
        return 1;
    }
}
