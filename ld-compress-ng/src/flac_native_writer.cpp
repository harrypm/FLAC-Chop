#include "flac_native_writer.h"

#include "flac_primitives.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ostream>
#include <span>
#include <stdexcept>
#include <utility>

namespace ldcompress {
namespace {

constexpr unsigned kMaxRicePartitionOrder = 8;
constexpr unsigned kMaxRiceParameter = 14;
constexpr unsigned kMaxLpcOrder = 12;
constexpr std::uint64_t kMaxStreamInfoTotalSamples = 0xfffffffffULL;
constexpr unsigned kWelchLpcCandidateTargetCount = 2;
constexpr unsigned kMinLpcBlockSize = 256;
constexpr double kPi = 3.14159265358979323846264338327950288;
using SampleSpan = std::span<const std::int32_t>;
using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point finish)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}

void add_elapsed_ns(std::uint64_t& counter, Clock::time_point start)
{
    counter += elapsed_ns(start, Clock::now());
}

void write_byte(std::ostream& output, std::uint8_t byte)
{
    output.put(static_cast<char>(byte));
    if (!output) {
        throw std::runtime_error("failed to write native FLAC output");
    }
}

void write_bytes(std::ostream& output, const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write native FLAC output");
    }
}

void write_u16be(std::ostream& output, unsigned value)
{
    write_byte(output, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    write_byte(output, static_cast<std::uint8_t>(value & 0xffU));
}

void write_u24be(std::ostream& output, unsigned value)
{
    write_byte(output, static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    write_byte(output, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    write_byte(output, static_cast<std::uint8_t>(value & 0xffU));
}

void validate_streaminfo(const FlacStreamInfo& info)
{
    if (info.min_block_size == 0 || info.min_block_size > 65535 ||
        info.max_block_size == 0 || info.max_block_size > 65535 ||
        info.min_block_size > info.max_block_size) {
        throw std::runtime_error("invalid FLAC STREAMINFO block size");
    }
    if (info.min_frame_size > 0xffffff || info.max_frame_size > 0xffffff) {
        throw std::runtime_error("invalid FLAC STREAMINFO frame size");
    }
    if (info.sample_rate > 0xfffff) {
        throw std::runtime_error("invalid FLAC STREAMINFO sample rate");
    }
    if (info.channels == 0 || info.channels > 8) {
        throw std::runtime_error("invalid FLAC STREAMINFO channel count");
    }
    if (info.bits_per_sample < 4 || info.bits_per_sample > 32) {
        throw std::runtime_error("invalid FLAC STREAMINFO bits per sample");
    }
}

void write_utf8_uint(std::vector<std::uint8_t>& output, std::uint64_t value)
{
    if (value < 0x80) {
        output.push_back(static_cast<std::uint8_t>(value));
        return;
    }
    if (value < 0x800) {
        output.push_back(static_cast<std::uint8_t>(0xc0U | (value >> 6U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x10000) {
        output.push_back(static_cast<std::uint8_t>(0xe0U | (value >> 12U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x200000) {
        output.push_back(static_cast<std::uint8_t>(0xf0U | (value >> 18U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x4000000) {
        output.push_back(static_cast<std::uint8_t>(0xf8U | (value >> 24U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 18U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x80000000ULL) {
        output.push_back(static_cast<std::uint8_t>(0xfcU | (value >> 30U)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 24U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 18U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    if (value < 0x1000000000ULL) {
        output.push_back(static_cast<std::uint8_t>(0xfeU));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 30U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 24U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 18U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<std::uint8_t>(0x80U | (value & 0x3fU)));
        return;
    }
    throw std::runtime_error("FLAC UTF-8 coded number is too large");
}

std::uint8_t block_size_code(std::size_t block_size)
{
    if (block_size == 0 || block_size > 65536) {
        throw std::runtime_error("invalid FLAC frame block size");
    }
    if (block_size <= 256) {
        return 6;
    }
    return 7;
}

std::uint8_t bits_per_sample_code(unsigned bits_per_sample)
{
    switch (bits_per_sample) {
    case 8:
        return 1;
    case 12:
        return 2;
    case 16:
        return 4;
    case 20:
        return 5;
    case 24:
        return 6;
    case 32:
        return 7;
    default:
        throw std::runtime_error("unsupported FLAC frame bits per sample");
    }
}

void validate_frame_bits_per_sample(unsigned bits_per_sample)
{
    (void)bits_per_sample_code(bits_per_sample);
}

void validate_sample(std::int32_t sample, unsigned bits_per_sample)
{
    const auto min_value = -(std::int64_t {1} << (bits_per_sample - 1U));
    const auto max_value = (std::int64_t {1} << (bits_per_sample - 1U)) - 1;
    if (sample < min_value || sample > max_value) {
        throw std::runtime_error("sample is outside the selected FLAC bit depth");
    }
}

std::int64_t fixed_prediction_residual(
    SampleSpan samples,
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
        throw std::runtime_error("unsupported FLAC fixed predictor order");
    }
}

std::uint64_t fold_signed_residual(std::int64_t residual)
{
    if (residual >= 0) {
        return static_cast<std::uint64_t>(residual) << 1U;
    }
    return (static_cast<std::uint64_t>(-(residual + 1)) << 1U) + 1U;
}

std::vector<std::int64_t> fixed_residuals(
    SampleSpan samples,
    unsigned order)
{
    std::vector<std::int64_t> residuals;
    residuals.reserve(samples.size() - order);
    for (std::size_t i = order; i < samples.size(); ++i) {
        residuals.push_back(fixed_prediction_residual(samples, i, order));
    }
    return residuals;
}

std::uint64_t rice_bits(
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count,
    unsigned parameter)
{
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto residual = residuals.at(offset + i);
        const auto folded = fold_signed_residual(residual);
        bits += (folded >> parameter) + 1U + parameter;
    }
    return bits;
}

unsigned choose_rice_parameter(
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count)
{
    unsigned best_parameter = 0;
    std::array<std::uint64_t, kMaxRiceParameter + 1U> bit_counts {};
    if (offset > residuals.size() || count > residuals.size() - offset) {
        throw std::runtime_error("internal FLAC residual partition range error");
    }

    for (unsigned parameter = 0; parameter <= kMaxRiceParameter; ++parameter) {
        bit_counts[parameter] = static_cast<std::uint64_t>(count) * (1U + parameter);
    }
    const auto end = offset + count;
    for (std::size_t i = offset; i < end; ++i) {
        const auto folded = fold_signed_residual(residuals[i]);
        for (unsigned parameter = 0; parameter <= kMaxRiceParameter; ++parameter) {
            bit_counts[parameter] += folded >> parameter;
        }
    }

    auto best_bits = bit_counts[0];
    for (unsigned parameter = 1; parameter <= kMaxRiceParameter; ++parameter) {
        if (bit_counts[parameter] < best_bits) {
            best_bits = bit_counts[parameter];
            best_parameter = parameter;
        }
    }
    return best_parameter;
}

enum class RiceSelectionMode {
    Exact,
    MeanEstimate,
    MeanEstimateOnly,
};

bool valid_partition_order(std::size_t block_size, unsigned predictor_order, unsigned partition_order);
unsigned checked_max_rice_partition_order(unsigned max_rice_partition_order);
std::uint64_t subframe_wasted_bits_overhead(unsigned wasted_bits);
std::size_t partition_residual_count(
    std::size_t block_size,
    unsigned predictor_order,
    unsigned partition_order,
    std::size_t partition);

struct RicePartitionChoice {
    unsigned partition_order = 0;
    std::vector<unsigned> parameters;
    std::uint64_t estimated_bits = std::numeric_limits<std::uint64_t>::max();
};

unsigned ilog2_u64(std::uint64_t value)
{
    unsigned result = 0;
    while (value > 1) {
        value >>= 1U;
        ++result;
    }
    return result;
}

std::uint64_t abs_residual_sum(
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count)
{
    if (offset > residuals.size() || count > residuals.size() - offset) {
        throw std::runtime_error("internal FLAC residual partition range error");
    }

    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto residual = residuals[offset + i];
        sum += residual < 0
            ? static_cast<std::uint64_t>(-(residual + 1)) + 1U
            : static_cast<std::uint64_t>(residual);
    }
    return sum;
}

unsigned mean_rice_parameter(std::uint64_t abs_sum, std::size_t sample_count)
{
    if (sample_count == 0) {
        return 0;
    }

    const auto divisor = 0x40000ULL / sample_count;
    const auto scaled_mean =
        abs_sum < 2 || divisor == 0 ? 0 : (((abs_sum - 1U) * divisor) >> 18U);
    if (scaled_mean == 0) {
        return 0;
    }
    return std::min(kMaxRiceParameter, ilog2_u64(scaled_mean) + 1U);
}

std::uint64_t mean_rice_partition_bits(
    unsigned rice_parameter,
    std::size_t sample_count,
    std::uint64_t abs_sum)
{
    std::uint64_t bits = 4U +
        (static_cast<std::uint64_t>(1U + rice_parameter) * sample_count);
    bits += rice_parameter == 0
        ? (abs_sum << 1U)
        : (abs_sum >> (rice_parameter - 1U));
    const auto correction = sample_count >> 1U;
    return bits > correction ? bits - correction : 0;
}

RicePartitionChoice choose_mean_rice_partition_order(
    const std::vector<std::int64_t>& residuals,
    std::size_t block_size,
    unsigned predictor_order,
    unsigned max_rice_partition_order)
{
    RicePartitionChoice best;
    const auto max_partition_order =
        checked_max_rice_partition_order(max_rice_partition_order);
    for (int partition_order = static_cast<int>(max_partition_order);
         partition_order >= 0;
         --partition_order) {
        const auto current_partition_order = static_cast<unsigned>(partition_order);
        if (!valid_partition_order(block_size, predictor_order, current_partition_order)) {
            continue;
        }

        RicePartitionChoice current;
        current.partition_order = current_partition_order;
        current.estimated_bits = 2U + 4U;
        const auto partition_count = std::size_t {1} << current_partition_order;
        current.parameters.reserve(partition_count);

        std::size_t residual_offset = 0;
        for (std::size_t partition = 0; partition < partition_count; ++partition) {
            const auto residual_count = partition_residual_count(
                block_size, predictor_order, current_partition_order, partition);
            const auto abs_sum = abs_residual_sum(residuals, residual_offset, residual_count);
            const auto parameter = mean_rice_parameter(abs_sum, residual_count);
            current.parameters.push_back(parameter);
            current.estimated_bits +=
                mean_rice_partition_bits(parameter, residual_count, abs_sum);
            residual_offset += residual_count;
        }
        if (residual_offset != residuals.size()) {
            throw std::runtime_error("internal FLAC residual partition accounting error");
        }

        if (current.estimated_bits < best.estimated_bits) {
            best = std::move(current);
        }
    }
    if (best.estimated_bits == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("internal FLAC mean Rice partition search found no valid partition order");
    }
    return best;
}

std::uint64_t fixed_rice_subframe_mean_estimated_bits(
    unsigned order,
    unsigned wasted_bits,
    std::size_t block_size,
    unsigned bits_per_sample,
    const RicePartitionChoice& rice_choice)
{
    (void)block_size;
    constexpr unsigned kSubframeHeaderBits = 8;
    return kSubframeHeaderBits +
        (static_cast<std::uint64_t>(order) * bits_per_sample) +
        subframe_wasted_bits_overhead(wasted_bits) +
        rice_choice.estimated_bits;
}

std::uint64_t lpc_rice_subframe_mean_estimated_bits(
    unsigned order,
    unsigned wasted_bits,
    unsigned coefficient_precision,
    std::size_t block_size,
    unsigned bits_per_sample,
    const RicePartitionChoice& rice_choice)
{
    (void)block_size;
    constexpr unsigned kSubframeHeaderBits = 8;
    constexpr unsigned kCoefficientPrecisionBits = 4;
    constexpr unsigned kQuantizationShiftBits = 5;
    return kSubframeHeaderBits +
        subframe_wasted_bits_overhead(wasted_bits) +
        (static_cast<std::uint64_t>(order) * bits_per_sample) +
        kCoefficientPrecisionBits +
        kQuantizationShiftBits +
        (static_cast<std::uint64_t>(order) * coefficient_precision) +
        rice_choice.estimated_bits;
}

struct FixedRiceSubframe {
    unsigned order = 0;
    unsigned partition_order = 0;
    unsigned wasted_bits = 0;
    unsigned effective_bits_per_sample = 0;
    std::vector<std::int32_t> shifted_samples;
    std::vector<unsigned> rice_parameters;
    std::vector<std::int64_t> residuals;
    std::uint64_t bits = 0;
};

struct LpcRiceSubframe {
    unsigned order = 0;
    unsigned partition_order = 0;
    unsigned wasted_bits = 0;
    unsigned effective_bits_per_sample = 0;
    unsigned coefficient_precision = 10;
    int quantization_shift = 0;
    FlacLpcWindowKind window = FlacLpcWindowKind::Rectangular;
    FlacLpcQuantizationKind quantization = FlacLpcQuantizationKind::Independent;
    std::vector<std::int32_t> shifted_samples;
    std::vector<std::int32_t> coefficients;
    std::vector<unsigned> rice_parameters;
    std::vector<std::int64_t> residuals;
    std::uint64_t bits = 0;
};

FlacSubframeDecision fixed_rice_decision(const FixedRiceSubframe& subframe)
{
    return FlacSubframeDecision {
        .kind = FlacSubframeKind::FixedRice,
        .fixed_order = subframe.order,
        .lpc_order = 0,
        .rice_partition_order = subframe.partition_order,
        .wasted_bits = subframe.wasted_bits,
        .estimated_bits = subframe.bits,
    };
}

FlacSubframeDecision lpc_rice_decision(const LpcRiceSubframe& subframe)
{
    return FlacSubframeDecision {
        .kind = FlacSubframeKind::LpcRice,
        .fixed_order = 0,
        .lpc_order = subframe.order,
        .rice_partition_order = subframe.partition_order,
        .wasted_bits = subframe.wasted_bits,
        .estimated_bits = subframe.bits,
    };
}

struct QuantizedLpcCoefficients {
    int quantization_shift = 0;
    FlacLpcQuantizationKind quantization = FlacLpcQuantizationKind::Independent;
    std::vector<std::int32_t> coefficients;
};

bool all_samples_equal(SampleSpan samples)
{
    return std::all_of(samples.begin(), samples.end(), [&](std::int32_t sample) {
        return sample == samples.front();
    });
}

std::uint64_t verbatim_subframe_bits(std::size_t block_size, unsigned bits_per_sample)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    return kSubframeHeaderBits + (static_cast<std::uint64_t>(block_size) * bits_per_sample);
}

std::uint64_t constant_subframe_bits(unsigned bits_per_sample)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    return kSubframeHeaderBits + bits_per_sample;
}

std::uint64_t subframe_wasted_bits_overhead(unsigned wasted_bits)
{
    return wasted_bits == 0 ? 0 : wasted_bits;
}

std::uint64_t verbatim_subframe_bits(
    std::size_t block_size,
    unsigned bits_per_sample,
    unsigned wasted_bits)
{
    return verbatim_subframe_bits(block_size, bits_per_sample - wasted_bits) +
        subframe_wasted_bits_overhead(wasted_bits);
}

std::uint64_t constant_subframe_bits(unsigned bits_per_sample, unsigned wasted_bits)
{
    return constant_subframe_bits(bits_per_sample - wasted_bits) +
        subframe_wasted_bits_overhead(wasted_bits);
}

FlacSubframeDecision verbatim_decision(
    std::size_t block_size,
    unsigned bits_per_sample,
    unsigned wasted_bits)
{
    return FlacSubframeDecision {
        .kind = FlacSubframeKind::Verbatim,
        .fixed_order = 0,
        .lpc_order = 0,
        .rice_partition_order = 0,
        .wasted_bits = wasted_bits,
        .estimated_bits = verbatim_subframe_bits(block_size, bits_per_sample, wasted_bits),
    };
}

FlacSubframeDecision constant_decision(unsigned bits_per_sample, unsigned wasted_bits)
{
    return FlacSubframeDecision {
        .kind = FlacSubframeKind::Constant,
        .fixed_order = 0,
        .lpc_order = 0,
        .rice_partition_order = 0,
        .wasted_bits = wasted_bits,
        .estimated_bits = constant_subframe_bits(bits_per_sample, wasted_bits),
    };
}

unsigned sample_trailing_zero_bits(std::int32_t sample, unsigned bits_per_sample)
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

unsigned common_wasted_bits(
    SampleSpan samples,
    unsigned bits_per_sample)
{
    unsigned wasted_bits = bits_per_sample;
    for (const auto sample : samples) {
        wasted_bits = std::min(wasted_bits,
            sample_trailing_zero_bits(sample, bits_per_sample));
        if (wasted_bits == 0) {
            return 0;
        }
    }
    return std::min(wasted_bits, bits_per_sample - 1U);
}

unsigned validated_common_wasted_bits(
    SampleSpan samples,
    unsigned bits_per_sample)
{
    unsigned wasted_bits = bits_per_sample;
    for (const auto sample : samples) {
        validate_sample(sample, bits_per_sample);
        wasted_bits = std::min(wasted_bits,
            sample_trailing_zero_bits(sample, bits_per_sample));
    }
    return std::min(wasted_bits, bits_per_sample - 1U);
}

struct ValidatedShiftedSamples {
    unsigned wasted_bits = 0;
    std::vector<std::int32_t> shifted_samples;
};

struct ValidatedShiftedResiduals {
    unsigned wasted_bits = 0;
    std::vector<std::int32_t> shifted_samples;
    std::vector<std::int64_t> residuals;
};

struct SelectedWastedBitsValidator {
    unsigned bits_per_sample = 0;
    unsigned selected_wasted_bits = 0;
    std::uint64_t sample_mask = 0;
    std::uint64_t lower_mask = 0;
    std::uint64_t witness_bit = 0;
    bool saw_exact_wasted_bits = false;

    SelectedWastedBitsValidator(
        unsigned input_bits_per_sample,
        unsigned input_selected_wasted_bits)
        : bits_per_sample(input_bits_per_sample),
          selected_wasted_bits(input_selected_wasted_bits)
    {
        if (selected_wasted_bits >= bits_per_sample) {
            throw std::runtime_error("selected FLAC subframe wasted-bits count does not match samples");
        }
        sample_mask = bits_per_sample == 64
            ? std::numeric_limits<std::uint64_t>::max()
            : ((std::uint64_t {1} << bits_per_sample) - 1U);
        lower_mask = selected_wasted_bits == 0
            ? 0
            : ((std::uint64_t {1} << selected_wasted_bits) - 1U);
        witness_bit = std::uint64_t {1} << selected_wasted_bits;
        saw_exact_wasted_bits = selected_wasted_bits + 1U >= bits_per_sample;
    }

    void observe(std::int32_t sample)
    {
        validate_sample(sample, bits_per_sample);
        const auto raw = static_cast<std::uint64_t>(sample) & sample_mask;
        if ((raw & lower_mask) != 0) {
            throw std::runtime_error("selected FLAC subframe wasted-bits count does not match samples");
        }
        saw_exact_wasted_bits = saw_exact_wasted_bits ||
            ((raw & witness_bit) != 0);
    }

    unsigned finish() const
    {
        if (!saw_exact_wasted_bits) {
            throw std::runtime_error("selected FLAC subframe wasted-bits count does not match samples");
        }
        return selected_wasted_bits;
    }
};

ValidatedShiftedSamples validate_and_shift_selected_samples(
    SampleSpan samples,
    unsigned bits_per_sample,
    unsigned selected_wasted_bits)
{
    if (selected_wasted_bits >= bits_per_sample) {
        throw std::runtime_error("selected FLAC subframe wasted-bits count does not match samples");
    }

    ValidatedShiftedSamples result;
    result.shifted_samples.reserve(samples.size());
    unsigned wasted_bits = bits_per_sample;

    const auto divisor = selected_wasted_bits == 0
        ? std::int64_t {1}
        : std::int64_t {1} << selected_wasted_bits;
    for (const auto sample : samples) {
        validate_sample(sample, bits_per_sample);
        wasted_bits = std::min(wasted_bits,
            sample_trailing_zero_bits(sample, bits_per_sample));
        result.shifted_samples.push_back(selected_wasted_bits == 0
            ? sample
            : static_cast<std::int32_t>(static_cast<std::int64_t>(sample) / divisor));
    }

    result.wasted_bits = std::min(wasted_bits, bits_per_sample - 1U);
    return result;
}

std::int64_t fixed_prediction_residual_from_shifted(
    const std::vector<std::int32_t>& samples,
    std::size_t index,
    unsigned order)
{
    switch (order) {
    case 0:
        return samples[index];
    case 1:
        return static_cast<std::int64_t>(samples[index]) - samples[index - 1U];
    case 2:
        return static_cast<std::int64_t>(samples[index]) -
            (2LL * samples[index - 1U]) + samples[index - 2U];
    case 3:
        return static_cast<std::int64_t>(samples[index]) -
            (3LL * samples[index - 1U]) + (3LL * samples[index - 2U]) -
            samples[index - 3U];
    case 4:
        return static_cast<std::int64_t>(samples[index]) -
            (4LL * samples[index - 1U]) + (6LL * samples[index - 2U]) -
            (4LL * samples[index - 3U]) + samples[index - 4U];
    default:
        throw std::runtime_error("unsupported FLAC fixed predictor order");
    }
}

std::vector<std::int32_t> shift_samples(
    SampleSpan samples,
    unsigned wasted_bits)
{
    std::vector<std::int32_t> shifted;
    shifted.reserve(samples.size());
    if (wasted_bits == 0) {
        shifted.assign(samples.begin(), samples.end());
        return shifted;
    }

    const auto divisor = std::int64_t {1} << wasted_bits;
    for (const auto sample : samples) {
        shifted.push_back(static_cast<std::int32_t>(
            static_cast<std::int64_t>(sample) / divisor));
    }
    return shifted;
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

std::int64_t lpc_prediction_residual_from_shifted(
    const std::vector<std::int32_t>& samples,
    std::size_t index,
    const std::vector<std::int32_t>& coefficients,
    unsigned order,
    int quantization_shift)
{
    std::int64_t sum = 0;
    for (unsigned j = 0; j < order; ++j) {
        sum += static_cast<std::int64_t>(coefficients[j]) *
            samples[index - j - 1U];
    }
    const auto predicted = arithmetic_shift_right(
        sum, static_cast<unsigned>(quantization_shift));
    return static_cast<std::int64_t>(samples[index]) - predicted;
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

unsigned checked_max_rice_partition_order(unsigned max_rice_partition_order)
{
    if (max_rice_partition_order > kMaxRicePartitionOrder) {
        throw std::runtime_error("native FLAC max Rice partition order must be 0..8");
    }
    return max_rice_partition_order;
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

std::vector<unsigned> rice_parameters_for_partition_order(
    const std::vector<std::int64_t>& residuals,
    std::size_t block_size,
    unsigned predictor_order,
    unsigned partition_order)
{
    if (!valid_partition_order(block_size, predictor_order, partition_order)) {
        throw std::runtime_error("selected FLAC subframe has invalid Rice partition order");
    }

    const auto partition_count = std::size_t {1} << partition_order;
    std::vector<unsigned> rice_parameters;
    rice_parameters.reserve(partition_count);
    std::size_t residual_offset = 0;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const auto residual_count = partition_residual_count(
            block_size, predictor_order, partition_order, partition);
        rice_parameters.push_back(choose_rice_parameter(
            residuals, residual_offset, residual_count));
        residual_offset += residual_count;
    }
    if (residual_offset != residuals.size()) {
        throw std::runtime_error("selected FLAC subframe residual partition accounting error");
    }
    return rice_parameters;
}

std::vector<unsigned> selected_or_computed_rice_parameters(
    const std::vector<unsigned>& selected_rice_parameters,
    const std::vector<std::int64_t>& residuals,
    std::size_t block_size,
    unsigned predictor_order,
    unsigned partition_order)
{
    if (selected_rice_parameters.empty()) {
        return rice_parameters_for_partition_order(
            residuals, block_size, predictor_order, partition_order);
    }

    if (!valid_partition_order(block_size, predictor_order, partition_order)) {
        throw std::runtime_error("selected FLAC subframe has invalid Rice partition order");
    }

    const auto partition_count = std::size_t {1} << partition_order;
    if (selected_rice_parameters.size() != partition_count) {
        throw std::runtime_error("selected FLAC subframe Rice parameter count mismatch");
    }
    for (const auto parameter : selected_rice_parameters) {
        if (parameter > kMaxRiceParameter) {
            throw std::runtime_error("selected FLAC subframe Rice parameter is out of range");
        }
    }
    return selected_rice_parameters;
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
        throw std::runtime_error("internal FLAC residual partition accounting error");
    }
    return bits;
}

std::uint64_t lpc_rice_subframe_bits(
    unsigned order,
    unsigned partition_order,
    unsigned wasted_bits,
    unsigned coefficient_precision,
    const std::vector<unsigned>& rice_parameters,
    const std::vector<std::int64_t>& residuals,
    std::size_t block_size,
    unsigned bits_per_sample)
{
    constexpr unsigned kSubframeHeaderBits = 8;
    constexpr unsigned kCoefficientPrecisionBits = 4;
    constexpr unsigned kQuantizationShiftBits = 5;
    constexpr unsigned kRiceMethodBits = 2;
    constexpr unsigned kPartitionOrderBits = 4;
    constexpr unsigned kRiceParameterBits = 4;

    std::uint64_t bits =
        kSubframeHeaderBits + subframe_wasted_bits_overhead(wasted_bits) +
        (static_cast<std::uint64_t>(order) * bits_per_sample) +
        kCoefficientPrecisionBits + kQuantizationShiftBits +
        (static_cast<std::uint64_t>(order) * coefficient_precision) +
        kRiceMethodBits + kPartitionOrderBits;

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
        throw std::runtime_error("internal FLAC LPC residual partition accounting error");
    }
    return bits;
}

template <typename SampleAt>
std::vector<double> autocorrelation_by_index(
    std::size_t sample_count,
    unsigned max_order,
    SampleAt sample_at)
{
    std::vector<double> autoc(max_order + 1U, 0.0);
    for (unsigned lag = 0; lag <= max_order; ++lag) {
        double sum = 0.0;
        for (std::size_t i = lag; i < sample_count; ++i) {
            sum += static_cast<double>(sample_at(i)) *
                static_cast<double>(sample_at(i - lag));
        }
        autoc[lag] = sum;
    }
    return autoc;
}

std::vector<double> autocorrelation(
    SampleSpan samples,
    unsigned max_order)
{
    return autocorrelation_by_index(samples.size(), max_order, [&](std::size_t index) {
        return samples[index];
    });
}

std::vector<double> autocorrelation(
    const std::vector<double>& samples,
    unsigned max_order)
{
    return autocorrelation_by_index(samples.size(), max_order, [&](std::size_t index) {
        return samples[index];
    });
}

std::vector<double> tukey_windowed_samples(
    SampleSpan samples,
    double taper_fraction)
{
    std::vector<double> windowed;
    windowed.reserve(samples.size());
    for (const auto sample : samples) {
        windowed.push_back(static_cast<double>(sample));
    }

    if (samples.size() <= 1 || taper_fraction <= 0.0) {
        return windowed;
    }
    if (taper_fraction >= 1.0) {
        const auto denominator = static_cast<double>(samples.size() - 1U);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            const auto phase = 2.0 * kPi * static_cast<double>(n) / denominator;
            windowed[n] *= 0.5 - (0.5 * std::cos(phase));
        }
        return windowed;
    }

    const auto edge_width = static_cast<std::size_t>(
        (taper_fraction / 2.0) * static_cast<double>(samples.size()));
    if (edge_width == 0) {
        return windowed;
    }
    const auto np = edge_width - 1U;
    if (np == 0) {
        windowed.front() = 0.0;
        windowed.back() = 0.0;
        return windowed;
    }

    for (std::size_t n = 0; n <= np; ++n) {
        const auto left_weight =
            0.5 - (0.5 * std::cos(kPi * static_cast<double>(n) / static_cast<double>(np)));
        const auto right_weight =
            0.5 - (0.5 * std::cos(kPi * static_cast<double>(n + np) / static_cast<double>(np)));
        windowed[n] *= left_weight;
        windowed[samples.size() - np - 1U + n] *= right_weight;
    }
    return windowed;
}

std::vector<double> welch_windowed_samples(SampleSpan samples)
{
    std::vector<double> windowed;
    windowed.reserve(samples.size());
    for (const auto sample : samples) {
        windowed.push_back(static_cast<double>(sample));
    }

    if (samples.size() <= 1) {
        if (!windowed.empty()) {
            windowed.front() = 0.0;
        }
        return windowed;
    }

    const auto endpoint = static_cast<double>(samples.size() - 1U);
    const auto midpoint = endpoint / 2.0;
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const auto k = (static_cast<double>(n) - midpoint) / midpoint;
        windowed[n] *= 1.0 - (k * k);
    }
    return windowed;
}

bool should_consider_welch_lpc_order(unsigned order, unsigned max_order)
{
    if (order == 0 || max_order == 0) {
        return false;
    }

    const auto welch_candidate_count =
        std::min(kWelchLpcCandidateTargetCount, max_order);
    return order > max_order - welch_candidate_count;
}

std::vector<double> levinson_durbin_coefficients(
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

std::vector<double> levinson_durbin_prediction_errors(
    const std::vector<double>& autoc,
    unsigned max_order)
{
    std::vector<double> errors;
    if (max_order == 0 || autoc.size() <= max_order || autoc[0] <= 0.0) {
        return errors;
    }

    std::vector<double> coefficients(max_order, 0.0);
    double error = autoc[0];
    errors.reserve(max_order);
    for (unsigned i = 0; i < max_order; ++i) {
        double reflection = autoc[i + 1U];
        for (unsigned j = 0; j < i; ++j) {
            reflection -= coefficients[j] * autoc[i - j];
        }
        reflection /= error;
        if (!std::isfinite(reflection)) {
            break;
        }

        auto next = coefficients;
        for (unsigned j = 0; j < i; ++j) {
            next[j] = coefficients[j] - (reflection * coefficients[i - j - 1U]);
        }
        next[i] = reflection;
        coefficients = std::move(next);

        error *= 1.0 - (reflection * reflection);
        if (!std::isfinite(error) || error < 0.0) {
            error = 0.0;
        }
        errors.push_back(error);
        if (error <= 1.0e-9) {
            break;
        }
    }
    return errors;
}

double expected_bits_per_residual_sample(double lpc_error, double error_scale)
{
    if (lpc_error > 0.0) {
        const auto bits = 0.5 * std::log(error_scale * lpc_error) / std::log(2.0);
        return bits >= 0.0 ? bits : 0.0;
    }
    if (lpc_error < 0.0) {
        return 1.0e32;
    }
    return 0.0;
}

std::optional<unsigned> guess_lpc_order_from_autocorrelation(
    const std::vector<double>& autoc,
    unsigned max_order,
    std::size_t total_samples,
    unsigned overhead_bits_per_order)
{
    const auto errors = levinson_durbin_prediction_errors(autoc, max_order);
    if (errors.empty() || total_samples == 0) {
        return std::nullopt;
    }

    unsigned best_order = 1;
    double best_bits = std::numeric_limits<double>::infinity();
    const auto error_scale = 0.5 / static_cast<double>(total_samples);
    for (unsigned order = 1; order <= errors.size(); ++order) {
        const auto residual_samples =
            total_samples > order ? total_samples - order : 0;
        const auto bits =
            expected_bits_per_residual_sample(errors[order - 1U], error_scale) *
                static_cast<double>(residual_samples) +
            static_cast<double>(order * overhead_bits_per_order);
        if (bits < best_bits) {
            best_bits = bits;
            best_order = order;
        }
    }
    return best_order;
}

std::vector<double> partial_tukey_windowed_samples(
    SampleSpan samples,
    double taper_fraction,
    double start,
    double end)
{
    std::vector<double> windowed(samples.size(), 0.0);
    if (samples.empty() || end <= start) {
        return windowed;
    }

    const auto start_n = std::min<std::size_t>(
        samples.size(), static_cast<std::size_t>(start * static_cast<double>(samples.size())));
    const auto end_n = std::min<std::size_t>(
        samples.size(), static_cast<std::size_t>(end * static_cast<double>(samples.size())));
    const auto width = end_n > start_n ? end_n - start_n : 0;
    if (width == 0) {
        return windowed;
    }

    const auto taper = std::clamp(taper_fraction, 0.05, 0.95);
    const auto edge_width = static_cast<std::size_t>(
        (taper / 2.0) * static_cast<double>(width));
    for (std::size_t n = start_n; n < end_n; ++n) {
        double weight = 1.0;
        if (edge_width != 0 && n < start_n + edge_width) {
            const auto i = n - start_n + 1U;
            weight = 0.5 - (0.5 * std::cos(kPi * static_cast<double>(i) /
                static_cast<double>(edge_width)));
        } else if (edge_width != 0 && n >= end_n - edge_width) {
            const auto i = end_n - n;
            weight = 0.5 - (0.5 * std::cos(kPi * static_cast<double>(i) /
                static_cast<double>(edge_width)));
        }
        windowed[n] = static_cast<double>(samples[n]) * weight;
    }
    return windowed;
}

std::vector<double> punchout_tukey_windowed_samples(
    SampleSpan samples,
    double taper_fraction,
    double start,
    double end)
{
    std::vector<double> windowed;
    windowed.reserve(samples.size());
    for (const auto sample : samples) {
        windowed.push_back(static_cast<double>(sample));
    }
    if (samples.empty() || end <= start) {
        return windowed;
    }

    const auto start_n = std::min<std::size_t>(
        samples.size(), static_cast<std::size_t>(start * static_cast<double>(samples.size())));
    const auto end_n = std::min<std::size_t>(
        samples.size(), static_cast<std::size_t>(end * static_cast<double>(samples.size())));
    const auto taper = std::clamp(taper_fraction, 0.05, 0.95);
    const auto left_edge = static_cast<std::size_t>(
        (taper / 2.0) * static_cast<double>(start_n));
    const auto right_span = samples.size() > end_n ? samples.size() - end_n : 0;
    const auto right_edge = static_cast<std::size_t>(
        (taper / 2.0) * static_cast<double>(right_span));

    for (std::size_t n = 0; n < samples.size(); ++n) {
        double weight = 1.0;
        if (n >= start_n && n < end_n) {
            weight = 0.0;
        } else if (left_edge != 0 && n < left_edge) {
            weight = 0.5 - (0.5 * std::cos(kPi * static_cast<double>(n + 1U) /
                static_cast<double>(left_edge)));
        } else if (left_edge != 0 && n >= start_n - left_edge && n < start_n) {
            const auto i = start_n - n;
            weight = 0.5 - (0.5 * std::cos(kPi * static_cast<double>(i) /
                static_cast<double>(left_edge)));
        } else if (right_edge != 0 && n >= end_n && n < end_n + right_edge) {
            const auto i = n - end_n + 1U;
            weight = 0.5 - (0.5 * std::cos(kPi * static_cast<double>(i) /
                static_cast<double>(right_edge)));
        } else if (right_edge != 0 && n >= samples.size() - right_edge) {
            const auto i = samples.size() - n;
            weight = 0.5 - (0.5 * std::cos(kPi * static_cast<double>(i) /
                static_cast<double>(right_edge)));
        }
        windowed[n] *= weight;
    }
    return windowed;
}

struct LpcAutocorrelationCandidate {
    FlacLpcWindowKind window = FlacLpcWindowKind::Rectangular;
    std::vector<double> autocorrelation;
};

std::vector<LpcAutocorrelationCandidate> lpc_order_guess_autocorrelations(
    const std::vector<std::int32_t>& shifted_samples,
    unsigned max_order)
{
    std::vector<LpcAutocorrelationCandidate> candidates;
    candidates.push_back(LpcAutocorrelationCandidate {
        .window = FlacLpcWindowKind::Rectangular,
        .autocorrelation = autocorrelation(shifted_samples, max_order),
    });
    candidates.push_back(LpcAutocorrelationCandidate {
        .window = FlacLpcWindowKind::Tukey,
        .autocorrelation =
            autocorrelation(tukey_windowed_samples(shifted_samples, 0.5), max_order),
    });
    candidates.push_back(LpcAutocorrelationCandidate {
        .window = FlacLpcWindowKind::Welch,
        .autocorrelation =
            autocorrelation(welch_windowed_samples(shifted_samples), max_order),
    });
    return candidates;
}

std::vector<LpcAutocorrelationCandidate> subdivide_tukey3_autocorrelations(
    const std::vector<std::int32_t>& shifted_samples,
    unsigned max_order)
{
    std::vector<LpcAutocorrelationCandidate> candidates;
    constexpr unsigned kParts = 3;
    const double taper_fraction = 0.5 / static_cast<double>(kParts);

    candidates.push_back(LpcAutocorrelationCandidate {
        .window = FlacLpcWindowKind::Tukey,
        .autocorrelation =
            autocorrelation(tukey_windowed_samples(shifted_samples, taper_fraction), max_order),
    });

    for (unsigned parts : {2U, 3U}) {
        for (unsigned part = 0; part < parts; ++part) {
            const auto start = static_cast<double>(part) / static_cast<double>(parts);
            const auto end = static_cast<double>(part + 1U) / static_cast<double>(parts);
            candidates.push_back(LpcAutocorrelationCandidate {
                .window = FlacLpcWindowKind::Tukey,
                .autocorrelation = autocorrelation(
                    partial_tukey_windowed_samples(
                        shifted_samples, taper_fraction, start, end),
                    max_order),
            });
        }
    }

    for (unsigned part = 0; part < kParts; ++part) {
        const auto start = static_cast<double>(part) / static_cast<double>(kParts);
        const auto end = static_cast<double>(part + 1U) / static_cast<double>(kParts);
        candidates.push_back(LpcAutocorrelationCandidate {
            .window = FlacLpcWindowKind::Tukey,
            .autocorrelation = autocorrelation(
                punchout_tukey_windowed_samples(
                    shifted_samples, taper_fraction, start, end),
                max_order),
        });
    }

    return candidates;
}

void append_quantized_lpc_candidate(
    std::vector<QuantizedLpcCoefficients>& candidates,
    int quantization_shift,
    FlacLpcQuantizationKind quantization,
    std::vector<std::int32_t> coefficients)
{
    const auto any_nonzero = std::any_of(
        coefficients.begin(), coefficients.end(), [](std::int32_t value) {
            return value != 0;
        });
    if (!any_nonzero) {
        return;
    }
    const auto duplicate = std::any_of(
        candidates.begin(), candidates.end(), [&](const QuantizedLpcCoefficients& candidate) {
            return candidate.quantization_shift == quantization_shift &&
                candidate.coefficients == coefficients;
        });
    if (duplicate) {
        return;
    }
    candidates.push_back(QuantizedLpcCoefficients {
        .quantization_shift = quantization_shift,
        .quantization = quantization,
        .coefficients = std::move(coefficients),
    });
}

std::vector<QuantizedLpcCoefficients> quantize_lpc_coefficients(
    const std::vector<double>& coefficients,
    unsigned coefficient_precision)
{
    double max_abs = 0.0;
    for (const auto coefficient : coefficients) {
        if (!std::isfinite(coefficient)) {
            return {};
        }
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

    std::vector<QuantizedLpcCoefficients> candidates;

    std::vector<std::int32_t> independently_rounded;
    independently_rounded.reserve(coefficients.size());
    for (const auto coefficient : coefficients) {
        auto value = static_cast<std::int64_t>(
            std::llround(std::ldexp(coefficient, quantization_shift)));
        value = std::clamp(value, min_quantized, max_quantized);
        independently_rounded.push_back(static_cast<std::int32_t>(value));
    }
    append_quantized_lpc_candidate(
        candidates,
        quantization_shift,
        FlacLpcQuantizationKind::Independent,
        std::move(independently_rounded));

    std::vector<std::int32_t> error_feedback_rounded;
    error_feedback_rounded.reserve(coefficients.size());
    double error = 0.0;
    for (const auto coefficient : coefficients) {
        const auto scaled = raw_shift < 0
            ? std::ldexp(coefficient, raw_shift)
            : std::ldexp(coefficient, quantization_shift);
        error += scaled;
        auto value = static_cast<std::int64_t>(std::llround(error));
        value = std::clamp(value, min_quantized, max_quantized);
        error -= static_cast<double>(value);
        error_feedback_rounded.push_back(static_cast<std::int32_t>(value));
    }
    append_quantized_lpc_candidate(
        candidates,
        quantization_shift,
        FlacLpcQuantizationKind::ErrorFeedback,
        std::move(error_feedback_rounded));

    return candidates;
}

std::vector<std::int64_t> lpc_residuals(
    SampleSpan samples,
    const std::vector<std::int32_t>& coefficients,
    unsigned order,
    int quantization_shift)
{
    std::vector<std::int64_t> residuals;
    residuals.reserve(samples.size() - order);
    for (std::size_t i = order; i < samples.size(); ++i) {
        std::int64_t sum = 0;
        for (unsigned j = 0; j < order; ++j) {
            sum += static_cast<std::int64_t>(coefficients[j]) *
                samples[i - j - 1U];
        }
        const auto predicted = arithmetic_shift_right(
            sum, static_cast<unsigned>(quantization_shift));
        residuals.push_back(static_cast<std::int64_t>(samples[i]) - predicted);
    }
    return residuals;
}

FixedRiceSubframe choose_fixed_rice_subframe(
    SampleSpan samples,
    unsigned bits_per_sample,
    unsigned max_rice_partition_order)
{
    max_rice_partition_order = checked_max_rice_partition_order(max_rice_partition_order);
    const auto wasted_bits = common_wasted_bits(samples, bits_per_sample);
    auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = bits_per_sample - wasted_bits;

    const auto max_order = shifted_samples.size() > 1
        ? std::min<std::size_t>(4, shifted_samples.size() - 1)
        : 0;

    FixedRiceSubframe best;
    auto best_bits = std::numeric_limits<std::uint64_t>::max();
    for (unsigned order = 0; order <= max_order; ++order) {
        auto residuals = fixed_residuals(shifted_samples, order);
        for (unsigned partition_order = 0; partition_order <= max_rice_partition_order; ++partition_order) {
            if (!valid_partition_order(shifted_samples.size(), order, partition_order)) {
                continue;
            }

            const auto partition_count = std::size_t {1} << partition_order;
            std::vector<unsigned> rice_parameters;
            rice_parameters.reserve(partition_count);
            std::size_t residual_offset = 0;
            for (std::size_t partition = 0; partition < partition_count; ++partition) {
                const auto residual_count = partition_residual_count(
                    shifted_samples.size(), order, partition_order, partition);
                rice_parameters.push_back(choose_rice_parameter(
                    residuals, residual_offset, residual_count));
                residual_offset += residual_count;
            }
            if (residual_offset != residuals.size()) {
                throw std::runtime_error("internal FLAC residual partition accounting error");
            }

            const auto bits = fixed_rice_subframe_bits(
                order, partition_order, wasted_bits, rice_parameters, residuals,
                shifted_samples.size(), effective_bits_per_sample);
            if (bits < best_bits) {
                best_bits = bits;
                best.order = order;
                best.partition_order = partition_order;
                best.wasted_bits = wasted_bits;
                best.effective_bits_per_sample = effective_bits_per_sample;
                best.shifted_samples = shifted_samples;
                best.rice_parameters = std::move(rice_parameters);
                best.residuals = residuals;
                best.bits = bits;
            }
        }
    }
    return best;
}

unsigned guess_fixed_predictor_order(const std::vector<std::int32_t>& shifted_samples)
{
    const auto max_order = shifted_samples.size() > 1
        ? std::min<std::size_t>(4, shifted_samples.size() - 1)
        : 0;
    unsigned best_order = 0;
    std::uint64_t best_sum = std::numeric_limits<std::uint64_t>::max();
    for (unsigned order = 0; order <= max_order; ++order) {
        std::uint64_t sum = 0;
        for (std::size_t i = order; i < shifted_samples.size(); ++i) {
            const auto residual = fixed_prediction_residual(shifted_samples, i, order);
            sum += residual < 0
                ? static_cast<std::uint64_t>(-(residual + 1)) + 1U
                : static_cast<std::uint64_t>(residual);
        }
        if (sum < best_sum) {
            best_sum = sum;
            best_order = order;
        }
    }
    return best_order;
}

FixedRiceSubframe analyze_fixed_rice_order(
    const std::vector<std::int32_t>& shifted_samples,
    unsigned bits_per_sample,
    unsigned wasted_bits,
    unsigned order,
    unsigned max_rice_partition_order,
    RiceSelectionMode rice_mode)
{
    max_rice_partition_order = checked_max_rice_partition_order(max_rice_partition_order);
    const auto effective_bits_per_sample = bits_per_sample - wasted_bits;
    auto residuals = fixed_residuals(shifted_samples, order);

    FixedRiceSubframe best;
    best.bits = std::numeric_limits<std::uint64_t>::max();
    if (rice_mode == RiceSelectionMode::MeanEstimate ||
        rice_mode == RiceSelectionMode::MeanEstimateOnly) {
        const auto rice_choice = choose_mean_rice_partition_order(
            residuals, shifted_samples.size(), order, max_rice_partition_order);
        best.order = order;
        best.partition_order = rice_choice.partition_order;
        best.wasted_bits = wasted_bits;
        best.effective_bits_per_sample = effective_bits_per_sample;
        best.shifted_samples = shifted_samples;
        best.rice_parameters = rice_choice.parameters;
        best.residuals = std::move(residuals);
        best.bits = rice_mode == RiceSelectionMode::MeanEstimateOnly
            ? fixed_rice_subframe_mean_estimated_bits(
                order, wasted_bits, shifted_samples.size(), effective_bits_per_sample,
                rice_choice)
            : fixed_rice_subframe_bits(
                order, best.partition_order, wasted_bits, best.rice_parameters,
                best.residuals, shifted_samples.size(), effective_bits_per_sample);
        return best;
    }

    for (unsigned partition_order = 0; partition_order <= max_rice_partition_order; ++partition_order) {
        if (!valid_partition_order(shifted_samples.size(), order, partition_order)) {
            continue;
        }

        const auto rice_parameters = rice_parameters_for_partition_order(
            residuals, shifted_samples.size(), order, partition_order);
        const auto bits = fixed_rice_subframe_bits(
            order, partition_order, wasted_bits, rice_parameters, residuals,
            shifted_samples.size(), effective_bits_per_sample);
        if (bits < best.bits) {
            best.order = order;
            best.partition_order = partition_order;
            best.wasted_bits = wasted_bits;
            best.effective_bits_per_sample = effective_bits_per_sample;
            best.shifted_samples = shifted_samples;
            best.rice_parameters = rice_parameters;
            best.residuals = residuals;
            best.bits = bits;
        }
    }

    return best;
}

FixedRiceSubframe choose_fixed_rice_subframe_order_guess(
    SampleSpan samples,
    unsigned bits_per_sample,
    unsigned max_rice_partition_order,
    RiceSelectionMode rice_mode)
{
    max_rice_partition_order = checked_max_rice_partition_order(max_rice_partition_order);
    const auto wasted_bits = common_wasted_bits(samples, bits_per_sample);
    auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto order = guess_fixed_predictor_order(shifted_samples);
    return analyze_fixed_rice_order(
        shifted_samples, bits_per_sample, wasted_bits, order,
        max_rice_partition_order, rice_mode);
}

std::optional<LpcRiceSubframe> analyze_lpc_rice_candidate(
    const std::vector<std::int32_t>& shifted_samples,
    const QuantizedLpcCoefficients& quantized,
    unsigned order,
    FlacLpcWindowKind window,
    unsigned wasted_bits,
    unsigned effective_bits_per_sample,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    RiceSelectionMode rice_mode = RiceSelectionMode::Exact)
{
    if (quantized.coefficients.size() != order ||
        quantized.quantization_shift < 0 ||
        quantized.quantization_shift > 15) {
        return std::nullopt;
    }

    auto residuals = lpc_residuals(
        shifted_samples, quantized.coefficients, order,
        quantized.quantization_shift);

    LpcRiceSubframe best;
    best.bits = std::numeric_limits<std::uint64_t>::max();
    if (rice_mode == RiceSelectionMode::MeanEstimate ||
        rice_mode == RiceSelectionMode::MeanEstimateOnly) {
        const auto rice_choice = choose_mean_rice_partition_order(
            residuals, shifted_samples.size(), order, max_rice_partition_order);
        best.order = order;
        best.partition_order = rice_choice.partition_order;
        best.wasted_bits = wasted_bits;
        best.effective_bits_per_sample = effective_bits_per_sample;
        best.coefficient_precision = lpc_coefficient_precision;
        best.quantization_shift = quantized.quantization_shift;
        best.window = window;
        best.quantization = quantized.quantization;
        best.shifted_samples = shifted_samples;
        best.coefficients = quantized.coefficients;
        best.rice_parameters = rice_choice.parameters;
        best.residuals = std::move(residuals);
        best.bits = rice_mode == RiceSelectionMode::MeanEstimateOnly
            ? lpc_rice_subframe_mean_estimated_bits(
                order, wasted_bits, lpc_coefficient_precision, shifted_samples.size(),
                effective_bits_per_sample, rice_choice)
            : lpc_rice_subframe_bits(
                order, best.partition_order, wasted_bits, lpc_coefficient_precision,
                best.rice_parameters, best.residuals, shifted_samples.size(),
                effective_bits_per_sample);
        return best;
    }

    for (unsigned partition_order = 0; partition_order <= max_rice_partition_order; ++partition_order) {
        if (!valid_partition_order(shifted_samples.size(), order, partition_order)) {
            continue;
        }

        const auto partition_count = std::size_t {1} << partition_order;
        std::vector<unsigned> rice_parameters;
        rice_parameters.reserve(partition_count);
        std::size_t residual_offset = 0;
        for (std::size_t partition = 0; partition < partition_count; ++partition) {
            const auto residual_count = partition_residual_count(
                shifted_samples.size(), order, partition_order, partition);
            rice_parameters.push_back(choose_rice_parameter(
                residuals, residual_offset, residual_count));
            residual_offset += residual_count;
        }
        if (residual_offset != residuals.size()) {
            throw std::runtime_error("internal FLAC LPC residual partition accounting error");
        }

        const auto bits = lpc_rice_subframe_bits(
            order, partition_order, wasted_bits, lpc_coefficient_precision,
            rice_parameters, residuals, shifted_samples.size(),
            effective_bits_per_sample);
        if (bits < best.bits) {
            best.order = order;
            best.partition_order = partition_order;
            best.wasted_bits = wasted_bits;
            best.effective_bits_per_sample = effective_bits_per_sample;
            best.coefficient_precision = lpc_coefficient_precision;
            best.quantization_shift = quantized.quantization_shift;
            best.window = window;
            best.quantization = quantized.quantization;
            best.shifted_samples = shifted_samples;
            best.coefficients = quantized.coefficients;
            best.rice_parameters = std::move(rice_parameters);
            best.residuals = residuals;
            best.bits = bits;
        }
    }

    if (best.bits == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return best;
}

void append_lpc_rice_order_candidates(
    const std::vector<std::int32_t>& shifted_samples,
    const std::vector<double>& autocorrelation_candidate,
    FlacLpcWindowKind window,
    unsigned order,
    unsigned wasted_bits,
    unsigned effective_bits_per_sample,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    RiceSelectionMode rice_mode,
    std::vector<LpcRiceSubframe>& candidates)
{
    auto coefficients = levinson_durbin_coefficients(autocorrelation_candidate, order);
    if (coefficients.size() != order) {
        return;
    }

    auto quantized_candidates = quantize_lpc_coefficients(
        coefficients, lpc_coefficient_precision);
    for (const auto& quantized : quantized_candidates) {
        auto candidate = analyze_lpc_rice_candidate(
            shifted_samples,
            quantized,
            order,
            window,
            wasted_bits,
            effective_bits_per_sample,
            lpc_coefficient_precision,
            max_rice_partition_order,
            rice_mode);
        if (candidate.has_value()) {
            candidates.push_back(std::move(*candidate));
        }
    }
}

void consider_lpc_rice_order(
    const std::vector<std::int32_t>& shifted_samples,
    const std::vector<double>& autocorrelation_candidate,
    FlacLpcWindowKind window,
    unsigned order,
    unsigned wasted_bits,
    unsigned effective_bits_per_sample,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    RiceSelectionMode rice_mode,
    LpcRiceSubframe& best)
{
    std::vector<LpcRiceSubframe> candidates;
    append_lpc_rice_order_candidates(
        shifted_samples,
        autocorrelation_candidate,
        window,
        order,
        wasted_bits,
        effective_bits_per_sample,
        lpc_coefficient_precision,
        max_rice_partition_order,
        rice_mode,
        candidates);
    for (auto& candidate : candidates) {
        if (candidate.bits < best.bits) {
            best = std::move(candidate);
        }
    }
}

LpcRiceSubframe choose_lpc_rice_subframe_for_order(
    SampleSpan samples,
    unsigned bits_per_sample,
    unsigned lpc_order,
    unsigned max_lpc_order,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    max_rice_partition_order = checked_max_rice_partition_order(max_rice_partition_order);
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("native FLAC LPC coefficient precision must be 1..15");
    }

    LpcRiceSubframe best;
    best.bits = std::numeric_limits<std::uint64_t>::max();
    if (samples.size() < kMinLpcBlockSize || lpc_order == 0 ||
        max_lpc_order == 0) {
        return best;
    }

    const auto max_order = std::min<std::size_t>(
        std::min(max_lpc_order, kMaxLpcOrder), samples.size() - 1U);
    if (lpc_order > max_order) {
        return best;
    }

    const auto wasted_bits = common_wasted_bits(samples, bits_per_sample);
    auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = bits_per_sample - wasted_bits;
    const auto rectangular_autocorrelation = autocorrelation(shifted_samples, lpc_order);
    consider_lpc_rice_order(
        shifted_samples,
        rectangular_autocorrelation,
        FlacLpcWindowKind::Rectangular,
        lpc_order,
        wasted_bits, effective_bits_per_sample, lpc_coefficient_precision,
        max_rice_partition_order, RiceSelectionMode::Exact, best);
    const auto tukey_autocorrelation =
        autocorrelation(tukey_windowed_samples(shifted_samples, 0.5), lpc_order);
    consider_lpc_rice_order(
        shifted_samples,
        tukey_autocorrelation,
        FlacLpcWindowKind::Tukey,
        lpc_order,
        wasted_bits, effective_bits_per_sample, lpc_coefficient_precision,
        max_rice_partition_order, RiceSelectionMode::Exact, best);

    if (should_consider_welch_lpc_order(lpc_order, static_cast<unsigned>(max_order))) {
        const auto welch_autocorrelation =
            autocorrelation(welch_windowed_samples(shifted_samples), lpc_order);
        consider_lpc_rice_order(
            shifted_samples,
            welch_autocorrelation,
            FlacLpcWindowKind::Welch,
            lpc_order,
            wasted_bits, effective_bits_per_sample, lpc_coefficient_precision,
            max_rice_partition_order, RiceSelectionMode::Exact, best);
    }
    return best;
}

LpcRiceSubframe choose_lpc_rice_subframe(
    SampleSpan samples,
    unsigned bits_per_sample,
    unsigned max_lpc_order,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order)
{
    max_rice_partition_order = checked_max_rice_partition_order(max_rice_partition_order);
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("native FLAC LPC coefficient precision must be 1..15");
    }
    LpcRiceSubframe best;
    best.bits = std::numeric_limits<std::uint64_t>::max();

    if (samples.size() < kMinLpcBlockSize || max_lpc_order == 0) {
        return best;
    }

    const auto wasted_bits = common_wasted_bits(samples, bits_per_sample);
    auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = bits_per_sample - wasted_bits;
    const auto max_order = std::min<std::size_t>(
        std::min(max_lpc_order, kMaxLpcOrder), shifted_samples.size() - 1U);
    const auto rectangular_autocorrelation =
        autocorrelation(shifted_samples, static_cast<unsigned>(max_order));
    const auto tukey_autocorrelation =
        autocorrelation(tukey_windowed_samples(shifted_samples, 0.5),
            static_cast<unsigned>(max_order));
    const auto welch_autocorrelation =
        autocorrelation(welch_windowed_samples(shifted_samples),
            static_cast<unsigned>(max_order));

    for (unsigned order = 1; order <= max_order; ++order) {
        consider_lpc_rice_order(
            shifted_samples,
            rectangular_autocorrelation,
            FlacLpcWindowKind::Rectangular,
            order,
            wasted_bits, effective_bits_per_sample, lpc_coefficient_precision,
            max_rice_partition_order, RiceSelectionMode::Exact, best);
        consider_lpc_rice_order(
            shifted_samples,
            tukey_autocorrelation,
            FlacLpcWindowKind::Tukey,
            order,
            wasted_bits, effective_bits_per_sample, lpc_coefficient_precision,
            max_rice_partition_order, RiceSelectionMode::Exact, best);
        if (should_consider_welch_lpc_order(order, static_cast<unsigned>(max_order))) {
            consider_lpc_rice_order(
                shifted_samples,
                welch_autocorrelation,
                FlacLpcWindowKind::Welch,
                order,
                wasted_bits, effective_bits_per_sample, lpc_coefficient_precision,
                max_rice_partition_order, RiceSelectionMode::Exact, best);
        }
    }

    return best;
}

LpcRiceSubframe choose_lpc_rice_subframe_order_guess(
    SampleSpan samples,
    unsigned bits_per_sample,
    unsigned max_lpc_order,
    unsigned lpc_coefficient_precision,
    unsigned max_rice_partition_order,
    RiceSelectionMode rice_mode,
    bool use_subdivide_tukey3)
{
    max_rice_partition_order = checked_max_rice_partition_order(max_rice_partition_order);
    if (lpc_coefficient_precision == 0 || lpc_coefficient_precision > 15) {
        throw std::runtime_error("native FLAC LPC coefficient precision must be 1..15");
    }

    LpcRiceSubframe best;
    best.bits = std::numeric_limits<std::uint64_t>::max();
    if (samples.size() < kMinLpcBlockSize || max_lpc_order == 0) {
        return best;
    }

    const auto wasted_bits = common_wasted_bits(samples, bits_per_sample);
    auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = bits_per_sample - wasted_bits;
    const auto max_order = static_cast<unsigned>(std::min<std::size_t>(
        std::min(max_lpc_order, kMaxLpcOrder), shifted_samples.size() - 1U));
    if (max_order == 0) {
        return best;
    }

    const auto autocorrelations = use_subdivide_tukey3
        ? subdivide_tukey3_autocorrelations(shifted_samples, max_order)
        : lpc_order_guess_autocorrelations(shifted_samples, max_order);
    const auto overhead_bits_per_order =
        effective_bits_per_sample + lpc_coefficient_precision;
    for (const auto& candidate : autocorrelations) {
        const auto guessed_order = guess_lpc_order_from_autocorrelation(
            candidate.autocorrelation, max_order, shifted_samples.size(),
            overhead_bits_per_order);
        if (!guessed_order.has_value() || *guessed_order == 0 ||
            *guessed_order > max_order) {
            continue;
        }
        consider_lpc_rice_order(
            shifted_samples,
            candidate.autocorrelation,
            candidate.window,
            *guessed_order,
            wasted_bits,
            effective_bits_per_sample,
            lpc_coefficient_precision,
            max_rice_partition_order,
            rice_mode,
            best);
    }
    return best;
}

unsigned checked_selected_wasted_bits(
    unsigned actual_wasted_bits,
    unsigned selected_wasted_bits)
{
    if (selected_wasted_bits != actual_wasted_bits) {
        throw std::runtime_error("selected FLAC subframe wasted-bits count does not match samples");
    }
    return actual_wasted_bits;
}

void validate_selected_fixed_rice_shape(
    std::size_t block_size,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected)
{
    checked_max_rice_partition_order(info.max_rice_partition_order);
    if (selected.fixed_order > 4 || selected.fixed_order >= block_size) {
        throw std::runtime_error("selected fixed/Rice subframe has invalid predictor order");
    }
    if (selected.rice_partition_order > info.max_rice_partition_order) {
        throw std::runtime_error("selected fixed/Rice subframe exceeds max Rice partition order");
    }
}

void validate_selected_lpc_rice_shape(
    std::size_t block_size,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected)
{
    checked_max_rice_partition_order(info.max_rice_partition_order);
    if (selected.lpc_order == 0 || selected.lpc_order > 32 ||
        selected.lpc_order > info.max_lpc_order ||
        selected.lpc_order >= block_size) {
        throw std::runtime_error("selected LPC/Rice subframe has invalid predictor order");
    }
    if (selected.coefficient_precision == 0 || selected.coefficient_precision > 15) {
        throw std::runtime_error("selected LPC/Rice subframe has invalid coefficient precision");
    }
    if (selected.quantization_shift < 0 || selected.quantization_shift > 15) {
        throw std::runtime_error("selected LPC/Rice subframe has invalid quantization shift");
    }
    if (selected.rice_partition_order > info.max_rice_partition_order) {
        throw std::runtime_error("selected LPC/Rice subframe exceeds max Rice partition order");
    }
    if (selected.coefficients.size() != selected.lpc_order) {
        throw std::runtime_error("selected LPC/Rice subframe coefficient count mismatch");
    }
    for (const auto coefficient : selected.coefficients) {
        if (!signed_value_fits_bits(coefficient, selected.coefficient_precision)) {
            throw std::runtime_error("selected LPC/Rice subframe coefficient does not fit precision");
        }
    }
}

ValidatedShiftedResiduals validate_shift_and_fixed_residuals(
    SampleSpan samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected)
{
    validate_selected_fixed_rice_shape(samples.size(), info, selected);

    ValidatedShiftedResiduals result;
    result.shifted_samples.resize(samples.size());
    result.residuals.resize(samples.size() - selected.fixed_order);
    SelectedWastedBitsValidator wasted_bits(
        info.bits_per_sample, selected.wasted_bits);

    const auto divisor = selected.wasted_bits == 0
        ? std::int64_t {1}
        : std::int64_t {1} << selected.wasted_bits;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto sample = samples[i];
        wasted_bits.observe(sample);
        result.shifted_samples[i] = selected.wasted_bits == 0
            ? sample
            : static_cast<std::int32_t>(static_cast<std::int64_t>(sample) / divisor);
        if (i >= selected.fixed_order) {
            result.residuals[i - selected.fixed_order] =
                fixed_prediction_residual_from_shifted(
                    result.shifted_samples, i, selected.fixed_order);
        }
    }

    result.wasted_bits = wasted_bits.finish();
    return result;
}

ValidatedShiftedResiduals validate_shift_and_lpc_residuals(
    SampleSpan samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected)
{
    validate_selected_lpc_rice_shape(samples.size(), info, selected);

    ValidatedShiftedResiduals result;
    result.shifted_samples.resize(samples.size());
    result.residuals.resize(samples.size() - selected.lpc_order);
    SelectedWastedBitsValidator wasted_bits(
        info.bits_per_sample, selected.wasted_bits);

    const auto divisor = selected.wasted_bits == 0
        ? std::int64_t {1}
        : std::int64_t {1} << selected.wasted_bits;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto sample = samples[i];
        wasted_bits.observe(sample);
        result.shifted_samples[i] = selected.wasted_bits == 0
            ? sample
            : static_cast<std::int32_t>(static_cast<std::int64_t>(sample) / divisor);
        if (i >= selected.lpc_order) {
            result.residuals[i - selected.lpc_order] =
                lpc_prediction_residual_from_shifted(
                    result.shifted_samples,
                    i,
                    selected.coefficients,
                    selected.lpc_order,
                    selected.quantization_shift);
        }
    }

    result.wasted_bits = wasted_bits.finish();
    return result;
}

void check_selected_decision_matches(
    const FlacSelectedSubframe& selected,
    const FlacSubframeDecision& decision)
{
    if (decision.kind != selected.kind ||
        decision.rice_partition_order != selected.rice_partition_order ||
        decision.wasted_bits != selected.wasted_bits) {
        throw std::runtime_error("selected FLAC decision does not match selected subframe");
    }

    switch (selected.kind) {
    case FlacSubframeKind::Constant:
    case FlacSubframeKind::Verbatim:
        return;
    case FlacSubframeKind::FixedRice:
        if (decision.fixed_order != selected.fixed_order) {
            throw std::runtime_error("selected FLAC fixed decision does not match selected subframe");
        }
        return;
    case FlacSubframeKind::LpcRice:
        if (decision.lpc_order != selected.lpc_order) {
            throw std::runtime_error("selected FLAC LPC decision does not match selected subframe");
        }
        return;
    }
    throw std::runtime_error("unknown selected FLAC subframe kind");
}

FixedRiceSubframe make_selected_fixed_rice_subframe(
    std::vector<std::int32_t> shifted_samples,
    std::vector<std::int64_t> residuals,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected,
    unsigned wasted_bits,
    std::optional<std::uint64_t> trusted_bits,
    FlacSelectedFrameWriteTimings* timings)
{
    validate_selected_fixed_rice_shape(shifted_samples.size(), info, selected);
    if (residuals.size() != shifted_samples.size() - selected.fixed_order) {
        throw std::runtime_error("selected fixed/Rice subframe residual count mismatch");
    }

    const auto effective_bits_per_sample = info.bits_per_sample - wasted_bits;
    const auto rice_started = Clock::now();
    auto rice_parameters = selected_or_computed_rice_parameters(
        selected.rice_parameters,
        residuals,
        shifted_samples.size(),
        selected.fixed_order,
        selected.rice_partition_order);
    if (timings != nullptr) {
        add_elapsed_ns(timings->rice_parameter_ns, rice_started);
    }
    const auto bits = trusted_bits.has_value()
        ? *trusted_bits
        : fixed_rice_subframe_bits(
            selected.fixed_order,
            selected.rice_partition_order,
            wasted_bits,
            rice_parameters,
            residuals,
            shifted_samples.size(),
            effective_bits_per_sample);

    return FixedRiceSubframe {
        .order = selected.fixed_order,
        .partition_order = selected.rice_partition_order,
        .wasted_bits = wasted_bits,
        .effective_bits_per_sample = effective_bits_per_sample,
        .shifted_samples = std::move(shifted_samples),
        .rice_parameters = std::move(rice_parameters),
        .residuals = std::move(residuals),
        .bits = bits,
    };
}

FixedRiceSubframe make_selected_fixed_rice_subframe(
    std::vector<std::int32_t> shifted_samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected,
    unsigned wasted_bits,
    std::optional<std::uint64_t> trusted_bits,
    FlacSelectedFrameWriteTimings* timings)
{
    validate_selected_fixed_rice_shape(shifted_samples.size(), info, selected);
    const auto residual_started = Clock::now();
    auto residuals = fixed_residuals(shifted_samples, selected.fixed_order);
    if (timings != nullptr) {
        add_elapsed_ns(timings->residual_ns, residual_started);
    }
    return make_selected_fixed_rice_subframe(
        std::move(shifted_samples),
        std::move(residuals),
        info,
        selected,
        wasted_bits,
        trusted_bits,
        timings);
}

LpcRiceSubframe make_selected_lpc_rice_subframe(
    std::vector<std::int32_t> shifted_samples,
    std::vector<std::int64_t> residuals,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected,
    unsigned wasted_bits,
    std::optional<std::uint64_t> trusted_bits,
    FlacSelectedFrameWriteTimings* timings)
{
    validate_selected_lpc_rice_shape(shifted_samples.size(), info, selected);
    if (residuals.size() != shifted_samples.size() - selected.lpc_order) {
        throw std::runtime_error("selected LPC/Rice subframe residual count mismatch");
    }
    const auto effective_bits_per_sample = info.bits_per_sample - wasted_bits;
    const auto rice_started = Clock::now();
    auto rice_parameters = selected_or_computed_rice_parameters(
        selected.rice_parameters,
        residuals,
        shifted_samples.size(),
        selected.lpc_order,
        selected.rice_partition_order);
    if (timings != nullptr) {
        add_elapsed_ns(timings->rice_parameter_ns, rice_started);
    }
    const auto bits = trusted_bits.has_value()
        ? *trusted_bits
        : lpc_rice_subframe_bits(
            selected.lpc_order,
            selected.rice_partition_order,
            wasted_bits,
            selected.coefficient_precision,
            rice_parameters,
            residuals,
            shifted_samples.size(),
            effective_bits_per_sample);

    return LpcRiceSubframe {
        .order = selected.lpc_order,
        .partition_order = selected.rice_partition_order,
        .wasted_bits = wasted_bits,
        .effective_bits_per_sample = effective_bits_per_sample,
        .coefficient_precision = selected.coefficient_precision,
        .quantization_shift = selected.quantization_shift,
        .shifted_samples = std::move(shifted_samples),
        .coefficients = selected.coefficients,
        .rice_parameters = std::move(rice_parameters),
        .residuals = std::move(residuals),
        .bits = bits,
    };
}

LpcRiceSubframe make_selected_lpc_rice_subframe(
    std::vector<std::int32_t> shifted_samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected,
    unsigned wasted_bits,
    std::optional<std::uint64_t> trusted_bits,
    FlacSelectedFrameWriteTimings* timings)
{
    validate_selected_lpc_rice_shape(shifted_samples.size(), info, selected);
    const auto residual_started = Clock::now();
    auto residuals = lpc_residuals(
        shifted_samples,
        selected.coefficients,
        selected.lpc_order,
        selected.quantization_shift);
    if (timings != nullptr) {
        add_elapsed_ns(timings->residual_ns, residual_started);
    }
    return make_selected_lpc_rice_subframe(
        std::move(shifted_samples),
        std::move(residuals),
        info,
        selected,
        wasted_bits,
        trusted_bits,
        timings);
}

void write_subframe_header(BitWriter& output, unsigned type, unsigned wasted_bits)
{
    output.write_bits(0, 1);
    output.write_bits(type, 6);
    if (wasted_bits == 0) {
        output.write_bits(0, 1);
        return;
    }

    output.write_bits(1, 1);
    output.write_unary(wasted_bits - 1U);
}

void write_rice_signed_block(
    BitWriter& output,
    const std::vector<std::int64_t>& residuals,
    std::size_t offset,
    std::size_t count,
    unsigned parameter)
{
    if (offset > residuals.size() || count > residuals.size() - offset) {
        throw std::runtime_error("internal FLAC residual block range error");
    }
    output.write_rice_signed_block(
        std::span<const std::int64_t>(residuals.data() + offset, count),
        parameter);
}

void write_frame_with_body(
    std::ostream& output,
    std::size_t block_size,
    const FlacFrameInfo& info,
    BitWriter& frame_body)
{
    if (block_size == 0) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    if (info.sample_rate > 0xfffff) {
        throw std::runtime_error("invalid FLAC frame sample rate");
    }
    const auto bps_code = bits_per_sample_code(info.bits_per_sample);
    const auto block_code = block_size_code(block_size);

    std::vector<std::uint8_t> header;
    BitWriter header_bits;
    header_bits.write_bits(0x3ffe, 14);
    header_bits.write_bits(0, 1);
    header_bits.write_bits(0, 1);
    header_bits.write_bits(block_code, 4);
    // Use the STREAMINFO sample rate; this keeps 40 kHz files out of the
    // sample-rate extension path while the native writer is still minimal.
    header_bits.write_bits(0, 4);
    header_bits.write_bits(0, 4);
    header_bits.write_bits(bps_code, 3);
    header_bits.write_bits(0, 1);
    header_bits.align_zero();
    header = header_bits.bytes();
    write_utf8_uint(header, info.frame_number);
    if (block_code == 6) {
        header.push_back(static_cast<std::uint8_t>(block_size - 1U));
    } else {
        const auto coded_size = static_cast<unsigned>(block_size - 1U);
        header.push_back(static_cast<std::uint8_t>((coded_size >> 8U) & 0xffU));
        header.push_back(static_cast<std::uint8_t>(coded_size & 0xffU));
    }
    header.push_back(flac_crc8(header.data(), header.size()));
    write_bytes(output, header);

    frame_body.align_zero();
    const auto& body_bytes = frame_body.bytes();
    write_bytes(output, body_bytes);

    FlacCrc16 crc;
    crc.update(header.data(), header.size());
    crc.update(body_bytes.data(), body_bytes.size());
    write_u16be(output, crc.value());
}

FlacSubframeDecision write_fixed_rice_frame(
    std::ostream& output,
    const FlacFrameInfo& info,
    const FixedRiceSubframe& subframe,
    FlacSelectedFrameWriteTimings* timings = nullptr)
{
    const auto bitstream_started = Clock::now();
    BitWriter frame_body;
    frame_body.reserve_bits(subframe.bits);
    write_subframe_header(frame_body, 0x08U + subframe.order, subframe.wasted_bits);
    for (unsigned i = 0; i < subframe.order; ++i) {
        frame_body.write_signed(subframe.shifted_samples[i],
            subframe.effective_bits_per_sample);
    }
    frame_body.write_bits(0, 2);
    frame_body.write_bits(subframe.partition_order, 4);

    std::size_t residual_offset = 0;
    const auto partition_count = std::size_t {1} << subframe.partition_order;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const auto residual_count = partition_residual_count(
            subframe.shifted_samples.size(), subframe.order,
            subframe.partition_order, partition);
        const auto rice_parameter = subframe.rice_parameters.at(partition);
        frame_body.write_bits(rice_parameter, 4);
        write_rice_signed_block(
            frame_body, subframe.residuals, residual_offset, residual_count,
            rice_parameter);
        residual_offset += residual_count;
    }
    if (residual_offset != subframe.residuals.size()) {
        throw std::runtime_error("internal FLAC residual partition accounting error");
    }
    if (timings != nullptr) {
        add_elapsed_ns(timings->bitstream_ns, bitstream_started);
    }
    const auto output_started = Clock::now();
    write_frame_with_body(output, subframe.shifted_samples.size(), info, frame_body);
    if (timings != nullptr) {
        add_elapsed_ns(timings->frame_output_ns, output_started);
    }
    return fixed_rice_decision(subframe);
}

FlacSubframeDecision write_lpc_rice_frame(
    std::ostream& output,
    const FlacFrameInfo& info,
    const LpcRiceSubframe& subframe,
    FlacSelectedFrameWriteTimings* timings = nullptr)
{
    if (subframe.order == 0 || subframe.order > 32) {
        throw std::runtime_error("invalid FLAC LPC order");
    }
    if (subframe.coefficient_precision == 0 || subframe.coefficient_precision > 15) {
        throw std::runtime_error("invalid FLAC LPC coefficient precision");
    }
    if (subframe.quantization_shift < 0 || subframe.quantization_shift > 15) {
        throw std::runtime_error("invalid FLAC LPC quantization shift");
    }

    const auto bitstream_started = Clock::now();
    BitWriter frame_body;
    frame_body.reserve_bits(subframe.bits);
    write_subframe_header(frame_body, 0x20U + subframe.order - 1U, subframe.wasted_bits);
    for (unsigned i = 0; i < subframe.order; ++i) {
        frame_body.write_signed(subframe.shifted_samples[i],
            subframe.effective_bits_per_sample);
    }
    frame_body.write_bits(subframe.coefficient_precision - 1U, 4);
    frame_body.write_signed(subframe.quantization_shift, 5);
    for (const auto coefficient : subframe.coefficients) {
        frame_body.write_signed(coefficient, subframe.coefficient_precision);
    }
    frame_body.write_bits(0, 2);
    frame_body.write_bits(subframe.partition_order, 4);

    std::size_t residual_offset = 0;
    const auto partition_count = std::size_t {1} << subframe.partition_order;
    for (std::size_t partition = 0; partition < partition_count; ++partition) {
        const auto residual_count = partition_residual_count(
            subframe.shifted_samples.size(), subframe.order,
            subframe.partition_order, partition);
        const auto rice_parameter = subframe.rice_parameters.at(partition);
        frame_body.write_bits(rice_parameter, 4);
        write_rice_signed_block(
            frame_body, subframe.residuals, residual_offset, residual_count,
            rice_parameter);
        residual_offset += residual_count;
    }
    if (residual_offset != subframe.residuals.size()) {
        throw std::runtime_error("internal FLAC LPC residual partition accounting error");
    }
    if (timings != nullptr) {
        add_elapsed_ns(timings->bitstream_ns, bitstream_started);
    }
    const auto output_started = Clock::now();
    write_frame_with_body(output, subframe.shifted_samples.size(), info, frame_body);
    if (timings != nullptr) {
        add_elapsed_ns(timings->frame_output_ns, output_started);
    }
    return lpc_rice_decision(subframe);
}

FlacSubframeDecision write_mono_verbatim_frame_impl(
    std::ostream& output,
    SampleSpan samples,
    const FlacFrameInfo& info)
{
    validate_frame_bits_per_sample(info.bits_per_sample);
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }

    const auto wasted_bits = common_wasted_bits(samples, info.bits_per_sample);
    const auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = info.bits_per_sample - wasted_bits;

    BitWriter frame_body;
    write_subframe_header(frame_body, 1, wasted_bits);
    for (const auto sample : shifted_samples) {
        validate_sample(sample, effective_bits_per_sample);
        frame_body.write_signed(sample, effective_bits_per_sample);
    }
    write_frame_with_body(output, samples.size(), info, frame_body);
    return FlacSubframeDecision {
        .kind = FlacSubframeKind::Verbatim,
        .fixed_order = 0,
        .rice_partition_order = 0,
        .wasted_bits = wasted_bits,
        .estimated_bits = verbatim_subframe_bits(
            samples.size(), info.bits_per_sample, wasted_bits),
    };
}

FlacSubframeDecision write_mono_constant_frame_impl(
    std::ostream& output,
    SampleSpan samples,
    const FlacFrameInfo& info)
{
    validate_frame_bits_per_sample(info.bits_per_sample);
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    if (!all_samples_equal(samples)) {
        throw std::runtime_error("constant FLAC subframe received varying samples");
    }
    validate_sample(samples.front(), info.bits_per_sample);

    const auto wasted_bits = common_wasted_bits(samples, info.bits_per_sample);
    const auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = info.bits_per_sample - wasted_bits;

    BitWriter frame_body;
    write_subframe_header(frame_body, 0, wasted_bits);
    frame_body.write_signed(shifted_samples.front(), effective_bits_per_sample);
    write_frame_with_body(output, samples.size(), info, frame_body);
    return FlacSubframeDecision {
        .kind = FlacSubframeKind::Constant,
        .fixed_order = 0,
        .rice_partition_order = 0,
        .wasted_bits = wasted_bits,
        .estimated_bits = constant_subframe_bits(info.bits_per_sample, wasted_bits),
    };
}

FlacSubframeDecision write_mono_selected_frame_impl(
    std::ostream& output,
    SampleSpan samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected,
    const FlacSubframeDecision* trusted_decision,
    FlacSelectedFrameWriteTimings* timings)
{
    validate_frame_bits_per_sample(info.bits_per_sample);
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty selected FLAC frame");
    }
    if (trusted_decision != nullptr) {
        check_selected_decision_matches(selected, *trusted_decision);
    }

    const auto validated_wasted_bits = [&]() {
        const auto validation_started = Clock::now();
        const auto wasted_bits = validated_common_wasted_bits(samples, info.bits_per_sample);
        (void)checked_selected_wasted_bits(wasted_bits, selected.wasted_bits);
        if (timings != nullptr) {
            add_elapsed_ns(timings->validation_wasted_ns, validation_started);
        }
        return wasted_bits;
    };
    const auto validated_shifted_samples = [&]() {
        const auto validation_started = Clock::now();
        auto shifted = validate_and_shift_selected_samples(
            samples, info.bits_per_sample, selected.wasted_bits);
        (void)checked_selected_wasted_bits(shifted.wasted_bits, selected.wasted_bits);
        if (timings != nullptr) {
            add_elapsed_ns(timings->validation_wasted_ns, validation_started);
        }
        return shifted;
    };

    switch (selected.kind) {
    case FlacSubframeKind::Constant: {
        (void)validated_wasted_bits();
        const auto decision = write_mono_constant_frame_impl(output, samples, info);
        return trusted_decision != nullptr ? *trusted_decision : decision;
    }
    case FlacSubframeKind::Verbatim: {
        (void)validated_wasted_bits();
        const auto decision = write_mono_verbatim_frame_impl(output, samples, info);
        return trusted_decision != nullptr ? *trusted_decision : decision;
    }
    case FlacSubframeKind::FixedRice: {
        if (trusted_decision != nullptr) {
            const auto validation_started = Clock::now();
            auto shifted = validate_shift_and_fixed_residuals(samples, info, selected);
            if (timings != nullptr) {
                add_elapsed_ns(timings->validation_wasted_ns, validation_started);
            }
            const auto subframe = make_selected_fixed_rice_subframe(
                std::move(shifted.shifted_samples),
                std::move(shifted.residuals),
                info,
                selected,
                shifted.wasted_bits,
                std::optional<std::uint64_t>(trusted_decision->estimated_bits),
                timings);
            return write_fixed_rice_frame(output, info, subframe, timings);
        }
        auto shifted = validated_shifted_samples();
        const auto subframe = make_selected_fixed_rice_subframe(
            std::move(shifted.shifted_samples), info, selected, shifted.wasted_bits,
            std::nullopt, timings);
        return write_fixed_rice_frame(output, info, subframe, timings);
    }
    case FlacSubframeKind::LpcRice: {
        if (trusted_decision != nullptr) {
            const auto validation_started = Clock::now();
            auto shifted = validate_shift_and_lpc_residuals(samples, info, selected);
            if (timings != nullptr) {
                add_elapsed_ns(timings->validation_wasted_ns, validation_started);
            }
            const auto subframe = make_selected_lpc_rice_subframe(
                std::move(shifted.shifted_samples),
                std::move(shifted.residuals),
                info,
                selected,
                shifted.wasted_bits,
                std::optional<std::uint64_t>(trusted_decision->estimated_bits),
                timings);
            return write_lpc_rice_frame(output, info, subframe, timings);
        }
        auto shifted = validated_shifted_samples();
        const auto subframe = make_selected_lpc_rice_subframe(
            std::move(shifted.shifted_samples), info, selected, shifted.wasted_bits,
            std::nullopt, timings);
        return write_lpc_rice_frame(output, info, subframe, timings);
    }
    }
    throw std::runtime_error("unknown selected FLAC subframe kind");
}

}  // namespace

std::uint64_t flac_streaminfo_total_samples_or_unknown(
    std::uint64_t actual_total_samples) noexcept
{
    return actual_total_samples <= kMaxStreamInfoTotalSamples
        ? actual_total_samples
        : 0;
}

void write_native_flac_streaminfo(std::ostream& output, const FlacStreamInfo& info)
{
    validate_streaminfo(info);

    output.write("fLaC", 4);
    if (!output) {
        throw std::runtime_error("failed to write native FLAC marker");
    }

    write_byte(output, 0x80U);
    write_u24be(output, 34);
    write_u16be(output, info.min_block_size);
    write_u16be(output, info.max_block_size);
    write_u24be(output, info.min_frame_size);
    write_u24be(output, info.max_frame_size);

    BitWriter streaminfo_bits;
    streaminfo_bits.write_bits(info.sample_rate, 20);
    streaminfo_bits.write_bits(info.channels - 1U, 3);
    streaminfo_bits.write_bits(info.bits_per_sample - 1U, 5);
    streaminfo_bits.write_bits(
        flac_streaminfo_total_samples_or_unknown(info.total_samples), 36);
    streaminfo_bits.align_zero();
    write_bytes(output, streaminfo_bits.bytes());

    output.write(reinterpret_cast<const char*>(info.md5.data()),
        static_cast<std::streamsize>(info.md5.size()));
    if (!output) {
        throw std::runtime_error("failed to write native FLAC STREAMINFO MD5");
    }
}

FlacSubframeDecision analyze_mono_best_frame(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    validate_frame_bits_per_sample(info.bits_per_sample);
    if (samples.empty()) {
        throw std::runtime_error("cannot analyze an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }

    if (all_samples_equal(samples)) {
        const auto wasted_bits = common_wasted_bits(samples, info.bits_per_sample);
        return constant_decision(info.bits_per_sample, wasted_bits);
    }

    const auto wasted_bits = common_wasted_bits(samples, info.bits_per_sample);
    const auto fixed = choose_fixed_rice_subframe(
        samples, info.bits_per_sample, info.max_rice_partition_order);
    const auto lpc = choose_lpc_rice_subframe(
        samples, info.bits_per_sample, info.max_lpc_order,
        info.lpc_coefficient_precision, info.max_rice_partition_order);
    const auto verbatim_bits = verbatim_subframe_bits(
        samples.size(), info.bits_per_sample, wasted_bits);
    if (lpc.bits < fixed.bits && lpc.bits < verbatim_bits) {
        return lpc_rice_decision(lpc);
    }
    if (fixed.bits < verbatim_bits) {
        return fixed_rice_decision(fixed);
    }
    return verbatim_decision(samples.size(), info.bits_per_sample, wasted_bits);
}

FlacLpcSubframeAnalysis lpc_analysis_from_subframe(const LpcRiceSubframe& lpc)
{
    return FlacLpcSubframeAnalysis {
        .order = lpc.order,
        .rice_partition_order = lpc.partition_order,
        .wasted_bits = lpc.wasted_bits,
        .coefficient_precision = lpc.coefficient_precision,
        .quantization_shift = lpc.quantization_shift,
        .window = lpc.window,
        .quantization = lpc.quantization,
        .coefficients = lpc.coefficients,
        .estimated_bits = lpc.bits,
    };
}

std::optional<FlacLpcSubframeAnalysis> analyze_mono_lpc_frame(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    validate_frame_bits_per_sample(info.bits_per_sample);
    if (samples.empty()) {
        throw std::runtime_error("cannot analyze an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }

    const auto lpc = choose_lpc_rice_subframe(
        samples, info.bits_per_sample, info.max_lpc_order,
        info.lpc_coefficient_precision, info.max_rice_partition_order);
    if (lpc.bits == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    return lpc_analysis_from_subframe(lpc);
}

std::optional<FlacLpcSubframeAnalysis> analyze_mono_lpc_order(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info,
    unsigned lpc_order)
{
    validate_frame_bits_per_sample(info.bits_per_sample);
    if (samples.empty()) {
        throw std::runtime_error("cannot analyze an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }
    if (lpc_order > info.max_lpc_order) {
        return std::nullopt;
    }

    const auto lpc = choose_lpc_rice_subframe_for_order(
        samples, info.bits_per_sample, lpc_order,
        info.max_lpc_order, info.lpc_coefficient_precision,
        info.max_rice_partition_order);
    if (lpc.bits == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }

    return lpc_analysis_from_subframe(lpc);
}

std::vector<FlacLpcSubframeAnalysis> analyze_mono_lpc_order_candidates(
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info,
    unsigned lpc_order)
{
    validate_frame_bits_per_sample(info.bits_per_sample);
    if (samples.empty()) {
        throw std::runtime_error("cannot analyze an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }
    checked_max_rice_partition_order(info.max_rice_partition_order);
    if (info.lpc_coefficient_precision == 0 || info.lpc_coefficient_precision > 15) {
        throw std::runtime_error("native FLAC LPC coefficient precision must be 1..15");
    }
    if (samples.size() < kMinLpcBlockSize || lpc_order == 0 ||
        info.max_lpc_order == 0 || lpc_order > info.max_lpc_order) {
        return {};
    }

    const auto max_order = std::min<std::size_t>(
        std::min(info.max_lpc_order, kMaxLpcOrder), samples.size() - 1U);
    if (lpc_order > max_order) {
        return {};
    }

    const auto wasted_bits = common_wasted_bits(samples, info.bits_per_sample);
    auto shifted_samples = shift_samples(samples, wasted_bits);
    const auto effective_bits_per_sample = info.bits_per_sample - wasted_bits;

    std::vector<LpcRiceSubframe> candidates;
    const auto rectangular_autocorrelation = autocorrelation(shifted_samples, lpc_order);
    append_lpc_rice_order_candidates(
        shifted_samples,
        rectangular_autocorrelation,
        FlacLpcWindowKind::Rectangular,
        lpc_order,
        wasted_bits,
        effective_bits_per_sample,
        info.lpc_coefficient_precision,
        info.max_rice_partition_order,
        RiceSelectionMode::Exact,
        candidates);
    const auto tukey_autocorrelation =
        autocorrelation(tukey_windowed_samples(shifted_samples, 0.5), lpc_order);
    append_lpc_rice_order_candidates(
        shifted_samples,
        tukey_autocorrelation,
        FlacLpcWindowKind::Tukey,
        lpc_order,
        wasted_bits,
        effective_bits_per_sample,
        info.lpc_coefficient_precision,
        info.max_rice_partition_order,
        RiceSelectionMode::Exact,
        candidates);
    if (should_consider_welch_lpc_order(lpc_order, static_cast<unsigned>(max_order))) {
        const auto welch_autocorrelation =
            autocorrelation(welch_windowed_samples(shifted_samples), lpc_order);
        append_lpc_rice_order_candidates(
            shifted_samples,
            welch_autocorrelation,
            FlacLpcWindowKind::Welch,
            lpc_order,
            wasted_bits,
            effective_bits_per_sample,
            info.lpc_coefficient_precision,
            info.max_rice_partition_order,
            RiceSelectionMode::Exact,
            candidates);
    }

    std::vector<FlacLpcSubframeAnalysis> analyses;
    analyses.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        analyses.push_back(lpc_analysis_from_subframe(candidate));
    }
    return analyses;
}

FlacSubframeDecision write_mono_verbatim_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    return write_mono_verbatim_frame_impl(output, samples, info);
}

FlacSubframeDecision write_mono_constant_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    return write_mono_constant_frame_impl(output, samples, info);
}

FlacSubframeDecision write_mono_fixed_rice_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    validate_frame_bits_per_sample(info.bits_per_sample);
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }

    const auto subframe = choose_fixed_rice_subframe(
        samples, info.bits_per_sample, info.max_rice_partition_order);
    return write_fixed_rice_frame(output, info, subframe);
}

FlacSubframeDecision write_mono_selected_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected)
{
    return write_mono_selected_frame(
        output, std::span<const std::int32_t>(samples), info, selected);
}

FlacSubframeDecision write_mono_selected_frame(
    std::ostream& output,
    std::span<const std::int32_t> samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected)
{
    return write_mono_selected_frame_impl(output, samples, info, selected, nullptr, nullptr);
}

FlacSubframeDecision write_mono_selected_frame_with_decision(
    std::ostream& output,
    std::span<const std::int32_t> samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected,
    const FlacSubframeDecision& decision)
{
    return write_mono_selected_frame_impl(output, samples, info, selected, &decision, nullptr);
}

FlacSubframeDecision write_mono_selected_frame_with_decision(
    std::ostream& output,
    std::span<const std::int32_t> samples,
    const FlacFrameInfo& info,
    const FlacSelectedSubframe& selected,
    const FlacSubframeDecision& decision,
    FlacSelectedFrameWriteTimings* timings)
{
    return write_mono_selected_frame_impl(output, samples, info, selected, &decision, timings);
}

FlacSubframeDecision write_mono_best_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info)
{
    return write_mono_best_frame(output, samples, info, NativeAnalysisProfile::Exact);
}

FlacSubframeDecision write_mono_best_frame(
    std::ostream& output,
    const std::vector<std::int32_t>& samples,
    const FlacFrameInfo& info,
    NativeAnalysisProfile profile)
{
    validate_frame_bits_per_sample(info.bits_per_sample);
    if (samples.empty()) {
        throw std::runtime_error("cannot write an empty FLAC frame");
    }
    for (const auto sample : samples) {
        validate_sample(sample, info.bits_per_sample);
    }

    if (all_samples_equal(samples)) {
        return write_mono_constant_frame(output, samples, info);
    }

    const auto wasted_bits = common_wasted_bits(samples, info.bits_per_sample);
    FixedRiceSubframe fixed;
    LpcRiceSubframe lpc;
    switch (profile) {
    case NativeAnalysisProfile::Exact:
        fixed = choose_fixed_rice_subframe(
            samples, info.bits_per_sample, info.max_rice_partition_order);
        lpc = choose_lpc_rice_subframe(
            samples, info.bits_per_sample, info.max_lpc_order,
            info.lpc_coefficient_precision, info.max_rice_partition_order);
        break;
    case NativeAnalysisProfile::OrderGuessExactRice:
        fixed = choose_fixed_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_rice_partition_order,
            RiceSelectionMode::Exact);
        lpc = choose_lpc_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_lpc_order,
            info.lpc_coefficient_precision, info.max_rice_partition_order,
            RiceSelectionMode::Exact, false);
        break;
    case NativeAnalysisProfile::OrderGuessMeanRice:
        fixed = choose_fixed_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_rice_partition_order,
            RiceSelectionMode::MeanEstimate);
        lpc = choose_lpc_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_lpc_order,
            info.lpc_coefficient_precision, info.max_rice_partition_order,
            RiceSelectionMode::MeanEstimate, false);
        break;
    case NativeAnalysisProfile::OrderGuessMeanEstimateRice:
        fixed = choose_fixed_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_rice_partition_order,
            RiceSelectionMode::MeanEstimateOnly);
        lpc = choose_lpc_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_lpc_order,
            info.lpc_coefficient_precision, info.max_rice_partition_order,
            RiceSelectionMode::MeanEstimateOnly, false);
        break;
    case NativeAnalysisProfile::SubdivideTukey3MeanRice:
        fixed = choose_fixed_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_rice_partition_order,
            RiceSelectionMode::MeanEstimate);
        lpc = choose_lpc_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_lpc_order,
            info.lpc_coefficient_precision, info.max_rice_partition_order,
            RiceSelectionMode::MeanEstimate, true);
        break;
    case NativeAnalysisProfile::SubdivideTukey3MeanEstimateRice:
        fixed = choose_fixed_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_rice_partition_order,
            RiceSelectionMode::MeanEstimateOnly);
        lpc = choose_lpc_rice_subframe_order_guess(
            samples, info.bits_per_sample, info.max_lpc_order,
            info.lpc_coefficient_precision, info.max_rice_partition_order,
            RiceSelectionMode::MeanEstimateOnly, true);
        break;
    }
    const auto verbatim_bits = verbatim_subframe_bits(
        samples.size(), info.bits_per_sample, wasted_bits);
    if (lpc.bits < fixed.bits && lpc.bits < verbatim_bits) {
        return write_lpc_rice_frame(output, info, lpc);
    }
    if (fixed.bits < verbatim_bits) {
        return write_fixed_rice_frame(output, info, fixed);
    }
    return write_mono_verbatim_frame(output, samples, info);
}

}  // namespace ldcompress
