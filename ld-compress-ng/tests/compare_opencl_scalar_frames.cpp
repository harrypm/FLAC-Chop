#include "flac_native_writer.h"
#include "lds_codec.h"
#include "opencl_analysis.h"
#include "opencl_analysis_test_support.h"
#include "opencl_devices.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/*
 * Portions of the OpenCL-style LPC trace logic in this diagnostic are derived
 * from CUETools.FLACCL: FLAC audio encoder using OpenCL
 * Copyright (c) 2010-2022 Gregory S. Chudov
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * Local modifications for ld-compress-ng, 2026-07-05:
 * - Reimplemented the generated-LPC autocorrelation, Levinson-Durbin, and
 *   coefficient-quantization flow as host-side trace helpers.
 * - Emits scalar/OpenCL checkpoint CSV rows for parity diagnostics only.
 */

namespace {

constexpr std::size_t kOpenClBatchFrames = 32;

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
    bool dump_candidates = false;
    bool dump_tukey_trace = false;
    std::optional<std::size_t> candidate_frame;
    std::optional<unsigned> candidate_order;
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
    std::optional<std::size_t> opencl_task_slot;
    std::uint64_t estimated_bits = 0;
    std::vector<std::int32_t> coefficients;
};

struct OpenClLpcTaskShape {
    std::size_t tasks_per_frame = 0;
    std::size_t lpc_tasks_per_window = 0;
    std::size_t total_lpc_tasks = 0;
};

struct MaxDelta {
    std::size_t index = 0;
    double scalar = 0.0;
    double opencl = 0.0;
    double delta = 0.0;
};

struct ScalarQuantizationTrace {
    int quantization_shift = 0;
    std::vector<std::int32_t> independent;
    std::vector<std::int32_t> error_feedback;
};

struct OpenClQuantizationTrace {
    int quantization_shift = 0;
    unsigned coefficient_bits = 0;
    std::vector<std::int32_t> coefficients;
};

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr unsigned kTraceOpenClMaxOrder = 32;

void usage()
{
    std::cerr
        << "usage: compare_opencl_scalar_frames [options] INPUT.lds\n"
        << "options:\n"
        << "  --device INDEX\n"
        << "  --frame-samples N\n"
        << "  --lpc-order N\n"
        << "  --lpc-precision N\n"
        << "  --rice-partition-order N\n"
        << "  --start-frame N\n"
        << "  --max-frames N\n"
        << "  --mismatch-limit N\n"
        << "  --show-coefficients\n"
        << "  --dump-candidates\n"
        << "  --dump-tukey-trace\n"
        << "  --candidate-frame N\n"
        << "  --candidate-order N\n";
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
        if (value > ((~std::uint64_t {0}) - digit) / 10U) {
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

        if (arg == "--device" || arg == "--opencl-device") {
            options.device_index = static_cast<std::size_t>(
                parse_u64(require_value(arg), "OpenCL device index"));
        } else if (arg == "--frame-samples") {
            options.frame_samples = parse_bounded_unsigned(
                require_value(arg), "frame sample count", 16, 4608);
        } else if (arg == "--lpc-order") {
            options.max_lpc_order = parse_bounded_unsigned(
                require_value(arg), "LPC order", 0, 12);
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
        } else if (arg == "--dump-candidates") {
            options.dump_candidates = true;
        } else if (arg == "--dump-tukey-trace") {
            options.dump_tukey_trace = true;
        } else if (arg == "--candidate-frame") {
            options.candidate_frame = static_cast<std::size_t>(
                parse_u64(require_value(arg), "candidate frame"));
        } else if (arg == "--candidate-order") {
            options.candidate_order = parse_bounded_unsigned(
                require_value(arg), "candidate LPC order", 1, 12);
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
    if (options.frame_samples == 0) {
        throw std::runtime_error("frame sample count must be positive");
    }
    if (options.max_frames.has_value() && *options.max_frames == 0) {
        throw std::runtime_error("max frames must be positive");
    }
    if (options.max_lpc_order == 0) {
        throw std::runtime_error("this diagnostic currently targets the generated-LPC OpenCL path");
    }
    if (options.candidate_order.has_value() && *options.candidate_order > options.max_lpc_order) {
        throw std::runtime_error("candidate LPC order cannot exceed --lpc-order");
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

unsigned diagnostic_sample_trailing_zero_bits(std::int32_t sample, unsigned bits_per_sample)
{
    if (sample == 0) {
        return bits_per_sample;
    }
    const auto mask = bits_per_sample == 64
        ? std::numeric_limits<std::uint64_t>::max()
        : ((std::uint64_t {1} << bits_per_sample) - 1U);
    auto raw = static_cast<std::uint64_t>(sample) & mask;

    unsigned zeros = 0;
    while ((raw & 1U) == 0U && zeros < bits_per_sample) {
        raw >>= 1U;
        ++zeros;
    }
    return zeros;
}

unsigned diagnostic_common_wasted_bits(
    const std::vector<std::int32_t>& samples,
    unsigned bits_per_sample)
{
    unsigned wasted_bits = bits_per_sample;
    for (const auto sample : samples) {
        wasted_bits = std::min(
            wasted_bits,
            diagnostic_sample_trailing_zero_bits(sample, bits_per_sample));
        if (wasted_bits == 0) {
            return 0;
        }
    }
    return std::min(wasted_bits, bits_per_sample - 1U);
}

std::vector<std::int32_t> diagnostic_shift_samples(
    const std::vector<std::int32_t>& samples,
    unsigned wasted_bits)
{
    std::vector<std::int32_t> shifted;
    shifted.reserve(samples.size());
    if (wasted_bits == 0) {
        return samples;
    }
    const auto divisor = std::int64_t {1} << wasted_bits;
    for (const auto sample : samples) {
        shifted.push_back(static_cast<std::int32_t>(
            static_cast<std::int64_t>(sample) / divisor));
    }
    return shifted;
}

std::vector<double> scalar_tukey_weights(std::size_t block_size)
{
    std::vector<double> weights(block_size, 1.0);
    if (block_size <= 1) {
        return weights;
    }

    const auto edge_width = static_cast<std::size_t>(
        0.25 * static_cast<double>(block_size));
    if (edge_width == 0) {
        return weights;
    }
    const auto np = edge_width - 1U;
    if (np == 0) {
        weights.front() = 0.0;
        weights.back() = 0.0;
        return weights;
    }

    for (std::size_t n = 0; n <= np; ++n) {
        const auto left_weight =
            0.5 - (0.5 * std::cos(kPi * static_cast<double>(n) / static_cast<double>(np)));
        const auto right_weight =
            0.5 - (0.5 * std::cos(kPi * static_cast<double>(n + np) / static_cast<double>(np)));
        weights[n] = left_weight;
        weights[block_size - np - 1U + n] = right_weight;
    }
    return weights;
}

std::vector<float> opencl_tukey_weights(std::size_t block_size)
{
    const auto scalar_weights = scalar_tukey_weights(block_size);
    std::vector<float> weights;
    weights.reserve(scalar_weights.size());
    for (const auto weight : scalar_weights) {
        weights.push_back(static_cast<float>(weight));
    }
    return weights;
}

std::vector<double> scalar_windowed_samples(
    const std::vector<std::int32_t>& shifted_samples,
    const std::vector<double>& weights)
{
    std::vector<double> windowed;
    windowed.reserve(shifted_samples.size());
    for (std::size_t i = 0; i < shifted_samples.size(); ++i) {
        windowed.push_back(static_cast<double>(shifted_samples[i]) * weights[i]);
    }
    return windowed;
}

std::vector<float> opencl_windowed_samples(
    const std::vector<std::int32_t>& shifted_samples,
    const std::vector<float>& weights)
{
    std::vector<float> windowed;
    windowed.reserve(shifted_samples.size());
    for (std::size_t i = 0; i < shifted_samples.size(); ++i) {
        windowed.push_back(static_cast<float>(shifted_samples[i]) * weights[i]);
    }
    return windowed;
}

std::vector<double> scalar_autocorrelation(
    const std::vector<double>& windowed_samples,
    unsigned order)
{
    std::vector<double> autoc(order + 1U, 0.0);
    for (unsigned lag = 0; lag <= order; ++lag) {
        double sum = 0.0;
        for (std::size_t i = lag; i < windowed_samples.size(); ++i) {
            sum += windowed_samples[i] * windowed_samples[i - lag];
        }
        autoc[lag] = sum;
    }
    return autoc;
}

std::array<float, kTraceOpenClMaxOrder + 1U> opencl_style_autocorrelation(
    const std::vector<float>& windowed_samples)
{
    std::array<float, kTraceOpenClMaxOrder + 1U> autoc {};
    for (unsigned lag = 0; lag <= kTraceOpenClMaxOrder; ++lag) {
        float sum = 0.0f;
        for (std::size_t i = lag; i < windowed_samples.size(); ++i) {
            sum += windowed_samples[i] * windowed_samples[i - lag];
        }
        autoc[lag] = sum;
    }
    return autoc;
}

std::vector<double> scalar_levinson_durbin(
    const std::vector<double>& autoc,
    unsigned order)
{
    std::vector<double> coefficients(order, 0.0);
    if (order == 0 || autoc.empty() || autoc[0] <= 0.0) {
        return {};
    }

    double error = autoc[0];
    for (unsigned i = 0; i < order; ++i) {
        double reflection = autoc[i + 1U];
        for (unsigned j = 0; j < i; ++j) {
            reflection -= coefficients[j] * autoc[i - j];
        }
        reflection /= error;
        if (!std::isfinite(reflection)) {
            return {};
        }

        auto next = coefficients;
        for (unsigned j = 0; j < i; ++j) {
            next[j] = coefficients[j] - (reflection * coefficients[i - j - 1U]);
        }
        next[i] = reflection;
        coefficients = std::move(next);

        error *= 1.0 - (reflection * reflection);
        if (!std::isfinite(error) || error <= 1.0e-9) {
            break;
        }
    }
    return coefficients;
}

std::array<std::array<float, kTraceOpenClMaxOrder>, kTraceOpenClMaxOrder + 1U>
opencl_style_lpc_coefficients(
    const std::array<float, kTraceOpenClMaxOrder + 1U>& autoc)
{
    std::array<std::array<float, kTraceOpenClMaxOrder>, kTraceOpenClMaxOrder + 1U> lpcs {};
    std::array<float, kTraceOpenClMaxOrder> ldr {};
    std::array<float, kTraceOpenClMaxOrder> gen0 {};
    std::array<float, kTraceOpenClMaxOrder> gen1 {};
    std::array<float, kTraceOpenClMaxOrder> err {};

    for (std::size_t i = 0; i < kTraceOpenClMaxOrder; ++i) {
        gen0[i] = autoc[i + 1U];
        gen1[i] = autoc[i + 1U];
    }

    float error = autoc[0];
    for (std::size_t order = 0; order < kTraceOpenClMaxOrder; ++order) {
        float reflection = 0.0f;
        if (error > 0.0f) {
            reflection = -gen1[0] / error;
        }
        if (!std::isfinite(reflection)) {
            reflection = 0.0f;
        }

        error *= 1.0f - (reflection * reflection);
        if (!std::isfinite(error) || error < 0.0f) {
            error = 0.0f;
        }

        for (std::size_t j = 0; j < kTraceOpenClMaxOrder - 1U - order; ++j) {
            const float next_gen1 = gen1[j + 1U] + (reflection * gen0[j]);
            gen0[j] = (gen1[j + 1U] * reflection) + gen0[j];
            gen1[j] = next_gen1;
        }

        err[order] = error;
        ldr[order] = reflection;
        for (std::size_t j = 0; j < order / 2U; ++j) {
            const float tmp = ldr[j];
            ldr[j] += reflection * ldr[order - 1U - j];
            ldr[order - 1U - j] += reflection * tmp;
        }
        if ((order & 1U) != 0) {
            ldr[order / 2U] += ldr[order / 2U] * reflection;
        }

        for (std::size_t j = 0; j <= order; ++j) {
            lpcs[order][j] = -ldr[order - j];
        }
    }

    for (std::size_t j = 0; j < kTraceOpenClMaxOrder; ++j) {
        lpcs[kTraceOpenClMaxOrder][j] = err[j];
    }
    return lpcs;
}

int count_leading_zero_bits(std::int32_t value)
{
    const auto bits = static_cast<std::uint32_t>(value);
    int count = 0;
    for (int bit = 31; bit >= 0; --bit) {
        if ((bits & (std::uint32_t {1} << bit)) != 0) {
            break;
        }
        ++count;
    }
    return count;
}

std::int32_t opencl_abs_bits(std::int32_t value)
{
    return value ^ (value >> 31);
}

std::int32_t round_to_even_i32(float value)
{
    if (!std::isfinite(value)) {
        return 0;
    }
    const double rounded = std::nearbyint(static_cast<double>(value));
    if (rounded < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        rounded > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error("OpenCL-style coefficient conversion overflowed int32");
    }
    return static_cast<std::int32_t>(rounded);
}

ScalarQuantizationTrace scalar_quantization_trace(
    const std::vector<double>& coefficients,
    unsigned coefficient_precision)
{
    double max_abs = 0.0;
    for (const auto coefficient : coefficients) {
        max_abs = std::max(max_abs, std::abs(coefficient));
    }
    if (max_abs == 0.0) {
        return {};
    }

    const auto min_quantized = -(std::int64_t {1} << (coefficient_precision - 1U));
    const auto max_quantized = (std::int64_t {1} << (coefficient_precision - 1U)) - 1;
    int exponent = 0;
    std::frexp(max_abs, &exponent);
    const int log2_max_abs = exponent - 1;
    const int coefficient_magnitude_bits = static_cast<int>(coefficient_precision) - 1;
    const int raw_shift = coefficient_magnitude_bits - log2_max_abs - 1;
    const int quantization_shift = std::clamp(raw_shift, 0, 15);

    ScalarQuantizationTrace trace;
    trace.quantization_shift = quantization_shift;
    trace.independent.reserve(coefficients.size());
    for (const auto coefficient : coefficients) {
        auto value = static_cast<std::int64_t>(
            std::llround(std::ldexp(coefficient, quantization_shift)));
        value = std::clamp(value, min_quantized, max_quantized);
        trace.independent.push_back(static_cast<std::int32_t>(value));
    }

    trace.error_feedback.reserve(coefficients.size());
    double error = 0.0;
    for (const auto coefficient : coefficients) {
        const auto scaled = raw_shift < 0
            ? std::ldexp(coefficient, raw_shift)
            : std::ldexp(coefficient, quantization_shift);
        error += scaled;
        auto value = static_cast<std::int64_t>(std::llround(error));
        value = std::clamp(value, min_quantized, max_quantized);
        error -= static_cast<double>(value);
        trace.error_feedback.push_back(static_cast<std::int32_t>(value));
    }
    return trace;
}

OpenClQuantizationTrace opencl_quantization_trace(
    const std::vector<float>& raw_order_coefficients,
    unsigned coefficient_precision)
{
    const int desired_bits = std::clamp(static_cast<int>(coefficient_precision), 1, 15);
    std::int32_t max_coefficient = 0;
    for (const auto coefficient : raw_order_coefficients) {
        const auto quantized = round_to_even_i32(
            coefficient * static_cast<float>(1 << 15));
        max_coefficient |= opencl_abs_bits(quantized);
    }

    int quantization_shift = 0;
    if (max_coefficient != 0) {
        quantization_shift = std::clamp(
            count_leading_zero_bits(max_coefficient) - 18 + desired_bits,
            0,
            15);
    }
    const auto limit = (1 << (desired_bits - 1)) - 1;

    std::vector<std::int32_t> raw_coefficients;
    raw_coefficients.reserve(raw_order_coefficients.size());
    std::int32_t actual_max_coefficient = 0;
    for (const auto coefficient : raw_order_coefficients) {
        const auto quantized = std::clamp(
            round_to_even_i32(coefficient * static_cast<float>(1 << quantization_shift)),
            -limit,
            limit);
        actual_max_coefficient |= opencl_abs_bits(quantized);
        raw_coefficients.push_back(quantized);
    }

    OpenClQuantizationTrace trace;
    trace.quantization_shift = quantization_shift;
    trace.coefficient_bits = actual_max_coefficient == 0
        ? 1U
        : static_cast<unsigned>(1 + 32 - count_leading_zero_bits(actual_max_coefficient));
    trace.coefficients.reserve(raw_coefficients.size());
    for (auto i = raw_coefficients.rbegin(); i != raw_coefficients.rend(); ++i) {
        trace.coefficients.push_back(*i);
    }
    return trace;
}

MaxDelta max_delta(
    const std::vector<double>& scalar,
    const std::vector<float>& opencl)
{
    const auto count = std::min(scalar.size(), opencl.size());
    MaxDelta result;
    for (std::size_t i = 0; i < count; ++i) {
        const auto opencl_value = static_cast<double>(opencl[i]);
        const auto delta = std::abs(scalar[i] - opencl_value);
        if (i == 0 || delta > result.delta) {
            result.index = i;
            result.scalar = scalar[i];
            result.opencl = opencl_value;
            result.delta = delta;
        }
    }
    return result;
}

void print_trace_max_delta(
    std::size_t frame,
    unsigned order,
    std::string_view checkpoint,
    const MaxDelta& delta)
{
    std::cout << "tukey_trace_max_delta"
              << ',' << frame
              << ',' << order
              << ',' << checkpoint
              << ',' << delta.index
              << ',' << delta.scalar
              << ',' << delta.opencl
              << ',' << delta.delta
              << '\n';
}

void dump_tukey_trace_rows(
    std::size_t global_frame,
    std::size_t local_frame,
    unsigned candidate_order,
    const std::vector<std::int32_t>& frame_samples,
    const ldcompress::FlacFrameInfo& info,
    const OpenClLpcTaskShape& opencl_shape,
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& opencl_analyzed_tasks)
{
    const auto old_precision = std::cout.precision();
    std::cout << std::setprecision(17);

    const auto wasted_bits =
        diagnostic_common_wasted_bits(frame_samples, info.bits_per_sample);
    const auto shifted_samples = diagnostic_shift_samples(frame_samples, wasted_bits);

    std::vector<double> shifted_double;
    std::vector<float> shifted_float;
    shifted_double.reserve(shifted_samples.size());
    shifted_float.reserve(shifted_samples.size());
    for (const auto sample : shifted_samples) {
        shifted_double.push_back(static_cast<double>(sample));
        shifted_float.push_back(static_cast<float>(sample));
    }

    const auto scalar_weights = scalar_tukey_weights(shifted_samples.size());
    const auto opencl_weights = opencl_tukey_weights(shifted_samples.size());
    const auto scalar_windowed = scalar_windowed_samples(shifted_samples, scalar_weights);
    const auto opencl_windowed = opencl_windowed_samples(shifted_samples, opencl_weights);

    const auto scalar_autoc = scalar_autocorrelation(scalar_windowed, candidate_order);
    const auto opencl_autoc_array = opencl_style_autocorrelation(opencl_windowed);
    std::vector<float> opencl_autoc;
    opencl_autoc.reserve(candidate_order + 1U);
    for (unsigned lag = 0; lag <= candidate_order; ++lag) {
        opencl_autoc.push_back(opencl_autoc_array[lag]);
    }

    const auto scalar_lpc = scalar_levinson_durbin(scalar_autoc, candidate_order);
    if (scalar_lpc.size() != candidate_order) {
        throw std::runtime_error("scalar Tukey trace did not produce the requested LPC order");
    }
    const auto opencl_lpc_rows = opencl_style_lpc_coefficients(opencl_autoc_array);
    std::vector<float> opencl_lpc_raw_order;
    std::vector<float> opencl_lpc_native_order;
    opencl_lpc_raw_order.reserve(candidate_order);
    opencl_lpc_native_order.reserve(candidate_order);
    for (unsigned i = 0; i < candidate_order; ++i) {
        opencl_lpc_raw_order.push_back(opencl_lpc_rows[candidate_order - 1U][i]);
    }
    for (auto i = opencl_lpc_raw_order.rbegin(); i != opencl_lpc_raw_order.rend(); ++i) {
        opencl_lpc_native_order.push_back(*i);
    }

    const auto scalar_quantized =
        scalar_quantization_trace(scalar_lpc, info.lpc_coefficient_precision);
    const auto opencl_quantized =
        opencl_quantization_trace(opencl_lpc_raw_order, info.lpc_coefficient_precision);

    std::optional<ldcompress::FlacSelectedSubframe> opencl_tukey_selected;
    const auto frame_base = local_frame * opencl_shape.tasks_per_frame;
    const auto tukey_slot = opencl_shape.lpc_tasks_per_window + candidate_order - 1U;
    if (frame_base + tukey_slot < opencl_analyzed_tasks.size() &&
        tukey_slot < opencl_shape.total_lpc_tasks) {
        const auto& task = opencl_analyzed_tasks[frame_base + tukey_slot];
        if (task.data.type == ldcompress::opencl_detail::kFlacClSubframeLpc &&
            task.data.residualOrder == static_cast<std::int32_t>(candidate_order)) {
            opencl_tukey_selected =
                ldcompress::opencl_detail::flaccl_task_to_selected_subframe(task);
        }
    }
    const auto opencl_quantization_shift = opencl_tukey_selected.has_value()
        ? opencl_tukey_selected->quantization_shift
        : opencl_quantized.quantization_shift;
    const auto opencl_coefficient_bits = opencl_tukey_selected.has_value()
        ? opencl_tukey_selected->coefficient_precision
        : opencl_quantized.coefficient_bits;

    std::cout << "tukey_trace_summary,frame,order,wasted_bits,"
              << "scalar_quantization_shift,opencl_quantization_shift,opencl_cbits\n";
    std::cout << "tukey_trace_summary"
              << ',' << global_frame
              << ',' << candidate_order
              << ',' << wasted_bits
              << ',' << scalar_quantized.quantization_shift
              << ',' << opencl_quantization_shift
              << ',' << opencl_coefficient_bits
              << '\n';

    std::cout << "tukey_trace_max_delta,frame,order,checkpoint,index,"
              << "scalar_value,opencl_style_value,abs_delta\n";
    print_trace_max_delta(
        global_frame, candidate_order, "shifted_sample", max_delta(shifted_double, shifted_float));
    print_trace_max_delta(
        global_frame, candidate_order, "tukey_weight", max_delta(scalar_weights, opencl_weights));
    print_trace_max_delta(
        global_frame, candidate_order, "windowed_sample", max_delta(scalar_windowed, opencl_windowed));

    std::cout << "tukey_trace_autocorrelation,frame,order,lag,"
              << "scalar_value,opencl_style_value,abs_delta\n";
    for (unsigned lag = 0; lag <= candidate_order; ++lag) {
        const auto opencl_value = static_cast<double>(opencl_autoc[lag]);
        std::cout << "tukey_trace_autocorrelation"
                  << ',' << global_frame
                  << ',' << candidate_order
                  << ',' << lag
                  << ',' << scalar_autoc[lag]
                  << ',' << opencl_value
                  << ',' << std::abs(scalar_autoc[lag] - opencl_value)
                  << '\n';
    }

    std::cout << "tukey_trace_lpc,frame,order,index,"
              << "scalar_native,opencl_style_native,abs_delta\n";
    for (unsigned i = 0; i < candidate_order; ++i) {
        const auto opencl_value = static_cast<double>(opencl_lpc_native_order[i]);
        std::cout << "tukey_trace_lpc"
                  << ',' << global_frame
                  << ',' << candidate_order
                  << ',' << i
                  << ',' << scalar_lpc[i]
                  << ',' << opencl_value
                  << ',' << std::abs(scalar_lpc[i] - opencl_value)
                  << '\n';
    }

    std::cout << "tukey_trace_quantized,frame,order,index,"
              << "scalar_independent,scalar_error_feedback,opencl_round_to_even,"
              << "independent_delta,error_feedback_delta\n";
    for (unsigned i = 0; i < candidate_order; ++i) {
        const auto opencl_coefficient =
            opencl_tukey_selected.has_value() && i < opencl_tukey_selected->coefficients.size()
            ? opencl_tukey_selected->coefficients[i]
            : opencl_quantized.coefficients[i];
        std::cout << "tukey_trace_quantized"
                  << ',' << global_frame
                  << ',' << candidate_order
                  << ',' << i
                  << ',' << scalar_quantized.independent[i]
                  << ',' << scalar_quantized.error_feedback[i]
                  << ',' << opencl_coefficient
                  << ',' << (opencl_coefficient - scalar_quantized.independent[i])
                  << ',' << (opencl_coefficient - scalar_quantized.error_feedback[i])
                  << '\n';
    }

    std::cout << std::setprecision(old_precision);
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

std::string optional_slot_text(std::optional<std::size_t> slot)
{
    return slot.has_value() ? std::to_string(*slot) : std::string {};
}

ldcompress::opencl_detail::OpenClMonoAnalysisTaskOptions make_opencl_task_options(
    const Options& options)
{
    ldcompress::opencl_detail::OpenClMonoAnalysisTaskOptions task_options;
    task_options.frame_samples = options.frame_samples;
    task_options.bits_per_sample = 16;
    task_options.max_lpc_order = options.max_lpc_order;
    task_options.min_fixed_order = 0;
    task_options.max_fixed_order = 4;
    task_options.include_constant = true;
    return task_options;
}

OpenClLpcTaskShape opencl_lpc_task_shape(const Options& options)
{
    const auto plan = ldcompress::opencl_detail::build_mono_analysis_task_plan(
        1, make_opencl_task_options(options));
    OpenClLpcTaskShape shape {
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
        throw std::runtime_error("could not derive OpenCL generated-LPC task shape");
    }
    return shape;
}

std::optional<std::size_t> choose_opencl_best_task_slot(
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& analyzed_tasks,
    std::size_t frame,
    const OpenClLpcTaskShape& shape)
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

std::string opencl_lpc_window_name(
    std::optional<std::size_t> slot,
    const OpenClLpcTaskShape& shape)
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
        .opencl_task_slot = std::nullopt,
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

DecisionView make_opencl_view(
    const ldcompress::FlacSubframeDecision& decision,
    const ldcompress::FlacSelectedSubframe& selected,
    std::optional<std::size_t> task_slot,
    const OpenClLpcTaskShape& shape)
{
    return DecisionView {
        .kind = kind_name(decision.kind),
        .order = decision_order(decision),
        .rice_partition_order = decision.rice_partition_order,
        .wasted_bits = decision.wasted_bits,
        .coefficient_precision = selected.coefficient_precision,
        .quantization_shift = selected.quantization_shift,
        .lpc_window = opencl_lpc_window_name(task_slot, shape),
        .lpc_quantization = decision.kind == ldcompress::FlacSubframeKind::LpcRice
            ? "round_to_even"
            : "",
        .opencl_task_slot = task_slot,
        .estimated_bits = decision.estimated_bits,
        .coefficients = selected.coefficients,
    };
}

bool same_decision_shape(const DecisionView& scalar, const DecisionView& opencl)
{
    return scalar.kind == opencl.kind &&
        scalar.order == opencl.order &&
        scalar.rice_partition_order == opencl.rice_partition_order &&
        scalar.wasted_bits == opencl.wasted_bits &&
        scalar.coefficient_precision == opencl.coefficient_precision &&
        scalar.quantization_shift == opencl.quantization_shift;
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
    const std::vector<std::int32_t>& opencl)
{
    const auto common = std::min(scalar.size(), opencl.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (scalar[i] != opencl[i]) {
            return i;
        }
    }
    return common;
}

std::uint64_t max_abs_coefficient_delta(
    const std::vector<std::int32_t>& scalar,
    const std::vector<std::int32_t>& opencl)
{
    const auto common = std::min(scalar.size(), opencl.size());
    std::uint64_t max_delta = 0;
    for (std::size_t i = 0; i < common; ++i) {
        const auto left = static_cast<std::int64_t>(scalar[i]);
        const auto right = static_cast<std::int64_t>(opencl[i]);
        const auto delta = left > right
            ? static_cast<std::uint64_t>(left - right)
            : static_cast<std::uint64_t>(right - left);
        max_delta = std::max(max_delta, delta);
    }
    return max_delta;
}

std::uint64_t recost_selected_subframe_bits(
    const std::vector<std::int32_t>& samples,
    const ldcompress::FlacFrameInfo& info,
    const ldcompress::FlacSelectedSubframe& selected)
{
    std::ostringstream sink(std::ios::binary);
    const auto recost = ldcompress::write_mono_selected_frame(sink, samples, info, selected);
    return recost.estimated_bits;
}

ldcompress::FlacSelectedSubframe selected_from_lpc_analysis(
    const ldcompress::FlacLpcSubframeAnalysis& analysis)
{
    return ldcompress::FlacSelectedSubframe {
        .kind = ldcompress::FlacSubframeKind::LpcRice,
        .fixed_order = 0,
        .lpc_order = analysis.order,
        .rice_partition_order = analysis.rice_partition_order,
        .wasted_bits = analysis.wasted_bits,
        .coefficient_precision = analysis.coefficient_precision,
        .quantization_shift = analysis.quantization_shift,
        .coefficients = analysis.coefficients,
    };
}

void print_candidate_header(bool show_coefficients)
{
    std::cout
        << "candidate_frame,sample_offset,order,window,opencl_slot,"
        << "scalar_quantization,scalar_precision,scalar_shift,scalar_partition,"
        << "scalar_bits,scalar_recost_bits,opencl_config_precision,opencl_cbits,"
        << "opencl_shift,opencl_partition,opencl_bits,opencl_recost_bits,"
        << "delta_bits,coefficients_match,first_coefficient_mismatch,"
        << "max_abs_coefficient_delta";
    if (show_coefficients) {
        std::cout << ",scalar_coefficients,opencl_coefficients";
    }
    std::cout << '\n';
}

std::optional<std::size_t> find_opencl_candidate_slot(
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& opencl_analyzed_tasks,
    std::size_t local_frame,
    unsigned order,
    std::string_view window,
    const OpenClLpcTaskShape& opencl_shape)
{
    const auto frame_base = local_frame * opencl_shape.tasks_per_frame;
    if (frame_base + opencl_shape.tasks_per_frame > opencl_analyzed_tasks.size()) {
        throw std::runtime_error("candidate frame is outside OpenCL analyzed task range");
    }
    for (std::size_t slot = 0; slot < opencl_shape.total_lpc_tasks; ++slot) {
        const auto& task = opencl_analyzed_tasks[frame_base + slot];
        if (task.data.type == ldcompress::opencl_detail::kFlacClSubframeLpc &&
            task.data.residualOrder == static_cast<std::int32_t>(order) &&
            opencl_lpc_window_name(slot, opencl_shape) == window) {
            return slot;
        }
    }
    return std::nullopt;
}

void print_joined_candidate_row(
    const Options& options,
    std::size_t global_frame,
    unsigned frame_samples,
    const std::vector<std::int32_t>& samples,
    const ldcompress::FlacFrameInfo& info,
    const ldcompress::FlacLpcSubframeAnalysis& scalar_candidate,
    std::optional<std::size_t> opencl_slot,
    const ldcompress::opencl_detail::FlacClSubframeTask* opencl_task)
{
    const auto scalar_selected = selected_from_lpc_analysis(scalar_candidate);
    const auto scalar_recost_bits =
        recost_selected_subframe_bits(samples, info, scalar_selected);
    const auto window = lpc_window_name(scalar_candidate.window);

    if (opencl_task == nullptr) {
        std::cout << global_frame
                  << ',' << (global_frame * static_cast<std::size_t>(frame_samples))
                  << ',' << scalar_candidate.order
                  << ',' << window
                  << ','
                  << ',' << lpc_quantization_name(scalar_candidate.quantization)
                  << ',' << scalar_candidate.coefficient_precision
                  << ',' << scalar_candidate.quantization_shift
                  << ',' << scalar_candidate.rice_partition_order
                  << ',' << scalar_candidate.estimated_bits
                  << ',' << scalar_recost_bits
                  << ',' << info.lpc_coefficient_precision;
        for (unsigned i = 0; i < 9; ++i) {
            std::cout << ',';
        }
        if (options.show_coefficients) {
            std::cout << ',' << format_coefficients(scalar_candidate.coefficients) << ',';
        }
        std::cout << '\n';
        return;
    }

    const auto opencl_selected =
        ldcompress::opencl_detail::flaccl_task_to_selected_subframe(*opencl_task);
    const auto opencl_decision =
        ldcompress::opencl_detail::flaccl_task_to_subframe_decision(*opencl_task);
    const auto opencl_recost_bits =
        recost_selected_subframe_bits(samples, info, opencl_selected);
    const auto delta = static_cast<std::int64_t>(opencl_decision.estimated_bits) -
        static_cast<std::int64_t>(scalar_candidate.estimated_bits);
    const auto first_mismatch = first_coefficient_mismatch(
        scalar_candidate.coefficients, opencl_selected.coefficients);

    std::cout << global_frame
              << ',' << (global_frame * static_cast<std::size_t>(frame_samples))
              << ',' << scalar_candidate.order
              << ',' << window
              << ',' << optional_slot_text(opencl_slot)
              << ',' << lpc_quantization_name(scalar_candidate.quantization)
              << ',' << scalar_candidate.coefficient_precision
              << ',' << scalar_candidate.quantization_shift
              << ',' << scalar_candidate.rice_partition_order
              << ',' << scalar_candidate.estimated_bits
              << ',' << scalar_recost_bits
              << ',' << info.lpc_coefficient_precision
              << ',' << opencl_selected.coefficient_precision
              << ',' << opencl_selected.quantization_shift
              << ',' << opencl_decision.rice_partition_order
              << ',' << opencl_decision.estimated_bits
              << ',' << opencl_recost_bits
              << ',' << delta
              << ',' << (scalar_candidate.coefficients == opencl_selected.coefficients ? "yes" : "no")
              << ',' << first_mismatch
              << ',' << max_abs_coefficient_delta(
                  scalar_candidate.coefficients, opencl_selected.coefficients);
    if (options.show_coefficients) {
        std::cout << ',' << format_coefficients(scalar_candidate.coefficients)
                  << ',' << format_coefficients(opencl_selected.coefficients);
    }
    std::cout << '\n';
}

void dump_candidate_rows(
    const Options& options,
    std::size_t global_frame,
    std::size_t local_frame,
    unsigned candidate_order,
    const std::vector<std::int32_t>& frame_samples,
    const ldcompress::FlacFrameInfo& info,
    const OpenClLpcTaskShape& opencl_shape,
    const std::vector<ldcompress::opencl_detail::FlacClSubframeTask>& opencl_analyzed_tasks)
{
    print_candidate_header(options.show_coefficients);

    const auto scalar_candidates =
        ldcompress::analyze_mono_lpc_order_candidates(frame_samples, info, candidate_order);
    for (const auto& candidate : scalar_candidates) {
        const auto opencl_slot = find_opencl_candidate_slot(
            opencl_analyzed_tasks,
            local_frame,
            candidate.order,
            lpc_window_name(candidate.window),
            opencl_shape);
        const auto* opencl_task = opencl_slot.has_value()
            ? &opencl_analyzed_tasks[(local_frame * opencl_shape.tasks_per_frame) + *opencl_slot]
            : nullptr;
        print_joined_candidate_row(
            options,
            global_frame,
            options.frame_samples,
            frame_samples,
            info,
            candidate,
            opencl_slot,
            opencl_task);
    }
}

void print_row(
    std::size_t frame,
    unsigned frame_samples,
    const DecisionView& scalar,
    const DecisionView& opencl,
    bool show_coefficients,
    std::uint64_t opencl_recost_bits)
{
    const auto delta = static_cast<std::int64_t>(opencl.estimated_bits) -
        static_cast<std::int64_t>(scalar.estimated_bits);
    const auto recost_delta = static_cast<std::int64_t>(opencl_recost_bits) -
        static_cast<std::int64_t>(opencl.estimated_bits);
    const auto first_mismatch = first_coefficient_mismatch(
        scalar.coefficients, opencl.coefficients);
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
              << ',' << opencl.kind
              << ',' << opencl.order
              << ',' << opencl.rice_partition_order
              << ',' << opencl.wasted_bits
              << ',' << opencl.coefficient_precision
              << ',' << opencl.quantization_shift
              << ',' << opencl.lpc_window
              << ',' << opencl.lpc_quantization
              << ',' << optional_slot_text(opencl.opencl_task_slot)
              << ',' << opencl.estimated_bits
              << ',' << delta
              << ',' << opencl_recost_bits
              << ',' << recost_delta
              << ',' << (scalar.coefficients == opencl.coefficients ? "yes" : "no")
              << ',' << first_mismatch
              << ',' << max_abs_coefficient_delta(scalar.coefficients, opencl.coefficients);
    if (show_coefficients) {
        std::cout << ',' << format_coefficients(scalar.coefficients)
                  << ',' << format_coefficients(opencl.coefficients);
    }
    std::cout << '\n';
}

int run(const Options& options)
{
    const auto selected_device = ldcompress::select_opencl_device(options.device_index);
    std::cerr << "using OpenCL device " << selected_device.flat_index << ": "
              << selected_device.device_name << '\n';

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
    const auto sample_end = sample_begin + (frame_count * static_cast<std::size_t>(options.frame_samples));
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

    ldcompress::FlacFrameInfo info {
        .frame_number = options.start_frame,
        .sample_rate = 40000,
        .bits_per_sample = 16,
        .max_lpc_order = options.max_lpc_order,
        .lpc_coefficient_precision = options.lpc_precision,
        .max_rice_partition_order = options.max_rice_partition_order,
    };

    std::vector<ldcompress::FlacSubframeDecision> opencl_decisions;
    std::vector<ldcompress::FlacSelectedSubframe> opencl_selected;
    std::vector<ldcompress::opencl_detail::FlacClSubframeTask> opencl_analyzed_tasks;
    const auto opencl_shape = opencl_lpc_task_shape(options);
    opencl_decisions.reserve(frame_count);
    opencl_selected.reserve(frame_count);
    opencl_analyzed_tasks.reserve(frame_count * opencl_shape.tasks_per_frame);
    for (std::size_t batch_frame = 0; batch_frame < frame_count;
         batch_frame += kOpenClBatchFrames) {
        const auto batch_count = std::min(kOpenClBatchFrames, frame_count - batch_frame);
        const auto batch_sample_begin = batch_frame * static_cast<std::size_t>(options.frame_samples);
        const auto batch_sample_end = batch_sample_begin +
            (batch_count * static_cast<std::size_t>(options.frame_samples));
        const std::vector<std::int32_t> batch_samples(
            samples.begin() + static_cast<std::ptrdiff_t>(batch_sample_begin),
            samples.begin() + static_cast<std::ptrdiff_t>(batch_sample_end));

        info.frame_number = options.start_frame + batch_frame;
        auto opencl_result = ldcompress::opencl_detail::analyze_opencl_mono_generated_frames(
            batch_samples, info, options.frame_samples, selected_device.flat_index);
        if (opencl_result.decisions.size() != batch_count ||
            opencl_result.selected_subframes.size() != batch_count ||
            opencl_result.best_tasks.size() != batch_count ||
            opencl_result.analyzed_tasks.size() != batch_count * opencl_shape.tasks_per_frame) {
            throw std::runtime_error("OpenCL result frame count did not match input batch");
        }
        opencl_decisions.insert(
            opencl_decisions.end(),
            opencl_result.decisions.begin(),
            opencl_result.decisions.end());
        opencl_selected.insert(
            opencl_selected.end(),
            opencl_result.selected_subframes.begin(),
            opencl_result.selected_subframes.end());
        opencl_analyzed_tasks.insert(
            opencl_analyzed_tasks.end(),
            opencl_result.analyzed_tasks.begin(),
            opencl_result.analyzed_tasks.end());
    }

    std::cout
        << "frame,sample_offset,sample_count,"
        << "scalar_kind,scalar_order,scalar_partition,scalar_wasted,"
        << "scalar_precision,scalar_shift,scalar_window,scalar_quantization,scalar_bits,"
        << "opencl_kind,opencl_order,opencl_partition,opencl_wasted,"
        << "opencl_precision,opencl_shift,opencl_window,opencl_quantization,"
        << "opencl_task_slot,opencl_bits,delta_bits,"
        << "opencl_recost_bits,opencl_recost_delta_bits,"
        << "coefficients_match,first_coefficient_mismatch,max_abs_coefficient_delta";
    if (options.show_coefficients) {
        std::cout << ",scalar_coefficients,opencl_coefficients";
    }
    std::cout << '\n';

    std::uint64_t scalar_bits = 0;
    std::uint64_t opencl_bits = 0;
    std::uint64_t opencl_recost_bits_total = 0;
    std::size_t shape_mismatches = 0;
    std::size_t bit_mismatches = 0;
    std::size_t opencl_recost_mismatches = 0;
    std::size_t opencl_larger = 0;
    std::size_t opencl_smaller = 0;
    std::size_t coefficient_mismatches = 0;
    std::size_t printed = 0;
    std::optional<std::size_t> first_opencl_larger;

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto offset = frame * static_cast<std::size_t>(options.frame_samples);
        const std::vector<std::int32_t> frame_samples(
            samples.begin() + static_cast<std::ptrdiff_t>(offset),
            samples.begin() + static_cast<std::ptrdiff_t>(offset + options.frame_samples));
        info.frame_number = options.start_frame + frame;

        const auto scalar_decision = ldcompress::analyze_mono_best_frame(frame_samples, info);
        const auto scalar = make_scalar_view(scalar_decision, frame_samples, info);
        const auto opencl_slot = choose_opencl_best_task_slot(
            opencl_analyzed_tasks, frame, opencl_shape);
        const auto opencl = make_opencl_view(
            opencl_decisions[frame],
            opencl_selected[frame],
            opencl_slot,
            opencl_shape);
        const auto opencl_recost_bits = recost_selected_subframe_bits(
            frame_samples, info, opencl_selected[frame]);

        scalar_bits += scalar.estimated_bits;
        opencl_bits += opencl.estimated_bits;
        opencl_recost_bits_total += opencl_recost_bits;

        const bool shape_differs = !same_decision_shape(scalar, opencl);
        const bool bits_differ = scalar.estimated_bits != opencl.estimated_bits;
        const bool recost_differs = opencl_recost_bits != opencl.estimated_bits;
        if (shape_differs) {
            ++shape_mismatches;
        }
        if (bits_differ) {
            ++bit_mismatches;
        }
        if (recost_differs) {
            ++opencl_recost_mismatches;
        }
        if (scalar.coefficients != opencl.coefficients) {
            ++coefficient_mismatches;
        }
        if (opencl.estimated_bits > scalar.estimated_bits) {
            ++opencl_larger;
            if (!first_opencl_larger.has_value()) {
                first_opencl_larger = options.start_frame + frame;
            }
        } else if (opencl.estimated_bits < scalar.estimated_bits) {
            ++opencl_smaller;
        }

        if ((shape_differs || bits_differ) && printed < options.mismatch_limit) {
            print_row(
                options.start_frame + frame,
                options.frame_samples,
                scalar,
                opencl,
                options.show_coefficients,
                opencl_recost_bits);
            ++printed;
        }
    }

    if (options.dump_candidates) {
        const auto candidate_frame = options.candidate_frame.value_or(options.start_frame);
        if (candidate_frame < options.start_frame ||
            candidate_frame >= options.start_frame + frame_count) {
            throw std::runtime_error("candidate frame is outside compared frame range");
        }
        const auto candidate_order = options.candidate_order.value_or(options.max_lpc_order);
        const auto local_frame = candidate_frame - options.start_frame;
        const auto offset = local_frame * static_cast<std::size_t>(options.frame_samples);
        const std::vector<std::int32_t> frame_samples(
            samples.begin() + static_cast<std::ptrdiff_t>(offset),
            samples.begin() + static_cast<std::ptrdiff_t>(offset + options.frame_samples));
        info.frame_number = candidate_frame;
        dump_candidate_rows(
            options,
            candidate_frame,
            local_frame,
            candidate_order,
            frame_samples,
            info,
            opencl_shape,
            opencl_analyzed_tasks);
    }

    if (options.dump_tukey_trace) {
        const auto candidate_frame = options.candidate_frame.value_or(options.start_frame);
        if (candidate_frame < options.start_frame ||
            candidate_frame >= options.start_frame + frame_count) {
            throw std::runtime_error("candidate frame is outside compared frame range");
        }
        const auto candidate_order = options.candidate_order.value_or(options.max_lpc_order);
        if (candidate_order > kTraceOpenClMaxOrder) {
            throw std::runtime_error("Tukey trace order exceeds OpenCL trace max order");
        }
        const auto local_frame = candidate_frame - options.start_frame;
        const auto offset = local_frame * static_cast<std::size_t>(options.frame_samples);
        const std::vector<std::int32_t> frame_samples(
            samples.begin() + static_cast<std::ptrdiff_t>(offset),
            samples.begin() + static_cast<std::ptrdiff_t>(offset + options.frame_samples));
        info.frame_number = candidate_frame;
        dump_tukey_trace_rows(
            candidate_frame,
            local_frame,
            candidate_order,
            frame_samples,
            info,
            opencl_shape,
            opencl_analyzed_tasks);
    }

    const auto bit_delta = static_cast<std::int64_t>(opencl_bits) -
        static_cast<std::int64_t>(scalar_bits);
    const auto recost_delta = static_cast<std::int64_t>(opencl_recost_bits_total) -
        static_cast<std::int64_t>(opencl_bits);
    std::cerr << "summary frames=" << frame_count
              << " scalar_bits=" << scalar_bits
              << " opencl_bits=" << opencl_bits
              << " delta_bits=" << bit_delta
              << " opencl_recost_bits=" << opencl_recost_bits_total
              << " opencl_recost_delta_bits=" << recost_delta
              << " shape_mismatches=" << shape_mismatches
              << " bit_mismatches=" << bit_mismatches
              << " opencl_recost_mismatches=" << opencl_recost_mismatches
              << " coefficient_mismatches=" << coefficient_mismatches
              << " opencl_larger=" << opencl_larger
              << " opencl_smaller=" << opencl_smaller;
    if (first_opencl_larger.has_value()) {
        std::cerr << " first_opencl_larger=" << *first_opencl_larger;
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
        std::cerr << "compare_opencl_scalar_frames: " << ex.what() << '\n';
        return 1;
    }
}
