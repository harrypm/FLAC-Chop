#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <vector>

#include "native_analysis_profile.h"

namespace ldcompress {

struct FlacStreamInfo {
    unsigned min_block_size = 0;
    unsigned max_block_size = 0;
    unsigned min_frame_size = 0;
    unsigned max_frame_size = 0;
    unsigned sample_rate = 0;
    unsigned channels = 1;
    unsigned bits_per_sample = 16;
    std::uint64_t total_samples = 0;
    std::array<std::uint8_t, 16> md5 {};
};

// FLAC STREAMINFO has a 36-bit total-samples field. Zero means unknown and is
// required when the actual sample count does not fit in that field.
std::uint64_t flac_streaminfo_total_samples_or_unknown(
    std::uint64_t actual_total_samples) noexcept;

struct FlacFrameInfo {
    std::uint64_t frame_number = 0;
    unsigned sample_rate = 0;
    unsigned bits_per_sample = 16;
    unsigned max_lpc_order = 12;
    unsigned lpc_coefficient_precision = 12;
    unsigned max_rice_partition_order = 5;
};

enum class FlacSubframeKind {
    Constant,
    Verbatim,
    FixedRice,
    LpcRice,
};

enum class FlacLpcWindowKind {
    Rectangular,
    Tukey,
    Welch,
};

enum class FlacLpcQuantizationKind {
    Independent,
    ErrorFeedback,
};

struct FlacSubframeDecision {
    FlacSubframeKind kind = FlacSubframeKind::Verbatim;
    unsigned fixed_order = 0;
    unsigned lpc_order = 0;
    unsigned rice_partition_order = 0;
    unsigned wasted_bits = 0;
    std::uint64_t estimated_bits = 0;
};

struct FlacLpcSubframeAnalysis {
    unsigned order = 0;
    unsigned rice_partition_order = 0;
    unsigned wasted_bits = 0;
    unsigned coefficient_precision = 0;
    int quantization_shift = 0;
    FlacLpcWindowKind window = FlacLpcWindowKind::Rectangular;
    FlacLpcQuantizationKind quantization = FlacLpcQuantizationKind::Independent;
    std::vector<std::int32_t> coefficients;
    std::uint64_t estimated_bits = 0;
};

struct FlacSelectedSubframe {
    FlacSubframeKind kind = FlacSubframeKind::Verbatim;
    unsigned fixed_order = 0;
    unsigned lpc_order = 0;
    unsigned rice_partition_order = 0;
    unsigned wasted_bits = 0;
    unsigned coefficient_precision = 0;
    int quantization_shift = 0;
    std::vector<std::int32_t> coefficients;
    std::vector<unsigned> rice_parameters;
};

struct FlacSelectedFrameWriteTimings {
    std::uint64_t validation_wasted_ns = 0;
    std::uint64_t shift_ns = 0;
    std::uint64_t residual_ns = 0;
    std::uint64_t rice_parameter_ns = 0;
    std::uint64_t bitstream_ns = 0;
    std::uint64_t frame_output_ns = 0;
};

void write_native_flac_streaminfo(std::ostream& output, const FlacStreamInfo& info);

FlacSubframeDecision analyze_mono_best_frame(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

std::optional<FlacLpcSubframeAnalysis> analyze_mono_lpc_frame(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

std::optional<FlacLpcSubframeAnalysis> analyze_mono_lpc_order(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info,
    unsigned lpc_order);

std::vector<FlacLpcSubframeAnalysis> analyze_mono_lpc_order_candidates(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info,
    unsigned lpc_order);

FlacSubframeDecision write_mono_verbatim_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

FlacSubframeDecision write_mono_constant_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

FlacSubframeDecision write_mono_fixed_rice_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

FlacSubframeDecision write_mono_selected_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected);

FlacSubframeDecision write_mono_selected_frame(
    std::ostream& output,
    std::span<const std::int32_t> samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected);

FlacSubframeDecision write_mono_selected_frame_with_decision(
    std::ostream& output,
    std::span<const std::int32_t> samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected,
    const FlacSubframeDecision& decision);

FlacSubframeDecision write_mono_selected_frame_with_decision(
    std::ostream& output,
    std::span<const std::int32_t> samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected,
    const FlacSubframeDecision& decision,
    FlacSelectedFrameWriteTimings* timings);

FlacSubframeDecision write_mono_best_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info);

FlacSubframeDecision write_mono_best_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info,
    NativeAnalysisProfile profile);

}  // namespace ldcompress
